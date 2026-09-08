/* Real codec regressions for observation and teardown failure paths. */
#define main encoded_game_main
#include "encoded-game-check.c"
#undef main
#include <assert.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>

static int witness = -1;
static GstStateChangeReturn forced_stop = GST_STATE_CHANGE_SUCCESS;
static gboolean forbidden_cleanup, wrong_geometry;
static unsigned pulled;

GstStateChangeReturn __real_gst_element_set_state(GstElement *, GstState);
GstStateChangeReturn __wrap_gst_element_set_state(GstElement *element, GstState state) {
  if (state == GST_STATE_NULL && forced_stop != GST_STATE_CHANGE_SUCCESS) {
    assert(write(witness, "S", 1) == 1);
    forbidden_cleanup = TRUE;
    return forced_stop;
  }
  return __real_gst_element_set_state(element, state);
}
void __real_gst_object_unref(gpointer);
void __wrap_gst_object_unref(gpointer object) {
  if (forbidden_cleanup) _exit(90);
  __real_gst_object_unref(object);
}
void __real_gst_pad_remove_probe(GstPad *, gulong);
void __wrap_gst_pad_remove_probe(GstPad *pad, gulong id) {
  if (forbidden_cleanup) _exit(91);
  __real_gst_pad_remove_probe(pad, id);
}
void __real_g_mutex_clear(GMutex *);
void __wrap_g_mutex_clear(GMutex *mutex) {
  if (forbidden_cleanup) _exit(92);
  __real_g_mutex_clear(mutex);
}
EGLBoolean __real_eglTerminate(EGLDisplay);
EGLBoolean __wrap_eglTerminate(EGLDisplay display) {
  if (forbidden_cleanup) _exit(93);
  return __real_eglTerminate(display);
}
void __real_gbm_device_destroy(struct gbm_device *);
void __wrap_gbm_device_destroy(struct gbm_device *device) {
  if (forbidden_cleanup) _exit(94);
  __real_gbm_device_destroy(device);
}
GstSample *__real_gst_app_sink_try_pull_sample(GstAppSink *, GstClockTime);
GstSample *__wrap_gst_app_sink_try_pull_sample(GstAppSink *sink, GstClockTime timeout) {
  GstSample *sample = __real_gst_app_sink_try_pull_sample(sink, timeout);
  if (sample && wrong_geometry && ++pulled > 30) {
    if (pulled == 31) assert(write(witness, "G", 1) == 1);
    GstCaps *caps = gst_caps_copy(gst_sample_get_caps(sample));
    gst_caps_set_simple(caps, "width", G_TYPE_INT, 642, NULL);
    GstSample *replacement = gst_sample_new(gst_sample_get_buffer(sample), caps, gst_sample_get_segment(sample), NULL);
    gst_caps_unref(caps); gst_sample_unref(sample); sample = replacement;
  }
  return sample;
}

static void codec_failure(GstStateChangeReturn stop_result, gboolean geometry) {
  int observed[2]; assert(pipe2(observed, O_CLOEXEC) == 0);
  pid_t child = fork(); assert(child >= 0);
  if (child == 0) {
    close(observed[0]); witness = observed[1]; forced_stop = stop_result; wrong_geometry = geometry;
    char *arguments[] = {"test-encoded-game-check", "--self-test", NULL};
    _exit(encoded_game_main(2, arguments));
  }
  close(observed[1]); int status = 0;
  assert(waitpid(child, &status, 0) == child);
  char marker = 0;
  assert(read(observed[0], &marker, 1) == 1 && marker == (geometry ? 'G' : 'S'));
  assert(read(observed[0], &marker, 1) == 0); close(observed[0]);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 1);
}

static int listener_at(const char *path) {
  int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0); assert(listener >= 0);
  struct sockaddr_un address = {.sun_family = AF_UNIX};
  assert(strlen(path) < sizeof(address.sun_path)); strcpy(address.sun_path, path);
  assert(bind(listener, (struct sockaddr *)&address, sizeof(address)) == 0);
  assert(listen(listener, 1) == 0); return listener;
}

/* Linux pathname AF_UNIX connect through O_PATH must resolve the retained
 * socket dentry even after a replacement takes over the original pathname. */
static void pinned_socket_replacement(void) {
  char directory[] = "/tmp/polaris-encoded-test-XXXXXX"; assert(mkdtemp(directory));
  char original[108], retired[108], pinned_path[64];
  snprintf(original, sizeof(original), "%s/source", directory);
  snprintf(retired, sizeof(retired), "%s/retired", directory);
  int first = listener_at(original);
  int pinned = open(original, O_PATH | O_NOFOLLOW | O_CLOEXEC); assert(pinned >= 0);
  assert(rename(original, retired) == 0);
  int replacement = listener_at(original);
  snprintf(pinned_path, sizeof(pinned_path), "/proc/self/fd/%d", pinned);
  int client = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0); assert(client >= 0);
  struct sockaddr_un address = {.sun_family = AF_UNIX}; strcpy(address.sun_path, pinned_path);
  assert(connect(client, (struct sockaddr *)&address, sizeof(address)) == 0);
  int accepted = accept4(first, NULL, NULL, SOCK_CLOEXEC); assert(accepted >= 0);
  assert(accept4(replacement, NULL, NULL, SOCK_CLOEXEC) == -1 && errno == EAGAIN);
  close(accepted); close(client); close(replacement); close(pinned); close(first);
  assert(unlink(original) == 0 && unlink(retired) == 0 && rmdir(directory) == 0);
}

int main(void) {
  pinned_socket_replacement();
  codec_failure(GST_STATE_CHANGE_FAILURE, FALSE);
  codec_failure(GST_STATE_CHANGE_ASYNC, FALSE);
  codec_failure(GST_STATE_CHANGE_SUCCESS, TRUE);
  return 0;
}
