/* Shared allocated-GPU import context. Pipelines and samples must retire
 * before the retained DRM descriptor and EGL/GBM objects are released. */
#ifndef POLARIS_CAPTURE_GPU_H
#define POLARIS_CAPTURE_GPU_H
#include <gst/gst.h>
#include <gst/gl/gl.h>
#include <gst/gl/egl/gstgldisplay_egl.h>
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <gbm.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define CAPTURE_DOWNLOAD_CHAIN \
  "glupload name=upload ! video/x-raw(memory:GLMemory),format=RGBA,texture-target=2D ! " \
  "glcolorconvert name=convert ! video/x-raw(memory:GLMemory),format=BGRA,texture-target=2D ! " \
  "gldownload name=download ! videoconvert ! "

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

#endif
