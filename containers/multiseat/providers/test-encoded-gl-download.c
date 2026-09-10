/* Exercise the physical download chain with real EGL-wrapped GPU pixels using
 * software EGL. No GPU devices, capture socket or live compositor are needed. */
#define main encoded_game_main
#include "encoded-game-check.c"
#undef main
#include <gst/gl/egl/gsteglimage.h>
#include <gst/gl/egl/gstglmemoryegl.h>

struct wrapped_pattern {
  GstVideoInfo info;
  guint8 pixels[64 * 48 * 4];
  GstGLMemory *texture;
  GstMemory *wrapped;
};

static void wrap_pattern(GstGLContext *context, gpointer opaque) {
  struct wrapped_pattern *pattern = opaque;
  GstGLMemoryAllocator *allocator = gst_gl_memory_allocator_get_default(context);
  GstGLVideoAllocationParams *params = gst_gl_video_allocation_params_new_wrapped_data(
    context, NULL, &pattern->info, 0, NULL, GST_GL_TEXTURE_TARGET_2D, GST_GL_RGBA,
    pattern->pixels, NULL, NULL);
  pattern->texture = GST_GL_MEMORY_CAST(gst_gl_base_memory_alloc(
    GST_GL_BASE_MEMORY_ALLOCATOR(allocator), (GstGLAllocationParams *)params));
  gst_gl_allocation_params_free((GstGLAllocationParams *)params);
  gst_object_unref(allocator);
  g_assert_nonnull(pattern->texture);
  GstMapInfo mapped;
  g_assert_true(gst_memory_map(GST_MEMORY_CAST(pattern->texture), &mapped, GST_MAP_READ | GST_MAP_GL));
  GstEGLImage *image = gst_egl_image_from_texture(context, pattern->texture, NULL);
  gst_memory_unmap(GST_MEMORY_CAST(pattern->texture), &mapped);
  g_assert_nonnull(image);
  allocator = GST_GL_MEMORY_ALLOCATOR(gst_allocator_find(GST_GL_MEMORY_EGL_ALLOCATOR_NAME));
  g_assert_nonnull(allocator);
  params = gst_gl_video_allocation_params_new_wrapped_gl_handle(context, NULL, &pattern->info,
    0, NULL, GST_GL_TEXTURE_TARGET_2D, GST_GL_RGBA, image, NULL, NULL);
  pattern->wrapped = GST_MEMORY_CAST(gst_gl_base_memory_alloc(
    GST_GL_BASE_MEMORY_ALLOCATOR(allocator), (GstGLAllocationParams *)params));
  gst_gl_allocation_params_free((GstGLAllocationParams *)params);
  gst_object_unref(allocator);
  gst_egl_image_unref(image);
  g_assert_nonnull(pattern->wrapped);
  /* Seed any unused CPU shadow with defined black bytes. A missing readback
   * now fails deterministically instead of comparing uninitialized storage. */
  GstGLBaseMemory *memory = GST_GL_BASE_MEMORY_CAST(pattern->wrapped);
  g_assert_true(gst_gl_base_memory_alloc_data(memory));
  memset(memory->data, 0, memory->mem.maxsize);
}

int main(int argc, char **argv) {
  g_setenv("LIBGL_ALWAYS_SOFTWARE", "1", TRUE);
  g_setenv("GST_GL_PLATFORM", "egl", TRUE);
  g_setenv("GST_GL_API", "gles2", TRUE);
  g_setenv("GST_GL_WINDOW", "surfaceless", TRUE);
  gst_init(&argc, &argv);
  PFNEGLGETPLATFORMDISPLAYEXTPROC get_display = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
  g_assert_nonnull(get_display);
  EGLDisplay egl = get_display(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
  g_assert_true(egl != EGL_NO_DISPLAY && eglInitialize(egl, NULL, NULL));
  GstGLDisplay *display = GST_GL_DISPLAY(gst_gl_display_egl_new_with_egl_display(egl));
  gst_gl_display_filter_gl_api(display, GST_GL_API_GLES2);
  GstGLContext *context = gst_gl_context_new(display);
  GError *error = NULL;
  g_assert_true(gst_gl_context_create(context, NULL, &error));
  g_assert_no_error(error);
  GST_OBJECT_LOCK(display);
  g_assert_true(gst_gl_display_add_context(display, context));
  GST_OBJECT_UNLOCK(display);
  gst_gl_memory_egl_init_once();
  struct wrapped_pattern pattern = {0};
  g_assert_true(gst_video_info_set_format(&pattern.info, GST_VIDEO_FORMAT_RGBA, 64, 48));
  for (unsigned y = 0; y < 48; ++y) for (unsigned x = 0; x < 64; ++x) {
    guint8 *pixel = pattern.pixels + (y * 64 + x) * 4;
    pixel[0] = x < 32 ? 31 : 197;
    pixel[1] = y < 24 ? 211 : 53;
    pixel[2] = (x < 32) == (y < 24) ? 97 : 239;
    pixel[3] = 255;
  }
  gst_gl_context_thread_add(context, wrap_pattern, &pattern);
  GstElement *pipeline = gst_parse_launch("appsrc name=source format=time ! " CAPTURE_DOWNLOAD_CHAIN
    "video/x-raw,format=RGB ! appsink name=decoded sync=false", &error);
  g_assert_no_error(error);
  g_assert_nonnull(pipeline);
  GstContext *shared = gst_context_new(GST_GL_DISPLAY_CONTEXT_TYPE, TRUE);
  gst_context_set_gl_display(shared, display);
  gst_element_set_context(pipeline, shared);
  gst_context_unref(shared);
  shared = gst_context_new("gst.gl.app_context", TRUE);
  gst_structure_set(gst_context_writable_structure(shared), "context", GST_TYPE_GL_CONTEXT, context, NULL);
  gst_element_set_context(pipeline, shared);
  gst_context_unref(shared);
  GstElement *source = gst_bin_get_by_name(GST_BIN(pipeline), "source");
  GstElement *sink = gst_bin_get_by_name(GST_BIN(pipeline), "decoded");
  GstCaps *caps = gst_video_info_to_caps(&pattern.info);
  gst_caps_set_features(caps, 0, gst_caps_features_new(GST_CAPS_FEATURE_MEMORY_GL_MEMORY, NULL));
  gst_caps_set_simple(caps, "texture-target", G_TYPE_STRING, "2D", NULL);
  gst_app_src_set_caps(GST_APP_SRC(source), caps);
  gst_caps_unref(caps);
  g_assert_true(gst_element_set_state(pipeline, GST_STATE_PLAYING) != GST_STATE_CHANGE_FAILURE);
  GstBuffer *buffer = gst_buffer_new();
  gst_buffer_append_memory(buffer, pattern.wrapped);
  GST_BUFFER_PTS(buffer) = 0;
  g_assert_cmpint(gst_app_src_push_buffer(GST_APP_SRC(source), buffer), ==, GST_FLOW_OK);
  g_assert_cmpint(gst_app_src_end_of_stream(GST_APP_SRC(source)), ==, GST_FLOW_OK);
  GstSample *sample = gst_app_sink_try_pull_sample(GST_APP_SINK(sink), 5 * GST_SECOND);
  if (!sample) {
    GstBus *bus = gst_element_get_bus(pipeline);
    report_bus_error(bus);
    gst_object_unref(bus);
  }
  g_assert_nonnull(sample);
  GstVideoInfo info;
  g_assert_true(gst_video_info_from_caps(&info, gst_sample_get_caps(sample)));
  g_assert_cmpint(GST_VIDEO_INFO_FORMAT(&info), ==, GST_VIDEO_FORMAT_RGB);
  g_assert_cmpint(GST_VIDEO_INFO_WIDTH(&info), ==, 64);
  g_assert_cmpint(GST_VIDEO_INFO_HEIGHT(&info), ==, 48);
  GstVideoFrame frame;
  g_assert_true(gst_video_frame_map(&frame, &info, gst_sample_get_buffer(sample), GST_MAP_READ));
  for (unsigned y = 0; y < 48; ++y) for (unsigned x = 0; x < 64; ++x) {
    guint8 *actual = (guint8 *)GST_VIDEO_FRAME_PLANE_DATA(&frame, 0) + y * GST_VIDEO_FRAME_PLANE_STRIDE(&frame, 0) + x * 3;
    for (unsigned channel = 0; channel < 3; ++channel)
      g_assert_cmpuint(actual[channel], ==, pattern.pixels[(y * 64 + x) * 4 + channel]);
  }
  gst_video_frame_unmap(&frame);
  gst_sample_unref(sample);
  g_assert_cmpint(gst_element_set_state(pipeline, GST_STATE_NULL), ==, GST_STATE_CHANGE_SUCCESS);
  gst_object_unref(source);
  gst_object_unref(sink);
  gst_object_unref(pipeline);
  gst_memory_unref(GST_MEMORY_CAST(pattern.texture));
  gst_object_unref(context);
  gst_object_unref(display);
  g_assert_true(eglTerminate(egl));
  gst_deinit();
  puts("EGL-wrapped patterned pixels survived the physical download chain");
  return 0;
}
