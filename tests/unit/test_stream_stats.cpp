/**
 * @file tests/unit/test_stream_stats.cpp
 * @brief Test src/stream_stats.*.
 */

#include <src/stream_stats.h>
#include <src/config.h>
#include <src/doctor_actions.h>
#include <src/adaptive_bitrate.h>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
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

  nlohmann::json trusted_doctor_action_request(
      const doctor_actions::recovery_action_context_t &context) {
    const auto health = context.health.is_object() ?
      context.health : nlohmann::json::object();
    auto doctor = stream_stats::build_doctor_json(
      context.stats, health, context.app_uuid
    );
    const auto controller = adaptive_bitrate::get_doctor_state();
    stream_stats::bind_doctor_action_scope(
      doctor,
      context.launch_instance_id,
      context.session_generation,
      controller.action_authority_revision,
      context.stats.network_sample_revision,
      context.stats.video_sample_revision
    );
    auto payload = doctor.at("safe_recovery_action").at("payload_preview");
    static std::uint64_t request_sequence = 0;
    payload["request_id"] = "test-doctor-request-" +
      std::to_string(++request_sequence);
    return payload;
  }

  template<typename Callable>
  nlohmann::json execute_with_encoder_ack(int expected_bitrate_kbps,
                                          Callable &&callable) {
    std::atomic<bool> acknowledged {false};
    std::thread encoder([&] {
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
      while (std::chrono::steady_clock::now() < deadline) {
        if (const auto request = adaptive_bitrate::get_live_bitrate_request();
            request && request->target_bitrate_kbps == expected_bitrate_kbps) {
          adaptive_bitrate::acknowledge_live_bitrate_applied(
            request->revision,
            request->target_bitrate_kbps
          );
          acknowledged.store(true, std::memory_order_release);
          return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      }
    });

    auto result = callable();
    encoder.join();
    EXPECT_TRUE(acknowledged.load(std::memory_order_acquire))
      << "The test encoder did not observe the expected "
      << expected_bitrate_kbps << " kbps request";
    return result;
  }
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
  stats.runtime_reported_refresh_hz = 120.0;
  stats.capture_source_fps = 119.7;
  stats.capture_pacing = "source_driven";
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
  EXPECT_DOUBLE_EQ(json.at("runtime_reported_refresh_hz"), 120.0);
  EXPECT_DOUBLE_EQ(json.at("capture_source_fps"), 119.7);
  EXPECT_EQ(json.at("capture_pacing"), "source_driven");
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
  stats.input_steam_input_status = "xbox_opt_in";
  stats.input_steam_profiles_checked = 2;
  stats.input_steam_profiles_with_xbox_support = 1;
  stats.input_steam_forced_app_count = 0;
  stats.input_steam_input_detail = "Steam Input is opted in for Xbox controllers in 1 local profile(s).";
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
  EXPECT_EQ(input.at("steam_input_status"), "xbox_opt_in");
  EXPECT_EQ(input.at("steam_profiles_checked"), 2);
  EXPECT_EQ(input.at("steam_profiles_with_xbox_support"), 1);
  EXPECT_EQ(input.at("steam_forced_app_count"), 0);
  EXPECT_EQ(input.at("steam_input_detail"), "Steam Input is opted in for Xbox controllers in 1 local profile(s).");
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

TEST(StreamStatsCapturePathTests, CaptureFallbackTransitionsInvalidateDoctorAuthorityOnce) {
  stream_stats::update_stream_active(false);
  adaptive_bitrate::reset();

  stream_stats::update_capture_metadata(platf::frame_metadata_t {
    .transport = platf::frame_transport_e::dmabuf,
    .residency = platf::frame_residency_e::gpu,
  });
  stream_stats::update_encode_path_metadata(
    "/dev/dri/renderD128",
    platf::frame_residency_e::gpu,
    platf::frame_format_e::nv12
  );
  const auto gpu_revision = adaptive_bitrate::get_doctor_state().revision;
  EXPECT_FALSE(adaptive_bitrate::doctor_policy_blocks_quality_restore());

  stream_stats::update_capture_metadata(platf::frame_metadata_t {
    .transport = platf::frame_transport_e::dmabuf,
    .residency = platf::frame_residency_e::gpu,
  });
  EXPECT_EQ(adaptive_bitrate::get_doctor_state().revision, gpu_revision);

  stream_stats::update_capture_metadata(platf::frame_metadata_t {
    .transport = platf::frame_transport_e::shm,
    .residency = platf::frame_residency_e::cpu,
  });
  const auto fallback_revision = adaptive_bitrate::get_doctor_state().revision;
  EXPECT_GT(fallback_revision, gpu_revision);
  EXPECT_TRUE(adaptive_bitrate::doctor_policy_blocks_quality_restore());
  const auto fallback_stats = stream_stats::get_current();
  EXPECT_EQ(fallback_stats.capture_transport, platf::frame_transport_e::shm);
  EXPECT_EQ(fallback_stats.capture_residency, platf::frame_residency_e::cpu);

  stream_stats::update_capture_metadata(platf::frame_metadata_t {
    .transport = platf::frame_transport_e::dmabuf,
    .residency = platf::frame_residency_e::gpu,
  });
  const auto recovered_revision = adaptive_bitrate::get_doctor_state().revision;
  EXPECT_GT(recovered_revision, fallback_revision);
  EXPECT_FALSE(adaptive_bitrate::doctor_policy_blocks_quality_restore());

  stream_stats::update_encode_path_metadata(
    "cpu",
    platf::frame_residency_e::cpu,
    platf::frame_format_e::nv12
  );
  EXPECT_GT(adaptive_bitrate::get_doctor_state().revision, recovered_revision);
  EXPECT_TRUE(adaptive_bitrate::doctor_policy_blocks_quality_restore());

  stream_stats::update_stream_active(false);
  adaptive_bitrate::reset();
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

TEST(StreamStatsDoctorTests, SteamInputFindingSurvivesTheEndOfTheStream) {
  // The conflict is host state, and its one-click fix is only safe with Steam
  // closed -- which is to say, once the stream has ended. Wiping these fields
  // with the rest of the session made the finding disappear at exactly the
  // moment it became actionable.
  stream_stats::update_stream_active(true, "client", "10.0.0.5");
  stream_stats::update_controller_input_state(true, 0, "xone", "", "strict_bwrap", "strict isolation active", true, "");
  stream_stats::update_steam_input_state("xbox_opt_in", 1, 1, 0, "Steam Input is opted in for Xbox controllers in 1 local profile(s).");

  stream_stats::update_stream_active(false, "", "");

  const auto after = stream_stats::get_current();
  EXPECT_FALSE(after.streaming);
  EXPECT_EQ(after.input_host_controller_isolation, "strict_bwrap");
  EXPECT_EQ(after.input_steam_input_status, "xbox_opt_in");
  EXPECT_EQ(after.input_steam_profiles_with_xbox_support, 1);

  // The idle Doctor payload still reports the finding, but this release exposes
  // read-only manual guidance rather than a host mutation.
  const auto doctor = stream_stats::build_doctor_json(after, nlohmann::json::object());
  EXPECT_EQ(doctor.at("primary_issue"), "steam_input_conflict");
  EXPECT_EQ(doctor.at("safe_recovery_action").at("id"), "none");
  EXPECT_EQ(doctor.at("safe_recovery_action").at("kind"), "manual_guidance");

  // Session telemetry is still gone; only the host facts carry over.
  EXPECT_EQ(after.client_name, "");
  EXPECT_DOUBLE_EQ(after.encode_time_ms, 0.0);
}

TEST(StreamStatsDoctorTests, LegacySteamVdfActionFailsClosedReadOnly) {
  const auto result = doctor_actions::execute({{"action_id", "disable_steam_input_xbox"}});
  EXPECT_FALSE(result.at("status").get<bool>());
  EXPECT_FALSE(result.at("changed").get<bool>());
  EXPECT_EQ(result.at("state"), "read_only");
}

TEST(StreamStatsDoctorTests, WarnsWhenSteamInputConflictsWithStrictIsolation) {
  stream_stats::stats_t stats {};
  stats.streaming = true;
  stats.capture_transport = platf::frame_transport_e::dmabuf;
  stats.capture_residency = platf::frame_residency_e::gpu;
  stats.encode_target_residency = platf::frame_residency_e::gpu;
  stats.encode_time_ms = 4.0;
  stats.input_host_controller_isolation = "strict_bwrap";
  stats.input_steam_input_status = "xbox_opt_in_and_per_app_forced";
  stats.input_steam_profiles_checked = 2;
  stats.input_steam_profiles_with_xbox_support = 1;
  stats.input_steam_forced_app_count = 2;
  stats.input_steam_input_detail =
    "Steam Input is opted in for Xbox controllers in 1 local profile(s), and 2 app override(s) force Steam Input on.";

  const auto doctor = stream_stats::build_doctor_json(
    stats,
    {{"primary_issue", "steady"}, {"grade", "good"}}
  );

  EXPECT_EQ(doctor.at("primary_issue"), "steam_input_conflict");
  EXPECT_EQ(doctor.at("traffic_light"), "amber");
  EXPECT_EQ(doctor.at("status"), "needs_action");
  EXPECT_EQ(doctor.at("confidence").at("level"), "high");
  EXPECT_EQ(doctor.at("recommendation").at("next_step_label"), "Adjust Steam Input");
  // High-confidence evidence remains, but the release action is manual only.
  const auto &action = doctor.at("safe_recovery_action");
  EXPECT_EQ(action.at("id"), "none");
  EXPECT_EQ(action.at("kind"), "manual_guidance");
  EXPECT_FALSE(action.at("destructive"));
  EXPECT_FALSE(action.at("requires_confirmation"));
  EXPECT_FALSE(action.at("requires_owner"));
  EXPECT_FALSE(action.at("undo").at("supported"));
  EXPECT_EQ(action.at("endpoint"), "");
  EXPECT_FALSE(action.at("payload_preview").contains("action_id"));
  EXPECT_EQ(
    doctor.at("advanced_evidence").at("controller_input").at("steam_forced_app_count"),
    2
  );
  ASSERT_EQ(doctor.at("advanced_evidence").at("recent_issue_codes").size(), 1);
  EXPECT_EQ(
    doctor.at("advanced_evidence").at("recent_issue_codes").at(0),
    "steam_input_conflict"
  );

  bool saw_conflict = false;
  for (const auto &item : doctor.at("evidence")) {
    if (item.at("id") == "steam_input_compatibility") {
      saw_conflict = true;
      EXPECT_EQ(item.at("status"), "fail");
      EXPECT_EQ(item.at("source"), "local_steam_config");
      EXPECT_EQ(item.at("value"), "xbox_opt_in_and_per_app_forced");
    }
  }
  EXPECT_TRUE(saw_conflict);
}

TEST(StreamStatsDoctorTests, IgnoresSteamInputSettingsWithoutStrictIsolation) {
  stream_stats::stats_t stats {};
  stats.streaming = true;
  stats.capture_transport = platf::frame_transport_e::dmabuf;
  stats.capture_residency = platf::frame_residency_e::gpu;
  stats.encode_target_residency = platf::frame_residency_e::gpu;
  stats.encode_time_ms = 4.0;
  stats.input_host_controller_isolation = "disabled";
  stats.input_steam_input_status = "xbox_opt_in";
  stats.input_steam_profiles_checked = 1;
  stats.input_steam_profiles_with_xbox_support = 1;

  const auto doctor = stream_stats::build_doctor_json(
    stats,
    {{"primary_issue", "steady"}, {"grade", "good"}}
  );

  EXPECT_EQ(doctor.at("primary_issue"), "none");
  EXPECT_EQ(doctor.at("traffic_light"), "green");
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

TEST(StreamStatsDoctorTests, ClassifiesMetronomicHalfRateAsPacingWithoutNetworkEvidence) {
  stream_stats::stats_t stats {};
  stats.streaming = true;
  stats.fps = 60.0;
  stats.encode_target_fps = 120.0;
  stats.capture_source_fps = 120.0;
  stats.frame_jitter_ms = 8.3333;
  stats.capture_transport = platf::frame_transport_e::dmabuf;
  stats.capture_residency = platf::frame_residency_e::gpu;
  stats.encode_target_residency = platf::frame_residency_e::gpu;
  stats.encode_time_ms = 2.0;
  stats.packet_loss = 0.0;
  stats.latency_ms = 4.0;
  stats.network_risk = false;

  const auto doctor = stream_stats::build_doctor_json(
    stats,
    {{"primary_issue", "network_jitter"}, {"grade", "degraded"}}
  );

  EXPECT_EQ(doctor.at("primary_issue"), "frame_pacing");
  EXPECT_EQ(doctor.at("summary"), "Frame pacing telemetry needs attention.");
  EXPECT_NE(doctor.at("safe_recovery_action").at("id"), "lower_bitrate");

  bool saw_fps_gap = false;
  bool saw_interval_error = false;
  for (const auto &item : doctor.at("evidence")) {
    if (item.at("id") == "target_fps_gap") {
      saw_fps_gap = true;
      EXPECT_DOUBLE_EQ(item.at("value"), 60.0);
      EXPECT_EQ(item.at("status"), "watch");
    }
    if (item.at("id") == "frame_pacing") {
      saw_interval_error = true;
      EXPECT_EQ(item.at("label"), "Mean target interval error");
      EXPECT_EQ(item.at("unit"), "ms");
    }
  }
  EXPECT_TRUE(saw_fps_gap);
  EXPECT_TRUE(saw_interval_error);

  const auto serialized = nlohmann::json::parse(stats.to_json());
  EXPECT_DOUBLE_EQ(serialized.at("frame_interval_error_ms"), 8.3333);
  EXPECT_DOUBLE_EQ(serialized.at("frame_jitter_ms"), 8.3333);
}

TEST(StreamStatsDoctorTests, SuppressesStaleNetworkFindingWhenLiveEvidenceIsClean) {
  stream_stats::stats_t stats {};
  stats.streaming = true;
  stats.fps = 60.0;
  stats.encode_target_fps = 60.0;
  stats.bitrate_kbps = 20000;
  stats.capture_transport = platf::frame_transport_e::dmabuf;
  stats.capture_residency = platf::frame_residency_e::gpu;
  stats.encode_target_residency = platf::frame_residency_e::gpu;
  stats.encode_time_ms = 4.0;
  stats.packet_loss = 0.0;
  stats.packet_loss_available = true;
  stats.network_sample_revision = 1;
  stats.network_last_received_age_ms = 0;
  stats.media_loss_sample_revision = 1;
  stats.media_loss_last_received_age_ms = 0;
  stats.latency_ms = 3.8;
  stats.network_risk = false;

  const auto doctor = stream_stats::build_doctor_json(
    stats,
    {{"primary_issue", "network_jitter"}, {"grade", "degraded"}, {"summary", "Old network warning"}, {"safe_bitrate_kbps", 7580}}
  );

  EXPECT_EQ(doctor.at("version"), 2);
  EXPECT_EQ(doctor.at("primary_issue"), "none");
  EXPECT_EQ(doctor.at("summary"), "Streaming telemetry looks ready.");
  EXPECT_EQ(doctor.at("safe_recovery_action").at("id"), "none");
  ASSERT_EQ(doctor.at("suppressed_findings").size(), 1);
  EXPECT_EQ(doctor.at("suppressed_findings").at(0).at("id"), "stale_network_jitter");
}

TEST(StreamStatsDoctorTests, NetworkWatchRechecksWithoutChangingBitrate) {
  stream_stats::stats_t stats {};
  stats.streaming = true;
  stats.fps = 60.0;
  stats.encode_target_fps = 60.0;
  stats.bitrate_kbps = 20000;
  stats.capture_transport = platf::frame_transport_e::dmabuf;
  stats.capture_residency = platf::frame_residency_e::gpu;
  stats.encode_target_residency = platf::frame_residency_e::gpu;
  stats.network_risk = true;
  stats.packet_loss = 0.4;
  stats.control_channel_samples = 1;
  stats.network_sample_revision = 1;
  stats.network_last_received_age_ms = 0;
  stats.latency_ms = 20.0;

  const auto doctor = stream_stats::build_doctor_json(
    stats,
    {{"primary_issue", "network_jitter"}, {"grade", "watch"}, {"safe_bitrate_kbps", 12000}}
  );
  const auto &action = doctor.at("safe_recovery_action");

  EXPECT_EQ(doctor.at("primary_issue"), "network_observation");
  EXPECT_EQ(action.at("id"), "recheck_network");
  EXPECT_EQ(action.at("endpoint"), "/api/doctor/action");
  EXPECT_FALSE(action.at("requires_confirmation"));
  EXPECT_FALSE(action.at("undo").at("supported"));
  EXPECT_FALSE(action.at("payload_preview").contains("target_bitrate_kbps"));
}

TEST(StreamStatsDoctorTests, ControlLossIsInformationalAndCannotReduceQuality) {
  stream_stats::stats_t stats {};
  stats.streaming = true;
  stats.fps = 59.71;
  stats.encode_target_fps = 60.0;
  stats.bitrate_kbps = 16988;
  stats.paired_target_bitrate_kbps = 20000;
  stats.capture_transport = platf::frame_transport_e::dmabuf;
  stats.capture_residency = platf::frame_residency_e::gpu;
  stats.encode_target_residency = platf::frame_residency_e::gpu;
  stats.encode_time_ms = 0.77;
  stats.frame_jitter_ms = 1.57;
  stats.dropped_frame_ratio = 0.03;
  stats.packet_loss = 0.0;
  stats.packet_loss_available = false;
  stats.packet_loss_source = "unavailable";
  stats.control_channel_packet_loss = 8.72039794921875;
  stats.control_channel_samples = 42;
  stats.network_sample_revision = 42;
  stats.network_last_received_age_ms = 0;
  stats.latency_ms = 4.0;
  stats.network_risk = false;

  const auto doctor = stream_stats::build_doctor_json(stats, nlohmann::json::object());

  EXPECT_EQ(doctor.at("primary_issue"), "control_channel_observation");
  EXPECT_EQ(doctor.at("status"), "ok");
  EXPECT_EQ(doctor.at("traffic_light"), "green");
  EXPECT_EQ(doctor.at("confidence").at("basis"), "control_channel_only");
  EXPECT_EQ(doctor.at("confidence").at("sample_window").at("samples"), 42);
  EXPECT_EQ(doctor.at("safe_recovery_action").at("id"), "none");
  EXPECT_EQ(doctor.at("recommendation").at("next_step_label"), "Keep monitoring");

  bool saw_unknown_media_loss = false;
  bool saw_control_observation = false;
  for (const auto &item : doctor.at("evidence")) {
    if (item.at("id") == "packet_loss") {
      saw_unknown_media_loss = true;
      EXPECT_EQ(item.at("status"), "unknown");
      EXPECT_TRUE(item.at("value").is_null());
    }
    if (item.at("id") == "control_channel_packet_loss") {
      saw_control_observation = true;
      EXPECT_EQ(item.at("status"), "watch");
      EXPECT_DOUBLE_EQ(item.at("value"), 8.72039794921875);
    }
  }
  EXPECT_TRUE(saw_unknown_media_loss);
  EXPECT_TRUE(saw_control_observation);
}

TEST(StreamStatsDoctorTests, ConfirmedNetworkPressureOffersGuardedFixWithUndo) {
  stream_stats::stats_t stats {};
  stats.streaming = true;
  stats.fps = 60.0;
  stats.encode_target_fps = 60.0;
  stats.bitrate_kbps = 20000;
  stats.capture_transport = platf::frame_transport_e::dmabuf;
  stats.capture_residency = platf::frame_residency_e::gpu;
  stats.encode_target_residency = platf::frame_residency_e::gpu;
  stats.network_risk = true;
  stats.packet_loss = 3.4;
  stats.packet_loss_available = true;
  stats.network_sample_revision = 1;
  stats.network_last_received_age_ms = 0;
  stats.media_loss_sample_revision = 1;
  stats.media_loss_last_received_age_ms = 0;
  stats.latency_ms = 52.0;
  stats.adaptive_runtime_update_supported = true;

  const auto doctor = stream_stats::build_doctor_json(
    stats,
    {{"primary_issue", "network_jitter"}, {"grade", "degraded"}}
  );
  const auto &action = doctor.at("safe_recovery_action");

  EXPECT_EQ(doctor.at("primary_issue"), "network_jitter");
  EXPECT_EQ(action.at("id"), "lower_bitrate");
  EXPECT_EQ(action.at("endpoint"), "/api/doctor/action");
  EXPECT_EQ(action.at("payload_preview").at("target_bitrate_kbps"), 16000);
  EXPECT_EQ(action.at("payload_preview").at("source_result_id"), doctor.at("result_id"));
  EXPECT_FALSE(action.at("requires_confirmation"));
  EXPECT_TRUE(action.at("undo").at("supported"));
}

TEST(StreamStatsDoctorTests, AutoSafeOwnsConfirmedNetworkCorrectionWithoutCompetingAutoFix) {
  stream_stats::stats_t stats {};
  stats.streaming = true;
  stats.fps = 60.0;
  stats.encode_target_fps = 60.0;
  stats.bitrate_kbps = 20000;
  stats.adaptive_target_bitrate_kbps = 16000;
  stats.adaptive_bitrate_enabled = true;
  stats.adaptive_bitrate_active = true;
  stats.adaptive_bitrate_state = "network_pressure";
  stats.adaptive_runtime_update_supported = true;
  stats.capture_transport = platf::frame_transport_e::dmabuf;
  stats.capture_residency = platf::frame_residency_e::gpu;
  stats.encode_target_residency = platf::frame_residency_e::gpu;
  stats.network_risk = true;
  stats.packet_loss = 3.4;
  stats.packet_loss_available = true;
  stats.network_sample_revision = 1;
  stats.network_last_received_age_ms = 0;
  stats.media_loss_sample_revision = 1;
  stats.media_loss_last_received_age_ms = 0;
  stats.latency_ms = 52.0;

  const auto doctor = stream_stats::build_doctor_json(
    stats,
    {{"primary_issue", "network_jitter"}, {"grade", "degraded"}}
  );
  const auto &action = doctor.at("safe_recovery_action");

  EXPECT_EQ(doctor.at("primary_issue"), "network_jitter");
  EXPECT_EQ(action.at("id"), "recheck_network");
  EXPECT_EQ(action.at("capability"), "recheck");
  EXPECT_FALSE(action.at("undo").at("supported"));
  EXPECT_NE(
    doctor.at("recommendation").at("body").get<std::string>().find("Auto Safe"),
    std::string::npos
  );
  const auto &evidence = doctor.at("evidence");
  const auto owner = std::find_if(evidence.begin(), evidence.end(), [](const auto &item) {
    return item.at("id") == "live_bitrate_owner";
  });
  ASSERT_NE(owner, evidence.end());
  EXPECT_EQ(owner->at("value"), "auto_safe");
}

TEST(StreamStatsDoctorTests, AutoSafePolicyRemainsReadOnlyAcrossTransientActuatorStates) {
  const std::array<std::string, 4> transient_states {
    "recreating_encoder",
    "doctor_override",
    "explicit_live_target",
    "rollback_pending",
  };

  for (const auto &state : transient_states) {
    stream_stats::stats_t stats {};
    stats.streaming = true;
    stats.fps = 60.0;
    stats.encode_target_fps = 60.0;
    stats.bitrate_kbps = 20000;
    stats.adaptive_target_bitrate_kbps = 13000;
    stats.adaptive_bitrate_enabled = true;
    stats.adaptive_bitrate_active = state != "recreating_encoder";
    stats.adaptive_bitrate_state = state;
    stats.adaptive_runtime_update_supported = state != "recreating_encoder";
    stats.capture_transport = platf::frame_transport_e::dmabuf;
    stats.capture_residency = platf::frame_residency_e::gpu;
    stats.encode_target_residency = platf::frame_residency_e::gpu;
    stats.network_risk = true;
    stats.packet_loss = 3.4;
    stats.packet_loss_available = true;
    stats.network_sample_revision = 1;
    stats.network_last_received_age_ms = 0;
    stats.media_loss_sample_revision = 1;
    stats.media_loss_last_received_age_ms = 0;
    stats.latency_ms = 52.0;

    const auto doctor = stream_stats::build_doctor_json(
      stats,
      {{"primary_issue", "network_jitter"}, {"grade", "degraded"}}
    );
    const auto &action = doctor.at("safe_recovery_action");

    EXPECT_EQ(action.at("id"), "recheck_network") << state;
    EXPECT_EQ(action.at("capability"), "recheck") << state;
    EXPECT_FALSE(action.at("undo").at("supported").get<bool>()) << state;
    const auto &evidence = doctor.at("evidence");
    const auto owner = std::find_if(evidence.begin(), evidence.end(), [](const auto &item) {
      return item.at("id") == "live_bitrate_owner";
    });
    ASSERT_NE(owner, evidence.end()) << state;
    EXPECT_EQ(owner->at("value"), "auto_safe") << state;
  }
}

TEST(StreamStatsDoctorTests, UnsupportedRuntimeBitrateUsesAppliedRateAndOffersNoLiveFix) {
  stream_stats::stats_t stats {};
  stats.streaming = true;
  stats.fps = 60.0;
  stats.encode_target_fps = 120.0;
  stats.bitrate_kbps = 26000;
  stats.adaptive_target_bitrate_kbps = 2000;
  stats.adaptive_bitrate_active = false;
  stats.adaptive_runtime_update_supported = false;
  stats.capture_transport = platf::frame_transport_e::shm;
  stats.capture_residency = platf::frame_residency_e::cpu;
  stats.encode_target_residency = platf::frame_residency_e::gpu;
  stats.network_risk = true;
  stats.packet_loss = 21.8;
  stats.packet_loss_available = true;
  stats.network_sample_revision = 1;
  stats.network_last_received_age_ms = 0;
  stats.media_loss_sample_revision = 1;
  stats.media_loss_last_received_age_ms = 0;
  stats.latency_ms = 52.0;

  const auto doctor = stream_stats::build_doctor_json(
    stats,
    {{"primary_issue", "network_jitter"}, {"grade", "degraded"}}
  );

  EXPECT_EQ(doctor.at("primary_issue"), "network_jitter");
  EXPECT_EQ(doctor.at("safe_recovery_action").at("id"), "none");
  EXPECT_EQ(
    doctor.at("safe_recovery_action").at("unavailable_reason"),
    "The active encoder does not support runtime bitrate updates."
  );
  EXPECT_EQ(doctor.at("recommendation").at("next_step_label"), "Lower next-stream bitrate");

  for (const auto &item : doctor.at("evidence")) {
    if (item.at("id") == "bitrate") {
      EXPECT_EQ(item.at("value"), 26000);
    }
    if (item.at("id") == "live_bitrate_control") {
      EXPECT_FALSE(item.at("value").get<bool>());
    }
  }
}

TEST(StreamStatsDoctorTests, CleanLiveReductionOffersCapabilityBoundedQualityRestore) {
  stream_stats::stats_t stats {};
  stats.streaming = true;
  stats.fps = 40.0;
  stats.encode_target_fps = 40.0;
  stats.bitrate_kbps = 15000;
  stats.adaptive_target_bitrate_kbps = 7580;
  stats.paired_target_bitrate_kbps = 20000;
  stats.effective_launch_bitrate_kbps = 15000;
  stats.optimization_source = "deterministic_preset_v1";
  stats.capture_transport = platf::frame_transport_e::dmabuf;
  stats.capture_residency = platf::frame_residency_e::gpu;
  stats.encode_target_residency = platf::frame_residency_e::gpu;
  stats.encode_time_ms = 4.0;
  stats.packet_loss = 0.0;
  stats.packet_loss_available = true;
  stats.network_sample_revision = 1;
  stats.network_last_received_age_ms = 0;
  stats.media_loss_sample_revision = 1;
  stats.media_loss_last_received_age_ms = 0;
  stats.latency_ms = 3.8;
  stats.network_risk = false;
  stats.adaptive_runtime_update_supported = true;

  const auto doctor = stream_stats::build_doctor_json(
    stats,
    {{"primary_issue", "network_jitter"}, {"grade", "degraded"}, {"summary", "Old network warning"}}
  );
  const auto &action = doctor.at("safe_recovery_action");

  EXPECT_EQ(doctor.at("primary_issue"), "quality_reduced_live");
  EXPECT_EQ(doctor.at("traffic_light"), "amber");
  EXPECT_EQ(doctor.at("confidence").at("level"), "high");
  EXPECT_EQ(action.at("id"), "restore_quality");
  EXPECT_EQ(action.at("payload_preview").at("target_bitrate_kbps"), 15000);
  EXPECT_FALSE(action.at("requires_confirmation"));
  EXPECT_TRUE(action.at("undo").at("supported"));
  ASSERT_EQ(doctor.at("suppressed_findings").size(), 1);
}

TEST(StreamStatsDoctorTests, AutoSafeOwnsCleanQualityRecoveryWithoutCompetingAutoFix) {
  stream_stats::stats_t stats {};
  stats.streaming = true;
  stats.fps = 60.0;
  stats.encode_target_fps = 60.0;
  stats.bitrate_kbps = 15000;
  stats.adaptive_target_bitrate_kbps = 9000;
  stats.adaptive_bitrate_enabled = true;
  stats.adaptive_bitrate_active = true;
  stats.adaptive_bitrate_state = "recovering";
  stats.adaptive_runtime_update_supported = true;
  stats.paired_target_bitrate_kbps = 20000;
  stats.effective_launch_bitrate_kbps = 15000;
  stats.capture_transport = platf::frame_transport_e::dmabuf;
  stats.capture_residency = platf::frame_residency_e::gpu;
  stats.encode_target_residency = platf::frame_residency_e::gpu;
  stats.packet_loss = 0.0;
  stats.packet_loss_available = true;
  stats.network_sample_revision = 1;
  stats.network_last_received_age_ms = 0;
  stats.media_loss_sample_revision = 1;
  stats.media_loss_last_received_age_ms = 0;
  stats.latency_ms = 4.0;
  stats.network_risk = false;

  const auto doctor = stream_stats::build_doctor_json(stats, nlohmann::json::object());
  const auto &action = doctor.at("safe_recovery_action");

  EXPECT_EQ(doctor.at("primary_issue"), "quality_reduced_live");
  EXPECT_EQ(action.at("id"), "recheck_network");
  EXPECT_EQ(action.at("capability"), "recheck");
  EXPECT_FALSE(action.at("undo").at("supported"));
  EXPECT_NE(
    doctor.at("recommendation").at("body").get<std::string>().find("Auto Safe"),
    std::string::npos
  );
}

TEST(StreamStatsDoctorTests, QualityRestoreWaitsForMeasuredCleanNetworkEvidence) {
  stream_stats::stats_t stats {};
  stats.streaming = true;
  stats.fps = 60.0;
  stats.encode_target_fps = 60.0;
  stats.bitrate_kbps = 15000;
  stats.adaptive_target_bitrate_kbps = 7580;
  stats.paired_target_bitrate_kbps = 20000;
  stats.effective_launch_bitrate_kbps = 15000;
  stats.capture_transport = platf::frame_transport_e::dmabuf;
  stats.capture_residency = platf::frame_residency_e::gpu;
  stats.encode_target_residency = platf::frame_residency_e::gpu;
  stats.adaptive_runtime_update_supported = true;

  const auto doctor = stream_stats::build_doctor_json(stats, nlohmann::json::object());

  EXPECT_EQ(doctor.at("primary_issue"), "none");
  EXPECT_EQ(doctor.at("safe_recovery_action").at("id"), "none");
  const auto &evidence = doctor.at("evidence");
  const auto latency = std::find_if(evidence.begin(), evidence.end(), [](const auto &item) {
    return item.at("id") == "latency";
  });
  ASSERT_NE(latency, evidence.end());
  EXPECT_EQ(latency->at("status"), "unknown");
  EXPECT_TRUE(latency->at("value").is_null());
  EXPECT_EQ(latency->at("source"), "unavailable");
}

TEST(StreamStatsDoctorTests, RelaunchFindingOffersReadOnlyPacingRecheck) {
  stream_stats::stats_t stats {};
  stats.streaming = true;
  stats.fps = 54.0;
  stats.encode_target_fps = 60.0;
  stats.capture_source_fps = 60.0;
  stats.bitrate_kbps = 30000;
  stats.codec = "AV1";
  stats.capture_transport = platf::frame_transport_e::dmabuf;
  stats.capture_residency = platf::frame_residency_e::gpu;
  stats.encode_target_residency = platf::frame_residency_e::gpu;

  const auto doctor = stream_stats::build_doctor_json(
    stats,
    {
      {"primary_issue", "frame_pacing"},
      {"grade", "watch"},
      {"relaunch_recommended", true},
      {"safe_display_mode", "headless"},
      {"safe_target_fps", 40},
      {"safe_bitrate_kbps", 18000},
      {"safe_codec", "hevc"},
      {"safe_hdr", false}
    },
    "game-a"
  );
  const auto &action = doctor.at("safe_recovery_action");

  EXPECT_EQ(action.at("id"), "recheck_pacing");
  EXPECT_EQ(action.at("label"), "Recheck");
  EXPECT_EQ(action.at("capability"), "recheck");
  EXPECT_EQ(action.at("kind"), "verification");
  EXPECT_EQ(action.at("endpoint"), "/api/doctor/action");
  EXPECT_EQ(action.at("paired_endpoint"), "");
  EXPECT_EQ(action.at("method"), "POST");
  EXPECT_FALSE(action.at("requires_confirmation"));
  EXPECT_TRUE(action.at("requires_owner"));
  EXPECT_FALSE(action.at("owner_tuning_allowed"));
  EXPECT_FALSE(action.at("undo").at("supported"));
  EXPECT_EQ(action.at("payload_preview").size(), 2);
  EXPECT_EQ(action.at("payload_preview").at("action_id"), action.at("id"));
  EXPECT_EQ(action.at("payload_preview").at("source_result_id"), doctor.at("result_id"));
  EXPECT_FALSE(action.at("payload_preview").contains("app_uuid"));
  EXPECT_EQ(action.at("verification").at("mode"), "live_telemetry");
  EXPECT_EQ(
    action.at("rollback"),
    "This check is read-only and cannot change the next launch."
  );

  const auto identical = stream_stats::build_doctor_json(
    stats,
    {
      {"primary_issue", "frame_pacing"},
      {"grade", "watch"},
      {"relaunch_recommended", true},
      {"safe_display_mode", "headless"},
      {"safe_target_fps", 40},
      {"safe_bitrate_kbps", 18000},
      {"safe_codec", "hevc"},
      {"safe_hdr", false}
    },
    "game-a"
  );
  const auto changed_profile = stream_stats::build_doctor_json(
    stats,
    {
      {"primary_issue", "frame_pacing"},
      {"grade", "watch"},
      {"relaunch_recommended", true},
      {"safe_display_mode", "headless"},
      {"safe_target_fps", 30},
      {"safe_bitrate_kbps", 14000},
      {"safe_codec", "hevc"},
      {"safe_hdr", false}
    },
    "game-a"
  );
  EXPECT_EQ(identical.at("result_id"), doctor.at("result_id"));
  EXPECT_NE(changed_profile.at("result_id"), doctor.at("result_id"));
}

TEST(DoctorActionTests, RequiresCurrentNetworkEvidenceBeforeReducingQuality) {
  stream_stats::stats_t stats {};
  stats.streaming = true;
  stats.network_risk = true;
  stats.network_sample_revision = 1;
  stats.network_last_received_age_ms = 0;
  stats.packet_loss = 0.4;
  stats.latency_ms = 20.0;

  EXPECT_FALSE(doctor_actions::network_pressure_confirmed(stats));

  stats.packet_loss = 3.4;
  stats.packet_loss_available = true;
  stats.media_loss_sample_revision = 1;
  stats.media_loss_last_received_age_ms = 0;
  EXPECT_TRUE(doctor_actions::network_pressure_confirmed(stats));

  stats.packet_loss = 0.0;
  stats.packet_loss_available = false;
  stats.latency_ms = 45.0;
  EXPECT_TRUE(doctor_actions::network_pressure_confirmed(stats));

  stats.network_last_received_age_ms = 2001;
  EXPECT_FALSE(doctor_actions::network_pressure_confirmed(stats));
}

TEST(DoctorActionTests, HttpStatusContractUsesConflictForTypedActionFailures) {
  EXPECT_EQ(doctor_actions::http_status_code({{"status", true}}), 200);
  EXPECT_EQ(doctor_actions::http_status_code({{"status", false}}), 409);
  EXPECT_EQ(doctor_actions::http_status_code({{"status", "false"}}), 409);
  EXPECT_EQ(doctor_actions::http_status_code(nlohmann::json::object()), 409);
}

TEST(DoctorActionTests, HostNetworkPublicationInvalidatesDoctorSnapshotWithAdaptiveDisabled) {
  config::video.adaptive_bitrate.enabled = false;
  config::video.adaptive_bitrate.min_bitrate_kbps = 2000;
  config::video.adaptive_bitrate.max_bitrate_kbps = 50000;
  adaptive_bitrate::load_config();
  adaptive_bitrate::reset();
  adaptive_bitrate::set_runtime_update_supported(true, "supported", 20000);
  adaptive_bitrate::set_base_bitrate(20000);
  const auto stale_controller = adaptive_bitrate::get_doctor_state();
  ASSERT_FALSE(stale_controller.enabled);

  stream_stats::update_control_channel_stats(55.0, 0.0, 1000);

  const auto current_controller = adaptive_bitrate::get_doctor_state();
  EXPECT_GT(current_controller.revision, stale_controller.revision);
  EXPECT_FALSE(adaptive_bitrate::set_doctor_bitrate_if_revision(
    stale_controller.revision,
    25000,
    stale_controller.max_bitrate_kbps
  ));
  adaptive_bitrate::reset();
}

TEST(DoctorActionTests, CaptureCadenceTransitionInvalidatesVideoPolicyOnceWithAdaptiveDisabled) {
  stream_stats::update_stream_active(false);
  config::video.adaptive_bitrate.enabled = false;
  config::video.adaptive_bitrate.min_bitrate_kbps = 2000;
  config::video.adaptive_bitrate.max_bitrate_kbps = 50000;
  adaptive_bitrate::load_config();
  adaptive_bitrate::reset();
  adaptive_bitrate::set_runtime_update_supported(true, "supported", 20000);
  adaptive_bitrate::set_base_bitrate(20000);

  // A 108/120 delivered cadence with duplicate-heavy 40 FPS source content is
  // static evidence, not a pacing warning, so quality policy remains stable.
  stream_stats::update_capture_source_fps(40.0);
  stream_stats::note_doctor_video_policy_sample(
    120.0, 108.0, 0.20, 0.0, 5.0, 1.0, 5.0
  );
  const auto static_controller = adaptive_bitrate::get_doctor_state();
  ASSERT_FALSE(static_controller.enabled);

  stream_stats::note_doctor_video_policy_sample(
    120.0, 108.0, 0.20, 0.0, 5.0, 1.0, 5.0
  );
  EXPECT_EQ(
    adaptive_bitrate::get_doctor_state().revision,
    static_controller.revision
  );

  // When the source proves motion, the same delivered shortfall becomes a
  // pacing warning. The source publication must invalidate the old restore
  // envelope before exposing the new cadence, but only on this transition.
  stream_stats::update_capture_source_fps(110.0);
  const auto motion_controller = adaptive_bitrate::get_doctor_state();
  EXPECT_GT(motion_controller.revision, static_controller.revision);
  EXPECT_FALSE(adaptive_bitrate::set_doctor_bitrate_if_revision(
    static_controller.revision,
    25000,
    static_controller.max_bitrate_kbps
  ));
  stream_stats::update_capture_source_fps(110.0);
  EXPECT_EQ(
    adaptive_bitrate::get_doctor_state().revision,
    motion_controller.revision
  );

  stream_stats::update_stream_active(false);
  adaptive_bitrate::reset();
}

TEST(DoctorActionTests, FreshControlObservationCannotRefreshStaleMediaLoss) {
  using namespace std::chrono_literals;
  stream_stats::update_stream_active(false);
  stream_stats::update_stream_active(true, "DoctorMediaProvenance", "203.0.113.22");

  for (int i = 0; i < 6; ++i) {
    stream_stats::update_network_stats(5.0, 0.0, 1000);
  }
  for (int i = 0; i < 3; ++i) {
    stream_stats::update_network_stats(35.0, 3.4, 1000);
  }
  stream_stats::age_latest_network_observation_for_tests(3s);
  stream_stats::update_control_channel_stats(35.0, 0.0, 1000);

  const auto stats = stream_stats::get_current();
  EXPECT_TRUE(stats.network_risk);
  EXPECT_GE(stats.media_loss_last_received_age_ms, 2000);
  EXPECT_LT(stats.network_last_received_age_ms, 2000);
  EXPECT_FALSE(doctor_actions::network_pressure_confirmed(stats));
  const auto doctor = stream_stats::build_doctor_json(
    stats,
    nlohmann::json::object(),
    "media-provenance-app"
  );
  EXPECT_NE(doctor.at("primary_issue"), "network_jitter");
  const auto packet_loss_evidence = std::find_if(
    doctor.at("evidence").begin(),
    doctor.at("evidence").end(),
    [](const auto &item) {
      return item.value("id", std::string {}) == "packet_loss";
    }
  );
  ASSERT_NE(packet_loss_evidence, doctor.at("evidence").end());
  EXPECT_EQ(packet_loss_evidence->at("status"), "unknown");
  EXPECT_TRUE(packet_loss_evidence->at("value").is_null());

  stream_stats::update_stream_active(false);
}

TEST(DoctorActionTests, StaleHostNetworkEvidenceCannotMutateBitrate) {
  using namespace std::chrono_literals;
  stream_stats::update_stream_active(false);
  config::video.adaptive_bitrate.min_bitrate_kbps = 2000;
  config::video.adaptive_bitrate.max_bitrate_kbps = 100000;
  adaptive_bitrate::load_config();
  adaptive_bitrate::reset();
  adaptive_bitrate::set_runtime_update_supported(true);
  adaptive_bitrate::set_live_bitrate(10000);
  adaptive_bitrate::set_base_bitrate(15000);
  stream_stats::update_stream_active(true, "DoctorStaleEvidence", "203.0.113.21");
  stream_stats::update_video_stats(60.0, 10000, 5.0, "hevc", 1920, 1080);
  stream_stats::update_session_targets(
    60.0, 60.0, 60.0, "client_requested", "deterministic_preset_v1",
    "deterministic", "not_applicable", "Capability-validated launch profile.",
    "", 1, 15000, 15000
  );

  for (int i = 0; i < 6; ++i) {
    stream_stats::update_network_stats(5.0, 0.0, 1000);
  }
  stream_stats::age_latest_network_observation_for_tests(3s);
  const auto stale_restore = doctor_actions::execute({{"action_id", "restore_quality"}});
  EXPECT_FALSE(stale_restore.at("status").get<bool>());
  EXPECT_EQ(stale_restore.at("state"), "evidence_changed");
  EXPECT_EQ(adaptive_bitrate::get_doctor_state().live_bitrate_kbps, 10000);

  for (int i = 0; i < 3; ++i) {
    stream_stats::update_network_stats(55.0, 3.5, 1000);
  }
  stream_stats::age_latest_network_observation_for_tests(3s);
  const auto stale_reduce = doctor_actions::execute({{"action_id", "lower_bitrate"}});
  EXPECT_FALSE(stale_reduce.at("status").get<bool>());
  EXPECT_EQ(stale_reduce.at("state"), "evidence_changed");
  EXPECT_EQ(adaptive_bitrate::get_doctor_state().live_bitrate_kbps, 10000);

  adaptive_bitrate::set_enabled(false);
  stream_stats::update_stream_active(false);
}

TEST(DoctorActionTests, PacingRecheckWaitsForAFreshHostSampleWithoutMutation) {
  using namespace std::chrono_literals;
  stream_stats::update_stream_active(false);
  stream_stats::update_stream_active(true, "DoctorRecheck", "203.0.113.17");
  stream_stats::update_video_stats(60.0, 20000, 5.0, "hevc", 1920, 1080);

  std::thread publisher([] {
    std::this_thread::sleep_for(250ms);
    stream_stats::update_video_stats(59.0, 20000, 5.0, "hevc", 1920, 1080);
  });
  const auto started = std::chrono::steady_clock::now();
  const auto result = doctor_actions::execute({{"action_id", "recheck_pacing"}});
  const auto elapsed = std::chrono::steady_clock::now() - started;
  publisher.join();

  EXPECT_TRUE(result.at("status").get<bool>());
  EXPECT_FALSE(result.at("changed").get<bool>());
  EXPECT_EQ(result.at("state"), "observed");
  EXPECT_GE(elapsed, 2500ms);
  EXPECT_TRUE(result.contains("doctor"));

  stream_stats::update_stream_active(false);
}

TEST(DoctorActionTests, NeverDropsMoreThanOneGuardedBitrateStep) {
  EXPECT_EQ(doctor_actions::guarded_bitrate_target(20000, 7580, 2000), 16000);
  EXPECT_EQ(doctor_actions::guarded_bitrate_target(20000, 18000, 2000), 18000);
  EXPECT_EQ(doctor_actions::guarded_bitrate_target(8000, 2000, 7000), 7000);
}

TEST(DoctorActionTests, QualityRetryClimbsAtMostTwentyFivePercentPerCheck) {
  EXPECT_EQ(doctor_actions::guarded_quality_retry_target(7580, 20000), 9475);
  EXPECT_EQ(doctor_actions::guarded_quality_retry_target(18000, 20000), 20000);
  EXPECT_EQ(doctor_actions::guarded_quality_retry_target(20000, 20000), 20000);
}

TEST(DoctorActionTests, QualityRestoreUsesCapturedLaunchCeilingInsteadOfCurrentBitrate) {
  stream_stats::update_stream_active(false);
  adaptive_bitrate::set_max_bitrate(100000);
  adaptive_bitrate::set_live_bitrate(7580);
  // Recreate an adaptive reduction: the stream's base remains the validated
  // 15 Mbps launch ceiling while the current live target is 7.58 Mbps.
  adaptive_bitrate::set_base_bitrate(15000);
  adaptive_bitrate::set_enabled(true);
  adaptive_bitrate::set_runtime_update_supported(true);

  stream_stats::update_stream_active(true, "DoctorRestoreTest", "203.0.113.8");
  stream_stats::update_video_stats(60.0, 7580, 5.0, "hevc", 1920, 1080);
  stream_stats::update_session_targets(
    60.0,
    60.0,
    60.0,
    "client_requested",
    "deterministic_preset_v1",
    "deterministic",
    "not_applicable",
    "Capability-validated launch profile.",
    "",
    1,
    20000,
    15000
  );
  for (int i = 0; i < 6; ++i) {
    stream_stats::update_network_stats(5.0, 0.0, 1000);
  }

  const auto applied = doctor_actions::execute({{"action_id", "restore_quality"}});
  ASSERT_TRUE(applied.at("status").get<bool>());
  EXPECT_EQ(applied.at("requested").at("bitrate_kbps"), 9475);
  EXPECT_EQ(applied.at("requested").at("target_bitrate_kbps"), 15000);
  EXPECT_EQ(applied.at("evidence").at("effective_launch_bitrate_kbps"), 15000);

  const auto run_id = applied.at("run_id").get<std::string>();
  const auto apply_request = adaptive_bitrate::get_live_bitrate_request();
  ASSERT_TRUE(apply_request.has_value());
  adaptive_bitrate::acknowledge_live_bitrate_applied(
    apply_request->revision,
    apply_request->target_bitrate_kbps
  );
  const auto undone = execute_with_encoder_ack(7580, [&] {
    return doctor_actions::execute({
      {"action_id", "undo"}, {"run_id", run_id}
    });
  });
  EXPECT_TRUE(undone.at("status").get<bool>());
  EXPECT_EQ(adaptive_bitrate::get_state().base_bitrate_kbps, 15000);
  EXPECT_EQ(adaptive_bitrate::get_target_bitrate_kbps(), 7580);

  adaptive_bitrate::set_enabled(false);
  stream_stats::update_stream_active(false);
}

TEST(DoctorActionTests, CachedQualityVerificationCannotAuthorizeANewerStepAfterDegradation) {
  stream_stats::update_stream_active(false);
  config::video.adaptive_bitrate.min_bitrate_kbps = 2000;
  config::video.adaptive_bitrate.max_bitrate_kbps = 100000;
  adaptive_bitrate::load_config();
  adaptive_bitrate::reset();
  adaptive_bitrate::set_runtime_update_supported(true);
  adaptive_bitrate::set_live_bitrate(7580);
  adaptive_bitrate::set_base_bitrate(15000);

  stream_stats::update_stream_active(true, "DoctorRestoreFreshness", "203.0.113.18");
  stream_stats::update_video_stats(60.0, 7580, 5.0, "hevc", 1920, 1080);
  stream_stats::update_session_targets(
    60.0, 60.0, 60.0, "client_requested", "deterministic_preset_v1",
    "deterministic", "not_applicable", "Capability-validated launch profile.",
    "", 1, 20000, 15000
  );
  for (int i = 0; i < 6; ++i) {
    stream_stats::update_network_stats(5.0, 0.0, 1000);
  }

  const auto applied = doctor_actions::execute({{"action_id", "restore_quality"}});
  ASSERT_TRUE(applied.at("status").get<bool>());
  ASSERT_EQ(applied.at("requested").at("bitrate_kbps"), 9475);
  const auto run_id = applied.at("run_id").get<std::string>();

  for (int i = 0; i < 2; ++i) {
    stream_stats::update_network_stats(5.0, 0.0, 1000);
  }
  doctor_actions::make_verification_window_complete_for_tests();
  doctor_actions::run_verification_watchdog_for_tests();

  // The watchdog's clean result is receipt history only. New current
  // degradation must prevent the next restoration step and roll the entire
  // reversible transaction back to its captured target.
  for (int i = 0; i < 3; ++i) {
    stream_stats::update_network_stats(55.0, 3.5, 1000);
  }
  const auto degraded = execute_with_encoder_ack(7580, [&] {
    return doctor_actions::execute({
      {"action_id", "verify"}, {"run_id", run_id}
    });
  });
  EXPECT_TRUE(degraded.at("status").get<bool>());
  EXPECT_TRUE(degraded.at("changed").get<bool>());
  EXPECT_EQ(degraded.at("state"), "rolled_back");
  EXPECT_EQ(adaptive_bitrate::get_doctor_state().live_bitrate_kbps, 7580);

  adaptive_bitrate::set_enabled(false);
  stream_stats::update_stream_active(false);
}

TEST(DoctorActionTests, VideoWarningDuringQualityVerificationRollsBackTheRestore) {
  stream_stats::update_stream_active(false);
  config::video.adaptive_bitrate.min_bitrate_kbps = 2000;
  config::video.adaptive_bitrate.max_bitrate_kbps = 100000;
  adaptive_bitrate::load_config();
  adaptive_bitrate::reset();
  adaptive_bitrate::set_runtime_update_supported(true);
  adaptive_bitrate::set_live_bitrate(7580);
  adaptive_bitrate::set_base_bitrate(15000);

  stream_stats::update_stream_active(true, "DoctorRestoreVideoGuard", "203.0.113.24");
  stream_stats::update_video_stats(60.0, 7580, 5.0, "hevc", 1920, 1080);
  stream_stats::update_session_targets(
    60.0, 60.0, 60.0, "client_requested", "deterministic_preset_v1",
    "deterministic", "not_applicable", "Capability-validated launch profile.",
    "", 1, 20000, 15000
  );
  stream_stats::note_doctor_video_policy_sample(
    60.0, 60.0, 0.0, 0.0, 5.0, 1.0, 5.0
  );
  for (int i = 0; i < 6; ++i) {
    stream_stats::update_network_stats(5.0, 0.0, 1000);
  }

  const auto applied = doctor_actions::execute({{"action_id", "restore_quality"}});
  ASSERT_TRUE(applied.at("status").get<bool>());
  ASSERT_EQ(applied.at("requested").at("bitrate_kbps"), 9475);
  const auto run_id = applied.at("run_id").get<std::string>();
  const auto apply_request = adaptive_bitrate::get_live_bitrate_request();
  ASSERT_TRUE(apply_request.has_value());
  adaptive_bitrate::acknowledge_live_bitrate_applied(
    apply_request->revision,
    apply_request->target_bitrate_kbps
  );

  // A new encoder watch is evidence against another quality increase. Even if
  // a later sample clears, the host must remember the regression for this
  // reversible transaction and restore the pre-action target.
  stream_stats::note_doctor_video_policy_sample(
    60.0, 60.0, 0.0, 0.0, 5.0, 1.0, 9.0
  );
  stream_stats::note_doctor_video_policy_sample(
    60.0, 60.0, 0.0, 0.0, 5.0, 1.0, 5.0
  );
  for (int i = 0; i < 2; ++i) {
    stream_stats::update_network_stats(5.0, 0.0, 1000);
  }
  doctor_actions::make_verification_window_complete_for_tests();
  const auto rolled_back = execute_with_encoder_ack(7580, [&] {
    doctor_actions::run_verification_watchdog_for_tests();
    return doctor_actions::execute({
      {"action_id", "verify"}, {"run_id", run_id}
    });
  });
  EXPECT_TRUE(rolled_back.at("status").get<bool>());
  EXPECT_TRUE(rolled_back.at("changed").get<bool>());
  EXPECT_EQ(rolled_back.at("state"), "rolled_back");
  EXPECT_EQ(rolled_back.at("restored_bitrate_kbps"), 7580);
  EXPECT_EQ(adaptive_bitrate::get_doctor_state().live_bitrate_kbps, 7580);

  adaptive_bitrate::set_enabled(false);
  stream_stats::update_stream_active(false);
}

TEST(DoctorActionTests, NewerExplicitBitrateSupersedesUndoWithoutBeingOverwritten) {
  stream_stats::update_stream_active(false);
  config::video.adaptive_bitrate.min_bitrate_kbps = 2000;
  config::video.adaptive_bitrate.max_bitrate_kbps = 100000;
  adaptive_bitrate::load_config();
  adaptive_bitrate::reset();
  adaptive_bitrate::set_runtime_update_supported(true);
  adaptive_bitrate::set_base_bitrate(20000);
  adaptive_bitrate::set_enabled(false);

  stream_stats::update_stream_active(true, "DoctorExplicitWriter", "203.0.113.19");
  stream_stats::update_video_stats(60.0, 20000, 5.0, "hevc", 1920, 1080);
  for (int i = 0; i < 6; ++i) {
    stream_stats::update_network_stats(5.0, 0.0, 1000);
  }
  for (int i = 0; i < 3; ++i) {
    stream_stats::update_network_stats(52.0, 3.4, 1000);
  }

  const auto applied = doctor_actions::execute({{"action_id", "lower_bitrate"}});
  ASSERT_TRUE(applied.at("status").get<bool>());
  ASSERT_EQ(adaptive_bitrate::get_doctor_state().live_bitrate_kbps, 16000);
  const auto run_id = applied.at("run_id").get<std::string>();

  adaptive_bitrate::set_base_bitrate(10000);
  const auto undo = doctor_actions::execute({
    {"action_id", "undo"}, {"run_id", run_id}
  });
  EXPECT_TRUE(undo.at("status").get<bool>());
  EXPECT_FALSE(undo.at("changed").get<bool>());
  EXPECT_EQ(undo.at("state"), "superseded");
  EXPECT_EQ(adaptive_bitrate::get_doctor_state().base_bitrate_kbps, 10000);
  EXPECT_EQ(adaptive_bitrate::get_doctor_state().live_bitrate_kbps, 10000);

  adaptive_bitrate::set_enabled(false);
  stream_stats::update_stream_active(false);
}

TEST(DoctorActionTests, WatchdogRetainsSupersededTerminalReceipt) {
  stream_stats::update_stream_active(false);
  config::video.adaptive_bitrate.min_bitrate_kbps = 2000;
  config::video.adaptive_bitrate.max_bitrate_kbps = 100000;
  adaptive_bitrate::load_config();
  adaptive_bitrate::reset();
  adaptive_bitrate::set_runtime_update_supported(true);
  adaptive_bitrate::set_base_bitrate(20000);
  adaptive_bitrate::set_enabled(false);

  stream_stats::update_stream_active(true, "DoctorWatchdogSupersede", "203.0.113.20");
  stream_stats::update_video_stats(60.0, 20000, 5.0, "hevc", 1920, 1080);
  for (int i = 0; i < 6; ++i) {
    stream_stats::update_network_stats(5.0, 0.0, 1000);
  }
  for (int i = 0; i < 3; ++i) {
    stream_stats::update_network_stats(52.0, 3.4, 1000);
  }

  const auto applied = doctor_actions::execute({{"action_id", "lower_bitrate"}});
  ASSERT_TRUE(applied.at("status").get<bool>());
  const auto run_id = applied.at("run_id").get<std::string>();

  adaptive_bitrate::set_base_bitrate(10000);
  doctor_actions::run_verification_watchdog_for_tests();

  const auto receipt = doctor_actions::execute({
    {"action_id", "verify"}, {"run_id", run_id}
  });
  EXPECT_TRUE(receipt.at("status").get<bool>());
  EXPECT_FALSE(receipt.at("changed").get<bool>());
  EXPECT_EQ(receipt.at("state"), "superseded");
  EXPECT_EQ(adaptive_bitrate::get_doctor_state().live_bitrate_kbps, 10000);

  adaptive_bitrate::set_enabled(false);
  stream_stats::update_stream_active(false);
}

TEST(DoctorActionTests, ExecuteRefusesLiveTuningAndStaleUndoWithoutAStream) {
  stream_stats::update_stream_active(false);

  const auto blocked = doctor_actions::execute({{"action_id", "lower_bitrate"}});
  EXPECT_FALSE(blocked.at("status").get<bool>());
  EXPECT_FALSE(blocked.at("changed").get<bool>());
  EXPECT_EQ(blocked.at("state"), "needs_stream");
  EXPECT_EQ(blocked.at("error"), "Start the affected stream before Doctor applies or verifies a live fix.");
  EXPECT_TRUE(blocked.contains("evidence"));

  const auto stale_undo = doctor_actions::execute({{"action_id", "undo"}, {"run_id", "doctor-run-missing"}});
  EXPECT_FALSE(stale_undo.at("status").get<bool>());
  EXPECT_EQ(stale_undo.at("error"), "This Doctor undo is no longer available.");
}

TEST(DoctorActionTests, ExecuteAppliesVerifiesAndUndoesOneGuardedStepEndToEnd) {
  // Earlier suites in this binary leave adaptive_bitrate process state
  // behind; normalize the two pieces this arc depends on before seeding
  // telemetry. The same-stream ceiling no longer mutates saved config.
  config::video.adaptive_bitrate.enabled = false;
  config::video.adaptive_bitrate.min_bitrate_kbps = 2000;
  config::video.adaptive_bitrate.max_bitrate_kbps = 100000;
  adaptive_bitrate::load_config();
  adaptive_bitrate::reset();
  adaptive_bitrate::set_runtime_update_supported(true);
  adaptive_bitrate::set_base_bitrate(20000);

  stream_stats::update_stream_active(true, "DoctorContractTest", "203.0.113.7");
  stream_stats::update_video_stats(60.0, 20000, 5.0, "hevc", 1920, 1080);
  // One calm reading arms the risk tracker past its warm-up grace before the
  // elevated readings land (network_risk_tracker_t debounces both edges).
  for (int i = 0; i < 6; ++i) {
    stream_stats::update_network_stats(5.0, 0.0, 1000);
  }

  const auto clean = doctor_actions::execute({{"action_id", "lower_bitrate"}});
  EXPECT_FALSE(clean.at("status").get<bool>());
  EXPECT_EQ(clean.at("state"), "evidence_changed");

  const auto unsupported = doctor_actions::execute({{"action_id", "defragment_stream"}});
  EXPECT_FALSE(unsupported.at("status").get<bool>());
  EXPECT_EQ(unsupported.at("error"), "Unsupported Doctor action.");

  for (int i = 0; i < 3; ++i) {
    stream_stats::update_network_stats(52.0, 3.4, 1000);
  }
  ASSERT_TRUE(stream_stats::get_current().network_risk);

  adaptive_bitrate::set_runtime_update_supported(false);
  const auto unavailable = doctor_actions::execute({{"action_id", "lower_bitrate"}});
  EXPECT_FALSE(unavailable.at("status").get<bool>());
  EXPECT_FALSE(unavailable.at("changed").get<bool>());
  EXPECT_EQ(unavailable.at("state"), "runtime_update_unavailable");

  adaptive_bitrate::set_runtime_update_supported(true);

  const auto applied = doctor_actions::execute({{"action_id", "lower_bitrate"}});
  ASSERT_TRUE(applied.at("status").get<bool>());
  EXPECT_TRUE(applied.at("changed").get<bool>());
  EXPECT_EQ(applied.at("state"), "applying");
  EXPECT_EQ(applied.at("requested").at("bitrate_kbps"), 16000);
  EXPECT_EQ(applied.at("before").at("bitrate_kbps"), 20000);
  EXPECT_EQ(applied.at("verification").at("delay_seconds"), 8);
  const auto run_id = applied.at("run_id").get<std::string>();
  ASSERT_FALSE(run_id.empty());
  EXPECT_EQ(adaptive_bitrate::get_target_bitrate_kbps(), 16000);

  const auto overlapping = doctor_actions::execute({{"action_id", "lower_bitrate"}});
  EXPECT_FALSE(overlapping.at("status").get<bool>());
  EXPECT_FALSE(overlapping.at("changed").get<bool>());
  EXPECT_EQ(overlapping.at("state"), "action_in_progress");
  EXPECT_EQ(overlapping.at("run_id"), run_id);
  EXPECT_EQ(adaptive_bitrate::get_target_bitrate_kbps(), 16000);

  const auto wrong_run = doctor_actions::execute({{"action_id", "verify"}, {"run_id", "doctor-run-imposter"}});
  EXPECT_FALSE(wrong_run.at("status").get<bool>());
  EXPECT_EQ(wrong_run.at("state"), "expired");

  const auto watching = doctor_actions::execute({{"action_id", "verify"}, {"run_id", run_id}});
  EXPECT_TRUE(watching.at("status").get<bool>());
  EXPECT_FALSE(watching.at("changed").get<bool>());
  EXPECT_EQ(watching.at("state"), "applying");
  EXPECT_GT(watching.at("retry_after_seconds").get<int>(), 0);
  EXPECT_TRUE(watching.at("undo").at("available").get<bool>());

  // Elapsed time alone is not verification. Without two newer host-received
  // network observations, the guarded change rolls back instead of accepting
  // stale pre-change evidence.
  doctor_actions::make_verification_due_for_tests();
  const auto no_fresh_evidence = execute_with_encoder_ack(20000, [&] {
    return doctor_actions::execute({
      {"action_id", "verify"}, {"run_id", run_id}
    });
  });
  EXPECT_TRUE(no_fresh_evidence.at("status").get<bool>());
  EXPECT_TRUE(no_fresh_evidence.at("changed").get<bool>());
  EXPECT_EQ(no_fresh_evidence.at("state"), "rolled_back");

  const auto rolled_back_undo = doctor_actions::execute({
    {"action_id", "undo"}, {"run_id", run_id}
  });
  EXPECT_TRUE(rolled_back_undo.at("status").get<bool>());
  EXPECT_TRUE(rolled_back_undo.at("changed").get<bool>());
  EXPECT_EQ(rolled_back_undo.at("state"), "rolled_back");
  EXPECT_EQ(rolled_back_undo.at("run_id"), run_id);
  EXPECT_FALSE(rolled_back_undo.at("undo").at("available").get<bool>());

  const auto clustered_run = doctor_actions::execute({{"action_id", "lower_bitrate"}});
  ASSERT_TRUE(clustered_run.at("status").get<bool>());
  const auto clustered_run_id = clustered_run.at("run_id").get<std::string>();

  // Two samples clustered at the end of a timer delay do not cover the
  // verification interval and must roll back.
  for (int i = 0; i < 2; ++i) {
    stream_stats::update_network_stats(5.0, 0.0, 1000);
  }
  ASSERT_FALSE(stream_stats::get_current().network_risk);
  doctor_actions::make_verification_due_for_tests();
  const auto clustered = execute_with_encoder_ack(20000, [&] {
    return doctor_actions::execute({
      {"action_id", "verify"}, {"run_id", clustered_run_id}
    });
  });
  EXPECT_TRUE(clustered.at("status").get<bool>());
  EXPECT_TRUE(clustered.at("changed").get<bool>());
  EXPECT_EQ(clustered.at("state"), "rolled_back");
  EXPECT_FALSE(clustered.at("verification_window").at("complete").get<bool>());

  for (int i = 0; i < 3; ++i) {
    stream_stats::update_network_stats(52.0, 3.4, 1000);
  }
  ASSERT_TRUE(stream_stats::get_current().network_risk);
  const auto reapplied_for_verification = doctor_actions::execute({{"action_id", "lower_bitrate"}});
  ASSERT_TRUE(reapplied_for_verification.at("status").get<bool>());
  const auto verified_run_id = reapplied_for_verification.at("run_id").get<std::string>();
  for (int i = 0; i < 2; ++i) {
    stream_stats::update_network_stats(5.0, 0.0, 1000);
  }
  ASSERT_FALSE(stream_stats::get_current().network_risk);
  doctor_actions::make_verification_window_complete_for_tests();
  const auto verified = doctor_actions::execute({
    {"action_id", "verify"}, {"run_id", verified_run_id}
  });
  EXPECT_TRUE(verified.at("status").get<bool>());
  EXPECT_FALSE(verified.at("changed").get<bool>());
  EXPECT_EQ(verified.at("state"), "resolved");
  EXPECT_NE(verified.at("message").get<std::string>().find("verified"), std::string::npos);
  EXPECT_TRUE(verified.at("verification_window").at("complete").get<bool>());
  EXPECT_EQ(adaptive_bitrate::get_target_bitrate_kbps(), 16000);

  const auto undone = execute_with_encoder_ack(20000, [&] {
    return doctor_actions::execute({
      {"action_id", "undo"}, {"run_id", verified_run_id}
    });
  });
  EXPECT_TRUE(undone.at("status").get<bool>());
  EXPECT_TRUE(undone.at("changed").get<bool>());
  EXPECT_EQ(undone.at("state"), "undone");
  EXPECT_EQ(undone.at("run_id"), verified_run_id);
  EXPECT_EQ(undone.at("restored_bitrate_kbps"), 20000);
  EXPECT_EQ(adaptive_bitrate::get_target_bitrate_kbps(), 0);

  const auto replay = doctor_actions::execute({
    {"action_id", "undo"}, {"run_id", verified_run_id}
  });
  EXPECT_TRUE(replay.at("status").get<bool>());
  EXPECT_TRUE(replay.at("changed").get<bool>());
  EXPECT_EQ(replay.at("state"), "undone");
  EXPECT_EQ(replay.at("run_id"), verified_run_id);
  EXPECT_EQ(replay.at("restored_bitrate_kbps"), 20000);

  for (int i = 0; i < 3; ++i) {
    stream_stats::update_network_stats(52.0, 3.4, 1000);
  }
  ASSERT_TRUE(stream_stats::get_current().network_risk);
  const auto reapplied = doctor_actions::execute({{"action_id", "lower_bitrate"}});
  ASSERT_TRUE(reapplied.at("status").get<bool>());
  const auto second_run_id = reapplied.at("run_id").get<std::string>();
  adaptive_bitrate::set_runtime_update_supported(false);

  const auto unavailable_during_verification = doctor_actions::execute({
    {"action_id", "verify"}, {"run_id", second_run_id}
  });
  EXPECT_FALSE(unavailable_during_verification.at("status").get<bool>());
  EXPECT_TRUE(unavailable_during_verification.at("changed").get<bool>());
  EXPECT_EQ(unavailable_during_verification.at("state"), "rollback_unconfirmed");
  EXPECT_EQ(
    unavailable_during_verification.at("requested_restore_bitrate_kbps"),
    20000
  );
  EXPECT_FALSE(
    unavailable_during_verification.at("undo").at("available").get<bool>()
  );
  EXPECT_EQ(adaptive_bitrate::get_target_bitrate_kbps(), 0);

  const auto unconfirmed_undo = doctor_actions::execute({
    {"action_id", "undo"}, {"run_id", second_run_id}
  });
  EXPECT_FALSE(unconfirmed_undo.at("status").get<bool>());
  EXPECT_TRUE(unconfirmed_undo.at("changed").get<bool>());
  EXPECT_EQ(unconfirmed_undo.at("state"), "rollback_unconfirmed");
  EXPECT_EQ(unconfirmed_undo.at("run_id"), second_run_id);
  EXPECT_FALSE(unconfirmed_undo.at("undo").at("available").get<bool>());
  adaptive_bitrate::set_runtime_update_supported(true);

  doctor_actions::recovery_action_context_t scoped_context;
  const auto live_action_state_path = std::filesystem::temp_directory_path() /
    "polaris-doctor-live-action-no-legacy-record.json";
  std::error_code live_action_state_error;
  std::filesystem::remove(live_action_state_path, live_action_state_error);
  scoped_context.active_owner = true;
  scoped_context.host_tuning_allowed = true;
  scoped_context.owner_uuid = "client-a";
  scoped_context.app_uuid = "game-a";
  scoped_context.session_generation = 101;
  scoped_context.state_path = live_action_state_path;
  doctor_actions::session_started("client-a", 101, 20000);
  adaptive_bitrate::set_runtime_update_supported(true);
  stream_stats::start_session_timing("client-a", 101);
  scoped_context.stats = stream_stats::get_current();

  scoped_context.host_tuning_allowed = false;
  const auto shutdown_blocked = doctor_actions::execute(
    {{"action_id", "lower_bitrate"}, {"request_id", "test-shutdown-blocked"}}, scoped_context
  );
  EXPECT_FALSE(shutdown_blocked.at("status").get<bool>());
  EXPECT_EQ(shutdown_blocked.at("state"), "scope_unavailable");
  EXPECT_EQ(adaptive_bitrate::get_target_bitrate_kbps(), 0);

  scoped_context.host_tuning_allowed = true;
  const auto forged_scoped = doctor_actions::execute(
    {{"action_id", "lower_bitrate"}, {"request_id", "test-forged-envelope"}}, scoped_context
  );
  EXPECT_FALSE(forged_scoped.at("status").get<bool>());
  EXPECT_EQ(forged_scoped.at("code"), "stale_action_envelope");
  EXPECT_EQ(adaptive_bitrate::get_target_bitrate_kbps(), 0);

  const auto scoped = doctor_actions::execute(
    trusted_doctor_action_request(scoped_context), scoped_context
  );
  ASSERT_TRUE(scoped.at("status").get<bool>());
  const auto scoped_run_id = scoped.at("run_id").get<std::string>();

  auto other_owner = scoped_context;
  other_owner.active_owner = false;
  other_owner.caller_is_viewer = true;
  other_owner.owner_uuid = "client-b";
  const auto cross_owner_undo = doctor_actions::execute({
    {"action_id", "undo"}, {"run_id", "attacker-controlled-run-id"}
  }, other_owner);
  EXPECT_FALSE(cross_owner_undo.at("status").get<bool>());
  EXPECT_EQ(cross_owner_undo.at("state"), "scope_mismatch");
  EXPECT_EQ(adaptive_bitrate::get_target_bitrate_kbps(), 16000);

  const auto still_owned = doctor_actions::execute({
    {"action_id", "verify"}, {"run_id", scoped_run_id}
  }, scoped_context);
  EXPECT_TRUE(still_owned.at("status").get<bool>());
  EXPECT_EQ(still_owned.at("state"), "applying");

  // Simulate teardown restoring the next stream's own target. A receipt from
  // generation 101 must reject generation 102 without clearing the owned run
  // or changing the new stream.
  doctor_actions::session_started("client-a", 102, 20000);
  adaptive_bitrate::set_runtime_update_supported(true);
  stream_stats::start_session_timing("client-a", 102);
  auto next_generation = scoped_context;
  next_generation.session_generation = 102;
  next_generation.stats = stream_stats::get_current();
  const auto stale_generation = doctor_actions::execute({
    {"action_id", "verify"}, {"run_id", scoped_run_id}
  }, next_generation);
  EXPECT_FALSE(stale_generation.at("status").get<bool>());
  EXPECT_EQ(stale_generation.at("state"), "expired");
  EXPECT_EQ(adaptive_bitrate::get_state().base_bitrate_kbps, 20000);
  EXPECT_FALSE(adaptive_bitrate::is_enabled());

  doctor_actions::session_ended("client-a", 101);
  EXPECT_EQ(adaptive_bitrate::get_state().base_bitrate_kbps, 20000);
  EXPECT_FALSE(adaptive_bitrate::is_enabled());
  const auto retired_undo = doctor_actions::execute({
    {"action_id", "undo"}, {"run_id", scoped_run_id}
  }, scoped_context);
  EXPECT_FALSE(retired_undo.at("status").get<bool>());
  EXPECT_EQ(retired_undo.at("error"), "This Doctor undo is no longer available.");
  doctor_actions::session_ended("client-a", 102);
  stream_stats::stop_session_timing("client-a", 102);
  std::filesystem::remove(live_action_state_path, live_action_state_error);

  stream_stats::update_stream_active(false);
}

TEST(DoctorActionTests, StaleWatchdogNeverRestoresIntoANewerStreamGeneration) {
  stream_stats::update_stream_active(false);
  config::video.adaptive_bitrate.min_bitrate_kbps = 2000;
  config::video.adaptive_bitrate.max_bitrate_kbps = 100000;
  adaptive_bitrate::set_max_bitrate(100000);
  adaptive_bitrate::set_enabled(false);
  adaptive_bitrate::set_runtime_update_supported(true);

  stream_stats::update_stream_active(true, "DoctorWatchdogTest", "203.0.113.9");
  stream_stats::update_video_stats(60.0, 20000, 5.0, "hevc", 1920, 1080);
  for (int i = 0; i < 6; ++i) {
    stream_stats::update_network_stats(5.0, 0.0, 1000);
  }
  for (int i = 0; i < 3; ++i) {
    stream_stats::update_network_stats(52.0, 3.4, 1000);
  }
  ASSERT_TRUE(stream_stats::get_current().network_risk);

  constexpr std::uint64_t original_generation = 201;
  constexpr std::uint64_t replacement_generation = 202;
  doctor_actions::session_started("client-watchdog", original_generation, 20000);
  adaptive_bitrate::set_runtime_update_supported(true);
  stream_stats::start_session_timing("client-watchdog", original_generation);

  doctor_actions::recovery_action_context_t context;
  context.active_owner = true;
  context.host_tuning_allowed = true;
  context.owner_uuid = "client-watchdog";
  context.app_uuid = "game-watchdog";
  context.session_generation = original_generation;
  context.stats = stream_stats::get_current();

  const auto applied = doctor_actions::execute(
    trusted_doctor_action_request(context), context
  );
  ASSERT_TRUE(applied.at("status").get<bool>());
  EXPECT_EQ(adaptive_bitrate::get_target_bitrate_kbps(), 16000);
  const auto run_id = applied.at("run_id").get<std::string>();

  // A reconnect has already installed generation 202 and its own target when
  // the delayed generation-201 watchdog wakes. It must retire the old receipt
  // without writing generation 201's rollback value into generation 202.
  doctor_actions::session_started("client-watchdog", replacement_generation, 20000);
  adaptive_bitrate::set_runtime_update_supported(true);
  stream_stats::start_session_timing("client-watchdog", replacement_generation);
  doctor_actions::run_verification_watchdog_for_tests();
  EXPECT_EQ(adaptive_bitrate::get_state().base_bitrate_kbps, 20000);
  EXPECT_FALSE(adaptive_bitrate::is_enabled());

  const auto stale_undo = doctor_actions::execute({
    {"action_id", "undo"}, {"run_id", run_id}
  }, context);
  EXPECT_FALSE(stale_undo.at("status").get<bool>());
  EXPECT_EQ(stale_undo.at("error"), "This Doctor undo is no longer available.");
  EXPECT_EQ(adaptive_bitrate::get_state().base_bitrate_kbps, 20000);
  EXPECT_FALSE(adaptive_bitrate::is_enabled());

  doctor_actions::session_ended("client-watchdog", original_generation);
  doctor_actions::session_ended("client-watchdog", replacement_generation);
  stream_stats::stop_session_timing("client-watchdog", replacement_generation);
  adaptive_bitrate::set_enabled(false);
  stream_stats::update_stream_active(false);
}

TEST(DoctorActionTests, AutoFixRefusesAProcessGlobalControllerSharedByTwoSessions) {
  stream_stats::update_stream_active(false);
  config::video.adaptive_bitrate.min_bitrate_kbps = 2000;
  config::video.adaptive_bitrate.max_bitrate_kbps = 100000;
  adaptive_bitrate::set_enabled(false);
  stream_stats::update_stream_active(true, "DoctorMultiSession", "203.0.113.10");
  stream_stats::update_video_stats(60.0, 20000, 5.0, "hevc", 1920, 1080);
  for (int i = 0; i < 6; ++i) {
    stream_stats::update_network_stats(5.0, 0.0, 1000);
  }
  for (int i = 0; i < 3; ++i) {
    stream_stats::update_network_stats(52.0, 3.4, 1000);
  }

  doctor_actions::session_started("client-one", 301, 20000);
  adaptive_bitrate::set_runtime_update_supported(true);
  stream_stats::start_session_timing("client-one", 301);

  doctor_actions::recovery_action_context_t context;
  context.active_owner = true;
  context.host_tuning_allowed = true;
  context.owner_uuid = "client-one";
  context.app_uuid = "game-one";
  context.session_generation = 301;
  context.stats = stream_stats::get_current();
  const auto request = trusted_doctor_action_request(context);

  doctor_actions::session_started("client-two", 302, 18000);
  adaptive_bitrate::set_runtime_update_supported(true);
  stream_stats::start_session_timing("client-two", 302);

  const auto blocked = doctor_actions::execute(request, context);
  EXPECT_FALSE(blocked.at("status").get<bool>());
  EXPECT_EQ(blocked.at("state"), "scope_unavailable");
  EXPECT_EQ(adaptive_bitrate::get_state().base_bitrate_kbps, 18000);
  EXPECT_FALSE(adaptive_bitrate::is_enabled());

  doctor_actions::session_ended("client-one", 301);
  EXPECT_EQ(adaptive_bitrate::get_state().base_bitrate_kbps, 18000);
  EXPECT_FALSE(adaptive_bitrate::is_enabled());
  doctor_actions::session_ended("client-two", 302);
  stream_stats::stop_session_timing("client-one", 301);
  stream_stats::stop_session_timing("client-two", 302);
  stream_stats::update_stream_active(false);
}

TEST(StreamStatsDoctorTests, MultipleSessionsNeverOfferProcessGlobalAutoFix) {
  stream_stats::stats_t stats {};
  stats.streaming = true;
  stats.network_risk = true;
  stats.packet_loss_available = true;
  stats.packet_loss = 3.5;
  stats.network_sample_revision = 1;
  stats.network_last_received_age_ms = 0;
  stats.media_loss_sample_revision = 1;
  stats.media_loss_last_received_age_ms = 0;
  stats.latency_ms = 50.0;
  stats.bitrate_kbps = 20000;
  stats.adaptive_runtime_update_supported = true;
  stats.capture_transport = platf::frame_transport_e::dmabuf;
  stats.capture_residency = platf::frame_residency_e::gpu;
  stats.encode_target_residency = platf::frame_residency_e::gpu;
  stats.clients.resize(2);

  const auto doctor = stream_stats::build_doctor_json(
    stats, nlohmann::json::object(), "multi-session-app"
  );
  EXPECT_EQ(doctor.at("primary_issue"), "network_jitter");
  EXPECT_EQ(doctor.at("safe_recovery_action").at("id"), "none");
  EXPECT_EQ(doctor.at("safe_recovery_action").at("capability"), "manual");
  EXPECT_NE(
    doctor.at("safe_recovery_action").at("unavailable_reason").get<std::string>().find("fresh, unshared stream generation"),
    std::string::npos
  );
}

TEST(DoctorActionTests, OlderStreamCannotAutoFixAfterTheNewestViewerLeaves) {
  stream_stats::update_stream_active(true, "DoctorSharedGeneration", "203.0.113.11");
  doctor_actions::session_started("client-owner", 401, 20000);
  doctor_actions::session_started("client-viewer", 402, 18000);
  doctor_actions::session_ended("client-viewer", 402);

  doctor_actions::recovery_action_context_t context;
  context.active_owner = true;
  context.host_tuning_allowed = true;
  context.owner_uuid = "client-owner";
  context.app_uuid = "game-owner";
  context.session_generation = 401;
  context.stats = stream_stats::get_current();

  const auto blocked = doctor_actions::execute({
    {"action_id", "lower_bitrate"}, {"request_id", "test-retired-owner"}
  }, context);
  EXPECT_FALSE(blocked.at("status").get<bool>());
  EXPECT_EQ(blocked.at("state"), "scope_unavailable");
  EXPECT_FALSE(stream_stats::get_current().doctor_live_action_scope_available);

  doctor_actions::session_ended("client-owner", 401);
  stream_stats::update_stream_active(false);
}

TEST(DoctorActionTests, IdempotentAutoFixCannotOverwriteANewerOwnerBitrate) {
  config::video.adaptive_bitrate.enabled = false;
  config::video.adaptive_bitrate.min_bitrate_kbps = 2000;
  config::video.adaptive_bitrate.max_bitrate_kbps = 100000;
  adaptive_bitrate::load_config();
  adaptive_bitrate::reset();
  stream_stats::update_stream_active(true, "DoctorIdempotent", "203.0.113.23");
  stream_stats::update_video_stats(60.0, 20000, 5.0, "hevc", 1920, 1080);
  for (int i = 0; i < 6; ++i) {
    stream_stats::update_network_stats(5.0, 0.0, 1000);
  }
  for (int i = 0; i < 3; ++i) {
    stream_stats::update_network_stats(52.0, 3.4, 1000);
  }

  constexpr std::uint64_t generation = 421;
  doctor_actions::session_started("client-owner", generation, "launch-421", 20000);
  adaptive_bitrate::set_runtime_update_supported(true, {}, 20000);
  stream_stats::start_session_timing("client-owner", generation, "launch-421");

  doctor_actions::recovery_action_context_t context;
  context.active_owner = true;
  context.host_tuning_allowed = true;
  context.owner_uuid = "client-owner";
  context.app_uuid = "game-owner";
  context.launch_instance_id = "launch-421";
  context.session_generation = generation;
  context.enforce_request_scope = true;
  context.stats = stream_stats::get_current();
  const auto request = trusted_doctor_action_request(context);

  auto stale_scope_request = request;
  stale_scope_request["session_generation"] = generation - 1;
  const auto stale_scope = doctor_actions::execute(stale_scope_request, context);
  EXPECT_FALSE(stale_scope.at("status").get<bool>());
  EXPECT_EQ(stale_scope.at("state"), "scope_mismatch");
  EXPECT_EQ(stale_scope.at("code"), "stale_stream_generation");
  EXPECT_EQ(adaptive_bitrate::get_doctor_state().live_bitrate_kbps, 20000);

  const auto applied = doctor_actions::execute(request, context);
  ASSERT_TRUE(applied.at("status").get<bool>());
  ASSERT_EQ(applied.at("state"), "applying");
  const auto run_id = applied.at("run_id").get<std::string>();
  const auto request_id = request.at("request_id").get<std::string>();

  const auto repeated = doctor_actions::execute(request, context);
  EXPECT_TRUE(repeated.at("status").get<bool>());
  EXPECT_FALSE(repeated.at("changed").get<bool>());
  EXPECT_EQ(repeated.at("run_id"), run_id);
  EXPECT_EQ(repeated.at("request_id"), request_id);
  EXPECT_FALSE(doctor_actions::set_owner_live_bitrate(
    "client-viewer", generation, "launch-421", 18000
  ));

  ASSERT_TRUE(doctor_actions::set_owner_live_bitrate(
    "client-owner", generation, "launch-421", 18000
  ));
  EXPECT_EQ(adaptive_bitrate::get_doctor_state().live_bitrate_kbps, 18000);
  const auto stale_retry = doctor_actions::execute(request, context);
  EXPECT_TRUE(stale_retry.at("status").get<bool>());
  EXPECT_EQ(stale_retry.at("state"), "superseded");
  EXPECT_EQ(stale_retry.at("request_id"), request_id);
  EXPECT_EQ(adaptive_bitrate::get_doctor_state().live_bitrate_kbps, 18000);

  // The explicit owner write retired the live run, but a durable client may
  // still present its Undo receipt afterward. Replay the exact terminal result
  // so the client can retire that stale Undo without touching the newer target.
  const auto retired_undo = doctor_actions::execute({
    {"action_id", "undo"},
    {"run_id", run_id},
    {"app_session_id", context.launch_instance_id},
    {"session_generation", generation}
  }, context);
  EXPECT_TRUE(retired_undo.at("status").get<bool>());
  EXPECT_FALSE(retired_undo.at("changed").get<bool>());
  EXPECT_EQ(retired_undo.at("state"), "superseded");
  EXPECT_EQ(retired_undo.at("run_id"), run_id);
  EXPECT_FALSE(retired_undo.at("undo").at("available").get<bool>());
  EXPECT_EQ(adaptive_bitrate::get_doctor_state().live_bitrate_kbps, 18000);

  doctor_actions::session_ended("client-owner", generation);
  stream_stats::stop_session_timing("client-owner", generation);
  stream_stats::update_stream_active(false);
}

TEST(DoctorActionTests, StaleControllerRevisionCannotOverrideANewerOwnerChoice) {
  config::video.adaptive_bitrate.enabled = false;
  config::video.adaptive_bitrate.min_bitrate_kbps = 2000;
  config::video.adaptive_bitrate.max_bitrate_kbps = 100000;
  adaptive_bitrate::load_config();
  adaptive_bitrate::reset();
  stream_stats::update_stream_active(true, "DoctorRevision", "203.0.113.24");
  stream_stats::update_video_stats(60.0, 20000, 5.0, "hevc", 1920, 1080);
  for (int i = 0; i < 6; ++i) {
    stream_stats::update_network_stats(5.0, 0.0, 1000);
  }
  for (int i = 0; i < 3; ++i) {
    stream_stats::update_network_stats(52.0, 3.4, 1000);
  }

  constexpr std::uint64_t generation = 422;
  doctor_actions::session_started("client-owner", generation, "launch-422", 20000);
  adaptive_bitrate::set_runtime_update_supported(true, {}, 20000);
  stream_stats::start_session_timing("client-owner", generation, "launch-422");

  doctor_actions::recovery_action_context_t context;
  context.active_owner = true;
  context.host_tuning_allowed = true;
  context.enforce_request_scope = true;
  context.owner_uuid = "client-owner";
  context.app_uuid = "game-owner";
  context.launch_instance_id = "launch-422";
  context.session_generation = generation;
  context.stats = stream_stats::get_current();
  const auto stale_request = trusted_doctor_action_request(context);

  ASSERT_TRUE(doctor_actions::set_owner_live_bitrate(
    "client-owner", generation, "launch-422", 20000
  ));
  const auto rejected = doctor_actions::execute(stale_request, context);
  EXPECT_FALSE(rejected.at("status").get<bool>());
  EXPECT_EQ(rejected.at("state"), "evidence_changed");
  EXPECT_EQ(rejected.at("code"), "stale_action_envelope");
  EXPECT_EQ(adaptive_bitrate::get_doctor_state().live_bitrate_kbps, 20000);

  doctor_actions::session_ended("client-owner", generation);
  stream_stats::stop_session_timing("client-owner", generation);
  stream_stats::update_stream_active(false);
}

TEST(DoctorActionTests, EquivalentFreshTelemetryCannotMakeAutoFixUnclickable) {
  config::video.adaptive_bitrate.enabled = false;
  config::video.adaptive_bitrate.min_bitrate_kbps = 2000;
  config::video.adaptive_bitrate.max_bitrate_kbps = 100000;
  adaptive_bitrate::load_config();
  adaptive_bitrate::reset();
  stream_stats::update_stream_active(
    true, "DoctorFreshTelemetry", "203.0.113.26"
  );
  stream_stats::update_video_stats(
    60.0, 20000, 5.0, "hevc", 1920, 1080
  );
  for (int i = 0; i < 6; ++i) {
    stream_stats::update_network_stats(5.0, 0.0, 1000);
  }
  for (int i = 0; i < 3; ++i) {
    stream_stats::update_network_stats(52.0, 3.4, 1000);
  }

  constexpr std::uint64_t generation = 426;
  doctor_actions::session_started(
    "client-owner", generation, "launch-426", 20000
  );
  adaptive_bitrate::set_runtime_update_supported(true, {}, 20000);
  stream_stats::start_session_timing(
    "client-owner", generation, "launch-426"
  );

  doctor_actions::recovery_action_context_t context;
  context.active_owner = true;
  context.host_tuning_allowed = true;
  context.enforce_request_scope = true;
  context.owner_uuid = "client-owner";
  context.app_uuid = "game-owner";
  context.launch_instance_id = "launch-426";
  context.session_generation = generation;
  context.stats = stream_stats::get_current();
  ASSERT_TRUE(context.stats.network_risk);
  const auto request = trusted_doctor_action_request(context);
  ASSERT_EQ(request.at("action_id"), "lower_bitrate");
  ASSERT_EQ(request.at("target_bitrate_kbps"), 16000);

  const auto displayed_controller = adaptive_bitrate::get_doctor_state();
  const auto displayed_evidence_revision =
    context.stats.network_sample_revision;

  // A fresh clean control ping moves the host evidence epoch, but the current
  // confirmed media-loss sample remains fresh and derives the same guarded
  // 20 -> 16 Mbps action. This routine telemetry must not make a human-speed
  // button click stale.
  stream_stats::update_control_channel_stats(5.0, 0.0, 1000);
  const auto refreshed_stats = stream_stats::get_current();
  const auto refreshed_controller = adaptive_bitrate::get_doctor_state();
  ASSERT_GT(
    refreshed_stats.network_sample_revision,
    displayed_evidence_revision
  );
  ASSERT_GT(refreshed_controller.revision, displayed_controller.revision);
  ASSERT_EQ(
    refreshed_controller.action_authority_revision,
    displayed_controller.action_authority_revision
  );
  ASSERT_TRUE(refreshed_stats.network_risk);

  const auto applied = execute_with_encoder_ack(16000, [&] {
    return doctor_actions::execute(request, context);
  });
  ASSERT_TRUE(applied.at("status").get<bool>());
  EXPECT_TRUE(applied.at("changed").get<bool>());
  EXPECT_EQ(applied.at("state"), "applying");
  EXPECT_EQ(applied.at("requested").at("bitrate_kbps"), 16000);
  EXPECT_EQ(adaptive_bitrate::get_doctor_state().live_bitrate_kbps, 16000);

  const auto run_id = applied.at("run_id").get<std::string>();
  const auto undone = execute_with_encoder_ack(20000, [&] {
    return doctor_actions::execute({
      {"action_id", "undo"},
      {"run_id", run_id},
      {"app_session_id", "launch-426"},
      {"session_generation", generation}
    }, context);
  });
  ASSERT_TRUE(undone.at("status").get<bool>());
  EXPECT_EQ(undone.at("state"), "undone");
  EXPECT_EQ(undone.at("restored_bitrate_kbps"), 20000);

  doctor_actions::session_ended("client-owner", generation);
  stream_stats::stop_session_timing("client-owner", generation);
  stream_stats::update_stream_active(false);
}

TEST(DoctorActionTests, EveryRequestIdRemainsIdempotentForTheWholeStreamGeneration) {
  config::video.adaptive_bitrate.enabled = false;
  config::video.adaptive_bitrate.min_bitrate_kbps = 2000;
  config::video.adaptive_bitrate.max_bitrate_kbps = 100000;
  adaptive_bitrate::load_config();
  adaptive_bitrate::reset();
  stream_stats::update_stream_active(true, "DoctorIdempotencyHistory", "203.0.113.25");
  stream_stats::update_video_stats(60.0, 20000, 5.0, "hevc", 1920, 1080);
  for (int i = 0; i < 6; ++i) {
    stream_stats::update_network_stats(5.0, 0.0, 1000);
  }
  for (int i = 0; i < 3; ++i) {
    stream_stats::update_network_stats(52.0, 3.4, 1000);
  }

  constexpr std::uint64_t generation = 423;
  doctor_actions::session_started("client-owner", generation, "launch-423", 20000);
  adaptive_bitrate::set_runtime_update_supported(true, {}, 20000);
  stream_stats::start_session_timing("client-owner", generation, "launch-423");

  doctor_actions::recovery_action_context_t context;
  context.active_owner = true;
  context.host_tuning_allowed = true;
  context.enforce_request_scope = true;
  context.owner_uuid = "client-owner";
  context.app_uuid = "game-owner";
  context.launch_instance_id = "launch-423";
  context.session_generation = generation;

  nlohmann::json first_request;
  for (int i = 0; i < 128; ++i) {
    context.stats = stream_stats::get_current();
    const auto request = trusted_doctor_action_request(context);
    if (i == 0) first_request = request;
    const auto applied = doctor_actions::execute(request, context);
    ASSERT_TRUE(applied.at("status").get<bool>());
    ASSERT_EQ(applied.at("state"), "applying");
    ASSERT_TRUE(doctor_actions::set_owner_live_bitrate(
      "client-owner", generation, "launch-423", 20000
    ));
  }

  context.stats = stream_stats::get_current();
  const auto over_capacity = doctor_actions::execute(
    trusted_doctor_action_request(context), context
  );
  EXPECT_FALSE(over_capacity.at("status").get<bool>());
  EXPECT_EQ(over_capacity.at("state"), "generation_action_limit");
  EXPECT_EQ(over_capacity.at("code"), "doctor_idempotency_capacity_reached");

  const auto oldest_retry = doctor_actions::execute(first_request, context);
  EXPECT_TRUE(oldest_retry.at("status").get<bool>());
  EXPECT_FALSE(oldest_retry.at("changed").get<bool>());
  EXPECT_EQ(oldest_retry.at("state"), "superseded");
  EXPECT_EQ(
    oldest_retry.at("request_id"),
    first_request.at("request_id")
  );
  EXPECT_EQ(adaptive_bitrate::get_doctor_state().live_bitrate_kbps, 20000);

  doctor_actions::session_ended("client-owner", generation);
  stream_stats::stop_session_timing("client-owner", generation);
  stream_stats::update_stream_active(false);
}

TEST(DoctorActionTests, AdaptiveToggleRestoresDoctorTargetBeforeChangingPolicy) {
  config::video.adaptive_bitrate.enabled = false;
  config::video.adaptive_bitrate.min_bitrate_kbps = 2000;
  config::video.adaptive_bitrate.max_bitrate_kbps = 100000;
  adaptive_bitrate::load_config();
  adaptive_bitrate::reset();
  stream_stats::update_stream_active(true, "DoctorAdaptiveToggle", "203.0.113.26");
  stream_stats::update_video_stats(60.0, 20000, 5.0, "hevc", 1920, 1080);
  for (int i = 0; i < 6; ++i) {
    stream_stats::update_network_stats(5.0, 0.0, 1000);
  }
  for (int i = 0; i < 3; ++i) {
    stream_stats::update_network_stats(52.0, 3.4, 1000);
  }

  constexpr std::uint64_t generation = 424;
  doctor_actions::session_started("client-owner", generation, "launch-424", 20000);
  adaptive_bitrate::set_runtime_update_supported(true, {}, 20000);
  stream_stats::start_session_timing("client-owner", generation, "launch-424");

  doctor_actions::recovery_action_context_t context;
  context.active_owner = true;
  context.host_tuning_allowed = true;
  context.enforce_request_scope = true;
  context.owner_uuid = "client-owner";
  context.app_uuid = "game-owner";
  context.launch_instance_id = "launch-424";
  context.session_generation = generation;
  context.stats = stream_stats::get_current();
  const auto request = trusted_doctor_action_request(context);
  const auto applied = execute_with_encoder_ack(16000, [&] {
    return doctor_actions::execute(request, context);
  });
  ASSERT_TRUE(applied.at("status").get<bool>());
  EXPECT_EQ(adaptive_bitrate::get_doctor_state().live_bitrate_kbps, 16000);

  auto guard = doctor_actions::acquire_paired_global_control(
    "client-owner", generation, "launch-424"
  );
  ASSERT_TRUE(static_cast<bool>(guard));
  std::atomic<bool> rollback_acknowledged {false};
  std::thread encoder([&] {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < deadline) {
      if (const auto pending = adaptive_bitrate::get_live_bitrate_request();
          pending && pending->target_bitrate_kbps == 20000) {
        adaptive_bitrate::acknowledge_live_bitrate_applied(
          pending->revision,
          pending->target_bitrate_kbps
        );
        rollback_acknowledged.store(true, std::memory_order_release);
        return;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
  });
  EXPECT_TRUE(guard.set_adaptive_enabled(true));
  guard.release();
  encoder.join();

  EXPECT_TRUE(rollback_acknowledged.load(std::memory_order_acquire));
  const auto restored = adaptive_bitrate::get_doctor_state();
  EXPECT_TRUE(restored.enabled);
  EXPECT_EQ(restored.base_bitrate_kbps, 20000);
  EXPECT_EQ(restored.live_bitrate_kbps, 20000);
  const auto stale_retry = doctor_actions::execute(request, context);
  EXPECT_TRUE(stale_retry.at("status").get<bool>());
  EXPECT_EQ(stale_retry.at("state"), "superseded");

  doctor_actions::set_adaptive_enabled(false);
  doctor_actions::session_ended("client-owner", generation);
  stream_stats::stop_session_timing("client-owner", generation);
  stream_stats::update_stream_active(false);
}

TEST(DoctorActionTests, GlobalControlAuthorizationSerializesStreamHandoff) {
  doctor_actions::session_started("client-owner", 431, "launch-431", 20000);
  auto guard = doctor_actions::acquire_paired_global_control(
    "client-owner", 431, "launch-431"
  );
  ASSERT_TRUE(static_cast<bool>(guard));

  std::atomic<bool> handoff_entered {false};
  std::atomic<bool> handoff_finished {false};
  std::thread handoff([&] {
    handoff_entered.store(true, std::memory_order_release);
    doctor_actions::session_started("client-viewer", 432, "launch-432", 18000);
    handoff_finished.store(true, std::memory_order_release);
  });
  while (!handoff_entered.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }
  std::this_thread::sleep_for(std::chrono::milliseconds(10));
  EXPECT_FALSE(handoff_finished.load(std::memory_order_acquire));

  guard.release();
  handoff.join();
  EXPECT_TRUE(handoff_finished.load(std::memory_order_acquire));

  auto stale_owner_guard = doctor_actions::acquire_paired_global_control(
    "client-owner", 431, "launch-431"
  );
  EXPECT_FALSE(static_cast<bool>(stale_owner_guard));
  doctor_actions::session_ended("client-owner", 431);
  doctor_actions::session_ended("client-viewer", 432);
}

TEST(DoctorActionTests, NewestStreamCannotAutoFixAfterTheOlderViewerLeaves) {
  stream_stats::update_stream_active(true, "DoctorSharedGeneration", "203.0.113.12");
  doctor_actions::session_started("client-viewer", 411, 18000);
  doctor_actions::session_started("client-owner", 412, 20000);
  doctor_actions::session_ended("client-viewer", 411);

  doctor_actions::recovery_action_context_t context;
  context.active_owner = true;
  context.host_tuning_allowed = true;
  context.owner_uuid = "client-owner";
  context.app_uuid = "game-owner";
  context.session_generation = 412;
  context.stats = stream_stats::get_current();

  const auto blocked = doctor_actions::execute({
    {"action_id", "lower_bitrate"}, {"request_id", "test-retired-viewer"}
  }, context);
  EXPECT_FALSE(blocked.at("status").get<bool>());
  EXPECT_EQ(blocked.at("state"), "scope_unavailable");
  EXPECT_FALSE(stream_stats::get_current().doctor_live_action_scope_available);

  doctor_actions::session_ended("client-owner", 412);
  stream_stats::update_stream_active(false);
}

TEST(StreamStatsDoctorTests, DuplicateOnlyStaticContentIsNotAFramePacingFault) {
  stream_stats::stats_t stats {};
  stats.streaming = true;
  stats.fps = 30.0;
  stats.encode_target_fps = 120.0;
  stats.capture_source_fps = 0.0;
  stats.duplicate_frame_ratio = 0.75;
  stats.frame_jitter_ms = 4.0;
  stats.dropped_frame_ratio = 0.0;
  stats.bitrate_kbps = 30000;
  stats.capture_transport = platf::frame_transport_e::dmabuf;
  stats.capture_residency = platf::frame_residency_e::gpu;
  stats.encode_target_residency = platf::frame_residency_e::gpu;

  const auto doctor = stream_stats::build_doctor_json(
    stats,
    {{"primary_issue", "frame_pacing"}, {"grade", "watch"}}
  );

  EXPECT_EQ(doctor.at("primary_issue"), "none");
  EXPECT_EQ(doctor.at("traffic_light"), "green");
  EXPECT_EQ(doctor.at("status"), "ok");
  EXPECT_EQ(doctor.at("safe_recovery_action").at("id"), "none");
  const auto &gap = *std::find_if(
    doctor.at("evidence").begin(),
    doctor.at("evidence").end(),
    [](const auto &item) { return item.value("id", "") == "target_fps_gap"; }
  );
  EXPECT_EQ(gap.at("status"), "unknown");
}

TEST(StreamStatsDoctorTests, StaticContentCannotHideConfirmedDroppedFrames) {
  stream_stats::stats_t stats {};
  stats.streaming = true;
  stats.fps = 30.0;
  stats.encode_target_fps = 120.0;
  stats.capture_source_fps = 0.0;
  stats.duplicate_frame_ratio = 0.75;
  stats.frame_jitter_ms = 4.0;
  stats.dropped_frame_ratio = 0.08;
  stats.bitrate_kbps = 30000;
  stats.capture_transport = platf::frame_transport_e::dmabuf;
  stats.capture_residency = platf::frame_residency_e::gpu;
  stats.encode_target_residency = platf::frame_residency_e::gpu;

  const auto doctor = stream_stats::build_doctor_json(
    stats,
    {{"primary_issue", "frame_pacing"}, {"grade", "watch"}}
  );

  EXPECT_EQ(doctor.at("primary_issue"), "frame_pacing");
  EXPECT_EQ(doctor.at("traffic_light"), "amber");
  EXPECT_EQ(doctor.at("status"), "needs_action");
  EXPECT_NE(doctor.at("safe_recovery_action").at("id"), "lower_bitrate");
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
  stats.control_channel_samples = 1;
  stats.network_sample_revision = 1;
  stats.network_last_received_age_ms = 0;
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

TEST(StreamStatsHotFieldTests, CaptureCadenceTelemetrySeparatesSourceFromEncoder) {
  stream_stats::update_video_stats(118.9, 24000, 3.25, "hevc", 1920, 1080);
  stream_stats::update_capture_source_fps(119.7);
  stream_stats::update_capture_pacing("source_driven");
  stream_stats::update_runtime_state({
    .requested_headless = true,
    .effective_headless = true,
    .gpu_native_override_active = false,
    .backend_name = "labwc",
    .path_id = "headless_stream",
    .reported_output_refresh_hz = 120.0,
  });

  const auto stats = stream_stats::get_current();

  EXPECT_DOUBLE_EQ(stats.runtime_reported_refresh_hz, 120.0);
  EXPECT_DOUBLE_EQ(stats.capture_source_fps, 119.7);
  EXPECT_DOUBLE_EQ(stats.fps, 118.9);
  EXPECT_EQ(stats.capture_pacing, "source_driven");

  stream_stats::update_stream_active(false);
}

TEST(StreamStatsHotFieldTests, UpdateNetworkStatsIsVisibleThroughGetCurrent) {
  stream_stats::update_network_stats(11.5, 0.4, 123456789ull);

  const auto stats = stream_stats::get_current();

  EXPECT_DOUBLE_EQ(stats.latency_ms, 11.5);
  EXPECT_DOUBLE_EQ(stats.packet_loss, 0.4);
  EXPECT_TRUE(stats.packet_loss_available);
  EXPECT_EQ(stats.packet_loss_source, "media_transport");
  EXPECT_EQ(stats.bytes_sent, 123456789ull);

  stream_stats::update_stream_active(false);
}

TEST(StreamStatsHotFieldTests, ClientMediaCountersPublishHostScopedDerivedLoss) {
  adaptive_bitrate::set_enabled(false);
  stream_stats::update_stream_active(false);
  stream_stats::start_session_timing("owner-a", 41, "app-session-a");
  stream_stats::update_stream_active(true);
  stream_stats::update_control_channel_stats(12.0, 9.0, 777);

  stream_stats::client_media_counters_t sample {
    .owner_uuid = "owner-a",
    .app_session_id = "app-session-a",
    .session_generation = 41,
    .client_monotonic_ms = 1'000,
    .frames_expected = 100,
    .frames_received = 100,
    .frames_lost = 0
  };
  const auto baseline = stream_stats::ingest_client_media_counters(sample);
  EXPECT_TRUE(baseline.accepted);
  EXPECT_FALSE(baseline.observation_published);
  EXPECT_EQ(baseline.state, stream_stats::client_media_ingest_state_e::baseline);
  EXPECT_FALSE(stream_stats::get_current().packet_loss_available);

  sample.client_monotonic_ms = 2'000;
  sample.frames_expected = 200;
  sample.frames_received = 196;
  sample.frames_lost = 4;
  const auto observed = stream_stats::ingest_client_media_counters(sample);
  EXPECT_TRUE(observed.accepted);
  EXPECT_TRUE(observed.observation_published);
  EXPECT_EQ(observed.state, stream_stats::client_media_ingest_state_e::observed);
  EXPECT_DOUBLE_EQ(observed.media_loss_pct, 4.0);

  const auto stats = stream_stats::get_current();
  EXPECT_DOUBLE_EQ(stats.latency_ms, 12.0)
    << "client media telemetry must preserve host-observed RTT";
  EXPECT_DOUBLE_EQ(stats.packet_loss, 4.0);
  EXPECT_TRUE(stats.packet_loss_available);
  EXPECT_EQ(stats.packet_loss_source, "media_transport");
  EXPECT_EQ(stats.bytes_sent, 777u);

  const auto replay = stream_stats::ingest_client_media_counters(sample);
  EXPECT_FALSE(replay.accepted);
  EXPECT_EQ(replay.state, stream_stats::client_media_ingest_state_e::non_monotonic);

  stream_stats::stop_session_timing("owner-a", 41);
  stream_stats::update_stream_active(false);
}

TEST(StreamStatsHotFieldTests, ConcurrentClientMediaIngestAndStreamResetLeaveNoStaleEvidence) {
  adaptive_bitrate::set_enabled(false);

  for (std::uint64_t round = 1; round <= 64; ++round) {
    stream_stats::update_stream_active(false);
    stream_stats::start_session_timing(
      "owner-concurrent-reset",
      round,
      "app-session-concurrent-reset"
    );
    stream_stats::update_stream_active(true);

    stream_stats::client_media_counters_t sample {
      .owner_uuid = "owner-concurrent-reset",
      .app_session_id = "app-session-concurrent-reset",
      .session_generation = round,
      .client_monotonic_ms = 1'000,
      .frames_expected = 100,
      .frames_received = 100,
      .frames_lost = 0
    };
    ASSERT_TRUE(stream_stats::ingest_client_media_counters(sample).accepted);

    sample.client_monotonic_ms = 2'000;
    sample.frames_expected = 200;
    sample.frames_received = 180;
    sample.frames_lost = 20;

    std::atomic<bool> start {false};
    std::thread ingest_thread([&] {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      (void) stream_stats::ingest_client_media_counters(sample);
    });
    std::thread reset_thread([&] {
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }
      stream_stats::update_stream_active(false);
    });

    start.store(true, std::memory_order_release);
    ingest_thread.join();
    reset_thread.join();

    const auto stats = stream_stats::get_current();
    EXPECT_FALSE(stats.streaming);
    EXPECT_FALSE(stats.packet_loss_available);
    EXPECT_DOUBLE_EQ(stats.packet_loss, 0.0);
    stream_stats::stop_session_timing("owner-concurrent-reset", round);
  }
}

TEST(StreamStatsHotFieldTests, StaleGenerationCannotRebaselineOrPublishAfterReconnect) {
  adaptive_bitrate::set_enabled(false);
  stream_stats::update_stream_active(false);
  stream_stats::start_session_timing("owner-reconnect", 71, "app-session-old");
  stream_stats::update_stream_active(true);

  stream_stats::client_media_counters_t stale {
    .owner_uuid = "owner-reconnect",
    .app_session_id = "app-session-old",
    .session_generation = 71,
    .client_monotonic_ms = 1'000,
    .frames_expected = 100,
    .frames_received = 100,
    .frames_lost = 0
  };
  ASSERT_TRUE(stream_stats::ingest_client_media_counters(stale).accepted);

  stream_stats::stop_session_timing("owner-reconnect", 71);
  stream_stats::update_stream_active(false);
  stream_stats::start_session_timing("owner-reconnect", 72, "app-session-new");
  stream_stats::update_stream_active(true);

  stale.client_monotonic_ms = 2'000;
  stale.frames_expected = 200;
  stale.frames_received = 180;
  stale.frames_lost = 20;
  const auto first_stale = stream_stats::ingest_client_media_counters(stale);
  EXPECT_FALSE(first_stale.accepted);
  EXPECT_FALSE(first_stale.observation_published);
  EXPECT_EQ(
    first_stale.state,
    stream_stats::client_media_ingest_state_e::scope_mismatch
  );

  stale.client_monotonic_ms = 3'000;
  stale.frames_expected = 300;
  stale.frames_received = 270;
  stale.frames_lost = 30;
  const auto second_stale = stream_stats::ingest_client_media_counters(stale);
  EXPECT_FALSE(second_stale.accepted);
  EXPECT_FALSE(second_stale.observation_published);
  EXPECT_EQ(
    second_stale.state,
    stream_stats::client_media_ingest_state_e::scope_mismatch
  );

  const auto stats = stream_stats::get_current();
  EXPECT_TRUE(stats.streaming);
  EXPECT_FALSE(stats.packet_loss_available);
  EXPECT_DOUBLE_EQ(stats.packet_loss, 0.0);

  stream_stats::stop_session_timing("owner-reconnect", 72);
  stream_stats::update_stream_active(false);
}

TEST(StreamStatsHotFieldTests, CleanControlPingsCannotEraseCurrentConfirmedMediaLoss) {
  adaptive_bitrate::set_enabled(false);
  adaptive_bitrate::set_runtime_update_supported(true);
  stream_stats::update_stream_active(false);
  stream_stats::start_session_timing("owner-interleaved", 42, "app-session-interleaved");
  stream_stats::update_stream_active(true);
  stream_stats::set_doctor_live_action_scope_available(true);

  // Arm the RTT warm-up with a proven-calm LAN before media loss begins.
  for (int i = 0; i < 6; ++i) {
    stream_stats::update_control_channel_stats(4.0, 0.0, 777);
  }

  stream_stats::client_media_counters_t sample {
    .owner_uuid = "owner-interleaved",
    .app_session_id = "app-session-interleaved",
    .session_generation = 42,
    .client_monotonic_ms = 1'000,
    .frames_expected = 1'000,
    .frames_received = 1'000,
    .frames_lost = 0
  };
  ASSERT_TRUE(stream_stats::ingest_client_media_counters(sample).accepted);

  sample.client_monotonic_ms = 2'000;
  sample.frames_expected = 1'100;
  sample.frames_received = 1'080;
  sample.frames_lost = 20;
  const auto loss = stream_stats::ingest_client_media_counters(sample);
  ASSERT_TRUE(loss.accepted);
  ASSERT_TRUE(loss.observation_published);
  ASSERT_DOUBLE_EQ(loss.media_loss_pct, 20.0);

  // Production pings arrive between Nova's one-second counter reports. They
  // carry no media-loss measurement, so their clean control-channel loss must
  // not be interpreted as clean video delivery.
  stream_stats::update_control_channel_stats(4.0, 0.0, 777);
  stream_stats::update_control_channel_stats(4.0, 0.0, 777);

  const auto stats = stream_stats::get_current();
  EXPECT_TRUE(stats.network_risk);
  EXPECT_TRUE(stats.packet_loss_available);
  EXPECT_DOUBLE_EQ(stats.packet_loss, 20.0);
  EXPECT_EQ(stats.packet_loss_source, "media_transport");

  const auto doctor = stream_stats::build_doctor_json(
    stats,
    nlohmann::json::object()
  );
  EXPECT_EQ(doctor.at("primary_issue"), "network_jitter");

  adaptive_bitrate::set_runtime_update_supported(false);
  stream_stats::stop_session_timing("owner-interleaved", 42);
  stream_stats::update_stream_active(false);
}

TEST(StreamStatsHotFieldTests, ClientMediaCounterRestartNeedsANewBaseline) {
  adaptive_bitrate::set_enabled(false);
  stream_stats::update_stream_active(false);
  stream_stats::start_session_timing("owner-b", 52, "app-session-b");
  stream_stats::update_stream_active(true);

  stream_stats::client_media_counters_t sample {
    .owner_uuid = "owner-b",
    .app_session_id = "app-session-b",
    .session_generation = 52,
    .client_monotonic_ms = 1'000,
    .frames_expected = 1'000,
    .frames_received = 990,
    .frames_lost = 10
  };
  ASSERT_TRUE(stream_stats::ingest_client_media_counters(sample).accepted);

  sample.client_monotonic_ms = 2'000;
  sample.frames_expected = 20;
  sample.frames_received = 20;
  sample.frames_lost = 0;
  const auto reset = stream_stats::ingest_client_media_counters(sample);
  EXPECT_TRUE(reset.accepted);
  EXPECT_FALSE(reset.observation_published);
  EXPECT_EQ(reset.state, stream_stats::client_media_ingest_state_e::counter_epoch_reset);

  sample.client_monotonic_ms = 3'000;
  sample.frames_expected = 120;
  sample.frames_received = 120;
  const auto clean = stream_stats::ingest_client_media_counters(sample);
  EXPECT_TRUE(clean.observation_published);
  EXPECT_DOUBLE_EQ(clean.media_loss_pct, 0.0);

  stream_stats::stop_session_timing("owner-b", 52);
  stream_stats::update_stream_active(false);
}

TEST(StreamStatsHotFieldTests, ClientMediaCoverageGapCannotRefreshOldLoss) {
  adaptive_bitrate::set_enabled(false);
  stream_stats::update_stream_active(false);
  stream_stats::start_session_timing("owner-c", 63, "app-session-c");
  stream_stats::update_stream_active(true);

  stream_stats::client_media_counters_t sample {
    .owner_uuid = "owner-c",
    .app_session_id = "app-session-c",
    .session_generation = 63,
    .client_monotonic_ms = 1'000,
    .frames_expected = 100,
    .frames_received = 100,
    .frames_lost = 0
  };
  ASSERT_TRUE(stream_stats::ingest_client_media_counters(sample).accepted);

  stream_stats::age_client_media_counter_baseline_for_tests(
    std::chrono::seconds(6)
  );
  sample.client_monotonic_ms = 2'000;
  sample.frames_expected = 200;
  sample.frames_received = 190;
  sample.frames_lost = 10;
  const auto gap = stream_stats::ingest_client_media_counters(sample);
  EXPECT_TRUE(gap.accepted);
  EXPECT_FALSE(gap.observation_published);
  EXPECT_EQ(gap.state, stream_stats::client_media_ingest_state_e::coverage_gap_reset);
  EXPECT_FALSE(stream_stats::get_current().packet_loss_available);

  sample.client_monotonic_ms = 3'000;
  sample.frames_expected = 300;
  sample.frames_received = 290;
  const auto current = stream_stats::ingest_client_media_counters(sample);
  EXPECT_TRUE(current.observation_published);
  EXPECT_DOUBLE_EQ(current.media_loss_pct, 0.0);

  stream_stats::stop_session_timing("owner-c", 63);
  stream_stats::update_stream_active(false);
}

TEST(StreamStatsHotFieldTests, ControlChannelLossNeverBecomesMediaLossOrRisk) {
  stream_stats::update_stream_active(false);
  stream_stats::update_stream_active(true);
  for (int i = 0; i < 50; ++i) {
    stream_stats::update_control_channel_stats(4.0, 8.72039794921875, 0);
  }

  const auto stats = stream_stats::get_current();

  EXPECT_DOUBLE_EQ(stats.latency_ms, 4.0);
  EXPECT_DOUBLE_EQ(stats.packet_loss, 0.0);
  EXPECT_FALSE(stats.packet_loss_available);
  EXPECT_EQ(stats.packet_loss_source, "unavailable");
  EXPECT_DOUBLE_EQ(stats.control_channel_packet_loss, 8.72039794921875);
  EXPECT_EQ(stats.control_channel_samples, 50);
  EXPECT_FALSE(stats.network_risk);

  stream_stats::update_stream_active(false);
}

// The periodic-ping handler in stream.cpp converts ENet's scaled control loss
// to percent for diagnostics, while keeping it out of media grading.
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
  stream_stats::start_session_timing(uuid, 7, "launch-token-7");

  const auto identity = stream_stats::get_single_active_session_identity();
  ASSERT_TRUE(identity.has_value());
  EXPECT_EQ(identity->device_uuid, uuid);
  EXPECT_EQ(identity->session_generation, 7u);
  EXPECT_EQ(identity->session_token, "launch-token-7");

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
