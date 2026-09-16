/* Adversarial allocation checks without opening a GPU or initializing CUDA. */
#define _GNU_SOURCE
#include "encoder-gpu.h"
#include <glib/gstdio.h>

typedef struct { GstElement parent; guint device; const char *path; } TestEncoder;
typedef struct { GstElementClass parent; } TestEncoderClass;
G_DEFINE_TYPE(TestEncoder, test_encoder, GST_TYPE_ELEMENT)
static void test_encoder_get_property(GObject *object, guint property, GValue *value, GParamSpec *spec) {
  TestEncoder *encoder = (TestEncoder *)object;
  if (property == 1) g_value_set_uint(value, encoder->device);
  else if (property == 2) g_value_set_string(value, encoder->path);
  else G_OBJECT_WARN_INVALID_PROPERTY_ID(object, property, spec);
}
static void test_encoder_init(TestEncoder *encoder) { encoder->device = 0; encoder->path = "/dev/null"; }
static void test_encoder_class_init(TestEncoderClass *klass) {
  GObjectClass *object = G_OBJECT_CLASS(klass);
  object->get_property = test_encoder_get_property;
  g_object_class_install_property(object, 1, g_param_spec_uint("cuda-device-id", "Device", "Device", 0, G_MAXUINT, 0, G_PARAM_READABLE));
  g_object_class_install_property(object, 2, g_param_spec_string("device-path", "Path", "Path", NULL, G_PARAM_READABLE));
}
int main(int argc, char **argv) {
  gst_init(&argc, &argv);
  drmPciBusInfo pci = {.domain=0, .bus=1, .dev=2, .func=0};
  g_assert_true(same_pci_address("0000:01:02.0", &pci));
  g_assert_false(same_pci_address("0000:01:02.1", &pci));
  g_assert_false(same_pci_address("0001:01:02.0", &pci));
  g_assert_false(same_pci_address("0000:01:02.0-extra", &pci));
  g_assert_false(same_pci_address("01:02.0", &pci));
  g_assert_true(h264_factory_name("nvh264enc", ENCODER_NVENC));
  g_assert_true(h264_factory_name("nvh264device1enc", ENCODER_NVENC));
  g_assert_false(h264_factory_name("nvautogpuh264enc", ENCODER_NVENC));
  g_assert_false(h264_factory_name("nvh264deviceenc", ENCODER_NVENC));
  g_assert_false(h264_factory_name("nvh264devicewrongenc", ENCODER_NVENC));
  g_assert_false(h264_factory_name("nvh264enc ! fakesink", ENCODER_NVENC));
  g_assert_true(h264_factory_name("varenderD129h264enc", ENCODER_VA));
  g_assert_false(h264_factory_name("vah265enc", ENCODER_VA));

  struct encoder_choice choice = {.kind=ENCODER_NVENC, .cuda_device=1};
  TestEncoder *encoder = g_object_new(test_encoder_get_type(), NULL);
  g_assert_false(encoder_matches(GST_ELEMENT(encoder), &choice));
  encoder->device = 1;
  g_assert_true(encoder_matches(GST_ELEMENT(encoder), &choice));
  encoder->device = 2;
  g_assert_false(encoder_matches(GST_ELEMENT(encoder), &choice));
  GstElement *identity = gst_element_factory_make("identity", NULL);
  g_assert_nonnull(identity);
  g_assert_false(encoder_matches(identity, &choice));
  gst_object_unref(identity);

  struct stat status;
  g_assert_cmpint(stat("/dev/null", &status), ==, 0);
  choice.kind = ENCODER_VA; choice.render_device = status.st_rdev;
  g_assert_true(encoder_matches(GST_ELEMENT(encoder), &choice));
  encoder->path = "/dev/zero";
  g_assert_false(encoder_matches(GST_ELEMENT(encoder), &choice));
  gchar *directory = g_dir_make_tmp("encoder-device-XXXXXX", NULL);
  g_assert_nonnull(directory);
  gchar *path = g_build_filename(directory, "device", NULL);
  g_assert_cmpint(symlink("/dev/null", path), ==, 0);
  encoder->path = path;
  g_assert_false(encoder_matches(GST_ELEMENT(encoder), &choice));
  g_assert_cmpint(unlink(path), ==, 0);
  g_assert_true(g_file_set_contents(path, "not a device", -1, NULL));
  g_assert_false(encoder_matches(GST_ELEMENT(encoder), &choice));
  g_assert_cmpint(unlink(path), ==, 0); g_assert_cmpint(rmdir(directory), ==, 0);
  g_free(path); g_free(directory); gst_object_unref(encoder);
  g_assert_false(choose_hardware_encoder(-1, &choice));
  choice.kind = ENCODER_NVENC; choice.factory = g_strdup("nvh264device1enc");
  gchar *description = encoder_description(&choice, 8000, 60000);
  g_assert_nonnull(strstr(description, "vbv-buffer-size=134"));
  g_free(description); g_free(choice.factory);
  gst_deinit();
  puts("allocated PCI/CUDA identity, per-device factories and VA device identity checks passed");
  return 0;
}
