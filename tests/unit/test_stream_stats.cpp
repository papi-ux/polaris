/**
 * @file tests/unit/test_stream_stats.cpp
 * @brief Test src/stream_stats.*.
 */

#include <src/stream_stats.h>
#include <src/config.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <filesystem>
#include <string>
#ifdef __linux__
  #include <unistd.h>
#endif

namespace {
  struct LinuxDisplayConfigGuard {
    LinuxDisplayConfigGuard():
        adapter_name {config::video.adapter_name},
        encoder {config::video.encoder},
        headless_mode {config::video.linux_display.headless_mode},
        use_cage_compositor {config::video.linux_display.use_cage_compositor},
        prefer_gpu_native_capture {config::video.linux_display.prefer_gpu_native_capture} {
    }

    ~LinuxDisplayConfigGuard() {
      config::video.adapter_name = adapter_name;
      config::video.encoder = encoder;
      config::video.linux_display.headless_mode = headless_mode;
      config::video.linux_display.use_cage_compositor = use_cage_compositor;
      config::video.linux_display.prefer_gpu_native_capture = prefer_gpu_native_capture;
    }

    std::string adapter_name;
    std::string encoder;
    bool headless_mode;
    bool use_cage_compositor;
    bool prefer_gpu_native_capture;
  };
}  // namespace

TEST(StreamStatsCapturePathTests, UnknownWhenNoPathMetadataExists) {
  stream_stats::stats_t stats {};

  EXPECT_EQ(stream_stats::capture_path_summary(stats), "unknown");
  EXPECT_EQ(stream_stats::capture_path_reason(stats), "no_capture_metadata");
  EXPECT_FALSE(stream_stats::capture_path_uses_cpu_copy(stats));
  EXPECT_FALSE(stream_stats::capture_path_is_gpu_native(stats));
}

TEST(StreamStatsCapturePathTests, DetectsShmCpuCapture) {
  LinuxDisplayConfigGuard guard;
  config::video.linux_display.use_cage_compositor = true;
  config::video.linux_display.prefer_gpu_native_capture = false;

  stream_stats::stats_t stats {};
  stats.runtime_effective_headless = true;
  stats.capture_transport = platf::frame_transport_e::shm;
  stats.capture_residency = platf::frame_residency_e::cpu;
  stats.encode_target_residency = platf::frame_residency_e::cpu;

  EXPECT_EQ(stream_stats::capture_path_summary(stats), "shm_cpu_capture");
  EXPECT_EQ(stream_stats::capture_path_reason(stats), "headless_shm_fallback");
  EXPECT_TRUE(stream_stats::capture_path_uses_cpu_copy(stats));
  EXPECT_FALSE(stream_stats::capture_path_is_gpu_native(stats));
  EXPECT_EQ(
    stream_stats::capture_path_reason_message(stream_stats::capture_path_reason(stats)),
    "Private Stream is using the conservative SHM/system-memory path; the stream can be healthy, but capable high-FPS hosts should use a GPU-native path when available."
  );
}

TEST(StreamStatsCapturePathTests, SerializesCaptureDecisionDiagnostics) {
  LinuxDisplayConfigGuard guard;
  config::video.linux_display.use_cage_compositor = true;

  stream_stats::stats_t stats {};
  stats.runtime_backend = "labwc";
  stats.runtime_requested_headless = true;
  stats.runtime_effective_headless = true;
  stats.capture_transport = platf::frame_transport_e::shm;
  stats.capture_residency = platf::frame_residency_e::cpu;
  stats.capture_format = platf::frame_format_e::bgra8;
  stats.encode_target_residency = platf::frame_residency_e::cpu;

  const auto json = nlohmann::json::parse(stats.to_json());

  EXPECT_EQ(json.at("capture_path"), "shm_cpu_capture");
  EXPECT_EQ(json.at("capture_path_reason"), "headless_shm_fallback");
  EXPECT_FALSE(json.at("capture_path_reason_message").get<std::string>().empty());
  ASSERT_TRUE(json.contains("capture_decision"));
  const auto &decision = json.at("capture_decision");
  EXPECT_EQ(decision.at("path"), json.at("capture_path"));
  EXPECT_EQ(decision.at("reason"), json.at("capture_path_reason"));
  EXPECT_EQ(decision.at("reason_message"), json.at("capture_path_reason_message"));
  EXPECT_EQ(decision.at("transport"), "shm");
  EXPECT_EQ(decision.at("residency"), "cpu");
  EXPECT_EQ(decision.at("format"), "bgra8");
  EXPECT_TRUE(decision.at("cpu_copy"));
  EXPECT_FALSE(decision.at("gpu_native"));
  EXPECT_EQ(decision.at("runtime_backend"), "labwc");
  EXPECT_TRUE(decision.at("requested_headless"));
  EXPECT_TRUE(decision.at("effective_headless"));
  EXPECT_FALSE(decision.at("gpu_native_override_active"));
}

TEST(StreamStatsLinuxGpuProfileTests, WarnsWhenNvidiaTrueHeadlessDisablesGpuNativeCapture) {
  LinuxDisplayConfigGuard guard;
  config::video.encoder = "nvenc";
  config::video.linux_display.headless_mode = true;
  config::video.linux_display.use_cage_compositor = true;
  config::video.linux_display.prefer_gpu_native_capture = false;

  stream_stats::stats_t stats {};
  stats.runtime_backend = "labwc";
  stats.runtime_requested_headless = true;
  stats.runtime_effective_headless = true;

  const auto json = nlohmann::json::parse(stats.to_json());
  const auto &profile = json.at("linux_gpu_profile");

  ASSERT_TRUE(profile.contains("configuration_warnings"));
  const auto &warnings = profile.at("configuration_warnings");
  ASSERT_EQ(warnings.size(), 1);
  EXPECT_EQ(warnings.at(0).at("id"), "nvidia_headless_gpu_native_disabled");
  EXPECT_EQ(warnings.at(0).at("severity"), "warning");
  EXPECT_NE(
    warnings.at(0).at("message").get<std::string>().find("503"),
    std::string::npos
  );
  EXPECT_NE(
    warnings.at(0).at("action").get<std::string>().find("linux_prefer_gpu_native_capture = enabled"),
    std::string::npos
  );
}

TEST(StreamStatsLinuxGpuProfileTests, DoesNotCallMissingCaptureDeviceAnAdapterMatch) {
  LinuxDisplayConfigGuard guard;
  config::video.adapter_name = "/dev/dri/renderD129";

  stream_stats::stats_t stats {};
  stats.capture_transport = platf::frame_transport_e::shm;
  stats.capture_residency = platf::frame_residency_e::cpu;
  stats.encode_target_device = "vaapi";
  stats.encode_target_residency = platf::frame_residency_e::gpu;

  const auto profile = stream_stats::linux_gpu_profile_json(stats);

  EXPECT_TRUE(profile.at("adapter_matches_capture_device").is_null());
  ASSERT_TRUE(profile.contains("adapter_pairing_status"));
  EXPECT_EQ(profile.at("adapter_pairing_status"), "unknown");
  ASSERT_TRUE(profile.contains("adapter_pairing_device_source"));
  EXPECT_EQ(profile.at("adapter_pairing_device_source"), "none");
}

TEST(StreamStatsLinuxGpuProfileTests, KeepsIdenticalUnresolvableDevicePairingUnknown) {
  LinuxDisplayConfigGuard guard;
  config::video.adapter_name = "/dev/dri/polaris-missing-shared-node";

  stream_stats::stats_t stats {};
  stats.wayland_main_device = "/dev/dri/polaris-missing-shared-node";

  const auto profile = stream_stats::linux_gpu_profile_json(stats);

  EXPECT_TRUE(profile.at("adapter_matches_wayland_main_device").is_null());
  EXPECT_EQ(profile.at("adapter_pairing_status"), "unknown");
  EXPECT_TRUE(profile.at("configuration_warnings").empty());
}

TEST(StreamStatsLinuxGpuProfileTests, KeepsUnresolvableDevicePairingUnknown) {
  LinuxDisplayConfigGuard guard;
  config::video.adapter_name = "/dev/dri/polaris-missing-encoder-node";

  stream_stats::stats_t stats {};
  stats.wayland_main_device = "/dev/dri/polaris-missing-wayland-node";

  const auto profile = stream_stats::linux_gpu_profile_json(stats);

  EXPECT_TRUE(profile.at("adapter_matches_wayland_main_device").is_null());
  EXPECT_EQ(profile.at("adapter_pairing_status"), "unknown");
  EXPECT_TRUE(profile.at("configuration_warnings").empty());
}

TEST(StreamStatsLinuxGpuProfileTests, UsesWaylandMainDeviceWhenCaptureFrameDeviceIsUnavailable) {
  LinuxDisplayConfigGuard guard;
  config::video.adapter_name = "/dev/null";

  stream_stats::stats_t stats {};
  stats.capture_transport = platf::frame_transport_e::shm;
  stats.capture_residency = platf::frame_residency_e::cpu;
  stats.wayland_main_device = "/dev/zero";
  stats.encode_target_device = "vaapi";
  stats.encode_target_residency = platf::frame_residency_e::gpu;

  const auto profile = stream_stats::linux_gpu_profile_json(stats);

  EXPECT_TRUE(profile.at("adapter_matches_capture_device").is_null());
  EXPECT_FALSE(profile.at("adapter_matches_wayland_main_device"));
  EXPECT_EQ(profile.at("adapter_pairing_status"), "mismatched");
  EXPECT_EQ(profile.at("adapter_pairing_device"), "/dev/zero");
  EXPECT_EQ(profile.at("adapter_pairing_device_source"), "wayland_main_device");

  const auto &warnings = profile.at("configuration_warnings");
  const auto mismatch = std::find_if(warnings.begin(), warnings.end(), [](const auto &warning) {
    return warning.value("id", std::string {}) == "linux_gpu_adapter_mismatch";
  });
  ASSERT_NE(mismatch, warnings.end());
  EXPECT_NE(mismatch->at("message").get<std::string>().find("/dev/null"), std::string::npos);
  EXPECT_NE(mismatch->at("message").get<std::string>().find("/dev/zero"), std::string::npos);
}

#ifdef __linux__
TEST(StreamStatsLinuxGpuProfileTests, TreatsSymlinkedAdapterAsTheSameDeviceNode) {
  LinuxDisplayConfigGuard guard;
  const auto link_path = std::filesystem::temp_directory_path() /
    ("polaris-stream-stats-device-link-" + std::to_string(::getpid()));
  std::error_code ec;
  std::filesystem::remove(link_path, ec);
  ec.clear();
  std::filesystem::create_symlink("/dev/null", link_path, ec);
  ASSERT_FALSE(ec);

  config::video.adapter_name = "/dev/null";
  stream_stats::stats_t stats {};
  stats.wayland_main_device = link_path.string();

  const auto profile = stream_stats::linux_gpu_profile_json(stats);
  EXPECT_EQ(profile.at("adapter_pairing_status"), "matched");
  EXPECT_TRUE(profile.at("adapter_matches_wayland_main_device"));

  std::filesystem::remove(link_path, ec);
}
#endif

TEST(StreamStatsControllerInputTests, SerializesNativeControllerDiagnostics) {
  stream_stats::stats_t stats {};
  stats.input_virtual_controller_created = true;
  stats.input_virtual_controller_number = 2;
  stats.input_virtual_controller_kind = "xone";
  stats.input_virtual_controller_error = "";
  stats.input_host_controller_isolation = "strict_bwrap";
  stats.input_host_controller_isolation_detail = "2 virtual nodes allowed; host pads masked";
  stats.input_haptics_supported = true;
  stats.input_haptics_detail = "rumble callbacks registered for client pad 2";

  const auto json = nlohmann::json::parse(stats.to_json());

  ASSERT_TRUE(json.contains("controller_input"));
  const auto &input = json.at("controller_input");
  EXPECT_TRUE(input.at("virtual_controller_created"));
  EXPECT_EQ(input.at("virtual_controller_number"), 2);
  EXPECT_EQ(input.at("virtual_controller_kind"), "xone");
  EXPECT_EQ(input.at("host_controller_isolation"), "strict_bwrap");
  EXPECT_EQ(input.at("host_controller_isolation_detail"), "2 virtual nodes allowed; host pads masked");
  EXPECT_TRUE(input.at("haptics_supported"));
  EXPECT_EQ(input.at("haptics_detail"), "rumble callbacks registered for client pad 2");
}

TEST(StreamStatsHdrStateTests, LabelsRequestedHdrWithoutSourceAsTenBitSdr) {
  stream_stats::stats_t stats {};
  stats.dynamic_range = 1;
  stats.display_hdr = false;
  stats.hdr_metadata_available = false;
  stats.stream_hdr_enabled = false;
  stats.color_coding = "SDR (Rec. 709)";

  EXPECT_EQ(stream_stats::hdr_effective_mode(stats), "sdr_10bit");
  EXPECT_EQ(stream_stats::hdr_downgrade_reason(stats), "display_not_hdr");
  EXPECT_NE(
    stream_stats::hdr_downgrade_message(stats).find("10-bit SDR, not HDR"),
    std::string::npos
  );

  const auto json = nlohmann::json::parse(stats.to_json());
  EXPECT_EQ(json.at("hdr_effective_mode"), "sdr_10bit");
  EXPECT_EQ(json.at("hdr_downgrade_reason"), "display_not_hdr");
  EXPECT_NE(
    json.at("hdr_downgrade_message").get<std::string>().find("10-bit SDR, not HDR"),
    std::string::npos
  );
}

TEST(StreamStatsHdrStateTests, LabelsRequestedHdrOnHeadlessAsHeadlessUnavailable) {
  stream_stats::stats_t stats {};
  stats.dynamic_range = 1;
  stats.runtime_effective_headless = true;
  stats.display_hdr = false;
  stats.hdr_metadata_available = false;
  stats.stream_hdr_enabled = false;
  stats.color_coding = "SDR (Rec. 709)";

  EXPECT_EQ(stream_stats::hdr_effective_mode(stats), "sdr_10bit");
  EXPECT_EQ(stream_stats::hdr_downgrade_reason(stats), "headless_hdr_unavailable");
  EXPECT_NE(
    stream_stats::hdr_downgrade_message(stats).find("Private Stream"),
    std::string::npos
  );

  const auto json = nlohmann::json::parse(stats.to_json());
  EXPECT_EQ(json.at("hdr_effective_mode"), "sdr_10bit");
  EXPECT_EQ(json.at("hdr_downgrade_reason"), "headless_hdr_unavailable");
  EXPECT_NE(json.dump().find("physical or virtual HDR-capable display path"), std::string::npos);
}

TEST(StreamStatsHdrStateTests, LabelsTrueHdrAndPlainSdrWithoutDowngrade) {
  stream_stats::stats_t hdr_stats {};
  hdr_stats.dynamic_range = 1;
  hdr_stats.display_hdr = true;
  hdr_stats.hdr_metadata_available = true;
  hdr_stats.stream_hdr_enabled = true;

  EXPECT_EQ(stream_stats::hdr_effective_mode(hdr_stats), "hdr10");
  EXPECT_EQ(stream_stats::hdr_downgrade_reason(hdr_stats), "none");
  EXPECT_TRUE(stream_stats::hdr_downgrade_message(hdr_stats).empty());

  stream_stats::stats_t sdr_stats {};
  EXPECT_EQ(stream_stats::hdr_effective_mode(sdr_stats), "sdr_8bit");
  EXPECT_EQ(stream_stats::hdr_downgrade_reason(sdr_stats), "none");
}

TEST(StreamStatsCapturePathTests, ExplainsGpuNativeShmFallback) {
  LinuxDisplayConfigGuard guard;
  config::video.adapter_name = "/dev/dri/renderD128";
  config::video.linux_display.use_cage_compositor = true;
  config::video.linux_display.prefer_gpu_native_capture = true;

  stream_stats::stats_t stats {};
  stats.runtime_effective_headless = true;
  stats.capture_transport = platf::frame_transport_e::shm;
  stats.capture_residency = platf::frame_residency_e::cpu;
  stats.capture_format = platf::frame_format_e::bgra8;
  stats.capture_device = "/dev/dri/renderD128";
  stats.encode_target_device = "vaapi";
  stats.encode_target_residency = platf::frame_residency_e::gpu;
  stats.encode_target_format = platf::frame_format_e::nv12;

  EXPECT_EQ(stream_stats::capture_path_summary(stats), "shm_cpu_capture");
  EXPECT_EQ(stream_stats::capture_path_reason(stats), "gpu_native_requested_shm_fallback");
  EXPECT_TRUE(stream_stats::capture_path_uses_cpu_copy(stats));
  EXPECT_FALSE(stream_stats::capture_path_is_gpu_native(stats));

  const auto json = nlohmann::json::parse(stats.to_json());
  ASSERT_TRUE(json.contains("linux_gpu_profile"));
  const auto &profile = json.at("linux_gpu_profile");
  EXPECT_EQ(profile.at("encoder_api"), "vaapi");
  EXPECT_EQ(profile.at("encoder_adapter"), "/dev/dri/renderD128");
  EXPECT_EQ(profile.at("capture_device"), "/dev/dri/renderD128");
  EXPECT_TRUE(profile.at("adapter_matches_capture_device"));
  EXPECT_TRUE(profile.at("gpu_native_requested"));
  EXPECT_FALSE(profile.at("gpu_native_succeeded"));
}

TEST(StreamStatsCapturePathTests, SerializesStructuredGpuNativeProbeFailures) {
  stream_stats::stats_t stats {};
  stats.gpu_native_probe.requested = true;
  stats.gpu_native_probe.headless_extcopy.attempted = true;
  stats.gpu_native_probe.headless_extcopy.result = "failed";
  stats.gpu_native_probe.headless_extcopy.failure_stage = "capture_init";
  stats.gpu_native_probe.headless_extcopy.failure_reason = "dmabuf_capture_not_initialized";
  stats.gpu_native_probe.windowed.attempted = true;
  stats.gpu_native_probe.windowed.result = "failed";
  stats.gpu_native_probe.windowed.failure_stage = "first_frame";
  stats.gpu_native_probe.windowed.failure_reason = "no_live_dmabuf_frame";
  stats.gpu_native_probe.selected_strategy = "headless_shm";
  stats.gpu_native_probe.fallback = "headless_shm";

  const auto json = nlohmann::json::parse(stats.to_json());
  const auto &probe = json.at("gpu_native_probe");

  EXPECT_TRUE(probe.at("requested"));
  EXPECT_TRUE(probe.at("attempted"));
  EXPECT_EQ(probe.at("headless_extcopy").at("failure_reason"), "dmabuf_capture_not_initialized");
  EXPECT_EQ(probe.at("windowed").at("failure_stage"), "first_frame");
  EXPECT_EQ(probe.at("windowed").at("failure_reason"), "no_live_dmabuf_frame");
  EXPECT_EQ(probe.at("selected_strategy"), "headless_shm");
  EXPECT_EQ(probe.at("fallback"), "headless_shm");

  const auto profile = stream_stats::linux_gpu_profile_json(stats);
  EXPECT_TRUE(profile.at("gpu_native_requested"));
  EXPECT_TRUE(profile.at("gpu_native_attempted"));
  EXPECT_FALSE(profile.at("gpu_native_succeeded"));
}

TEST(StreamStatsGpuNativeProbeTests, ClearsStaleDeviceIdentityForNewCaptureGeneration) {
  stream_stats::update_capture_metadata(platf::frame_metadata_t {
    .device = "/dev/dri/renderD130",
  });
  stream_stats::update_wayland_main_device("/dev/dri/renderD131");

  stream_stats::reset_gpu_native_probe(true, true);
  const auto stats = stream_stats::get_current();

  EXPECT_TRUE(stats.capture_device.empty());
  EXPECT_TRUE(stats.wayland_main_device.empty());
}

TEST(StreamStatsGpuNativeProbeTests, RecordsCachedFailuresAndResetsForNextDecision) {
  stream_stats::update_wayland_main_device("/dev/dri/renderD128");
  stream_stats::reset_gpu_native_probe(true);
  EXPECT_EQ(stream_stats::get_current().wayland_main_device, "/dev/dri/renderD128");
  stream_stats::update_gpu_native_probe_attempt(
    "headless_extcopy",
    "failed",
    "capture_init",
    "dmabuf_capture_not_initialized"
  );
  stream_stats::update_gpu_native_probe_attempt(
    "windowed",
    "failed",
    "cache",
    "cached_unsupported",
    true
  );
  stream_stats::update_gpu_native_probe_selection("headless_shm", "headless_shm");

  auto stats = stream_stats::get_current();
  EXPECT_TRUE(stats.gpu_native_probe.requested);
  EXPECT_EQ(stats.gpu_native_probe.headless_extcopy.failure_reason, "dmabuf_capture_not_initialized");
  EXPECT_TRUE(stats.gpu_native_probe.windowed.cached);
  EXPECT_FALSE(stats.gpu_native_probe.windowed.attempted);
  EXPECT_EQ(stats.gpu_native_probe.selected_strategy, "headless_shm");

  stream_stats::reset_gpu_native_probe(false);
  stats = stream_stats::get_current();
  EXPECT_FALSE(stats.gpu_native_probe.requested);
  EXPECT_FALSE(stats.gpu_native_probe.headless_extcopy.attempted);
  EXPECT_EQ(stats.gpu_native_probe.windowed.result, "not_attempted");
  EXPECT_EQ(stats.gpu_native_probe.selected_strategy, "none");

  stream_stats::update_gpu_native_probe_attempt("headless_extcopy", "failed", "policy", "not_requested");
  EXPECT_FALSE(stream_stats::get_current().gpu_native_probe.headless_extcopy.attempted);
  stream_stats::update_wayland_main_device({});
}

TEST(StreamStatsCapturePathTests, DetectsCpuEncodeUpload) {
  stream_stats::stats_t stats {};
  stats.capture_transport = platf::frame_transport_e::dmabuf;
  stats.capture_residency = platf::frame_residency_e::gpu;
  stats.encode_target_residency = platf::frame_residency_e::cpu;

  EXPECT_EQ(stream_stats::capture_path_summary(stats), "cpu_encode_upload");
  EXPECT_EQ(stream_stats::capture_path_reason(stats), "encoder_upload_cpu");
  EXPECT_TRUE(stream_stats::capture_path_uses_cpu_copy(stats));
  EXPECT_FALSE(stream_stats::capture_path_is_gpu_native(stats));
}

TEST(StreamStatsCapturePathTests, DetectsFullyGpuNativePath) {
  stream_stats::stats_t stats {};
  stats.capture_transport = platf::frame_transport_e::dmabuf;
  stats.capture_residency = platf::frame_residency_e::gpu;
  stats.encode_target_residency = platf::frame_residency_e::gpu;

  EXPECT_EQ(stream_stats::capture_path_summary(stats), "gpu_native");
  EXPECT_EQ(stream_stats::capture_path_reason(stats), "gpu_native");
  EXPECT_FALSE(stream_stats::capture_path_uses_cpu_copy(stats));
  EXPECT_TRUE(stream_stats::capture_path_is_gpu_native(stats));
}

TEST(StreamStatsCapturePathTests, LabelsHeadlessExtcopyDmabufPath) {
  LinuxDisplayConfigGuard guard;
  config::video.adapter_name = "/dev/dri/renderD128";
  config::video.linux_display.use_cage_compositor = true;

  stream_stats::stats_t stats {};
  stats.runtime_effective_headless = true;
  stats.capture_transport = platf::frame_transport_e::dmabuf;
  stats.capture_residency = platf::frame_residency_e::gpu;
  stats.capture_format = platf::frame_format_e::bgra8;
  stats.capture_device = "/dev/dri/renderD128";
  stats.encode_target_residency = platf::frame_residency_e::gpu;

  EXPECT_EQ(stream_stats::capture_path_summary(stats), "gpu_native");
#ifdef __linux__
  EXPECT_FALSE(stream_stats::capture_path_has_cross_gpu_dmabuf_risk(stats));
  EXPECT_EQ(stream_stats::capture_path_reason(stats), "headless_extcopy_dmabuf");
#else
  EXPECT_EQ(stream_stats::capture_path_reason(stats), "gpu_native");
#endif
  EXPECT_FALSE(stream_stats::capture_path_uses_cpu_copy(stats));
  EXPECT_TRUE(stream_stats::capture_path_is_gpu_native(stats));
}

TEST(StreamStatsCapturePathTests, FlagsHeadlessCrossGpuDmabufRisk) {
  LinuxDisplayConfigGuard guard;
  config::video.adapter_name = "/dev/null";
  config::video.linux_display.use_cage_compositor = true;

  stream_stats::stats_t stats {};
  stats.runtime_backend = "labwc";
  stats.runtime_requested_headless = true;
  stats.runtime_effective_headless = true;
  stats.capture_transport = platf::frame_transport_e::dmabuf;
  stats.capture_residency = platf::frame_residency_e::gpu;
  stats.capture_format = platf::frame_format_e::bgra8;
  stats.capture_device = "/dev/zero";
  stats.encode_target_residency = platf::frame_residency_e::gpu;

#ifdef __linux__
  EXPECT_TRUE(stream_stats::capture_path_has_cross_gpu_dmabuf_risk(stats));
  EXPECT_EQ(stream_stats::capture_path_reason(stats), "headless_extcopy_dmabuf_cross_gpu_risk");
#else
  EXPECT_FALSE(stream_stats::capture_path_has_cross_gpu_dmabuf_risk(stats));
#endif

  const auto json = nlohmann::json::parse(stats.to_json());
  EXPECT_EQ(json.at("capture_device"), "/dev/zero");
  ASSERT_TRUE(json.contains("capture_decision"));
  const auto &decision = json.at("capture_decision");
  EXPECT_EQ(decision.at("capture_device"), "/dev/zero");
  EXPECT_EQ(decision.at("encoder_adapter"), "/dev/null");
#ifdef __linux__
  EXPECT_TRUE(decision.at("cross_gpu_dmabuf_risk"));
  EXPECT_EQ(decision.at("reason"), "headless_extcopy_dmabuf_cross_gpu_risk");
#else
  EXPECT_FALSE(decision.at("cross_gpu_dmabuf_risk"));
#endif
  EXPECT_FALSE(stream_stats::capture_path_uses_cpu_copy(stats));
  EXPECT_TRUE(stream_stats::capture_path_is_gpu_native(stats));
}

TEST(StreamStatsCapturePathTests, IgnoresCrossGpuRiskWithoutExplicitEncoderAdapter) {
  LinuxDisplayConfigGuard guard;
  config::video.adapter_name.clear();
  config::video.linux_display.use_cage_compositor = true;

  stream_stats::stats_t stats {};
  stats.runtime_effective_headless = true;
  stats.capture_transport = platf::frame_transport_e::dmabuf;
  stats.capture_residency = platf::frame_residency_e::gpu;
  stats.capture_device = "/dev/dri/renderD129";
  stats.encode_target_residency = platf::frame_residency_e::gpu;

  EXPECT_FALSE(stream_stats::capture_path_has_cross_gpu_dmabuf_risk(stats));
#ifdef __linux__
  EXPECT_EQ(stream_stats::capture_path_reason(stats), "headless_extcopy_dmabuf");
#else
  EXPECT_EQ(stream_stats::capture_path_reason(stats), "gpu_native");
#endif
}

TEST(StreamStatsCapturePathTests, LabelsWindowedDmabufOverridePath) {
  LinuxDisplayConfigGuard guard;
  config::video.linux_display.use_cage_compositor = true;

  stream_stats::stats_t stats {};
  stats.runtime_gpu_native_override_active = true;
  stats.capture_transport = platf::frame_transport_e::dmabuf;
  stats.capture_residency = platf::frame_residency_e::gpu;
  stats.encode_target_residency = platf::frame_residency_e::gpu;

  EXPECT_EQ(stream_stats::capture_path_summary(stats), "gpu_native");
#ifdef __linux__
  EXPECT_EQ(stream_stats::capture_path_reason(stats), "windowed_dmabuf_override");
#else
  EXPECT_EQ(stream_stats::capture_path_reason(stats), "gpu_native");
#endif
  EXPECT_FALSE(stream_stats::capture_path_uses_cpu_copy(stats));
  EXPECT_TRUE(stream_stats::capture_path_is_gpu_native(stats));
}


TEST(StreamStatsDoctorTests, ClassifiesGpuNativeStreamAsReady) {
  stream_stats::stats_t stats {};
  stats.streaming = true;
  stats.capture_transport = platf::frame_transport_e::dmabuf;
  stats.capture_residency = platf::frame_residency_e::gpu;
  stats.encode_target_residency = platf::frame_residency_e::gpu;
  stats.encode_time_ms = 4.0;
  stats.packet_loss = 0.0;

  const auto json = nlohmann::json::parse(stats.to_json());
  ASSERT_TRUE(json.contains("doctor"));
  const auto &doctor = json.at("doctor");

  EXPECT_EQ(doctor.at("simple_state"), "Streaming ready");
  EXPECT_EQ(doctor.at("traffic_light"), "green");
  EXPECT_EQ(doctor.at("primary_issue"), "none");
  EXPECT_EQ(doctor.at("safe_recovery_action").at("id"), "none");
  EXPECT_FALSE(doctor.at("safe_recovery_action").at("destructive"));
}

TEST(StreamStatsDoctorTests, KeepsNearTargetHighRefreshPacingGreen) {
  stream_stats::stats_t stats {};
  stats.streaming = true;
  stats.fps = 115.6;
  stats.encode_target_fps = 120;
  stats.capture_transport = platf::frame_transport_e::dmabuf;
  stats.capture_residency = platf::frame_residency_e::gpu;
  stats.encode_target_residency = platf::frame_residency_e::gpu;
  stats.encode_time_ms = 4.0;
  stats.packet_loss = 0.0;

  const auto doctor = stream_stats::build_doctor_json(
    stats,
    {{"primary_issue", "steady"}, {"grade", "good"}}
  );

  EXPECT_EQ(doctor.at("traffic_light"), "green");
  EXPECT_EQ(doctor.at("status"), "ok");
  EXPECT_EQ(doctor.at("primary_issue"), "none");
}

TEST(StreamStatsDoctorTests, ClassifiesVaapiShmFallbackAsAdvancedIssue) {
  LinuxDisplayConfigGuard guard;
  config::video.adapter_name = "/dev/dri/renderD128";
  config::video.linux_display.use_cage_compositor = true;
  config::video.linux_display.prefer_gpu_native_capture = true;

  stream_stats::stats_t stats {};
  stats.streaming = true;
  stats.runtime_effective_headless = true;
  stats.capture_transport = platf::frame_transport_e::shm;
  stats.capture_residency = platf::frame_residency_e::cpu;
  stats.capture_format = platf::frame_format_e::bgra8;
  stats.capture_device = "/dev/dri/renderD128";
  stats.encode_target_device = "vaapi";
  stats.encode_target_residency = platf::frame_residency_e::gpu;
  stats.encode_target_format = platf::frame_format_e::nv12;

  const auto doctor = stream_stats::build_doctor_json(stats, nlohmann::json::object());

  EXPECT_EQ(doctor.at("traffic_light"), "amber");
  EXPECT_EQ(doctor.at("simple_state"), "Advanced issue detected");
  EXPECT_EQ(doctor.at("primary_issue"), "gpu_native_requested_shm_fallback");
  EXPECT_FALSE(doctor.at("safe_recovery_action").at("destructive"));
  EXPECT_TRUE(doctor.at("advanced_evidence").at("raw_fields_redacted"));
  EXPECT_EQ(
    doctor.at("advanced_evidence").at("linux_gpu_profile").at("encoder_api"),
    "vaapi"
  );
}

TEST(StreamStatsDoctorTests, KeepsNvidiaHeadlessWarningsInAdvancedEvidence) {
  LinuxDisplayConfigGuard guard;
  config::video.encoder = "nvenc";
  config::video.linux_display.headless_mode = true;
  config::video.linux_display.use_cage_compositor = true;
  config::video.linux_display.prefer_gpu_native_capture = false;

  stream_stats::stats_t stats {};
  stats.streaming = true;
  stats.runtime_backend = "labwc";
  stats.runtime_requested_headless = true;
  stats.runtime_effective_headless = true;

  const auto doctor = stream_stats::build_doctor_json(stats, nlohmann::json::object());
  const auto &warnings = doctor.at("advanced_evidence").at("linux_gpu_profile").at("configuration_warnings");

  ASSERT_FALSE(warnings.empty());
  EXPECT_EQ(warnings.at(0).at("id"), "nvidia_headless_gpu_native_disabled");
  EXPECT_NE(warnings.at(0).at("message").get<std::string>().find("503"), std::string::npos);
  EXPECT_FALSE(doctor.at("safe_recovery_action").at("destructive"));
}

TEST(StreamStatsDoctorTests, CaptureMissingNeedsTelemetryBeforeTuning) {
  stream_stats::stats_t stats {};
  stats.streaming = true;

  const auto doctor = stream_stats::build_doctor_json(stats, nlohmann::json::object());

  EXPECT_EQ(doctor.at("traffic_light"), "amber");
  EXPECT_EQ(doctor.at("status"), "unknown");
  EXPECT_EQ(doctor.at("primary_issue"), "capture_missing");
  EXPECT_EQ(doctor.at("recommendation").at("next_step_label"), "Start stream");
  EXPECT_TRUE(doctor.at("redaction").at("applied"));
}
