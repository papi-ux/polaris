/* Bind a hardware encoder to the already admitted DRM descriptor. A factory's
 * discovery order is never GPU authority. GPU seats fail if no exact match exists. */
#ifndef POLARIS_ENCODER_GPU_H
#define POLARIS_ENCODER_GPU_H
#include <gst/gst.h>
#include <xf86drm.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>

enum encoder_kind { ENCODER_SOFTWARE, ENCODER_NVENC, ENCODER_VA };
struct encoder_choice {
  enum encoder_kind kind;
  gchar *factory;
  dev_t render_device;
  guint cuda_device;
};

static gboolean same_pci_address(const char *address, const drmPciBusInfo *pci) {
  unsigned domain, bus, device, function;
  char extra;
  return address && pci &&
    sscanf(address, "%x:%x:%x.%x%c", &domain, &bus, &device, &function, &extra) == 4 &&
    domain == pci->domain && bus == pci->bus && device == pci->dev && function == pci->func;
}

static gboolean cuda_device_for_pci(const drmPciBusInfo *pci, guint *result) {
  void *library = dlopen("libcuda.so.1", RTLD_NOW | RTLD_LOCAL);
  if (!library) return FALSE;
  /* CUDA driver ABI uses int for CUresult and CUdevice. No SDK or host toolkit
   * is needed; libcuda is supplied by the pinned NVIDIA userspace image. */
  int (*initialize)(unsigned) = dlsym(library, "cuInit");
  int (*count_devices)(int *) = dlsym(library, "cuDeviceGetCount");
  int (*get_device)(int *, int) = dlsym(library, "cuDeviceGet");
  int (*get_pci)(char *, int, int) = dlsym(library, "cuDeviceGetPCIBusId");
  int count = 0;
  unsigned matches = 0;
  if (initialize && count_devices && get_device && get_pci && !initialize(0) &&
      !count_devices(&count) && count > 0 && count <= 64) {
    for (int ordinal = 0; ordinal < count; ++ordinal) {
      int device;
      char address[32] = {0};
      if (!get_device(&device, ordinal) && !get_pci(address, sizeof(address), device) &&
          address[sizeof(address)-1] == '\0' && same_pci_address(address, pci)) {
        *result = (guint)ordinal;
        ++matches;
      }
    }
  }
  dlclose(library);
  return matches == 1;
}

static gboolean encoder_matches(GstElement *element, const struct encoder_choice *choice) {
  const char *property = choice->kind == ENCODER_NVENC ? "cuda-device-id" : "device-path";
  GParamSpec *spec = g_object_class_find_property(G_OBJECT_GET_CLASS(element), property);
  if (!spec || !(spec->flags & G_PARAM_READABLE)) return FALSE;
  if (choice->kind == ENCODER_NVENC) {
    if (G_PARAM_SPEC_VALUE_TYPE(spec) != G_TYPE_UINT) return FALSE;
    guint device = G_MAXUINT;
    g_object_get(element, property, &device, NULL);
    return device == choice->cuda_device;
  }
  if (G_PARAM_SPEC_VALUE_TYPE(spec) != G_TYPE_STRING) return FALSE;
  gchar *path = NULL;
  g_object_get(element, property, &path, NULL);
  int descriptor = path ? open(path, O_PATH | O_NOFOLLOW | O_CLOEXEC) : -1;
  struct stat status;
  gboolean matches = descriptor >= 0 && !fstat(descriptor, &status) &&
    S_ISCHR(status.st_mode) && status.st_rdev == choice->render_device;
  if (descriptor >= 0) close(descriptor);
  g_free(path);
  return matches;
}

static gboolean h264_factory_name(const char *name, enum encoder_kind kind) {
  if (!name || strlen(name) > 80) return FALSE;
  for (const char *p = name; *p; ++p) if (!g_ascii_isalnum(*p)) return FALSE;
  if (kind == ENCODER_VA) return g_str_has_prefix(name, "va") && g_str_has_suffix(name, "h264enc");
  if (!strcmp(name, "nvh264enc")) return TRUE;
  const char prefix[] = "nvh264device";
  if (!g_str_has_prefix(name, prefix) || !g_str_has_suffix(name, "enc")) return FALSE;
  const char *end = name + strlen(name) - 3;
  if (name + sizeof(prefix) - 1 >= end) return FALSE;
  for (const char *p = name + sizeof(prefix) - 1; p < end; ++p) if (!g_ascii_isdigit(*p)) return FALSE;
  return TRUE;
}

static gboolean choose_hardware_encoder(int render_descriptor, struct encoder_choice *choice) {
  struct stat status;
  drmDevicePtr device = NULL;
  if (fstat(render_descriptor, &status) || !S_ISCHR(status.st_mode) ||
      drmGetDevice2(render_descriptor, 0, &device) || !device) return FALSE;
  gboolean admitted = FALSE;
  if (device->bustype == DRM_BUS_PCI && device->businfo.pci && device->deviceinfo.pci) {
    if (device->deviceinfo.pci->vendor_id == 0x10de) {
      choice->kind = ENCODER_NVENC;
      admitted = cuda_device_for_pci(device->businfo.pci, &choice->cuda_device);
    } else if (device->deviceinfo.pci->vendor_id == 0x1002 || device->deviceinfo.pci->vendor_id == 0x8086) {
      choice->kind = ENCODER_VA;
      admitted = TRUE;
    }
  }
  drmFreeDevice(&device);
  if (!admitted) return FALSE;
  choice->render_device = status.st_rdev;
  GList *factories = gst_element_factory_list_get_elements(GST_ELEMENT_FACTORY_TYPE_VIDEO_ENCODER, GST_RANK_NONE);
  const char *plugin = choice->kind == ENCODER_NVENC ? "nvcodec" : "va";
  for (GList *item = factories; item && !choice->factory; item = item->next) {
    GstElementFactory *factory = item->data;
    const char *name = gst_plugin_feature_get_name(GST_PLUGIN_FEATURE(factory));
    if (g_strcmp0(gst_plugin_feature_get_plugin_name(GST_PLUGIN_FEATURE(factory)), plugin) ||
        !h264_factory_name(name, choice->kind)) continue;
    GstElement *candidate = gst_element_factory_create(factory, NULL);
    if (candidate && encoder_matches(candidate, choice)) choice->factory = g_strdup(name);
    if (candidate) gst_object_unref(candidate);
  }
  gst_plugin_feature_list_free(factories);
  return choice->factory != NULL;
}

/* Called only with the whole pipeline in READY, before media is released.
 * Read back every rate-control property; an unsupported setting is a failure. */
static inline gboolean encoder_uint_property(GstElement *encoder, const char *name, guint value) {
  GParamSpec *spec = g_object_class_find_property(G_OBJECT_GET_CLASS(encoder), name);
  if (!spec || G_PARAM_SPEC_VALUE_TYPE(spec) != G_TYPE_UINT ||
      !(spec->flags & G_PARAM_WRITABLE) || !(spec->flags & G_PARAM_READABLE)) return FALSE;
  GParamSpecUInt *range = G_PARAM_SPEC_UINT(spec);
  if (value < range->minimum || value > range->maximum) return FALSE;
  g_object_set(encoder, name, value, NULL);
  guint actual = 0;
  g_object_get(encoder, name, &actual, NULL);
  return actual == value;
}

static inline gboolean encoder_select_bitrate(GstElement *encoder, const struct encoder_choice *choice,
    unsigned bitrate, unsigned refresh) {
  if (!bitrate || bitrate > 8000 || !refresh) return FALSE;
  const guint rate = choice->kind == ENCODER_SOFTWARE ? bitrate * 1000u : bitrate;
  const guint buffer = (bitrate * 1000u + refresh - 1) / refresh;
  return encoder_uint_property(encoder, "bitrate", rate) &&
    (choice->kind == ENCODER_VA || encoder_uint_property(encoder, "max-bitrate", rate)) &&
    (choice->kind == ENCODER_SOFTWARE || encoder_uint_property(encoder,
      choice->kind == ENCODER_NVENC ? "vbv-buffer-size" : "cpb-size", buffer));
}

static gchar *encoder_description(const struct encoder_choice *choice, unsigned bitrate, unsigned refresh) {
  /* One frame of VBV/CPB capacity, in kbits. Capture still downloads through
   * the verified GL path; hardware encoding does not imply zero-copy capture. */
  const unsigned buffer = (bitrate * 1000u + refresh - 1) / refresh;
  /* OpenH264 2.6 drops NVENC recovery IDRs with very low-QP CAVLC residuals
   * while returning success from the pipeline. A QP floor of 10 keeps the
   * constrained-baseline stream decodable by that client decoder as well as
   * FFmpeg. This is an explicit quality/compatibility tradeoff. */
  /* The locked worker nvcodec patch also fixes DPB and prediction to one
   * reference. Moonlight clients without reference invalidation require it. */
  /* No periodic keyframe. A client asks for one when it loses a frame, and
   * encode-media.c answers with force-key-unit, as the host encoder does. A
   * fixed 60-frame GOP sent an IDR twice a second at 120 fps: a bitrate spike,
   * a quality pulse and a fresh SPS every half second on an 8 Mbps stream. The
   * VA and OpenH264 values below are their own ways to say the same thing:
   * VA reads 0 as "work one out", so it gets its largest interval, and
   * OpenH264 reads an intra period of 0 as none. */
  if (choice->kind == ENCODER_NVENC)
    return g_strdup_printf("%s name=encoder bitrate=%u max-bitrate=%u rc-mode=cbr "
      "gop-size=-1 bframes=0 rc-lookahead=0 zerolatency=true preset=p1 tune=ultra-low-latency "
      "cabac=false qp-min-i=10 qp-min-p=10 repeat-sequence-header=true vbv-buffer-size=%u", choice->factory, bitrate, bitrate, buffer);
  if (choice->kind == ENCODER_VA)
    return g_strdup_printf("%s name=encoder bitrate=%u rate-control=cbr key-int-max=1024 "
      "b-frames=0 ref-frames=1 cabac=false dct8x8=false aud=true cpb-size=%u", choice->factory, bitrate, buffer);
  return g_strdup_printf("openh264enc name=encoder bitrate=%u max-bitrate=%u "
    "rate-control=bitrate gop-size=0 usage-type=screen complexity=low", bitrate * 1000, bitrate * 1000);
}
#endif
