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
#include <fstream>
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

  // A uniquely-named, empty regular file under the system temp directory,
  // removed on destruction. std::filesystem::equivalent()-based tests need
  // paths that reliably stat() in every environment this suite runs in -
  // unlike a GPU render node, or /dev/null and /dev/zero (neither of which
  // reliably resolves via equivalent() on every CI sandbox this suite has
  // actually run in), a freshly created regular file has no
  // environment-specific device-node handling to worry about.
  struct TempFileGuard {
    explicit TempFileGuard(const std::string &label) {
      static int counter = 0;
      path = std::filesystem::temp_directory_path() /
        ("polaris-stream-stats-" + label + "-" + std::to_string(++counter));
      std::ofstream(path).close();
    }

    ~TempFileGuard() {
      std::error_code ec;
      std::filesystem::remove(path, ec);
    }

    std::string string() const {
      return path.string();
    }

    std::filesystem::path path;
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
  // Flat capture_* only (nested capture_decision removed as pure UI-unused duplicate).
  EXPECT_FALSE(json.contains("capture_decision"));
  EXPECT_EQ(json.at("capture_transport"), "shm");
  EXPECT_EQ(json.at("capture_residency"), "cpu");
  EXPECT_EQ(json.at("capture_format"), "bgra8");
  EXPECT_TRUE(json.at("capture_cpu_copy"));
  EXPECT_FALSE(json.at("capture_gpu_native"));
  EXPECT_EQ(json.at("runtime_backend"), "labwc");
  EXPECT_TRUE(json.at("runtime_requested_headless"));
  EXPECT_TRUE(json.at("runtime_effective_headless"));
  EXPECT_FALSE(json.at("runtime_gpu_native_override_active"));
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
  TempFileGuard adapter_node("adapter-unavailable");
  TempFileGuard wayland_node("wayland-unavailable");
  config::video.adapter_name = adapter_node.string();

  stream_stats::stats_t stats {};
  stats.capture_transport = platf::frame_transport_e::shm;
  stats.capture_residency = platf::frame_residency_e::cpu;
  stats.wayland_main_device = wayland_node.string();
  stats.encode_target_device = "vaapi";
  stats.encode_target_residency = platf::frame_residency_e::gpu;

  const auto profile = stream_stats::linux_gpu_profile_json(stats);

  EXPECT_TRUE(profile.at("adapter_matches_capture_device").is_null());
  EXPECT_FALSE(profile.at("adapter_matches_wayland_main_device"));
  EXPECT_EQ(profile.at("adapter_pairing_status"), "mismatched");
  EXPECT_EQ(profile.at("adapter_pairing_device"), wayland_node.string());
  EXPECT_EQ(profile.at("adapter_pairing_device_source"), "wayland_main_device");

  const auto &warnings = profile.at("configuration_warnings");
  const auto mismatch = std::find_if(warnings.begin(), warnings.end(), [](const auto &warning) {
    return warning.value("id", std::string {}) == "linux_gpu_adapter_mismatch";
  });
  ASSERT_NE(mismatch, warnings.end());
  EXPECT_NE(mismatch->at("message").get<std::string>().find(adapter_node.string()), std::string::npos);
  EXPECT_NE(mismatch->at("message").get<std::string>().find(wayland_node.string()), std::string::npos);
}

#ifdef __linux__
TEST(StreamStatsLinuxGpuProfileTests, TreatsSymlinkedAdapterAsTheSameDeviceNode) {
  LinuxDisplayConfigGuard guard;
  TempFileGuard real_node("symlink-target");
  const auto link_path = std::filesystem::temp_directory_path() /
    ("polaris-stream-stats-device-link-" + std::to_string(::getpid()));
  std::error_code ec;
  std::filesystem::remove(link_path, ec);
  ec.clear();
  std::filesystem::create_symlink(real_node.path, link_path, ec);
  ASSERT_FALSE(ec);

  config::video.adapter_name = real_node.string();
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
  config::video.adapter_name = "/dev/null";
  config::video.linux_display.use_cage_compositor = true;
  config::video.linux_display.prefer_gpu_native_capture = true;

  stream_stats::stats_t stats {};
  stats.runtime_effective_headless = true;
  stats.capture_transport = platf::frame_transport_e::shm;
  stats.capture_residency = platf::frame_residency_e::cpu;
  stats.capture_format = platf::frame_format_e::bgra8;
  stats.capture_device = "/dev/null";
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
  EXPECT_EQ(profile.at("encoder_adapter"), "/dev/null");
  EXPECT_EQ(profile.at("capture_device"), "/dev/null");
  // Not EXPECT_TRUE: adapter_matches_capture_device comes from
  // std::filesystem::equivalent(), which needs the path to actually stat()
  // successfully on whatever machine runs this test. /dev/null reliably
  // exists on pc-papi and, empirically, does not reliably resolve the same
  // way on GitHub's hosted runner (CI caught this: identical device paths
  // still produced a null - i.e. "could not determine" - result there).
  // This test's job is explaining the GPU-native-requested-but-SHM-fallback
  // path, not proving device-node equivalence - that has its own dedicated
  // coverage (DoesNotCallMissingCaptureDeviceAnAdapterMatch,
  // TreatsSymlinkedAdapterAsTheSameDeviceNode). Accept either a real match
  // or an honest "unknown" here rather than asserting a specific filesystem
  // outcome this test doesn't actually depend on.
  const auto &adapter_match = profile.at("adapter_matches_capture_device");
  EXPECT_TRUE(adapter_match.is_null() || (adapter_match.is_boolean() && adapter_match.get<bool>()));
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
  TempFileGuard adapter_node("cross-gpu-adapter");
  TempFileGuard capture_node("cross-gpu-capture");
  config::video.adapter_name = adapter_node.string();
  config::video.linux_display.use_cage_compositor = true;

  stream_stats::stats_t stats {};
  stats.runtime_backend = "labwc";
  stats.runtime_requested_headless = true;
  stats.runtime_effective_headless = true;
  stats.capture_transport = platf::frame_transport_e::dmabuf;
  stats.capture_residency = platf::frame_residency_e::gpu;
  stats.capture_format = platf::frame_format_e::bgra8;
  stats.capture_device = capture_node.string();
  stats.encode_target_residency = platf::frame_residency_e::gpu;

#ifdef __linux__
  EXPECT_TRUE(stream_stats::capture_path_has_cross_gpu_dmabuf_risk(stats));
  EXPECT_EQ(stream_stats::capture_path_reason(stats), "headless_extcopy_dmabuf_cross_gpu_risk");
#else
  EXPECT_FALSE(stream_stats::capture_path_has_cross_gpu_dmabuf_risk(stats));
#endif

  const auto json = nlohmann::json::parse(stats.to_json());
  EXPECT_EQ(json.at("capture_device"), capture_node.string());
  EXPECT_FALSE(json.contains("capture_decision"));
#ifdef __linux__
  EXPECT_TRUE(json.at("capture_cross_gpu_dmabuf_risk"));
  EXPECT_EQ(json.at("capture_path_reason"), "headless_extcopy_dmabuf_cross_gpu_risk");
#else
  EXPECT_FALSE(json.at("capture_cross_gpu_dmabuf_risk"));
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

// The tests below exercise the global stats_t singleton (update_*() /
// get_current()) rather than locally-constructed stats_t values, unlike
// every test above. That is deliberate here: these are exactly the
// functions P0-2 changed from mutex-guarded fields to atomics, so the thing
// worth verifying is the plumbing between them, not pure-function behavior.
// Counters are asserted by delta, not absolute value, so run order and any
// other test's use of the singleton cannot make these flaky.

TEST(StreamStatsRecoveryCounterTests, RecordIdrRequestIncrementsByExactlyOnePerCall) {
  const auto before = stream_stats::get_current().idr_requests_total;

  stream_stats::record_idr_request();
  stream_stats::record_idr_request();
  stream_stats::record_idr_request();

  const auto after = stream_stats::get_current().idr_requests_total;
  EXPECT_EQ(after - before, 3u);
}

TEST(StreamStatsRecoveryCounterTests, RecordInvalidateRefFramesRequestIncrementsByExactlyOnePerCall) {
  const auto before = stream_stats::get_current().invalidate_ref_frames_requests_total;

  stream_stats::record_invalidate_ref_frames_request();

  const auto after = stream_stats::get_current().invalidate_ref_frames_requests_total;
  EXPECT_EQ(after - before, 1u);
}

TEST(StreamStatsHotFieldTests, UpdateVideoStatsIsVisibleThroughGetCurrent) {
  stream_stats::update_video_stats(87.5, 24000, 3.25, "hevc", 2560, 1440);

  const auto stats = stream_stats::get_current();

  EXPECT_DOUBLE_EQ(stats.fps, 87.5);
  EXPECT_EQ(stats.bitrate_kbps, 24000);
  EXPECT_DOUBLE_EQ(stats.encode_time_ms, 3.25);
  EXPECT_EQ(stats.codec, "hevc");
  EXPECT_EQ(stats.width, 2560);
  EXPECT_EQ(stats.height, 1440);

  // Reset so this test's values do not leak into any later use of the
  // singleton - mirrors what a real stream end already does.
  stream_stats::update_stream_active(false);
}

TEST(StreamStatsHotFieldTests, UpdateFrameDeliveryIsVisibleThroughGetCurrent) {
  stream_stats::update_frame_delivery(0.02, 0.01, 6.5, 1.2);

  const auto stats = stream_stats::get_current();

  EXPECT_DOUBLE_EQ(stats.duplicate_frame_ratio, 0.02);
  EXPECT_DOUBLE_EQ(stats.dropped_frame_ratio, 0.01);
  EXPECT_DOUBLE_EQ(stats.avg_frame_age_ms, 6.5);
  EXPECT_DOUBLE_EQ(stats.frame_jitter_ms, 1.2);

  stream_stats::update_stream_active(false);
}

TEST(StreamStatsHotFieldTests, UpdateNetworkStatsIsVisibleThroughGetCurrent) {
  stream_stats::update_network_stats(11.5, 0.4, 123456789ull);

  const auto stats = stream_stats::get_current();

  EXPECT_DOUBLE_EQ(stats.latency_ms, 11.5);
  EXPECT_DOUBLE_EQ(stats.packet_loss, 0.4);
  EXPECT_EQ(stats.bytes_sent, 123456789ull);

  stream_stats::update_stream_active(false);
}

// P0-3A: T0-T2 timing state is now per-session, keyed by device_uuid, with
// real start/stop lifecycle primitives (mirroring add_client()/remove_client()
// in stream.cpp). That gives every test below a genuinely clean, isolated
// slate via a unique uuid + start_session_timing() - no more need to
// flood-fill past a shared ring's capacity to drown out other tests' state.

TEST(StreamStatsSessionTimingTests, RecordFrameTimingReportsExactPercentilesForAFreshSession) {
  using namespace std::chrono;
  const std::string uuid = "test-uuid-fresh-session";
  const auto t0 = steady_clock::now();
  const auto t1 = t0 + milliseconds(3);
  const auto t2 = t1 + milliseconds(2);

  stream_stats::start_session_timing(uuid);
  for (int i = 0; i < 10; ++i) {
    stream_stats::record_frame_timing(uuid, t0, t1, t2);
  }

  const auto timing = stream_stats::get_session_timing(uuid);

  EXPECT_TRUE(timing.session_active);
  EXPECT_TRUE(timing.ring_complete);
  EXPECT_NE(timing.epoch_start_ns, 0u);

  EXPECT_DOUBLE_EQ(timing.capture_to_encode.p50_ms, 3.0);
  EXPECT_DOUBLE_EQ(timing.capture_to_encode.p99_ms, 3.0);
  EXPECT_EQ(timing.capture_to_encode.sample_count, 10);

  EXPECT_DOUBLE_EQ(timing.encode_to_send.p50_ms, 2.0);
  EXPECT_EQ(timing.encode_to_send.sample_count, 10);

  EXPECT_DOUBLE_EQ(timing.capture_to_send.p50_ms, 5.0);
  EXPECT_EQ(timing.capture_to_send.sample_count, 10);

  stream_stats::stop_session_timing(uuid);
}

TEST(StreamStatsSessionTimingTests, SessionWithNoRecordedFramesReportsActiveButEmpty) {
  const std::string uuid = "test-uuid-active-empty";

  stream_stats::start_session_timing(uuid);
  const auto timing = stream_stats::get_session_timing(uuid);

  EXPECT_TRUE(timing.session_active);
  EXPECT_EQ(timing.capture_to_encode.sample_count, 0);
  EXPECT_EQ(timing.encode_to_send.sample_count, 0);
  EXPECT_EQ(timing.capture_to_send.sample_count, 0);

  stream_stats::stop_session_timing(uuid);
}

TEST(StreamStatsSessionTimingTests, RecordFrameTimingIsANoOpForASessionThatWasNeverStarted) {
  using namespace std::chrono;
  const std::string uuid = "test-uuid-never-started";
  const auto t0 = steady_clock::now();

  // No start_session_timing() call - there is nowhere safe to attribute
  // this sample, so it must be silently dropped rather than crash or
  // fabricate a phantom session.
  stream_stats::record_frame_timing(uuid, t0, t0, t0);

  const auto timing = stream_stats::get_session_timing(uuid);
  EXPECT_FALSE(timing.session_active);
  EXPECT_EQ(timing.capture_to_encode.sample_count, 0);
}

// The fix this whole slice is about: two concurrent sessions (Polaris runs
// one independent encoder per client, default max_sessions 2) must not see
// each other's frame timings.
TEST(StreamStatsSessionTimingTests, TwoConcurrentSessionsDoNotShareTimingState) {
  using namespace std::chrono;
  const std::string uuid_a = "test-uuid-concurrent-a";
  const std::string uuid_b = "test-uuid-concurrent-b";
  const auto t0 = steady_clock::now();
  const auto fast = t0 + milliseconds(1);
  const auto slow = t0 + milliseconds(11);

  stream_stats::start_session_timing(uuid_a);
  stream_stats::start_session_timing(uuid_b);

  for (int i = 0; i < 5; ++i) {
    stream_stats::record_frame_timing(uuid_a, t0, t0, fast);
    stream_stats::record_frame_timing(uuid_b, t0, t0, slow);
  }

  const auto timing_a = stream_stats::get_session_timing(uuid_a);
  const auto timing_b = stream_stats::get_session_timing(uuid_b);

  EXPECT_DOUBLE_EQ(timing_a.capture_to_send.p50_ms, 1.0);
  EXPECT_EQ(timing_a.capture_to_send.sample_count, 5);

  EXPECT_DOUBLE_EQ(timing_b.capture_to_send.p50_ms, 11.0);
  EXPECT_EQ(timing_b.capture_to_send.sample_count, 5);

  stream_stats::stop_session_timing(uuid_a);
  stream_stats::stop_session_timing(uuid_b);
}

TEST(StreamStatsSessionTimingTests, StopSessionTimingDiscardsState) {
  using namespace std::chrono;
  const std::string uuid = "test-uuid-stop-discards";
  const auto t0 = steady_clock::now();

  stream_stats::start_session_timing(uuid);
  stream_stats::record_frame_timing(uuid, t0, t0, t0 + std::chrono::milliseconds(4));
  ASSERT_TRUE(stream_stats::get_session_timing(uuid).session_active);

  stream_stats::stop_session_timing(uuid);

  EXPECT_FALSE(stream_stats::get_session_timing(uuid).session_active);
}

TEST(StreamStatsSessionTimingTests, RestartingASessionGetsAFreshEpochAndDiscardsOldSamples) {
  using namespace std::chrono;
  const std::string uuid = "test-uuid-reconnect";
  const auto t0 = steady_clock::now();

  stream_stats::start_session_timing(uuid);
  stream_stats::record_frame_timing(uuid, t0, t0, t0 + milliseconds(4));
  const auto first_epoch = stream_stats::get_session_timing(uuid);
  ASSERT_EQ(first_epoch.capture_to_send.sample_count, 1);
  stream_stats::stop_session_timing(uuid);

  // Same device reconnecting - same uuid, new session.
  stream_stats::start_session_timing(uuid);
  const auto second_epoch = stream_stats::get_session_timing(uuid);

  EXPECT_TRUE(second_epoch.session_active);
  EXPECT_NE(second_epoch.epoch_start_ns, first_epoch.epoch_start_ns);
  EXPECT_EQ(second_epoch.capture_to_send.sample_count, 0);

  stream_stats::stop_session_timing(uuid);
}

// Mixing two distinct values should keep p50 and p99 both within the
// [min, max] of what was actually pushed - this doesn't assume a specific
// interpolation method or ordering, just that percentiles can't invent
// values outside the observed range.
TEST(StreamStatsSessionTimingTests, RecordFrameTimingMixedValuesStayWithinObservedRange) {
  using namespace std::chrono;
  const std::string uuid = "test-uuid-mixed-values";
  const auto t0 = steady_clock::now();
  const auto fast_t2 = t0 + milliseconds(1);
  const auto slow_t2 = t0 + milliseconds(9);

  stream_stats::start_session_timing(uuid);
  for (int i = 0; i < 300; ++i) {
    stream_stats::record_frame_timing(uuid, t0, t0, fast_t2);
  }
  for (int i = 0; i < 300; ++i) {
    stream_stats::record_frame_timing(uuid, t0, t0, slow_t2);
  }

  const auto timing = stream_stats::get_session_timing(uuid);

  EXPECT_GE(timing.capture_to_send.p50_ms, 1.0);
  EXPECT_LE(timing.capture_to_send.p50_ms, 9.0);
  EXPECT_GE(timing.capture_to_send.p99_ms, 1.0);
  EXPECT_LE(timing.capture_to_send.p99_ms, 9.0);
  EXPECT_EQ(timing.capture_to_send.sample_count, 600);

  stream_stats::stop_session_timing(uuid);
}

// The ring capacity (16384, matching FRAME_TIMING_RING_CAPACITY in
// stream_stats.cpp - not visible here, it's file-local, so this hardcodes
// the same number the implementation does) is sized for a full 120 s/120 fps
// bench run, but must still degrade honestly if exceeded: oldest samples
// evicted, ring_complete flips false, sample_count caps at capacity rather
// than over-reporting.
TEST(StreamStatsSessionTimingTests, RingWrapsAndReportsIncompleteOnceCapacityIsExceeded) {
  using namespace std::chrono;
  const std::string uuid = "test-uuid-ring-wrap";
  const auto t0 = steady_clock::now();
  const auto t2 = t0 + milliseconds(7);
  constexpr int kRingCapacity = 16384;

  stream_stats::start_session_timing(uuid);
  for (int i = 0; i < kRingCapacity + 100; ++i) {
    stream_stats::record_frame_timing(uuid, t0, t0, t2);
  }

  const auto timing = stream_stats::get_session_timing(uuid);

  EXPECT_FALSE(timing.ring_complete);
  EXPECT_EQ(timing.capture_to_send.sample_count, kRingCapacity);
  // Every pushed sample was identical, so even after wrapping the
  // percentiles are still exactly known.
  EXPECT_DOUBLE_EQ(timing.capture_to_send.p50_ms, 7.0);
  EXPECT_DOUBLE_EQ(timing.capture_to_send.p99_ms, 7.0);

  stream_stats::stop_session_timing(uuid);
}
