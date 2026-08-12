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

TEST(StreamStatsDoctorTests, FrameAgeOnCpuCopyCaptureIndictsCaptureNotEncoder) {
  // The issue #367 bundle: encode at 5.8 ms of a 16.6 ms budget, but frames
  // arriving 26.5 ms old because every 4K frame crossed the SHM CPU copy.
  // Frame age on a CPU-copy capture path measures the capture side, so Doctor
  // must indict the capture path — the old encoder_load verdict sent the user
  // to lower bitrate, which cannot help.
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
  stats.encode_target_device = "vaapi";
  stats.encode_target_residency = platf::frame_residency_e::gpu;
  stats.encode_time_ms = 5.8;
  stats.avg_frame_age_ms = 26.5;

  const auto doctor = stream_stats::build_doctor_json(stats, nlohmann::json::object());

  EXPECT_EQ(doctor.at("primary_issue"), "gpu_native_requested_shm_fallback");
  EXPECT_NE(doctor.at("safe_recovery_action").at("id"), "lower_bitrate");
  for (const auto &item : doctor.at("evidence")) {
    if (item.at("id") == "encoder") {
      EXPECT_EQ(item.at("status"), "pass")
        << "a healthy encode time must not be blamed for capture-side frame age";
    }
    if (item.at("id") == "capture_path") {
      EXPECT_EQ(item.at("status"), "fail")
        << "a CPU-copy capture path that blows the frame-age budget is the failing evidence";
    }
  }
}

TEST(StreamStatsDoctorTests, CaptureLatencyFailOutranksNetworkWatch) {
  // A WiFi client adds a mild network watch to the same SHM-bound stream; the
  // red capture verdict must keep the primary issue (and with it the
  // recommendation), or Doctor hands out lower-bitrate advice again through
  // the network branch. A hard network failure still takes priority.
  LinuxDisplayConfigGuard guard;
  config::video.linux_display.use_cage_compositor = true;
  config::video.linux_display.prefer_gpu_native_capture = true;

  stream_stats::stats_t stats {};
  stats.streaming = true;
  stats.runtime_effective_headless = true;
  stats.capture_transport = platf::frame_transport_e::shm;
  stats.capture_residency = platf::frame_residency_e::cpu;
  stats.encode_target_device = "vaapi";
  stats.encode_time_ms = 5.8;
  stats.avg_frame_age_ms = 26.5;
  stats.network_risk = true;
  stats.packet_loss = 0.5;
  stats.latency_ms = 20.0;

  const auto doctor = stream_stats::build_doctor_json(stats, nlohmann::json::object());

  EXPECT_EQ(doctor.at("primary_issue"), "gpu_native_requested_shm_fallback");
  EXPECT_NE(doctor.at("safe_recovery_action").at("id"), "lower_bitrate");
}

TEST(StreamStatsDoctorTests, SlowEncoderStillFailsOnCpuCopyCapture) {
  // The reattribution must not blind Doctor to a genuinely slow encoder: an
  // encode time over budget is encoder_load whatever the capture path.
  LinuxDisplayConfigGuard guard;
  config::video.linux_display.use_cage_compositor = true;
  config::video.linux_display.prefer_gpu_native_capture = true;

  stream_stats::stats_t stats {};
  stats.streaming = true;
  stats.runtime_effective_headless = true;
  stats.capture_transport = platf::frame_transport_e::shm;
  stats.capture_residency = platf::frame_residency_e::cpu;
  stats.encode_target_device = "vaapi";
  stats.encode_time_ms = 14.0;
  stats.avg_frame_age_ms = 26.5;

  const auto doctor = stream_stats::build_doctor_json(stats, nlohmann::json::object());

  EXPECT_EQ(doctor.at("primary_issue"), "encoder_load");

  // Without a computed safe bitrate the verdict must NOT offer lower_bitrate:
  // a zero payload would clear the client's bitrate cap instead of lowering it.
  EXPECT_NE(doctor.at("safe_recovery_action").at("id"), "lower_bitrate");

  // With a safe bitrate, the action must target the web server's own API
  // surface; the game-stream server's endpoints are unreachable from a
  // browser session.
  const auto doctor_with_target = stream_stats::build_doctor_json(stats, nlohmann::json {{"safe_bitrate_kbps", 12000}});
  const auto action = doctor_with_target.at("safe_recovery_action");
  EXPECT_EQ(action.at("id"), "lower_bitrate");
  EXPECT_EQ(action.at("endpoint"), "/api/clients/settings");
  EXPECT_EQ(action.at("method"), "POST");
  EXPECT_EQ(action.at("payload_preview").at("target_bitrate_kbps"), 12000);
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

// The periodic-ping handler in stream.cpp sources loss from ENet's scaled
// peer->packetLoss; this module stores percent (0-100), matching the readers
// (served network_risk elevates at 2% after warm-up and debounce; session
// grading stays at 0.5/2/5%).
TEST(StreamStatsHotFieldTests, RuntimeDisplayWarningIsServedAndResetWithTheStream) {
  stream_stats::update_stream_active(true);
  stream_stats::update_runtime_display_warning("Host Virtual Display could not be created");

  EXPECT_EQ(stream_stats::get_current().runtime_display_warning,
            "Host Virtual Display could not be created");

  // The end-of-stream wholesale reset is what makes the warning per-session.
  stream_stats::update_stream_active(false);
  EXPECT_TRUE(stream_stats::get_current().runtime_display_warning.empty());
}

TEST(StreamStatsHotFieldTests, PacketLossPercentConvertsScaledRatios) {
  constexpr uint64_t scale = 1ull << 16;  // ENET_PEER_PACKET_LOSS_SCALE

  EXPECT_DOUBLE_EQ(stream_stats::packet_loss_percent(0, scale), 0.0);
  EXPECT_DOUBLE_EQ(stream_stats::packet_loss_percent(scale, scale), 100.0);
  EXPECT_DOUBLE_EQ(stream_stats::packet_loss_percent(scale / 2, scale), 50.0);
  // 0.35% loss - the old hair-trigger risk threshold, now well inside calm -
  // survives the conversion.
  EXPECT_NEAR(stream_stats::packet_loss_percent(229, scale), 0.3494, 0.001);
}

TEST(NetworkRiskTrackerTests, WarmupNeverGrades) {
  stream_stats::network_risk_tracker_t tracker;
  // Session start is exactly when ENet's ratio comes from the fewest
  // packets; even absurd readings must not elevate yet.
  for (int i = 0; i < stream_stats::network_risk_tracker_t::k_warmup_samples; ++i) {
    EXPECT_FALSE(tracker.update(100.0, 500.0));
  }
}

TEST(NetworkRiskTrackerTests, TheLiveFalsePositiveStaysCalm) {
  stream_stats::network_risk_tracker_t tracker;
  // The regression this exists for: a perfect 115/120fps 3ms stream held a
  // permanent "Network jitter" because ENet's EWMA hovered past 0.35%.
  for (int i = 0; i < 50; ++i) {
    EXPECT_FALSE(tracker.update(0.5, 3.0));
  }
}

TEST(NetworkRiskTrackerTests, TwoElevatedFlipAndTwoCalmClear) {
  stream_stats::network_risk_tracker_t tracker;
  for (int i = 0; i < stream_stats::network_risk_tracker_t::k_warmup_samples; ++i) {
    tracker.update(0.0, 1.0);
  }
  EXPECT_FALSE(tracker.update(3.0, 1.0));  // one bad reading is noise
  EXPECT_TRUE(tracker.update(3.0, 1.0));  // two in a row is a state
  EXPECT_TRUE(tracker.update(0.0, 1.0));  // one calm reading is noise too
  EXPECT_FALSE(tracker.update(0.0, 1.0));  // two clear it
}

TEST(NetworkRiskTrackerTests, RttAloneElevates) {
  stream_stats::network_risk_tracker_t tracker;
  for (int i = 0; i < stream_stats::network_risk_tracker_t::k_warmup_samples; ++i) {
    tracker.update(0.0, 1.0);
  }
  tracker.update(0.0, 30.0);
  EXPECT_TRUE(tracker.update(0.0, 30.0));
}

TEST(NetworkRiskTrackerTests, EnetRttConvergenceNeverElevates) {
  stream_stats::network_risk_tracker_t tracker;
  // The live regression: ENet seeds a fresh peer at 500ms RTT and converges
  // by ~1/8 per ack, so early samples sit above the 28ms cut for ~20 samples
  // on a LAN whose true RTT is 3ms. That descent must never grade as risk.
  double rtt = 500.0;
  int calm_at = -1;
  for (int i = 0; i < 60; ++i) {
    EXPECT_FALSE(tracker.update(0.2, rtt)) << "sample " << i << " rtt " << rtt;
    if (calm_at < 0 && rtt < stream_stats::network_risk_tracker_t::k_rtt_elevated_ms) {
      calm_at = i;
    }
    rtt = 3.0 + (rtt - 3.0) * 7.0 / 8.0;
  }
  ASSERT_GT(calm_at, stream_stats::network_risk_tracker_t::k_warmup_samples);
  // Once armed by the calm readings, real degradation still flags promptly.
  tracker.update(0.2, 80.0);
  EXPECT_TRUE(tracker.update(0.2, 80.0));
}

TEST(NetworkRiskTrackerTests, BadFromTheStartStillFlagsAfterBoundedGrace) {
  stream_stats::network_risk_tracker_t tracker;
  // A link that never produces a calm reading cannot hide forever behind the
  // convergence grace: the sample bound arms the tracker and the debounce
  // flips it on the next confirmation.
  bool flagged = false;
  for (int i = 0; i < stream_stats::network_risk_tracker_t::k_armed_after_samples + 2; ++i) {
    flagged = tracker.update(6.0, 90.0);
  }
  EXPECT_TRUE(flagged);
}

TEST(StreamStatsHotFieldTests, PacketLossPercentClampsDegenerateInputs) {
  constexpr uint64_t scale = 1ull << 16;

  // A transport briefly reporting loss above its own scale must clamp, not
  // exceed 100%.
  EXPECT_DOUBLE_EQ(stream_stats::packet_loss_percent(scale * 2, scale), 100.0);
  // A zero scale is a caller bug; report no loss rather than dividing by zero.
  EXPECT_DOUBLE_EQ(stream_stats::packet_loss_percent(123, 0), 0.0);
}

// P0-3A: T0-T2 timing state is per-session, keyed by device_uuid plus a
// session_generation (measurement-spec-v1.md's ownership model - a
// process-lifetime-monotonic counter, distinct from device_uuid, assigned
// fresh to every session_t including a reconnecting device that reuses its
// uuid). start_session_timing()/stop_session_timing() mirror
// add_client()/remove_client() in stream.cpp. Real lifecycle primitives give
// every test below a genuinely clean, isolated slate via a unique uuid - no
// need to flood-fill past a shared ring's capacity to drown out other
// tests' state. Generation numbers in these tests are arbitrary opaque
// values chosen for readability, not the real global counter in stream.cpp
// (start_session_timing()/record_frame_timing() take whatever the caller
// hands them - stream.cpp's session_t::session_generation is just one
// caller).

TEST(StreamStatsSessionTimingTests, RecordFrameTimingReportsExactPercentilesForAFreshSession) {
  using namespace std::chrono;
  const std::string uuid = "test-uuid-fresh-session";
  const auto t0 = steady_clock::now();
  const auto t1 = t0 + milliseconds(3);
  const auto t2 = t1 + milliseconds(2);

  stream_stats::start_session_timing(uuid, 1);
  for (int i = 0; i < 10; ++i) {
    stream_stats::record_frame_timing(uuid, 1, t0, t1, t2);
  }

  const auto timing = stream_stats::get_session_timing(uuid);

  EXPECT_TRUE(timing.session_active);
  EXPECT_TRUE(timing.ring_complete);
  EXPECT_EQ(timing.session_generation, 1u);

  EXPECT_DOUBLE_EQ(timing.capture_to_encode.p50_ms, 3.0);
  EXPECT_DOUBLE_EQ(timing.capture_to_encode.p99_ms, 3.0);
  EXPECT_EQ(timing.capture_to_encode.sample_count, 10);
  EXPECT_EQ(timing.capture_to_encode.invalid_count, 0);

  EXPECT_DOUBLE_EQ(timing.encode_to_send.p50_ms, 2.0);
  EXPECT_EQ(timing.encode_to_send.sample_count, 10);

  EXPECT_DOUBLE_EQ(timing.capture_to_send.p50_ms, 5.0);
  EXPECT_EQ(timing.capture_to_send.sample_count, 10);

  stream_stats::stop_session_timing(uuid, 1);
}

TEST(StreamStatsSessionTimingTests, SessionWithNoRecordedFramesReportsActiveButEmpty) {
  const std::string uuid = "test-uuid-active-empty";

  stream_stats::start_session_timing(uuid, 1);
  const auto timing = stream_stats::get_session_timing(uuid);

  EXPECT_TRUE(timing.session_active);
  EXPECT_EQ(timing.capture_to_encode.sample_count, 0);
  EXPECT_EQ(timing.encode_to_send.sample_count, 0);
  EXPECT_EQ(timing.capture_to_send.sample_count, 0);

  stream_stats::stop_session_timing(uuid, 1);
}

TEST(StreamStatsSessionTimingTests, RecordFrameTimingIsANoOpForASessionThatWasNeverStarted) {
  using namespace std::chrono;
  const std::string uuid = "test-uuid-never-started";
  const auto t0 = steady_clock::now();

  // No start_session_timing() call - there is nowhere safe to attribute
  // this sample, so it must be silently dropped rather than crash or
  // fabricate a phantom session.
  stream_stats::record_frame_timing(uuid, 1, t0, t0, t0);

  const auto timing = stream_stats::get_session_timing(uuid);
  EXPECT_FALSE(timing.session_active);
  EXPECT_EQ(timing.capture_to_encode.sample_count, 0);
}

// The fix this whole slice is about: two concurrent sessions (Polaris runs
// one independent encoder per client, default max_sessions 2) must not see
// each other's frame timings. Different uuids here also stands in for
// measurement-spec-v1.md's "two sessions share one source IP" case
// (§15.1.3): stream_stats never sees an IP at all, only device_uuid, so
// nothing about a shared source address could make two distinct uuids
// collide in the first place.
TEST(StreamStatsSessionTimingTests, TwoConcurrentSessionsDoNotShareTimingState) {
  using namespace std::chrono;
  const std::string uuid_a = "test-uuid-concurrent-a";
  const std::string uuid_b = "test-uuid-concurrent-b";
  const auto t0 = steady_clock::now();
  const auto fast = t0 + milliseconds(1);
  const auto slow = t0 + milliseconds(11);

  stream_stats::start_session_timing(uuid_a, 1);
  stream_stats::start_session_timing(uuid_b, 2);

  for (int i = 0; i < 5; ++i) {
    stream_stats::record_frame_timing(uuid_a, 1, t0, t0, fast);
    stream_stats::record_frame_timing(uuid_b, 2, t0, t0, slow);
  }

  const auto timing_a = stream_stats::get_session_timing(uuid_a);
  const auto timing_b = stream_stats::get_session_timing(uuid_b);

  EXPECT_DOUBLE_EQ(timing_a.capture_to_send.p50_ms, 1.0);
  EXPECT_EQ(timing_a.capture_to_send.sample_count, 5);

  EXPECT_DOUBLE_EQ(timing_b.capture_to_send.p50_ms, 11.0);
  EXPECT_EQ(timing_b.capture_to_send.sample_count, 5);

  stream_stats::stop_session_timing(uuid_a, 1);
  stream_stats::stop_session_timing(uuid_b, 2);
}

TEST(StreamStatsSessionTimingTests, StopSessionTimingDiscardsState) {
  using namespace std::chrono;
  const std::string uuid = "test-uuid-stop-discards";
  const auto t0 = steady_clock::now();

  stream_stats::start_session_timing(uuid, 1);
  stream_stats::record_frame_timing(uuid, 1, t0, t0, t0 + std::chrono::milliseconds(4));
  ASSERT_TRUE(stream_stats::get_session_timing(uuid).session_active);

  stream_stats::stop_session_timing(uuid, 1);

  EXPECT_FALSE(stream_stats::get_session_timing(uuid).session_active);
}

TEST(StreamStatsSessionTimingTests, RestartingASessionGetsAFreshGenerationAndDiscardsOldSamples) {
  using namespace std::chrono;
  const std::string uuid = "test-uuid-reconnect";
  const auto t0 = steady_clock::now();

  stream_stats::start_session_timing(uuid, 1);
  stream_stats::record_frame_timing(uuid, 1, t0, t0, t0 + milliseconds(4));
  const auto first = stream_stats::get_session_timing(uuid);
  ASSERT_EQ(first.capture_to_send.sample_count, 1);
  stream_stats::stop_session_timing(uuid, 1);

  // Same device reconnecting - same uuid, new (higher) generation, matching
  // stream.cpp's next_session_generation: it only ever increases.
  stream_stats::start_session_timing(uuid, 2);
  const auto second = stream_stats::get_session_timing(uuid);

  EXPECT_TRUE(second.session_active);
  EXPECT_GT(second.session_generation, first.session_generation);
  EXPECT_EQ(second.capture_to_send.sample_count, 0);

  stream_stats::stop_session_timing(uuid, 2);
}

// measurement-spec-v1.md §6.1/§15.1.2: "Old queued work drains after A
// retires. It cannot mutate B." The video send thread reads a packet's
// owning session_t (and hence its device_uuid/session_generation) from a
// queue that can still hold a few of session A's already-encoded packets
// at the exact moment A retires and a fast reconnect hands the same
// device_uuid to session B. Without the generation check, those stale
// writes would land in B's fresh state under A's old device_uuid.
TEST(StreamStatsSessionTimingTests, StaleGenerationWriteIsRejectedAfterANewerSessionTakesTheUuid) {
  using namespace std::chrono;
  const std::string uuid = "test-uuid-stale-write";
  const auto t0 = steady_clock::now();

  stream_stats::start_session_timing(uuid, 1);
  stream_stats::stop_session_timing(uuid, 1);  // Session A retires...
  stream_stats::start_session_timing(uuid, 2);  // ...B immediately reconnects.

  // A late-arriving packet from the drained queue, still carrying A's old
  // generation.
  stream_stats::record_frame_timing(uuid, 1, t0, t0, t0 + milliseconds(4));

  const auto timing = stream_stats::get_session_timing(uuid);
  EXPECT_EQ(timing.session_generation, 2u);
  EXPECT_EQ(timing.capture_to_send.sample_count, 0)
    << "session A's stale write must not appear in session B's data";

  stream_stats::stop_session_timing(uuid, 2);
}

// The other half of the same race: a stop() call for a retired generation
// must not discard a newer session's state, even though it targets the
// same device_uuid.
TEST(StreamStatsSessionTimingTests, StaleGenerationStopDoesNotDiscardANewerSessionsState) {
  using namespace std::chrono;
  const std::string uuid = "test-uuid-stale-stop";
  const auto t0 = steady_clock::now();

  stream_stats::start_session_timing(uuid, 1);
  stream_stats::start_session_timing(uuid, 2);  // B has already taken over.
  stream_stats::record_frame_timing(uuid, 2, t0, t0, t0 + milliseconds(4));

  // A's teardown path calls stop_session_timing() after B already exists.
  stream_stats::stop_session_timing(uuid, 1);

  const auto timing = stream_stats::get_session_timing(uuid);
  EXPECT_TRUE(timing.session_active) << "B's session must survive A's stale stop";
  EXPECT_EQ(timing.session_generation, 2u);
  EXPECT_EQ(timing.capture_to_send.sample_count, 1);

  stream_stats::stop_session_timing(uuid, 2);
}

// measurement-spec-v1.md §15.1.8: negative or non-monotonic durations
// (a corrupted or misordered timestamp pair) must be rejected and counted,
// never pushed into the percentile ring.
TEST(StreamStatsSessionTimingTests, NegativeOrNonMonotonicDurationsAreRejectedAndCounted) {
  using namespace std::chrono;
  const std::string uuid = "test-uuid-invalid-durations";
  const auto t0 = steady_clock::now();

  stream_stats::start_session_timing(uuid, 1);

  // encode_done_time before capture_time: capture_to_encode and
  // capture_to_send both go negative; encode_to_send stays valid (positive).
  stream_stats::record_frame_timing(uuid, 1, t0, t0 - milliseconds(1), t0 + milliseconds(5));
  // A fully valid sample too, to prove valid and invalid samples don't
  // interfere with each other's bookkeeping.
  stream_stats::record_frame_timing(uuid, 1, t0, t0 + milliseconds(2), t0 + milliseconds(5));

  const auto timing = stream_stats::get_session_timing(uuid);

  EXPECT_EQ(timing.capture_to_encode.invalid_count, 1);
  EXPECT_EQ(timing.capture_to_encode.sample_count, 1);

  EXPECT_EQ(timing.capture_to_send.invalid_count, 0)
    << "t0 -> t2 stayed monotonic in both samples even though t0 -> t1 didn't";
  EXPECT_EQ(timing.capture_to_send.sample_count, 2);

  EXPECT_EQ(timing.encode_to_send.invalid_count, 0);
  EXPECT_EQ(timing.encode_to_send.sample_count, 2);

  stream_stats::stop_session_timing(uuid, 1);
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

  stream_stats::start_session_timing(uuid, 1);
  for (int i = 0; i < 300; ++i) {
    stream_stats::record_frame_timing(uuid, 1, t0, t0, fast_t2);
  }
  for (int i = 0; i < 300; ++i) {
    stream_stats::record_frame_timing(uuid, 1, t0, t0, slow_t2);
  }

  const auto timing = stream_stats::get_session_timing(uuid);

  EXPECT_GE(timing.capture_to_send.p50_ms, 1.0);
  EXPECT_LE(timing.capture_to_send.p50_ms, 9.0);
  EXPECT_GE(timing.capture_to_send.p99_ms, 1.0);
  EXPECT_LE(timing.capture_to_send.p99_ms, 9.0);
  EXPECT_EQ(timing.capture_to_send.sample_count, 600);

  stream_stats::stop_session_timing(uuid, 1);
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

  stream_stats::start_session_timing(uuid, 1);
  for (int i = 0; i < kRingCapacity + 100; ++i) {
    stream_stats::record_frame_timing(uuid, 1, t0, t0, t2);
  }

  const auto timing = stream_stats::get_session_timing(uuid);

  EXPECT_FALSE(timing.ring_complete);
  EXPECT_EQ(timing.capture_to_send.sample_count, kRingCapacity);
  // Every pushed sample was identical, so even after wrapping the
  // percentiles are still exactly known.
  EXPECT_DOUBLE_EQ(timing.capture_to_send.p50_ms, 7.0);
  EXPECT_DOUBLE_EQ(timing.capture_to_send.p99_ms, 7.0);

  stream_stats::stop_session_timing(uuid, 1);
}

// get_single_active_session_identity() backs the P0-5 benchmark control
// surface's create route (measurement-spec-v1.md 6.4): a harness's
// create-and-arm request names no device_uuid at all, since the harness
// isn't the streaming client itself, so the route needs to find "the one
// active session" on its own.

TEST(GetSingleActiveSessionIdentityTests, ReturnsNulloptWhenNoSessionsAreActive) {
  EXPECT_FALSE(stream_stats::get_single_active_session_identity().has_value());
}

TEST(GetSingleActiveSessionIdentityTests, ReturnsTheSoleSessionsIdentityWhenExactlyOneIsActive) {
  const std::string uuid = "test-uuid-single-active-session";
  stream_stats::start_session_timing(uuid, 7);

  const auto identity = stream_stats::get_single_active_session_identity();
  ASSERT_TRUE(identity.has_value());
  EXPECT_EQ(identity->device_uuid, uuid);
  EXPECT_EQ(identity->session_generation, 7u);

  stream_stats::stop_session_timing(uuid, 7);
}

TEST(GetSingleActiveSessionIdentityTests, ReturnsNulloptWhenMultipleSessionsAreActive) {
  const std::string uuid_a = "test-uuid-multi-active-a";
  const std::string uuid_b = "test-uuid-multi-active-b";
  stream_stats::start_session_timing(uuid_a, 1);
  stream_stats::start_session_timing(uuid_b, 1);

  EXPECT_FALSE(stream_stats::get_single_active_session_identity().has_value());

  stream_stats::stop_session_timing(uuid_a, 1);
  stream_stats::stop_session_timing(uuid_b, 1);
}

// measurement-spec-v1.md 6.1: client_population_revision is a global,
// process-lifetime counter (not session-keyed like the tests above), so -
// like the pre-existing StreamStatsHotFieldTests - these assert on the
// delta a single call produces rather than an absolute value, since other
// tests in this binary call add_client()/remove_client()/
// update_stream_active() too.

TEST(StreamStatsClientPopulationRevisionTests, AddAndRemoveClientEachAdvanceTheRevisionByOne) {
  const auto before_add = stream_stats::client_population_revision();
  stream_stats::add_client("203.0.113.10", "pop-revision-test-add");
  const auto after_add = stream_stats::client_population_revision();
  EXPECT_EQ(after_add, before_add + 1);

  stream_stats::remove_client("203.0.113.10");
  const auto after_remove = stream_stats::client_population_revision();
  EXPECT_EQ(after_remove, after_add + 1);
}

// The exact scenario measurement-spec-v1.md 6.1 calls out: a leave/join
// replacement that returns the active client count to its original value
// must still move the revision, so an armed benchmark run can't be fooled
// by a population change that "cancels out" before anyone polls it.
TEST(StreamStatsClientPopulationRevisionTests, LeaveAndRejoinReplacementAdvancesRevisionDespiteReturningToTheSameCount) {
  const std::string ip = "203.0.113.11";
  stream_stats::add_client(ip, "pop-revision-test-steady-state");
  const auto steady_state_count = stream_stats::active_client_count();
  const auto revision_before_churn = stream_stats::client_population_revision();

  stream_stats::remove_client(ip);
  stream_stats::add_client(ip, "pop-revision-test-steady-state");

  EXPECT_EQ(stream_stats::active_client_count(), steady_state_count)
    << "count should be back to where it started";
  EXPECT_EQ(stream_stats::client_population_revision(), revision_before_churn + 2)
    << "but the revision must show the two real events that happened in between";

  stream_stats::remove_client(ip);
}

// The bug this design specifically guards against: update_stream_active(false)
// does current_stats = stats_t{} wholesale when all sessions end. If
// client_population_revision lived inside stats_t (as first implemented,
// then caught in review before it shipped), that reset would silently
// rewind the counter, and a benchmark run spanning a full stream restart
// could see its arm-time and freeze-time revisions coincidentally match.
TEST(StreamStatsClientPopulationRevisionTests, SurvivesTheFullStatsResetOnStreamEnd) {
  stream_stats::add_client("203.0.113.12", "pop-revision-test-reset-survival");
  const auto revision_after_add = stream_stats::client_population_revision();

  stream_stats::update_stream_active(false);

  EXPECT_EQ(stream_stats::client_population_revision(), revision_after_add)
    << "the population revision must not be rewound by the stats_t reset";
}

// P0-5 benchmark-run-capture engine, piece 1: classify_boundary() and
// benchmark_stage_capture_t are pure logic with no global state, so unlike
// the tests above, these need no unique IDs or delta comparisons - each
// test constructs its own local instance.

TEST(ClassifyBoundaryTests, AcceptsAnObservationEntirelyWithinTheWindow) {
  EXPECT_EQ(stream_stats::classify_boundary(10, 20, 100), stream_stats::boundary_classification_e::accepted);
}

TEST(ClassifyBoundaryTests, AcceptsAZeroDurationObservation) {
  EXPECT_EQ(stream_stats::classify_boundary(10, 10, 100), stream_stats::boundary_classification_e::accepted);
}

TEST(ClassifyBoundaryTests, AcceptsAnObservationStartingExactlyAtS) {
  // The spec's rule is the closed lower bound S <= a - a == 0 (== S) must
  // still be accepted, not excluded.
  EXPECT_EQ(stream_stats::classify_boundary(0, 5, 100), stream_stats::boundary_classification_e::accepted);
}

TEST(ClassifyBoundaryTests, ExcludesAnObservationThatStartedBeforeTheWindow) {
  EXPECT_EQ(stream_stats::classify_boundary(-5, 20, 100), stream_stats::boundary_classification_e::excluded_before_window);
}

TEST(ClassifyBoundaryTests, IgnoresAnObservationStartingAtOrAfterTheWindowEnd) {
  EXPECT_EQ(stream_stats::classify_boundary(100, 110, 100), stream_stats::boundary_classification_e::ignored_post_window)
    << "a == E is already post-window (E is exclusive)";
  EXPECT_EQ(stream_stats::classify_boundary(150, 160, 100), stream_stats::boundary_classification_e::ignored_post_window);
}

TEST(ClassifyBoundaryTests, ExcludesAnObservationThatCompletesExactlyAtOrAfterTheWindowEnd) {
  // The spec's rule is the open upper bound b < E - b == E must be
  // excluded, not accepted, even though a is still in-window.
  EXPECT_EQ(stream_stats::classify_boundary(10, 100, 100), stream_stats::boundary_classification_e::excluded_after_window);
  EXPECT_EQ(stream_stats::classify_boundary(10, 150, 100), stream_stats::boundary_classification_e::excluded_after_window);
}

TEST(ClassifyBoundaryTests, RejectsANonMonotonicObservationAsInvalid) {
  EXPECT_EQ(stream_stats::classify_boundary(20, 10, 100), stream_stats::boundary_classification_e::invalid_non_monotonic);
}

TEST(ClassifyBoundaryTests, ClassificationOrderPrefersBeforeWindowOverNonMonotonic) {
  // a < 0 and a > b are both true here - rule 1 (before-window) must win,
  // matching the spec's "classified exactly once, in this order".
  EXPECT_EQ(stream_stats::classify_boundary(-10, -20, 100), stream_stats::boundary_classification_e::excluded_before_window);
}

TEST(BenchmarkStageCaptureTests, RecordAcceptsAndStoresWithinCapacity) {
  stream_stats::benchmark_stage_capture_t stage(10);

  const auto classification = stage.record(10, 25, 1000);

  EXPECT_EQ(classification, stream_stats::boundary_classification_e::accepted);
  EXPECT_EQ(stage.accepted_count, 1u);
  ASSERT_EQ(stage.start_offset_us.size(), 1u);
  ASSERT_EQ(stage.end_offset_us.size(), 1u);
  ASSERT_EQ(stage.duration_us.size(), 1u);
  EXPECT_EQ(stage.start_offset_us[0], 10u);
  EXPECT_EQ(stage.end_offset_us[0], 25u);
  EXPECT_EQ(stage.duration_us[0], 15u);
}

TEST(BenchmarkStageCaptureTests, OverflowsPastCapacityWithoutGrowingStorage) {
  stream_stats::benchmark_stage_capture_t stage(2);

  stage.record(0, 1, 1000);
  stage.record(2, 3, 1000);
  stage.record(4, 5, 1000);  // Third accepted-shaped observation, capacity is 2.

  EXPECT_EQ(stage.accepted_count, 2u);
  EXPECT_EQ(stage.overflow_count, 1u);
  EXPECT_EQ(stage.start_offset_us.size(), 2u) << "the third observation must not have been stored";
}

TEST(BenchmarkStageCaptureTests, CountsExcludedBeforeWindowWithoutStoring) {
  stream_stats::benchmark_stage_capture_t stage(10);

  stage.record(-5, 20, 100);

  EXPECT_EQ(stage.excluded_started_before_window, 1u);
  EXPECT_EQ(stage.accepted_count, 0u);
  EXPECT_TRUE(stage.start_offset_us.empty());
}

TEST(BenchmarkStageCaptureTests, CountsExcludedAfterWindowWithoutStoring) {
  stream_stats::benchmark_stage_capture_t stage(10);

  stage.record(10, 100, 100);

  EXPECT_EQ(stage.excluded_completed_after_window, 1u);
  EXPECT_EQ(stage.accepted_count, 0u);
  EXPECT_TRUE(stage.start_offset_us.empty());
}

TEST(BenchmarkStageCaptureTests, CountsInvalidNonMonotonicWithoutStoring) {
  stream_stats::benchmark_stage_capture_t stage(10);

  stage.record(20, 10, 100);

  EXPECT_EQ(stage.invalid_count, 1u);
  EXPECT_EQ(stage.accepted_count, 0u);
  EXPECT_TRUE(stage.start_offset_us.empty());
}

TEST(BenchmarkStageCaptureTests, IgnoredPostWindowObservationIncrementsNoCounterAtAll) {
  stream_stats::benchmark_stage_capture_t stage(10);

  const auto classification = stage.record(150, 160, 100);

  EXPECT_EQ(classification, stream_stats::boundary_classification_e::ignored_post_window);
  EXPECT_EQ(stage.accepted_count, 0u);
  EXPECT_EQ(stage.excluded_started_before_window, 0u);
  EXPECT_EQ(stage.excluded_completed_after_window, 0u);
  EXPECT_EQ(stage.invalid_count, 0u);
  EXPECT_EQ(stage.overflow_count, 0u);
  EXPECT_EQ(stage.started_in_window_without_terminal_count, 0u);
}

TEST(BenchmarkStageCaptureTests, MultipleAcceptedObservationsStayInInsertionOrder) {
  stream_stats::benchmark_stage_capture_t stage(5);

  stage.record(0, 5, 1000);
  stage.record(100, 108, 1000);
  stage.record(200, 203, 1000);

  ASSERT_EQ(stage.duration_us.size(), 3u);
  EXPECT_EQ(stage.duration_us[0], 5u);
  EXPECT_EQ(stage.duration_us[1], 8u);
  EXPECT_EQ(stage.duration_us[2], 3u);
}

// P0-5 benchmark-run-capture engine, piece 2: benchmark_run_t and its
// process-wide storage/retention. Storage is global (not session-keyed),
// so - like ClientPopulationRevisionTests - these use unique run_ids per
// test rather than relying on execution order or isolation between tests.

TEST(BenchmarkRunTests, ConstructorForwardsCapacityToAllThreeStages) {
  stream_stats::benchmark_run_t run(42);

  EXPECT_EQ(run.sample_capacity, 42u);
  EXPECT_EQ(run.capture_to_encode.capacity, 42u);
  EXPECT_EQ(run.encode_to_send_release.capacity, 42u);
  EXPECT_EQ(run.capture_to_send_release.capacity, 42u);
  EXPECT_EQ(run.state, stream_stats::benchmark_run_state_e::armed)
    << "a freshly constructed run defaults to armed";
}

TEST(BenchmarkRunTests, ProcessInstanceIdIsNonEmptyAndStable) {
  const auto &first = stream_stats::process_instance_id();
  const auto &second = stream_stats::process_instance_id();

  EXPECT_FALSE(first.empty());
  EXPECT_EQ(&first, &second) << "must be the same stable value, not regenerated per call";
}

TEST(BenchmarkRunStorageTests, WithBenchmarkRunFindsAnInsertedRunByItsId) {
  stream_stats::benchmark_run_t run(10);
  run.run_id = "test-run-find-me";
  run.label = "find-me-label";
  stream_stats::insert_benchmark_run(std::move(run));

  bool visited = false;
  const bool found = stream_stats::with_benchmark_run("test-run-find-me", [&](stream_stats::benchmark_run_t &r) {
    visited = true;
    EXPECT_EQ(r.label, "find-me-label");
  });

  EXPECT_TRUE(found);
  EXPECT_TRUE(visited);

  stream_stats::erase_benchmark_run("test-run-find-me");
}

TEST(BenchmarkRunStorageTests, WithBenchmarkRunReturnsFalseForAnUnknownId) {
  bool visited = false;
  const bool found = stream_stats::with_benchmark_run("test-run-does-not-exist", [&](stream_stats::benchmark_run_t &) {
    visited = true;
  });

  EXPECT_FALSE(found);
  EXPECT_FALSE(visited);
}

TEST(BenchmarkRunStorageTests, WithBenchmarkRunMutationsPersist) {
  stream_stats::benchmark_run_t run(10);
  run.run_id = "test-run-mutate";
  stream_stats::insert_benchmark_run(std::move(run));

  stream_stats::with_benchmark_run("test-run-mutate", [](stream_stats::benchmark_run_t &r) {
    r.state = stream_stats::benchmark_run_state_e::active;
  });

  bool state_is_active = false;
  stream_stats::with_benchmark_run("test-run-mutate", [&](stream_stats::benchmark_run_t &r) {
    state_is_active = (r.state == stream_stats::benchmark_run_state_e::active);
  });
  EXPECT_TRUE(state_is_active);

  stream_stats::erase_benchmark_run("test-run-mutate");
}

TEST(BenchmarkRunStorageTests, EraseBenchmarkRunRemovesOnlyTheNamedRun) {
  stream_stats::benchmark_run_t run_a(10);
  run_a.run_id = "test-run-erase-a";
  stream_stats::insert_benchmark_run(std::move(run_a));

  stream_stats::benchmark_run_t run_b(10);
  run_b.run_id = "test-run-erase-b";
  stream_stats::insert_benchmark_run(std::move(run_b));

  stream_stats::erase_benchmark_run("test-run-erase-a");

  EXPECT_FALSE(stream_stats::with_benchmark_run("test-run-erase-a", [](stream_stats::benchmark_run_t &) {}));
  EXPECT_TRUE(stream_stats::with_benchmark_run("test-run-erase-b", [](stream_stats::benchmark_run_t &) {}));

  stream_stats::erase_benchmark_run("test-run-erase-b");
}

TEST(BenchmarkRunStorageTests, ExpireBenchmarkRunClearsPayloadButKeepsTombstoneMetadata) {
  stream_stats::benchmark_run_t run(10);
  run.run_id = "test-run-expire";
  run.label = "expire-me-label";
  run.capture_to_encode.record(0, 5, 1000);
  ASSERT_EQ(run.capture_to_encode.accepted_count, 1u);
  stream_stats::insert_benchmark_run(std::move(run));

  stream_stats::expire_benchmark_run("test-run-expire");

  bool visited = false;
  stream_stats::with_benchmark_run("test-run-expire", [&](stream_stats::benchmark_run_t &r) {
    visited = true;
    EXPECT_EQ(r.state, stream_stats::benchmark_run_state_e::expired);
    EXPECT_EQ(r.label, "expire-me-label") << "lightweight metadata survives as the tombstone";
    EXPECT_TRUE(r.capture_to_encode.start_offset_us.empty()) << "the heavy payload must be cleared";
    // The accepted_count itself is retained - it's a small integer, part
    // of the "bounded tombstone metadata needed to explain the missing
    // payload", not the payload itself.
    EXPECT_EQ(r.capture_to_encode.accepted_count, 1u);
  });
  EXPECT_TRUE(visited);

  stream_stats::erase_benchmark_run("test-run-expire");
}

TEST(BenchmarkRunStorageTests, ExpireBenchmarkRunIsANoOpForAnUnknownId) {
  // Must not crash or throw.
  stream_stats::expire_benchmark_run("test-run-expire-unknown-id");
}

TEST(BenchmarkRunRetentionTests, InsertingAFifthTerminalRunExpiresTheOldestOne) {
  using namespace std::chrono;
  const auto base = steady_clock::now();

  for (int i = 0; i < 5; ++i) {
    stream_stats::benchmark_run_t run(10);
    run.run_id = "test-run-retention-" + std::to_string(i);
    run.state = stream_stats::benchmark_run_state_e::frozen;
    run.frozen_monotonic = base + seconds(i);  // run 0 is oldest, run 4 is newest.
    stream_stats::insert_benchmark_run(std::move(run));
  }

  // The oldest of the 5 terminal runs must have been evicted (expired) once
  // the 5th was inserted - but expiry leaves a tombstone, so it must still
  // be findable, just no longer frozen.
  bool oldest_visited = false;
  ASSERT_TRUE(stream_stats::with_benchmark_run("test-run-retention-0", [&](stream_stats::benchmark_run_t &r) {
    oldest_visited = true;
    EXPECT_EQ(r.state, stream_stats::benchmark_run_state_e::expired);
  })) << "the tombstone must still be findable after eviction";
  EXPECT_TRUE(oldest_visited);

  for (int i = 1; i < 5; ++i) {
    EXPECT_TRUE(stream_stats::with_benchmark_run("test-run-retention-" + std::to_string(i), [](stream_stats::benchmark_run_t &r) {
      EXPECT_EQ(r.state, stream_stats::benchmark_run_state_e::frozen) << "the 4 newer runs must be untouched";
    }));
  }

  for (int i = 0; i < 5; ++i) {
    stream_stats::erase_benchmark_run("test-run-retention-" + std::to_string(i));
  }
}

TEST(BenchmarkRunRetentionTests, NonTerminalRunsAreNeverEvictedByTheTerminalRetentionLimit) {
  using namespace std::chrono;
  const auto base = steady_clock::now();

  // 5 non-terminal (active) runs - the 4-payload cap only applies to
  // frozen/aborted runs, so none of these should ever be touched by
  // insert-time retention.
  for (int i = 0; i < 5; ++i) {
    stream_stats::benchmark_run_t run(10);
    run.run_id = "test-run-non-terminal-" + std::to_string(i);
    run.state = stream_stats::benchmark_run_state_e::active;
    run.frozen_monotonic = base + seconds(i);  // Set anyway, to prove state (not timestamp presence) gates eviction.
    stream_stats::insert_benchmark_run(std::move(run));
  }

  for (int i = 0; i < 5; ++i) {
    EXPECT_TRUE(stream_stats::with_benchmark_run("test-run-non-terminal-" + std::to_string(i), [](stream_stats::benchmark_run_t &r) {
      EXPECT_EQ(r.state, stream_stats::benchmark_run_state_e::active);
    }));
  }

  for (int i = 0; i < 5; ++i) {
    stream_stats::erase_benchmark_run("test-run-non-terminal-" + std::to_string(i));
  }
}

TEST(BenchmarkRunRetentionTests, ExpireStaleBenchmarkRunsExpiresOnlyRunsPastTheTtl) {
  using namespace std::chrono;

  stream_stats::benchmark_run_t stale_run(10);
  stale_run.run_id = "test-run-stale";
  stale_run.state = stream_stats::benchmark_run_state_e::aborted;
  stale_run.frozen_monotonic = steady_clock::now() - minutes(31);
  stream_stats::insert_benchmark_run(std::move(stale_run));

  stream_stats::benchmark_run_t fresh_run(10);
  fresh_run.run_id = "test-run-fresh";
  fresh_run.state = stream_stats::benchmark_run_state_e::frozen;
  fresh_run.frozen_monotonic = steady_clock::now();
  stream_stats::insert_benchmark_run(std::move(fresh_run));

  stream_stats::expire_stale_benchmark_runs();

  stream_stats::with_benchmark_run("test-run-stale", [](stream_stats::benchmark_run_t &r) {
    EXPECT_EQ(r.state, stream_stats::benchmark_run_state_e::expired);
  });
  stream_stats::with_benchmark_run("test-run-fresh", [](stream_stats::benchmark_run_t &r) {
    EXPECT_EQ(r.state, stream_stats::benchmark_run_state_e::frozen) << "well within the 30-minute TTL, must not expire";
  });

  stream_stats::erase_benchmark_run("test-run-stale");
  stream_stats::erase_benchmark_run("test-run-fresh");
}

// P0-5 benchmark-run-capture engine, piece 3: create_benchmark_run() and its
// create-and-arm preconditions (measurement-spec-v1.md 6.4). Each rejection
// test perturbs exactly one field away from a known-valid baseline request
// so it exercises exactly the precondition it names. Not tested here:
// rejected_nominal_sample_budget_too_small - unreachable given the duration
// (>=60s) and target_fps (>=30) floors already enforced above it, since
// 60*30 = 1800 is always >= the 1000-frame floor. Kept in the engine as a
// direct, defensive expression of the spec's own precondition rather than
// an assumption the range floors can never change independently.

namespace {
  // RAII: pins the control-plane flag to a known state for one test and
  // restores whatever it was before, so an early ASSERT failure (which
  // skips the rest of the test body) can't leak enabled/disabled state
  // into a later test.
  struct BenchmarkControlPlaneGuard {
    explicit BenchmarkControlPlaneGuard(bool enabled):
        previous(stream_stats::benchmark_control_plane_enabled()) {
      stream_stats::set_benchmark_control_plane_enabled(enabled);
    }
    ~BenchmarkControlPlaneGuard() {
      stream_stats::set_benchmark_control_plane_enabled(previous);
    }
    bool previous;
  };

  // Matches measurement-spec-v1.md 6.4's create-and-arm example exactly
  // (120s / 250ms / 2000ms / 120fps / 32768 frames), so nominal_sample_budget
  // (120 * 120 = 14400) sits comfortably inside the valid capacity range.
  stream_stats::benchmark_run_create_request_t make_valid_create_request(const std::string &run_id) {
    stream_stats::benchmark_run_create_request_t request;
    request.run_id = run_id;
    request.manifest_sha256 = std::string(64, 'a');
    request.label = "P0-7-synthetic-A-pair-03";
    request.workload_id = "synthetic-frame-counter-v1";
    request.expected_duration_s = 120;
    request.duration_tolerance_ms = 250;
    request.drain_grace_ms = 2000;
    request.target_fps = 120;
    request.sample_capacity_frames = 32768;
    return request;
  }
}  // namespace

TEST(BenchmarkRunCreateTests, RejectsWhenControlPlaneIsNotEnabled) {
  BenchmarkControlPlaneGuard guard(false);
  stream_stats::add_client("203.0.113.20", "create-test-control-plane-disabled");

  const auto result = stream_stats::create_benchmark_run(
    make_valid_create_request("create-test-control-plane-disabled"),
    "device-control-plane-disabled", 1, true);

  EXPECT_EQ(result, stream_stats::benchmark_run_create_result_e::rejected_control_plane_not_enabled);
  stream_stats::remove_client("203.0.113.20");
}

TEST(BenchmarkRunCreateTests, RejectsWhenCallerIsNotAuthorizedAsHarness) {
  BenchmarkControlPlaneGuard guard(true);
  stream_stats::add_client("203.0.113.21", "create-test-unauthorized");

  const auto result = stream_stats::create_benchmark_run(
    make_valid_create_request("create-test-unauthorized"),
    "device-unauthorized", 1, false);

  EXPECT_EQ(result, stream_stats::benchmark_run_create_result_e::rejected_caller_not_authorized_as_harness);
  stream_stats::remove_client("203.0.113.21");
}

TEST(BenchmarkRunCreateTests, RejectsWhenNoActiveSession) {
  BenchmarkControlPlaneGuard guard(true);

  const auto result = stream_stats::create_benchmark_run(
    make_valid_create_request("create-test-zero-sessions"),
    "device-zero-sessions", 1, true);

  EXPECT_EQ(result, stream_stats::benchmark_run_create_result_e::rejected_not_exactly_one_active_session);
}

TEST(BenchmarkRunCreateTests, RejectsWhenMultipleActiveSessions) {
  BenchmarkControlPlaneGuard guard(true);
  stream_stats::add_client("203.0.113.22", "create-test-multi-session-a");
  stream_stats::add_client("203.0.113.23", "create-test-multi-session-b");

  const auto result = stream_stats::create_benchmark_run(
    make_valid_create_request("create-test-multi-session"),
    "device-multi-session", 1, true);

  EXPECT_EQ(result, stream_stats::benchmark_run_create_result_e::rejected_not_exactly_one_active_session);
  stream_stats::remove_client("203.0.113.22");
  stream_stats::remove_client("203.0.113.23");
}

TEST(BenchmarkRunCreateTests, RejectsDurationOutOfRange) {
  BenchmarkControlPlaneGuard guard(true);
  stream_stats::add_client("203.0.113.24", "create-test-duration-range");

  auto too_short = make_valid_create_request("create-test-duration-too-short");
  too_short.expected_duration_s = 59;
  EXPECT_EQ(stream_stats::create_benchmark_run(too_short, "device-duration-too-short", 1, true),
            stream_stats::benchmark_run_create_result_e::rejected_duration_out_of_range);

  auto too_long = make_valid_create_request("create-test-duration-too-long");
  too_long.expected_duration_s = 181;
  EXPECT_EQ(stream_stats::create_benchmark_run(too_long, "device-duration-too-long", 1, true),
            stream_stats::benchmark_run_create_result_e::rejected_duration_out_of_range);

  stream_stats::remove_client("203.0.113.24");
}

TEST(BenchmarkRunCreateTests, RejectsDurationToleranceOutOfRange) {
  BenchmarkControlPlaneGuard guard(true);
  stream_stats::add_client("203.0.113.25", "create-test-tolerance-range");

  auto negative = make_valid_create_request("create-test-tolerance-negative");
  negative.duration_tolerance_ms = -1;
  EXPECT_EQ(stream_stats::create_benchmark_run(negative, "device-tolerance-negative", 1, true),
            stream_stats::benchmark_run_create_result_e::rejected_duration_tolerance_out_of_range);

  auto too_large = make_valid_create_request("create-test-tolerance-too-large");
  too_large.duration_tolerance_ms = 1001;
  EXPECT_EQ(stream_stats::create_benchmark_run(too_large, "device-tolerance-too-large", 1, true),
            stream_stats::benchmark_run_create_result_e::rejected_duration_tolerance_out_of_range);

  stream_stats::remove_client("203.0.113.25");
}

TEST(BenchmarkRunCreateTests, RejectsDrainGraceOutOfRange) {
  BenchmarkControlPlaneGuard guard(true);
  stream_stats::add_client("203.0.113.26", "create-test-drain-grace-range");

  auto too_small = make_valid_create_request("create-test-drain-grace-too-small");
  too_small.drain_grace_ms = 0;
  EXPECT_EQ(stream_stats::create_benchmark_run(too_small, "device-drain-grace-too-small", 1, true),
            stream_stats::benchmark_run_create_result_e::rejected_drain_grace_out_of_range);

  auto too_large = make_valid_create_request("create-test-drain-grace-too-large");
  too_large.drain_grace_ms = 5001;
  EXPECT_EQ(stream_stats::create_benchmark_run(too_large, "device-drain-grace-too-large", 1, true),
            stream_stats::benchmark_run_create_result_e::rejected_drain_grace_out_of_range);

  stream_stats::remove_client("203.0.113.26");
}

TEST(BenchmarkRunCreateTests, RejectsTargetFpsOutOfRange) {
  BenchmarkControlPlaneGuard guard(true);
  stream_stats::add_client("203.0.113.27", "create-test-fps-range");

  auto too_low = make_valid_create_request("create-test-fps-too-low");
  too_low.target_fps = 29;
  EXPECT_EQ(stream_stats::create_benchmark_run(too_low, "device-fps-too-low", 1, true),
            stream_stats::benchmark_run_create_result_e::rejected_target_fps_out_of_range);

  auto too_high = make_valid_create_request("create-test-fps-too-high");
  too_high.target_fps = 241;
  EXPECT_EQ(stream_stats::create_benchmark_run(too_high, "device-fps-too-high", 1, true),
            stream_stats::benchmark_run_create_result_e::rejected_target_fps_out_of_range);

  stream_stats::remove_client("203.0.113.27");
}

TEST(BenchmarkRunCreateTests, RejectsCapacityBelowNominalBudget) {
  BenchmarkControlPlaneGuard guard(true);
  stream_stats::add_client("203.0.113.28", "create-test-capacity-below-nominal");

  auto request = make_valid_create_request("create-test-capacity-below-nominal");
  request.sample_capacity_frames = (120 * 120) - 1;  // nominal budget minus one frame.
  const auto result = stream_stats::create_benchmark_run(request, "device-capacity-below-nominal", 1, true);

  EXPECT_EQ(result, stream_stats::benchmark_run_create_result_e::rejected_capacity_below_nominal_budget);
  stream_stats::remove_client("203.0.113.28");
}

TEST(BenchmarkRunCreateTests, RejectsCapacityAboveMaximum) {
  BenchmarkControlPlaneGuard guard(true);
  stream_stats::add_client("203.0.113.29", "create-test-capacity-above-maximum");

  auto request = make_valid_create_request("create-test-capacity-above-maximum");
  request.sample_capacity_frames = 65537;
  const auto result = stream_stats::create_benchmark_run(request, "device-capacity-above-maximum", 1, true);

  EXPECT_EQ(result, stream_stats::benchmark_run_create_result_e::rejected_capacity_exceeds_maximum);
  stream_stats::remove_client("203.0.113.29");
}

TEST(BenchmarkRunCreateTests, RejectsInvalidManifestSha256Format) {
  BenchmarkControlPlaneGuard guard(true);
  stream_stats::add_client("203.0.113.30", "create-test-manifest-format");

  auto wrong_length = make_valid_create_request("create-test-manifest-wrong-length");
  wrong_length.manifest_sha256 = "abc123";
  EXPECT_EQ(stream_stats::create_benchmark_run(wrong_length, "device-manifest-wrong-length", 1, true),
            stream_stats::benchmark_run_create_result_e::rejected_invalid_manifest_sha256_format);

  auto uppercase = make_valid_create_request("create-test-manifest-uppercase");
  uppercase.manifest_sha256 = std::string(64, 'A');
  EXPECT_EQ(stream_stats::create_benchmark_run(uppercase, "device-manifest-uppercase", 1, true),
            stream_stats::benchmark_run_create_result_e::rejected_invalid_manifest_sha256_format);

  stream_stats::remove_client("203.0.113.30");
}

TEST(BenchmarkRunCreateTests, RejectsWhenSessionAlreadyHasAnActiveRun) {
  BenchmarkControlPlaneGuard guard(true);
  stream_stats::add_client("203.0.113.31", "create-test-session-already-active");

  const auto first = stream_stats::create_benchmark_run(
    make_valid_create_request("create-test-session-already-active-first"),
    "device-session-already-active", 1, true);
  ASSERT_EQ(first, stream_stats::benchmark_run_create_result_e::created);

  const auto second = stream_stats::create_benchmark_run(
    make_valid_create_request("create-test-session-already-active-second"),
    "device-session-already-active", 1, true);
  EXPECT_EQ(second, stream_stats::benchmark_run_create_result_e::rejected_session_already_has_an_active_run);

  stream_stats::remove_client("203.0.113.31");
}

TEST(BenchmarkRunCreateTests, RejectsRunIdAlreadyUsed) {
  BenchmarkControlPlaneGuard guard(true);
  stream_stats::add_client("203.0.113.32", "create-test-run-id-reused");

  const auto first = stream_stats::create_benchmark_run(
    make_valid_create_request("create-test-run-id-reused"),
    "device-run-id-reused-first", 1, true);
  ASSERT_EQ(first, stream_stats::benchmark_run_create_result_e::created);

  // Different device_uuid, so the session-already-has-an-active-run
  // precondition (scoped to owning_device_uuid) doesn't fire first - this
  // isolates the run_id collision specifically.
  const auto second = stream_stats::create_benchmark_run(
    make_valid_create_request("create-test-run-id-reused"),
    "device-run-id-reused-second", 1, true);
  EXPECT_EQ(second, stream_stats::benchmark_run_create_result_e::rejected_run_id_already_used);

  stream_stats::remove_client("203.0.113.32");
}

TEST(BenchmarkRunCreateTests, CreatesAndArmsWhenAllPreconditionsPass) {
  BenchmarkControlPlaneGuard guard(true);
  stream_stats::add_client("203.0.113.33", "create-test-happy-path");

  const auto revision_at_arm = stream_stats::client_population_revision();
  const auto before = std::chrono::steady_clock::now();
  const auto result = stream_stats::create_benchmark_run(
    make_valid_create_request("create-test-happy-path"),
    "device-happy-path", 7, true);
  const auto after = std::chrono::steady_clock::now();

  ASSERT_EQ(result, stream_stats::benchmark_run_create_result_e::created);

  const bool found = stream_stats::with_benchmark_run("create-test-happy-path",
    [&](stream_stats::benchmark_run_t &run) {
      EXPECT_EQ(run.state, stream_stats::benchmark_run_state_e::armed);
      EXPECT_EQ(run.owning_device_uuid, "device-happy-path");
      EXPECT_EQ(run.owning_session_generation, 7u);
      EXPECT_EQ(run.manifest_sha256, std::string(64, 'a'));
      EXPECT_EQ(run.label, "P0-7-synthetic-A-pair-03");
      EXPECT_EQ(run.workload_id, "synthetic-frame-counter-v1");
      EXPECT_EQ(run.target_fps, 120);
      EXPECT_EQ(run.sample_capacity, 32768u);
      EXPECT_EQ(run.expected_duration_ns, std::chrono::seconds(120));
      EXPECT_EQ(run.duration_tolerance_ns, std::chrono::milliseconds(250));
      EXPECT_EQ(run.drain_grace_ns, std::chrono::milliseconds(2000));
      EXPECT_EQ(run.client_population_revision_at_arm, revision_at_arm);
      EXPECT_GE(run.armed_monotonic, before);
      EXPECT_LE(run.armed_monotonic, after);
    });
  EXPECT_TRUE(found);

  stream_stats::remove_client("203.0.113.33");
}

// P0-5 benchmark-run-capture engine, piece 4: start_benchmark_run(),
// stop_benchmark_run(), get_benchmark_run(), delete_benchmark_run(), and
// the lazy state-reconciliation they all share (measurement-spec-v1.md
// 6.4's active->draining and draining->frozen deadlines, plus the
// session-ended/generation-changed/population-changed abort triggers for
// an already-active-or-draining run). "Lazy" means there is no timer
// thread - reconciliation only happens when something next calls one of
// these four functions for a given run_id, which is why several tests
// below backdate a run's started_monotonic/stopped_monotonic directly
// (the same technique BenchmarkRunRetentionTests uses for frozen_monotonic
// above) rather than actually sleeping past a 60-180s duration in a unit
// test.

namespace {
  // RAII: stands up one active client + one live session-timing entry +
  // one armed benchmark run owned by that session - the trio every
  // start/stop/get/delete test needs, since start_benchmark_run and the
  // abort-trigger check both call get_session_timing(). Control plane must
  // already be enabled (via BenchmarkControlPlaneGuard) before construction,
  // since arming goes through the real create_benchmark_run. Teardown uses
  // the ungated erase_benchmark_run/stop_session_timing/remove_client
  // directly rather than the gated public delete/stop calls under test, so
  // cleanup can't itself be blocked by whatever state a test left behind
  // (e.g. control plane disabled, or the run already deleted).
  struct ArmedRunFixture {
    std::string ip;
    std::string device_uuid;
    std::uint64_t session_generation;
    std::string run_id;

    ArmedRunFixture(std::string ip_in, std::string device_uuid_in, std::uint64_t session_generation_in, std::string run_id_in):
        ip(std::move(ip_in)), device_uuid(std::move(device_uuid_in)),
        session_generation(session_generation_in), run_id(std::move(run_id_in)) {
      stream_stats::add_client(ip, device_uuid);
      stream_stats::start_session_timing(device_uuid, session_generation);
      const auto result = stream_stats::create_benchmark_run(
        make_valid_create_request(run_id), device_uuid, session_generation, true);
      EXPECT_EQ(result, stream_stats::benchmark_run_create_result_e::created);
    }

    ~ArmedRunFixture() {
      stream_stats::erase_benchmark_run(run_id);
      stream_stats::stop_session_timing(device_uuid, session_generation);
      stream_stats::remove_client(ip);
    }
  };

  // The exact lower bound make_valid_create_request()'s 120s/250ms pair
  // implies, per measurement-spec-v1.md 6.4's
  // abs(actual_duration_ns - expected_duration_ns) <= duration_tolerance_ns.
  std::chrono::nanoseconds valid_request_duration_lower_bound() {
    return std::chrono::seconds(120) - std::chrono::milliseconds(250);
  }
}  // namespace

TEST(BenchmarkRunLifecycleTests, StartRejectsWhenControlPlaneIsNotEnabled) {
  BenchmarkControlPlaneGuard guard(true);
  ArmedRunFixture fixture("203.0.113.40", "device-start-cp-disabled", 1, "lifecycle-start-cp-disabled");

  stream_stats::set_benchmark_control_plane_enabled(false);
  const auto result = stream_stats::start_benchmark_run(fixture.run_id, fixture.device_uuid, fixture.session_generation, true);
  EXPECT_EQ(result, stream_stats::benchmark_run_start_result_e::rejected_control_plane_not_enabled);
  stream_stats::set_benchmark_control_plane_enabled(true);
}

TEST(BenchmarkRunLifecycleTests, StartRejectsWhenCallerIsNotAuthorizedAsHarness) {
  BenchmarkControlPlaneGuard guard(true);
  ArmedRunFixture fixture("203.0.113.41", "device-start-unauthorized", 1, "lifecycle-start-unauthorized");

  const auto result = stream_stats::start_benchmark_run(fixture.run_id, fixture.device_uuid, fixture.session_generation, false);
  EXPECT_EQ(result, stream_stats::benchmark_run_start_result_e::rejected_caller_not_authorized_as_harness);
}

TEST(BenchmarkRunLifecycleTests, StartRejectsWhenRunNotFound) {
  BenchmarkControlPlaneGuard guard(true);

  const auto result = stream_stats::start_benchmark_run("lifecycle-start-unknown-run", "device-x", 1, true);
  EXPECT_EQ(result, stream_stats::benchmark_run_start_result_e::rejected_run_not_found);
}

TEST(BenchmarkRunLifecycleTests, StartRejectsWrongSession) {
  BenchmarkControlPlaneGuard guard(true);
  ArmedRunFixture fixture("203.0.113.42", "device-start-wrong-session", 1, "lifecycle-start-wrong-session");

  EXPECT_EQ(stream_stats::start_benchmark_run(fixture.run_id, "some-other-device", fixture.session_generation, true),
            stream_stats::benchmark_run_start_result_e::rejected_wrong_session)
    << "wrong device_uuid";
  EXPECT_EQ(stream_stats::start_benchmark_run(fixture.run_id, fixture.device_uuid, fixture.session_generation + 1, true),
            stream_stats::benchmark_run_start_result_e::rejected_wrong_session)
    << "right device_uuid, wrong session_generation";
}

TEST(BenchmarkRunLifecycleTests, StartRejectsWhenRunNotInArmedState) {
  BenchmarkControlPlaneGuard guard(true);
  ArmedRunFixture fixture("203.0.113.43", "device-start-duplicate", 1, "lifecycle-start-duplicate");

  ASSERT_EQ(stream_stats::start_benchmark_run(fixture.run_id, fixture.device_uuid, fixture.session_generation, true),
            stream_stats::benchmark_run_start_result_e::started);

  const auto second = stream_stats::start_benchmark_run(fixture.run_id, fixture.device_uuid, fixture.session_generation, true);
  EXPECT_EQ(second, stream_stats::benchmark_run_start_result_e::rejected_run_not_in_armed_state)
    << "a duplicate start must be rejected, not silently re-accepted";
}

TEST(BenchmarkRunLifecycleTests, StartRejectsWhenPopulationChangedSinceArm) {
  BenchmarkControlPlaneGuard guard(true);
  ArmedRunFixture fixture("203.0.113.44", "device-start-population-drift", 1, "lifecycle-start-population-drift");

  stream_stats::add_client("203.0.113.45", "lifecycle-start-population-churn");
  stream_stats::remove_client("203.0.113.45");

  const auto result = stream_stats::start_benchmark_run(fixture.run_id, fixture.device_uuid, fixture.session_generation, true);
  EXPECT_EQ(result, stream_stats::benchmark_run_start_result_e::rejected_population_changed_since_arm);
}

TEST(BenchmarkRunLifecycleTests, StartRejectsWhenSessionNoLongerActive) {
  BenchmarkControlPlaneGuard guard(true);
  ArmedRunFixture fixture("203.0.113.46", "device-start-session-ended", 1, "lifecycle-start-session-ended");

  stream_stats::stop_session_timing(fixture.device_uuid, fixture.session_generation);

  const auto result = stream_stats::start_benchmark_run(fixture.run_id, fixture.device_uuid, fixture.session_generation, true);
  EXPECT_EQ(result, stream_stats::benchmark_run_start_result_e::rejected_session_no_longer_active);
}

TEST(BenchmarkRunLifecycleTests, StartSucceedsAndActivatesWhenAllPreconditionsPass) {
  BenchmarkControlPlaneGuard guard(true);
  ArmedRunFixture fixture("203.0.113.47", "device-start-happy-path", 1, "lifecycle-start-happy-path");

  const auto before = std::chrono::steady_clock::now();
  const auto result = stream_stats::start_benchmark_run(fixture.run_id, fixture.device_uuid, fixture.session_generation, true);
  const auto after = std::chrono::steady_clock::now();
  ASSERT_EQ(result, stream_stats::benchmark_run_start_result_e::started);

  const auto get_result = stream_stats::get_benchmark_run(fixture.run_id, true, [&](stream_stats::benchmark_run_t &run) {
    EXPECT_EQ(run.state, stream_stats::benchmark_run_state_e::active);
    ASSERT_TRUE(run.started_monotonic.has_value());
    EXPECT_GE(*run.started_monotonic, before);
    EXPECT_LE(*run.started_monotonic, after);
  });
  EXPECT_EQ(get_result, stream_stats::benchmark_run_get_result_e::found);
}

TEST(BenchmarkRunLifecycleTests, StopRejectsWhenControlPlaneIsNotEnabled) {
  BenchmarkControlPlaneGuard guard(true);
  ArmedRunFixture fixture("203.0.113.48", "device-stop-cp-disabled", 1, "lifecycle-stop-cp-disabled");
  ASSERT_EQ(stream_stats::start_benchmark_run(fixture.run_id, fixture.device_uuid, fixture.session_generation, true),
            stream_stats::benchmark_run_start_result_e::started);

  stream_stats::set_benchmark_control_plane_enabled(false);
  const auto result = stream_stats::stop_benchmark_run(fixture.run_id, fixture.device_uuid, fixture.session_generation, true);
  EXPECT_EQ(result, stream_stats::benchmark_run_stop_result_e::rejected_control_plane_not_enabled);
  stream_stats::set_benchmark_control_plane_enabled(true);
}

TEST(BenchmarkRunLifecycleTests, StopRejectsWhenCallerIsNotAuthorizedAsHarness) {
  BenchmarkControlPlaneGuard guard(true);
  ArmedRunFixture fixture("203.0.113.49", "device-stop-unauthorized", 1, "lifecycle-stop-unauthorized");
  ASSERT_EQ(stream_stats::start_benchmark_run(fixture.run_id, fixture.device_uuid, fixture.session_generation, true),
            stream_stats::benchmark_run_start_result_e::started);

  const auto result = stream_stats::stop_benchmark_run(fixture.run_id, fixture.device_uuid, fixture.session_generation, false);
  EXPECT_EQ(result, stream_stats::benchmark_run_stop_result_e::rejected_caller_not_authorized_as_harness);
}

TEST(BenchmarkRunLifecycleTests, StopRejectsWhenRunNotFound) {
  BenchmarkControlPlaneGuard guard(true);

  const auto result = stream_stats::stop_benchmark_run("lifecycle-stop-unknown-run", "device-x", 1, true);
  EXPECT_EQ(result, stream_stats::benchmark_run_stop_result_e::rejected_run_not_found);
}

TEST(BenchmarkRunLifecycleTests, StopRejectsWrongSession) {
  BenchmarkControlPlaneGuard guard(true);
  ArmedRunFixture fixture("203.0.113.50", "device-stop-wrong-session", 1, "lifecycle-stop-wrong-session");
  ASSERT_EQ(stream_stats::start_benchmark_run(fixture.run_id, fixture.device_uuid, fixture.session_generation, true),
            stream_stats::benchmark_run_start_result_e::started);

  const auto result = stream_stats::stop_benchmark_run(fixture.run_id, "some-other-device", fixture.session_generation, true);
  EXPECT_EQ(result, stream_stats::benchmark_run_stop_result_e::rejected_wrong_session);
}

TEST(BenchmarkRunLifecycleTests, StopRejectsWhenNotCurrentlyActive) {
  BenchmarkControlPlaneGuard guard(true);
  ArmedRunFixture fixture("203.0.113.51", "device-stop-not-active", 1, "lifecycle-stop-not-active");

  const auto stop_while_armed = stream_stats::stop_benchmark_run(fixture.run_id, fixture.device_uuid, fixture.session_generation, true);
  EXPECT_EQ(stop_while_armed, stream_stats::benchmark_run_stop_result_e::rejected_not_currently_active)
    << "never started";

  ASSERT_EQ(stream_stats::start_benchmark_run(fixture.run_id, fixture.device_uuid, fixture.session_generation, true),
            stream_stats::benchmark_run_start_result_e::started);
  // Real elapsed time here is microseconds, well before the 119.75s lower
  // bound, so this first stop is correctly an early abort, not a plain
  // stop - StopAbortsWhenCalledBeforeTheDurationLowerBound covers that
  // path directly. What this test cares about is that state is no longer
  // active either way, so the second call below must still be rejected.
  ASSERT_EQ(stream_stats::stop_benchmark_run(fixture.run_id, fixture.device_uuid, fixture.session_generation, true),
            stream_stats::benchmark_run_stop_result_e::stopped_early_and_aborted);

  const auto duplicate_stop = stream_stats::stop_benchmark_run(fixture.run_id, fixture.device_uuid, fixture.session_generation, true);
  EXPECT_EQ(duplicate_stop, stream_stats::benchmark_run_stop_result_e::rejected_not_currently_active)
    << "a duplicate stop must be rejected, not silently re-accepted";
}

TEST(BenchmarkRunLifecycleTests, StopAbortsWhenCalledBeforeTheDurationLowerBound) {
  BenchmarkControlPlaneGuard guard(true);
  ArmedRunFixture fixture("203.0.113.52", "device-stop-early", 1, "lifecycle-stop-early");
  ASSERT_EQ(stream_stats::start_benchmark_run(fixture.run_id, fixture.device_uuid, fixture.session_generation, true),
            stream_stats::benchmark_run_start_result_e::started);

  const auto result = stream_stats::stop_benchmark_run(fixture.run_id, fixture.device_uuid, fixture.session_generation, true);
  EXPECT_EQ(result, stream_stats::benchmark_run_stop_result_e::stopped_early_and_aborted)
    << "this test stops within milliseconds of starting, nowhere near the 119.75s lower bound";

  const auto get_result = stream_stats::get_benchmark_run(fixture.run_id, true, [](stream_stats::benchmark_run_t &run) {
    EXPECT_EQ(run.state, stream_stats::benchmark_run_state_e::aborted);
    EXPECT_EQ(run.abort_reason, stream_stats::benchmark_abort_reason_e::stopped_before_duration_lower_bound);
  });
  EXPECT_EQ(get_result, stream_stats::benchmark_run_get_result_e::found);
}

TEST(BenchmarkRunLifecycleTests, StopTransitionsToDrainingWhenCalledExactlyAtTheDurationLowerBound) {
  BenchmarkControlPlaneGuard guard(true);
  ArmedRunFixture fixture("203.0.113.53", "device-stop-at-lower-bound", 1, "lifecycle-stop-at-lower-bound");
  ASSERT_EQ(stream_stats::start_benchmark_run(fixture.run_id, fixture.device_uuid, fixture.session_generation, true),
            stream_stats::benchmark_run_start_result_e::started);

  // Backdate started_monotonic so "elapsed" already equals the lower bound
  // exactly, without sleeping through a 60-180s duration in a unit test -
  // the same technique BenchmarkRunRetentionTests uses on frozen_monotonic.
  stream_stats::with_benchmark_run(fixture.run_id, [](stream_stats::benchmark_run_t &run) {
    run.started_monotonic = std::chrono::steady_clock::now() - valid_request_duration_lower_bound();
  });

  const auto result = stream_stats::stop_benchmark_run(fixture.run_id, fixture.device_uuid, fixture.session_generation, true);
  EXPECT_EQ(result, stream_stats::benchmark_run_stop_result_e::stopped)
    << "exactly at the lower bound must be accepted, not aborted";

  const auto get_result = stream_stats::get_benchmark_run(fixture.run_id, true, [](stream_stats::benchmark_run_t &run) {
    EXPECT_EQ(run.state, stream_stats::benchmark_run_state_e::draining);
  });
  EXPECT_EQ(get_result, stream_stats::benchmark_run_get_result_e::found);
}

TEST(BenchmarkRunLifecycleTests, GetRejectsWhenControlPlaneIsNotEnabled) {
  BenchmarkControlPlaneGuard guard(true);
  ArmedRunFixture fixture("203.0.113.54", "device-get-cp-disabled", 1, "lifecycle-get-cp-disabled");

  stream_stats::set_benchmark_control_plane_enabled(false);
  const auto result = stream_stats::get_benchmark_run(fixture.run_id, true, [](stream_stats::benchmark_run_t &) {
    FAIL() << "callback must not run when the control plane is disabled";
  });
  EXPECT_EQ(result, stream_stats::benchmark_run_get_result_e::rejected_control_plane_not_enabled);
  stream_stats::set_benchmark_control_plane_enabled(true);
}

TEST(BenchmarkRunLifecycleTests, GetRejectsWhenCallerIsNotAuthorizedAsHarness) {
  BenchmarkControlPlaneGuard guard(true);
  ArmedRunFixture fixture("203.0.113.55", "device-get-unauthorized", 1, "lifecycle-get-unauthorized");

  const auto result = stream_stats::get_benchmark_run(fixture.run_id, false, [](stream_stats::benchmark_run_t &) {
    FAIL() << "callback must not run for an unauthorized caller";
  });
  EXPECT_EQ(result, stream_stats::benchmark_run_get_result_e::rejected_caller_not_authorized_as_harness);
}

TEST(BenchmarkRunLifecycleTests, GetRejectsWhenRunNotFound) {
  BenchmarkControlPlaneGuard guard(true);

  const auto result = stream_stats::get_benchmark_run("lifecycle-get-unknown-run", true, [](stream_stats::benchmark_run_t &) {
    FAIL() << "callback must not run for an unknown run_id";
  });
  EXPECT_EQ(result, stream_stats::benchmark_run_get_result_e::rejected_run_not_found);
}

TEST(BenchmarkRunLifecycleTests, GetFindsAnArmedRunAndAppliesNoTransition) {
  BenchmarkControlPlaneGuard guard(true);
  ArmedRunFixture fixture("203.0.113.56", "device-get-armed", 1, "lifecycle-get-armed");

  const auto result = stream_stats::get_benchmark_run(fixture.run_id, true, [](stream_stats::benchmark_run_t &run) {
    EXPECT_EQ(run.state, stream_stats::benchmark_run_state_e::armed);
  });
  EXPECT_EQ(result, stream_stats::benchmark_run_get_result_e::found);
}

TEST(BenchmarkRunLifecycleTests, GetAppliesLazyTransitionFromActiveToDrainingBeforeReturning) {
  BenchmarkControlPlaneGuard guard(true);
  ArmedRunFixture fixture("203.0.113.57", "device-get-lazy-draining", 1, "lifecycle-get-lazy-draining");
  ASSERT_EQ(stream_stats::start_benchmark_run(fixture.run_id, fixture.device_uuid, fixture.session_generation, true),
            stream_stats::benchmark_run_start_result_e::started);

  // The full expected duration has "already elapsed" but drain grace has
  // not - stop_benchmark_run is never called, so this is purely GET
  // noticing the deadline on its own.
  stream_stats::with_benchmark_run(fixture.run_id, [](stream_stats::benchmark_run_t &run) {
    run.started_monotonic = std::chrono::steady_clock::now() - std::chrono::seconds(120);
  });

  const auto result = stream_stats::get_benchmark_run(fixture.run_id, true, [](stream_stats::benchmark_run_t &run) {
    EXPECT_EQ(run.state, stream_stats::benchmark_run_state_e::draining);
  });
  EXPECT_EQ(result, stream_stats::benchmark_run_get_result_e::found);
}

TEST(BenchmarkRunLifecycleTests, GetCascadesLazyTransitionAllTheWayToFrozen) {
  BenchmarkControlPlaneGuard guard(true);
  ArmedRunFixture fixture("203.0.113.58", "device-get-lazy-frozen", 1, "lifecycle-get-lazy-frozen");
  ASSERT_EQ(stream_stats::start_benchmark_run(fixture.run_id, fixture.device_uuid, fixture.session_generation, true),
            stream_stats::benchmark_run_start_result_e::started);

  const auto revision_before_freeze = stream_stats::client_population_revision();

  // Both the full duration AND the drain grace have "already elapsed" -
  // a single get() call must cascade active -> draining -> frozen, not
  // require two separate lazy checks to fully resolve.
  stream_stats::with_benchmark_run(fixture.run_id, [](stream_stats::benchmark_run_t &run) {
    run.started_monotonic = std::chrono::steady_clock::now() - std::chrono::seconds(123);
  });

  const auto result = stream_stats::get_benchmark_run(fixture.run_id, true, [&](stream_stats::benchmark_run_t &run) {
    EXPECT_EQ(run.state, stream_stats::benchmark_run_state_e::frozen);
    EXPECT_TRUE(run.frozen_monotonic.has_value());
    EXPECT_EQ(run.client_population_revision_at_freeze, revision_before_freeze);
  });
  EXPECT_EQ(result, stream_stats::benchmark_run_get_result_e::found);
}

TEST(BenchmarkRunLifecycleTests, GetObservesAbortWhenSessionEndedMidRun) {
  BenchmarkControlPlaneGuard guard(true);
  ArmedRunFixture fixture("203.0.113.59", "device-get-abort-session-ended", 1, "lifecycle-get-abort-session-ended");
  ASSERT_EQ(stream_stats::start_benchmark_run(fixture.run_id, fixture.device_uuid, fixture.session_generation, true),
            stream_stats::benchmark_run_start_result_e::started);

  stream_stats::stop_session_timing(fixture.device_uuid, fixture.session_generation);

  const auto result = stream_stats::get_benchmark_run(fixture.run_id, true, [](stream_stats::benchmark_run_t &run) {
    EXPECT_EQ(run.state, stream_stats::benchmark_run_state_e::aborted);
    EXPECT_EQ(run.abort_reason, stream_stats::benchmark_abort_reason_e::session_ended);
  });
  EXPECT_EQ(result, stream_stats::benchmark_run_get_result_e::found);
}

TEST(BenchmarkRunLifecycleTests, GetObservesAbortWhenPopulationChangedMidRun) {
  BenchmarkControlPlaneGuard guard(true);
  ArmedRunFixture fixture("203.0.113.60", "device-get-abort-population", 1, "lifecycle-get-abort-population");
  ASSERT_EQ(stream_stats::start_benchmark_run(fixture.run_id, fixture.device_uuid, fixture.session_generation, true),
            stream_stats::benchmark_run_start_result_e::started);

  stream_stats::add_client("203.0.113.61", "lifecycle-get-abort-population-churn");
  stream_stats::remove_client("203.0.113.61");

  const auto result = stream_stats::get_benchmark_run(fixture.run_id, true, [](stream_stats::benchmark_run_t &run) {
    EXPECT_EQ(run.state, stream_stats::benchmark_run_state_e::aborted);
    EXPECT_EQ(run.abort_reason, stream_stats::benchmark_abort_reason_e::client_population_revision_changed);
  });
  EXPECT_EQ(result, stream_stats::benchmark_run_get_result_e::found);
}

TEST(BenchmarkRunLifecycleTests, GetObservesAbortWhenSessionGenerationChangedMidRun) {
  BenchmarkControlPlaneGuard guard(true);
  ArmedRunFixture fixture("203.0.113.62", "device-get-abort-generation", 1, "lifecycle-get-abort-generation");
  ASSERT_EQ(stream_stats::start_benchmark_run(fixture.run_id, fixture.device_uuid, fixture.session_generation, true),
            stream_stats::benchmark_run_start_result_e::started);

  // Simulates the same device reconnecting mid-run and claiming a new
  // generation - start_session_timing() discards and replaces any existing
  // entry for this uuid, same as a real reconnect would.
  stream_stats::start_session_timing(fixture.device_uuid, fixture.session_generation + 1);

  const auto result = stream_stats::get_benchmark_run(fixture.run_id, true, [](stream_stats::benchmark_run_t &run) {
    EXPECT_EQ(run.state, stream_stats::benchmark_run_state_e::aborted);
    EXPECT_EQ(run.abort_reason, stream_stats::benchmark_abort_reason_e::session_generation_changed);
  });
  EXPECT_EQ(result, stream_stats::benchmark_run_get_result_e::found);

  // The fixture's own teardown stops session_generation, which the newer
  // generation registered above has already displaced - clean that one up
  // too so it doesn't leak into a later test.
  stream_stats::stop_session_timing(fixture.device_uuid, fixture.session_generation + 1);
}

TEST(BenchmarkRunLifecycleTests, DeleteRejectsWhenControlPlaneIsNotEnabled) {
  BenchmarkControlPlaneGuard guard(true);
  ArmedRunFixture fixture("203.0.113.63", "device-delete-cp-disabled", 1, "lifecycle-delete-cp-disabled");

  stream_stats::set_benchmark_control_plane_enabled(false);
  const auto result = stream_stats::delete_benchmark_run(fixture.run_id, true);
  EXPECT_EQ(result, stream_stats::benchmark_run_delete_result_e::rejected_control_plane_not_enabled);
  stream_stats::set_benchmark_control_plane_enabled(true);
}

TEST(BenchmarkRunLifecycleTests, DeleteRejectsWhenCallerIsNotAuthorizedAsHarness) {
  BenchmarkControlPlaneGuard guard(true);
  ArmedRunFixture fixture("203.0.113.64", "device-delete-unauthorized", 1, "lifecycle-delete-unauthorized");

  const auto result = stream_stats::delete_benchmark_run(fixture.run_id, false);
  EXPECT_EQ(result, stream_stats::benchmark_run_delete_result_e::rejected_caller_not_authorized_as_harness);
}

TEST(BenchmarkRunLifecycleTests, DeleteRejectsWhenRunNotFound) {
  BenchmarkControlPlaneGuard guard(true);

  const auto result = stream_stats::delete_benchmark_run("lifecycle-delete-unknown-run", true);
  EXPECT_EQ(result, stream_stats::benchmark_run_delete_result_e::rejected_run_not_found);
}

TEST(BenchmarkRunLifecycleTests, DeleteRemovesAnArmedRunImmediatelyAndLeavesNoTombstone) {
  BenchmarkControlPlaneGuard guard(true);
  ArmedRunFixture fixture("203.0.113.65", "device-delete-armed", 1, "lifecycle-delete-armed");

  ASSERT_EQ(stream_stats::delete_benchmark_run(fixture.run_id, true), stream_stats::benchmark_run_delete_result_e::deleted);

  const auto get_result = stream_stats::get_benchmark_run(fixture.run_id, true, [](stream_stats::benchmark_run_t &) {
    FAIL() << "deleted runs must leave no tombstone, unlike expiry";
  });
  EXPECT_EQ(get_result, stream_stats::benchmark_run_get_result_e::rejected_run_not_found);
}

TEST(BenchmarkRunLifecycleTests, DeleteWorksRegardlessOfRunState) {
  BenchmarkControlPlaneGuard guard(true);
  ArmedRunFixture fixture("203.0.113.66", "device-delete-active", 1, "lifecycle-delete-active");
  ASSERT_EQ(stream_stats::start_benchmark_run(fixture.run_id, fixture.device_uuid, fixture.session_generation, true),
            stream_stats::benchmark_run_start_result_e::started);

  const auto result = stream_stats::delete_benchmark_run(fixture.run_id, true);
  EXPECT_EQ(result, stream_stats::benchmark_run_delete_result_e::deleted)
    << "delete must not require a terminal state first";
}

// P0-5 benchmark-run-capture engine, piece 5 (final): record_benchmark_sample(),
// the hot-path recorder wired into stream.cpp's send thread right alongside
// record_frame_timing() (same call site, same five arguments, same already-
// taken T0/T1/T2 timestamps - see stream.cpp's video packet send path).
// Reuses ArmedRunFixture/BenchmarkControlPlaneGuard/make_valid_create_request
// from the piece 3/4 tests above.

TEST(BenchmarkRunHotPathTests, RecordIsANoOpWhenControlPlaneIsNotEnabled) {
  BenchmarkControlPlaneGuard guard(true);
  ArmedRunFixture fixture("203.0.113.70", "device-hotpath-cp-disabled", 1, "hotpath-cp-disabled");
  ASSERT_EQ(stream_stats::start_benchmark_run(fixture.run_id, fixture.device_uuid, fixture.session_generation, true),
            stream_stats::benchmark_run_start_result_e::started);

  stream_stats::set_benchmark_control_plane_enabled(false);
  const auto now = std::chrono::steady_clock::now();
  stream_stats::record_benchmark_sample(fixture.device_uuid, fixture.session_generation,
                                         now, now + std::chrono::milliseconds(5), now + std::chrono::milliseconds(9));
  stream_stats::set_benchmark_control_plane_enabled(true);

  const auto result = stream_stats::get_benchmark_run(fixture.run_id, true, [](stream_stats::benchmark_run_t &run) {
    EXPECT_EQ(run.capture_to_encode.accepted_count, 0u);
  });
  EXPECT_EQ(result, stream_stats::benchmark_run_get_result_e::found);
}

TEST(BenchmarkRunHotPathTests, RecordIsANoOpWhenNoMatchingRunExists) {
  BenchmarkControlPlaneGuard guard(true);

  const auto now = std::chrono::steady_clock::now();
  stream_stats::record_benchmark_sample("device-hotpath-no-run", 1,
                                         now, now + std::chrono::milliseconds(5), now + std::chrono::milliseconds(9));
  // Nothing to assert beyond "this did not crash" - there is no run to
  // inspect, which is the point of this test.
}

TEST(BenchmarkRunHotPathTests, RecordIsANoOpForAnArmedRunThatHasNotStarted) {
  BenchmarkControlPlaneGuard guard(true);
  ArmedRunFixture fixture("203.0.113.71", "device-hotpath-armed", 1, "hotpath-armed");

  const auto now = std::chrono::steady_clock::now();
  stream_stats::record_benchmark_sample(fixture.device_uuid, fixture.session_generation,
                                         now, now + std::chrono::milliseconds(5), now + std::chrono::milliseconds(9));

  const auto result = stream_stats::get_benchmark_run(fixture.run_id, true, [](stream_stats::benchmark_run_t &run) {
    EXPECT_EQ(run.state, stream_stats::benchmark_run_state_e::armed);
    EXPECT_EQ(run.capture_to_encode.accepted_count, 0u);
  });
  EXPECT_EQ(result, stream_stats::benchmark_run_get_result_e::found);
}

TEST(BenchmarkRunHotPathTests, RecordIsANoOpForWrongSessionGeneration) {
  BenchmarkControlPlaneGuard guard(true);
  ArmedRunFixture fixture("203.0.113.72", "device-hotpath-wrong-generation", 1, "hotpath-wrong-generation");
  ASSERT_EQ(stream_stats::start_benchmark_run(fixture.run_id, fixture.device_uuid, fixture.session_generation, true),
            stream_stats::benchmark_run_start_result_e::started);

  const auto now = std::chrono::steady_clock::now();
  stream_stats::record_benchmark_sample(fixture.device_uuid, fixture.session_generation + 1,
                                         now, now + std::chrono::milliseconds(5), now + std::chrono::milliseconds(9));

  const auto result = stream_stats::get_benchmark_run(fixture.run_id, true, [](stream_stats::benchmark_run_t &run) {
    EXPECT_EQ(run.capture_to_encode.accepted_count, 0u)
      << "a write from a stale/foreign generation must be silently dropped, same as record_frame_timing";
  });
  EXPECT_EQ(result, stream_stats::benchmark_run_get_result_e::found);
}

TEST(BenchmarkRunHotPathTests, RecordAcceptsAWithinWindowSampleOnAnActiveRun) {
  BenchmarkControlPlaneGuard guard(true);
  ArmedRunFixture fixture("203.0.113.73", "device-hotpath-active", 1, "hotpath-active");
  ASSERT_EQ(stream_stats::start_benchmark_run(fixture.run_id, fixture.device_uuid, fixture.session_generation, true),
            stream_stats::benchmark_run_start_result_e::started);

  const auto t0 = std::chrono::steady_clock::now();
  const auto t1 = t0 + std::chrono::milliseconds(5);
  const auto t2 = t0 + std::chrono::milliseconds(9);
  stream_stats::record_benchmark_sample(fixture.device_uuid, fixture.session_generation, t0, t1, t2);

  const auto result = stream_stats::get_benchmark_run(fixture.run_id, true, [](stream_stats::benchmark_run_t &run) {
    EXPECT_EQ(run.capture_to_encode.accepted_count, 1u);
    EXPECT_EQ(run.encode_to_send_release.accepted_count, 1u);
    EXPECT_EQ(run.capture_to_send_release.accepted_count, 1u);
    ASSERT_EQ(run.capture_to_send_release.duration_us.size(), 1u);
    EXPECT_EQ(run.capture_to_send_release.duration_us[0], 9000u)
      << "T0->T2 duration must be the full 9ms in microseconds";
  });
  EXPECT_EQ(result, stream_stats::benchmark_run_get_result_e::found);
}

TEST(BenchmarkRunHotPathTests, RecordDuringDrainAcceptsInWindowStartsAndExcludesPostWindowCompletions) {
  BenchmarkControlPlaneGuard guard(true);
  ArmedRunFixture fixture("203.0.113.74", "device-hotpath-drain", 1, "hotpath-drain");
  ASSERT_EQ(stream_stats::start_benchmark_run(fixture.run_id, fixture.device_uuid, fixture.session_generation, true),
            stream_stats::benchmark_run_start_result_e::started);

  // Put the run in draining with plenty of drain grace left, without
  // touching real wall-clock time - the same backdating technique used
  // throughout BenchmarkRunLifecycleTests above. started_monotonic is set
  // 10s in the past purely so the synthetic T0/T1/T2 offsets below (all
  // ~120s past it) land on real steady_clock::time_points; the hot-path
  // recorder only ever computes offsets relative to started_monotonic, so
  // this has no effect on the classification math itself.
  const auto backdated_start = std::chrono::steady_clock::now() - std::chrono::seconds(10);
  stream_stats::with_benchmark_run(fixture.run_id, [&](stream_stats::benchmark_run_t &run) {
    run.started_monotonic = backdated_start;
    run.stopped_monotonic = backdated_start + std::chrono::seconds(120);  // = started + expected_duration_ns
    run.state = stream_stats::benchmark_run_state_e::draining;
  });

  // T0/T1 both land before E=120s; T2 lands just after it - exactly the
  // "in flight when the deadline hit" scenario drain grace exists for.
  const auto t0 = backdated_start + std::chrono::milliseconds(119900);
  const auto t1 = backdated_start + std::chrono::milliseconds(119950);
  const auto t2 = backdated_start + std::chrono::milliseconds(120100);
  stream_stats::record_benchmark_sample(fixture.device_uuid, fixture.session_generation, t0, t1, t2);

  const auto result = stream_stats::get_benchmark_run(fixture.run_id, true, [](stream_stats::benchmark_run_t &run) {
    ASSERT_EQ(run.state, stream_stats::benchmark_run_state_e::draining)
      << "stopped_monotonic (60s+ in the future) must not have let this cascade to frozen yet";

    EXPECT_EQ(run.capture_to_encode.accepted_count, 1u)
      << "T0->T1: both endpoints inside the window";

    EXPECT_EQ(run.encode_to_send_release.accepted_count, 0u);
    EXPECT_EQ(run.encode_to_send_release.excluded_completed_after_window, 1u)
      << "T1->T2: started inside the window, completed at/after E - recorded, not silently dropped, but not admitted either";

    EXPECT_EQ(run.capture_to_send_release.accepted_count, 0u);
    EXPECT_EQ(run.capture_to_send_release.excluded_completed_after_window, 1u)
      << "T0->T2: same shape as T1->T2 above";
  });
  EXPECT_EQ(result, stream_stats::benchmark_run_get_result_e::found);
}
