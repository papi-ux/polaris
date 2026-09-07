/** @file tests/unit/platform/test_kms_named_binding.cpp */
#include "../../tests_common.h"
#include "src/platform/linux/kms_named_binding.h"
#include "src/platform/linux/kms_frame_transfer.h"
#include "src/video.h"
#include <map>
#include <fcntl.h>
#include <unistd.h>

namespace {
  struct drm_fixture_t {
    drmModeConnector connector_value {};
    drmModeEncoder encoder_value {};
    std::vector<uint32_t> plane_ids {10, 11};
    std::map<uint32_t, drmModePlane> plane_values;
    std::map<uint32_t, uint64_t> types {{10, DRM_PLANE_TYPE_PRIMARY}, {11, DRM_PLANE_TYPE_CURSOR}};
    bool missing_connector {}, missing_encoder {}, missing_resources {};
    drm_fixture_t() {
      connector_value.connector_id = 1;
      connector_value.encoder_id = 2;
      connector_value.connector_type = DRM_MODE_CONNECTOR_DisplayPort;
      connector_value.connector_type_id = 3;
      connector_value.connection = DRM_MODE_CONNECTED;
      encoder_value.crtc_id = 4;
      plane_values[10] = {.plane_id = 10, .crtc_id = 4, .fb_id = 100};
      plane_values[11] = {.plane_id = 11, .crtc_id = 4, .fb_id = 101};
    }
    auto connector(int, uint32_t) { return missing_connector ? std::optional<drmModeConnector> {} : connector_value; }
    auto encoder(int, uint32_t) { return missing_encoder ? std::optional<drmModeEncoder> {} : encoder_value; }
    auto planes(int) {
      return missing_resources ? std::optional<drmModePlaneRes> {} : drmModePlaneRes {static_cast<uint32_t>(plane_ids.size()), plane_ids.data()};
    }
    auto plane(int, uint32_t id) {
      const auto it = plane_values.find(id);
      return it == plane_values.end() ? std::optional<drmModePlane> {} : it->second;
    }
    auto plane_type(int, uint32_t id) {
      const auto it = types.find(id);
      return it == types.end() ? std::optional<uint64_t> {} : it->second;
    }
  };
  const platf::kms_selection::binding_t binding {"DP-3", 1, 4, 10};
}

TEST(KmsNamedBindingTests, LiveQueriesAcceptTheSelectedPrimaryAndIgnoreItsCursor) {
  drm_fixture_t api;
  EXPECT_TRUE(platf::kms_selection::binding_matches(api, -1, binding));
  api.plane_values[11].fb_id = 0;
  EXPECT_TRUE(platf::kms_selection::binding_matches(api, -1, binding));
}

TEST(KmsNamedBindingTests, DisconnectAndReassignmentInvalidateTheExistingCapture) {
  for (int change = 0; change < 8; ++change) {
    drm_fixture_t api;
    ASSERT_TRUE(platf::kms_selection::binding_matches(api, -1, binding));
    switch (change) {
      case 0: api.missing_connector = true; break;
      case 1: api.connector_value.connection = DRM_MODE_DISCONNECTED; break;
      case 2: api.connector_value.connector_type_id = 4; break;
      case 3: api.encoder_value.crtc_id = 5; break;
      case 4: api.plane_values[10].crtc_id = 5; break;
      case 5: api.plane_values[10].fb_id = 0; break;
      case 6: api.plane_values.erase(10); break;
      case 7: api.missing_encoder = true; break;
    }
    EXPECT_FALSE(platf::kms_selection::binding_matches(api, -1, binding)) << change;
  }
}

TEST(KmsNamedBindingTests, FreshPlaneResourcesDetectCompetitionAndUnknownPlaneTypes) {
  drm_fixture_t api;
  ASSERT_TRUE(platf::kms_selection::binding_matches(api, -1, binding));
  // A plane added after the previous read must be seen by the post-export read.
  api.plane_ids.push_back(12);
  api.plane_values[12] = {.plane_id = 12, .crtc_id = 4, .fb_id = 102};
  api.types[12] = DRM_PLANE_TYPE_OVERLAY;
  EXPECT_FALSE(platf::kms_selection::binding_matches(api, -1, binding));
  api.plane_values[12].crtc_id = 5;
  EXPECT_TRUE(platf::kms_selection::binding_matches(api, -1, binding));
  api.types.erase(10);
  EXPECT_FALSE(platf::kms_selection::binding_matches(api, -1, binding));
  api.types[10] = DRM_PLANE_TYPE_PRIMARY;
  api.missing_resources = true;
  EXPECT_FALSE(platf::kms_selection::binding_matches(api, -1, binding));
}

TEST(KmsFrameTransferTests, RejectedExportsCannotCloseAnUnrelatedReplacementDescriptor) {
  egl::img_descriptor_t image;
  int exported = -1;
  const auto status = platf::kms_capture::refresh_owned_frame(image, [&](auto *fds, auto *surface, auto &) {
    fds[0].el = open("/dev/null", O_RDONLY | O_CLOEXEC);
    exported = fds[0].el;
    surface->fds[0] = exported;
    return platf::capture_e::reinit;
  });
  ASSERT_GE(exported, 0);
  ASSERT_EQ(status, platf::capture_e::reinit);
  EXPECT_EQ(fcntl(exported, F_GETFD), -1);
  const int replacement = open("/dev/null", O_RDONLY | O_CLOEXEC);
  ASSERT_GE(replacement, 0);
  if (replacement != exported) { ASSERT_EQ(dup2(replacement, exported), exported); close(replacement); }
  image.reset();
  EXPECT_NE(fcntl(exported, F_GETFD), -1);
  close(exported);
}

TEST(KmsFrameTransferTests, SuccessfulExportsTransferExactlyOneOwnerToTheImage) {
  egl::img_descriptor_t image;
  int exported = -1;
  ASSERT_EQ(platf::kms_capture::refresh_owned_frame(image, [&](auto *fds, auto *surface, auto &timestamp) {
    fds[0].el = open("/dev/null", O_RDONLY | O_CLOEXEC);
    exported = surface->fds[0] = fds[0].el;
    timestamp = std::chrono::steady_clock::now();
    return platf::capture_e::ok;
  }), platf::capture_e::ok);
  ASSERT_GE(exported, 0);
  EXPECT_EQ(image.sd.fds[0], exported);
  EXPECT_NE(fcntl(exported, F_GETFD), -1);
  EXPECT_TRUE(image.frame_timestamp.has_value());
  image.reset();
  EXPECT_EQ(fcntl(exported, F_GETFD), -1);
}

TEST(KmsFrameTransferTests, PostExportPlaneMovementOrCompetitionRejectsTheFrameWithoutTransferringDescriptors) {
  for (const bool competing : {false, true}) {
    drm_fixture_t api;
    egl::img_descriptor_t image;
    int exported = -1;
    const auto status = platf::kms_capture::refresh_owned_frame(image, [&](auto *fds, auto *surface, auto &) {
      if (!platf::kms_selection::binding_matches(api, -1, binding)) return platf::capture_e::reinit;
      fds[0].el = open("/dev/null", O_RDONLY | O_CLOEXEC);
      exported = surface->fds[0] = fds[0].el;
      if (competing) {
        api.plane_ids.push_back(12);
        api.plane_values[12] = {.plane_id = 12, .crtc_id = 4, .fb_id = 102};
        api.types[12] = DRM_PLANE_TYPE_OVERLAY;
      } else {
        api.plane_values[10].crtc_id = 5;
      }
      return platf::kms_selection::binding_matches(api, -1, binding) ? platf::capture_e::ok : platf::capture_e::reinit;
    });
    EXPECT_EQ(status, platf::capture_e::reinit);
    ASSERT_GE(exported, 0);
    EXPECT_EQ(fcntl(exported, F_GETFD), -1);
    for (const auto fd : image.sd.fds) EXPECT_EQ(fd, -1);
  }
}

namespace platf {
  std::optional<std::string> kms_capture_binding_for_tests(display_t &display);
  std::vector<std::string> kms_display_names(mem_type_e type);
  std::shared_ptr<display_t> kms_display(mem_type_e type, const std::string &name, const video::config_t &config);
}

TEST(KmsPhysicalCaptureTests, QualifiedAndNumericSelectionsCaptureBoundedFramesFromTheSameOutput) {
  if (!std::getenv("POLARIS_TEST_KMS_CAPTURE")) GTEST_SKIP() << "Requires an explicitly isolated, capability-bearing physical capture test";
  ASSERT_EQ(gbm::init(), 0);
  const auto names = platf::kms_display_names(platf::mem_type_e::vulkan);
  ASSERT_FALSE(names.empty());
  const auto selected = std::find_if(names.begin(), names.end(), [](const auto &name) { return name.starts_with("kms:"); });
  ASSERT_NE(selected, names.end());
  const auto numeric = std::to_string(std::distance(names.begin(), selected));
  video::config_t config {};
  config.framerate = config.encodingFramerate = 60;
  std::optional<std::pair<int, int>> dimensions;
  std::optional<std::string> resolved_binding;
  for (const auto &name : {*selected, numeric}) {
    const auto start = std::chrono::steady_clock::now();
    auto display = platf::kms_display(platf::mem_type_e::vulkan, name, config);
    ASSERT_NE(display, nullptr);
    const auto current_binding = platf::kms_capture_binding_for_tests(*display);
    ASSERT_TRUE(current_binding);
    if (resolved_binding) { EXPECT_EQ(current_binding, resolved_binding); }
    resolved_binding = current_binding;
    const auto size = std::pair {display->width, display->height};
    if (dimensions) { EXPECT_EQ(size, *dimensions); }
    dimensions = size;
    const auto deadline = start + std::chrono::seconds(5);
    int frames = 0;
    bool cursor = false;
    std::shared_ptr<platf::img_t> image = display->alloc_img();
    ASSERT_NE(image, nullptr);
    const auto result = display->capture([&](std::shared_ptr<platf::img_t> frame, bool captured) {
      if (std::chrono::steady_clock::now() >= deadline) return false;
      if (!captured) return frames < 30;
      auto *descriptor = dynamic_cast<egl::img_descriptor_t *>(frame.get());
      EXPECT_NE(descriptor, nullptr);
      if (!descriptor) return false;
      EXPECT_GE(descriptor->sd.fds[0], 0);
      EXPECT_NE(fcntl(descriptor->sd.fds[0], F_GETFD), -1);
      ++frames;
      return frames < 30;
    }, [&](std::shared_ptr<platf::img_t> &target) { if (std::chrono::steady_clock::now() >= deadline) return false; target = image; return true; }, &cursor);
    EXPECT_EQ(result, platf::capture_e::ok);
    EXPECT_EQ(frames, 30);
    image.reset();
    display.reset();
    std::cout << "KMS_CAPTURE_RESULT " << name << " frames=" << frames
      << " binding=" << *resolved_binding << " elapsed_ms=" << std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count() << '\n';
  }
}
