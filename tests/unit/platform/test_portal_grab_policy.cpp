/**
 * @file tests/unit/platform/test_portal_grab_policy.cpp
 * @brief Test XDG Desktop Portal and PipeWire capture policy.
 */

#include "../../tests_common.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

#include <drm_fourcc.h>
#include <spa/param/video/raw.h>

#include "src/platform/common.h"
#include "src/platform/linux/pipewire_capture.h"
#include "src/platform/linux/portal_session.h"

namespace portal {
  std::uint32_t portal_pick_cursor_mode_for_tests(std::uint32_t available);
}

TEST(PortalGrabPolicyTests, DesktopDisplayRequestsMonitorSource) {
  EXPECT_EQ(portal::capture_type_for_stream_display(false, false), 1u);
  EXPECT_EQ(portal::capture_type_for_stream_display(false, false, "desktop_display"), 1u);
}

TEST(PortalGrabPolicyTests, DongleRequestsMonitorSourceDespiteHeadlessFlag) {
  // headless_dongle uses headless_mode for topology privacy, not window capture.
  EXPECT_EQ(portal::capture_type_for_stream_display(true, false, "headless_dongle"), 1u);
}

TEST(PortalGrabPolicyTests, PrivateAndWindowedCagePathsRequestWindowSource) {
  EXPECT_EQ(portal::capture_type_for_stream_display(true, true), 2u);
  EXPECT_EQ(portal::capture_type_for_stream_display(false, true), 2u);
  EXPECT_EQ(portal::capture_type_for_stream_display(true, false, "gamescope_stream"), 2u);
}

// XDG ScreenCast AvailableCursorModes bits: 1=Hidden, 2=Embedded, 4=Metadata.
TEST(PortalGrabPolicyTests, CursorModePrefersEmbeddedThenMetadataThenHidden) {
  EXPECT_EQ(portal::portal_pick_cursor_mode_for_tests(0), 0u);
  EXPECT_EQ(portal::portal_pick_cursor_mode_for_tests(1), 1u);  // Hidden only
  EXPECT_EQ(portal::portal_pick_cursor_mode_for_tests(2), 2u);  // Embedded
  EXPECT_EQ(portal::portal_pick_cursor_mode_for_tests(4), 4u);  // Metadata
  EXPECT_EQ(portal::portal_pick_cursor_mode_for_tests(7), 2u);  // all → Embedded
  EXPECT_EQ(portal::portal_pick_cursor_mode_for_tests(5), 4u);  // Hidden|Metadata → Metadata
  EXPECT_EQ(portal::portal_pick_cursor_mode_for_tests(3), 2u);  // Hidden|Embedded → Embedded
}

TEST(PortalGrabPolicyTests, SelectSourcesInvalidatesRestoreTokenOnFailure) {
  // Source-level contract (S2: D-Bus lives in portal_session.cpp): failed
  // SelectSources must clear portal_restore_token and retry once without
  // restore_token (never permanently disable tokens).
  const auto path = std::filesystem::path(POLARIS_SOURCE_DIR) / "src/platform/linux/portal_session.cpp";
  std::ifstream in(path);
  ASSERT_TRUE(in.good());
  std::ostringstream out;
  out << in.rdbuf();
  const auto body = out.str();
  EXPECT_NE(body.find("clear_restore_token()"), std::string::npos);
  EXPECT_NE(body.find("retry once without restore_token"), std::string::npos);
  EXPECT_NE(body.find("save_restore_token("), std::string::npos);
  EXPECT_NE(body.find("portal_wait_cursor_modes("), std::string::npos);
  // Do not permanently disable restore tokens as a "fix".
  EXPECT_EQ(body.find("restore_token_disabled"), std::string::npos);
}

TEST(PortalGrabPolicyTests, EnsureGlobalCaptureLockContractAndUniqueTokens) {
  // S4: single media_cache_t + g_media_mu (no dual-mutex). Negotiation waits
  // outside the lock so release_global_capture can progress. Session/token
  // hygiene lives in portal_session.cpp (S2).
  const auto grab_path = std::filesystem::path(POLARIS_SOURCE_DIR) / "src/platform/linux/portal_grab.cpp";
  std::ifstream grab_in(grab_path);
  ASSERT_TRUE(grab_in.good());
  std::ostringstream grab_out;
  grab_out << grab_in.rdbuf();
  const auto grab = grab_out.str();

  EXPECT_NE(grab.find("struct media_cache_t"), std::string::npos);
  EXPECT_NE(grab.find("g_media_mu"), std::string::npos);
  EXPECT_EQ(grab.find("g_portal_mu"), std::string::npos);
  EXPECT_EQ(grab.find("g_capture_mtx"), std::string::npos);
  EXPECT_NE(grab.find("ensure_session_unlocked()"), std::string::npos);
  EXPECT_NE(grab.find("Wait outside g_media_mu"), std::string::npos);
  EXPECT_NE(grab.find("pipewire_capture::capture_t"), std::string::npos);
  EXPECT_NE(grab.find("polaris-gamescope-force"), std::string::npos);
  // Hybrid guards: exclusive PQ when force ∧ dynamicRange>0; exclusive 8-bit when SDR.
  EXPECT_NE(grab.find("portal_prefer_hdr_formats"), std::string::npos);
  EXPECT_NE(grab.find("portal_prefer_sdr_formats"), std::string::npos);
  EXPECT_NE(grab.find("client_dynamic_range"), std::string::npos);
  EXPECT_NE(grab.find("portal_force_hdr_enabled()"), std::string::npos);
  EXPECT_NE(grab.find("gamescope_stream"), std::string::npos);
  EXPECT_NE(grab.find("prefer_sdr_formats"), std::string::npos);

  // Self-deadlock guard: ensure_global_capture must not call locking
  // ensure_global_session() under g_media_mu (only ensure_session_unlocked).
  const auto fn_start = grab.find("static std::shared_ptr<pipewire_capture::capture_t> ensure_global_capture(");
  ASSERT_NE(fn_start, std::string::npos);
  const auto fn_end = grab.find("class portal_display_t", fn_start);
  ASSERT_NE(fn_end, std::string::npos);
  const auto fn = grab.substr(fn_start, fn_end - fn_start);
  EXPECT_NE(fn.find("ensure_session_unlocked()"), std::string::npos);
  EXPECT_NE(fn.find("Wait outside g_media_mu"), std::string::npos);
  auto stripped = fn;
  for (;;) {
    const auto p = stripped.find("ensure_session_unlocked");
    if (p == std::string::npos) {
      break;
    }
    stripped.replace(p, sizeof("ensure_session_unlocked") - 1, "UNLOCKED_OK");
  }
  EXPECT_EQ(stripped.find("ensure_global_session("), std::string::npos)
    << "ensure_global_capture must not call locking ensure_global_session() under g_media_mu";

  const auto session_path = std::filesystem::path(POLARIS_SOURCE_DIR) / "src/platform/linux/portal_session.cpp";
  std::ifstream session_in(session_path);
  ASSERT_TRUE(session_in.good());
  std::ostringstream session_out;
  session_out << session_in.rdbuf();
  const auto session = session_out.str();
  EXPECT_NE(session.find("next_handle_token("), std::string::npos);
  EXPECT_NE(session.find("Start timeout/failure"), std::string::npos);
  EXPECT_NE(session.find("no Start retry"), std::string::npos);
  EXPECT_NE(session.find("portal_restore_token_host.txt"), std::string::npos);
  EXPECT_NE(session.find("portal_restore_token_private.txt"), std::string::npos);
  EXPECT_NE(session.find("POLARIS_PORTAL_DBUS_ADDRESS"), std::string::npos);
}

TEST(PortalGrabPolicyTests, HeadlessDongleNormalizeForcesKmsCapture) {
  const auto path = std::filesystem::path(POLARIS_SOURCE_DIR) / "src/platform/linux/stream_display_policy.cpp";
  std::ifstream in(path);
  ASSERT_TRUE(in.good());
  std::ostringstream out;
  out << in.rdbuf();
  const auto body = out.str();
  EXPECT_NE(body.find("normalize_config_from_load"), std::string::npos);
  EXPECT_NE(body.find("k_headless_dongle"), std::string::npos);
  // Dongle defaults to portal (host ScreenCast); explicit kms still allowed.
  EXPECT_NE(body.find("capture = \"portal\""), std::string::npos);
}

TEST(PortalGrabPolicyTests, DonglePrivacyBootstrapKeepsDeskWithoutHostToken) {
  // Source contract: atomic enable+disable is forbidden; privacy blank only when
  // portal_restore_token_host.txt exists (or capture is non-portal).
  const auto path = std::filesystem::path(POLARIS_SOURCE_DIR) / "src/platform/linux/display_topology.cpp";
  std::ifstream in(path);
  ASSERT_TRUE(in.good());
  std::ostringstream out;
  out << in.rdbuf();
  const auto body = out.str();
  EXPECT_NE(body.find("host_portal_restore_token_present"), std::string::npos);
  EXPECT_NE(body.find("bootstrap"), std::string::npos);
  EXPECT_NE(body.find("portal_restore_token_host.txt"), std::string::npos);
  EXPECT_NE(body.find("QT_QPA_PLATFORM=wayland"), std::string::npos);
  // Staged enable before disable — not a single atomic enable+disable command.
  EXPECT_NE(body.find("enable streaming output"), std::string::npos);
}

TEST(PipeWireCapturePolicyTests, MapsSupportedSpaFormatsToDrmFormats) {
  EXPECT_EQ(pipewire_capture::drm_format_for_spa(SPA_VIDEO_FORMAT_BGRx), DRM_FORMAT_XRGB8888);
  EXPECT_EQ(pipewire_capture::drm_format_for_spa(SPA_VIDEO_FORMAT_BGRA), DRM_FORMAT_ARGB8888);
  EXPECT_EQ(pipewire_capture::drm_format_for_spa(SPA_VIDEO_FORMAT_RGBx), DRM_FORMAT_XBGR8888);
  EXPECT_EQ(pipewire_capture::drm_format_for_spa(SPA_VIDEO_FORMAT_RGBA), DRM_FORMAT_ABGR8888);
}

TEST(PipeWireCapturePolicyTests, RejectsUnsupportedSpaFormats) {
  EXPECT_EQ(pipewire_capture::drm_format_for_spa(SPA_VIDEO_FORMAT_NV12), std::nullopt);
}

TEST(PipeWireCapturePolicyTests, CopiesPaddedRowsAndLeavesDestinationPaddingUntouched) {
  std::array<std::uint8_t, 20> source {
    10, 20, 30, 40,
    50, 60, 70, 80,
    0xAA, 0xAA, 0xAA, 0xAA,
    90, 100, 110, 120,
    130, 140, 150, 160,
  };
  std::vector<std::uint8_t> destination(24, 0xEE);

  const auto result = pipewire_capture::copy_memptr_frame_to_bgra(
    source.data(), source.size(), 0, source.size(), 2, 2, 12, SPA_VIDEO_FORMAT_BGRx, destination.data(), 12);

  ASSERT_TRUE(result);
  EXPECT_EQ(destination, (std::vector<std::uint8_t> {
    10, 20, 30, 40,
    50, 60, 70, 80,
    0xEE, 0xEE, 0xEE, 0xEE,
    90, 100, 110, 120,
    130, 140, 150, 160,
    0xEE, 0xEE, 0xEE, 0xEE,
  }));
}

TEST(PipeWireCapturePolicyTests, RejectsInsufficientSourcePayload) {
  std::array<std::uint8_t, 15> source {};
  std::array<std::uint8_t, 16> destination {};

  EXPECT_FALSE(pipewire_capture::copy_memptr_frame_to_bgra(
    source.data(), source.size(), 0, source.size(), 2, 2, 8, SPA_VIDEO_FORMAT_BGRx, destination.data(), 8));
}

TEST(PipeWireCapturePolicyTests, AppliesChunkOffsetBeforeCopying) {
  std::array<std::uint8_t, 12> source {
    0xCC, 0xCC, 0xCC, 0xCC,
    1, 2, 3, 4,
    5, 6, 7, 8,
  };
  std::array<std::uint8_t, 8> destination {};

  const auto result = pipewire_capture::copy_memptr_frame_to_bgra(
    source.data(), source.size(), 4, 8, 2, 1, 8, SPA_VIDEO_FORMAT_BGRx, destination.data(), 8);

  ASSERT_TRUE(result);
  EXPECT_EQ(destination, (std::array<std::uint8_t, 8> {1, 2, 3, 4, 5, 6, 7, 8}));
}

TEST(PipeWireCapturePolicyTests, BgrFormatsPreserveByteOrder) {
  const std::array<std::uint8_t, 8> source {1, 2, 3, 4, 5, 6, 7, 8};
  for (const auto format : {SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA}) {
    std::array<std::uint8_t, 8> destination {};
    ASSERT_TRUE(pipewire_capture::copy_memptr_frame_to_bgra(
      source.data(), source.size(), 0, source.size(), 2, 1, 8, format, destination.data(), 8));
    EXPECT_EQ(destination, source);
  }
}

TEST(PipeWireCapturePolicyTests, RgbFormatsSwapRedAndBlueToBgraByteOrder) {
  std::array<std::uint8_t, 8> source {
    10, 20, 30, 40,
    50, 60, 70, 80,
  };
  for (const auto format : {SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA}) {
    std::array<std::uint8_t, 8> destination {};
    ASSERT_TRUE(pipewire_capture::copy_memptr_frame_to_bgra(
      source.data(), source.size(), 0, source.size(), 2, 1, 8, format, destination.data(), 8));
    EXPECT_EQ(destination, (std::array<std::uint8_t, 8> {
      30, 20, 10, 40,
      70, 60, 50, 80,
    }));
  }
}

TEST(PipeWireCapturePolicyTests, RejectsNonPositiveStride) {
  std::array<std::uint8_t, 8> source {};
  std::array<std::uint8_t, 8> destination {};

  EXPECT_FALSE(pipewire_capture::copy_memptr_frame_to_bgra(
    source.data(), source.size(), 0, source.size(), 2, 1, 0, SPA_VIDEO_FORMAT_BGRx, destination.data(), 8));
  EXPECT_FALSE(pipewire_capture::copy_memptr_frame_to_bgra(
    source.data(), source.size(), 0, source.size(), 2, 1, -8, SPA_VIDEO_FORMAT_BGRx, destination.data(), 8));
}

TEST(PipeWireCapturePolicyTests, CpuFramesReportSharedMemoryMetadata) {
  const auto metadata = pipewire_capture::cpu_frame_metadata();

  EXPECT_EQ(metadata.transport, platf::frame_transport_e::shm);
  EXPECT_EQ(metadata.residency, platf::frame_residency_e::cpu);
  EXPECT_EQ(metadata.format, platf::frame_format_e::bgra8);
  EXPECT_TRUE(metadata.device.empty());
}

TEST(PipeWireCapturePolicyTests, DmaBufFramesReportGpuAndRenderNodeMetadata) {
  const auto metadata = pipewire_capture::dmabuf_frame_metadata("/dev/dri/renderD128");

  EXPECT_EQ(metadata.transport, platf::frame_transport_e::dmabuf);
  EXPECT_EQ(metadata.residency, platf::frame_residency_e::gpu);
  EXPECT_EQ(metadata.format, platf::frame_format_e::bgra8);
  EXPECT_EQ(metadata.device, "/dev/dri/renderD128");
}

TEST(PipeWireCapturePolicyTests, CpuCopyWarningMatchesActualFrameResidency) {
  EXPECT_TRUE(pipewire_capture::frame_requires_cpu_copy(pipewire_capture::cpu_frame_metadata()));
  EXPECT_FALSE(pipewire_capture::frame_requires_cpu_copy(
    pipewire_capture::dmabuf_frame_metadata("/dev/dri/renderD128")));

  auto inconsistent = pipewire_capture::dmabuf_frame_metadata("/dev/dri/renderD128");
  inconsistent.residency = platf::frame_residency_e::cpu;
  EXPECT_TRUE(pipewire_capture::frame_requires_cpu_copy(inconsistent));
}

TEST(PipeWireCapturePolicyTests, RenderNodeValidationAcceptsOnlyCanonicalRenderNodes) {
  EXPECT_EQ(pipewire_capture::canonical_render_node("/dev/dri/renderD128"), "/dev/dri/renderD128");
  EXPECT_EQ(pipewire_capture::canonical_render_node("/dev/dri/renderD0"), "/dev/dri/renderD0");

  EXPECT_EQ(pipewire_capture::canonical_render_node(" /dev/dri/renderD128"), std::nullopt);
  EXPECT_EQ(pipewire_capture::canonical_render_node("/dev/dri/card0"), std::nullopt);
  EXPECT_EQ(pipewire_capture::canonical_render_node("/dev/dri/renderD128/../renderD129"), std::nullopt);
  EXPECT_EQ(pipewire_capture::canonical_render_node("/dev/dri/renderD"), std::nullopt);
  EXPECT_EQ(pipewire_capture::canonical_render_node("/dev/dri/renderDabc"), std::nullopt);
}

TEST(PipeWireCapturePolicyTests, SameRenderNodeEligibilityRequiresExplicitMatchingGpuPathAndEglSupport) {
  const pipewire_capture::dmabuf_eligibility_t eligible {
    .capture_render_node = "/dev/dri/renderD128",
    .encoder_render_node = "/dev/dri/renderD128",
    .mem_type = platf::mem_type_e::vaapi,
    .egl_import_supported = true,
  };

  EXPECT_TRUE(pipewire_capture::may_offer_dmabuf(eligible));

  auto missing_capture = eligible;
  missing_capture.capture_render_node.reset();
  EXPECT_FALSE(pipewire_capture::may_offer_dmabuf(missing_capture));

  auto mismatched = eligible;
  mismatched.encoder_render_node = "/dev/dri/renderD129";
  EXPECT_FALSE(pipewire_capture::may_offer_dmabuf(mismatched));

  auto system_memory = eligible;
  system_memory.mem_type = platf::mem_type_e::system;
  EXPECT_FALSE(pipewire_capture::may_offer_dmabuf(system_memory));

  auto no_egl = eligible;
  no_egl.egl_import_supported = false;
  EXPECT_FALSE(pipewire_capture::may_offer_dmabuf(no_egl));

  auto noncanonical = eligible;
  noncanonical.capture_render_node = "renderD128";
  noncanonical.encoder_render_node = "renderD128";
  EXPECT_FALSE(pipewire_capture::may_offer_dmabuf(noncanonical));
}

TEST(PipeWireCapturePolicyTests, DmaBufCapabilityFilteringKeepsOnlyPackedRgbImportableNonExternalFormats) {
  const std::vector<pipewire_capture::dmabuf_format_modifier_t> portal_formats {
    {.spa_format = SPA_VIDEO_FORMAT_BGRx, .drm_fourcc = DRM_FORMAT_XRGB8888, .modifier = DRM_FORMAT_MOD_LINEAR},
    {.spa_format = SPA_VIDEO_FORMAT_BGRA, .drm_fourcc = DRM_FORMAT_ARGB8888, .modifier = 0x0100000000000002ULL},
    {.spa_format = SPA_VIDEO_FORMAT_NV12, .drm_fourcc = DRM_FORMAT_NV12, .modifier = DRM_FORMAT_MOD_LINEAR},
    {.spa_format = SPA_VIDEO_FORMAT_RGBx, .drm_fourcc = DRM_FORMAT_XBGR8888, .modifier = 0x0100000000000003ULL},
  };
  const std::vector<pipewire_capture::egl_dmabuf_format_t> egl_formats {
    {.drm_fourcc = DRM_FORMAT_XRGB8888, .modifiers = {DRM_FORMAT_MOD_LINEAR}},
    {.drm_fourcc = DRM_FORMAT_ARGB8888, .modifiers = {0x0100000000000002ULL}, .external_only_modifiers = {0x0100000000000002ULL}},
    {.drm_fourcc = DRM_FORMAT_NV12, .modifiers = {DRM_FORMAT_MOD_LINEAR}},
  };

  const auto filtered = pipewire_capture::filter_importable_dmabuf_formats(portal_formats, egl_formats);

  ASSERT_EQ(filtered.size(), 1u);
  EXPECT_EQ(filtered[0].spa_format, SPA_VIDEO_FORMAT_BGRx);
  EXPECT_EQ(filtered[0].drm_fourcc, DRM_FORMAT_XRGB8888);
  EXPECT_EQ(filtered[0].modifier, DRM_FORMAT_MOD_LINEAR);
}

TEST(PipeWireCapturePolicyTests, DmaBufPlaneDescriptorValidationRequiresOneCompletePackedRgbPlane) {
  pipewire_capture::dmabuf_frame_t frame {
    .width = 640,
    .height = 480,
    .spa_format = SPA_VIDEO_FORMAT_BGRx,
    .drm_fourcc = DRM_FORMAT_XRGB8888,
    .modifier = DRM_FORMAT_MOD_LINEAR,
    .planes = {{
      {.fd = 3, .chunk_offset = 0, .chunk_size = 640u * 480u * 4u, .stride = 640 * 4, .maxsize = 640u * 480u * 4u},
    }},
    .plane_count = 1,
  };

  EXPECT_TRUE(pipewire_capture::valid_dmabuf_frame(frame));

  auto no_planes = frame;
  no_planes.plane_count = 0;
  EXPECT_FALSE(pipewire_capture::valid_dmabuf_frame(no_planes));

  auto multiple_planes = frame;
  multiple_planes.plane_count = 2;
  multiple_planes.planes[1] = multiple_planes.planes[0];
  EXPECT_FALSE(pipewire_capture::valid_dmabuf_frame(multiple_planes));

  auto bad_fd = frame;
  bad_fd.planes[0].fd = -1;
  EXPECT_FALSE(pipewire_capture::valid_dmabuf_frame(bad_fd));

  auto bad_stride = frame;
  bad_stride.planes[0].stride = 0;
  EXPECT_FALSE(pipewire_capture::valid_dmabuf_frame(bad_stride));

  auto short_payload = frame;
  short_payload.planes[0].chunk_size = 64;
  EXPECT_FALSE(pipewire_capture::valid_dmabuf_frame(short_payload));

  auto wrapped_chunk = frame;
  wrapped_chunk.planes[0].chunk_offset = wrapped_chunk.planes[0].maxsize;
  EXPECT_TRUE(pipewire_capture::valid_dmabuf_frame(wrapped_chunk));

  auto bad_chunk = frame;
  bad_chunk.planes[0].chunk_size = bad_chunk.planes[0].maxsize + 1;
  EXPECT_FALSE(pipewire_capture::valid_dmabuf_frame(bad_chunk));

  auto zero_maxsize = frame;
  zero_maxsize.planes[0].maxsize = 0;
  EXPECT_FALSE(pipewire_capture::valid_dmabuf_frame(zero_maxsize));
}

TEST(PipeWireCapturePolicyTests, DmaBufDescriptorDuplicatesFdAndNormalizesChunkOffset) {
  int pipe_fds[2] {-1, -1};
  ASSERT_EQ(pipe(pipe_fds), 0);

  pipewire_capture::dmabuf_frame_t frame {
    .width = 4,
    .height = 2,
    .spa_format = SPA_VIDEO_FORMAT_BGRx,
    .drm_fourcc = DRM_FORMAT_XRGB8888,
    .modifier = DRM_FORMAT_MOD_LINEAR,
    .planes = {{
      {.fd = pipe_fds[0], .chunk_offset = 128, .chunk_size = 32, .stride = 16, .maxsize = 128},
    }},
    .plane_count = 1,
  };
  egl::img_descriptor_t descriptor;
  std::fill_n(descriptor.sd.fds, 4, -1);

  ASSERT_TRUE(pipewire_capture::fill_dmabuf_descriptor(frame, descriptor));
  EXPECT_GE(descriptor.sd.fds[0], 0);
  EXPECT_NE(descriptor.sd.fds[0], pipe_fds[0]);
  EXPECT_EQ(descriptor.sd.offsets[0], 0u);
  EXPECT_EQ(descriptor.sd.pitches[0], 16u);
  EXPECT_EQ(descriptor.sd.fourcc, DRM_FORMAT_XRGB8888);

  close(pipe_fds[0]);
  close(pipe_fds[1]);
}

TEST(PipeWireCapturePolicyTests, BufferDataTypePolicyMatchesNegotiatedEncoderTransport) {
  EXPECT_EQ(pipewire_capture::offered_buffer_data_types(true),
            (std::vector<std::uint32_t> {SPA_DATA_DmaBuf}));
  EXPECT_EQ(pipewire_capture::offered_buffer_data_types(false),
            (std::vector<std::uint32_t> {SPA_DATA_MemFd, SPA_DATA_MemPtr}));
}

TEST(PipeWireCapturePolicyTests, ResolveCaptureRenderNodePrefersPortalValue) {
  const auto resolved = pipewire_capture::resolve_capture_render_node(
    std::optional<std::string> {"/dev/dri/renderD128"},
    std::optional<std::string> {"/dev/dri/renderD129"});
  ASSERT_TRUE(resolved);
  EXPECT_EQ(*resolved, "/dev/dri/renderD128");
}

TEST(PipeWireCapturePolicyTests, ResolveCaptureRenderNodeFallsBackToEncoderAdapter) {
  const auto resolved = pipewire_capture::resolve_capture_render_node(
    std::nullopt, std::optional<std::string> {"/dev/dri/renderD128"});
  ASSERT_TRUE(resolved);
  EXPECT_EQ(*resolved, "/dev/dri/renderD128");
}

TEST(PipeWireCapturePolicyTests, ResolveCaptureRenderNodeRejectsNonRenderPaths) {
  EXPECT_FALSE(pipewire_capture::resolve_capture_render_node(
    std::optional<std::string> {"/dev/dri/card1"},
    std::nullopt));
  const auto resolved = pipewire_capture::resolve_capture_render_node(
    std::optional<std::string> {"/dev/dri/card1"},
    std::optional<std::string> {"/dev/dri/renderD128"});
  ASSERT_TRUE(resolved);
  EXPECT_EQ(*resolved, "/dev/dri/renderD128");
}
