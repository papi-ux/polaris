/* Image-owned producer. Only inherited, verified read-only evdev descriptors
 * enter; no path discovery, udev metadata, device writes or input ioctls. */
#define _GNU_SOURCE
#include "seat-input.h"
#include <gst/gst.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

static volatile sig_atomic_t stopping;
static _Atomic gint64 last_frame;
static void stop(int number) { (void)number; stopping = 1; }
static GstPadProbeReturn frame_ready(GstPad *pad, GstPadProbeInfo *info, gpointer unused) {
  (void)pad; (void)info; (void)unused;
  atomic_store(&last_frame, g_get_monotonic_time());
  return GST_PAD_PROBE_OK;
}
static bool send_input(void *opaque, const struct seat_command *command) {
  GstStructure *structure = NULL;
  switch (command->kind) {
    case SEAT_KEY:
      structure = gst_structure_new("KeyboardKey", "key", G_TYPE_UINT, command->code,
                                    "pressed", G_TYPE_BOOLEAN, command->pressed, NULL); break;
    case SEAT_BUTTON:
      structure = gst_structure_new("MouseButton", "button", G_TYPE_UINT, command->code,
                                    "pressed", G_TYPE_BOOLEAN, command->pressed, NULL); break;
    case SEAT_MOVE_RELATIVE: case SEAT_MOVE_ABSOLUTE:
      structure = gst_structure_new(command->kind == SEAT_MOVE_RELATIVE ? "MouseMoveRelative" : "MouseMoveAbsolute",
                                    "pointer_x", G_TYPE_DOUBLE, command->x, "pointer_y", G_TYPE_DOUBLE, command->y, NULL); break;
    case SEAT_AXIS:
      structure = gst_structure_new("MouseAxis", "x", G_TYPE_DOUBLE, command->x, "y", G_TYPE_DOUBLE, command->y, NULL); break;
  }
  return structure && gst_element_send_event(GST_ELEMENT(opaque), gst_event_new_custom(GST_EVENT_CUSTOM_UPSTREAM, structure));
}
static unsigned dimension(const char *text) {
  char *end; errno = 0; unsigned long value = strtoul(text, &end, 10);
  if (errno || !*text || *end || text[0] == '0' || value > 16384) return 0;
  for (const char *p=text; *p; ++p) if (*p < '0' || *p > '9') return 0;
  return (unsigned)value;
}
int main(int argc, char **argv) {
  if (argc < 8 || strcmp(argv[3], "--")) return 1;
  unsigned width = dimension(argv[1]), height = dimension(argv[2]);
  if (!width || !height) return 1;
  struct pollfd polls[4] = {{4,POLLIN,0},{5,POLLIN,0},{6,POLLIN,0},{-1,POLLIN,0}};
  dev_t devices[3];
  for (int i=0;i<3;++i) {
    struct stat status; int flags = fcntl(polls[i].fd,F_GETFL);
    if (flags < 0 || (flags & O_ACCMODE) != O_RDONLY || (flags & O_PATH) || !(flags & O_NONBLOCK) ||
        fstat(polls[i].fd,&status) || !S_ISCHR(status.st_mode) || major(status.st_rdev) != 13) return 1;
    devices[i] = status.st_rdev;
    for (int j=0;j<i;++j) if (devices[j] == devices[i]) return 1;
  }
  struct sigaction action = {0}; action.sa_handler = stop;
  sigaction(SIGTERM,&action,NULL); sigaction(SIGINT,&action,NULL);
  GError *error = NULL;
  if (!gst_init_check(NULL,NULL,&error)) { if (error) g_error_free(error); return 1; }
  GstElement *pipeline = gst_parse_launchv((const gchar **)&argv[4], &error);
  if (!pipeline || error) { if (pipeline) gst_object_unref(pipeline); if (error) g_error_free(error); return 1; }
  GstElement *display = gst_bin_get_by_name(GST_BIN(pipeline), "display");
  if (!display) { gst_object_unref(pipeline); return 1; }
  GstPad *pad = gst_element_get_static_pad(display,"src");
  if (!pad) { gst_object_unref(display); gst_object_unref(pipeline); return 1; }
  gulong probe = gst_pad_add_probe(pad,GST_PAD_PROBE_TYPE_BUFFER,frame_ready,NULL,NULL);
  GstBus *bus = gst_element_get_bus(pipeline);
  GPollFD bus_fd; gst_bus_get_pollfd(bus,&bus_fd); polls[3].fd = bus_fd.fd;
  struct seat_input state[3] = {{.role=SEAT_KEYBOARD,.width=width,.height=height},
                               {.role=SEAT_RELATIVE,.width=width,.height=height},
                               {.role=SEAT_ABSOLUTE,.width=width,.height=height}};
  atomic_store(&last_frame,0);
  const gint64 started = g_get_monotonic_time();
  int failed = gst_element_set_state(pipeline,GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE;
  gint64 window = g_get_monotonic_time(); unsigned events = 0;
  while (!stopping && !failed) {
    int result = poll(polls,4,100);
    if (result < 0) { if (errno == EINTR) continue; failed=1; break; }
    const gint64 captured = atomic_load(&last_frame), checked = g_get_monotonic_time();
    if ((captured && checked-captured > G_USEC_PER_SEC) ||
        (!captured && checked-started > 15*G_USEC_PER_SEC)) { failed=1; break; }
    if (polls[3].revents) {
      GstMessage *message;
      while ((message=gst_bus_pop(bus))) {
        if (GST_MESSAGE_TYPE(message) == GST_MESSAGE_ERROR || GST_MESSAGE_TYPE(message) == GST_MESSAGE_EOS) failed=1;
        gst_message_unref(message);
      }
    }
    for (int i=0;i<3 && !failed;++i) {
      if (polls[i].revents & (POLLERR|POLLHUP|POLLNVAL)) { failed=1; break; }
      if (!(polls[i].revents & POLLIN)) continue;
      struct input_event batch[64]; ssize_t count = read(polls[i].fd,batch,sizeof(batch));
      if (count < 0 && (errno == EINTR || errno == EAGAIN)) continue;
      if (count <= 0 || count % sizeof(batch[0])) { failed=1; break; }
      gint64 now=g_get_monotonic_time();
      if (now-window >= G_USEC_PER_SEC) { window=now; events=0; }
      events += count/sizeof(batch[0]);
      /* Bound work and the plugin command backlog if compositor delivery stops. */
      if (events > 16384 || !captured || now-captured > G_USEC_PER_SEC) { failed=1; break; }
      for (size_t j=0;j<(size_t)count/sizeof(batch[0]);++j) {
        if (!seat_input_event(&state[i],&batch[j],send_input,display)) { failed=1; break; }
      }
    }
  }
  for (int i=0;i<3;++i) { seat_input_release(&state[i],send_input,display); close(polls[i].fd); }
  gst_element_set_state(pipeline,GST_STATE_NULL);
  gst_pad_remove_probe(pad,probe); gst_object_unref(pad); gst_object_unref(bus);
  gst_object_unref(display); gst_object_unref(pipeline);
  return failed ? 1 : 0;
}
