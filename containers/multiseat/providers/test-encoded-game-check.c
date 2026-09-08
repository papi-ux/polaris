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
static unsigned char serialized_layout[1024];
static size_t serialized_layout_size;

GstElement *__real_gst_parse_launch(const gchar *, GError **);
GstElement *__wrap_gst_parse_launch(const gchar *description, GError **error) {
  if (serialized_layout_size) {
    /* Run at the actual receiver's startup boundary, before any pipeline
     * element can accidentally register the missing implementation. */
    GstBuffer *buffer = gst_buffer_new();
    guint32 consumed = 0;
    GstMeta *restored = gst_meta_deserialize(buffer, serialized_layout, serialized_layout_size, &consumed);
    assert(restored && consumed == serialized_layout_size);
    GstVideoMeta *layout = gst_buffer_get_video_meta(buffer);
    assert(layout && layout->format == GST_VIDEO_FORMAT_NV12 && layout->width == 640 && layout->height == 480);
    assert(layout->n_planes == 2 && layout->offset[0] == 64 && layout->offset[1] == 64 + 768 * 480);
    assert(layout->stride[0] == 768 && layout->stride[1] == 768);
    gst_buffer_unref(buffer);
    serialized_layout_size = 0;
    assert(write(witness, "M", 1) == 1);
  }
  return __real_gst_parse_launch(description, error);
}

/* Fork both peers before the parent initializes GStreamer. A sender in the
 * receiver process would register GstVideoMeta and hide this regression. */
static void serialized_video_layout(void) {
  int wire[2]; assert(pipe2(wire, O_CLOEXEC) == 0);
  pid_t sender = fork(); assert(sender >= 0);
  if (sender == 0) {
    close(wire[0]);
    gst_init(NULL, NULL);
    GstBuffer *buffer = gst_buffer_new();
    gsize offsets[GST_VIDEO_MAX_PLANES] = {64, 64 + 768 * 480};
    gint strides[GST_VIDEO_MAX_PLANES] = {768, 768};
    GstVideoMeta *meta = gst_buffer_add_video_meta_full(buffer, GST_VIDEO_FRAME_FLAG_NONE,
      GST_VIDEO_FORMAT_NV12, 640, 480, 2, offsets, strides);
    assert(meta);
    GByteArray *bytes = g_byte_array_new();
    assert(gst_meta_serialize_simple(&meta->meta, bytes));
    assert(bytes->len > 0 && bytes->len <= sizeof(serialized_layout));
    assert(write(wire[1], bytes->data, bytes->len) == (ssize_t)bytes->len);
    g_byte_array_unref(bytes); gst_buffer_unref(buffer); gst_deinit();
    close(wire[1]); _exit(0);
  }
  close(wire[1]);
  for (;;) {
    unsigned char chunk[1024];
    ssize_t count = read(wire[0], chunk, sizeof(chunk));
    assert(count >= 0);
    if (!count) break;
    assert((size_t)count <= sizeof(serialized_layout) - serialized_layout_size);
    memcpy(serialized_layout + serialized_layout_size, chunk, (size_t)count);
    serialized_layout_size += (size_t)count;
  }
  close(wire[0]); int status = 0;
  assert(waitpid(sender, &status, 0) == sender && WIFEXITED(status) && WEXITSTATUS(status) == 0);
  assert(serialized_layout_size);
  int observed[2]; assert(pipe2(observed, O_CLOEXEC) == 0);
  pid_t receiver = fork(); assert(receiver >= 0);
  if (receiver == 0) {
    close(observed[0]); witness = observed[1];
    char *arguments[] = {"test-encoded-game-check", "--self-test", NULL};
    _exit(encoded_game_main(2, arguments));
  }
  close(observed[1]);
  assert(waitpid(receiver, &status, 0) == receiver);
  char marker = 0;
  assert(read(observed[0], &marker, 1) == 1 && marker == 'M');
  assert(read(observed[0], &marker, 1) == 0); close(observed[0]);
  assert(WIFEXITED(status) && WEXITSTATUS(status) == 0);
  serialized_layout_size = 0;
}

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
  /* Exercise actual AF_UNIX modes produced under the provider's umask,
   * retaining the same descriptor while rejecting access by another user. */
  struct stat identity;
  for (unsigned mode = 0600; mode <= 0700; mode += 0100) {
    assert(chmod(original, mode) == 0 && fstat(pinned, &identity) == 0);
    assert(valid_capture_socket(&identity));
  }
  assert(chmod(original, 0750) == 0 && fstat(pinned, &identity) == 0);
  assert(!valid_capture_socket(&identity));
  assert(chmod(original, 0700) == 0);
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
  serialized_video_layout();
  pinned_socket_replacement();
  codec_failure(GST_STATE_CHANGE_FAILURE, FALSE);
  codec_failure(GST_STATE_CHANGE_ASYNC, FALSE);
  codec_failure(GST_STATE_CHANGE_SUCCESS, TRUE);
  return 0;
}
