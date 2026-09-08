/* Bounded physical acceptance probe. The software encoder is selected explicitly;
 * no production encoder selection or client media routing is enabled here. */
#define _GNU_SOURCE
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <gst/gl/gl.h>
#include <gst/gl/egl/gstgldisplay_egl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <gbm.h>
#include <sys/sysmacros.h>
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define FRAME_COUNT 60
#define MAX_FRAME_BYTES (16u * 1024u * 1024u)
static volatile sig_atomic_t stopping;
static void stop(int number) { (void)number; stopping = 1; }
struct encoded_stats {
  GMutex lock;
  unsigned frames, keyframes;
  uint64_t bytes;
  size_t maximum;
  gboolean failed;
};
struct synthetic_source { unsigned width, height, frame; gboolean empty, frozen; };

static GstPadProbeReturn encoded_buffer(GstPad *pad, GstPadProbeInfo *info, gpointer opaque) {
  (void)pad;
  struct encoded_stats *stats = opaque;
  GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
  const size_t size = buffer ? gst_buffer_get_size(buffer) : 0;
  g_mutex_lock(&stats->lock);
  if (!size || size > MAX_FRAME_BYTES || stats->frames >= FRAME_COUNT) stats->failed = TRUE;
  else {
    ++stats->frames;
    if (!GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT)) ++stats->keyframes;
    stats->bytes += size;
    if (size > stats->maximum) stats->maximum = size;
  }
  const gboolean failed = stats->failed;
  g_mutex_unlock(&stats->lock);
  return failed ? GST_PAD_PROBE_DROP : GST_PAD_PROBE_OK;
}

/* Build-time codec validation uses generated pixels and is labeled synthetic.
 * Its shape and colours match the fixed game; an empty scene must fail. */
static void synthetic_frame(GstAppSrc *source, guint requested, gpointer opaque) {
  (void)requested;
  struct synthetic_source *state = opaque;
  if (state->frame == FRAME_COUNT) { gst_app_src_end_of_stream(source); return; }
  GstBuffer *buffer = gst_buffer_new_allocate(NULL, (size_t)state->width * state->height * 3, NULL);
  GstMapInfo mapped;
  if (!buffer || !gst_buffer_map(buffer, &mapped, GST_MAP_WRITE)) {
    if (buffer) gst_buffer_unref(buffer);
    gst_app_src_end_of_stream(source); return;
  }
  for (unsigned y = 0; y < state->height; ++y) for (unsigned x = 0; x < state->width; ++x) {
    unsigned colour = 0x101827;
    if (!state->empty) {
      if (x >= state->width * .035 && x < state->width * .047 && y >= state->height * .4 && y < state->height * .6) colour = 0x36dbc3;
      if (x >= state->width * .95 && x < state->width * .962 && y >= state->height * .4 && y < state->height * .6) colour = 0xef8c6b;
      unsigned ball = state->width / 3 + (state->frozen ? 0 : state->frame * 2);
      if (x >= ball && x < ball + 12 && y >= state->height / 2 && y < state->height / 2 + 12) colour = 0xf1f4f8;
    }
    size_t offset = ((size_t)y * state->width + x) * 3;
    mapped.data[offset] = colour >> 16; mapped.data[offset + 1] = colour >> 8; mapped.data[offset + 2] = colour;
  }
  gst_buffer_unmap(buffer, &mapped);
  GST_BUFFER_PTS(buffer) = gst_util_uint64_scale(state->frame, GST_SECOND, 60);
  GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale(1, GST_SECOND, 60);
  ++state->frame;
  (void)gst_app_src_push_buffer(source, buffer);
}

struct scene_observation { gboolean found; double ball_x, ball_y; };
/* FALSE means malformed decoded video and is fatal, even if earlier frames
 * matched. A valid frame without the expected scene is a separate result. */
static gboolean inspect_scene(GstSample *sample, unsigned width, unsigned height, struct scene_observation *scene) {
  GstVideoInfo info;
  GstVideoFrame frame;
  *scene = (struct scene_observation){0};
  if (!gst_video_info_from_caps(&info, gst_sample_get_caps(sample)) ||
      GST_VIDEO_INFO_FORMAT(&info) != GST_VIDEO_FORMAT_RGB ||
      GST_VIDEO_INFO_WIDTH(&info) != (int)width || GST_VIDEO_INFO_HEIGHT(&info) != (int)height ||
      !gst_video_frame_map(&frame, &info, gst_sample_get_buffer(sample), GST_MAP_READ)) return FALSE;
  const int stride = GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0);
  if (stride < (int)width * 3) { gst_video_frame_unmap(&frame); return FALSE; }
  const unsigned char *pixels = GST_VIDEO_FRAME_PLANE_DATA(&frame, 0);
  unsigned teal = 0, orange = 0, dark = 0, total = 0, white = 0;
  unsigned min_x = width, min_y = height, max_x = 0, max_y = 0;
  uint64_t sum_x = 0, sum_y = 0;
  for (unsigned y = 0; y < height; y += 2) for (unsigned x = 0; x < width; x += 2) {
    const unsigned char *p = pixels + (size_t)y * stride + (size_t)x * 3;
    ++total;
    if (p[0] < 40 && p[1] < 50 && p[2] < 65) ++dark;
    if (x > width * .02 && x < width * .08 && y > height * .05 &&
        p[0] > 20 && p[0] < 95 && p[1] > 175 && p[1] < 250 && p[2] > 150 && p[2] < 235) ++teal;
    if (x > width * .92 && x < width * .99 && y > height * .05 &&
        p[0] > 195 && p[1] > 100 && p[1] < 185 && p[2] > 55 && p[2] < 155) ++orange;
    /* Exclude the HUD and recognize the compact white ball. Lossy codec
     * variation in a stationary scene must not count as game motion. */
    if (x > width * .02 && x < width * .98 && y > height * .08 && y < height * .99 &&
        p[0] > 195 && p[1] > 195 && p[2] > 195) {
      ++white; sum_x += x; sum_y += y;
      if (x < min_x) min_x = x;
      if (x > max_x) max_x = x;
      if (y < min_y) min_y = y;
      if (y > max_y) max_y = y;
    }
  }
  gst_video_frame_unmap(&frame);
  scene->found = teal >= total / 1000 && orange >= total / 1000 && dark >= total / 2 &&
    white >= 6 && white <= 100 && max_x - min_x <= 20 && max_y - min_y <= 20;
  if (scene->found) { scene->ball_x = (double)sum_x / white; scene->ball_y = (double)sum_y / white; }
  return TRUE;
}

static unsigned dimension(const char *text) {
  char *end = NULL; errno = 0;
  unsigned long value = strtoul(text, &end, 10);
  return errno || !*text || *end || value < 320 || value > 3840 || value % 2 ? 0 : (unsigned)value;
}

static int pin_capture_socket(const char *path, struct stat *identity) {
  const char prefix[] = "/run/polaris/polaris-frames-";
  if (strlen(path) != sizeof(prefix) - 1 + 64 || strncmp(path, prefix, sizeof(prefix) - 1)) return -1;
  for (const char *p = path + sizeof(prefix) - 1; *p; ++p) if (!(*p >= '0' && *p <= '9') && !(*p >= 'a' && *p <= 'f')) return -1;
  struct stat directory;
  if (lstat("/run/polaris", &directory) || !S_ISDIR(directory.st_mode) ||
      (directory.st_mode & 07777) != 0700 || directory.st_uid != geteuid()) return -1;
  int fd = open(path, O_PATH | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return -1;
  if (fstat(fd, identity) || !S_ISSOCK(identity->st_mode) ||
      (identity->st_mode & 07777) != 0600 || identity->st_uid != geteuid()) { close(fd); return -1; }
  return fd;
}

struct capture_gpu { int descriptor; struct gbm_device *gbm; EGLDisplay egl; GstGLDisplayEGL *display; };
static void release_gpu(struct capture_gpu *gpu) {
  // All samples, pipelines and GL contexts must retire before GBM and its FD.
  if (gpu->display) gst_object_unref(gpu->display);
  if (gpu->egl != EGL_NO_DISPLAY) eglTerminate(gpu->egl);
  if (gpu->gbm) gbm_device_destroy(gpu->gbm);
  if (gpu->descriptor >= 0) close(gpu->descriptor);
}
static gboolean open_gpu(const char *path, struct capture_gpu *gpu) {
  const char prefix[] = "/dev/dri/renderD";
  if (strncmp(path, prefix, sizeof(prefix)-1) || strlen(path) > sizeof(prefix)+6 || !path[sizeof(prefix)-1]) return FALSE;
  for (const char *p = path + sizeof(prefix)-1; *p; ++p) if (*p < '0' || *p > '9') return FALSE;
  gpu->descriptor = open(path, O_RDWR | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
  struct stat node;
  if (gpu->descriptor < 0 || fstat(gpu->descriptor, &node) || !S_ISCHR(node.st_mode) ||
      major(node.st_rdev) != 226 || minor(node.st_rdev) < 128) return FALSE;
  gpu->gbm = gbm_create_device(gpu->descriptor);
  if (!gpu->gbm) return FALSE;
  gpu->egl = eglGetPlatformDisplay(EGL_PLATFORM_GBM_KHR, gpu->gbm, NULL);
  if (gpu->egl == EGL_NO_DISPLAY || !eglInitialize(gpu->egl, NULL, NULL)) return FALSE;
  gpu->display = gst_gl_display_egl_new_with_egl_display(gpu->egl);
  if (!gpu->display) return FALSE;
  gst_gl_display_egl_set_foreign(gpu->display, TRUE);
  gst_gl_display_filter_gl_api(GST_GL_DISPLAY(gpu->display), GST_GL_API_GLES2);
  return TRUE;
}

int main(int argc, char **argv) {
  const gboolean synthetic = argc == 2 && (!strcmp(argv[1], "--self-test") || !strcmp(argv[1], "--self-test-empty") || !strcmp(argv[1], "--self-test-frozen"));
  unsigned width = synthetic ? 640 : argc == 5 ? dimension(argv[2]) : 0;
  unsigned height = synthetic ? 480 : argc == 5 ? dimension(argv[3]) : 0;
  if (!width || !height) return 1;
  struct stat identity;
  int pinned = synthetic ? -1 : pin_capture_socket(argv[1], &identity);
  if (!synthetic && pinned < 0) return 1;
  struct sigaction action = {0}; action.sa_handler = stop;
  sigaction(SIGTERM, &action, NULL); sigaction(SIGINT, &action, NULL);
  /* The executable itself never forks a registry-scanner child. */
  g_setenv("GST_REGISTRY_FORK", "no", TRUE);
  g_setenv("GST_GL_PLATFORM", "egl", TRUE);
  g_setenv("GST_GL_API", "gles2", TRUE);
  GError *error = NULL;
  gst_init(NULL, NULL);
  const char *head = synthetic ? "appsrc name=source format=time ! videoconvert ! " :
    "unixfdsrc name=source num-buffers=60 ! glupload ! glcolorconvert ! video/x-raw(memory:GLMemory),format=RGBA ! gldownload ! videoconvert ! ";
  char *description = g_strconcat(head,
    "video/x-raw,format=I420 ! openh264enc name=encoder bitrate=8000000 gop-size=30 ! "
    "h264parse ! video/x-h264,stream-format=byte-stream,alignment=au ! identity name=encoded ! "
    "openh264dec ! videoconvert ! video/x-raw,format=RGB ! appsink name=decoded max-buffers=2 drop=false sync=false", NULL);
  GstElement *pipeline = gst_parse_launch(description, &error); g_free(description);
  int result = 1;
  if (!pipeline || error) {
    if (error) g_error_free(error);
    if (pipeline) gst_object_unref(pipeline);
    if (pinned >= 0) close(pinned);
    fprintf(stderr, "encoded game pipeline unavailable\n"); return 1;
  }
  struct capture_gpu gpu = {.descriptor = -1, .egl = EGL_NO_DISPLAY};
  if (!synthetic) {
    if (!open_gpu(argv[4], &gpu)) {
      gst_object_unref(pipeline); release_gpu(&gpu); close(pinned);
      fprintf(stderr, "allocated capture GPU import context unavailable\n"); return 1;
    }
    GstContext *context = gst_context_new(GST_GL_DISPLAY_CONTEXT_TYPE, TRUE);
    gst_context_set_gl_display(context, GST_GL_DISPLAY(gpu.display));
    gst_element_set_context(pipeline, context); gst_context_unref(context);
  }
  GstElement *source = gst_bin_get_by_name(GST_BIN(pipeline), "source");
  struct synthetic_source synthetic_state = {width, height, 0,
    synthetic && !strcmp(argv[1], "--self-test-empty"), synthetic && !strcmp(argv[1], "--self-test-frozen")};
  if (synthetic) {
    GstCaps *caps = gst_caps_new_simple("video/x-raw", "format", G_TYPE_STRING, "RGB", "width", G_TYPE_INT, (int)width,
      "height", G_TYPE_INT, (int)height, "framerate", GST_TYPE_FRACTION, 60, 1, NULL);
    gst_app_src_set_caps(GST_APP_SRC(source), caps); gst_caps_unref(caps);
    GstAppSrcCallbacks callbacks = {.need_data = synthetic_frame};
    gst_app_src_set_callbacks(GST_APP_SRC(source), &callbacks, &synthetic_state, NULL);
  } else {
    /* Connect through the retained dentry, not a pathname that can be replaced
     * and restored between identity checks. The FD lives through teardown. */
    char socket_path[64];
    snprintf(socket_path, sizeof(socket_path), "/proc/self/fd/%d", pinned);
    g_object_set(source, "socket-path", socket_path, NULL);
  }
  GstElement *encoded = gst_bin_get_by_name(GST_BIN(pipeline), "encoded");
  GstPad *pad = gst_element_get_static_pad(encoded, "src");
  struct encoded_stats stats = {0}; g_mutex_init(&stats.lock);
  gulong probe = gst_pad_add_probe(pad, GST_PAD_PROBE_TYPE_BUFFER, encoded_buffer, &stats, NULL);
  GstElement *decoded = gst_bin_get_by_name(GST_BIN(pipeline), "decoded");
  GstBus *bus = gst_element_get_bus(pipeline);
  unsigned frames = 0, scene_frames = 0, motion_frames = 0;
  struct scene_observation anchor = {0};
  const gint64 deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
  gboolean failed = gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE;
  while (!failed && !stopping && g_get_monotonic_time() < deadline && frames < FRAME_COUNT) {
    GstMessage *message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
    if (message) {
      GError *detail = NULL; gst_message_parse_error(message, &detail, NULL);
      if (detail) { fprintf(stderr, "encoded pipeline error: %.512s\n", detail->message); g_error_free(detail); }
      gst_message_unref(message); failed = TRUE; break;
    }
    g_mutex_lock(&stats.lock); failed = stats.failed; g_mutex_unlock(&stats.lock);
    if (failed) break;
    GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(decoded), 100 * GST_MSECOND);
    if (!sample) { if (gst_app_sink_is_eos(GST_APP_SINK(decoded))) break; continue; }
    struct scene_observation scene;
    if (!inspect_scene(sample, width, height, &scene)) failed = TRUE;
    else {
      ++frames;
      if (scene.found) {
        ++scene_frames;
        const double dx = scene.ball_x - anchor.ball_x, dy = scene.ball_y - anchor.ball_y;
        if (!anchor.found) anchor = scene;
        else if (dx * dx + dy * dy >= 16) { ++motion_frames; anchor = scene; }
      }
    }
    gst_sample_unref(sample);
  }
  if (gst_element_set_state(pipeline, GST_STATE_NULL) != GST_STATE_CHANGE_SUCCESS) {
    /* This isolated process cannot prove streaming threads have stopped. Keep
     * their probes, stack data, GL display and device alive until process exit;
     * ordinary object cleanup here could race their remaining GPU access. */
    const char message[] = "encoded pipeline teardown did not quiesce\n";
    const ssize_t reported = write(STDERR_FILENO, message, sizeof(message) - 1);
    (void)reported;
    _exit(1);
  }
  gst_pad_remove_probe(pad, probe);
  if (!synthetic) {
    struct stat after;
    if (lstat(argv[1], &after) || after.st_dev != identity.st_dev || after.st_ino != identity.st_ino ||
        after.st_mode != identity.st_mode || after.st_uid != identity.st_uid) failed = TRUE;
    if (close(pinned)) failed = TRUE;
  }
  if (!failed && !stopping && !stats.failed && frames == FRAME_COUNT && stats.frames == FRAME_COUNT &&
      scene_frames >= 30 && motion_frames >= 10 && stats.keyframes >= 1 && stats.bytes > 0) result = 0;
  printf("{\"source\":\"%s\",\"encoder\":\"openh264\",\"width\":%u,\"height\":%u,\"encoded_frames\":%u,\"decoded_frames\":%u,\"scene_frames\":%u,\"motion_frames\":%u,\"keyframes\":%u,\"encoded_bytes\":%" PRIu64 ",\"max_frame_bytes\":%zu,\"passed\":%s}\n",
    synthetic ? "synthetic" : "worker-capture", width, height, stats.frames, frames, scene_frames, motion_frames,
    stats.keyframes, stats.bytes, stats.maximum, result ? "false" : "true");
  gst_object_unref(bus); gst_object_unref(decoded); gst_object_unref(pad); gst_object_unref(encoded); gst_object_unref(source);
  gst_object_unref(pipeline); g_mutex_clear(&stats.lock);
  release_gpu(&gpu); gst_deinit();
  return result;
}
