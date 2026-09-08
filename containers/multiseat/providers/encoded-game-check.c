/* Bounded physical acceptance probe. The software encoder is selected explicitly;
 * no production encoder selection or client media routing is enabled here. */
#define _GNU_SOURCE
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video.h>
#include <gst/gl/gl.h>
#include <gst/gl/gstglfuncs.h>
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
static gint driver_error_reported;
static void first_driver_error(GstDebugCategory *category, GstDebugLevel level, const gchar *file,
    const gchar *function, gint line, GObject *object, GstDebugMessage *message, gpointer unused) {
  (void)file; (void)function; (void)line; (void)object; (void)unused;
  if (level > GST_LEVEL_WARNING || strcmp(gst_debug_category_get_name(category), "gldebug") ||
      !g_atomic_int_compare_and_exchange(&driver_error_reported, 0, 1)) return;
  const gchar *detail = gst_debug_message_get(message);
  fprintf(stderr, "GL driver: %.240s\n", detail ? detail : "unspecified error");
}
struct encoded_stats {
  GMutex lock;
  unsigned frames, keyframes;
  uint64_t bytes;
  size_t maximum;
  gboolean failed;
};
struct synthetic_source { unsigned width, height, frame; gboolean empty, frozen; };

struct gl_error_sample { gboolean checked; guint error; };
struct import_observation {
  GMutex lock;
  GstPad *pad;
  gulong probe;
  unsigned buffers, first_memories, first_planes, first_flags;
  size_t first_bytes;
  char memory_type[25];
  GstGLContext *context;
  struct gl_error_sample first_gl, final_gl;
  gboolean validate_target;
  gint invalid_target;
};

static gboolean matching_texture_target(GstPad *pad, GstBuffer *buffer) {
  if (gst_buffer_n_memory(buffer) != 1) return FALSE;
  GstMemory *memory = gst_buffer_peek_memory(buffer, 0);
  if (!gst_is_gl_memory(memory)) return FALSE;
  GstCaps *caps = gst_pad_get_current_caps(pad);
  gboolean valid = FALSE;
  if (caps && gst_caps_get_size(caps) == 1 && gst_caps_is_fixed(caps)) {
    const char *name = gst_structure_get_string(gst_caps_get_structure(caps, 0), "texture-target");
    GstGLTextureTarget expected = name ? gst_gl_texture_target_from_string(name) : GST_GL_TEXTURE_TARGET_NONE;
    valid = expected != GST_GL_TEXTURE_TARGET_NONE &&
      expected == gst_gl_memory_get_texture_target(GST_GL_MEMORY_CAST(memory));
  }
  if (caps) gst_caps_unref(caps);
  return valid;
}

static void inspect_gl_error(GstGLContext *context, gpointer opaque) {
  struct gl_error_sample *sample = opaque;
  if (context->gl_vtable->GetError) {
    sample->error = context->gl_vtable->GetError();
    sample->checked = TRUE;
  }
}

/* Observe metadata only. Never CPU-map a non-linear DMA-BUF for diagnosis. */
static GstPadProbeReturn import_buffer(GstPad *pad, GstPadProbeInfo *info, gpointer opaque) {
  (void)pad;
  struct import_observation *observation = opaque;
  GstBuffer *buffer = GST_PAD_PROBE_INFO_BUFFER(info);
  if (!buffer) return GST_PAD_PROBE_OK;
  /* A renegotiated uploader must not deliver a texture with a different GL
   * target. Reject every mismatched buffer before a downstream texture bind. */
  if (observation->validate_target && !matching_texture_target(pad, buffer)) {
    if (g_atomic_int_compare_and_exchange(&observation->invalid_target, 0, 1))
      fprintf(stderr, "import texture target does not match negotiated caps\n");
    return GST_PAD_PROBE_DROP;
  }
  g_mutex_lock(&observation->lock);
  if (observation->buffers++ == 0) {
    observation->first_memories = gst_buffer_n_memory(buffer);
    observation->first_bytes = gst_buffer_get_size(buffer);
    observation->first_flags = GST_BUFFER_FLAGS(buffer);
    GstVideoMeta *meta = gst_buffer_get_video_meta(buffer);
    observation->first_planes = meta ? meta->n_planes : 0;
    if (observation->first_memories) {
      GstMemory *memory = gst_buffer_peek_memory(buffer, 0);
      const char *type = memory && memory->allocator ? memory->allocator->mem_type : NULL;
      snprintf(observation->memory_type, sizeof(observation->memory_type), "%.24s", type ? type : "unknown");
      /* thread_add is synchronous. Retain the actual memory's context through
       * a final check after delayed CPU readback and proven pipeline quiescence. */
      if (memory && gst_is_gl_memory(memory)) {
        observation->context = gst_object_ref(GST_GL_BASE_MEMORY_CAST(memory)->context);
        gst_gl_context_thread_add(observation->context, inspect_gl_error, &observation->first_gl);
      }
    }
  }
  g_mutex_unlock(&observation->lock);
  return GST_PAD_PROBE_OK;
}

static gboolean report_bus_error(GstBus *bus) {
  GstMessage *message = gst_bus_pop_filtered(bus, GST_MESSAGE_ERROR);
  if (!message) return FALSE;
  GError *detail = NULL; gst_message_parse_error(message, &detail, NULL);
  const char *name = GST_MESSAGE_SRC(message) ? GST_OBJECT_NAME(GST_MESSAGE_SRC(message)) : NULL;
  fprintf(stderr, "encoded pipeline error at %.32s: %.256s\n", name ? name : "unknown", detail ? detail->message : "unknown");
  if (detail) g_error_free(detail);
  gst_message_unref(message);
  return TRUE;
}

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

struct scene_observation {
  gboolean found;
  double ball_x, ball_y;
  unsigned teal, orange, dark, white, total;
  char preview[8 * 8 * 6 + 1];
};
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
  /* Bounded diagnostic of already-decoded CPU RGB. This never maps the
   * captured DMA-BUF and does not contribute to scene acceptance. */
  for (unsigned row = 0; row < 8; ++row) for (unsigned column = 0; column < 8; ++column) {
    const unsigned char *p = pixels + (size_t)((2 * row + 1) * height / 16) * stride +
      (size_t)((2 * column + 1) * width / 16) * 3;
    snprintf(scene->preview + (row * 8 + column) * 6, 7, "%02x%02x%02x", p[0], p[1], p[2]);
  }
  scene->teal = teal; scene->orange = orange; scene->dark = dark; scene->white = white; scene->total = total;
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

static gboolean valid_capture_socket(const struct stat *identity) {
  /* GSocket creates 0700 under the provider's 0077 umask; owner execute
   * permission has no extra authority on an AF_UNIX socket. */
  return S_ISSOCK(identity->st_mode) && identity->st_uid == geteuid() &&
    ((identity->st_mode & 07777) == 0600 || (identity->st_mode & 07777) == 0700);
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
  if (fstat(fd, identity) || !valid_capture_socket(identity)) { close(fd); return -1; }
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
  if (!width || !height) { fprintf(stderr, "encoded probe dimensions or invocation invalid\n"); return 1; }
  struct stat identity;
  int pinned = synthetic ? -1 : pin_capture_socket(argv[1], &identity);
  if (!synthetic && pinned < 0) { fprintf(stderr, "allocated capture socket identity unavailable\n"); return 1; }
  struct sigaction action = {0}; action.sa_handler = stop;
  sigaction(SIGTERM, &action, NULL); sigaction(SIGINT, &action, NULL);
  /* The executable itself never forks a registry-scanner child. */
  g_setenv("GST_REGISTRY_FORK", "no", TRUE);
  g_setenv("GST_GL_PLATFORM", "egl", TRUE);
  g_setenv("GST_GL_API", "gles2", TRUE);
  GError *error = NULL;
  gst_init(NULL, NULL);
  if (!synthetic) {
    /* GStreamer enables the driver's GL debug callback at WARNING or above.
     * Keep only its first bounded message; repeated driver errors must not
     * flood the worker's diagnostic pipe. This isolated executable owns logging. */
    gst_debug_remove_log_function(gst_debug_log_default);
    gst_debug_add_log_function(first_driver_error, NULL, NULL);
    gst_debug_set_threshold_for_name("gldebug", GST_LEVEL_WARNING);
  }
  /* unixfdsrc 1.26 deserializes only registered meta implementations. Register
   * before receiving any frame so the producer's DMA-BUF offsets and strides
   * survive the process boundary; caps cannot describe a non-linear layout. */
  if (!gst_video_meta_get_info()) {
    if (pinned >= 0) close(pinned);
    gst_deinit();
    fprintf(stderr, "capture video metadata registration unavailable\n"); return 1;
  }
  /* 1.26 can retain its 2D uploader after forced-OES renegotiation. Negotiate
   * 2D explicitly and verify each emitted memory target before conversion. */
  const char *head = synthetic ? "appsrc name=source format=time ! videoconvert ! " :
    "unixfdsrc name=source num-buffers=60 ! glupload name=upload ! video/x-raw(memory:GLMemory),format=RGBA,texture-target=2D ! "
    "glcolorconvert name=convert ! video/x-raw(memory:GLMemory),format=RGBA,texture-target=2D ! gldownload name=download ! videoconvert ! ";
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
  const char *import_stages[] = {"source", "upload", "convert", "download"};
  struct import_observation imports[4] = {0};
  if (!synthetic) for (unsigned i = 0; i < 4; ++i) {
    g_mutex_init(&imports[i].lock);
    imports[i].validate_target = i == 1 || i == 2;
    GstElement *element = gst_bin_get_by_name(GST_BIN(pipeline), import_stages[i]);
    imports[i].pad = gst_element_get_static_pad(element, "src");
    imports[i].probe = gst_pad_add_probe(imports[i].pad, GST_PAD_PROBE_TYPE_BUFFER, import_buffer, &imports[i], NULL);
    gst_object_unref(element);
  }
  unsigned frames = 0, scene_frames = 0, motion_frames = 0;
  struct scene_observation anchor = {0};
  struct scene_observation last_scene = {0};
  const gint64 deadline = g_get_monotonic_time() + 10 * G_USEC_PER_SEC;
  gboolean failed = gst_element_set_state(pipeline, GST_STATE_PLAYING) == GST_STATE_CHANGE_FAILURE;
  while (!failed && !stopping && g_get_monotonic_time() < deadline && frames < FRAME_COUNT) {
    if (report_bus_error(bus)) { failed = TRUE; break; }
    g_mutex_lock(&stats.lock); failed = stats.failed; g_mutex_unlock(&stats.lock);
    if (!synthetic) for (unsigned i = 0; i < 4; ++i)
      failed |= g_atomic_int_get(&imports[i].invalid_target) != 0;
    if (failed) break;
    GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(decoded), 100 * GST_MSECOND);
    if (!sample) { if (gst_app_sink_is_eos(GST_APP_SINK(decoded))) break; continue; }
    struct scene_observation scene;
    if (!inspect_scene(sample, width, height, &scene)) failed = TRUE;
    else {
      last_scene = scene;
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
  /* Pulling from appsink can observe terminal flow before the main loop's
   * next bus check. Preserve that queued error and its element identity. */
  if (frames != FRAME_COUNT && report_bus_error(bus)) failed = TRUE;
  if (!synthetic && (failed || frames != FRAME_COUNT)) {
    GstPad *source_pad = gst_element_get_static_pad(source, "src");
    GstCaps *caps = source_pad ? gst_pad_get_current_caps(source_pad) : NULL;
    if (caps && gst_caps_get_size(caps) == 1) {
      const GstStructure *structure = gst_caps_get_structure(caps, 0);
      const char *format = gst_structure_get_string(structure, "format");
      const char *drm_format = gst_structure_get_string(structure, "drm-format");
      fprintf(stderr, "capture import format=%.16s drm-format=%.64s\n", format ? format : "unknown", drm_format ? drm_format : "unknown");
    }
    if (caps) gst_caps_unref(caps);
    if (source_pad) gst_object_unref(source_pad);
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
  if (!synthetic) for (unsigned i = 0; i < 4; ++i) {
    failed |= g_atomic_int_get(&imports[i].invalid_target) != 0;
    if (imports[i].context)
      gst_gl_context_thread_add(imports[i].context, inspect_gl_error, &imports[i].final_gl);
    if ((imports[i].first_gl.checked && imports[i].first_gl.error) ||
        (imports[i].final_gl.checked && imports[i].final_gl.error)) failed = TRUE;
    if (failed || frames != FRAME_COUNT || scene_frames < 30 || motion_frames < 10) fprintf(stderr,
      "import %s gl_first=%d:%x gl_final=%d:%x\n", import_stages[i],
      imports[i].first_gl.checked, imports[i].first_gl.error, imports[i].final_gl.checked, imports[i].final_gl.error);
  }
  if (!synthetic && frames && (scene_frames < 30 || motion_frames < 10)) fprintf(stderr,
    "decoded scene teal=%u orange=%u dark=%u white=%u total=%u preview8x8=%s\n",
    last_scene.teal, last_scene.orange, last_scene.dark, last_scene.white, last_scene.total, last_scene.preview);
  if (!synthetic) for (unsigned i = 0; i < 4; ++i) {
    struct import_observation *observation = &imports[i];
    if (failed || frames != FRAME_COUNT) fprintf(stderr,
      "import %s buffers=%u memories=%u bytes=%zu type=%.24s planes=%u flags=%x\n",
      import_stages[i], observation->buffers, observation->first_memories, observation->first_bytes,
      observation->memory_type, observation->first_planes, observation->first_flags);
    gst_pad_remove_probe(observation->pad, observation->probe);
    gst_object_unref(observation->pad);
    if (observation->context) gst_object_unref(observation->context);
    g_mutex_clear(&observation->lock);
  }
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
