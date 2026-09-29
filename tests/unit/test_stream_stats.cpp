/**
 * @file tests/unit/test_stream_stats.cpp
 * @brief Test src/stream_stats.*.
 */

#include <src/stream_stats.h>
#include <src/stream_bitrate.h>
#include <src/config.h>
#include <src/configuration_store.h>
#include <src/platform/common.h>
#include <src/doctor_actions.h>
#include <src/adaptive_bitrate.h>
#include <src/live_tuning.h>
#include <src/private_state_file.h>
#include <src/utility.h>
#include "../tests_events.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#ifdef __linux__
  #include <src/platform/linux/kms_capture_readiness.h>
  #include <src/platform/linux/misc.h>
  #include <src/platform/linux/stream_display_policy.h>
  #include <src/platform/linux/user_unit_override.h>
  #include <src/platform/linux/virtual_display.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

namespace {
  struct LinuxDisplayConfigGuard {
    LinuxDisplayConfigGuard():
        adapter_name {config::video.adapter_name},
        capture {config::video.capture},
        encoder {config::video.encoder},
        headless_mode {config::video.linux_display.headless_mode},
        use_cage_compositor {config::video.linux_display.use_cage_compositor},
        prefer_gpu_native_capture {config::video.linux_display.prefer_gpu_native_capture},
        stream_mode {config::video.linux_display.stream_mode} {
    }

    ~LinuxDisplayConfigGuard() {
      config::video.adapter_name = adapter_name;
      config::video.capture = capture;
      config::video.encoder = encoder;
      config::video.linux_display.headless_mode = headless_mode;
      config::video.linux_display.use_cage_compositor = use_cage_compositor;
      config::video.linux_display.prefer_gpu_native_capture = prefer_gpu_native_capture;
      config::video.linux_display.stream_mode = stream_mode;
#ifdef __linux__
      platf::set_selected_capture_backend_for_tests(std::nullopt);
      platf::set_effective_encoder_render_device_for_tests(std::string {});
#endif
      stream_stats::set_build_has_cuda_for_tests(std::nullopt);
    }

    std::string adapter_name;
    std::string capture;
    std::string encoder;
    bool headless_mode;
    bool use_cage_compositor;
    bool prefer_gpu_native_capture;
    std::string stream_mode;
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

  struct LiveConfigurationGuard {
    std::string old = config::sunshine.config_file;
    std::filesystem::path directory = std::filesystem::temp_directory_path() /
      ("polaris-live-config-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    LiveConfigurationGuard() {
      std::filesystem::create_directory(directory);
      config::sunshine.config_file = (directory / "polaris.conf").string();
      (void) private_state_file::write_atomic(config::sunshine.config_file, "adaptive_bitrate_enabled = disabled\n");
    }
    ~LiveConfigurationGuard() {
      config::sunshine.config_file = old;
      std::filesystem::remove_all(directory);
    }
  };

  void mark_doctor_pacing_window_confirmed(stream_stats::stats_t &stats) {
    stats.video_policy_sample_count = stream_stats::DOCTOR_PACING_WARMUP_SAMPLES;
    stats.pacing_warning_streak = stream_stats::DOCTOR_PACING_CONFIRMATION_SAMPLES;
  }

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
  stream_stats::set_build_has_cuda_for_tests(true);

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

#if defined(__linux__) && defined(POLARIS_BUILD_VULKAN)
TEST(StreamStatsLinuxGpuProfileTests, PreservesExplicitAmdVaapiCompatibilityChoice) {
  LinuxDisplayConfigGuard guard;
  config::video.encoder = "vaapi";
  config::video.linux_display.use_cage_compositor = true;

  stream_stats::stats_t stats {};
  stats.runtime_requested_headless = true;
  stats.runtime_effective_headless = true;
  stats.capture_transport = platf::frame_transport_e::shm;
  stats.capture_residency = platf::frame_residency_e::cpu;
  stats.encode_target_device = "vaapi";
  stats.encode_target_residency = platf::frame_residency_e::gpu;
  stats.vaapi_vendor = "Mesa Gallium driver for AMD Radeon (radeonsi)";

  const auto profile = stream_stats::linux_gpu_profile_json(stats);
  const auto &warnings = profile.at("configuration_warnings");
  const auto recommendation = std::find_if(warnings.begin(), warnings.end(), [](const auto &warning) {
    return warning.value("id", std::string {}) == "amd_private_explicit_vaapi_shm";
  });

  ASSERT_NE(recommendation, warnings.end());
  EXPECT_EQ(recommendation->at("severity"), "info");
  const auto action = recommendation->at("action").get<std::string>();
  EXPECT_NE(action.find("Keep VA-API selected"), std::string::npos);
  EXPECT_NE(action.find("reduce resolution, frame rate, or bitrate"), std::string::npos);
  EXPECT_EQ(action.find("Autodetect"), std::string::npos);
  EXPECT_EQ(action.find("will live-probe"), std::string::npos);
  EXPECT_EQ(config::video.encoder, "vaapi");
}
#endif

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

TEST(StreamStatsCapturePathTests, CpuEncoderInputDoesNotInventGpuCapture) {
  stream_stats::stats_t stats {};
  stats.encode_target_residency = platf::frame_residency_e::cpu;
  const auto reason = stream_stats::capture_path_reason(stats);
  EXPECT_EQ(reason, "encoder_upload_cpu");
  EXPECT_EQ(stream_stats::capture_path_reason_message(reason).find("GPU-resident"), std::string::npos);
  EXPECT_FALSE(stream_stats::capture_path_is_gpu_native(stats));

  stats.capture_transport = platf::frame_transport_e::internal;
  stats.capture_residency = platf::frame_residency_e::cpu;
  stats.capture_format = platf::frame_format_e::bgra8;
  EXPECT_EQ(stream_stats::capture_path_reason(stats), "cpu_capture");
}

TEST(StreamStatsCapturePathTests, CaptureFallbackTransitionsInvalidateAuthorityWithoutBlockingHealthyRestore) {
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
  EXPECT_FALSE(adaptive_bitrate::doctor_policy_blocks_quality_restore());
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
  EXPECT_FALSE(adaptive_bitrate::doctor_policy_blocks_quality_restore());

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


TEST(StreamStatsCaptureSourceTests, KeepsReconnectAndIndependentClientEvidenceSeparate) {
  stream_stats::update_stream_active(false);
  auto reset = util::fail_guard([] { stream_stats::update_stream_active(false); });
  const stream_stats::capture_source_t large {3840, 2160, 1920, 1080,
    platf::frame_transport_e::shm, platf::frame_residency_e::cpu};
  const stream_stats::capture_source_t small {1280, 800, 1280, 800,
    platf::frame_transport_e::dmabuf, platf::frame_residency_e::gpu};
  EXPECT_FALSE(stream_stats::record_capture_source(0, large));
  EXPECT_FALSE(stream_stats::record_capture_source(101, large));
  stream_stats::add_client("same-client", "First", 101);
  stream_stats::add_client("same-client", "Replacement", 102);
  ASSERT_TRUE(stream_stats::record_capture_source(101, large));
  ASSERT_TRUE(stream_stats::record_capture_source(102, small));
  auto json = nlohmann::json::parse(stream_stats::get_current().to_json());
  EXPECT_EQ(json["capture_source"]["width"], 3840);
  EXPECT_EQ(json["clients"][1]["capture_source"]["width"], 1280);

  stream_stats::remove_client("same-client", 101);
  EXPECT_FALSE(stream_stats::record_capture_source(101, large));
  json = nlohmann::json::parse(stream_stats::get_current().to_json());
  EXPECT_EQ(json["capture_source"]["width"], 1280);
  EXPECT_EQ(json["capture_source"]["pixel_ratio"], 1.0);
  stream_stats::remove_client("same-client", 102);
  stream_stats::add_client("same-client", "New generation", 103);
  EXPECT_FALSE(stream_stats::record_capture_source(102, small));
  EXPECT_TRUE(nlohmann::json::parse(stream_stats::get_current().to_json())["capture_source"].is_null());
}

TEST(StreamStatsCaptureSourceTests, RecordsRenegotiationAndRejectsInvalidSizes) {
  stream_stats::update_stream_active(false);
  auto reset = util::fail_guard([] { stream_stats::update_stream_active(false); });
  stream_stats::add_client("client", "Client", 201);
  stream_stats::capture_source_t source {3840, 2160, 1920, 1080,
    platf::frame_transport_e::shm, platf::frame_residency_e::cpu};
  ASSERT_TRUE(stream_stats::record_capture_source(201, source));
  source.width = 1920; source.height = 1080;
  source.transport = platf::frame_transport_e::dmabuf;
  source.residency = platf::frame_residency_e::gpu;
  ASSERT_TRUE(stream_stats::record_capture_source(201, source));
  for (int field = 0; field < 4; ++field) {
    auto invalid = source;
    const std::array<int *, 4> dimensions {&invalid.width, &invalid.height, &invalid.stream_width, &invalid.stream_height};
    *dimensions[field] = field % 2 ? -1 : 0;
    EXPECT_FALSE(stream_stats::record_capture_source(201, invalid));
  }
  const auto json = nlohmann::json::parse(stream_stats::get_current().to_json());
  EXPECT_EQ(json["capture_source"]["width"], 1920);
  EXPECT_EQ(json["capture_source"]["height"], 1080);
  EXPECT_EQ(json["capture_source"]["transport"], "dmabuf");
  EXPECT_EQ(json["capture_source"]["residency"], "gpu");
  EXPECT_EQ(json["capture_source"]["pixel_ratio"], 1.0);
}

TEST(StreamStatsCaptureSourceTests, ExplainsExtraCpuPixelsWithoutChangingDoctorVerdictOrAction) {
  stream_stats::stats_t stats {};
  stats.streaming = true;
  stats.capture_transport = platf::frame_transport_e::dmabuf;
  stats.capture_residency = platf::frame_residency_e::gpu;
  stats.encode_target_residency = platf::frame_residency_e::gpu;
  stats.clients.emplace_back();
  const auto before = stream_stats::build_doctor_json(stats, nlohmann::json::object());
  // The observed source belongs to this client, independent of process-wide
  // capture fields that another capture path may have most recently published.
  stats.clients[0].capture_source = {3840, 2160, 1920, 1080,
    platf::frame_transport_e::shm, platf::frame_residency_e::cpu};
  const auto doctor = stream_stats::build_doctor_json(stats, nlohmann::json::object());
  const auto &evidence = doctor.at("evidence");
  const auto row = std::find_if(evidence.begin(), evidence.end(), [](const auto &item) {
    return item.value("id", "") == "capture_source_size";
  });
  ASSERT_NE(row, evidence.end());
  EXPECT_EQ(row->at("status"), "info");
  EXPECT_EQ(row->at("source"), "session_capture_frame");
  EXPECT_EQ(row->at("value").at("pixel_ratio"), 4.0);
  EXPECT_NE(row->at("detail").get<std::string>().find("CPU capture path"), std::string::npos);
  EXPECT_NE(row->at("detail").get<std::string>().find("not proof"), std::string::npos);
  for (const auto *key : {"primary_issue", "traffic_light", "status", "severity"}) EXPECT_EQ(doctor.at(key), before.at(key));
  EXPECT_EQ(doctor.at("safe_recovery_action").at("id"), before.at("safe_recovery_action").at("id"));
  EXPECT_EQ(doctor.at("safe_recovery_action").at("endpoint"), before.at("safe_recovery_action").at("endpoint"));
}

namespace {
  stream_stats::capture_backend_t portal_capture(std::string route = "portal_screencast", std::string fallback = {}) {
    return {"", "portal", "portal", std::move(route), "", std::move(fallback)};
  }

  const std::array<const char *, 6> k_capture_mirrors {
    "capture_backend_preference", "capture_backend_requested", "capture_backend_opened",
    "capture_backend_route", "capture_mode_override_reason", "capture_route_fallback_reason",
  };

  bool has_capture_mirror(const nlohmann::json &json) {
    return std::any_of(k_capture_mirrors.begin(), k_capture_mirrors.end(), [&json](const char *key) {
      return json.contains(key);
    });
  }

  nlohmann::json current_stats_json() {
    return nlohmann::json::parse(stream_stats::get_current().to_json());
  }
}  // namespace

TEST(StreamStatsCaptureBackendTests, TwoViewersOfOneDisplayEachCarryItsCaptureAndOnlyOneIsMirrored) {
  stream_stats::update_stream_active(false);
  auto reset = util::fail_guard([] { stream_stats::update_stream_active(false); });
  stream_stats::add_client("10.0.0.5", "Owner", 301);
  stream_stats::add_client("10.0.0.6", "Viewer", 302);
  // The viewer joined the display the owner opened, and publishes it for itself.
  const auto shared = portal_capture("portal_kwin_node");
  ASSERT_TRUE(stream_stats::record_capture_backend(301, shared));
  ASSERT_TRUE(stream_stats::record_capture_backend(302, shared));
  auto json = current_stats_json();
  ASSERT_EQ(json["clients"].size(), 2u);
  for (const auto &client : json["clients"]) {
    EXPECT_EQ(client["capture"]["opened"], "portal");
    EXPECT_EQ(client["capture"]["route"], "portal_kwin_node");
    EXPECT_EQ(client["capture"]["requested"], "portal");
  }
  // Two clients have two answers, and the first one's is no answer for both.
  EXPECT_FALSE(has_capture_mirror(json)) << json.dump();

  // The moment two become one, the one left is mirrored.
  stream_stats::remove_client("10.0.0.5", 301);
  json = current_stats_json();
  EXPECT_EQ(json["capture_backend_preference"], "");
  EXPECT_EQ(json["capture_backend_requested"], "portal");
  EXPECT_EQ(json["capture_backend_opened"], "portal");
  EXPECT_EQ(json["capture_backend_route"], "portal_kwin_node");
  EXPECT_FALSE(json.contains("capture_mode_override_reason")) << "absent when no rule set anything aside";
  EXPECT_FALSE(json.contains("capture_route_fallback_reason")) << "absent when the route is the one asked for";

  stream_stats::remove_client("10.0.0.6", 302);
  EXPECT_FALSE(has_capture_mirror(current_stats_json()));
}

TEST(StreamStatsCaptureBackendTests, EachSessionCarriesAnOpaqueInstanceIdentityAndALegacyEntryNone) {
  stream_stats::update_stream_active(false);
  auto reset = util::fail_guard([] { stream_stats::update_stream_active(false); });
  stream_stats::add_client("10.0.0.5", "Same address", 311);
  stream_stats::add_client("10.0.0.5", "Same address", 312);
  stream_stats::add_client("10.0.0.7", "Legacy");
  const auto json = current_stats_json();
  ASSERT_EQ(json["clients"].size(), 3u);
  const std::regex shape {"^([0-9a-f]{16})\\.([0-9]+)$"};
  std::smatch first;
  std::smatch second;
  const auto first_id = json["clients"][0].value("stream_instance_id", "");
  const auto second_id = json["clients"][1].value("stream_instance_id", "");
  ASSERT_TRUE(std::regex_match(first_id, first, shape)) << first_id;
  ASSERT_TRUE(std::regex_match(second_id, second, shape)) << second_id;
  EXPECT_EQ(first[2], "311");
  EXPECT_EQ(second[2], "312");
  EXPECT_EQ(first[1], second[1]) << "one value is drawn per process, not per session";
  // Generation zero has no owner, so no identity is made up for it.
  EXPECT_FALSE(json["clients"][2].contains("stream_instance_id")) << json["clients"][2].dump();
  EXPECT_EQ(stream_stats::stream_instance_id(0), "");
  // The support export redacts a field named session_id as a Web UI credential.
  for (const auto &client : json["clients"]) {
    EXPECT_FALSE(client.contains("session_id"));
  }
}

TEST(StreamStatsCaptureBackendTests, OverlappingReconnectsFromOneAddressKeepTheirOwnCapture) {
  stream_stats::update_stream_active(false);
  auto reset = util::fail_guard([] { stream_stats::update_stream_active(false); });
  stream_stats::add_client("10.0.0.5", "Leaving", 321);
  stream_stats::add_client("10.0.0.5", "Replacement", 322);
  ASSERT_TRUE(stream_stats::record_capture_backend(321, portal_capture("portal_kwin_node")));
  ASSERT_TRUE(stream_stats::record_capture_backend(322, portal_capture("portal_screencast", "kwin_node_unavailable")));
  stream_stats::remove_client("10.0.0.5", 321);
  EXPECT_FALSE(stream_stats::record_capture_backend(321, portal_capture("portal_kwin_node")))
    << "a retired generation writes nothing, whatever address it shares";
  const auto json = current_stats_json();
  ASSERT_EQ(json["clients"].size(), 1u);
  EXPECT_EQ(json["clients"][0]["name"], "Replacement");
  EXPECT_EQ(json["clients"][0]["capture"]["route"], "portal_screencast");
  EXPECT_EQ(json["clients"][0]["capture"]["route_fallback_reason"], "kwin_node_unavailable");
  EXPECT_EQ(json["capture_backend_route"], "portal_screencast");
  EXPECT_EQ(json["capture_route_fallback_reason"], "kwin_node_unavailable");
}

TEST(StreamStatsCaptureBackendTests, APublicationBeforeTheSessionRegistersIsRefusedUntilARetryLands) {
  stream_stats::update_stream_active(false);
  auto reset = util::fail_guard([] { stream_stats::update_stream_active(false); });
  // The video thread opens the display before the session calls add_client.
  EXPECT_FALSE(stream_stats::record_capture_backend(331, portal_capture()));
  stream_stats::add_client("10.0.0.5", "Client", 331);
  auto json = current_stats_json();
  EXPECT_FALSE(json["clients"][0].contains("capture")) << "missing is unknown, never a default";
  EXPECT_FALSE(has_capture_mirror(json));
  ASSERT_TRUE(stream_stats::record_capture_backend(331, portal_capture()));
  json = current_stats_json();
  EXPECT_EQ(json["clients"][0]["capture"]["opened"], "portal");
  EXPECT_EQ(json["capture_backend_opened"], "portal");
  // A display that opened nothing has nothing to say, and generation zero belongs to no one.
  EXPECT_FALSE(stream_stats::record_capture_backend(331, stream_stats::capture_backend_t {}));
  EXPECT_FALSE(stream_stats::record_capture_backend(0, portal_capture()));
}

TEST(StreamStatsCaptureBackendTests, AnInitializationWithNoAcceptedFrameLeavesTheFramesUnknown) {
  stream_stats::update_stream_active(false);
  auto reset = util::fail_guard([] {
    stream_stats::update_capture_metadata({});
    stream_stats::update_stream_active(false);
  });
  stream_stats::add_client("10.0.0.5", "Client", 341);
  // Another stream's capture already filled the process-wide capture metadata. This client's
  // encoder has accepted no frame, so none of it is this client's.
  stream_stats::update_capture_metadata(platf::frame_metadata_t {
    .transport = platf::frame_transport_e::dmabuf,
    .residency = platf::frame_residency_e::gpu,
    .format = platf::frame_format_e::p010,
  });
  ASSERT_TRUE(stream_stats::record_capture_backend(341, portal_capture()));
  const auto json = current_stats_json();
  ASSERT_EQ(json["capture_transport"], "dmabuf") << "the process-wide value this client must not borrow";
  const auto &capture = json["clients"][0]["capture"];
  EXPECT_EQ(capture["transport"], "unknown");
  EXPECT_EQ(capture["residency"], "unknown");
  EXPECT_EQ(capture["format"], "unknown");
  EXPECT_TRUE(json["clients"][0]["capture_source"].is_null());
  EXPECT_FALSE(capture.contains("mode_override_reason"));
  EXPECT_FALSE(capture.contains("route_fallback_reason"));
}

TEST(StreamStatsCaptureBackendTests, NamingNvfbcNeverMakesUnknownTransferGpuNativeOrClearsAWarning) {
  stream_stats::stats_t stats {};
  stats.streaming = true;
  stats.clients.emplace_back();
  stats.clients[0].session_generation = 351;
  const auto before = nlohmann::json::parse(stats.to_json());
  const auto doctor_before = stream_stats::build_doctor_json(stats, nlohmann::json::object());
  const auto capture_path_evidence = [](const nlohmann::json &doctor) {
    for (const auto &entry : doctor.at("evidence")) {
      if (entry.at("id") == "capture_path") {
        return entry;
      }
    }
    return nlohmann::json {};
  };
  // The warning naming NvFBC must not clear is there to begin with.
  ASSERT_EQ(before["capture_path_reason"], "no_capture_metadata");
  ASSERT_EQ(capture_path_evidence(doctor_before).value("status", ""), "unknown");
  // NvFBC reports no transfer evidence of its own.
  stats.clients[0].capture_backend = {"nvfbc", "nvfbc", "nvfbc", "nvfbc", "", ""};
  const auto after = nlohmann::json::parse(stats.to_json());
  const auto doctor = stream_stats::build_doctor_json(stats, nlohmann::json::object());
  const auto &capture = after["clients"][0]["capture"];
  EXPECT_EQ(capture["opened"], "nvfbc");
  EXPECT_EQ(capture["transport"], "unknown");
  EXPECT_EQ(capture["residency"], "unknown");
  EXPECT_EQ(capture["format"], "unknown");
  EXPECT_FALSE(after["capture_gpu_native"].get<bool>());
  for (const auto *key : {"capture_path", "capture_path_reason", "capture_path_reason_message", "capture_gpu_native", "capture_cpu_copy"}) {
    EXPECT_EQ(after[key], before[key]) << key;
  }
  for (const auto *key : {"primary_issue", "traffic_light", "status", "severity"}) {
    EXPECT_EQ(doctor.at(key), doctor_before.at(key)) << key;
  }
  EXPECT_EQ(doctor.at("evidence"), doctor_before.at("evidence"));
  EXPECT_EQ(after["capture_path_reason"], "no_capture_metadata");
  EXPECT_EQ(capture_path_evidence(doctor).value("status", ""), "unknown");

  // Known process-wide metadata is some capture's, not evidence NvFBC reported for this client.
  stats.capture_transport = platf::frame_transport_e::dmabuf;
  stats.capture_residency = platf::frame_residency_e::gpu;
  stats.capture_format = platf::frame_format_e::p010;
  const auto with_known_metadata = nlohmann::json::parse(stats.to_json());
  const auto &borrowed = with_known_metadata["clients"][0]["capture"];
  EXPECT_EQ(borrowed["transport"], "unknown");
  EXPECT_EQ(borrowed["residency"], "unknown");
  EXPECT_EQ(borrowed["format"], "unknown");
}

// What this proves is the stats layer: a later write replaces an earlier one, a publication starts
// the frames over, and a format change is kept. It opens no display. That the host publishes again
// for each display it opens is held by
// SourceSafetyContracts.CaptureReadoutIsPublishedFromEveryEncodeLoopForEachDisplay.
TEST(StreamStatsCaptureBackendTests, ANewPublicationStartsTheFramesOverAndLaterWritesReplaceEarlierOnes) {
  stream_stats::update_stream_active(false);
  auto reset = util::fail_guard([] { stream_stats::update_stream_active(false); });
  stream_stats::add_client("10.0.0.5", "Client", 361);
  ASSERT_TRUE(stream_stats::record_capture_backend(361, portal_capture("portal_kwin_node")));
  stream_stats::capture_source_t source {1920, 1080, 1920, 1080,
    platf::frame_transport_e::dmabuf, platf::frame_residency_e::gpu, platf::frame_format_e::p010};
  ASSERT_TRUE(stream_stats::record_capture_source(361, source));
  auto capture = current_stats_json()["clients"][0]["capture"];
  EXPECT_EQ(capture["transport"], "dmabuf");
  EXPECT_EQ(capture["residency"], "gpu");
  EXPECT_EQ(capture["format"], "p010");

  // The display is opened again, on the ScreenCast this time. Until it delivers a frame, the frames
  // the last display delivered say nothing about it, and a stuck new display must not keep saying gpu.
  ASSERT_TRUE(stream_stats::record_capture_backend(361, portal_capture("portal_screencast", "kwin_node_failed")));
  auto json = current_stats_json();
  capture = json["clients"][0]["capture"];
  EXPECT_EQ(capture["route"], "portal_screencast");
  EXPECT_EQ(capture["transport"], "unknown");
  EXPECT_EQ(capture["residency"], "unknown");
  EXPECT_EQ(capture["format"], "unknown");
  // capture_source keeps the last frame any display delivered, as it did before capture existed.
  EXPECT_EQ(json["clients"][0]["capture_source"]["transport"], "dmabuf");

  // Its frames arrive in shared memory.
  source.transport = platf::frame_transport_e::shm;
  source.residency = platf::frame_residency_e::cpu;
  source.format = platf::frame_format_e::bgra8;
  ASSERT_TRUE(stream_stats::record_capture_source(361, source));
  capture = current_stats_json()["clients"][0]["capture"];
  EXPECT_EQ(capture["route"], "portal_screencast");
  EXPECT_EQ(capture["route_fallback_reason"], "kwin_node_failed");
  EXPECT_EQ(capture["transport"], "shm");
  EXPECT_EQ(capture["residency"], "cpu");
  EXPECT_EQ(capture["format"], "bgra8");
}

// The serializer's half: one record carrying both reasons says both, and mirrors both. That the
// policy's reason and the portal's fallback reach that record through the session's publication is
// StreamDisplayPolicyTests.AModeRewriteAndARouteFallbackReachTheSessionsPublishedCapture.
TEST(StreamStatsCaptureBackendTests, OneRecordWithBothReasonsSaysAndMirrorsBoth) {
  stream_stats::update_stream_active(false);
  auto reset = util::fail_guard([] { stream_stats::update_stream_active(false); });
  stream_stats::add_client("10.0.0.5", "Client", 371);
  // polaris.conf says kms, a Gamescope session asked for the portal, and the portal found no
  // gamescope node and took a ScreenCast.
  ASSERT_TRUE(stream_stats::record_capture_backend(371,
    {"kms", "portal", "portal", "portal_screencast", "gamescope_session", "gamescope_node_missing"}));
  const auto json = current_stats_json();
  const auto &capture = json["clients"][0]["capture"];
  EXPECT_EQ(capture["preference"], "kms");
  EXPECT_EQ(capture["requested"], "portal");
  EXPECT_EQ(capture["mode_override_reason"], "gamescope_session");
  EXPECT_EQ(capture["route_fallback_reason"], "gamescope_node_missing");
  EXPECT_EQ(json["capture_backend_preference"], "kms");
  EXPECT_EQ(json["capture_mode_override_reason"], "gamescope_session");
  EXPECT_EQ(json["capture_route_fallback_reason"], "gamescope_node_missing");
}

TEST(StreamStatsCaptureBackendTests, ReadoutWritesLeaveTheNetworkAndVideoPolicyRevisionsAlone) {
  stream_stats::update_stream_active(false);
  auto reset = util::fail_guard([] { stream_stats::update_stream_active(false); });
  stream_stats::add_client("10.0.0.5", "Client", 381);
  const auto before = stream_stats::get_current();
  const auto controller_before = adaptive_bitrate::get_doctor_state();
  ASSERT_TRUE(stream_stats::record_capture_backend(381, portal_capture()));
  ASSERT_TRUE(stream_stats::record_capture_source(381, {1920, 1080, 1920, 1080,
    platf::frame_transport_e::shm, platf::frame_residency_e::cpu, platf::frame_format_e::bgra8}));
  const auto after = stream_stats::get_current();
  const auto controller_after = adaptive_bitrate::get_doctor_state();
  EXPECT_EQ(after.video_sample_revision, before.video_sample_revision);
  EXPECT_EQ(after.network_sample_revision, before.network_sample_revision);
  EXPECT_EQ(after.video_policy_sample_count, before.video_policy_sample_count);
  EXPECT_EQ(after.pacing_warning_streak, before.pacing_warning_streak);
  EXPECT_EQ(controller_after.revision, controller_before.revision);
  EXPECT_EQ(controller_after.action_authority_revision, controller_before.action_authority_revision);
}

TEST(StreamStatsPyroWaveRouteTests, EachSessionCarriesItsOwnRouteAndKeepsItWhenItEnds) {
  stream_stats::update_stream_active(false);
  auto reset = util::fail_guard([] { stream_stats::update_stream_active(false); });
  stream_stats::add_client("10.0.0.5", "Deck", 391);
  stream_stats::add_client("10.0.0.6", "Tablet", 392);
  const auto before = stream_stats::get_current();
  const auto controller_before = adaptive_bitrate::get_doctor_state();

  EXPECT_FALSE(stream_stats::record_pyrowave_route(393, "zero_copy")) << "a generation no client holds";
  EXPECT_FALSE(stream_stats::record_pyrowave_route(0, "zero_copy")) << "generation zero belongs to no one";
  EXPECT_FALSE(stream_stats::record_pyrowave_route(391, "")) << "unknown is written as a route";
  ASSERT_TRUE(stream_stats::record_pyrowave_route(391, "zero_copy"));

  auto json = current_stats_json();
  ASSERT_EQ(json["clients"].size(), 2u);
  EXPECT_EQ(json["clients"][0].value("pyrowave_route", ""), "zero_copy");
  EXPECT_FALSE(json["clients"][1].contains("pyrowave_route")) << "one session's route landed on another's entry";

  // A readout write, so no policy revision moves.
  const auto after = stream_stats::get_current();
  const auto controller_after = adaptive_bitrate::get_doctor_state();
  EXPECT_EQ(after.video_sample_revision, before.video_sample_revision);
  EXPECT_EQ(after.network_sample_revision, before.network_sample_revision);
  EXPECT_EQ(after.video_policy_sample_count, before.video_policy_sample_count);
  EXPECT_EQ(controller_after.revision, controller_before.revision);
  EXPECT_EQ(controller_after.action_authority_revision, controller_before.action_authority_revision);

  // A GPU path that falls back changes the route, and the session ends with the one it had last.
  ASSERT_TRUE(stream_stats::record_pyrowave_route(391, "cpu_convert"));
  stream_stats::remove_client("10.0.0.5", 391);
  json = current_stats_json();
  ASSERT_TRUE(json.contains("last_session")) << json.dump();
  EXPECT_EQ(json["last_session"].value("pyrowave_route", ""), "cpu_convert");
  EXPECT_FALSE(stream_stats::record_pyrowave_route(391, "zero_copy")) << "a retired generation still writes";

  stream_stats::remove_client("10.0.0.6", 392);
  EXPECT_FALSE(current_stats_json()["last_session"].contains("pyrowave_route"))
    << "a session that never reported a route ended with another session's";
}

/**
 * A display opened again has encoded nothing, so the session's PyroWave route reads unknown until it
 * does, and a session that ends before then never pairs that display's unknown frames with the route
 * of the display before it.
 */
TEST(StreamStatsPyroWaveRouteTests, ADisplayOpenedAgainHasNoRouteUntilItsEncoderTakesAFrame) {
  stream_stats::update_stream_active(false);
  auto reset = util::fail_guard([] { stream_stats::update_stream_active(false); });
  stream_stats::add_client("10.0.0.5", "Deck", 395);
  ASSERT_TRUE(stream_stats::record_capture_backend(395, portal_capture("portal_kwin_node")));
  ASSERT_TRUE(stream_stats::record_capture_source(395,
    {1920, 1080, 1920, 1080, platf::frame_transport_e::dmabuf, platf::frame_residency_e::gpu, platf::frame_format_e::bgra8}));
  ASSERT_TRUE(stream_stats::record_pyrowave_route(395, "zero_copy"));
  const auto unknown = [] {
    stream_stats::stats_t none;
    none.streaming = true;
    none.clients.emplace_back();
    return stream_stats::pyrowave_route_reason(none);
  }();
  ASSERT_NE(stream_stats::pyrowave_route_reason(stream_stats::get_current(), 395), unknown);

  // Capture reinitializes onto the ScreenCast, and the stream ends before that display delivers.
  ASSERT_TRUE(stream_stats::record_capture_backend(395, portal_capture("portal_screencast", "kwin_node_failed")));
  auto json = current_stats_json();
  ASSERT_EQ(json["clients"].size(), 1u);
  EXPECT_FALSE(json["clients"][0].contains("pyrowave_route"))
    << "the display that has encoded nothing kept the last display's route: " << json["clients"][0].dump();
  EXPECT_EQ(stream_stats::pyrowave_route_reason(stream_stats::get_current(), 395), unknown);

  stream_stats::remove_client("10.0.0.5", 395);
  const auto last = current_stats_json()["last_session"];
  ASSERT_EQ(last.value("stream_instance_id", ""), stream_stats::stream_instance_id(395)) << last.dump();
  EXPECT_EQ(last["capture"].value("transport", ""), "unknown");
  EXPECT_FALSE(last.contains("pyrowave_route"))
    << "the ended session paired unknown frames with the route of the display before: " << last.dump();
}

TEST(StreamStatsPyroWaveRouteTests, TheReasonOpensWithWhereTheStreamConvertsColour) {
  stream_stats::stats_t stats;
  stats.streaming = true;
  stats.codec = "pyrowave";
  stats.clients.emplace_back();
  const auto reason_for = [&stats](std::string route) {
    stats.clients.front().pyrowave_route = std::move(route);
    return stream_stats::pyrowave_route_reason(stats);
  };
  const auto opens_with = [](const std::string &reason, std::string_view start) {
    return reason.rfind(start, 0) == 0;
  };

  // Nova's Doctor card shows two lines, so the answer comes first. A route is what the encoder saw
  // at its input: an imported DMA-BUF says nothing about how capture filled it, so the zero_copy
  // sentence claims the encoder input and no more.
  const auto zero_copy = reason_for("zero_copy");
  EXPECT_EQ(zero_copy, "PyroWave imports captured DMA-BUF frames and converts colour on the GPU, without a "
                       "CPU upload at the encoder input.");
  const auto gpu_upload = reason_for("gpu_upload");
  EXPECT_EQ(gpu_upload, "PyroWave converts colour on the GPU after copying captured frames there from host memory.");
  const auto cpu_convert = reason_for("cpu_convert");
  EXPECT_TRUE(opens_with(cpu_convert, "PyroWave converts colour on the CPU and copies the planes to the GPU, "
                                      "which costs host CPU time on captured frames.")) << cpu_convert;
  EXPECT_NE(cpu_convert.find("POLARIS_PYROWAVE_GPU_INPUT=off"), std::string::npos) << cpu_convert;
  const auto unknown = reason_for("");
  EXPECT_TRUE(opens_with(unknown, "PyroWave has not encoded a captured frame yet")) << unknown;

  for (const auto &reason : {zero_copy, gpu_upload, cpu_convert, unknown}) {
    // A repeated frame is encoded again without another upload or conversion.
    EXPECT_EQ(reason.find("each frame"), std::string::npos) << reason;
    EXPECT_EQ(reason.find("every frame"), std::string::npos) << reason;
  }
  for (const auto &reason : {zero_copy, gpu_upload, unknown}) {
    EXPECT_EQ(reason.find("on the CPU"), std::string::npos) << "a stream that converts on the GPU reads: " << reason;
    EXPECT_EQ(reason.find("CPU time"), std::string::npos) << "a stream that converts on the GPU reads: " << reason;
  }
  // Nothing past the encoder input is claimed for an imported frame.
  EXPECT_EQ(zero_copy.find("host memory"), std::string::npos) << zero_copy;
  EXPECT_EQ(zero_copy.find("where capture left it"), std::string::npos) << zero_copy;

  // Two streams have two routes, and one's is no answer for the other. Each stream that asks is
  // answered from its own entry.
  stats.clients.front().session_generation = 391;
  stats.clients.front().pyrowave_route = "zero_copy";
  stats.clients.emplace_back();
  stats.clients.back().session_generation = 392;
  stats.clients.back().pyrowave_route = "cpu_convert";
  EXPECT_TRUE(opens_with(stream_stats::pyrowave_route_reason(stats, 392), "PyroWave converts colour on the CPU"));
  EXPECT_TRUE(opens_with(stream_stats::pyrowave_route_reason(stats, 391), "PyroWave imports captured DMA-BUF frames"));

  // Asked from the host, or by a client with no stream among them, the reason says the routes differ
  // and never sends its reader to a field Nova does not read.
  for (const std::uint64_t asker : {std::uint64_t {0}, std::uint64_t {999}}) {
    const auto differ = stream_stats::pyrowave_route_reason(stats, asker);
    EXPECT_TRUE(opens_with(differ, "PyroWave streams on this host convert colour in different places")) << differ;
    EXPECT_EQ(differ.find("pyrowave_route"), std::string::npos) << differ;
  }

  // Where they agree there is one answer, including beside a stream that has reported none yet.
  stats.clients.back().pyrowave_route = "zero_copy";
  stats.clients.emplace_back();
  stats.clients.back().session_generation = 393;
  const auto agreed = stream_stats::pyrowave_route_reason(stats);
  EXPECT_TRUE(opens_with(agreed, "PyroWave imports captured DMA-BUF frames")) << agreed;
  EXPECT_TRUE(opens_with(stream_stats::pyrowave_route_reason(stats, 393), "PyroWave has not encoded a captured frame yet"))
    << "a stream that has not reported was answered with another stream's route";
}

namespace {
  const std::regex k_utc_second {"^[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z$"};

  nlohmann::json last_session_json() {
    const auto json = current_stats_json();
    return json.contains("last_session") ? json["last_session"] : nlohmann::json {};
  }

  stream_stats::capture_source_t shm_frames() {
    return {1920, 1080, 1920, 1080, platf::frame_transport_e::shm, platf::frame_residency_e::cpu, platf::frame_format_e::bgra8};
  }
}  // namespace

TEST(StreamStatsLastSessionTests, AnEndedSessionKeepsItsCaptureOutcomeUnderItsOwnIdentity) {
  stream_stats::update_stream_active(false);
  auto reset = util::fail_guard([] { stream_stats::update_stream_active(false); });
  stream_stats::add_client("10.0.0.5", "Deck", 401);
  ASSERT_TRUE(stream_stats::record_capture_backend(401,
    {"kms", "portal", "portal", "portal_screencast", "gamescope_session", "gamescope_node_missing"}));
  ASSERT_TRUE(stream_stats::record_capture_source(401, shm_frames()));
  stream_stats::update_video_stats(60.0, 20000, 4.0, "hevc", 1920, 1080, "vaapi", 401);
  const auto live = current_stats_json();
  ASSERT_EQ(live["clients"].size(), 1u);
  const auto live_capture = live["clients"][0]["capture"];

  const auto before = stream_stats::get_current();
  const auto controller_before = adaptive_bitrate::get_doctor_state();
  stream_stats::remove_client("10.0.0.5", 401);
  const auto after = stream_stats::get_current();
  const auto controller_after = adaptive_bitrate::get_doctor_state();
  // Freezing is a readout write, and moves no network or video policy revision.
  EXPECT_EQ(after.video_sample_revision, before.video_sample_revision);
  EXPECT_EQ(after.network_sample_revision, before.network_sample_revision);
  EXPECT_EQ(after.video_policy_sample_count, before.video_policy_sample_count);
  EXPECT_EQ(controller_after.revision, controller_before.revision);
  EXPECT_EQ(controller_after.action_authority_revision, controller_before.action_authority_revision);

  const auto json = nlohmann::json::parse(after.to_json());
  EXPECT_TRUE(json["clients"].empty());
  EXPECT_FALSE(has_capture_mirror(json)) << "the mirrors describe a client that is still streaming";
  ASSERT_TRUE(json.contains("last_session")) << json.dump();
  const auto &last = json["last_session"];
  EXPECT_EQ(last["state"], "ended");
  EXPECT_EQ(last["stream_instance_id"], stream_stats::stream_instance_id(401));
  EXPECT_EQ(last["client_name"], "Deck");
  EXPECT_EQ(last["capture"], live_capture) << "the capture the session streamed with, as it streamed";
  EXPECT_EQ(last["capture"]["mode_override_reason"], "gamescope_session");
  EXPECT_EQ(last["capture"]["route_fallback_reason"], "gamescope_node_missing");
  EXPECT_EQ(last["capture"]["transport"], "shm");
  EXPECT_EQ(last["codec"], "hevc");
  EXPECT_EQ(last["encoder_backend"], "vaapi");
  const auto started = last.value("started_at", "");
  const auto ended = last.value("ended_at", "");
  EXPECT_TRUE(std::regex_match(started, k_utc_second)) << started;
  EXPECT_TRUE(std::regex_match(ended, k_utc_second)) << ended;
  EXPECT_LE(started, ended);
  // A capture outcome, not a telemetry snapshot: loss and latency have no session-owned freshness
  // to carry, and encode targets are the host's, not the session's.
  std::set<std::string> keys;
  for (const auto &item : last.items()) {
    keys.insert(item.key());
  }
  EXPECT_EQ(keys, (std::set<std::string> {
    "state", "stream_instance_id", "client_name", "started_at", "ended_at", "capture", "codec", "encoder_backend",
  }));
}

TEST(StreamStatsLastSessionTests, HowTheAppWasStoppedIsKeptOnTheSessionThatEnded) {
  stream_stats::update_stream_active(false);
  auto reset = util::fail_guard([] { stream_stats::update_stream_active(false); });
  stream_stats::add_client("10.0.0.5", "RP6", 431);
  stream_stats::update_video_stats(60.0, 20000, 4.0, "hevc", 1920, 1080, "vaapi", 431);
  stream_stats::remove_client("10.0.0.5", 431);
  EXPECT_FALSE(last_session_json().contains("app_stop"));

  // The teardown stops the app after its streams end, so it writes to the session that ended.
  stream_stats::record_app_stop("close_request", 1, std::chrono::milliseconds {4200});
  const auto last = last_session_json();
  ASSERT_TRUE(last.contains("app_stop")) << last.dump();
  EXPECT_EQ(last["app_stop"]["path"], "close_request");
  EXPECT_EQ(last["app_stop"]["windows_asked"], 1);
  EXPECT_EQ(last["app_stop"]["waited_ms"], 4200);
  EXPECT_EQ(last["stream_instance_id"], stream_stats::stream_instance_id(431));

  // An empty path says nothing and changes nothing.
  stream_stats::record_app_stop("", 0, {});
  EXPECT_EQ(last_session_json(), last);
}

TEST(StreamStatsLastSessionTests, LastSessionAppStopCarriesLauncherFlatpakAndCapture) {
  stream_stats::update_stream_active(false);
  auto reset = util::fail_guard([] { stream_stats::update_stream_active(false); });
  stream_stats::add_client("10.0.0.6", "Android TV", 432);
  stream_stats::update_video_stats(60.0, 20000, 4.0, "hevc", 1920, 1080, "vaapi", 432);
  stream_stats::remove_client("10.0.0.6", 432);

  // A Heroic game quit from a private stream: the game closed when asked, then Heroic quit.
  stream_stats::app_stop_t stop;
  stop.path = "close_request";
  stop.windows_asked = 1;
  stop.waited = std::chrono::milliseconds {4210};
  stop.target = "flatpak";
  stop.launcher = stream_stats::app_stop_t::launcher_t {"com.heroicgameslauncher.hgl", "sigterm", std::chrono::milliseconds {850}};
  stop.flatpak_instances = stream_stats::app_stop_t::flatpak_instances_t {1, 1, 1, 2};
  stream_stats::record_app_stop(stop);
  auto last = last_session_json();
  ASSERT_TRUE(last.contains("app_stop")) << last.dump();
  EXPECT_EQ(last["app_stop"]["path"], "close_request");
  EXPECT_EQ(last["app_stop"]["target"], "flatpak");
  EXPECT_EQ(last["app_stop"]["launcher"]["app_id"], "com.heroicgameslauncher.hgl");
  EXPECT_EQ(last["app_stop"]["launcher"]["path"], "sigterm");
  EXPECT_EQ(last["app_stop"]["launcher"]["waited_ms"], 850);
  EXPECT_EQ(last["app_stop"]["flatpak_instances"]["launcher"], 1);
  EXPECT_EQ(last["app_stop"]["flatpak_instances"]["game"], 1);
  EXPECT_EQ(last["app_stop"]["flatpak_instances"]["helper"], 1);
  EXPECT_EQ(last["app_stop"]["flatpak_instances"]["left_alone"], 2);
  EXPECT_FALSE(last["app_stop"].contains("capture")) << "the check after the compositor has not reported yet";

  // The check after the compositor stopped found every process of the session.
  stream_stats::record_app_stop_check(true, 0, false);
  last = last_session_json();
  EXPECT_EQ(last["app_stop"]["capture"], "complete");
  EXPECT_EQ(last["app_stop"]["unattributed"], 0);
  EXPECT_EQ(last["app_stop"]["path"], "close_request");

  // What the 2026-09-27 teardown would have said: the compositor went down with the app live, and
  // two processes could not be attributed.
  stream_stats::record_app_stop_check(false, 2, true);
  last = last_session_json();
  EXPECT_EQ(last["app_stop"]["path"], "compositor_stop");
  EXPECT_EQ(last["app_stop"]["capture"], "incomplete");
  EXPECT_EQ(last["app_stop"]["unattributed"], 2);

  // A Steam game's stop names its lane and nothing it did not use.
  stream_stats::app_stop_t steam;
  steam.path = "sigterm";
  steam.target = "steam";
  stream_stats::record_app_stop(steam);
  last = last_session_json();
  EXPECT_EQ(last["app_stop"]["target"], "steam");
  EXPECT_FALSE(last["app_stop"].contains("launcher"));
  EXPECT_FALSE(last["app_stop"].contains("flatpak_instances"));
  EXPECT_FALSE(last["app_stop"].contains("capture"));
}

TEST(StreamStatsLastSessionTests, NothingWrittenAfterTheSessionEndsChangesIt) {
  stream_stats::update_stream_active(false);
  auto reset = util::fail_guard([] { stream_stats::update_stream_active(false); });
  stream_stats::add_client("10.0.0.5", "Deck", 411);
  ASSERT_TRUE(stream_stats::record_capture_backend(411, portal_capture("portal_kwin_node")));
  ASSERT_TRUE(stream_stats::record_capture_source(411, shm_frames()));
  stream_stats::update_video_stats(60.0, 20000, 4.0, "hevc", 1920, 1080, "vaapi", 411);
  stream_stats::remove_client("10.0.0.5", 411);
  const auto frozen = last_session_json();
  ASSERT_EQ(frozen.value("stream_instance_id", ""), stream_stats::stream_instance_id(411)) << frozen.dump();

  // Late writes for the retired generation find no entry, and a legacy periodic write with no
  // generation finds no client to take it.
  auto dmabuf = shm_frames();
  dmabuf.transport = platf::frame_transport_e::dmabuf;
  dmabuf.residency = platf::frame_residency_e::gpu;
  EXPECT_FALSE(stream_stats::record_capture_backend(411, portal_capture("portal_screencast", "kwin_node_failed")));
  EXPECT_FALSE(stream_stats::record_capture_source(411, dmabuf));
  stream_stats::update_video_stats(30.0, 5000, 9.0, "h264", 1280, 720, "software", 411);
  stream_stats::update_video_stats(30.0, 5000, 9.0, "av1", 1280, 720, "nvenc");
  EXPECT_EQ(last_session_json(), frozen);

  // The reset when the last stream ends comes just before a person looks for it.
  stream_stats::update_stream_active(false);
  EXPECT_EQ(last_session_json(), frozen);

  // A new session from the same address streams with its own facts and changes nothing until it
  // ends itself.
  stream_stats::add_client("10.0.0.5", "Deck", 412);
  ASSERT_TRUE(stream_stats::record_capture_backend(412, portal_capture("portal_screencast", "kwin_node_failed")));
  ASSERT_TRUE(stream_stats::record_capture_source(412, dmabuf));
  stream_stats::update_video_stats("10.0.0.5", 0.0, 5000, 0.0, "h264", 1280, 720, {}, 412);
  stream_stats::update_video_stats(30.0, 5000, 9.0, "h264", 1280, 720, "software", 412);
  EXPECT_EQ(last_session_json(), frozen);
}

TEST(StreamStatsLastSessionTests, AViewerThatEndsWhileTheOwnerStreamsIsTheLastSession) {
  stream_stats::update_stream_active(false);
  auto reset = util::fail_guard([] { stream_stats::update_stream_active(false); });
  stream_stats::add_client("10.0.0.5", "Owner", 421);
  stream_stats::add_client("10.0.0.6", "Viewer", 422);
  ASSERT_TRUE(stream_stats::record_capture_backend(421, portal_capture("portal_kwin_node")));
  ASSERT_TRUE(stream_stats::record_capture_backend(422, portal_capture("portal_kwin_node")));
  // Each session's encode loop reports its own encoder. The owner is listed first and writes last.
  stream_stats::update_video_stats(60.0, 20000, 4.0, "hevc", 1920, 1080, "vaapi", 421);
  stream_stats::update_video_stats(30.0, 8000, 6.0, "h264", 1280, 720, "software", 422);
  stream_stats::update_video_stats(60.0, 20000, 4.0, "hevc", 1920, 1080, "vaapi", 421);
  auto json = current_stats_json();
  ASSERT_EQ(json["clients"].size(), 2u);
  EXPECT_EQ(json["clients"][0]["codec"], "hevc");
  EXPECT_EQ(json["clients"][1]["codec"], "h264") << "the viewer's own encoder, not the owner's";
  EXPECT_EQ(json["clients"][1]["fps"], 30.0);

  stream_stats::remove_client("10.0.0.6", 422);
  json = current_stats_json();
  ASSERT_EQ(json["clients"].size(), 1u);
  EXPECT_EQ(json["clients"][0]["name"], "Owner");
  EXPECT_TRUE(json["streaming"].get<bool>());
  EXPECT_EQ(json["capture_backend_route"], "portal_kwin_node") << "the owner is the one live client now";
  const auto &viewer = json["last_session"];
  EXPECT_EQ(viewer["stream_instance_id"], stream_stats::stream_instance_id(422));
  EXPECT_EQ(viewer["client_name"], "Viewer");
  EXPECT_EQ(viewer["capture"]["route"], "portal_kwin_node");
  EXPECT_EQ(viewer["codec"], "h264");
  EXPECT_EQ(viewer["encoder_backend"], "software");

  // The owner ends next and replaces it.
  stream_stats::remove_client("10.0.0.5", 421);
  const auto owner = last_session_json();
  EXPECT_EQ(owner["stream_instance_id"], stream_stats::stream_instance_id(421));
  EXPECT_EQ(owner["client_name"], "Owner");
  EXPECT_EQ(owner["codec"], "hevc");
  EXPECT_EQ(owner["encoder_backend"], "vaapi");
}

TEST(StreamStatsLastSessionTests, OverlappingReconnectsFromOneAddressEachEndWithTheirOwnCodec) {
  stream_stats::update_stream_active(false);
  auto reset = util::fail_guard([] { stream_stats::update_stream_active(false); });
  // The session start writes by address. The replacement shares it with the session still leaving.
  stream_stats::add_client("10.0.0.5", "Leaving", 431);
  stream_stats::update_video_stats("10.0.0.5", 0.0, 20000, 0.0, "hevc", 1920, 1080, {}, 431);
  stream_stats::add_client("10.0.0.5", "Replacement", 432);
  stream_stats::update_video_stats("10.0.0.5", 0.0, 8000, 0.0, "av1", 1920, 1080, {}, 432);
  // The replacement ends first, while the older session is still listed ahead of it at the same
  // address, so a freeze that found the ending session by its address would take the older one.
  stream_stats::remove_client("10.0.0.5", 432);
  auto last = last_session_json();
  EXPECT_EQ(last.value("stream_instance_id", ""), stream_stats::stream_instance_id(432));
  EXPECT_EQ(last.value("client_name", ""), "Replacement");
  EXPECT_EQ(last.value("codec", ""), "av1") << "the replacement's codec landed on the older session";
  stream_stats::remove_client("10.0.0.5", 431);
  last = last_session_json();
  EXPECT_EQ(last.value("stream_instance_id", ""), stream_stats::stream_instance_id(431));
  EXPECT_EQ(last.value("client_name", ""), "Leaving");
  EXPECT_EQ(last.value("codec", ""), "hevc");
}

TEST(StreamStatsLastSessionTests, AStreamThatNamesNoSessionNeverWritesASessionsEntry) {
  stream_stats::update_stream_active(false);
  auto reset = util::fail_guard([] { stream_stats::update_stream_active(false); });
  // Browser Stream's encode loop names no session, and nothing keeps it from running beside a
  // Moonlight or Nova session. That session is listed first, and an entry registered with no
  // generation after it.
  stream_stats::add_client("10.0.0.5", "Deck", 461);
  stream_stats::add_client("10.0.0.9", "Legacy");
  stream_stats::update_video_stats("10.0.0.5", 0.0, 20000, 0.0, "hevc", 1920, 1080, {}, 461);
  stream_stats::update_video_stats(60.0, 20000, 4.0, "hevc", 1920, 1080, "vaapi", 461);
  stream_stats::update_video_stats(30.0, 5000, 9.0, "h264", 1280, 720, "software");
  stream_stats::update_video_stats("10.0.0.5", 30.0, 5000, 9.0, "h264", 1280, 720, "software");
  const auto json = current_stats_json();
  ASSERT_EQ(json["clients"].size(), 2u);
  const auto &session = json["clients"][0];
  EXPECT_EQ(session["codec"], "hevc") << "the stream with no session wrote over the session's entry";
  EXPECT_EQ(session["fps"], 60.0);
  EXPECT_EQ(session["bitrate_kbps"], 20000);
  EXPECT_EQ(session["width"], 1920);
  EXPECT_EQ(json["clients"][1]["codec"], "h264") << "an entry with no generation still takes it";
  // The process-wide values take every writer's, as they always have.
  const auto host = stream_stats::get_current();
  EXPECT_EQ(host.codec, "h264");
  EXPECT_EQ(host.encoder_backend, "software");
  ASSERT_EQ(host.clients.size(), 2u);
  EXPECT_EQ(host.clients.front().encoder_backend, "vaapi");

  stream_stats::remove_client("10.0.0.5", 461);
  const auto last = last_session_json();
  EXPECT_EQ(last.value("stream_instance_id", ""), stream_stats::stream_instance_id(461));
  EXPECT_EQ(last.value("codec", ""), "hevc");
  EXPECT_EQ(last.value("encoder_backend", ""), "vaapi");
}

/**
 * Each live client serves the encoder its own stream sampled, as last_session keeps it when it ends,
 * and only a sole client's repeats at the top level.
 */
TEST(StreamStatsLastSessionTests, EachLiveClientServesTheEncoderItsEndedSessionKeeps) {
  stream_stats::update_stream_active(false);
  auto reset = util::fail_guard([] { stream_stats::update_stream_active(false); });
  // Both start writes carry the codec and no encoder yet, the way rtsp_stream::start() makes them.
  const auto start = [](const std::string &ip, const std::string &name, std::uint64_t generation,
                        const std::string &codec) {
    stream_stats::add_client(ip, name, generation);
    stream_stats::update_video_stats(ip, 0.0, 20000, 0.0, codec, 1920, 1080, {}, generation);
    stream_stats::update_video_stats(0.0, 20000, 0.0, codec, 1920, 1080, {}, generation);
  };
  start("10.0.0.5", "Owner", 481, "hevc");
  start("10.0.0.6", "Viewer", 482, "h264");
  auto json = current_stats_json();
  ASSERT_EQ(json["clients"].size(), 2u);
  for (const auto &client : json["clients"]) {
    EXPECT_FALSE(client.contains("encoder_backend")) << "an encoder before the first sample: " << client.dump();
  }

  stream_stats::update_video_stats(60.0, 20000, 4.0, "hevc", 1920, 1080, "vaapi", 481);
  stream_stats::update_video_stats(30.0, 8000, 6.0, "h264", 1280, 720, "software", 482);
  // Browser Stream's encode loop names no session, and writes only the process-wide encoder.
  stream_stats::update_video_stats(30.0, 5000, 9.0, "h264", 1280, 720, "nvenc");
  json = current_stats_json();
  ASSERT_EQ(json["clients"].size(), 2u);
  EXPECT_EQ(json["clients"][0].value("encoder_backend", ""), "vaapi");
  EXPECT_EQ(json["clients"][1].value("encoder_backend", ""), "software") << "the viewer's own encoder";
  EXPECT_FALSE(json.contains("encoder_backend")) << "two clients have no one encoder";
  const auto viewer_live = json["clients"][1].value("encoder_backend", "");

  stream_stats::remove_client("10.0.0.6", 482);
  json = current_stats_json();
  EXPECT_EQ(json["last_session"].value("encoder_backend", ""), viewer_live)
    << "the ended session reports an encoder its live entry never showed";
  ASSERT_EQ(json["clients"].size(), 1u);
  ASSERT_EQ(stream_stats::get_current().encoder_backend, "nvenc");
  EXPECT_EQ(json.value("encoder_backend", ""), "vaapi")
    << "the top level is the sole client's own encoder, not the last sample on the host";
  const auto owner_live = json["clients"][0].value("encoder_backend", "");

  stream_stats::remove_client("10.0.0.5", 481);
  json = current_stats_json();
  EXPECT_EQ(json["last_session"].value("encoder_backend", ""), owner_live);
  EXPECT_FALSE(json.contains("encoder_backend")) << "no client streams";
}

TEST(StreamStatsLastSessionTests, ARepeatedOrUnmatchedRemovalLeavesItAsItWas) {
  stream_stats::update_stream_active(false);
  auto reset = util::fail_guard([] { stream_stats::update_stream_active(false); });
  // A replacement from the same address keeps streaming through every removal below, so a
  // removal that matched by address would find it.
  stream_stats::add_client("10.0.0.5", "Deck", 441);
  stream_stats::add_client("10.0.0.5", "Replacement", 442);
  ASSERT_TRUE(stream_stats::record_capture_backend(441, portal_capture()));
  ASSERT_TRUE(stream_stats::record_capture_backend(442, portal_capture("portal_kwin_node")));
  stream_stats::remove_client("10.0.0.5", 441);
  const auto frozen = last_session_json();
  ASSERT_EQ(frozen.value("stream_instance_id", ""), stream_stats::stream_instance_id(441)) << frozen.dump();

  // The same teardown again, a generation that was never added, and a legacy entry with no
  // generation, which owns no facts and has no identity to report.
  stream_stats::remove_client("10.0.0.5", 441);
  EXPECT_EQ(last_session_json(), frozen);
  stream_stats::remove_client("10.0.0.5", 449);
  EXPECT_EQ(last_session_json(), frozen);
  stream_stats::add_client("10.0.0.9", "Legacy");
  stream_stats::remove_client("10.0.0.9");
  EXPECT_EQ(last_session_json(), frozen);
  const auto live = current_stats_json();
  ASSERT_EQ(live["clients"].size(), 1u) << "none of them removes the replacement";
  EXPECT_EQ(live["clients"][0]["stream_instance_id"], stream_stats::stream_instance_id(442));
}

TEST(StreamStatsLastSessionTests, ADisplayOpenedAgainEndsWithUnknownFramesUntilItDeliversOne) {
  stream_stats::update_stream_active(false);
  auto reset = util::fail_guard([] { stream_stats::update_stream_active(false); });
  stream_stats::add_client("10.0.0.5", "Deck", 471);
  ASSERT_TRUE(stream_stats::record_capture_backend(471, portal_capture("portal_kwin_node")));
  ASSERT_TRUE(stream_stats::record_capture_source(471, shm_frames()));
  // The display is opened again on the fallback route, and the stream ends before that display
  // delivers a frame. The frames the first display delivered say nothing about this one.
  ASSERT_TRUE(stream_stats::record_capture_backend(471, portal_capture("portal_screencast", "kwin_node_failed")));
  stream_stats::remove_client("10.0.0.5", 471);
  const auto capture = last_session_json()["capture"];
  EXPECT_EQ(capture.value("route", ""), "portal_screencast");
  EXPECT_EQ(capture.value("route_fallback_reason", ""), "kwin_node_failed");
  EXPECT_EQ(capture.value("transport", ""), "unknown") << "the earlier display's frames: " << capture.dump();
  EXPECT_EQ(capture.value("residency", ""), "unknown");
  EXPECT_EQ(capture.value("format", ""), "unknown");
}

TEST(StreamStatsLastSessionTests, ASessionWhoseDisplayNeverOpenedEndsWithNoCapture) {
  stream_stats::update_stream_active(false);
  auto reset = util::fail_guard([] { stream_stats::update_stream_active(false); });
  // Stats that never saw a session end say nothing about one.
  EXPECT_FALSE(nlohmann::json::parse(stream_stats::stats_t {}.to_json()).contains("last_session"));

  stream_stats::add_client("10.0.0.5", "Deck", 451);
  stream_stats::remove_client("10.0.0.5", 451);
  const auto last = last_session_json();
  EXPECT_EQ(last.value("state", ""), "ended");
  EXPECT_EQ(last.value("stream_instance_id", ""), stream_stats::stream_instance_id(451));
  // Missing is unknown, never a default.
  EXPECT_FALSE(last.contains("capture")) << last.dump();
  EXPECT_FALSE(last.contains("codec"));
  EXPECT_FALSE(last.contains("encoder_backend"));
}

TEST(StreamStatsCaptureSourceTests, DoesNotInferExtraCpuCopyFromSizeOrEncoderUploadAlone) {
  for (const auto source : {
    stream_stats::capture_source_t {3840, 2160, 1920, 1080, platf::frame_transport_e::dmabuf, platf::frame_residency_e::gpu},
    stream_stats::capture_source_t {1920, 1080, 1920, 1080, platf::frame_transport_e::shm, platf::frame_residency_e::cpu},
    stream_stats::capture_source_t {1280, 720, 1920, 1080, platf::frame_transport_e::shm, platf::frame_residency_e::cpu},
    stream_stats::capture_source_t {2560, 1440, 1920, 1080, platf::frame_transport_e::shm, platf::frame_residency_e::cpu},
  }) {
    stream_stats::stats_t stats {};
    stats.streaming = true;
    stats.encode_target_residency = platf::frame_residency_e::cpu;
    stats.clients.emplace_back();
    stats.clients[0].capture_source = source;
    const auto doctor = stream_stats::build_doctor_json(stats, nlohmann::json::object());
    const auto &evidence = doctor.at("evidence");
    const auto row = std::find_if(evidence.begin(), evidence.end(), [](const auto &item) {
      return item.value("id", "") == "capture_source_size";
    });
    ASSERT_NE(row, evidence.end());
    EXPECT_EQ(row->at("detail").get<std::string>().find("CPU capture path"), std::string::npos);
  }
}

TEST(StreamStatsCaptureSourceTests, OmitsUnobservedIdleAndMultiClientComparisons) {
  stream_stats::stats_t stats {};
  stats.streaming = true;
  stats.clients.emplace_back();
  const auto absent = [&] {
    const auto doctor = stream_stats::build_doctor_json(stats, nlohmann::json::object());
    for (const auto &row : doctor.at("evidence")) EXPECT_NE(row.value("id", ""), "capture_source_size");
  };
  absent();
  stats.clients[0].capture_source = {3840, 2160, 1920, 1080,
    platf::frame_transport_e::shm, platf::frame_residency_e::cpu};
  stats.streaming = false;
  absent();
  stats.streaming = true;
  stats.clients.emplace_back();
  absent();
}

namespace {
  /// A host with nothing streaming whose last session ended the given time ago with this start.
  stream_stats::stats_t after_last_start(std::string_view outcome, std::string codec,
                                         std::chrono::system_clock::duration ago) {
    stream_stats::stats_t stats {};
    stats.streaming = false;
    stream_stats::ended_session_t last;
    last.session_generation = 901;
    last.client_name = "Living Room TV";
    last.started_at = std::chrono::system_clock::now() - ago - std::chrono::seconds {10};
    last.ended_at = std::chrono::system_clock::now() - ago;
    last.codec = std::move(codec);
    last.start_outcome = outcome;
    last.start_client_left_after_ms = outcome == "client_left_during_setup" ? 113 : -1;
    stats.last_session = last;
    return stats;
  }

  const nlohmann::json *find_evidence_row(const nlohmann::json &doctor, std::string_view id) {
    for (const auto &entry : doctor.at("evidence")) {
      if (entry.at("id") == id) {
        return &entry;
      }
    }
    return nullptr;
  }
}  // namespace

TEST(StreamStatsDoctorTests, NamesAPyroWaveStartTheClientLeftAsTheIssue) {
  // An Android TV client negotiated PyroWave, could not build the decoder, and left during video
  // setup. The report then said only "no_active_stream" and suggested exporting the report it was.
  const auto doctor = stream_stats::build_doctor_json(
    after_last_start("client_left_during_setup", "pyrowave", std::chrono::minutes {2}),
    {{"primary_issue", "steady"}, {"grade", "good"}}
  );

  EXPECT_EQ(doctor.at("primary_issue"), "stream_failed_to_start");
  EXPECT_EQ(doctor.at("status"), "needs_action");
  EXPECT_EQ(doctor.at("traffic_light"), "amber");
  const auto summary = doctor.at("summary").get<std::string>();
  EXPECT_EQ(summary,
            "The last stream, to Living Room TV, failed to start: the client left during video setup 113 ms after "
            "connecting, before any video arrived. It had negotiated PyroWave.");
  const auto next = std::string {
    "The client could not start its PyroWave decoder. Choose HEVC or H.264 for that device, or update the client."};
  EXPECT_EQ(doctor.at("recommendation").at("body"), next);
  EXPECT_EQ(doctor.at("recommendation").at("next_step_label"), "Choose HEVC or H.264");
  const auto &action = doctor.at("safe_recovery_action");
  EXPECT_EQ(action.at("id"), "none") << "the report must not suggest exporting itself: " << action.dump();
  EXPECT_EQ(action.at("kind"), "manual_guidance");
  EXPECT_EQ(action.at("unavailable_reason"), next);
  EXPECT_FALSE(action.at("requires_owner").get<bool>());

  const auto *row = find_evidence_row(doctor, "last_stream_start");
  ASSERT_NE(row, nullptr) << doctor.at("evidence").dump();
  EXPECT_EQ(row->at("value"), "client_left_during_setup");
  EXPECT_EQ(row->at("status"), "fail");
  EXPECT_EQ(row->at("detail"), summary);
  // It follows the "no active stream" row, so the first rows a support report keeps name it.
  ASSERT_GE(doctor.at("evidence").size(), 2u);
  EXPECT_EQ(doctor.at("evidence")[0].at("id"), "streaming");
  EXPECT_EQ(doctor.at("evidence")[1].at("id"), "last_stream_start");
}

TEST(StreamStatsDoctorTests, AnotherCodecsFailedStartPointsAtTheClientsOwnError) {
  for (const auto *codec : {"h264", "hevc", "av1"}) {
    const auto doctor = stream_stats::build_doctor_json(
      after_last_start("client_left_during_setup", codec, std::chrono::minutes {1}),
      {{"primary_issue", "steady"}, {"grade", "good"}}
    );
    EXPECT_EQ(doctor.at("primary_issue"), "stream_failed_to_start") << codec;
    EXPECT_EQ(doctor.at("recommendation").at("body"),
              "The client stopped during video setup; its own error message names the cause.")
      << codec;
    EXPECT_EQ(doctor.at("safe_recovery_action").at("unavailable_reason"),
              "The client stopped during video setup; its own error message names the cause.")
      << codec;
    EXPECT_EQ(doctor.at("summary").get<std::string>().find("PyroWave"), std::string::npos) << codec;
  }
}

TEST(StreamStatsDoctorTests, AFailedStartLeadsOnlyWhileNothingStreamsAndForFifteenMinutes) {
  const nlohmann::json steady = {{"primary_issue", "steady"}, {"grade", "good"}};

  // Older than the window: nothing streams, and that is all Doctor says; the row stays as a fact.
  auto doctor = stream_stats::build_doctor_json(
    after_last_start("client_left_during_setup", "pyrowave", std::chrono::minutes {16}), steady
  );
  EXPECT_EQ(doctor.at("primary_issue"), "no_active_stream");
  const auto *row = find_evidence_row(doctor, "last_stream_start");
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(row->at("status"), "info");

  // A stream running now is what Doctor reads, whatever the last one did.
  auto streaming = after_last_start("client_left_during_setup", "pyrowave", std::chrono::minutes {1});
  streaming.streaming = true;
  doctor = stream_stats::build_doctor_json(streaming, steady);
  EXPECT_NE(doctor.at("primary_issue"), "stream_failed_to_start");
  EXPECT_EQ(find_evidence_row(doctor, "last_stream_start"), nullptr);

  // A client that stayed and whose packets never arrived is not a failed setup: the row names the
  // path, and the issue stays no_active_stream.
  doctor = stream_stats::build_doctor_json(after_last_start("no_ping", "hevc", std::chrono::minutes {1}), steady);
  EXPECT_EQ(doctor.at("primary_issue"), "no_active_stream");
  row = find_evidence_row(doctor, "last_stream_start");
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(row->at("status"), "watch");
  // no_ping is also written when only the audio socket waited out a stream whose video had arrived,
  // so the row must not claim that nothing came.
  EXPECT_EQ(row->at("detail"),
            "The last stream, to Living Room TV, ended waiting for a first packet from the client on its video or "
            "audio port. A firewall or a UDP path problem between the client and this host usually does that.");

  // A last session that started says so, and changes nothing.
  doctor = stream_stats::build_doctor_json(after_last_start("started", "hevc", std::chrono::minutes {1}), steady);
  EXPECT_EQ(doctor.at("primary_issue"), "no_active_stream");
  row = find_evidence_row(doctor, "last_stream_start");
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(row->at("status"), "pass");
}

TEST(StreamStatsLastSessionTests, AnEndedSessionKeepsHowItsStartEnded) {
  stream_stats::update_stream_active(false);
  auto reset = util::fail_guard([] { stream_stats::update_stream_active(false); });
  stream_stats::add_client("10.0.0.6", "Living Room TV", 931);
  stream_stats::update_video_stats("10.0.0.6", 0, 20000, 0, "pyrowave", 1920, 1080, {}, 931);

  EXPECT_FALSE(stream_stats::record_start_outcome(932, "client_left_during_setup", 113)) << "a generation no client holds";
  EXPECT_FALSE(stream_stats::record_start_outcome(0, "client_left_during_setup", 113)) << "generation zero is no one's";
  EXPECT_FALSE(stream_stats::record_start_outcome(931, "", 113)) << "an empty outcome was written";
  ASSERT_TRUE(stream_stats::record_start_outcome(931, "client_left_during_setup", 113));
  auto json = current_stats_json();
  ASSERT_EQ(json["clients"].size(), 1u);
  EXPECT_EQ(json["clients"][0].value("start_outcome", ""), "client_left_during_setup");

  stream_stats::remove_client("10.0.0.6", 931);
  json = current_stats_json();
  ASSERT_TRUE(json.contains("last_session")) << json.dump();
  const auto &last = json["last_session"];
  EXPECT_EQ(last.value("client_name", ""), "Living Room TV");
  EXPECT_EQ(last.value("codec", ""), "pyrowave");
  ASSERT_TRUE(last.contains("start")) << last.dump();
  EXPECT_EQ(last["start"].value("outcome", ""), "client_left_during_setup");
  EXPECT_EQ(last["start"].value("client_left_after_ms", -1), 113);
  EXPECT_FALSE(stream_stats::record_start_outcome(931, "started")) << "a retired generation still writes";

  // And Doctor, reading the same stats a moment later, names it.
  const auto doctor = stream_stats::build_doctor_json(stream_stats::get_current(), {{"primary_issue", "steady"}});
  EXPECT_EQ(doctor.at("primary_issue"), "stream_failed_to_start");

  // A session that started keeps started, without a time the client left.
  stream_stats::add_client("10.0.0.6", "Living Room TV", 933);
  ASSERT_TRUE(stream_stats::record_start_outcome(933, "started"));
  stream_stats::remove_client("10.0.0.6", 933);
  const auto started = current_stats_json()["last_session"];
  EXPECT_EQ(started["start"].value("outcome", ""), "started");
  EXPECT_FALSE(started["start"].contains("client_left_after_ms")) << started.dump();
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

TEST(StreamStatsDoctorTests, KeepsOversizedFecFramesInformational) {
  stream_stats::stats_t stats {};
  stats.streaming = true;
  stats.capture_transport = platf::frame_transport_e::dmabuf;
  stats.capture_residency = platf::frame_residency_e::gpu;
  stats.encode_target_residency = platf::frame_residency_e::gpu;
  stats.encode_time_ms = 4.0;
  stats.fec_protection.oversized_frames_total = 288;
  stats.fec_protection.oversized_idr_frames_total = 4;
  stats.fec_protection.largest_encoded_frame_bytes = 2'900'000;
  stats.fec_protection.largest_packetized_frame_bytes = 2'940'000;
  stats.fec_protection.protected_payload_limit_bytes = 1'006'720;
  stats.fec_protection.max_required_blocks = 12;
  stats.fec_protection.fec_percentage = 5;
  stats.fec_protection.packet_size = 1024;
  stats.fec_protection.largest_frame_type = "idr";

  const auto doctor = stream_stats::build_doctor_json(
    stats, nlohmann::json::object()
  );

  EXPECT_EQ(doctor.at("traffic_light"), "green");
  EXPECT_EQ(doctor.at("status"), "ok");
  EXPECT_EQ(doctor.at("severity"), "info");
  EXPECT_EQ(doctor.at("primary_issue"), "none");
  EXPECT_EQ(doctor.at("safe_recovery_action").at("id"), "none");

  const auto &fec = *std::find_if(
    doctor.at("evidence").begin(), doctor.at("evidence").end(),
    [](const auto &item) { return item.value("id", "") == "fec_protection"; }
  );
  EXPECT_EQ(fec.at("status"), "info");
  EXPECT_EQ(fec.at("source"), "sender_packetizer");
  EXPECT_EQ(fec.at("value"), 288);
  EXPECT_NE(
    fec.at("detail").get<std::string>().find("not proof of media packet loss"),
    std::string::npos
  );
  EXPECT_EQ(
    doctor.at("advanced_evidence").at("fec_protection").at("max_required_blocks"),
    12
  );
}

TEST(StreamStatsFecProtectionTests, RoutesEvidenceBySessionGeneration) {
  stream_stats::update_stream_active(false);
  stream_stats::add_client("198.51.100.40", "Primary", 7401);
  stream_stats::add_client("198.51.100.41", "Viewer", 7402);

  stream_stats::record_oversized_fec_frame(
    7402, 1'250'000, 1'280'000, 1'006'720, 6, 5, 1024, "delta"
  );
  auto stats = stream_stats::get_current();
  ASSERT_EQ(stats.clients.size(), 2);
  EXPECT_EQ(stats.fec_protection.oversized_frames_total, 0);
  EXPECT_EQ(stats.clients.at(1).fec_protection.oversized_frames_total, 1);

  stream_stats::record_oversized_fec_frame(
    7401, 2'900'000, 2'940'000, 1'006'720, 12, 5, 1024, "idr"
  );
  stats = stream_stats::get_current();
  EXPECT_EQ(stats.fec_protection.oversized_frames_total, 1);
  EXPECT_EQ(stats.fec_protection.oversized_idr_frames_total, 1);
  EXPECT_EQ(stats.fec_protection.max_required_blocks, 12);

  stream_stats::remove_client("198.51.100.40", 7401);
  stats = stream_stats::get_current();
  ASSERT_EQ(stats.clients.size(), 1);
  EXPECT_EQ(stats.client_name, "Viewer");
  EXPECT_EQ(stats.fec_protection.oversized_frames_total, 1);
  EXPECT_EQ(stats.fec_protection.max_required_blocks, 6);

  stream_stats::remove_client("198.51.100.41", 7402);
  stream_stats::update_stream_active(false);
  EXPECT_EQ(
    stream_stats::get_current().fec_protection.oversized_frames_total,
    0
  );
}

TEST(StreamStatsDoctorTests, ReportsAHostWithNoCaptureBackendAsFailed) {
  // Polaris logs this fatally at startup and then serves anyway, so the host pairs, accepts
  // launches and advertises H.264 as the only codec it has while Doctor reads clean. See #677.
  platf::set_capture_sources_missing_for_tests(true);

  stream_stats::stats_t stats {};
  const auto doctor = stream_stats::build_doctor_json(stats, {{"primary_issue", "steady"}, {"grade", "good"}});

  bool saw_warning = false;
  for (const auto &warning :
       doctor.at("advanced_evidence").at("linux_gpu_profile").at("configuration_warnings")) {
    if (warning.at("id") != "no_capture_backend") {
      continue;
    }
    saw_warning = true;
    EXPECT_EQ(warning.at("severity"), "fail");
    EXPECT_NE(warning.at("message").get<std::string>().find("H.264"), std::string::npos);
  }
  EXPECT_TRUE(saw_warning);

  platf::set_capture_sources_missing_for_tests(false);
}

#ifdef __linux__
// #782: a settings file the store refused was explained only on the console's Settings page.
// The Doctor carries it as settings_file_unreadable for as long as the last read or save met the
// refusal. Paired clients and support bundles read this list, so it names the kind of refusal and
// never the file's path.
TEST(StreamStatsDoctorTests, ReportsASettingsFileTheStoreRefused) {
  const auto old_path = config::sunshine.config_file;
  const auto directory = std::filesystem::temp_directory_path() /
    ("polaris-doctor-settings-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directory(directory);
  auto restore = util::fail_guard([&] {
    config::sunshine.config_file = old_path;
    std::filesystem::remove_all(directory);
  });
  const auto path = (directory / "polaris.conf").string();
  config::sunshine.config_file = path;
  ASSERT_TRUE(private_state_file::write_atomic(path, "sunshine_name = doctor\n"));
  const auto finding = [] {
    const auto doctor = stream_stats::build_doctor_json({}, {{"primary_issue", "steady"}, {"grade", "good"}});
    for (const auto &warning :
         doctor.at("advanced_evidence").at("linux_gpu_profile").at("configuration_warnings")) {
      if (warning.at("id") == "settings_file_unreadable") return warning;
    }
    return nlohmann::json {};
  };

  ASSERT_TRUE(configuration_store::read(path));
  EXPECT_TRUE(finding().is_null());

  ASSERT_EQ(::chmod(path.c_str(), 0664), 0);
  configuration_store::refusal_t refusal;
  ASSERT_FALSE(configuration_store::read(path, &refusal));
  ASSERT_EQ(refusal.kind, private_state_file::refusal_e::group_writable);
  const auto warning = finding();
  ASSERT_FALSE(warning.is_null());
  EXPECT_EQ(warning.at("severity"), "warning");
  EXPECT_EQ(warning.at("refusal"), "group_writable");
  EXPECT_EQ(warning.dump().find(directory.string()), std::string::npos) << warning.dump();
  EXPECT_NE(warning.at("action").get<std::string>().find("banner"), std::string::npos);
  // Only settings go through the store. Apps, pairing and the password have files of their own and
  // still save, so the finding must not say that nothing can be saved.
  const auto message = warning.at("message").get<std::string>();
  EXPECT_NE(message.find("no settings change can be saved"), std::string::npos) << message;
  EXPECT_NE(message.find("still save"), std::string::npos) << message;
  EXPECT_EQ(message.find("no change can be saved"), std::string::npos) << message;
  // The top level stats payload carries the same list.
  bool in_stats = false;
  // Keep the payload alive for the loop: a range-for over .at() on the temporary would iterate a
  // reference into an object destroyed before the first iteration.
  const auto stats_payload = stream_stats::linux_gpu_profile_json({});
  for (const auto &entry : stats_payload.at("configuration_warnings")) {
    in_stats = in_stats || entry.at("id") == "settings_file_unreadable";
  }
  EXPECT_TRUE(in_stats);

  ASSERT_EQ(::chmod(path.c_str(), 0600), 0);
  ASSERT_TRUE(configuration_store::read(path));
  EXPECT_TRUE(finding().is_null());

  // A missing file is a refusal too, and the id says which.
  ASSERT_TRUE(std::filesystem::remove(path));
  ASSERT_FALSE(configuration_store::read(path));
  ASSERT_FALSE(finding().is_null());
  EXPECT_EQ(finding().at("refusal"), "missing");
  ASSERT_TRUE(private_state_file::write_atomic(path, "sunshine_name = doctor\n"));
  ASSERT_TRUE(configuration_store::read(path));
  EXPECT_TRUE(finding().is_null());

  // Every kind has its own name, so no two refusals read alike.
  std::set<std::string> names;
  for (auto kind = static_cast<int>(private_state_file::refusal_e::none);
       kind <= static_cast<int>(private_state_file::refusal_e::read_failed); ++kind) {
    const std::string name {private_state_file::refusal_name(static_cast<private_state_file::refusal_e>(kind))};
    EXPECT_FALSE(name.empty()) << kind;
    EXPECT_TRUE(names.insert(name).second) << name;
  }
}
#endif

TEST(StreamStatsDoctorTests, ReportsThreadPriorityThatCouldNotBeRaised) {
  // Logged once at the first stream and then gone from view. A support bundle carried it only
  // in raw log text, beside a microstutter report it may have explained.
  platf::set_thread_priority_unavailable_note_for_tests("RLIMIT_RTPRIO=0, RLIMIT_NICE=0");

  stream_stats::stats_t stats {};
  const auto doctor = stream_stats::build_doctor_json(stats, {{"primary_issue", "steady"}, {"grade", "good"}});

  bool saw_warning = false;
  for (const auto &warning :
       doctor.at("advanced_evidence").at("linux_gpu_profile").at("configuration_warnings")) {
    if (warning.at("id") != "thread_priority_unavailable") {
      continue;
    }
    saw_warning = true;
    EXPECT_EQ(warning.at("severity"), "warning");
    // The limits that applied, so a reader can tell RLIMIT_RTPRIO from RLIMIT_NICE.
    EXPECT_NE(warning.at("message").get<std::string>().find("RLIMIT_RTPRIO=0, RLIMIT_NICE=0"), std::string::npos);
    EXPECT_NE(warning.at("action").get<std::string>().find("polaris.service"), std::string::npos);
  }
  EXPECT_TRUE(saw_warning);

  platf::set_thread_priority_unavailable_note_for_tests("");
}

TEST(StreamStatsDoctorTests, SaysNothingAboutThreadPriorityUntilElevationIsRefused) {
  platf::set_thread_priority_unavailable_note_for_tests("");

  stream_stats::stats_t stats {};
  const auto doctor = stream_stats::build_doctor_json(stats, {{"primary_issue", "steady"}, {"grade", "good"}});

  for (const auto &warning :
       doctor.at("advanced_evidence").at("linux_gpu_profile").at("configuration_warnings")) {
    EXPECT_NE(warning.at("id"), "thread_priority_unavailable");
  }
}

#ifdef __linux__
namespace {
  std::vector<nlohmann::json> host_virtual_display_warnings() {
    stream_stats::stats_t stats {};
    const auto doctor = stream_stats::build_doctor_json(stats, {{"primary_issue", "steady"}, {"grade", "good"}});
    std::vector<nlohmann::json> found;
    for (const auto &warning :
         doctor.at("advanced_evidence").at("linux_gpu_profile").at("configuration_warnings")) {
      if (warning.at("id").get<std::string>().rfind("hvd_", 0) == 0) {
        found.push_back(warning);
      }
    }
    return found;
  }

  const nlohmann::json *warning_with_id(const std::vector<nlohmann::json> &warnings, std::string_view id) {
    for (const auto &warning : warnings) {
      if (warning.at("id") == id) {
        return &warning;
      }
    }
    return nullptr;
  }
}  // namespace

TEST(StreamStatsDoctorTests, NamesTheHostVirtualDisplayProblemsFoundOnPlasma) {
  // Each was silent on pc-papi: the game opened on the desk's monitor because the screen came
  // from EVDI, which KWin ran at a stored 1.35 scale, and a tap missed the stream screen.
  virtual_display::doctor_notes_t notes;
  notes.plasma = true;
  notes.last_backend = virtual_display::backend_e::EVDI;
  notes.preference = "auto";
  notes.kwin_reason = "kscreen-doctor is not installed; Polaris needs it to place the new screen.";
  notes.scaled_screen = "DVI-I-1";
  notes.scaled_screen_scale = 1.35;
  notes.input_routes.push_back({"Touch passthrough", "Virtual-polaris-0", false, "No such object path"});
  notes.input_routes.push_back({"Pen passthrough", "Virtual-polaris-0", true, ""});
  virtual_display::set_doctor_notes_for_tests(notes);

  const auto warnings = host_virtual_display_warnings();
  const auto *input = warning_with_id(warnings, "hvd_input_not_mapped");
  ASSERT_NE(input, nullptr);
  EXPECT_EQ(input->at("severity"), "warning");
  EXPECT_NE(input->at("message").get<std::string>().find("Touch passthrough"), std::string::npos);
  EXPECT_NE(input->at("message").get<std::string>().find("No such object path"), std::string::npos);

  const auto *scaled = warning_with_id(warnings, "hvd_screen_scaled");
  ASSERT_NE(scaled, nullptr);
  EXPECT_NE(scaled->at("message").get<std::string>().find("[DVI-I-1] at 135%"), std::string::npos);
  // Against what was asked for, so the advice sends someone to the number they chose rather than
  // to 100% on a screen they deliberately made readable at 200%.
  EXPECT_NE(scaled->at("message").get<std::string>().find("rather than the 100%"), std::string::npos);
  EXPECT_NE(scaled->at("action").get<std::string>().find("100%"), std::string::npos);

  notes.scaled_screen_expected = 2.0;
  virtual_display::set_doctor_notes_for_tests(notes);
  const auto asked_two = host_virtual_display_warnings();
  const auto *scaled_two = warning_with_id(asked_two, "hvd_screen_scaled");
  ASSERT_NE(scaled_two, nullptr);
  EXPECT_NE(scaled_two->at("message").get<std::string>().find("rather than the 200%"), std::string::npos);
  EXPECT_NE(scaled_two->at("action").get<std::string>().find("200%"), std::string::npos);
  notes.scaled_screen_expected = 1.0;
  virtual_display::set_doctor_notes_for_tests(notes);

  const auto *unused = warning_with_id(warnings, "hvd_kwin_screen_unused");
  ASSERT_NE(unused, nullptr);
  EXPECT_EQ(unused->at("severity"), "info");
  EXPECT_NE(unused->at("message").get<std::string>().find("used EVDI because Polaris could not create a KWin screen: kscreen-doctor is not installed"), std::string::npos);

  // A backend the user picked is named as the reason, with the way back.
  notes.preference = "evdi";
  notes.scaled_screen.clear();
  notes.input_routes.clear();
  virtual_display::set_doctor_notes_for_tests(notes);
  const auto chosen = host_virtual_display_warnings();
  ASSERT_EQ(chosen.size(), 1U);
  EXPECT_NE(chosen[0].at("message").get<std::string>().find("Host Virtual Display Backend is set to EVDI"), std::string::npos);
  EXPECT_NE(chosen[0].at("action").get<std::string>().find("Automatic"), std::string::npos);

  virtual_display::set_doctor_notes_for_tests(std::nullopt);
}

TEST(StreamStatsTests, PlayersAreNumberedInTheOrderTheirPadsAppeared) {
  // Two clients with a pad each both hold controller 0 in their own session, and both read as
  // player 1. A game numbers them by when each pad appeared, and so does the list.
  stream_stats::note_virtual_pad(3, 0, "Xbox One");
  stream_stats::note_virtual_pad(1, 0, "DualSense");
  stream_stats::note_virtual_pad(2, 1, "Nintendo Pro");
  stream_stats::forget_virtual_pad(2);

  const auto json = nlohmann::json::parse(stream_stats::get_current().to_json());
  const auto &pads = json.at("controller_input").at("pads");
  ASSERT_EQ(pads.size(), 2U);
  EXPECT_EQ(pads[0].at("player"), 1);
  EXPECT_EQ(pads[0].at("kind"), "Xbox One");
  EXPECT_EQ(pads[1].at("player"), 2);
  EXPECT_EQ(pads[1].at("kind"), "DualSense");

  stream_stats::forget_virtual_pad(3);
  stream_stats::forget_virtual_pad(1);
}

TEST(StreamStatsDoctorTests, SaysNothingAboutHostVirtualDisplayWhenItWorkedOrIsNotPlasma) {
  virtual_display::doctor_notes_t notes;
  notes.plasma = true;
  notes.last_backend = virtual_display::backend_e::KWIN_VIRTUAL_OUTPUT;
  notes.preference = "auto";
  // Pointed at the screen, and a device pointed at no screen after the stream: nothing to say.
  notes.input_routes.push_back({"Touch passthrough", "Virtual-polaris-0", true, ""});
  notes.input_routes.push_back({"Pen passthrough", "", false, "KWin went away"});
  virtual_display::set_doctor_notes_for_tests(notes);
  EXPECT_TRUE(host_virtual_display_warnings().empty());

  // No screen made yet: no backend to question.
  notes.last_backend.reset();
  virtual_display::set_doctor_notes_for_tests(notes);
  EXPECT_TRUE(host_virtual_display_warnings().empty());

  // Off Plasma none of this applies, whatever the notes say.
  notes.plasma = false;
  notes.last_backend = virtual_display::backend_e::EVDI;
  notes.scaled_screen = "DVI-I-1";
  notes.scaled_screen_scale = 1.35;
  virtual_display::set_doctor_notes_for_tests(notes);
  EXPECT_TRUE(host_virtual_display_warnings().empty());

  virtual_display::set_doctor_notes_for_tests(std::nullopt);
}
#endif

namespace {
  const nlohmann::json *find_doctor_evidence(const nlohmann::json &doctor, const std::string &id) {
    for (const auto &item : doctor.at("evidence")) {
      if (item.value("id", std::string {}) == id) {
        return &item;
      }
    }
    return nullptr;
  }
}  // namespace

TEST(StreamStatsDoctorTests, NamesADisplayModeOverrideThatReplacedTheClientsRequest) {
  // A support bundle said "can't select 1080p". The pairing pinned 4K, the client's 1080p request
  // was replaced without a word to the client, and only raw log text said so.
  stream_stats::stats_t stats {};
  stats.streaming = true;
  stats.display_mode_requested = "1920x1080x60";
  stats.display_mode_applied = "3840x2160x60";
  stats.display_mode_pinned_by_host = true;

  const auto doctor = stream_stats::build_doctor_json(stats, {{"primary_issue", "steady"}, {"grade", "good"}});

  const auto *row = find_doctor_evidence(doctor, "display_mode_decision");
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(row->at("value"), "3840x2160x60");
  EXPECT_EQ(row->at("status"), "watch");
  const auto detail = row->at("detail").get<std::string>();
  EXPECT_NE(detail.find("Display Mode Override"), std::string::npos);
  EXPECT_NE(detail.find("1920x1080x60"), std::string::npos);
  // Where to clear it, by the name the web UI gives that page.
  EXPECT_NE(detail.find("Devices page"), std::string::npos);
}

TEST(StreamStatsDoctorTests, AnOverrideThatMatchesTheRequestIsOnlyInformation) {
  stream_stats::stats_t stats {};
  stats.streaming = true;
  stats.display_mode_requested = "1920x1080x60";
  stats.display_mode_applied = "1920x1080x60";
  stats.display_mode_pinned_by_host = true;

  const auto doctor = stream_stats::build_doctor_json(stats, {{"primary_issue", "steady"}, {"grade", "good"}});

  const auto *row = find_doctor_evidence(doctor, "display_mode_decision");
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(row->at("status"), "info");
  EXPECT_NE(row->at("detail").get<std::string>().find("matches"), std::string::npos);
}

TEST(StreamStatsDoctorTests, TheClientsOwnDisplayModeIsOnlyInformation) {
  stream_stats::stats_t stats {};
  stats.streaming = true;
  stats.display_mode_requested = "1280x800x60";
  stats.display_mode_applied = "1280x800x60";

  const auto doctor = stream_stats::build_doctor_json(stats, {{"primary_issue", "steady"}, {"grade", "good"}});

  const auto *row = find_doctor_evidence(doctor, "display_mode_decision");
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(row->at("status"), "info");
  EXPECT_EQ(row->at("detail"), "Polaris used the display mode the client asked for.");
}

TEST(StreamStatsDoctorTests, PointsATailnetClientAtTheRelayCheck) {
  // The same bundle's microstutter came from a client that had moved from the LAN to Tailscale.
  // Informational: a direct tailnet path is fine, so the row says what to check, not what is wrong.
  stream_stats::stats_t stats {};
  stats.streaming = true;
  stats.client_network_path = "cgnat";

  const auto doctor = stream_stats::build_doctor_json(stats, {{"primary_issue", "steady"}, {"grade", "good"}});

  const auto *row = find_doctor_evidence(doctor, "client_network_path");
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(row->at("value"), "cgnat");
  EXPECT_EQ(row->at("status"), "info");
  EXPECT_NE(row->at("detail").get<std::string>().find("tailscale ping"), std::string::npos);
}

namespace {
  stream_stats::stats_t one_stream_from(const std::string &name, const std::string &family) {
    stream_stats::stats_t stats {};
    stats.streaming = true;
    stats.client_name = name;
    stats.client_ip = "10.0.0.50";
    stream_stats::client_stats_t client;
    client.name = name;
    client.ip = stats.client_ip;
    client.client_family = family;
    stats.clients.push_back(client);
    return stats;
  }
}  // namespace

TEST(StreamStatsDoctorTests, NamesAMoonlightClientAndWhatItCannotUse) {
  // A Moonlight or Artemis stream read like any other, so a stream that can never report media loss
  // or switch Live Tuning from the client looked as though something on it was broken.
  const auto stats = one_stream_from("Living Room TV", "moonlight");

  const auto doctor = stream_stats::build_doctor_json(stats, {{"primary_issue", "steady"}, {"grade", "good"}});

  const auto *row = find_doctor_evidence(doctor, "client_family");
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(row->at("label"), "Client");
  EXPECT_EQ(row->at("value"), "moonlight");
  EXPECT_EQ(row->at("status"), "info");
  const auto detail = row->at("detail").get<std::string>();
  EXPECT_EQ(detail.rfind("Living Room TV speaks only the Moonlight protocol, as Moonlight and Artemis do.", 0), 0U)
    << detail;
  EXPECT_NE(detail.find("no media loss"), std::string::npos) << detail;
  for (const auto *nova_only : {"PyroWave", "choosing the launch mode per launch", "Live Tuning from the client"}) {
    EXPECT_NE(detail.find(nova_only), std::string::npos) << nova_only;
  }
  // Artemis's virtual display option asks for Host Virtual Display for one launch, so the row does not
  // say only Nova can ask for a mode (compatibility.md, the Launch mode per launch row).
  EXPECT_NE(detail.find("though Artemis can ask for Host Virtual Display"), std::string::npos) << detail;
  EXPECT_NE(detail.find("Live Tuning on Mission Control still tunes this stream."), std::string::npos) << detail;
  // A Moonlight player reads this too, and has no Play Setup to go to.
  EXPECT_EQ(detail.find("Play Setup"), std::string::npos) << detail;
  // Information, never a finding: nothing is wrong with a Moonlight stream for being one.
  const auto without_kind = stream_stats::build_doctor_json(one_stream_from("Living Room TV", ""),
                                                            {{"primary_issue", "steady"}, {"grade", "good"}});
  EXPECT_EQ(doctor.at("primary_issue"), without_kind.at("primary_issue"));
  EXPECT_EQ(doctor.at("traffic_light"), without_kind.at("traffic_light"));
  EXPECT_EQ(doctor.at("severity"), without_kind.at("severity"));
}

TEST(StreamStatsDoctorTests, NamesNovaWithoutClaimingWhichNova) {
  // Nothing in the stream says whether Nova runs on Android or Linux, so the row does not guess.
  const auto stats = one_stream_from("RetroidPocket6", "nova");

  const auto doctor = stream_stats::build_doctor_json(stats, {{"primary_issue", "steady"}, {"grade", "good"}});

  const auto *row = find_doctor_evidence(doctor, "client_family");
  ASSERT_NE(row, nullptr);
  EXPECT_EQ(row->at("value"), "nova");
  EXPECT_EQ(row->at("status"), "info");
  EXPECT_EQ(row->at("detail"), "RetroidPocket6 is Nova, for Android or for Linux; Polaris cannot tell which from the stream.");
}

TEST(StreamStatsDoctorTests, NamesNoClientKindItWasNotGiven) {
  // A session given no kind, a top-level name no session entry carries, and no stream at all each
  // leave the row out rather than guess Moonlight.
  EXPECT_EQ(find_doctor_evidence(stream_stats::build_doctor_json(one_stream_from("Deck", ""), nlohmann::json::object()),
                                 "client_family"),
            nullptr);

  auto other_name = one_stream_from("Deck", "nova");
  other_name.client_name = "Pixel";
  EXPECT_EQ(find_doctor_evidence(stream_stats::build_doctor_json(other_name, nlohmann::json::object()), "client_family"),
            nullptr);

  auto ended = one_stream_from("Deck", "moonlight");
  ended.streaming = false;
  EXPECT_EQ(find_doctor_evidence(stream_stats::build_doctor_json(ended, nlohmann::json::object()), "client_family"),
            nullptr);
}

TEST(StreamStatsDoctorTests, AStreamIsNovaOnlyWhenItsPairingRecordSaysSo) {
  // Every RTSP stream is a paired device's launch, and the host marks a device Nova the first time it
  // calls the Polaris API. Any other record is a client that speaks only the Moonlight protocol.
  EXPECT_EQ(stream_stats::client_family_for_stream("nova"), "nova");
  EXPECT_EQ(stream_stats::client_family_for_stream(""), "moonlight");
  EXPECT_EQ(stream_stats::client_family_for_stream("Nova"), "moonlight");

  // The stream start hands the launch's record to the stats, read from the launch session and not
  // from the pairing state: nvhttp holds the pairing lock while it asks RTSP for its sessions, and a
  // stream starts under RTSP's session lock, so looking it up there could deadlock.
  std::ifstream input(std::filesystem::path {POLARIS_SOURCE_DIR} / "src/stream.cpp");
  ASSERT_TRUE(input.good());
  std::ostringstream contents;
  contents << input.rdbuf();
  const auto source = contents.str();
  EXPECT_NE(source.find("session->client_family = launch_session.client_family;"), std::string::npos);
  EXPECT_NE(source.find("stream_stats::client_family_for_stream(session.client_family)"), std::string::npos);
}

TEST(StreamStatsDoctorTests, TheClientKindFollowsTheStreamTheTopLevelNames) {
  // A watcher starts after the owner and becomes the top-level client_name, as every session start
  // does, so the top-level kind is looked up by that session and not copied from the first entry.
  stream_stats::update_stream_active(false);
  auto reset = util::fail_guard([] { stream_stats::update_stream_active(false); });
  stream_stats::add_client("10.0.0.5", "Owner", 401, "nova");
  stream_stats::update_stream_active(true, "Owner", "10.0.0.5");
  auto json = nlohmann::json::parse(stream_stats::get_current().to_json());
  EXPECT_EQ(json.at("client_family"), "nova");
  EXPECT_EQ(json.at("clients").at(0).at("client_family"), "nova");

  stream_stats::add_client("10.0.0.6", "Television", 402, "moonlight");
  stream_stats::update_stream_active(true, "Television", "10.0.0.6");
  json = nlohmann::json::parse(stream_stats::get_current().to_json());
  EXPECT_EQ(json.at("client_family"), "moonlight");
  EXPECT_EQ(json.at("clients").at(1).at("client_family"), "moonlight");

  // The caller that says nothing leaves the entry without a kind, and the entry says nothing.
  stream_stats::add_client("10.0.0.7", "Legacy", 403);
  json = nlohmann::json::parse(stream_stats::get_current().to_json());
  EXPECT_FALSE(json.at("clients").at(2).contains("client_family"));

  stream_stats::remove_client("10.0.0.7", 403);
  stream_stats::remove_client("10.0.0.6", 402);
  json = nlohmann::json::parse(stream_stats::get_current().to_json());
  EXPECT_EQ(json.at("client_name"), "Owner");
  EXPECT_EQ(json.at("client_family"), "nova");

  stream_stats::remove_client("10.0.0.5", 401);
  json = nlohmann::json::parse(stream_stats::get_current().to_json());
  EXPECT_EQ(json.at("client_family"), "");
}

TEST(StreamStatsDoctorTests, SaysNothingAboutPathOrDisplayModeBeforeAnyLaunch) {
  stream_stats::stats_t stats {};

  const auto doctor = stream_stats::build_doctor_json(stats, {{"primary_issue", "steady"}, {"grade", "good"}});

  EXPECT_EQ(find_doctor_evidence(doctor, "client_network_path"), nullptr);
  EXPECT_EQ(find_doctor_evidence(doctor, "display_mode_decision"), nullptr);
}

TEST(StreamStatsDoctorTests, PathAndDisplayModeSurviveTheEndOfTheStream) {
  // Support bundles get exported after disconnecting, and that bundle was. The path is kept as
  // its kind; the address itself goes with the rest of the session.
  stream_stats::record_display_mode_decision("1920x1080x60", "3840x2160x60", true);
  stream_stats::update_stream_active(true, "client", "100.109.196.18");

  stream_stats::update_stream_active(false, "", "");

  const auto after = stream_stats::get_current();
  EXPECT_FALSE(after.streaming);
  EXPECT_EQ(after.client_ip, "");
  EXPECT_EQ(after.client_network_path, "cgnat");
  EXPECT_EQ(after.display_mode_requested, "1920x1080x60");
  EXPECT_EQ(after.display_mode_applied, "3840x2160x60");
  EXPECT_TRUE(after.display_mode_pinned_by_host);

  const auto doctor = stream_stats::build_doctor_json(after, nlohmann::json::object());
  const auto *path_row = find_doctor_evidence(doctor, "client_network_path");
  ASSERT_NE(path_row, nullptr);
  EXPECT_EQ(path_row->at("detail").get<std::string>().rfind("From the last stream. ", 0), 0U);
  const auto *mode_row = find_doctor_evidence(doctor, "display_mode_decision");
  ASSERT_NE(mode_row, nullptr);
  EXPECT_EQ(mode_row->at("detail").get<std::string>().rfind("From the last launch. ", 0), 0U);

  const auto json = nlohmann::json::parse(after.to_json());
  EXPECT_EQ(json.at("client_network_path"), "cgnat");
  EXPECT_EQ(json.at("display_mode_decision").at("applied"), "3840x2160x60");

  stream_stats::record_display_mode_decision("", "", false);
}

TEST(StreamStatsDoctorTests, SaysNothingAboutCaptureBeforeAnythingHasBeenEvaluated) {
  // The accessor must not report a problem it has never looked for: Doctor is asked for a report
  // before startup has finished, and an empty source set then means "not yet", not "broken".
  platf::set_capture_sources_missing_for_tests(false);

  stream_stats::stats_t stats {};
  const auto doctor = stream_stats::build_doctor_json(stats, {{"primary_issue", "steady"}, {"grade", "good"}});

  for (const auto &warning :
       doctor.at("advanced_evidence").at("linux_gpu_profile").at("configuration_warnings")) {
    EXPECT_NE(warning.at("id"), "no_capture_backend");
  }
}

TEST(StreamStatsDoctorTests, ReportsACaptureBackendThatWasSubstituted) {
  // Before this, the only trace of a substituted capture backend was one warning in the middle of
  // startup, while the host went on serving with a backend nobody chose. See #677.
  platf::set_capture_backend_substitution_for_tests("wlr -> portal");

  stream_stats::stats_t stats {};
  const auto doctor = stream_stats::build_doctor_json(stats, {{"primary_issue", "steady"}, {"grade", "good"}});

  bool saw_warning = false;
  for (const auto &warning :
       doctor.at("advanced_evidence").at("linux_gpu_profile").at("configuration_warnings")) {
    if (warning.at("id") != "capture_backend_substituted") {
      continue;
    }
    saw_warning = true;
    EXPECT_EQ(warning.at("severity"), "warning");
    EXPECT_NE(warning.at("message").get<std::string>().find("wlr -> portal"), std::string::npos);
  }
  EXPECT_TRUE(saw_warning);

  platf::set_capture_backend_substitution_for_tests("");
}

TEST(StreamStatsDoctorTests, SaysNothingWhenTheConfiguredCaptureBackendWasUsed) {
  platf::set_capture_backend_substitution_for_tests("");

  stream_stats::stats_t stats {};
  const auto doctor = stream_stats::build_doctor_json(stats, {{"primary_issue", "steady"}, {"grade", "good"}});

  for (const auto &warning :
       doctor.at("advanced_evidence").at("linux_gpu_profile").at("configuration_warnings")) {
    EXPECT_NE(warning.at("id"), "capture_backend_substituted");
  }
}

TEST(StreamStatsDoctorTests, NamesTheCapabilityWhenKmsWasRefusedAndNothingElseCaptures) {
  // capture = kms on a binary without CAP_SYS_ADMIN: kmsgrab finds the display, cannot read a
  // framebuffer, and the host serves with no capture at all. The journal says which command to
  // run, once, at boot. The Doctor has to say it where the person is standing.
  LinuxDisplayConfigGuard guard;
  config::video.linux_display.stream_mode = "desktop_display";
  config::video.linux_display.use_cage_compositor = false;
  platf::set_capture_sources_missing_for_tests(true);
  platf::set_kms_capture_refused_for_tests(true);

  // drm is kms under another name: dispatch opens KMS for it, and the probe records its refusal.
  for (const auto capture : {"kms", "drm"}) {
    config::video.capture = capture;
    stream_stats::stats_t stats {};
    const auto doctor = stream_stats::build_doctor_json(stats, {{"primary_issue", "steady"}, {"grade", "good"}});

    bool saw_warning = false;
    for (const auto &warning :
         doctor.at("advanced_evidence").at("linux_gpu_profile").at("configuration_warnings")) {
      if (warning.at("id") != "kms_capture_needs_capability") {
        continue;
      }
      saw_warning = true;
      EXPECT_EQ(warning.at("severity"), "fail");
      EXPECT_NE(warning.at("message").get<std::string>().find("CAP_SYS_ADMIN"), std::string::npos);
      EXPECT_NE(warning.at("action").get<std::string>().find("--setup-host --enable-kms"), std::string::npos);
      // The helper package keeps the capability across updates, and the first run may only park it
      // until a login, so the finding says to follow the command, not to repeat it after updates.
      EXPECT_EQ(warning.at("message").get<std::string>().find("replaces the binary without it"), std::string::npos);
      EXPECT_NE(warning.at("message").get<std::string>().find("polaris-kms package"), std::string::npos);
      EXPECT_EQ(warning.at("action").get<std::string>().find("after each install or update"), std::string::npos);
      EXPECT_NE(warning.at("action").get<std::string>().find("once and do what it prints"), std::string::npos);
    }
    EXPECT_TRUE(saw_warning) << capture;
  }

  platf::set_kms_capture_refused_for_tests(false);
  platf::set_capture_sources_missing_for_tests(false);
}

TEST(StreamStatsDoctorTests, NamesTheKmsCapabilityOnlyWhereCaptureAsksForKms) {
  // A private compositor mode captures through wlroots whatever capture says, and the evaluation
  // still probes KMS for a host set to kms or drm there. Telling that host to grant a capability
  // changed nothing about its stream. The finding asks what a launch refusal asks: whether KMS is
  // what capture asks for in the live mode, which is kms, drm, or auto, whose search reaches KMS.
  LinuxDisplayConfigGuard guard;
  platf::set_capture_backend_substitution_for_tests("");
  platf::set_capture_sources_missing_for_tests(false);
  platf::set_kms_capture_refused_for_tests(true);
  const auto reports_capability = []() {
    stream_stats::stats_t stats {};
    const auto doctor = stream_stats::build_doctor_json(stats, {{"primary_issue", "steady"}, {"grade", "good"}});
    for (const auto &warning :
         doctor.at("advanced_evidence").at("linux_gpu_profile").at("configuration_warnings")) {
      if (warning.at("id") == "kms_capture_needs_capability") {
        return true;
      }
    }
    return false;
  };

  for (const auto mode : {"headless_stream", "windowed_stream"}) {
    config::video.linux_display.stream_mode = mode;
    config::video.linux_display.use_cage_compositor = true;
    for (const auto capture : {"kms", "drm", ""}) {
      config::video.capture = capture;
      EXPECT_FALSE(reports_capability()) << mode << " capture=[" << capture << "]";
    }
  }

  config::video.linux_display.stream_mode = "desktop_display";
  config::video.linux_display.use_cage_compositor = false;
  for (const auto capture : {"kms", "drm", ""}) {
    config::video.capture = capture;
    EXPECT_TRUE(reports_capability()) << "desktop_display capture=[" << capture << "]";
  }
  // A host set to another backend never asked for KMS.
  for (const auto capture : {"portal", "kwin", "wlr"}) {
    config::video.capture = capture;
    EXPECT_FALSE(reports_capability()) << "desktop_display capture=[" << capture << "]";
  }

  // A launch into Gamescope Stream or the dongle fills an unset capture with the portal before it
  // asks for anything, so an idle host in either mode with capture unset never asks for KMS,
  // whatever its startup search probed. That is the #635 reporter's route. An explicit kms or drm
  // is kept in both modes and asks for KMS by name.
  for (const auto mode : {"gamescope_stream", "headless_dongle"}) {
    config::video.linux_display.stream_mode = mode;
    config::video.capture = "";
    EXPECT_FALSE(reports_capability()) << mode << " capture unset";
    for (const auto capture : {"kms", "drm"}) {
      config::video.capture = capture;
      EXPECT_TRUE(reports_capability()) << mode << " capture=[" << capture << "]";
    }
  }
  // The fill comes before the mode's own decision, as it does in a launch, so a dongle host set to
  // kms whose KMS captured nothing is still asked about the capability rather than read as filled.
  platf::set_capture_backend_substitution_for_tests("kms -> portal");
  config::video.linux_display.stream_mode = "headless_dongle";
  config::video.capture = "kms";
  EXPECT_TRUE(reports_capability()) << "headless_dongle capture=[kms] substituted";
  platf::set_capture_backend_substitution_for_tests("");

  platf::set_kms_capture_refused_for_tests(false);
}

TEST(StreamStatsDoctorTests, DoesNotAskForAKmsCapabilityTheHostSetAsideOnPurpose) {
  // Autodetect in Mirror Desktop starts Polaris without capabilities, so the portal and KWin accept
  // it, and its search then meets KMS without one. That refusal is the design: the stream captures
  // through the portal, and --enable-kms, which a host running the helper has already run, would
  // change nothing about it.
  LinuxDisplayConfigGuard guard;
  platf::set_capture_backend_substitution_for_tests("");
  platf::set_capture_sources_missing_for_tests(false);
  platf::set_kms_capture_refused_for_tests(true);
  config::video.linux_display.stream_mode = "desktop_display";
  config::video.linux_display.use_cage_compositor = false;
  config::video.capture = "";
  const auto reports_capability = []() {
    stream_stats::stats_t stats {};
    const auto doctor = stream_stats::build_doctor_json(stats, {{"primary_issue", "steady"}, {"grade", "good"}});
    for (const auto &warning :
         doctor.at("advanced_evidence").at("linux_gpu_profile").at("configuration_warnings")) {
      if (warning.at("id") == "kms_capture_needs_capability") {
        return true;
      }
    }
    return false;
  };

  const bool before = platf::kms_readiness::capability_set_aside();
  platf::kms_readiness::note_capability_set_aside(true);
  EXPECT_FALSE(reports_capability());
  platf::kms_readiness::note_capability_set_aside(false);
  EXPECT_TRUE(reports_capability()) << "a process that kept its capabilities searched KMS and was refused";
  platf::kms_readiness::note_capability_set_aside(before);

  platf::set_kms_capture_refused_for_tests(false);
}

TEST(StreamStatsDoctorTests, ASubstitutedKmsNamesTheCapabilityNotTheCompositor) {
  // The substitution text was written for wlr on KDE. Read for kms it blames compositor
  // protocols and tells the user to stop using the one backend that carries HDR.
  LinuxDisplayConfigGuard guard;
  config::video.capture = "kms";
  platf::set_capture_backend_substitution_for_tests("kms -> portal");
  platf::set_kms_capture_refused_for_tests(true);

  stream_stats::stats_t stats {};
  const auto doctor = stream_stats::build_doctor_json(stats, {{"primary_issue", "steady"}, {"grade", "good"}});

  bool saw_substituted = false;
  bool saw_capability = false;
  for (const auto &warning :
       doctor.at("advanced_evidence").at("linux_gpu_profile").at("configuration_warnings")) {
    const auto id = warning.at("id").get<std::string>();
    if (id == "capture_backend_substituted") {
      saw_substituted = true;
      EXPECT_NE(warning.at("message").get<std::string>().find("CAP_SYS_ADMIN"), std::string::npos);
      EXPECT_EQ(warning.at("message").get<std::string>().find("wlroots capture protocols"), std::string::npos);
      EXPECT_NE(warning.at("action").get<std::string>().find("--enable-kms"), std::string::npos);
      EXPECT_EQ(warning.at("action").get<std::string>().find("after each install or update"), std::string::npos);
    }
    if (id == "kms_capture_needs_capability") {
      saw_capability = true;
      EXPECT_EQ(warning.at("severity"), "warning");
    }
  }
  EXPECT_TRUE(saw_substituted);
  EXPECT_TRUE(saw_capability);

  platf::set_kms_capture_refused_for_tests(false);
  platf::set_capture_backend_substitution_for_tests("");
}

TEST(StreamStatsDoctorTests, SaysNothingAboutTheKmsCapabilityUnlessKmsWasRefused) {
  LinuxDisplayConfigGuard guard;
  config::video.capture = "wlr";
  platf::set_capture_backend_substitution_for_tests("wlr -> portal");
  platf::set_kms_capture_refused_for_tests(false);

  stream_stats::stats_t stats {};
  const auto doctor = stream_stats::build_doctor_json(stats, {{"primary_issue", "steady"}, {"grade", "good"}});

  for (const auto &warning :
       doctor.at("advanced_evidence").at("linux_gpu_profile").at("configuration_warnings")) {
    EXPECT_NE(warning.at("id"), "kms_capture_needs_capability");
    if (warning.at("id") == "capture_backend_substituted") {
      EXPECT_EQ(warning.at("message").get<std::string>().find("CAP_SYS_ADMIN"), std::string::npos);
    }
  }

  platf::set_capture_backend_substitution_for_tests("");
}

TEST(StreamStatsDoctorTests, HdrFindingNamesTheRecipeAndTheCapabilityWhenKmsWasRefused) {
  // Explaining why HDR did not engage is only half of it; the person wants to know what to
  // change. Now that the recipe is proven (kms capture on a mode that shows the real HDR
  // output), the finding can name it, and name the capability when that is what stopped it.
  LinuxDisplayConfigGuard guard;
  config::video.capture = "wlr";
  config::video.linux_display.use_cage_compositor = true;
  platf::set_kms_capture_refused_for_tests(true);

  stream_stats::stats_t stats {};
  stats.dynamic_range = 1;
  stats.runtime_effective_headless = true;
  stats.display_hdr = false;
  stats.hdr_metadata_available = false;
  stats.stream_hdr_enabled = false;

  const auto doctor = stream_stats::build_doctor_json(stats, {{"primary_issue", "steady"}, {"grade", "good"}});

  bool saw_hdr = false;
  for (const auto &warning :
       doctor.at("advanced_evidence").at("linux_gpu_profile").at("configuration_warnings")) {
    if (warning.at("id") != "hdr_capture_path_cannot_report_hdr") {
      continue;
    }
    saw_hdr = true;
    const auto action = warning.at("action").get<std::string>();
    EXPECT_NE(action.find("capture = kms"), std::string::npos);
    EXPECT_NE(action.find("Mirror Desktop"), std::string::npos);
    EXPECT_NE(action.find("--enable-kms"), std::string::npos);
    // The sentence that gives the recipe names only the mode that keeps kms. It used to name Host
    // Virtual Display, Desktop Takeover and Gamescope, which the host's own warnings say rewrite it.
    const auto recipe_start = action.find("capture = kms with");
    ASSERT_NE(recipe_start, std::string::npos) << action;
    const auto recipe = action.substr(recipe_start, action.find('.', recipe_start) - recipe_start);
    EXPECT_NE(recipe.find("Mirror Desktop"), std::string::npos) << recipe;
    for (const auto mode : {"Host Virtual Display", "Desktop Takeover", "Gamescope", "Private Stream"}) {
      EXPECT_EQ(recipe.find(mode), std::string::npos) << mode << " in: " << recipe;
    }
    // It used to call Mirror Desktop the one mode that keeps kms and say a Gamescope Stream session
    // is captured through the portal. A host whose own mode is Gamescope Stream or the dongle keeps
    // kms, and a Host Virtual Display host's load replaced kms for its Mirror Desktop launches too.
    EXPECT_EQ(action.find("the stream mode that keeps"), std::string::npos) << action;
    EXPECT_EQ(action.find("Gamescope Stream session is captured through the portal"), std::string::npos) << action;
    EXPECT_NE(action.find("Gamescope Stream and the dongle keep kms only as the host's own mode"), std::string::npos)
      << action;
    EXPECT_NE(action.find("launch into either from another mode captures through the portal"), std::string::npos)
      << action;
    EXPECT_NE(action.find("own mode is Host Virtual Display or Desktop Takeover"), std::string::npos) << action;
    EXPECT_NE(action.find("until Polaris restarts"), std::string::npos) << action;
  }
  EXPECT_TRUE(saw_hdr);

#ifdef __linux__
  // What the action says is what the policy does, for the host's own mode and for a launch that
  // enters another one.
  using stream_display_policy::capture_filled_for_mode;
  using stream_display_policy::capture_for_host_virtual_display_backend;
  using stream_display_policy::capture_for_mode;
  using stream_display_policy::capture_for_session_transition;
  // Mirror Desktop keeps kms as the host's mode, and a launch into it keeps the kms a host in
  // Private Stream, Gamescope Stream or the dongle still holds.
  EXPECT_EQ(capture_for_mode("kms", "desktop_display", false, false, false), "kms");
  for (const auto from : {"headless_stream", "windowed_stream", "gamescope_stream", "headless_dongle"}) {
    EXPECT_EQ(capture_for_session_transition(from, "desktop_display", "kms"), "kms") << from;
  }
  // Loading Host Virtual Display or Desktop Takeover puts another backend in place of kms until
  // restart, and a launch into Mirror Desktop starts from that replacement, not from kms.
  for (const auto backend : {virtual_display::backend_e::EVDI,
                             virtual_display::backend_e::KSCREEN_DOCTOR,
                             virtual_display::backend_e::KWIN_VIRTUAL_OUTPUT,
                             virtual_display::backend_e::WAYLAND_WLR}) {
    const auto loaded = capture_for_host_virtual_display_backend(backend, "kms");
    EXPECT_NE(loaded, "kms") << virtual_display::backend_name(backend);
    for (const auto from : {"host_virtual_display", "desktop_takeover"}) {
      EXPECT_NE(capture_for_session_transition(from, "desktop_display", loaded), "kms")
        << from << " on " << virtual_display::backend_name(backend);
    }
  }
  // Gamescope Stream and the dongle keep kms as the host's own mode, filled or not, and a launch
  // that enters either from another mode captures through the portal.
  for (const auto mode : {"gamescope_stream", "headless_dongle"}) {
    EXPECT_EQ(capture_for_mode(capture_filled_for_mode(mode, "kms"), mode, false, false, false), "kms") << mode;
    EXPECT_EQ(capture_for_session_transition("desktop_display", mode, "kms"), "portal") << mode;
  }
  // Private Stream captures through wlroots whatever capture says.
  for (const auto mode : {"headless_stream", "windowed_stream"}) {
    EXPECT_EQ(capture_for_mode("kms", mode, true, false, false), "wlr") << mode;
  }
#endif

  platf::set_kms_capture_refused_for_tests(false);
}

TEST(StreamStatsDoctorTests, NamesTheCapturePathWhenHdrWasAskedForAndNotDelivered) {
  // The host already knew why and only ever said so over the session-status route, while a
  // stream was live. A person who ticks "request HDR", sees SDR and goes looking for a reason
  // is standing in front of the console with nothing streaming.
  LinuxDisplayConfigGuard guard;
  config::video.capture = "wlr";
  config::video.linux_display.use_cage_compositor = true;

  stream_stats::stats_t stats {};
  stats.dynamic_range = 1;
  stats.runtime_effective_headless = true;
  stats.display_hdr = false;
  stats.hdr_metadata_available = false;
  stats.stream_hdr_enabled = false;

  const auto doctor = stream_stats::build_doctor_json(stats, {{"primary_issue", "steady"}, {"grade", "good"}});

  bool saw_hdr_evidence = false;
  for (const auto &entry : doctor.at("evidence")) {
    if (entry.at("id") != "hdr") {
      continue;
    }
    saw_hdr_evidence = true;
    EXPECT_EQ(entry.at("status"), "watch");
    EXPECT_EQ(entry.at("value"), "sdr_10bit");
    EXPECT_NE(entry.at("detail").get<std::string>().find("10-bit SDR, not HDR"), std::string::npos);
  }
  EXPECT_TRUE(saw_hdr_evidence);

  bool saw_warning = false;
  for (const auto &warning :
       doctor.at("advanced_evidence").at("linux_gpu_profile").at("configuration_warnings")) {
    if (warning.at("id") != "hdr_capture_path_cannot_report_hdr") {
      continue;
    }
    saw_warning = true;
    EXPECT_EQ(warning.at("severity"), "info");
    // The wlroots path never overrides is_hdr(), so this is permanent, not a bad moment.
    EXPECT_NE(warning.at("message").get<std::string>().find("wlroots"), std::string::npos);
    EXPECT_NE(warning.at("action").get<std::string>().find("KMS/DRM"), std::string::npos);
  }
  EXPECT_TRUE(saw_warning);
}

TEST(StreamStatsDoctorTests, HdrEvidenceStaysInformationalWhenNobodyAskedForHdr) {
  // Most hosts never ask for HDR. The row still reports, but it must not nag, and it must not
  // accuse a capture path of failing at something nobody requested.
  LinuxDisplayConfigGuard guard;
  config::video.capture = "wlr";
  config::video.linux_display.use_cage_compositor = true;

  stream_stats::stats_t stats {};
  stats.dynamic_range = 0;

  const auto doctor = stream_stats::build_doctor_json(stats, {{"primary_issue", "steady"}, {"grade", "good"}});

  bool saw_hdr_evidence = false;
  for (const auto &entry : doctor.at("evidence")) {
    if (entry.at("id") != "hdr") {
      continue;
    }
    saw_hdr_evidence = true;
    EXPECT_EQ(entry.at("status"), "info");
    EXPECT_EQ(entry.at("value"), "sdr_8bit");
  }
  EXPECT_TRUE(saw_hdr_evidence);

  for (const auto &warning :
       doctor.at("advanced_evidence").at("linux_gpu_profile").at("configuration_warnings")) {
    EXPECT_NE(warning.at("id"), "hdr_capture_path_cannot_report_hdr");
  }
}

TEST(StreamStatsDoctorTests, NamesTheSavedSettingThatSwitchedHdrOff) {
  // This is the shape that leaves someone with nothing to go on. A saved setting stops HDR being
  // requested at all, so dynamic_range never leaves zero, hdr_downgrade_reason answers "none",
  // and every capability-based check stays quiet while the user re-toggles a client switch that
  // was never the problem. It has to speak without a request having been made.
  LinuxDisplayConfigGuard guard;

  stream_stats::stats_t stats {};
  stats.dynamic_range = 0;
  stats.hdr_policy_hdr = false;
  stats.hdr_policy_reason = "paired_device_hdr_unsupported";
  stats.hdr_policy_device = "RetroidPocket6";

  const auto doctor = stream_stats::build_doctor_json(stats, {{"primary_issue", "steady"}, {"grade", "good"}});

  bool saw_warning = false;
  for (const auto &warning :
       doctor.at("advanced_evidence").at("linux_gpu_profile").at("configuration_warnings")) {
    if (warning.at("id") != "hdr_disabled_by_saved_setting") {
      continue;
    }
    saw_warning = true;
    const auto message = warning.at("message").get<std::string>();
    EXPECT_NE(message.find("device_db.json"), std::string::npos);
    EXPECT_NE(message.find("RetroidPocket6"), std::string::npos);
    EXPECT_NE(warning.at("action").get<std::string>().find("hdr_capable"), std::string::npos);
  }
  EXPECT_TRUE(saw_warning);
}

TEST(StreamStatsDoctorTests, SaysAKwinVirtualScreenHasNoHdr) {
  // Not a saved setting: Host Virtual Display got a screen from KWin, which cannot carry HDR,
  // so an HDR request comes out SDR. It must not be reported as something the user switched off.
  LinuxDisplayConfigGuard guard;

  stream_stats::stats_t stats {};
  stats.hdr_policy_hdr = false;
  stats.hdr_policy_reason = "kwin_virtual_output_sdr";
  stats.hdr_policy_device = "RetroidPocket6";

  const auto doctor = stream_stats::build_doctor_json(stats, {{"primary_issue", "steady"}, {"grade", "good"}});

  bool saw_warning = false;
  for (const auto &warning :
       doctor.at("advanced_evidence").at("linux_gpu_profile").at("configuration_warnings")) {
    EXPECT_NE(warning.at("id"), "hdr_disabled_by_saved_setting");
    if (warning.at("id") != "hdr_unavailable_on_kwin_virtual_screen") {
      continue;
    }
    saw_warning = true;
    EXPECT_NE(warning.at("message").get<std::string>().find("KWin virtual screens carry no HDR"), std::string::npos);
    EXPECT_NE(warning.at("action").get<std::string>().find("Mirror Desktop"), std::string::npos);
  }
  EXPECT_TRUE(saw_warning);
}

TEST(StreamStatsDoctorTests, SaysNothingAboutSavedSettingsWhenHdrWasAllowed) {
  LinuxDisplayConfigGuard guard;

  stream_stats::stats_t stats {};
  stats.hdr_policy_hdr = true;
  stats.hdr_policy_reason = "requested_hdr_setting";
  stats.hdr_policy_device = "RetroidPocket6";

  const auto doctor = stream_stats::build_doctor_json(stats, {{"primary_issue", "steady"}, {"grade", "good"}});

  for (const auto &warning :
       doctor.at("advanced_evidence").at("linux_gpu_profile").at("configuration_warnings")) {
    EXPECT_NE(warning.at("id"), "hdr_disabled_by_saved_setting");
  }
}

TEST(StreamStatsDoctorTests, HdrVerdictSurvivesTheEndOfTheStream) {
  // Same reasoning as the Steam Input finding above: the answer describes the host's capture
  // path, and the person asking the question has already disconnected.
  stream_stats::update_stream_active(true, "client", "10.0.0.5");
  stream_stats::update_dynamic_range(1);
  stream_stats::update_hdr_state(false, false, false, "SDR (Rec. 709)");
  stream_stats::update_hdr_policy(false, "paired_device_hdr_unsupported", "RetroidPocket6");

  stream_stats::update_stream_active(false, "", "");

  const auto after = stream_stats::get_current();
  EXPECT_FALSE(after.streaming);
  EXPECT_EQ(after.dynamic_range, 1);
  EXPECT_FALSE(after.stream_hdr_enabled);
  EXPECT_EQ(stream_stats::hdr_effective_mode(after), "sdr_10bit");
  EXPECT_NE(stream_stats::hdr_downgrade_reason(after), "none");
  EXPECT_EQ(after.hdr_policy_reason, "paired_device_hdr_unsupported");
  EXPECT_EQ(after.hdr_policy_device, "RetroidPocket6");

  stream_stats::update_stream_active(false, "", "");
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

TEST(StreamStatsDoctorTests, EncoderAutoFallbackIsVisibleAsWatchEvidence) {
  stream_stats::stats_t stats {};
  stats.streaming = true;
  stats.capture_transport = platf::frame_transport_e::dmabuf;
  stats.capture_residency = platf::frame_residency_e::gpu;
  stats.encode_target_residency = platf::frame_residency_e::gpu;
  stats.encode_time_ms = 4.0;

  const auto doctor = stream_stats::build_doctor_json(
    stats,
    {
      {"primary_issue", "steady"},
      {"grade", "good"},
      {"encoder_selection", {
        {"selected_encoder", "vaapi"},
        {"fallback_used", true},
        {"reason", "Vulkan did not satisfy the exact live route; selected VA-API instead."}
      }}
    }
  );

  const auto selection = std::find_if(
    doctor.at("evidence").begin(),
    doctor.at("evidence").end(),
    [](const auto &item) {
      return item.value("id", std::string {}) == "encoder_selection";
    }
  );
  ASSERT_NE(selection, doctor.at("evidence").end());
  EXPECT_EQ(selection->at("value"), "vaapi");
  EXPECT_EQ(selection->at("status"), "watch");
  EXPECT_EQ(
    doctor.at("advanced_evidence").at("encoder_selection").at("fallback_used"),
    true
  );
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

TEST(StreamStatsDoctorTests, GradesEncoderTimeAgainstTheActiveFpsBudget) {
  stream_stats::stats_t stats {};
  stats.streaming = true;
  stats.fps = 97.0;
  stats.encode_target_fps = 97.0;
  stats.capture_transport = platf::frame_transport_e::dmabuf;
  stats.capture_residency = platf::frame_residency_e::gpu;
  stats.encode_target_device = "vaapi";
  stats.encode_target_residency = platf::frame_residency_e::gpu;
  stats.encode_time_ms = 10.4;

  const auto high_refresh = stream_stats::build_doctor_json(stats, nlohmann::json::object());
  EXPECT_EQ(high_refresh.at("primary_issue"), "encoder_load");
  EXPECT_EQ(high_refresh.at("traffic_light"), "red");
  EXPECT_EQ(high_refresh.at("status"), "needs_action");

  stats.fps = 60.0;
  stats.encode_target_fps = 60.0;
  const auto sixty_fps = stream_stats::build_doctor_json(stats, nlohmann::json::object());
  EXPECT_EQ(sixty_fps.at("primary_issue"), "encoder_load");
  EXPECT_EQ(sixty_fps.at("traffic_light"), "amber");
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
  mark_doctor_pacing_window_confirmed(stats);

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

TEST(StreamStatsDoctorTests, KeepsStartupPacingUnknownUntilWarmupCompletes) {
  stream_stats::stats_t stats {};
  stats.streaming = true;
  stats.fps = 60.0;
  stats.encode_target_fps = 120.0;
  stats.capture_source_fps = 120.0;
  stats.frame_jitter_ms = 8.3333;
  stats.capture_transport = platf::frame_transport_e::dmabuf;
  stats.capture_residency = platf::frame_residency_e::gpu;
  stats.encode_target_residency = platf::frame_residency_e::gpu;
  stats.video_policy_sample_count = stream_stats::DOCTOR_PACING_WARMUP_SAMPLES - 1;
  stats.pacing_warning_streak = stream_stats::DOCTOR_PACING_WARMUP_SAMPLES - 1;

  const auto doctor = stream_stats::build_doctor_json(
    stats,
    {{"primary_issue", "frame_pacing"}, {"grade", "watch"}}
  );

  EXPECT_EQ(doctor.at("primary_issue"), "none");
  EXPECT_EQ(doctor.at("traffic_light"), "green");
  EXPECT_EQ(doctor.at("confidence").at("basis"), "pacing_window_collecting");
  const auto &pacing = *std::find_if(
    doctor.at("evidence").begin(),
    doctor.at("evidence").end(),
    [](const auto &item) { return item.value("id", "") == "frame_pacing"; }
  );
  EXPECT_EQ(pacing.at("status"), "unknown");
  ASSERT_EQ(doctor.at("suppressed_findings").size(), 1);
  EXPECT_EQ(doctor.at("suppressed_findings").front().at("id"), "pacing_window_collecting");
}

TEST(StreamStatsDoctorTests, RequiresTwoConsecutivePacingWarningsAfterWarmup) {
  stream_stats::stats_t stats {};
  stats.streaming = true;
  stats.fps = 60.0;
  stats.encode_target_fps = 120.0;
  stats.capture_source_fps = 120.0;
  stats.frame_jitter_ms = 8.3333;
  stats.capture_transport = platf::frame_transport_e::dmabuf;
  stats.capture_residency = platf::frame_residency_e::gpu;
  stats.encode_target_residency = platf::frame_residency_e::gpu;
  stats.video_policy_sample_count = stream_stats::DOCTOR_PACING_WARMUP_SAMPLES;
  stats.pacing_warning_streak = stream_stats::DOCTOR_PACING_CONFIRMATION_SAMPLES - 1;

  const auto first = stream_stats::build_doctor_json(
    stats,
    {{"primary_issue", "frame_pacing"}, {"grade", "watch"}}
  );
  EXPECT_EQ(first.at("primary_issue"), "none");
  EXPECT_EQ(first.at("advanced_evidence").at("pacing_coverage").at("ready"), true);
  EXPECT_EQ(first.at("advanced_evidence").at("pacing_coverage").at("confirmed"), false);

  stats.pacing_warning_streak = stream_stats::DOCTOR_PACING_CONFIRMATION_SAMPLES;
  const auto second = stream_stats::build_doctor_json(
    stats,
    {{"primary_issue", "frame_pacing"}, {"grade", "watch"}}
  );
  EXPECT_EQ(second.at("primary_issue"), "frame_pacing");
  EXPECT_EQ(second.at("traffic_light"), "amber");
  EXPECT_EQ(second.at("advanced_evidence").at("pacing_coverage").at("confirmed"), true);
}

TEST(StreamStatsDoctorTests, TracksAndResetsPerStreamPacingCoverage) {
  stream_stats::update_stream_active(false);
  stream_stats::update_stream_active(true, "PacingCoverage", "203.0.113.30");
  for (std::uint64_t i = 0; i < stream_stats::DOCTOR_PACING_WARMUP_SAMPLES; ++i) {
    stream_stats::note_doctor_video_policy_sample(
      120.0, 60.0, 0.0, 0.0, 2.0, 8.3333, 2.0
    );
  }

  const auto active = stream_stats::get_current();
  EXPECT_EQ(active.video_policy_sample_count, stream_stats::DOCTOR_PACING_WARMUP_SAMPLES);
  EXPECT_EQ(active.pacing_warning_streak, stream_stats::DOCTOR_PACING_WARMUP_SAMPLES);

  stream_stats::update_stream_active(false);
  const auto stopped = stream_stats::get_current();
  EXPECT_EQ(stopped.video_policy_sample_count, 0);
  EXPECT_EQ(stopped.pacing_warning_streak, 0);
}

TEST(StreamStatsDoctorTests, SuppressesUnconfirmedPacingSummaryWhenCurrentWindowIsClean) {
  stream_stats::stats_t stats {};
  stats.streaming = true;
  stats.fps = 60.0;
  stats.encode_target_fps = 60.0;
  stats.capture_source_fps = 60.0;
  stats.capture_transport = platf::frame_transport_e::dmabuf;
  stats.capture_residency = platf::frame_residency_e::gpu;
  stats.encode_target_residency = platf::frame_residency_e::gpu;
  stats.video_policy_sample_count = stream_stats::DOCTOR_PACING_WARMUP_SAMPLES;

  const auto doctor = stream_stats::build_doctor_json(
    stats,
    {
      {"primary_issue", "frame_pacing"},
      {"grade", "watch"},
      {"summary", "Old pacing warning"}
    }
  );

  EXPECT_EQ(doctor.at("primary_issue"), "none");
  EXPECT_EQ(doctor.at("summary"), "Streaming telemetry looks ready.");
  EXPECT_EQ(
    doctor.at("confidence").at("basis"),
    "live_evidence_overrode_unconfirmed_pacing"
  );
  ASSERT_EQ(doctor.at("suppressed_findings").size(), 1);
  EXPECT_EQ(
    doctor.at("suppressed_findings").front().at("id"),
    "unconfirmed_frame_pacing"
  );
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

namespace {
  /**
   * A hand-built stream's windowed network verdict, as if its whole window read what its newest
   * fields read. Doctor grades the network from network_verdict alone, so a test that sets the newest
   * readings sets the verdict they would have made: the fast debounce's risk is loss pressure when
   * the loss is over the line, and RTT pressure otherwise.
   */
  stream_stats::stats_t judged(stream_stats::stats_t stats) {
    auto &verdict = stats.network_verdict;
    const bool reading = stats.network_sample_revision > 0;
    verdict.loss_available = reading && stats.packet_loss_available;
    verdict.loss_pct = verdict.loss_available ? stats.packet_loss : 0.0;
    verdict.media_samples = verdict.loss_available ? 20 : 0;
    verdict.frames_expected = verdict.loss_available ? 2400 : 0;
    verdict.frames_lost = verdict.loss_available ? static_cast<std::uint64_t>(std::llround(stats.packet_loss * 24.0)) : 0;
    verdict.loss_elevated = verdict.loss_available && stats.network_risk &&
      stats.packet_loss > stream_stats::network_judge_t::k_loss_enter_pct;
    verdict.rtt_available = reading;
    verdict.rtt_ms = stats.latency_ms;
    verdict.rtt_samples = reading ? 200 : 0;
    verdict.rtt_elevated = reading && stats.network_risk && !verdict.loss_elevated;
    verdict.control_loss_available = reading && stats.control_channel_samples > 0;
    verdict.control_loss_pct = verdict.control_loss_available ? stats.control_channel_packet_loss : 0.0;
    verdict.control_samples = verdict.control_loss_available ? 200 : 0;
    verdict.control_loss_elevated = verdict.control_loss_available &&
      stats.control_channel_packet_loss >= stream_stats::network_judge_t::k_loss_enter_pct;
    verdict.risk = verdict.loss_elevated || verdict.rtt_elevated;
    return stats;
  }

  nlohmann::json judged_doctor(const stream_stats::stats_t &stats,
                               const nlohmann::json &health = nlohmann::json::object(),
                               std::string_view app_uuid = {}) {
    return stream_stats::build_doctor_json(judged(stats), health, app_uuid);
  }

  /**
   * Network pressure as the window judges it: the readings before this leave the window, and enough
   * reports at this RTT and loss arrive for a verdict.
   */
  void sustain_network_pressure(double latency_ms, double loss_pct) {
    stream_stats::age_network_judge_for_tests(stream_stats::network_judge_t::k_window);
    for (int i = 0; i < stream_stats::network_judge_t::k_min_media_samples; ++i) {
      stream_stats::update_network_stats(latency_ms, loss_pct, 1000);
    }
  }
}  // namespace

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

  const auto doctor = judged_doctor(
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

  const auto doctor = judged_doctor(stats);

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

TEST(StreamStatsDoctorTests, ControlChannelFindingSaysOnlyWhatTheWindowJudged) {
  stream_stats::stats_t stats {};
  stats.streaming = true;
  stats.fps = 60.0;
  stats.encode_target_fps = 60.0;
  stats.bitrate_kbps = 20000;
  stats.capture_transport = platf::frame_transport_e::dmabuf;
  stats.capture_residency = platf::frame_residency_e::gpu;
  stats.encode_target_residency = platf::frame_residency_e::gpu;
  stats.encode_time_ms = 2.0;
  stats.control_channel_packet_loss = 7.75;
  stats.control_channel_samples = 900;
  stats.network_sample_revision = 900;
  stats.network_last_received_age_ms = 0;
  stats.latency_ms = 6.0;

  // A client that sends no media reports: RTT is judged, loss is not, and nothing says it cleared.
  const auto unreported = judged_doctor(stats);
  EXPECT_EQ(unreported.at("primary_issue"), "control_channel_observation");
  EXPECT_EQ(unreported.at("summary"), "Control-channel retries were observed, but no video frame loss is measured.");
  EXPECT_EQ(unreported.at("recommendation").at("body"),
            "The reliable control channel retried packets, but no video frame loss is measured, and round trip time over "
            "the last 20 seconds stays below network pressure, so Doctor changes nothing for this.");

  // Its reports arriving and judged light, with Live Tuning owning the bitrate.
  stats.packet_loss = 1.4;
  stats.packet_loss_available = true;
  stats.media_loss_sample_revision = 890;
  stats.media_loss_last_received_age_ms = 400;
  stats.adaptive_bitrate_enabled = true;
  const auto reported = judged_doctor(stats);
  EXPECT_EQ(reported.at("primary_issue"), "control_channel_observation");
  EXPECT_EQ(reported.at("summary"), "Control-channel retries were observed, but video frame loss stays below network pressure.");
  EXPECT_EQ(reported.at("recommendation").at("body"),
            "The reliable control channel retried packets, but video frame loss and round trip time over the last 20 "
            "seconds stay below network pressure, so Doctor changes nothing for this. Live Tuning keeps adjusting the "
            "live bitrate on its own.");
  const auto *loss = find_evidence_row(reported, "packet_loss");
  ASSERT_NE(loss, nullptr);
  EXPECT_EQ(loss->at("label"), "Video frame loss");
  EXPECT_EQ(loss->at("status"), "pass");
  EXPECT_EQ(loss->at("source"), "media_transport");
  EXPECT_EQ(loss->at("detail"),
            "34 of 2400 video frames in the client's last 20 reports never reached it whole after FEC recovery, 1.40%. "
            "Doctor calls loss network pressure at 2.00% over the last 20 seconds and clears it only below 1.00%. "
            "Frames the host dropped before sending are counted separately.");
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

  const auto doctor = judged_doctor(
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

  const auto doctor = judged_doctor(
    stats,
    {{"primary_issue", "network_jitter"}, {"grade", "degraded"}}
  );
  const auto &action = doctor.at("safe_recovery_action");

  EXPECT_EQ(doctor.at("primary_issue"), "network_jitter");
  EXPECT_EQ(action.at("id"), "recheck_network");
  EXPECT_EQ(action.at("capability"), "recheck");
  EXPECT_FALSE(action.at("undo").at("supported"));
  // Called what the rest of the product calls it.
  EXPECT_NE(
    doctor.at("recommendation").at("body").get<std::string>().find("Live Tuning already owns the live bitrate correction"),
    std::string::npos
  );
  EXPECT_EQ(doctor.at("recommendation").dump().find("Auto Safe"), std::string::npos);
  EXPECT_EQ(action.dump().find("Auto Safe"), std::string::npos);
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

    const auto doctor = judged_doctor(
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

  const auto doctor = judged_doctor(
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

  const auto doctor = judged_doctor(
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

  const auto doctor = judged_doctor(stats, nlohmann::json::object());
  const auto &action = doctor.at("safe_recovery_action");

  EXPECT_EQ(doctor.at("primary_issue"), "none");
  EXPECT_EQ(doctor.at("traffic_light"), "green");
  EXPECT_EQ(doctor.at("status"), "ok");
  EXPECT_EQ(action.at("id"), "none");
  EXPECT_FALSE(action.at("undo").at("supported"));
  const auto &evidence = doctor.at("evidence");
  const auto ceiling = std::find_if(evidence.begin(), evidence.end(), [](const auto &item) {
    return item.at("id") == "effective_quality_ceiling";
  });
  ASSERT_NE(ceiling, evidence.end());
  EXPECT_EQ(ceiling->at("status"), "watch");
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
  mark_doctor_pacing_window_confirmed(stats);

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

  EXPECT_FALSE(doctor_actions::network_pressure_confirmed(judged(stats)));

  stats.packet_loss = 3.4;
  stats.packet_loss_available = true;
  stats.media_loss_sample_revision = 1;
  stats.media_loss_last_received_age_ms = 0;
  EXPECT_TRUE(doctor_actions::network_pressure_confirmed(judged(stats)));

  stats.packet_loss = 0.0;
  stats.packet_loss_available = false;
  stats.latency_ms = 45.0;
  EXPECT_TRUE(doctor_actions::network_pressure_confirmed(judged(stats)));

  stats.network_last_received_age_ms = 2001;
  EXPECT_FALSE(doctor_actions::network_pressure_confirmed(judged(stats)));
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
  sustain_network_pressure(35.0, 3.4);
  // Older than a client media report may be and still count, so its window's loss is not judged.
  stream_stats::age_latest_network_observation_for_tests(6s);
  stream_stats::update_control_channel_stats(35.0, 0.0, 1000);

  const auto stats = stream_stats::get_current();
  EXPECT_TRUE(stats.network_risk);
  EXPECT_GT(stats.media_loss_last_received_age_ms, stream_stats::judged_network_t::k_media_report_max_age_ms);
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

  sustain_network_pressure(55.0, 3.5);
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
  sustain_network_pressure(55.0, 3.5);
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
  sustain_network_pressure(52.0, 3.4);

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
  sustain_network_pressure(52.0, 3.4);

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

namespace {
  /// A stream Doctor may step down: Live Tuning off, one session in scope, under sustained pressure.
  struct doctor_step_stream_t {
    explicit doctor_step_stream_t(const char *client) {
      config::video.adaptive_bitrate.enabled = false;
      config::video.adaptive_bitrate.min_bitrate_kbps = 2000;
      config::video.adaptive_bitrate.max_bitrate_kbps = 100000;
      adaptive_bitrate::load_config();
      adaptive_bitrate::reset();
      adaptive_bitrate::set_runtime_update_supported(true);
      adaptive_bitrate::set_base_bitrate(20000);
      adaptive_bitrate::set_enabled(false);
      stream_stats::update_stream_active(false);
      stream_stats::update_controller_input_state(false, 0, "", "", "unknown", "", false, "");
      stream_stats::update_steam_input_state("unknown", 0, 0, 0, "");
      stream_stats::update_stream_active(true, client, "203.0.113.83");
      stream_stats::update_video_stats(60.0, 20000, 5.0, "hevc", 1920, 1080);
      stream_stats::set_doctor_live_action_scope_available(true);
      for (int i = 0; i < 6; ++i) {
        stream_stats::update_network_stats(5.0, 0.0, 1000);
      }
      sustain_network_pressure(52.0, 3.4);
    }

    ~doctor_step_stream_t() {
      stream_stats::set_doctor_live_action_scope_available(false);
      adaptive_bitrate::set_enabled(false);
      stream_stats::update_stream_active(false);
    }

    static nlohmann::json doctor() {
      auto live = stream_stats::get_current();
      live.capture_transport = platf::frame_transport_e::dmabuf;
      live.capture_residency = platf::frame_residency_e::gpu;
      live.encode_target_residency = platf::frame_residency_e::gpu;
      return stream_stats::build_doctor_json(live, nlohmann::json::object());
    }
  };
}  // namespace

TEST(DoctorActionTests, AVerifiedStepLeavesDoctorJudgingOnlyTheReadingsAfterIt) {
  // Doctor verified a step against readings from after it while its headline went on judging a
  // window that still held the readings that asked for the step. A step that verified left
  // "Sustained network pressure" on the headline, and lower_bitrate on offer again, for most of
  // 20 seconds, so a second press stepped down a link that had already recovered.
  doctor_step_stream_t stream("DoctorVerifiedStep");
  const auto before = doctor_step_stream_t::doctor();
  ASSERT_EQ(before.at("primary_issue"), "network_jitter");
  ASSERT_EQ(before.at("safe_recovery_action").at("id"), "lower_bitrate");

  const auto applied = doctor_actions::execute({{"action_id", "lower_bitrate"}});
  ASSERT_TRUE(applied.at("status").get<bool>());
  const auto run_id = applied.at("run_id").get<std::string>();
  doctor_actions::confirm_encoder_application_for_tests();

  // The stepped-down stream runs clean.
  for (int i = 0; i < 8; ++i) {
    stream_stats::update_network_stats(8.0, 0.5, 1000);
  }
  doctor_actions::make_verification_window_complete_for_tests();
  const auto verified = doctor_actions::execute({{"action_id", "verify"}, {"run_id", run_id}});
  ASSERT_EQ(verified.at("state"), "resolved") << verified.dump();

  const auto network = stream_stats::judged_network(stream_stats::get_current());
  EXPECT_TRUE(network.loss_judged);
  EXPECT_FALSE(network.risk);
  const auto after = doctor_step_stream_t::doctor();
  EXPECT_NE(after.at("primary_issue"), "network_jitter") << after.at("summary");
  EXPECT_NE(after.at("safe_recovery_action").at("id"), "lower_bitrate");
  const auto verdict = after.at("advanced_evidence").at("network_verdict");
  EXPECT_EQ(verdict.at("media_samples"), 8);
  EXPECT_EQ(verdict.at("loss_state"), "clean");

  const auto undo = execute_with_encoder_ack(20000, [&] {
    return doctor_actions::execute({{"action_id", "undo"}, {"run_id", run_id}});
  });
  EXPECT_EQ(undo.at("state"), "undone") << undo.dump();
}

TEST(DoctorActionTests, AStepThePostStepWindowStillCallsPressureRollsBack) {
  // The other half of the same disagreement: the newest readings could clear a step whose own
  // readings the headline still judged as pressure. Two clean reports after six that lost 4% of
  // their frames cleared the fast debounce, and Doctor called the step verified beside a headline
  // that still said "Sustained network pressure".
  doctor_step_stream_t stream("DoctorPressureAfterStep");
  const auto applied = doctor_actions::execute({{"action_id", "lower_bitrate"}});
  ASSERT_TRUE(applied.at("status").get<bool>());
  const auto run_id = applied.at("run_id").get<std::string>();
  doctor_actions::confirm_encoder_application_for_tests();
  const auto stepped_at = std::chrono::steady_clock::now();

  for (int i = 0; i < 6; ++i) {
    stream_stats::update_network_stats(8.0, 4.0, 1000);
  }
  for (int i = 0; i < 2; ++i) {
    stream_stats::update_network_stats(8.0, 0.0, 1000);
  }
  ASSERT_FALSE(stream_stats::get_current().network_risk);
  const auto network = stream_stats::judged_network(stream_stats::get_current());
  ASSERT_TRUE(network.loss_pressure);
  EXPECT_EQ(doctor_step_stream_t::doctor().at("primary_issue"), "network_jitter");
  // The step's own readings: six of eight reports lost 4%, 3% over the window since the step.
  const auto since_step = stream_stats::network_verdict_since(stepped_at);
  ASSERT_TRUE(since_step.loss_available);
  EXPECT_EQ(since_step.media_samples, 8);
  EXPECT_TRUE(since_step.loss_elevated);

  doctor_actions::make_verification_window_complete_for_tests();
  const auto result = execute_with_encoder_ack(20000, [&] {
    return doctor_actions::execute({{"action_id", "verify"}, {"run_id", run_id}});
  });
  EXPECT_EQ(result.at("state"), "rolled_back") << result.dump();
  EXPECT_EQ(adaptive_bitrate::get_doctor_state().live_bitrate_kbps, 20000);
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

  sustain_network_pressure(52.0, 3.4);
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

  sustain_network_pressure(52.0, 3.4);
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

  sustain_network_pressure(52.0, 3.4);
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
  sustain_network_pressure(52.0, 3.4);
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
  sustain_network_pressure(52.0, 3.4);

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

  const auto doctor = judged_doctor(
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
  LiveConfigurationGuard live_configuration;
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
  sustain_network_pressure(52.0, 3.4);

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
  LiveConfigurationGuard live_configuration;
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
  sustain_network_pressure(52.0, 3.4);

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
  sustain_network_pressure(52.0, 3.4);

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

TEST(DoctorActionTests, AutoFixStepsAndUndoesFromTheRateTheEncoderOpenedAt) {
  config::video.adaptive_bitrate.enabled = false;
  config::video.adaptive_bitrate.min_bitrate_kbps = 2000;
  config::video.adaptive_bitrate.max_bitrate_kbps = 100000;
  adaptive_bitrate::load_config();
  adaptive_bitrate::reset();
  stream_stats::update_stream_active(
    true, "DoctorAboveCeiling", "203.0.113.27"
  );
  // PyroWave at 1080p60 opens near 161 Mbps, above the 100 Mbps adaptive ceiling.
  stream_stats::update_video_stats(
    60.0, 161000, 5.0, "pyrowave", 1920, 1080
  );
  for (int i = 0; i < 6; ++i) {
    stream_stats::update_network_stats(5.0, 0.0, 1000);
  }
  sustain_network_pressure(52.0, 3.4);

  constexpr std::uint64_t generation = 427;
  doctor_actions::session_started(
    "client-owner", generation, "launch-427", 161000
  );
  adaptive_bitrate::set_runtime_update_supported(true, {}, 161000);
  stream_stats::start_session_timing(
    "client-owner", generation, "launch-427"
  );
  const auto cleanup = util::fail_guard([&] {
    doctor_actions::session_ended("client-owner", generation);
    stream_stats::stop_session_timing("client-owner", generation);
    stream_stats::update_stream_active(false);
  });
  EXPECT_EQ(adaptive_bitrate::get_doctor_state().live_bitrate_kbps, 161000);

  doctor_actions::recovery_action_context_t context;
  context.active_owner = true;
  context.host_tuning_allowed = true;
  context.enforce_request_scope = true;
  context.owner_uuid = "client-owner";
  context.app_uuid = "game-owner";
  context.launch_instance_id = "launch-427";
  context.session_generation = generation;
  context.stats = stream_stats::get_current();
  ASSERT_TRUE(context.stats.network_risk);
  const auto request = trusted_doctor_action_request(context);
  ASSERT_EQ(request.at("action_id"), "lower_bitrate");
  ASSERT_EQ(request.at("target_bitrate_kbps"), 128800);

  // One guarded step is 20% of what the encoder runs at, not a cut to the
  // ceiling followed by 20% of that.
  const auto applied = execute_with_encoder_ack(128800, [&] {
    return doctor_actions::execute(request, context);
  });
  ASSERT_TRUE(applied.at("status").get<bool>());
  EXPECT_EQ(applied.at("before").at("bitrate_kbps"), 161000);
  EXPECT_EQ(applied.at("requested").at("bitrate_kbps"), 128800);

  const auto run_id = applied.at("run_id").get<std::string>();
  const auto undone = execute_with_encoder_ack(161000, [&] {
    return doctor_actions::execute({
      {"action_id", "undo"},
      {"run_id", run_id},
      {"app_session_id", "launch-427"},
      {"session_generation", generation}
    }, context);
  });
  ASSERT_TRUE(undone.at("status").get<bool>());
  EXPECT_EQ(undone.at("state"), "undone");
  EXPECT_EQ(undone.at("restored_bitrate_kbps"), 161000);
}

TEST(DoctorActionTests, OwnerLiveBitrateAboveTheAdaptiveCeilingReachesTheEncoder) {
  LiveConfigurationGuard live_configuration;
  config::video.adaptive_bitrate.enabled = false;
  config::video.adaptive_bitrate.min_bitrate_kbps = 2000;
  config::video.adaptive_bitrate.max_bitrate_kbps = 100000;
  adaptive_bitrate::load_config();
  adaptive_bitrate::reset();

  constexpr std::uint64_t generation = 428;
  doctor_actions::session_started(
    "client-owner", generation, "launch-428", 20000
  );
  adaptive_bitrate::set_runtime_update_supported(true, {}, 20000);
  const auto cleanup = util::fail_guard([&] {
    doctor_actions::session_ended("client-owner", generation);
  });

  // Nova's Deck HUD asks for 180 Mbps in the middle of a stream.
  ASSERT_TRUE(doctor_actions::set_owner_live_bitrate(
    "client-owner", generation, "launch-428", 180000
  ));
  EXPECT_EQ(adaptive_bitrate::get_doctor_state().live_bitrate_kbps, 180000);
  const auto request = adaptive_bitrate::get_live_bitrate_request();
  ASSERT_TRUE(request.has_value());
  EXPECT_EQ(request->target_bitrate_kbps, 180000);
}

TEST(PolarisEventListenerTests, ReportsExceptionsWithoutASourceLocation) {
  PolarisEventListener listener;
  const testing::TestPartResult result(
    testing::TestPartResult::kFatalFailure, nullptr, -1, "test exception"
  );
  listener.OnTestProgramStart(*testing::UnitTest::GetInstance());
  const auto cleanup = util::fail_guard([&] {
    listener.OnTestProgramEnd(*testing::UnitTest::GetInstance());
  });
  EXPECT_NO_THROW(listener.OnTestPartResult(result));
  std::string output;
  {
    const auto backend = listener.sink->locked_backend();
    output = listener.sink_buffer->str();
  }
  EXPECT_NE(output.find("<unknown file>"), std::string::npos);
  EXPECT_NE(output.find("test exception"), std::string::npos);
}

namespace {
  // PyroWave's advice reads the host's FEC share and max_bitrate. Pin both for a test, and put them back.
  struct PyroWaveHostGuard {
    int fec_percentage = config::stream.fec_percentage;
    int max_bitrate = config::video.max_bitrate;

    PyroWaveHostGuard() {
      config::stream.fec_percentage = 10;
      config::video.max_bitrate = 0;
    }

    ~PyroWaveHostGuard() {
      config::stream.fec_percentage = fec_percentage;
      config::video.max_bitrate = max_bitrate;
    }
  };

  // A 1080p60 4:2:0 PyroWave stream on a clean, current network, with Live Tuning off. The model's far
  // figure for it, at the calibrated 31 dB, is 89121 kbps at the encoder and 100148 kbps as a request at
  // 10% FEC.
  stream_stats::stats_t clean_pyrowave_stats(int encoder_kbps) {
    stream_stats::stats_t stats {};
    stats.streaming = true;
    stats.codec = "pyrowave";
    stats.width = 1920;
    stats.height = 1080;
    stats.fps = 60.0;
    stats.encode_target_fps = 60.0;
    stats.stream_chroma = "420";
    stats.bitrate_kbps = encoder_kbps;
    stats.effective_launch_bitrate_kbps = encoder_kbps;
    stats.capture_transport = platf::frame_transport_e::dmabuf;
    stats.capture_residency = platf::frame_residency_e::gpu;
    stats.encode_target_residency = platf::frame_residency_e::gpu;
    stats.encode_time_ms = 1.0;
    stats.packet_loss = 0.0;
    stats.packet_loss_available = true;
    stats.network_sample_revision = 1;
    stats.network_last_received_age_ms = 0;
    stats.media_loss_sample_revision = 1;
    stats.media_loss_last_received_age_ms = 0;
    stats.latency_ms = 3.8;
    stats.network_risk = false;
    stats.adaptive_runtime_update_supported = true;
    stats.adaptive_target_bitrate_kbps = encoder_kbps;
    return stats;
  }

  const nlohmann::json &evidence_row(const nlohmann::json &doctor, std::string_view id) {
    for (const auto &row : doctor.at("evidence")) {
      if (row.at("id") == id) {
        return row;
      }
    }
    static const nlohmann::json missing = nlohmann::json::object();
    ADD_FAILURE() << "no evidence row " << id;
    return missing;
  }
}  // namespace

TEST(StreamStatsPyroWaveTests, CeilingShareIsARollingWindowOfRecentFrames) {
  stream_stats::update_stream_active(false);
  constexpr std::uint64_t generation = 431;
  stream_stats::add_client("203.0.113.40", "PyroWaveWindow", generation);
  const auto cleanup = util::fail_guard([&] {
    stream_stats::remove_client("203.0.113.40", generation);
    stream_stats::update_stream_active(false);
  });

  EXPECT_FALSE(stream_stats::record_pyrowave_frames(0, 30, 30));
  EXPECT_FALSE(stream_stats::record_pyrowave_frames(generation, 0, 0));
  EXPECT_TRUE(stream_stats::record_stream_request(generation, true, {180000, 0, {}, 15000, "stability_preset_selected", 512}));
  auto stats = stream_stats::get_current();
  EXPECT_EQ(stats.stream_chroma, "444");
  EXPECT_EQ(stats.bitrate_request.set_aside_source, "stability_preset_selected");

  // Unknown until a second of frames is in.
  ASSERT_TRUE(stream_stats::record_pyrowave_frames(generation, 30, 30));
  EXPECT_FALSE(stream_stats::pyrowave_ceiling_frame_share(stream_stats::get_current()).has_value());
  for (int i = 0; i < 9; ++i) {
    ASSERT_TRUE(stream_stats::record_pyrowave_frames(generation, 30, 30));
  }
  stats = stream_stats::get_current();
  ASSERT_TRUE(stream_stats::pyrowave_ceiling_frame_share(stats).has_value());
  EXPECT_DOUBLE_EQ(*stream_stats::pyrowave_ceiling_frame_share(stats), 1.0);
  EXPECT_LE(stats.pyrowave_window_frames, stream_stats::k_pyrowave_ceiling_window_frames + 30);

  // Eight clean batches replace the window: the old ceiling frames age out.
  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(stream_stats::record_pyrowave_frames(generation, 30, 0));
  }
  stats = stream_stats::get_current();
  EXPECT_EQ(stats.pyrowave_window_frames, stream_stats::k_pyrowave_ceiling_window_frames);
  EXPECT_DOUBLE_EQ(*stream_stats::pyrowave_ceiling_frame_share(stats), 0.0);
  // A count above the batch is held to the batch.
  ASSERT_TRUE(stream_stats::record_pyrowave_frames(generation, 30, 99));
  EXPECT_EQ(stream_stats::get_current().pyrowave_window_ceiling_frames, 30u);
}

TEST(StreamStatsPyroWaveTests, SessionStatusCarriesTheAdviceOnlyWhileTheStreamIsPyroWave) {
  PyroWaveHostGuard host;
  auto stats = clean_pyrowave_stats(20000);
  stats.bitrate_request.set_aside_kbps = 15000;
  stats.bitrate_request.set_aside_source = "stability_preset_selected";
  const auto pyrowave = stream_stats::pyrowave_bitrate_json(stats);
  ASSERT_TRUE(pyrowave.is_object());
  EXPECT_EQ(pyrowave.at("version"), 1);
  EXPECT_EQ(pyrowave.at("model"), "psnr-hvs-m");
  EXPECT_EQ(pyrowave.at("target_db"), 35);
  EXPECT_EQ(pyrowave.at("far_target_db"), 31);
  EXPECT_EQ(pyrowave.at("width"), 1920);
  EXPECT_EQ(pyrowave.at("height"), 1080);
  EXPECT_EQ(pyrowave.at("fps"), 60);
  EXPECT_EQ(pyrowave.at("chroma"), "420");
  EXPECT_EQ(pyrowave.at("advice_far_kbps"), 100148);
  EXPECT_EQ(pyrowave.at("advice_near_kbps"), 245904);
  EXPECT_EQ(pyrowave.at("raise_goal_kbps"), 100148);
  EXPECT_EQ(pyrowave.at("cap_kbps"), 300000);
  EXPECT_EQ(pyrowave.at("encoder_kbps"), 20000);
  // The same rate as a request, which is what starved compares with the raise goal.
  EXPECT_EQ(pyrowave.at("request_kbps"), 23347);
  EXPECT_EQ(pyrowave.at("request_kbps"), stream_bitrate::wire_kbps_for_encoder(20000, 10, 512));
  EXPECT_TRUE(pyrowave.at("ceiling_frame_share").is_null());
  EXPECT_TRUE(pyrowave.at("starved").get<bool>());
  EXPECT_TRUE(pyrowave.at("request_cap").is_null());
  EXPECT_EQ(pyrowave.at("cap_set_aside").at("kbps"), 15000);
  EXPECT_EQ(pyrowave.at("cap_set_aside").at("source"), "stability_preset_selected");
  EXPECT_EQ(pyrowave.at("assumes"), (nlohmann::json {{"fec_percentage", 10}, {"audio_kbps", 512}}));

  stats.pyrowave_window_frames = 240;
  stats.pyrowave_window_ceiling_frames = 223;
  EXPECT_DOUBLE_EQ(stream_stats::pyrowave_bitrate_json(stats).at("ceiling_frame_share").get<double>(), 0.929);

  // Within a tenth of the far figure is healthy, and below that is starved, both as requests: 90% of
  // 100148 is 90133.2, and a request of 90134 lands the encoder on 80108, 90133 on 80107.
  EXPECT_FALSE(stream_stats::pyrowave_bitrate_json(clean_pyrowave_stats(80108)).at("starved").get<bool>());
  EXPECT_TRUE(stream_stats::pyrowave_bitrate_json(clean_pyrowave_stats(80107)).at("starved").get<bool>());

  // At the raise goal with a quiet ceiling, the host has nothing to ask for.
  auto fed = clean_pyrowave_stats(160000);
  EXPECT_FALSE(stream_stats::pyrowave_bitrate_json(fed).at("starved").get<bool>());

  stats.codec = "hevc";
  EXPECT_TRUE(stream_stats::pyrowave_bitrate_json(stats).is_null());
  stats.codec = "pyrowave";
  stats.streaming = false;
  EXPECT_TRUE(stream_stats::pyrowave_bitrate_json(stats).is_null());
}

TEST(StreamStatsPyroWaveTests, SessionStatusNamesTheFecAndAudioItsAdviceWasGrossedUpFor) {
  // The console words the advice as including this FEC share. The stream's fec_protection cannot say
  // it: it records a percentage only once a frame outgrows FEC, so a healthy stream reads 0 there.
  PyroWaveHostGuard host;
  auto stats = clean_pyrowave_stats(20000);
  ASSERT_EQ(stats.fec_protection.fec_percentage, 0);
  // The stream's handshake split its request at 20% FEC with 5.1 audio, and a config reload has put
  // the host's share back to 10 since. The stream keeps the share it started with, as its
  // bitrate_units do, so the two objects never disagree.
  stats.bitrate_request.fec_percentage = 20;
  stats.bitrate_request.audio_kbps = 1536;
  stats.bitrate_request_recorded = true;
  config::stream.fec_percentage = 10;

  const auto pyrowave = stream_stats::pyrowave_bitrate_json(stats);
  EXPECT_EQ(pyrowave.at("assumes").at("fec_percentage"), 20);
  EXPECT_EQ(pyrowave.at("assumes").at("audio_kbps"), 1536);
  // The figures are the ones grossed up for those two: more than the 10% FEC and stereo request.
  EXPECT_GT(pyrowave.at("advice_far_kbps").get<int>(), 100148);

  // Before a handshake is recorded, the host's share now and stereo in high quality.
  stats.bitrate_request_recorded = false;
  const auto pending = stream_stats::pyrowave_bitrate_json(stats);
  EXPECT_EQ(pending.at("assumes").at("fec_percentage"), 10);
  EXPECT_EQ(pending.at("assumes").at("audio_kbps"), 512);
  EXPECT_EQ(pending.at("advice_far_kbps"), 100148);
}

namespace {
  // One stream's handshake as rtsp.cpp records it, for a request nothing warped or capped: the split of
  // the client's total, the audio and FEC it was split for, and where it left the encoder.
  stream_bitrate::request_t negotiated_request(std::int64_t total_kbps, int fec_percentage, int audio_kbps) {
    stream_bitrate::request_t request;
    request.client_kbps = total_kbps;
    const auto encoder = stream_bitrate::split_request(request, total_kbps, fec_percentage, audio_kbps);
    stream_bitrate::settle_encoder(request, static_cast<int>(encoder.value_or(0)), std::nullopt);
    return request;
  }

  // A status snapshot with recorded streams, the way get_current() copies them out.
  stream_stats::client_stats_t recorded_stream(std::uint64_t generation, const stream_bitrate::request_t &request) {
    stream_stats::client_stats_t client;
    client.session_generation = generation;
    client.bitrate_kbps = request.encoder_kbps;
    client.bitrate_request = request;
    client.bitrate_request_recorded = true;
    return client;
  }

  stream_stats::stats_t streaming_stats(std::vector<stream_stats::client_stats_t> clients) {
    stream_stats::stats_t stats {};
    stats.streaming = true;
    stats.clients = std::move(clients);
    return stats;
  }

  // The published figures satisfy the formula they name, both ways: the split lands the encoder on
  // encoder_kbps exactly, and the smallest request that reaches encoder_kbps is at most the split. The
  // split is the client's request times the warp factor, cut to the cap when there is one.
  void expect_round_trip(const nlohmann::json &units) {
    ASSERT_TRUE(units.is_object());
    ASSERT_EQ(units.at("formula"), "stream_bitrate_v1");
    ASSERT_TRUE(units.at("split_kbps").is_number_integer()) << units.dump();
    const auto requested = units.at("requested_kbps").get<std::int64_t>();
    const auto split = units.at("split_kbps").get<std::int64_t>();
    auto bounded = requested * units.at("warp_factor").get<std::int64_t>();
    if (!units.at("cap_kbps").is_null()) {
      bounded = std::min(bounded, units.at("cap_kbps").get<std::int64_t>());
    }
    EXPECT_EQ(split, bounded);
    const auto encoder = units.at("encoder_kbps").get<std::int64_t>();
    const auto fec = units.at("fec_percentage").get<int>();
    const auto audio = units.at("audio_kbps").get<int>();
    EXPECT_EQ(stream_bitrate::encoder_kbps_for_wire(split, fec, audio), encoder);
    const auto smallest = stream_bitrate::wire_kbps_for_encoder(encoder, fec, audio);
    EXPECT_LE(smallest, split);
    EXPECT_EQ(stream_bitrate::encoder_kbps_for_wire(smallest, fec, audio), encoder);
    EXPECT_LT(stream_bitrate::encoder_kbps_for_wire(smallest - 1, fec, audio), encoder);
  }
}  // namespace

TEST(StreamStatsBitrateUnitsTests, EveryCodecsSessionStatusCarriesItsBitrateUnits) {
  // pyrowave_bitrate is there only for PyroWave. bitrate_units is there for every stream.
  stream_stats::update_stream_active(false);
  const std::string ip = "203.0.113.47";
  constexpr std::uint64_t generation = 471;
  for (const std::string codec : {"h264", "hevc", "av1", "pyrowave"}) {
    SCOPED_TRACE(codec);
    stream_stats::add_client(ip, "BitrateUnits", generation);
    const auto cleanup = util::fail_guard([&] {
      stream_stats::remove_client(ip, generation);
      stream_stats::update_stream_active(false);
    });
    const auto request = negotiated_request(50000, 10, stream_bitrate::audio_kbps(true, 2));
    stream_stats::update_video_stats(ip, 0, request.encoder_kbps, 0, codec, 1920, 1080, {}, generation);
    // Nothing until the handshake is recorded: its zeros would read as figures.
    EXPECT_TRUE(stream_stats::bitrate_units_json(stream_stats::get_current(), generation).is_null());
    EXPECT_FALSE(stream_stats::recorded_stream_request(generation).has_value());
    ASSERT_TRUE(stream_stats::record_stream_request(generation, false, request));

    const auto stats = stream_stats::get_current();
    EXPECT_EQ(stats.codec, codec);
    const auto units = stream_stats::bitrate_units_json(stats, generation);
    EXPECT_EQ(units, (nlohmann::json {
      {"version", 1},
      {"requested_kbps", 50000},
      {"warp_factor", 1},
      {"cap_kbps", nullptr},
      {"cap_source", nullptr},
      {"split_kbps", 50000},
      {"encoder_kbps", 43987},
      {"live_encoder_kbps", 43987},
      {"audio_kbps", 512},
      {"fec_percentage", 10},
      {"formula", "stream_bitrate_v1"},
    }));
    expect_round_trip(units);
    // A client with no stream here is answered about this one.
    EXPECT_EQ(stream_stats::bitrate_units_json(stats, 0), units);
    ASSERT_TRUE(stream_stats::recorded_stream_request(generation).has_value());
    EXPECT_EQ(*stream_stats::recorded_stream_request(generation), request);
  }
  EXPECT_TRUE(stream_stats::bitrate_units_json(stream_stats::get_current(), generation).is_null());
  EXPECT_FALSE(stream_stats::recorded_stream_request(generation).has_value());
  EXPECT_FALSE(stream_stats::recorded_stream_request(0).has_value());
}

TEST(StreamStatsBitrateUnitsTests, TheFiguresRoundTripThroughStreamBitrate) {
  for (const std::int64_t total : {1000, 2500, 5000, 20000, 50000, 150000, 300000, 500000}) {
    for (const int fec : {0, 10, 20, 50, 80, 90}) {
      for (const int audio : {192, 512, 576, 1536, 2048}) {
        SCOPED_TRACE(std::to_string(total) + " kbps at " + std::to_string(fec) + "% FEC with " +
                     std::to_string(audio) + " kbps audio");
        const auto request = negotiated_request(total, fec, audio);
        const auto units = stream_stats::bitrate_units_json(streaming_stats({recorded_stream(7, request)}), 7);
        EXPECT_EQ(units.at("requested_kbps"), total);
        EXPECT_EQ(units.at("split_kbps"), total);
        EXPECT_EQ(units.at("fec_percentage"), fec);
        EXPECT_EQ(units.at("audio_kbps"), audio);
        EXPECT_EQ(units.at("encoder_kbps"), stream_bitrate::encoder_kbps_for_wire(total, fec, audio));
        expect_round_trip(units);
      }
    }
  }
}

TEST(StreamStatsBitrateUnitsTests, SurroundAudioComesOffTheRequestAndIsReportedAsItsOwn) {
  // 80 Mbps at 10% FEC. The audio is what the handshake counts for the stream's channels and quality.
  const auto units_for = [](bool high_quality, int channels) {
    const auto request = negotiated_request(80000, 10, stream_bitrate::audio_kbps(high_quality, channels));
    return stream_stats::bitrate_units_json(streaming_stats({recorded_stream(9, request)}), 9);
  };
  const auto stereo_low = units_for(false, 2);
  const auto stereo = units_for(true, 2);
  const auto surround51 = units_for(true, 6);
  const auto surround71 = units_for(true, 8);

  EXPECT_EQ(stereo_low.at("audio_kbps"), 192);
  EXPECT_EQ(stereo.at("audio_kbps"), 512);
  EXPECT_EQ(surround51.at("audio_kbps"), 1536);
  EXPECT_EQ(surround71.at("audio_kbps"), 2048);
  EXPECT_EQ(stereo_low.at("encoder_kbps"), 71308);
  EXPECT_EQ(stereo.at("encoder_kbps"), 70988);
  EXPECT_EQ(surround51.at("encoder_kbps"), 69964);
  EXPECT_EQ(surround71.at("encoder_kbps"), 69452);
  // The same request, and the difference in audio is exactly what the encoder gives up.
  EXPECT_EQ(stereo.at("encoder_kbps").get<int>() - surround51.at("encoder_kbps").get<int>(), 1536 - 512);
  for (const auto &units : {stereo_low, stereo, surround51, surround71}) {
    EXPECT_EQ(units.at("requested_kbps"), 80000);
    EXPECT_EQ(units.at("fec_percentage"), 10);
    expect_round_trip(units);
  }
  // So the same encoder rate asks more of a surround stream than of a stereo one.
  EXPECT_GT(stream_bitrate::wire_kbps_for_encoder(70988, 10, 1536), 80000);
}

TEST(StreamStatsBitrateUnitsTests, TheFecShareChangesTheSplitUntilTheHandshakeStopsTakingItOff) {
  const auto units_for = [](int fec) {
    const auto request = negotiated_request(80000, fec, stream_bitrate::audio_kbps(true, 2));
    return stream_stats::bitrate_units_json(streaming_stats({recorded_stream(11, request)}), 11);
  };
  const auto none = units_for(0);
  const auto ten = units_for(10);
  const auto twenty = units_for(20);
  const auto half = units_for(50);
  const auto most = units_for(80);
  const auto beyond = units_for(90);

  EXPECT_EQ(none.at("encoder_kbps"), 78988);
  EXPECT_EQ(ten.at("encoder_kbps"), 70988);
  EXPECT_EQ(twenty.at("encoder_kbps"), 62988);
  EXPECT_EQ(half.at("encoder_kbps"), 38988);
  EXPECT_EQ(most.at("encoder_kbps"), 14988);
  // Above 80% the handshake takes no FEC off, so the split is the one with none.
  EXPECT_EQ(beyond.at("encoder_kbps"), none.at("encoder_kbps"));
  for (const auto &[fec, units] : std::vector<std::pair<int, nlohmann::json>> {
         {0, none}, {10, ten}, {20, twenty}, {50, half}, {80, most}, {90, beyond}}) {
    SCOPED_TRACE(fec);
    EXPECT_EQ(units.at("fec_percentage"), fec);
    EXPECT_EQ(units.at("requested_kbps"), 80000);
    EXPECT_EQ(units.at("audio_kbps"), 512);
    expect_round_trip(units);
  }
}

TEST(StreamStatsBitrateUnitsTests, TheLiveEncoderRateFollowsTheEncodeLoopAndTheNegotiatedOneStays) {
  stream_stats::update_stream_active(false);
  const std::string ip = "203.0.113.48";
  constexpr std::uint64_t generation = 472;
  stream_stats::add_client(ip, "BitrateUnitsLive", generation);
  const auto cleanup = util::fail_guard([&] {
    stream_stats::remove_client(ip, generation);
    stream_stats::update_stream_active(false);
  });
  const auto request = negotiated_request(50000, 10, stream_bitrate::audio_kbps(true, 2));
  stream_stats::update_video_stats(ip, 0, request.encoder_kbps, 0, "hevc", 2560, 1440, {}, generation);
  ASSERT_TRUE(stream_stats::record_stream_request(generation, false, request));

  // The session's own encode loop reports the rate it applied after Live Tuning or a live bitrate cut it.
  stream_stats::update_video_stats(60.0, 30000, 2.0, "hevc", 2560, 1440, "nvenc", generation);
  auto units = stream_stats::bitrate_units_json(stream_stats::get_current(), generation);
  EXPECT_EQ(units.at("live_encoder_kbps"), 30000);
  EXPECT_EQ(units.at("encoder_kbps"), 43987);
  EXPECT_EQ(units.at("requested_kbps"), 50000);
  expect_round_trip(units);

  // And the rate it climbed back to.
  stream_stats::update_video_stats(60.0, 43987, 2.0, "hevc", 2560, 1440, "nvenc", generation);
  units = stream_stats::bitrate_units_json(stream_stats::get_current(), generation);
  EXPECT_EQ(units.at("live_encoder_kbps"), 43987);
}

TEST(StreamStatsBitrateUnitsTests, AStreamWithNoSplitOfItsOwnSaysSoWithANullSplit) {
  // A watcher: its own request was split, then the handshake put its encoder on its owner's rate. The
  // formula makes 43987 of its 50000, not 20000, so the split is null and a client knows not to use it.
  auto watcher = negotiated_request(50000, 10, stream_bitrate::audio_kbps(true, 2));
  EXPECT_EQ(stream_bitrate::settle_encoder(watcher, watcher.encoder_kbps, 20000), 20000);
  const auto units = stream_stats::bitrate_units_json(streaming_stats({recorded_stream(13, watcher)}), 13);
  EXPECT_EQ(units.at("requested_kbps"), 50000);
  EXPECT_TRUE(units.at("split_kbps").is_null()) << units.dump();
  EXPECT_EQ(units.at("encoder_kbps"), 20000);
  EXPECT_EQ(units.at("live_encoder_kbps"), 20000);
  EXPECT_EQ(units.at("audio_kbps"), 512);
  EXPECT_EQ(units.at("fec_percentage"), 10);
  EXPECT_NE(stream_bitrate::encoder_kbps_for_wire(50000, 10, 512), 20000);

  // A client that sent no bitrate at all: nothing to split, but its audio and FEC are still recorded.
  const auto unsplit = negotiated_request(0, 10, stream_bitrate::audio_kbps(true, 6));
  const auto none = stream_stats::bitrate_units_json(streaming_stats({recorded_stream(14, unsplit)}), 14);
  EXPECT_EQ(none.at("requested_kbps"), 0);
  EXPECT_TRUE(none.at("split_kbps").is_null()) << none.dump();
  EXPECT_EQ(none.at("encoder_kbps"), 0);
  EXPECT_EQ(none.at("audio_kbps"), 1536);
  EXPECT_EQ(none.at("fec_percentage"), 10);
}

TEST(StreamStatsBitrateUnitsTests, TheRequestIsTheClientsOwnAndTheSplitIsWhatTheHostLeftOfIt) {
  // A Stability preset caps an HEVC request of 150 Mbps at 40: the client asked for 150, and 40 was
  // split. The cap says so.
  stream_bitrate::request_t capped;
  capped.client_kbps = 150000;
  capped.cap_kbps = 40000;
  capped.cap_source = "stability_preset_selected";
  const auto capped_encoder = stream_bitrate::split_request(capped, 40000, 10, 512);
  ASSERT_TRUE(capped_encoder.has_value());
  stream_bitrate::settle_encoder(capped, static_cast<int>(*capped_encoder), std::nullopt);
  const auto capped_units = stream_stats::bitrate_units_json(streaming_stats({recorded_stream(21, capped)}), 21);
  EXPECT_EQ(capped_units.at("requested_kbps"), 150000);
  EXPECT_EQ(capped_units.at("warp_factor"), 1);
  EXPECT_EQ(capped_units.at("cap_kbps"), 40000);
  EXPECT_EQ(capped_units.at("cap_source"), "stability_preset_selected");
  EXPECT_EQ(capped_units.at("split_kbps"), 40000);
  EXPECT_EQ(capped_units.at("encoder_kbps"), 34988);
  expect_round_trip(capped_units);

  // limit_framerate warps a request of 20 Mbps to 40 with nothing to cap it: the client asked for 20.
  stream_bitrate::request_t warped;
  warped.client_kbps = 20000;
  warped.warp_factor = 2;
  const auto warped_encoder = stream_bitrate::split_request(warped, 40000, 10, 512);
  ASSERT_TRUE(warped_encoder.has_value());
  stream_bitrate::settle_encoder(warped, static_cast<int>(*warped_encoder), std::nullopt);
  const auto warped_units = stream_stats::bitrate_units_json(streaming_stats({recorded_stream(22, warped)}), 22);
  EXPECT_EQ(warped_units.at("requested_kbps"), 20000);
  EXPECT_EQ(warped_units.at("warp_factor"), 2);
  EXPECT_TRUE(warped_units.at("cap_kbps").is_null());
  EXPECT_TRUE(warped_units.at("cap_source").is_null());
  EXPECT_EQ(warped_units.at("split_kbps"), 40000);
  EXPECT_EQ(warped_units.at("encoder_kbps"), 34988);
  expect_round_trip(warped_units);
}

TEST(StreamStatsBitrateUnitsTests, EachClientIsAnsweredAboutItsOwnStream) {
  const auto stereo = negotiated_request(50000, 10, stream_bitrate::audio_kbps(true, 2));
  const auto surround = negotiated_request(80000, 10, stream_bitrate::audio_kbps(true, 6));
  stream_stats::client_stats_t starting;
  starting.session_generation = 3;
  const auto stats = streaming_stats({recorded_stream(1, stereo), recorded_stream(2, surround), starting});

  EXPECT_EQ(stream_stats::bitrate_units_json(stats, 2).at("requested_kbps"), 80000);
  EXPECT_EQ(stream_stats::bitrate_units_json(stats, 2).at("audio_kbps"), 1536);
  EXPECT_EQ(stream_stats::bitrate_units_json(stats, 1).at("requested_kbps"), 50000);
  // No stream here: the first one that has recorded its handshake.
  EXPECT_EQ(stream_stats::bitrate_units_json(stats, 0).at("requested_kbps"), 50000);
  // A client whose own stream is not in stats gets nothing rather than another stream's figures: its
  // stream was torn down a moment before its session timing stopped, or a reconnect replaced it.
  EXPECT_TRUE(stream_stats::bitrate_units_json(stats, 99).is_null());
  // A stream still in its handshake gets nothing rather than another stream's figures.
  EXPECT_TRUE(stream_stats::bitrate_units_json(stats, 3).is_null());

  auto idle = stats;
  idle.streaming = false;
  EXPECT_TRUE(stream_stats::bitrate_units_json(idle, 1).is_null());
  EXPECT_TRUE(stream_stats::bitrate_units_json(streaming_stats({starting}), 0).is_null());
}

TEST(StreamStatsDoctorTests, PyroWaveBelowItsAdviceOnACleanNetworkOffersARaiseToTheFarAdvice) {
  PyroWaveHostGuard host;
  const auto stats = clean_pyrowave_stats(20000);
  const auto doctor = judged_doctor(stats, nlohmann::json::object());
  const auto &action = doctor.at("safe_recovery_action");

  EXPECT_EQ(doctor.at("primary_issue"), "pyrowave_starved");
  EXPECT_EQ(doctor.at("traffic_light"), "amber");
  // One figure, in one unit: what the stream runs at and what to set, both as requests.
  const auto summary = doctor.at("summary").get<std::string>();
  EXPECT_NE(summary.find("PyroWave runs at a request of about 24 Mbps, where Polaris advises a request of about "
                         "101 Mbps for 1920x1080 at 60 fps on a device's own screen."),
            std::string::npos)
    << summary;
  EXPECT_EQ(summary.find("dB"), std::string::npos) << summary;
  EXPECT_EQ(summary.find("encoder"), std::string::npos) << summary;
  EXPECT_NE(doctor.at("recommendation").at("body").get<std::string>().find("a request of 101 Mbps"), std::string::npos);
  EXPECT_EQ(action.at("id"), "restore_quality");
  EXPECT_EQ(action.at("payload_preview").at("target_bitrate_kbps"), 100148);
  EXPECT_EQ(action.at("payload_preview").at("goal_source"), "pyrowave_advice");
  EXPECT_FALSE(action.at("requires_confirmation"));
  EXPECT_TRUE(action.at("undo").at("supported"));
  EXPECT_EQ(doctor.at("recommendation").at("next_step_label"), "Raise and verify");
  const auto &bitrate = evidence_row(doctor, "bitrate");
  EXPECT_EQ(bitrate.at("status"), "watch");
  EXPECT_NE(bitrate.at("detail").get<std::string>().find("H 2.0"), std::string::npos);

  // max_bitrate caps the raise, and the payload says so.
  config::video.max_bitrate = 50000;
  const auto capped = judged_doctor(stats, nlohmann::json::object());
  EXPECT_EQ(capped.at("safe_recovery_action").at("payload_preview").at("target_bitrate_kbps"), 50000);
  // The summary quotes the figure Doctor raises to, and says what held it there.
  EXPECT_NE(capped.at("summary").get<std::string>().find("a request of about 50 Mbps for 1920x1080 at 60 fps on a "
                                                         "device's own screen, the host's max_bitrate"),
            std::string::npos)
    << capped.at("summary");
}

TEST(StreamStatsDoctorTests, PyroWaveAdviceIsTextWhileLiveTuningOwnsTheBitrate) {
  PyroWaveHostGuard host;
  auto stats = clean_pyrowave_stats(20000);
  stats.adaptive_bitrate_enabled = true;
  const auto doctor = judged_doctor(stats, nlohmann::json::object());
  const auto &action = doctor.at("safe_recovery_action");

  EXPECT_EQ(doctor.at("primary_issue"), "pyrowave_starved");
  EXPECT_EQ(action.at("id"), "none");
  EXPECT_EQ(action.at("kind"), "manual_guidance");
  const auto reason = action.at("unavailable_reason").get<std::string>();
  EXPECT_NE(reason.find("Live Tuning"), std::string::npos) << reason;
  EXPECT_NE(reason.find("this stream only"), std::string::npos) << reason;
  EXPECT_EQ(doctor.at("recommendation").at("next_step_label"), "Raise the bitrate");

  // A live bitrate applies at the encoder, so the figure to set live is the goal at the encoder, 89121
  // kbps, and the text ties it to the 100148 kbps request Doctor quotes everywhere else. Setting 101
  // live would run the encoder at 101 Mbps, a request of about 114, a tenth past the advice.
  const auto body = doctor.at("recommendation").at("body").get<std::string>();
  for (const auto &text : {reason, body}) {
    SCOPED_TRACE(text);
    EXPECT_NE(text.find("set about 90 Mbps as the live bitrate in your client"), std::string::npos);
    EXPECT_NE(text.find("A live bitrate applies at the encoder, so that is the request of about 101 Mbps Polaris "
                        "advises without its FEC and audio."),
              std::string::npos);
    EXPECT_EQ(text.find("101 Mbps as the live bitrate"), std::string::npos);
  }
}

TEST(StreamStatsDoctorTests, PyroWaveFindingWaitsForACleanNetworkAndRanksBelowFailures) {
  PyroWaveHostGuard host;
  auto lossy = clean_pyrowave_stats(20000);
  lossy.network_risk = true;
  lossy.latency_ms = 60.0;
  EXPECT_EQ(judged_doctor(lossy, nlohmann::json::object()).at("primary_issue"), "network_jitter");

  auto slow_encoder = clean_pyrowave_stats(20000);
  slow_encoder.encode_time_ms = 30.0;
  EXPECT_EQ(judged_doctor(slow_encoder, nlohmann::json::object()).at("primary_issue"), "encoder_load");

  // Without a current network observation there is no clean network to raise on.
  auto unmeasured = clean_pyrowave_stats(20000);
  unmeasured.network_sample_revision = 0;
  unmeasured.network_last_received_age_ms = -1;
  unmeasured.media_loss_sample_revision = 0;
  unmeasured.media_loss_last_received_age_ms = -1;
  EXPECT_NE(judged_doctor(unmeasured, nlohmann::json::object()).at("primary_issue"), "pyrowave_starved");
}

TEST(StreamStatsDoctorTests, PyroWaveAtTheRetroidPocket6CalibrationIsHealthyWhateverItsCeilingShare) {
  PyroWaveHostGuard host;
  // 1920x1080 at 120 fps in 4:4:4 at the 200 Mbps request judged right on a Retroid Pocket 6, with every
  // recent frame at PyroWave's byte ceiling. 93% of the 215 Mbps far figure: healthy.
  auto rp6 = clean_pyrowave_stats(static_cast<int>(stream_bitrate::encoder_kbps_for_wire(200000, 10, 512)));
  rp6.fps = 120.0;
  rp6.encode_target_fps = 120.0;
  rp6.stream_chroma = "444";
  rp6.pyrowave_window_frames = 240;
  rp6.pyrowave_window_ceiling_frames = 240;
  const auto status = stream_stats::pyrowave_bitrate_json(rp6);
  EXPECT_EQ(status.at("advice_far_kbps"), 214898);
  EXPECT_EQ(status.at("raise_goal_kbps"), 214898);
  EXPECT_DOUBLE_EQ(status.at("ceiling_frame_share").get<double>(), 1.0);
  EXPECT_FALSE(status.at("starved").get<bool>());
  // Session status carries the rate starved was decided on: the encoder's 178987 kbps as a request.
  EXPECT_EQ(status.at("encoder_kbps"), 178987);
  EXPECT_EQ(status.at("request_kbps"), 199999);
  const auto healthy = judged_doctor(rp6, nlohmann::json::object());
  EXPECT_EQ(healthy.at("primary_issue"), "none");
  const auto &bitrate_row = evidence_row(healthy, "bitrate");
  EXPECT_EQ(bitrate_row.at("status"), "pass");
  // The row's value is the encoder's rate in kbps and its detail quotes requests, so it says which is
  // which. The television figure is the model's alone past the 300 Mbps Polaris recommends.
  EXPECT_EQ(bitrate_row.at("value"), 178987);
  const auto detail = bitrate_row.at("detail").get<std::string>();
  EXPECT_NE(detail.find("PyroWave runs at a request of about 200 Mbps"), std::string::npos) << detail;
  EXPECT_NE(detail.find("The kbps value is the rate at the encoder, and every Mbps figure here is a request."),
            std::string::npos)
    << detail;
  EXPECT_NE(detail.find("On a television or monitor (H 2.0) PyroWave's model asks for a request of 594 Mbps, in "
                        "4:4:4, more than the 300 Mbps Polaris recommends on its own."),
            std::string::npos)
    << detail;
  EXPECT_EQ(detail.find("it advises"), std::string::npos) << detail;
  // Within what Polaris recommends, the television figure needs no such note.
  const auto low = evidence_row(judged_doctor(clean_pyrowave_stats(160000), nlohmann::json::object()),
                                "bitrate")
                     .at("detail")
                     .get<std::string>();
  EXPECT_NE(low.find("PyroWave's model asks for a request of 246 Mbps, in 4:2:0. The kbps value"), std::string::npos)
    << low;

  // A stream at its far figure with most frames at the ceiling is left alone too: nothing holds it
  // below what the model asks, so the share is only reported.
  auto fed = clean_pyrowave_stats(160000);
  fed.pyrowave_window_frames = 240;
  fed.pyrowave_window_ceiling_frames = 236;
  EXPECT_EQ(judged_doctor(fed, nlohmann::json::object()).at("primary_issue"), "none");

  // More than a tenth below it, 150 Mbps, is starved, and Doctor raises to the far figure.
  auto short_rp6 = rp6;
  const auto short_encoder = static_cast<int>(stream_bitrate::encoder_kbps_for_wire(150000, 10, 512));
  short_rp6.bitrate_kbps = short_encoder;
  short_rp6.effective_launch_bitrate_kbps = short_encoder;
  short_rp6.adaptive_target_bitrate_kbps = short_encoder;
  const auto starved = judged_doctor(short_rp6, nlohmann::json::object());
  EXPECT_EQ(starved.at("primary_issue"), "pyrowave_starved");
  EXPECT_EQ(starved.at("safe_recovery_action").at("payload_preview").at("target_bitrate_kbps"), 214898);
  EXPECT_NE(starved.at("summary").get<std::string>().find("a request of about 150 Mbps"), std::string::npos)
    << starved.at("summary");
  EXPECT_NE(starved.at("summary").get<std::string>().find("100% of recent frames hit its byte ceiling"), std::string::npos)
    << starved.at("summary");
}

namespace {
  // A PyroWave stream of this shape at a request, at 10% FEC with stereo, with ceiling_frames of its last
  // 240 frames at the byte budget, on the clean network clean_pyrowave_stats() describes.
  stream_stats::stats_t clean_pyrowave_stream(int width, int height, int fps, bool chroma444, int request_kbps,
                                              std::uint32_t ceiling_frames) {
    auto stats = clean_pyrowave_stats(static_cast<int>(stream_bitrate::encoder_kbps_for_wire(request_kbps, 10, 512)));
    stats.width = width;
    stats.height = height;
    stats.fps = fps;
    stats.encode_target_fps = fps;
    stats.stream_chroma = chroma444 ? "444" : "420";
    stats.pyrowave_window_frames = 240;
    stats.pyrowave_window_ceiling_frames = ceiling_frames;
    return stats;
  }

  // What Doctor tells a stream that wants more than Doctor raises it to, whoever owns the bitrate, and
  // what it adds where only the cap holds the stream back and a player can set more by hand.
  const std::string k_lower_mode_or_hevc = "Lower the resolution or frame rate, or use HEVC, for a sharper picture.";
  const std::string k_lower_mode_or_hevc_or_by_hand = k_lower_mode_or_hevc + " You can also set up to 500 Mbps by hand.";

  // A PyroWave stream that wants more than Doctor raises it to, with 85% of its frames at the byte budget.
  nlohmann::json limit_doctor(int width, int height, int request_kbps) {
    const auto doctor = judged_doctor(clean_pyrowave_stream(width, height, 120, true, request_kbps, 204),
                                                        nlohmann::json::object());
    EXPECT_EQ(doctor.at("primary_issue"), "pyrowave_needs_more_than_allowed");
    return doctor;
  }
}  // namespace

TEST(StreamStatsDoctorTests, PyroWaveHeldBelowItsModelByTheCapAtItsByteCeilingAsksForALowerModeOrHevc) {
  PyroWaveHostGuard host;
  // A Retroid Pocket 6 at 3840x2160, 120 fps and 4:4:4. PyroWave's model asks a request of 327675 kbps
  // on a device's own screen, and Doctor raises no further than the 300000 cap. At the cap the stream
  // is not starved, yet 204 of its last 240 frames, 85%, fill the byte budget.
  const auto stats = clean_pyrowave_stream(3840, 2160, 120, true, 300000, 204);
  const auto status = stream_stats::pyrowave_bitrate_json(stats);
  ASSERT_EQ(status.at("advice_far_kbps"), 327675);
  ASSERT_EQ(status.at("raise_goal_kbps"), 300000);
  ASSERT_EQ(status.at("raise_goal_limited_by"), "cap");
  EXPECT_FALSE(status.at("starved").get<bool>());
  EXPECT_DOUBLE_EQ(status.at("ceiling_frame_share").get<double>(), 0.85);

  const auto doctor = judged_doctor(stats, nlohmann::json::object());
  EXPECT_EQ(doctor.at("primary_issue"), "pyrowave_needs_more_than_allowed");
  EXPECT_EQ(doctor.at("traffic_light"), "amber");
  // A player can set up to 500 Mbps by hand, so the cap is only how far Doctor raises, not what the
  // host allows.
  EXPECT_EQ(doctor.at("summary").get<std::string>(),
            "PyroWave at 3840x2160 and 120 fps wants a request of about 328 Mbps on this screen, above the 300 Mbps "
            "Doctor raises it to, and 85% of recent frames fill its byte budget.");
  const auto &recommendation = doctor.at("recommendation");
  EXPECT_EQ(recommendation.at("body").get<std::string>(),
            "Lower the resolution or frame rate, or use HEVC, for a sharper picture. You can also set up to 500 Mbps "
            "by hand.");
  EXPECT_EQ(recommendation.at("next_step_label"), "Use a lower mode or HEVC");
  EXPECT_EQ(recommendation.at("expected_effect"),
            "A smaller or slower picture needs fewer bits, and HEVC needs fewer for the same picture.");
  EXPECT_EQ(recommendation.at("why"), doctor.at("summary"));
  const auto &action = doctor.at("safe_recovery_action");
  EXPECT_EQ(action.at("id"), "none");
  EXPECT_EQ(action.at("kind"), "manual_guidance");
  EXPECT_EQ(action.at("unavailable_reason").get<std::string>(), recommendation.at("body").get<std::string>());
  // The evidence behind the finding reads as a watch, not a pass.
  EXPECT_EQ(evidence_row(doctor, "bitrate").at("status"), "watch");
}

TEST(StreamStatsDoctorTests, PyroWaveAtItsByteCeilingIsAFindingOnlyWhereTheHostHoldsItBelowTheModel) {
  PyroWaveHostGuard host;
  const auto issue = [](int width, int height, int fps, int request_kbps, std::uint32_t ceiling_frames) {
    return judged_doctor(
             clean_pyrowave_stream(width, height, fps, true, request_kbps, ceiling_frames), nlohmann::json::object())
      .at("primary_issue")
      .get<std::string>();
  };
  // 1920x1080 at 120 fps in 4:4:4 at 200 Mbps, the Retroid Pocket 6 check. Its goal is the model's own
  // 215, which nothing holds back, so the share of frames at the byte budget is only reported.
  EXPECT_EQ(issue(1920, 1080, 120, 200000, 48), "none");
  EXPECT_EQ(issue(1920, 1080, 120, 200000, 204), "none");
  EXPECT_EQ(issue(1920, 1080, 120, 200000, 240), "none");

  // With max_bitrate at 200 Mbps the host holds the same stream below the model's 215. A low share is
  // still no finding, and neither is exactly 80%; more than 80% is.
  config::video.max_bitrate = 200000;
  EXPECT_EQ(issue(1920, 1080, 120, 200000, 48), "none");
  EXPECT_EQ(issue(1920, 1080, 120, 200000, 192), "none");
  const auto held = judged_doctor(clean_pyrowave_stream(1920, 1080, 120, true, 200000, 204),
                                                    nlohmann::json::object());
  EXPECT_EQ(held.at("primary_issue"), "pyrowave_needs_more_than_allowed");
  // The host takes no more than max_bitrate by hand either, so Doctor names it and offers nothing to set.
  EXPECT_EQ(held.at("summary").get<std::string>(),
            "PyroWave at 1920x1080 and 120 fps wants a request of about 215 Mbps on this screen, above the 200 Mbps "
            "this host's max_bitrate allows, and 85% of recent frames fill its byte budget.");
  EXPECT_EQ(held.at("recommendation").at("body").get<std::string>(),
            "Lower the resolution or frame rate, or use HEVC, for a sharper picture.");
  EXPECT_EQ(held.at("safe_recovery_action").at("unavailable_reason").get<std::string>(), k_lower_mode_or_hevc);
  config::video.max_bitrate = 0;

  // A stream set by hand above the model's figure has what the model asks, whatever its share.
  EXPECT_EQ(issue(3840, 2160, 120, 350000, 240), "none");
  // More than a tenth below the cap it is starved, and Doctor raises it to the cap instead.
  const auto starved = judged_doctor(clean_pyrowave_stream(3840, 2160, 120, true, 250000, 204),
                                                       nlohmann::json::object());
  EXPECT_EQ(starved.at("primary_issue"), "pyrowave_starved");
  EXPECT_EQ(starved.at("safe_recovery_action").at("payload_preview").at("target_bitrate_kbps"), 300000);
  // Network pressure comes first.
  auto lossy = clean_pyrowave_stream(3840, 2160, 120, true, 300000, 204);
  lossy.network_risk = true;
  lossy.latency_ms = 60.0;
  EXPECT_EQ(judged_doctor(lossy, nlohmann::json::object()).at("primary_issue"), "network_jitter");
}

TEST(StreamStatsDoctorTests, PyroWaveLimitFindingNamesWhatHoldsTheStreamAndWhatAPlayerCanSet) {
  PyroWaveHostGuard host;
  // With max_bitrate above the cap, the cap is still only how far Doctor raises, and a player can set
  // up to max_bitrate by hand.
  config::video.max_bitrate = 400000;
  const auto above_cap = limit_doctor(3840, 2160, 300000);
  EXPECT_EQ(above_cap.at("summary").get<std::string>(),
            "PyroWave at 3840x2160 and 120 fps wants a request of about 328 Mbps on this screen, above the 300 Mbps "
            "Doctor raises it to, and 85% of recent frames fill its byte budget.");
  EXPECT_EQ(above_cap.at("recommendation").at("body").get<std::string>(),
            k_lower_mode_or_hevc + " You can also set up to 400 Mbps by hand.");

  // A stream set by hand above the cap and still below the model's figure hears the same.
  config::video.max_bitrate = 0;
  const auto by_hand = limit_doctor(3840, 2160, 320000);
  EXPECT_EQ(by_hand.at("summary").get<std::string>(),
            "PyroWave at 3840x2160 and 120 fps wants a request of about 328 Mbps on this screen, above the 300 Mbps "
            "Doctor raises it to, and 85% of recent frames fill its byte budget.");
  EXPECT_EQ(by_hand.at("recommendation").at("body").get<std::string>(), k_lower_mode_or_hevc_or_by_hand);

  // With max_bitrate at the cap, the cap still sets the goal, but the host takes no more by hand, so
  // Doctor names max_bitrate and offers nothing to set.
  config::video.max_bitrate = 300000;
  const auto at_cap = limit_doctor(3840, 2160, 300000);
  EXPECT_EQ(stream_stats::pyrowave_bitrate_json(clean_pyrowave_stream(3840, 2160, 120, true, 300000, 204))
              .at("raise_goal_limited_by"),
            "cap");
  EXPECT_EQ(at_cap.at("summary").get<std::string>(),
            "PyroWave at 3840x2160 and 120 fps wants a request of about 328 Mbps on this screen, above the 300 Mbps "
            "this host's max_bitrate allows, and 85% of recent frames fill its byte budget.");
  EXPECT_EQ(at_cap.at("recommendation").at("body").get<std::string>(), k_lower_mode_or_hevc);
}

TEST(StreamStatsDoctorTests, PyroWaveThatWantsMoreThanDoctorRaisesToGetsNoBitrateAction) {
  PyroWaveHostGuard host;
  struct variant_t {
    const char *name;
    bool live_tuning;
    bool runtime_updates;
    bool single_scope;
  };
  for (const auto &variant : {variant_t {"Live Tuning off", false, true, true}, variant_t {"Live Tuning on", true, true, true},
                              variant_t {"no live bitrate", false, false, true}, variant_t {"shared encoder", false, true, false}}) {
    SCOPED_TRACE(variant.name);
    auto stats = clean_pyrowave_stream(3840, 2160, 120, true, 300000, 204);
    stats.adaptive_bitrate_enabled = variant.live_tuning;
    stats.adaptive_runtime_update_supported = variant.runtime_updates;
    stats.doctor_live_action_scope_available = variant.single_scope;
    const auto doctor = judged_doctor(stats, nlohmann::json::object());
    ASSERT_EQ(doctor.at("primary_issue"), "pyrowave_needs_more_than_allowed");
    const auto &action = doctor.at("safe_recovery_action");
    EXPECT_EQ(action.at("id"), "none");
    EXPECT_EQ(action.at("kind"), "manual_guidance");
    EXPECT_EQ(action.at("capability"), "manual");
    EXPECT_EQ(action.at("endpoint"), "");
    EXPECT_TRUE(action.at("payload_preview").empty()) << action.at("payload_preview");
    EXPECT_FALSE(action.at("undo").at("supported").get<bool>());
    const auto &recommendation = doctor.at("recommendation");
    EXPECT_EQ(recommendation.at("next_step_label"), "Use a lower mode or HEVC");
    EXPECT_EQ(recommendation.at("body").get<std::string>(), k_lower_mode_or_hevc_or_by_hand);
    for (const auto &text : {recommendation.at("body").get<std::string>(),
                             recommendation.at("next_step_label").get<std::string>(),
                             recommendation.at("expected_effect").get<std::string>(),
                             action.at("unavailable_reason").get<std::string>()}) {
      SCOPED_TRACE(text);
      EXPECT_NE(text.find("HEVC"), std::string::npos);
      for (const char *offer : {"aise", "live bitrate", "et a request", "et about", "ower the bitrate", "rim bitrate", "Auto Fix"}) {
        EXPECT_EQ(text.find(offer), std::string::npos) << offer;
      }
    }
  }
}

TEST(StreamStatsDoctorTests, PyroWaveHeldBelowItsModelButCutBelowItsLaunchBitrateGetsTheQualityRestore) {
  PyroWaveHostGuard host;
  // 3840x2160 at 120 fps in 4:4:4, launched at a request of 290 Mbps and cut live to 280 on a network
  // that is clean again. At 280 the stream is within a tenth of the 300 Mbps cap, below the model's
  // 328, and 85% of its frames fill the byte budget, yet Doctor can restore the 290 the player chose,
  // so it offers that restore rather than hide it behind the limit finding.
  const int launch_kbps = static_cast<int>(stream_bitrate::encoder_kbps_for_wire(290000, 10, 512));
  auto cut = clean_pyrowave_stream(3840, 2160, 120, true, 280000, 204);
  cut.effective_launch_bitrate_kbps = launch_kbps;
  const auto status = stream_stats::pyrowave_bitrate_json(cut);
  ASSERT_EQ(status.at("raise_goal_kbps"), 300000);
  ASSERT_FALSE(status.at("starved").get<bool>());
  ASSERT_LT(launch_kbps, static_cast<int>(stream_bitrate::encoder_kbps_for_wire(300000, 10, 512)));
  const auto doctor = judged_doctor(cut, nlohmann::json::object());
  EXPECT_EQ(doctor.at("primary_issue"), "quality_reduced_live");
  EXPECT_EQ(doctor.at("recommendation").at("next_step_label"), "Restore and verify");
  const auto &payload = doctor.at("safe_recovery_action").at("payload_preview");
  EXPECT_EQ(payload.at("action_id"), "restore_quality");
  EXPECT_EQ(payload.at("target_bitrate_kbps"), launch_kbps);
  EXPECT_EQ(payload.at("goal_source"), "launch_bitrate");

  // Back at the bitrate it launched at there is nothing to restore, and the limit finding stands.
  auto restored = clean_pyrowave_stream(3840, 2160, 120, true, 290000, 204);
  ASSERT_EQ(restored.effective_launch_bitrate_kbps, launch_kbps);
  EXPECT_EQ(judged_doctor(restored, nlohmann::json::object()).at("primary_issue"),
            "pyrowave_needs_more_than_allowed");

  // With Live Tuning on, its own recovery owns the climb and Doctor offers no restore, so the limit
  // finding stands there too.
  cut.adaptive_bitrate_enabled = true;
  EXPECT_EQ(judged_doctor(cut, nlohmann::json::object()).at("primary_issue"),
            "pyrowave_needs_more_than_allowed");
}

TEST(StreamStatsDoctorTests, PyroWaveAtItsFloorUnderPressureSuggestsHevcInsteadOfCutting) {
  PyroWaveHostGuard host;
  for (const bool live_tuning : {false, true}) {
    SCOPED_TRACE(live_tuning ? "Live Tuning on" : "Live Tuning off");
    // The calibrated floor for 1080p60 in 4:2:0: half the far figure, 44560 kbps at the encoder, which
    // is a request of 50636.
    auto stats = clean_pyrowave_stats(44560);
    stats.network_risk = true;
    stats.latency_ms = 60.0;
    stats.adaptive_bitrate_enabled = live_tuning;
    stats.adaptive_min_bitrate_kbps = 44560;
    stats.adaptive_floor_source = "pyrowave_advice";
    const auto doctor = judged_doctor(stats, nlohmann::json::object());
    const auto &action = doctor.at("safe_recovery_action");
    EXPECT_EQ(doctor.at("primary_issue"), "network_jitter");
    EXPECT_EQ(action.at("id"), "none");
    EXPECT_EQ(action.at("kind"), "manual_guidance");
    EXPECT_NE(action.at("unavailable_reason").get<std::string>().find("HEVC"), std::string::npos);
    EXPECT_NE(action.at("unavailable_reason").get<std::string>().find("its floor, a request of about 51 Mbps"),
              std::string::npos)
      << action.at("unavailable_reason");
    EXPECT_NE(evidence_row(doctor, "bitrate").at("detail").get<std::string>().find("no lower than a request of 51 Mbps"),
              std::string::npos);
    EXPECT_EQ(doctor.at("recommendation").at("next_step_label"), "Use HEVC or a lower mode");
  }

  // Above the floor, network pressure still gets Doctor's one guarded step.
  auto above = clean_pyrowave_stats(120000);
  above.network_risk = true;
  above.latency_ms = 60.0;
  above.adaptive_min_bitrate_kbps = 44560;
  above.adaptive_floor_source = "pyrowave_advice";
  EXPECT_EQ(judged_doctor(above, nlohmann::json::object()).at("safe_recovery_action").at("id"),
            "lower_bitrate");
}

TEST(StreamStatsDoctorTests, PyroWaveCutBelowARequestThatMeetsTheAdviceClimbsBackToTheRequest) {
  PyroWaveHostGuard host;
  // The player asked for 180 Mbps, which lands the encoder on 160988 kbps, above the 89121 the far
  // advice lands it on. Something cut the stream to 128790, and the network is clean again.
  auto stats = clean_pyrowave_stats(128790);
  stats.effective_launch_bitrate_kbps = 160988;
  const auto doctor = judged_doctor(stats, nlohmann::json::object());
  EXPECT_EQ(doctor.at("primary_issue"), "quality_reduced_live");
  const auto &payload = doctor.at("safe_recovery_action").at("payload_preview");
  EXPECT_EQ(payload.at("action_id"), "restore_quality");
  EXPECT_EQ(payload.at("target_bitrate_kbps"), 160988);
  EXPECT_EQ(payload.at("goal_source"), "launch_bitrate");

  // A saved paired profile sized for H.264 does not cap the climb back: the handshake set it aside.
  stats.paired_target_bitrate_kbps = 20000;
  const auto paired = judged_doctor(stats, nlohmann::json::object());
  EXPECT_EQ(paired.at("primary_issue"), "quality_reduced_live");
  EXPECT_EQ(paired.at("safe_recovery_action").at("payload_preview").at("target_bitrate_kbps"), 160988);
  EXPECT_EQ(paired.at("safe_recovery_action").at("payload_preview").at("goal_source"), "launch_bitrate");
  EXPECT_EQ(stream_stats::doctor_launch_quality_goal_kbps(stats), 160988);

  // With Live Tuning on, its own recovery climbs back to the request, and Doctor does not ask the
  // player to set less than they already asked for.
  stats.adaptive_bitrate_enabled = true;
  EXPECT_EQ(judged_doctor(stats, nlohmann::json::object()).at("primary_issue"), "none");

  // A request more than a tenth below the advice still gets PyroWave's raise, which goes past the
  // request.
  auto short_request = clean_pyrowave_stats(60000);
  short_request.effective_launch_bitrate_kbps = 70000;
  const auto raise = judged_doctor(short_request, nlohmann::json::object());
  EXPECT_EQ(raise.at("primary_issue"), "pyrowave_starved");
  EXPECT_EQ(raise.at("safe_recovery_action").at("payload_preview").at("goal_source"), "pyrowave_advice");
  EXPECT_EQ(raise.at("safe_recovery_action").at("payload_preview").at("target_bitrate_kbps"), 100148);
}

TEST(StreamStatsDoctorTests, PyroWaveEncoderLoadAsksForASmallerPictureNotALowerBitrate) {
  PyroWaveHostGuard host;
  auto stats = clean_pyrowave_stats(160000);
  stats.encode_time_ms = 30.0;
  const auto doctor = stream_stats::build_doctor_json(stats, nlohmann::json::object());
  ASSERT_EQ(doctor.at("primary_issue"), "encoder_load");
  const auto body = doctor.at("recommendation").at("body").get<std::string>();
  EXPECT_NE(body.find("resolution or FPS"), std::string::npos) << body;
  EXPECT_EQ(body.find("Trim bitrate"), std::string::npos) << body;

  stats.codec = "hevc";
  const auto hevc = stream_stats::build_doctor_json(stats, nlohmann::json::object());
  ASSERT_EQ(hevc.at("primary_issue"), "encoder_load");
  EXPECT_NE(hevc.at("recommendation").at("body").get<std::string>().find("Trim bitrate"), std::string::npos);
}

TEST(StreamStatsDoctorTests, CleanReductionWithoutAPairedProfileRestoresTheLaunchBitrate) {
  stream_stats::stats_t stats {};
  stats.streaming = true;
  stats.fps = 60.0;
  stats.encode_target_fps = 60.0;
  stats.bitrate_kbps = 15000;
  stats.adaptive_target_bitrate_kbps = 7580;
  stats.paired_target_bitrate_kbps = 0;
  stats.effective_launch_bitrate_kbps = 15000;
  stats.capture_transport = platf::frame_transport_e::dmabuf;
  stats.capture_residency = platf::frame_residency_e::gpu;
  stats.encode_target_residency = platf::frame_residency_e::gpu;
  stats.encode_time_ms = 4.0;
  stats.packet_loss_available = true;
  stats.network_sample_revision = 1;
  stats.network_last_received_age_ms = 0;
  stats.media_loss_sample_revision = 1;
  stats.media_loss_last_received_age_ms = 0;
  stats.latency_ms = 3.8;
  stats.adaptive_runtime_update_supported = true;

  const auto doctor = judged_doctor(stats, nlohmann::json::object());
  const auto &action = doctor.at("safe_recovery_action");
  EXPECT_EQ(doctor.at("primary_issue"), "quality_reduced_live");
  EXPECT_EQ(action.at("id"), "restore_quality");
  EXPECT_EQ(action.at("payload_preview").at("target_bitrate_kbps"), 15000);
  EXPECT_EQ(action.at("payload_preview").at("goal_source"), "launch_bitrate");

  stats.paired_target_bitrate_kbps = 12000;
  const auto paired = judged_doctor(stats, nlohmann::json::object());
  EXPECT_EQ(paired.at("safe_recovery_action").at("payload_preview").at("target_bitrate_kbps"), 12000);
  EXPECT_EQ(paired.at("safe_recovery_action").at("payload_preview").at("goal_source"), "launch_ceiling");
}

TEST(DoctorActionTests, QualityRestoreWithoutAPairedProfileClimbsToTheLaunchBitrate) {
  stream_stats::update_stream_active(false);
  config::video.adaptive_bitrate.enabled = false;
  config::video.adaptive_bitrate.min_bitrate_kbps = 2000;
  config::video.adaptive_bitrate.max_bitrate_kbps = 100000;
  adaptive_bitrate::load_config();
  adaptive_bitrate::reset();
  adaptive_bitrate::set_runtime_update_supported(true);
  adaptive_bitrate::set_live_bitrate(7580);
  adaptive_bitrate::set_base_bitrate(15000);

  stream_stats::update_stream_active(true, "DoctorRestoreUnpaired", "203.0.113.42");
  stream_stats::update_video_stats(60.0, 7580, 5.0, "hevc", 1920, 1080);
  // No saved paired profile: the paired target is 0.
  stream_stats::update_session_targets(
    60.0, 60.0, 60.0, "client_requested", "deterministic_preset_v1",
    "deterministic", "not_applicable", "Capability-validated launch profile.",
    "", 1, 0, 15000
  );
  for (int i = 0; i < 6; ++i) {
    stream_stats::update_network_stats(5.0, 0.0, 1000);
  }

  const auto applied = doctor_actions::execute({{"action_id", "restore_quality"}});
  ASSERT_TRUE(applied.at("status").get<bool>()) << applied.dump();
  EXPECT_EQ(applied.at("requested").at("bitrate_kbps"), 9475);
  EXPECT_EQ(applied.at("requested").at("target_bitrate_kbps"), 15000);
  EXPECT_EQ(applied.at("requested").at("goal_source"), "launch_bitrate");

  const auto run_id = applied.at("run_id").get<std::string>();
  const auto apply_request = adaptive_bitrate::get_live_bitrate_request();
  ASSERT_TRUE(apply_request.has_value());
  adaptive_bitrate::acknowledge_live_bitrate_applied(apply_request->revision, apply_request->target_bitrate_kbps);
  const auto undone = execute_with_encoder_ack(7580, [&] {
    return doctor_actions::execute({{"action_id", "undo"}, {"run_id", run_id}});
  });
  EXPECT_TRUE(undone.at("status").get<bool>());
  EXPECT_EQ(adaptive_bitrate::get_doctor_state().live_bitrate_kbps, 7580);

  adaptive_bitrate::set_enabled(false);
  stream_stats::update_stream_active(false);
}

TEST(DoctorActionTests, PyroWaveRaiseClimbsPastTheRequestToTheAdviceAndLiveTuningNeverFollows) {
  PyroWaveHostGuard host;
  stream_stats::update_stream_active(false);
  config::video.adaptive_bitrate.enabled = false;
  config::video.adaptive_bitrate.min_bitrate_kbps = 2000;
  config::video.adaptive_bitrate.max_bitrate_kbps = 100000;
  // One unshared stream owns the controller, as Doctor requires before it offers a live change.
  constexpr std::uint64_t generation = 433;
  doctor_actions::session_started("client-owner", generation, "launch-433", 20000);
  adaptive_bitrate::set_runtime_update_supported(true, {}, 20000);
  const auto cleanup = util::fail_guard([] {
    doctor_actions::session_ended("client-owner", generation);
    adaptive_bitrate::set_enabled(false);
    config::video.adaptive_bitrate.enabled = false;
    stream_stats::update_stream_active(false);
  });

  // The player asked for 20 Mbps of PyroWave at 1080p60. The controller facts describe the host and
  // outlive a stream, so a strict sandbox an earlier test left behind would outrank this watch finding.
  stream_stats::update_controller_input_state(false, 0, "", "", "unknown", "", false, "");
  stream_stats::update_steam_input_state("unknown", 0, 0, 0, "");
  stream_stats::update_stream_active(true, "DoctorPyroWaveRaise", "203.0.113.41");
  stream_stats::update_video_stats(60.0, 20000, 1.0, "pyrowave", 1920, 1080);
  stream_stats::update_session_targets(
    60.0, 60.0, 60.0, "client_requested", "deterministic_preset_v1",
    "deterministic", "not_applicable", "Capability-validated launch profile.",
    "", 1, 0, 20000
  );
  for (int i = 0; i < 6; ++i) {
    stream_stats::update_network_stats(5.0, 0.0, 1000);
  }

  // What Doctor offers for this stream, with the capture path a real stream would have published.
  auto live = stream_stats::get_current();
  live.capture_transport = platf::frame_transport_e::dmabuf;
  live.capture_residency = platf::frame_residency_e::gpu;
  live.encode_target_residency = platf::frame_residency_e::gpu;
  const auto doctor = stream_stats::build_doctor_json(live, nlohmann::json::object());
  ASSERT_EQ(doctor.at("primary_issue"), "pyrowave_starved") << doctor.dump();
  const auto &payload = doctor.at("safe_recovery_action").at("payload_preview");
  ASSERT_EQ(payload.at("action_id"), "restore_quality");
  EXPECT_EQ(payload.at("target_bitrate_kbps"), 100148);

  const auto applied = doctor_actions::execute({{"action_id", "restore_quality"}, {"goal_source", "pyrowave_advice"}});
  ASSERT_TRUE(applied.at("status").get<bool>()) << applied.dump();
  EXPECT_EQ(applied.at("requested").at("bitrate_kbps"), 25000);
  EXPECT_EQ(applied.at("requested").at("target_bitrate_kbps"), 89121);
  EXPECT_EQ(applied.at("requested").at("goal_source"), "pyrowave_advice");
  EXPECT_EQ(applied.at("requested").at("goal_request_kbps"), 100148);
  const auto run_id = applied.at("run_id").get<std::string>();

  // Guarded steps of a quarter each, every one verified on a clean window.
  nlohmann::json step = applied;
  for (int i = 0; i < 16 && step.at("state") != "resolved"; ++i) {
    for (int j = 0; j < 2; ++j) {
      stream_stats::update_network_stats(5.0, 0.0, 1000);
    }
    doctor_actions::make_verification_window_complete_for_tests();
    step = doctor_actions::execute({{"action_id", "verify"}, {"run_id", run_id}});
    ASSERT_TRUE(step.at("status").get<bool>()) << step.dump();
  }
  ASSERT_EQ(step.at("state"), "resolved") << step.dump();
  EXPECT_EQ(step.at("goal_source"), "pyrowave_advice");
  // Past the 20000 kbps the player asked for, to where the calibrated far advice lands the encoder.
  EXPECT_EQ(adaptive_bitrate::get_doctor_state().live_bitrate_kbps, 89121);

  // Live Tuning cannot follow the raise: its feedback is held while Doctor owns the change.
  adaptive_bitrate::update_network_stats(0.0, 3.0);
  EXPECT_EQ(adaptive_bitrate::get_doctor_state().live_bitrate_kbps, 89121);

  // Turning Live Tuning on first puts back the player's own request, which stays its ceiling.
  (void) execute_with_encoder_ack(20000, [] {
    doctor_actions::set_adaptive_enabled(true);
    return nlohmann::json::object();
  });
  const auto state = adaptive_bitrate::get_state();
  EXPECT_TRUE(state.enabled);
  EXPECT_EQ(state.base_bitrate_kbps, 20000);
  EXPECT_EQ(adaptive_bitrate::get_doctor_state().live_bitrate_kbps, 20000);
  for (int i = 0; i < 3; ++i) {
    adaptive_bitrate::update_network_stats(0.0, 3.0);
  }
  EXPECT_LE(adaptive_bitrate::get_doctor_state().live_bitrate_kbps, 20000);
}

TEST(DoctorActionTests, ManualLiveBitrateTurnsLiveTuningOffForThisStreamOnly) {
  LiveConfigurationGuard live_configuration;
  ASSERT_TRUE(private_state_file::write_atomic(config::sunshine.config_file, "adaptive_bitrate_enabled = enabled\n"));
  const auto saved_before = private_state_file::read_secure(config::sunshine.config_file, 4096).payload;
  config::video.adaptive_bitrate.enabled = true;
  config::video.adaptive_bitrate.min_bitrate_kbps = 2000;
  config::video.adaptive_bitrate.max_bitrate_kbps = 100000;
  const auto restore = util::fail_guard([] {
    config::video.adaptive_bitrate.enabled = false;
    adaptive_bitrate::load_config();
    adaptive_bitrate::reset();
  });

  constexpr std::uint64_t generation = 432;
  doctor_actions::session_started("client-owner", generation, "launch-432", 20000);
  adaptive_bitrate::set_runtime_update_supported(true, {}, 20000);
  ASSERT_TRUE(adaptive_bitrate::is_enabled());

  ASSERT_TRUE(doctor_actions::set_owner_live_bitrate("client-owner", generation, "launch-432", 180000));
  EXPECT_EQ(adaptive_bitrate::get_doctor_state().live_bitrate_kbps, 180000);
  EXPECT_FALSE(adaptive_bitrate::is_enabled());
  const auto state = adaptive_bitrate::get_state();
  EXPECT_TRUE(state.configured_enabled);
  EXPECT_TRUE(state.paused_for_stream);
  // Nothing is saved for later streams.
  EXPECT_TRUE(config::video.adaptive_bitrate.enabled);
  EXPECT_EQ(private_state_file::read_secure(config::sunshine.config_file, 4096).payload, saved_before);
  // Live Tuning reads as off for the rest of this stream, and its feedback moves nothing.
  const auto during = live_tuning::snapshot(stream_stats::get_current());
  EXPECT_EQ(during.at("enabled"), false);
  EXPECT_EQ(during.at("state"), "off");
  adaptive_bitrate::update_network_stats(10.0, 60.0);
  EXPECT_EQ(adaptive_bitrate::get_doctor_state().live_bitrate_kbps, 180000);

  // The stream ends, and the saved preference is back.
  doctor_actions::session_ended("client-owner", generation);
  EXPECT_TRUE(adaptive_bitrate::is_enabled());
  EXPECT_FALSE(adaptive_bitrate::get_state().paused_for_stream);
  EXPECT_EQ(live_tuning::snapshot(stream_stats::get_current()).at("enabled"), true);
  EXPECT_EQ(private_state_file::read_secure(config::sunshine.config_file, 4096).payload, saved_before);
}

TEST(DoctorActionTests, EveryRequestIdRemainsIdempotentForTheWholeStreamGeneration) {
  LiveConfigurationGuard live_configuration;
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
  sustain_network_pressure(52.0, 3.4);

  constexpr std::uint64_t generation = 423;
  doctor_actions::session_started("client-owner", generation, "launch-423", 20000);
  adaptive_bitrate::set_runtime_update_supported(true, {}, 20000);
  stream_stats::start_session_timing("client-owner", generation, "launch-423");
  const auto cleanup = util::fail_guard([&] {
    doctor_actions::session_ended("client-owner", generation);
    stream_stats::stop_session_timing("client-owner", generation);
    stream_stats::update_stream_active(false);
  });

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
    // Owner updates persist configuration. A slow filesystem must not age the
    // synthetic observation out while this test fills the receipt history.
    stream_stats::update_network_stats(52.0, 3.4, 1000);
    context.stats = stream_stats::get_current();
    const auto request = trusted_doctor_action_request(context);
    ASSERT_EQ(request.value("action_id", ""), "lower_bitrate") << request.dump();
    if (i == 0) first_request = request;
    const auto applied = doctor_actions::execute(request, context);
    ASSERT_TRUE(applied.at("status").get<bool>()) << applied.dump();
    ASSERT_EQ(applied.at("state"), "applying");
    ASSERT_TRUE(doctor_actions::set_owner_live_bitrate(
      "client-owner", generation, "launch-423", 20000
    ));
  }

  // Capacity and retained idempotency receipts do not depend on live telemetry.
  // Reuse an admitted action envelope with a new ID rather than deriving an
  // action from expired evidence, which correctly offers no new live fix.
  stream_stats::age_latest_network_observation_for_tests(std::chrono::seconds(3));
  context.stats = stream_stats::get_current();
  ASSERT_GE(context.stats.network_last_received_age_ms, 2000);
  ASSERT_GE(context.stats.media_loss_last_received_age_ms, 2000);
  auto capacity_request = first_request;
  capacity_request["request_id"] = "test-doctor-request-over-capacity";
  const auto over_capacity = doctor_actions::execute(
    capacity_request, context
  );
  ASSERT_FALSE(over_capacity.at("status").get<bool>()) << over_capacity.dump();
  ASSERT_TRUE(over_capacity.contains("state")) << over_capacity.dump();
  EXPECT_EQ(over_capacity.at("state"), "generation_action_limit");
  EXPECT_EQ(over_capacity.at("code"), "doctor_idempotency_capacity_reached");

  const auto oldest_retry = doctor_actions::execute(first_request, context);
  ASSERT_TRUE(oldest_retry.at("status").get<bool>()) << oldest_retry.dump();
  EXPECT_FALSE(oldest_retry.at("changed").get<bool>());
  EXPECT_EQ(oldest_retry.at("state"), "superseded");
  EXPECT_EQ(
    oldest_retry.at("request_id"),
    first_request.at("request_id")
  );
  EXPECT_EQ(adaptive_bitrate::get_doctor_state().live_bitrate_kbps, 20000);

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
  sustain_network_pressure(52.0, 3.4);

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
  mark_doctor_pacing_window_confirmed(stats);

  const auto doctor = stream_stats::build_doctor_json(
    stats,
    {{"primary_issue", "frame_pacing"}, {"grade", "watch"}}
  );

  EXPECT_EQ(doctor.at("primary_issue"), "frame_pacing");
  EXPECT_EQ(doctor.at("traffic_light"), "amber");
  EXPECT_EQ(doctor.at("status"), "needs_action");
  EXPECT_NE(doctor.at("safe_recovery_action").at("id"), "lower_bitrate");
}

TEST(StreamStatsDoctorTests, KeepsHealthyVaapiShmFallbackInformational) {
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
  stats.fps = 120.0;
  stats.encode_target_fps = 120.0;
  stats.capture_source_fps = 120.0;
  stats.encode_time_ms = 4.0;
  stats.avg_frame_age_ms = 6.0;

  const auto doctor = stream_stats::build_doctor_json(stats, nlohmann::json::object());

  EXPECT_EQ(doctor.at("traffic_light"), "green");
  EXPECT_EQ(doctor.at("status"), "ok");
  EXPECT_EQ(doctor.at("severity"), "info");
  EXPECT_EQ(doctor.at("simple_state"), "Streaming ready");
  EXPECT_EQ(doctor.at("primary_issue"), "none");
  EXPECT_EQ(doctor.at("safe_recovery_action").at("id"), "none");
  EXPECT_FALSE(doctor.at("safe_recovery_action").at("destructive"));
  EXPECT_TRUE(doctor.at("advanced_evidence").at("raw_fields_redacted"));
  EXPECT_EQ(
    doctor.at("advanced_evidence").at("linux_gpu_profile").at("encoder_api"),
    "vaapi"
  );
  const auto &capture = *std::find_if(
    doctor.at("evidence").begin(),
    doctor.at("evidence").end(),
    [](const auto &item) { return item.value("id", "") == "capture_path"; }
  );
  EXPECT_EQ(capture.at("status"), "watch")
    << "the compatibility path remains visible without grading a healthy stream down";
}

TEST(StreamStatsDoctorTests, KeepsNvidiaHeadlessWarningsInAdvancedEvidence) {
  LinuxDisplayConfigGuard guard;
  config::video.encoder = "nvenc";
  config::video.linux_display.headless_mode = true;
  config::video.linux_display.use_cage_compositor = true;
  config::video.linux_display.prefer_gpu_native_capture = false;
  stream_stats::set_build_has_cuda_for_tests(true);

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

  // Production pings arrive between Nova's one-second counter reports. They
  // carry no media-loss measurement, so their clean control-channel loss must
  // not be interpreted as clean video delivery, in the newest reading or in
  // the window's figure.
  for (int report = 0; report < stream_stats::network_judge_t::k_min_media_samples; ++report) {
    sample.client_monotonic_ms += 1'000;
    sample.frames_expected += 100;
    sample.frames_received += 80;
    sample.frames_lost += 20;
    const auto loss = stream_stats::ingest_client_media_counters(sample);
    ASSERT_TRUE(loss.accepted);
    ASSERT_TRUE(loss.observation_published);
    ASSERT_DOUBLE_EQ(loss.media_loss_pct, 20.0);
    stream_stats::update_control_channel_stats(4.0, 0.0, 777);
    stream_stats::update_control_channel_stats(4.0, 0.0, 777);
  }

  const auto stats = stream_stats::get_current();
  EXPECT_TRUE(stats.network_risk);
  EXPECT_TRUE(stats.packet_loss_available);
  EXPECT_DOUBLE_EQ(stats.packet_loss, 20.0);
  EXPECT_EQ(stats.packet_loss_source, "media_transport");
  EXPECT_DOUBLE_EQ(stats.network_verdict.loss_pct, 20.0);
  EXPECT_TRUE(stats.network_verdict.loss_elevated);

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

namespace {
  using judge_clock = stream_stats::network_judge_t::clock_type;

  /// One second's media report from a client: the frames it expected and how many never arrived.
  struct recorded_report_t {
    std::uint64_t expected;
    std::uint64_t lost;
  };

  // The HEVC run of the 2026-09-29 in-game smoke on the Retroid Pocket 6, 3840x2160 at 120 fps over
  // Wi-Fi. The host's polls read 0.0, 7.38, 0.0, 0.0, 0.0, 0.0, 7.44 and 0.0 percent: at 120 fps,
  // 9 of 122 frames and 9 of 121, each one second's report. The run went on like that for five
  // minutes, and a later poll read 5.83, 7 of 120.
  constexpr std::array<recorded_report_t, 8> k_recorded_hevc_reports {{
    {120, 0}, {122, 9}, {120, 0}, {120, 0}, {120, 0}, {120, 0}, {121, 9}, {120, 0}
  }};

  // Host RTT through that run, in ms: the readings paired with its screenshots.
  constexpr std::array<double, 10> k_recorded_hevc_rtt_ms {4.0, 4.4, 10.1, 7.6, 9.6, 7.9, 8.6, 4.3, 9.0, 17.0};

  // Host RTT through the PyroWave run, 1920x1080 at 120 fps on the same device, in ms: 10, 26 and 10
  // on close samples, and 23 and 39 in its Wi-Fi rtt_spike periods, where ENet's estimate stays up
  // for a second or two before it comes back down.
  constexpr std::array<double, 10> k_recorded_pyrowave_rtt_ms {10.0, 26.0, 10.0, 8.0, 39.0, 40.0, 23.0, 10.0, 6.0, 10.0};

  judge_clock::time_point judge_start() {
    return judge_clock::time_point {} + std::chrono::hours(1);
  }
}  // namespace

TEST(NetworkJudgeTests, RecordedHevcLossChangesTheVerdictOnceAndHoldsIt) {
  // Judged a report at a time, as Doctor did, this loss was confirmed and cleared twice every eight
  // seconds. Over the window it runs a little under 2% with a 7% burst every few seconds, so the
  // verdict turns to pressure once, as soon as the window holds enough of it, and keeps it: the
  // figure never falls to 1% while the pattern lasts.
  stream_stats::network_judge_t judge;
  auto at = judge_start();
  int per_report_changes = 0;
  bool per_report_was = false;
  int verdict_changes = 0;
  bool verdict_was = false;
  int entered_at = -1;
  int second = 0;
  for (int round = 0; round < 5; ++round) {
    for (const auto &report : k_recorded_hevc_reports) {
      ++second;
      at += std::chrono::seconds(1);
      judge.add_media(at, static_cast<double>(report.expected), static_cast<double>(report.lost));
      for (int ping = 0; ping < 10; ++ping) {
        judge.add_rtt(at + std::chrono::milliseconds(100 * ping), k_recorded_hevc_rtt_ms[ping]);
      }
      const bool per_report = report.lost * 100.0 / report.expected > 2.0;
      per_report_changes += per_report != per_report_was ? 1 : 0;
      per_report_was = per_report;
      const auto verdict = judge.verdict(at);
      if (verdict.loss_elevated != verdict_was) {
        ++verdict_changes;
        entered_at = second;
      }
      verdict_was = verdict.loss_elevated;
    }
  }
  EXPECT_EQ(per_report_changes, 20);
  EXPECT_EQ(verdict_changes, 1);
  // Seven seconds in, two of seven reports lost 18 of 843 frames, 2.1%.
  EXPECT_EQ(entered_at, 7);

  const auto verdict = judge.verdict(at);
  ASSERT_TRUE(verdict.loss_available);
  EXPECT_TRUE(verdict.loss_elevated);
  EXPECT_TRUE(verdict.risk);
  EXPECT_EQ(verdict.media_samples, 20);  // the last 20 seconds
  EXPECT_EQ(verdict.frames_expected, 2407u);
  EXPECT_EQ(verdict.frames_lost, 45u);
  EXPECT_NEAR(verdict.loss_pct, 45.0 * 100.0 / 2407.0, 1e-9);
  EXPECT_EQ(stream_stats::network_loss_state(verdict), "elevated");
  ASSERT_TRUE(verdict.rtt_available);
  EXPECT_LT(verdict.rtt_ms, stream_stats::network_judge_t::k_rtt_exit_ms);
  EXPECT_EQ(stream_stats::network_rtt_state(verdict), "clean");
}

TEST(NetworkJudgeTests, SustainedLossEntersAtItsBandAndHoldsThroughACleanSecond) {
  stream_stats::network_judge_t judge;
  auto at = judge_start();
  // A clean stream first, so the window is full and judged.
  for (int i = 0; i < 20; ++i) {
    at += std::chrono::seconds(1);
    judge.add_media(at, 120.0, 0.0);
  }
  ASSERT_TRUE(judge.verdict(at).loss_available);
  EXPECT_EQ(stream_stats::network_loss_state(judge.verdict(at)), "clean");

  // Real pressure: every second loses 6 of 120 frames. The window crosses 2% on the seventh.
  int entered_at = -1;
  for (int i = 0; i < 12; ++i) {
    at += std::chrono::seconds(1);
    judge.add_media(at, 120.0, 6.0);
    if (entered_at < 0 && judge.verdict(at).loss_elevated) {
      entered_at = i;
    }
  }
  EXPECT_EQ(entered_at, 7);

  // One clean second in the middle of it is not a recovery.
  at += std::chrono::seconds(1);
  judge.add_media(at, 120.0, 0.0);
  EXPECT_TRUE(judge.verdict(at).loss_elevated);
  EXPECT_TRUE(judge.verdict(at).risk);

  // Once the loss stops, the verdict stands until the window's figure falls below 1%, which takes
  // most of the window, and then clears once.
  int cleared_at = -1;
  for (int i = 0; i < 20; ++i) {
    at += std::chrono::seconds(1);
    judge.add_media(at, 120.0, 0.0);
    const auto verdict = judge.verdict(at);
    if (cleared_at < 0 && !verdict.loss_elevated) {
      cleared_at = i;
      EXPECT_LT(verdict.loss_pct, stream_stats::network_judge_t::k_loss_exit_pct);
    }
    if (cleared_at >= 0) {
      EXPECT_FALSE(verdict.loss_elevated) << "second " << i;
    }
  }
  EXPECT_GE(cleared_at, 8);
}

TEST(NetworkJudgeTests, AVerdictReadBetweenReportsQuotesTheFigureItsBandWasJudgedOn) {
  // The band moves only when a report arrives, a second apart or more, and PyroWave's reports come
  // 1000 ms apart or more. The figure used to be worked out again whenever the verdict was read, so
  // a lossy report leaving the window between two reports left a pressure verdict quoting 0.70%,
  // below the 1% that clears it, and a clean one leaving it left a clean verdict quoting 2.02%.
  stream_stats::network_judge_t judge;
  auto at = judge_start();
  // Three seconds that lost 8 of 120 frames each: pressure from the fifth report, held until the
  // window's figure falls below 1%.
  for (int i = 0; i < 3; ++i) {
    at += std::chrono::seconds(1);
    judge.add_media(at, 120.0, 8.0);
  }
  for (int i = 0; i < 17; ++i) {
    at += std::chrono::seconds(1);
    judge.add_media(at, 120.0, 0.0);
  }
  const auto held = judge.verdict(at);
  ASSERT_TRUE(held.loss_elevated);
  EXPECT_NEAR(held.loss_pct, 24.0 * 100.0 / 2400.0, 1e-9);

  // Half a second after the next report was due, the first lossy second has left the window. The
  // verdict still quotes the figure its band was judged on.
  const auto between = judge.verdict(at + std::chrono::milliseconds(1500));
  EXPECT_TRUE(between.loss_elevated);
  EXPECT_DOUBLE_EQ(between.loss_pct, held.loss_pct);
  EXPECT_EQ(between.frames_lost, held.frames_lost);
  EXPECT_EQ(between.frames_expected, held.frames_expected);
  EXPECT_EQ(stream_stats::network_loss_state(between), "elevated");

  // The next report judges the window as it now is, and that clears the band.
  at += std::chrono::milliseconds(1500);
  judge.add_media(at, 120.0, 0.0);
  const auto cleared = judge.verdict(at);
  EXPECT_FALSE(cleared.loss_elevated);
  EXPECT_LT(cleared.loss_pct, stream_stats::network_judge_t::k_loss_exit_pct);

  // The other way round: a clean window just under 2% stays clean, and its figure stays under 2%,
  // while a clean second leaves the window before the next report.
  stream_stats::network_judge_t rising;
  at = judge_start();
  for (int i = 0; i < 15; ++i) {
    at += std::chrono::seconds(1);
    rising.add_media(at, 120.0, 0.0);
  }
  for (const double lost : {9.0, 9.0, 9.0, 9.0, 10.0}) {
    at += std::chrono::seconds(1);
    rising.add_media(at, 120.0, lost);
    ASSERT_FALSE(rising.verdict(at).loss_elevated);
  }
  const auto clean = rising.verdict(at + std::chrono::milliseconds(1500));
  EXPECT_FALSE(clean.loss_elevated);
  EXPECT_NEAR(clean.loss_pct, 46.0 * 100.0 / 2400.0, 1e-9);
  EXPECT_LT(clean.loss_pct, stream_stats::network_judge_t::k_loss_enter_pct);
  EXPECT_EQ(stream_stats::network_loss_state(clean), "light");
}

TEST(NetworkJudgeTests, ControlChannelLossHoldsItsBandAndNeverCountsAsPressure) {
  // The HEVC run's control-channel estimate read 1.08 and then 2.81 on consecutive polls. Taken a
  // reading at a time against 2%, Doctor's control channel finding came and went with every poll.
  stream_stats::network_judge_t judge;
  auto at = judge_start();
  int per_reading_changes = 0;
  bool per_reading_was = false;
  int verdict_changes = 0;
  bool verdict_was = false;
  const auto second_of = [&](double loss_pct) {
    for (int ping = 0; ping < 10; ++ping) {
      at += std::chrono::milliseconds(100);
      judge.add_control_loss(at, loss_pct);
      const bool per_reading = loss_pct >= stream_stats::network_judge_t::k_loss_enter_pct;
      per_reading_changes += per_reading != per_reading_was ? 1 : 0;
      per_reading_was = per_reading;
      const bool judged = judge.verdict(at).control_loss_elevated;
      verdict_changes += judged != verdict_was ? 1 : 0;
      verdict_was = judged;
    }
  };
  // Retries that last: the window's figure crosses 2% and the finding stands.
  for (int second = 0; second < 5; ++second) {
    second_of(2.81);
  }
  // Then the estimate swings a poll at a time and averages under 2% but above 1%.
  for (int second = 0; second < 25; ++second) {
    second_of(second % 2 == 0 ? 1.08 : 2.81);
  }
  EXPECT_GE(per_reading_changes, 20);
  EXPECT_EQ(verdict_changes, 1);
  auto verdict = judge.verdict(at);
  ASSERT_TRUE(verdict.control_loss_available);
  EXPECT_TRUE(verdict.control_loss_elevated);
  EXPECT_NEAR(verdict.control_loss_pct, (1.08 + 2.81) / 2.0, 1e-9);
  EXPECT_FALSE(verdict.risk);

  // Quiet for long enough that the window's figure falls below 1%, and it clears once.
  for (int second = 0; second < 20; ++second) {
    second_of(0.2);
  }
  verdict = judge.verdict(at);
  EXPECT_FALSE(verdict.control_loss_elevated);
  EXPECT_EQ(verdict_changes, 2);
  const auto json = stream_stats::network_verdict_json(verdict);
  EXPECT_NEAR(json.at("control_loss_pct").get<double>(), 0.2, 1e-9);
  EXPECT_FALSE(json.at("control_loss_elevated").get<bool>());
}

TEST(NetworkJudgeTests, RecordedPyroWaveRttSpikesNeverElevate) {
  // The newest-reading tracker flips twice on this sequence. The window's median never moves off a
  // LAN's figures.
  stream_stats::network_judge_t judge;
  stream_stats::network_risk_tracker_t tracker;
  auto at = judge_start();
  int tracker_flips = 0;
  bool tracker_was = false;
  for (int round = 0; round < 3; ++round) {
    for (const auto rtt_ms : k_recorded_pyrowave_rtt_ms) {
      for (int ping = 0; ping < 10; ++ping) {
        at += std::chrono::milliseconds(100);
        judge.add_rtt(at, rtt_ms);
        const bool tracked = tracker.update(0.0, rtt_ms);
        tracker_flips += tracked != tracker_was ? 1 : 0;
        tracker_was = tracked;
        const auto verdict = judge.verdict(at);
        EXPECT_FALSE(verdict.rtt_elevated) << "rtt " << rtt_ms << " at round " << round;
      }
    }
  }
  EXPECT_GE(tracker_flips, 4);
  const auto verdict = judge.verdict(at);
  ASSERT_TRUE(verdict.rtt_available);
  EXPECT_DOUBLE_EQ(verdict.rtt_ms, 10.0);
}

TEST(NetworkJudgeTests, SustainedRttEntersAtItsBandAndClearsBelowIt) {
  stream_stats::network_judge_t judge;
  auto at = judge_start();
  for (int i = 0; i < 50; ++i) {
    at += std::chrono::milliseconds(100);
    judge.add_rtt(at, 8.0);
  }
  EXPECT_FALSE(judge.verdict(at).rtt_elevated);
  // A congested link: the median crosses 28 ms once more than half the window reads above it.
  for (int i = 0; i < 60; ++i) {
    at += std::chrono::milliseconds(100);
    judge.add_rtt(at, 35.0);
  }
  EXPECT_TRUE(judge.verdict(at).rtt_elevated);
  // Better, still inside the band: the verdict holds.
  for (int i = 0; i < 200; ++i) {
    at += std::chrono::milliseconds(100);
    judge.add_rtt(at, 24.0);
  }
  EXPECT_DOUBLE_EQ(judge.verdict(at).rtt_ms, 24.0);
  EXPECT_TRUE(judge.verdict(at).rtt_elevated);
  // Below 20 ms it clears.
  for (int i = 0; i < 200; ++i) {
    at += std::chrono::milliseconds(100);
    judge.add_rtt(at, 12.0);
  }
  EXPECT_FALSE(judge.verdict(at).rtt_elevated);
  EXPECT_FALSE(judge.verdict(at).risk);
}

TEST(NetworkJudgeTests, AThinWindowHasNoVerdictAndAGapStartsOver) {
  stream_stats::network_judge_t judge;
  auto at = judge_start();
  // Four reports are too few to judge, however bad.
  for (int i = 0; i < stream_stats::network_judge_t::k_min_media_samples - 1; ++i) {
    at += std::chrono::seconds(1);
    judge.add_media(at, 120.0, 60.0);
    EXPECT_FALSE(judge.verdict(at).loss_available);
    EXPECT_FALSE(judge.verdict(at).loss_elevated);
  }
  at += std::chrono::seconds(1);
  judge.add_media(at, 120.0, 60.0);
  EXPECT_TRUE(judge.verdict(at).loss_elevated);
  EXPECT_EQ(stream_stats::network_loss_state(judge.verdict(at)), "elevated");

  // Reports stop. The verdict is not carried across the gap: once the window empties there is none,
  // and the first reports after it are judged afresh.
  at += stream_stats::network_judge_t::k_window + std::chrono::seconds(1);
  EXPECT_FALSE(judge.verdict(at).loss_available);
  EXPECT_FALSE(judge.verdict(at).risk);
  judge.add_media(at, 120.0, 0.0);
  EXPECT_FALSE(judge.verdict(at).loss_elevated);
  EXPECT_EQ(stream_stats::network_loss_state(judge.verdict(at)), "collecting");
}

TEST(NetworkJudgeTests, EnetRttConvergenceNeverEntersTheWindow) {
  // ENet seeds a fresh peer at 500 ms and converges by about an eighth per ack; on a 3 ms LAN the
  // first readings sit far above the band. They are held back until a calm reading arms the window.
  stream_stats::network_judge_t judge;
  auto at = judge_start();
  double rtt = 500.0;
  for (int i = 0; i < 60; ++i) {
    at += std::chrono::milliseconds(100);
    judge.add_rtt(at, rtt);
    EXPECT_FALSE(judge.verdict(at).rtt_elevated) << "reading " << i << " rtt " << rtt;
    rtt = 3.0 + (rtt - 3.0) * 7.0 / 8.0;
  }
  EXPECT_LT(judge.verdict(at).rtt_ms, stream_stats::network_judge_t::k_rtt_exit_ms);
}

TEST(NetworkJudgeTests, ALinkBadFromTheStartStillElevatesAfterTheGrace) {
  stream_stats::network_judge_t judge;
  auto at = judge_start();
  for (int i = 0; i < stream_stats::network_judge_t::k_rtt_armed_after + stream_stats::network_judge_t::k_min_rtt_readings; ++i) {
    at += std::chrono::milliseconds(100);
    judge.add_rtt(at, 90.0);
  }
  EXPECT_TRUE(judge.verdict(at).rtt_elevated);
}

TEST(NetworkJudgeTests, TheFigureWeighsFramesNotReports) {
  stream_stats::network_judge_t judge;
  auto at = judge_start();
  // A short report that lost everything does not outweigh four full seconds that lost nothing.
  for (int i = 0; i < 4; ++i) {
    at += std::chrono::seconds(1);
    judge.add_media(at, 120.0, 0.0);
  }
  at += std::chrono::seconds(1);
  judge.add_media(at, 5.0, 5.0);
  const auto verdict = judge.verdict(at);
  ASSERT_TRUE(verdict.loss_available);
  EXPECT_EQ(verdict.frames_expected, 485u);
  EXPECT_EQ(verdict.frames_lost, 5u);
  EXPECT_NEAR(verdict.loss_pct, 500.0 / 485.0, 1e-9);
  EXPECT_FALSE(verdict.loss_elevated);

  const auto json = stream_stats::network_verdict_json(verdict);
  EXPECT_EQ(json.at("loss_basis"), "video_frames_lost_after_fec");
  EXPECT_EQ(json.at("window_seconds"), 20);
  EXPECT_EQ(json.at("frames_lost"), 5);
  EXPECT_EQ(json.at("frames_expected"), 485);
  EXPECT_EQ(json.at("loss_state"), "light");
  EXPECT_TRUE(json.at("rtt_median_ms").is_null());
  EXPECT_EQ(json.at("rtt_state"), "collecting");
}

TEST(StreamStatsHotFieldTests, ClientMediaCountersFillTheStreamsOwnClientRow) {
  adaptive_bitrate::set_enabled(false);
  stream_stats::update_stream_active(false);
  stream_stats::add_client("198.51.100.44", "RetroidPocket6", 61, "nova");
  stream_stats::start_session_timing("owner-row", 61, "app-session-row");
  stream_stats::update_stream_active(true);
  const auto cleanup = util::fail_guard([] {
    stream_stats::stop_session_timing("owner-row", 61);
    stream_stats::remove_client("198.51.100.44", 61);
    stream_stats::update_stream_active(false);
  });

  stream_stats::client_media_counters_t sample {
    .owner_uuid = "owner-row",
    .app_session_id = "app-session-row",
    .session_generation = 61,
    .client_monotonic_ms = 1'000,
    .frames_expected = 120,
    .frames_received = 120,
    .frames_lost = 0
  };
  ASSERT_TRUE(stream_stats::ingest_client_media_counters(sample).accepted);
  // The HEVC run's reports: the newest one second figure swings between 0 and 7.4%.
  for (const auto &report : k_recorded_hevc_reports) {
    sample.client_monotonic_ms += 1'000;
    sample.frames_expected += report.expected;
    sample.frames_lost += report.lost;
    sample.frames_received = sample.frames_expected - sample.frames_lost;
    ASSERT_TRUE(stream_stats::ingest_client_media_counters(sample).observation_published);
    const auto row = stream_stats::get_current().clients.front();
    const auto verdict = stream_stats::current_network_verdict();
    // Until the window can judge, the row has no figure rather than the newest report's.
    EXPECT_EQ(row.packet_loss_available, verdict.loss_available);
    if (verdict.loss_available) {
      EXPECT_DOUBLE_EQ(row.packet_loss, verdict.loss_pct);
    }
  }

  // The top level said the loss arrived while the stream's own row said it never had, which is how a
  // reading of this host's stats concluded a client's loss did not reach it. The row now quotes the
  // window's figure, as Doctor does: 18 of 963 frames, where the newest report lost none.
  const auto stats = stream_stats::get_current();
  ASSERT_EQ(stats.clients.size(), 1u);
  EXPECT_TRUE(stats.packet_loss_available);
  EXPECT_DOUBLE_EQ(stats.packet_loss, 0.0);
  EXPECT_TRUE(stats.clients.front().packet_loss_available);
  EXPECT_EQ(stats.clients.front().packet_loss_source, "media_transport");
  EXPECT_NEAR(stats.clients.front().packet_loss, 18.0 * 100.0 / 963.0, 1e-9);
  auto json = nlohmann::json::parse(stats.to_json());
  EXPECT_EQ(json.at("clients").at(0).at("packet_loss_available"), true);
  EXPECT_NEAR(json.at("clients").at(0).at("packet_loss").get<double>(), 18.0 * 100.0 / 963.0, 1e-9);

  // Once the client's reports stop for more than five seconds the row stops quoting it, as Doctor does.
  auto aged = stats;
  aged.clients.front().packet_loss_received_at -= std::chrono::seconds(6);
  json = nlohmann::json::parse(aged.to_json());
  EXPECT_EQ(json.at("clients").at(0).at("packet_loss_available"), false);
  EXPECT_EQ(json.at("clients").at(0).at("packet_loss_source"), "unavailable");
}

TEST(StreamStatsHotFieldTests, ClientMediaCountersAreJudgedOverTheWindow) {
  adaptive_bitrate::set_enabled(false);
  stream_stats::update_stream_active(false);
  stream_stats::start_session_timing("owner-window", 62, "app-session-window");
  stream_stats::update_stream_active(true);
  const auto cleanup = util::fail_guard([] {
    stream_stats::stop_session_timing("owner-window", 62);
    stream_stats::update_stream_active(false);
  });
  for (int i = 0; i < 6; ++i) {
    stream_stats::update_control_channel_stats(6.0, 7.75, 777);
  }

  stream_stats::client_media_counters_t sample {
    .owner_uuid = "owner-window",
    .app_session_id = "app-session-window",
    .session_generation = 62,
    .client_monotonic_ms = 1'000,
    .frames_expected = 0,
    .frames_received = 0,
    .frames_lost = 0
  };
  ASSERT_TRUE(stream_stats::ingest_client_media_counters(sample).accepted);
  for (const auto &report : k_recorded_hevc_reports) {
    sample.client_monotonic_ms += 1'000;
    sample.frames_expected += report.expected;
    sample.frames_lost += report.lost;
    sample.frames_received = sample.frames_expected - sample.frames_lost;
    const auto result = stream_stats::ingest_client_media_counters(sample);
    ASSERT_TRUE(result.observation_published);
    EXPECT_EQ(result.frames_expected, report.expected);
    EXPECT_EQ(result.frames_lost, report.lost);
  }

  // The newest report lost nothing. The window lost 18 of 963 frames, and the verdict it reached at
  // the seventh report, 18 of 843, holds.
  const auto stats = stream_stats::get_current();
  EXPECT_DOUBLE_EQ(stats.packet_loss, 0.0);
  ASSERT_TRUE(stats.network_verdict.loss_available);
  EXPECT_EQ(stats.network_verdict.media_samples, 8);
  EXPECT_EQ(stats.network_verdict.frames_lost, 18u);
  EXPECT_EQ(stats.network_verdict.frames_expected, 963u);
  EXPECT_NEAR(stats.network_verdict.loss_pct, 18.0 * 100.0 / 963.0, 1e-9);
  EXPECT_TRUE(stats.network_verdict.loss_elevated);
  const auto json = nlohmann::json::parse(stats.to_json());
  EXPECT_EQ(json.at("network_verdict").at("frames_lost"), 18);
  EXPECT_EQ(json.at("network_verdict").at("loss_state"), "elevated");
  EXPECT_EQ(json.at("network_verdict").at("loss_basis"), "video_frames_lost_after_fec");
}

TEST(StreamStatsHotFieldTests, AVerdictWhoseReportsStoppedIsServedAsStale) {
  // Doctor stops judging loss five seconds after the client's newest media report. The stream stats
  // kept serving the window's old figure, and "elevated" beside it, until the window emptied, so the
  // console and the session status went on quoting a loss Doctor said it was not judging.
  adaptive_bitrate::set_enabled(false);
  stream_stats::update_stream_active(false);
  stream_stats::start_session_timing("owner-stale", 63, "app-session-stale");
  stream_stats::update_stream_active(true);
  const auto cleanup = util::fail_guard([] {
    stream_stats::stop_session_timing("owner-stale", 63);
    stream_stats::update_stream_active(false);
  });
  for (int i = 0; i < 6; ++i) {
    stream_stats::update_control_channel_stats(6.0, 0.0, 777);
  }
  stream_stats::client_media_counters_t sample {
    .owner_uuid = "owner-stale",
    .app_session_id = "app-session-stale",
    .session_generation = 63,
    .client_monotonic_ms = 1'000,
    .frames_expected = 0,
    .frames_received = 0,
    .frames_lost = 0
  };
  ASSERT_TRUE(stream_stats::ingest_client_media_counters(sample).accepted);
  for (const auto &report : k_recorded_hevc_reports) {
    sample.client_monotonic_ms += 1'000;
    sample.frames_expected += report.expected;
    sample.frames_lost += report.lost;
    sample.frames_received = sample.frames_expected - sample.frames_lost;
    ASSERT_TRUE(stream_stats::ingest_client_media_counters(sample).observation_published);
  }
  const auto live = stream_stats::get_current();
  ASSERT_TRUE(stream_stats::judged_network(live).loss_pressure);
  EXPECT_EQ(nlohmann::json::parse(live.to_json()).at("network_verdict").at("loss_state"), "elevated");

  // The client's reports stop reaching the host while the control channel's pings go on.
  stream_stats::age_latest_network_observation_for_tests(std::chrono::seconds(6));
  stream_stats::update_control_channel_stats(6.0, 0.0, 777);
  const auto stale = stream_stats::get_current();
  const auto network = stream_stats::judged_network(stale);
  EXPECT_FALSE(network.loss_judged);
  EXPECT_TRUE(network.rtt_judged);
  const auto served = stream_stats::served_network_verdict(stale);
  EXPECT_FALSE(served.loss_available);
  EXPECT_TRUE(served.loss_stale);
  EXPECT_FALSE(served.loss_elevated);
  EXPECT_FALSE(served.risk);
  EXPECT_EQ(served.risk, network.risk);

  const auto json = nlohmann::json::parse(stale.to_json()).at("network_verdict");
  EXPECT_EQ(json.at("loss_state"), "stale");
  EXPECT_TRUE(json.at("loss_pct").is_null());
  EXPECT_EQ(json.at("rtt_state"), "clean");
  EXPECT_FALSE(json.at("risk").get<bool>());

  auto doctor_input = stale;
  doctor_input.capture_transport = platf::frame_transport_e::dmabuf;
  doctor_input.capture_residency = platf::frame_residency_e::gpu;
  doctor_input.encode_target_residency = platf::frame_residency_e::gpu;
  const auto doctor = stream_stats::build_doctor_json(doctor_input, nlohmann::json::object());
  const auto &evidence = doctor.at("evidence");
  const auto loss_row = std::find_if(evidence.begin(), evidence.end(), [](const nlohmann::json &row) {
    return row.value("id", std::string {}) == "packet_loss";
  });
  ASSERT_NE(loss_row, evidence.end());
  EXPECT_EQ(loss_row->at("status"), "unknown");
  EXPECT_TRUE(loss_row->at("value").is_null());
  EXPECT_NE(loss_row->at("detail").get<std::string>().find("No client media report has reached the host"), std::string::npos)
    << loss_row->dump();
  EXPECT_EQ(doctor.at("advanced_evidence").at("network_verdict").at("loss_state"), "stale");

  // What the window last judged is still there for a session that ends now to be graded by.
  EXPECT_TRUE(stale.network_verdict.loss_elevated);
  EXPECT_TRUE(stale.network_verdict.risk);
}

namespace {
  /// Each headline Doctor gave, and how many times it changed.
  struct headline_run_t {
    std::vector<std::string> headlines;

    int changes() const {
      int count = 0;
      for (std::size_t i = 1; i < headlines.size(); ++i) {
        count += headlines[i] != headlines[i - 1] ? 1 : 0;
      }
      return count;
    }

    std::string text() const {
      std::string joined;
      for (const auto &headline : headlines) {
        joined += (joined.empty() ? "" : " ") + headline;
      }
      return joined;
    }
  };

  /// What Doctor says of the live stream now, with the capture path a real stream would have published.
  std::string live_headline() {
    auto live = stream_stats::get_current();
    live.capture_transport = platf::frame_transport_e::dmabuf;
    live.capture_residency = platf::frame_residency_e::gpu;
    live.encode_target_residency = platf::frame_residency_e::gpu;
    return stream_stats::build_doctor_json(live, nlohmann::json::object()).at("primary_issue").get<std::string>();
  }
}  // namespace

TEST(StreamStatsHotFieldTests, LiveTuningHearsTheLossTheVerdictConfirms) {
  // The HEVC run's reports reach Live Tuning once the verdict calls their loss pressure: nothing while
  // the window stays under the pressure line, and each report's own figure from the report that
  // crosses it. Each report used to reach it whatever the verdict said, so the second one's 7.4% had
  // Live Tuning cutting while Doctor said the loss was not confirmed.
  config::video.adaptive_bitrate.enabled = true;
  config::video.adaptive_bitrate.min_bitrate_kbps = 2000;
  config::video.adaptive_bitrate.max_bitrate_kbps = 100000;
  adaptive_bitrate::load_config();
  adaptive_bitrate::reset();
  adaptive_bitrate::set_runtime_update_supported(true);
  adaptive_bitrate::set_base_bitrate(268988);
  stream_stats::update_stream_active(false);
  stream_stats::start_session_timing("owner-live-tuning", 72, "app-session-live-tuning");
  stream_stats::update_stream_active(true);
  const auto cleanup = util::fail_guard([] {
    stream_stats::stop_session_timing("owner-live-tuning", 72);
    stream_stats::update_stream_active(false);
    adaptive_bitrate::set_enabled(false);
    config::video.adaptive_bitrate.enabled = false;
  });
  for (int i = 0; i < 6; ++i) {
    stream_stats::update_control_channel_stats(6.0, 7.75, 777);
  }

  stream_stats::client_media_counters_t sample {
    .owner_uuid = "owner-live-tuning",
    .app_session_id = "app-session-live-tuning",
    .session_generation = 72,
    .client_monotonic_ms = 1'000,
    .frames_expected = 0,
    .frames_received = 0,
    .frames_lost = 0
  };
  ASSERT_TRUE(stream_stats::ingest_client_media_counters(sample).accepted);
  int report = 0;
  for (const auto &recorded : k_recorded_hevc_reports) {
    ++report;
    sample.client_monotonic_ms += 1'000;
    sample.frames_expected += recorded.expected;
    sample.frames_lost += recorded.lost;
    sample.frames_received = sample.frames_expected - sample.frames_lost;
    ASSERT_TRUE(stream_stats::ingest_client_media_counters(sample).observation_published);
    const auto heard = adaptive_bitrate::get_state().ewma_packet_loss;
    const auto verdict = stream_stats::get_current().network_verdict;
    if (report < 7) {
      EXPECT_FALSE(verdict.loss_elevated) << "report " << report;
      EXPECT_DOUBLE_EQ(heard, 0.0) << "report " << report;
    } else if (report == 7) {
      // The report that crosses the line: its own 9 of 121 frames, averaged in once.
      EXPECT_TRUE(verdict.loss_elevated) << "report " << report;
      EXPECT_NEAR(heard, 0.3 * 900.0 / 121.0, 1e-9) << "report " << report;
    } else {
      // A clean report under a verdict that still holds: the average falls, and nothing refreshes it.
      EXPECT_TRUE(verdict.loss_elevated) << "report " << report;
      EXPECT_NEAR(heard, 0.7 * 0.3 * 900.0 / 121.0, 1e-9) << "report " << report;
    }
  }
}

namespace {
  constexpr int k_live_tuning_base_kbps = 268988;

  /// A stream Live Tuning owns, at the HEVC run's 269 Mbps, with its client's counters at zero.
  struct live_tuning_stream_t {
    std::string owner;
    std::uint64_t generation;
    stream_stats::client_media_counters_t sample;

    live_tuning_stream_t(std::string owner_uuid, std::uint64_t session_generation):
        owner(std::move(owner_uuid)),
        generation(session_generation) {
      config::video.adaptive_bitrate.enabled = true;
      config::video.adaptive_bitrate.min_bitrate_kbps = 2000;
      config::video.adaptive_bitrate.max_bitrate_kbps = 100000;
      adaptive_bitrate::load_config();
      adaptive_bitrate::reset();
      adaptive_bitrate::set_runtime_update_supported(true);
      adaptive_bitrate::set_base_bitrate(k_live_tuning_base_kbps);
      stream_stats::update_stream_active(false);
      stream_stats::start_session_timing(owner, generation, "app-session-" + owner);
      stream_stats::update_stream_active(true);
      for (int ping = 0; ping < 6; ++ping) {
        stream_stats::update_control_channel_stats(6.0, 0.0, 777);
      }
      sample = {
        .owner_uuid = owner,
        .app_session_id = "app-session-" + owner,
        .session_generation = generation,
        .client_monotonic_ms = 1'000,
        .frames_expected = 0,
        .frames_received = 0,
        .frames_lost = 0
      };
      EXPECT_TRUE(stream_stats::ingest_client_media_counters(sample).accepted);
    }

    ~live_tuning_stream_t() {
      stream_stats::stop_session_timing(owner, generation);
      stream_stats::update_stream_active(false);
      adaptive_bitrate::set_enabled(false);
      config::video.adaptive_bitrate.enabled = false;
    }

    /// One second as Live Tuning lives it: the second passes for the judge and the controller, the
    /// client's media report arrives, and ten control pings follow, as Nova and ENet send them.
    /// Returns the live target at the end of the second.
    int second(std::uint64_t expected, std::uint64_t lost) {
      stream_stats::age_network_judge_for_tests(std::chrono::seconds(1));
      adaptive_bitrate::age_for_tests(std::chrono::seconds(1));
      sample.client_monotonic_ms += 1'000;
      sample.frames_expected += expected;
      sample.frames_lost += lost;
      sample.frames_received = sample.frames_expected - sample.frames_lost;
      EXPECT_TRUE(stream_stats::ingest_client_media_counters(sample).observation_published);
      for (int ping = 0; ping < 10; ++ping) {
        stream_stats::update_control_channel_stats(6.0, 0.0, 777);
        adaptive_bitrate::update_network_stats(std::nullopt, 6.0);
      }
      return adaptive_bitrate::get_state().target_bitrate_kbps;
    }

    /// A second whose client media report never reaches the host, with its ten control pings.
    int quiet_second() {
      stream_stats::age_network_judge_for_tests(std::chrono::seconds(1));
      adaptive_bitrate::age_for_tests(std::chrono::seconds(1));
      for (int ping = 0; ping < 10; ++ping) {
        stream_stats::update_control_channel_stats(6.0, 0.0, 777);
        adaptive_bitrate::update_network_stats(std::nullopt, 6.0);
      }
      return adaptive_bitrate::get_state().target_bitrate_kbps;
    }
  };
}  // namespace

TEST(StreamStatsHotFieldTests, LiveTuningStopsCuttingWithinSecondsOfCleanReports) {
  // Ten seconds that lose 5% of their frames, on a stream that was clean. Fed the window's figure,
  // Live Tuning cut every second the verdict held, which is most of 20 seconds after the loss ended,
  // and ended far below the bitrate it recovered to by the end here. Fed each report's figure while
  // the verdict holds, it waits for Doctor, cuts while the loss lasts, and stops within seconds.
  live_tuning_stream_t stream("owner-live-cuts", 73);
  std::vector<int> target;
  std::vector<bool> elevated;
  const auto run = [&](int seconds, std::uint64_t lost) {
    for (int i = 0; i < seconds; ++i) {
      target.push_back(stream.second(120, lost));
      elevated.push_back(stream_stats::current_network_verdict().loss_elevated);
    }
  };
  run(20, 0);
  const int first_lossy = static_cast<int>(target.size());
  run(10, 6);
  const int first_clean = static_cast<int>(target.size());
  run(40, 0);

  std::string trace;
  int entered = -1;
  int last_cut = -1;
  for (std::size_t i = 0; i < target.size(); ++i) {
    trace += std::to_string(i) + ":" + std::to_string(target[i] / 1000) + (elevated[i] ? "*" : "") + " ";
    if (entered < 0 && elevated[i]) {
      entered = static_cast<int>(i);
    }
    if (i > 0 && target[i] < target[i - 1]) {
      last_cut = static_cast<int>(i);
    }
  }
  RecordProperty("targets", trace);
  // Doctor names the pressure on the eighth lossy report, and Live Tuning cuts for none before it.
  ASSERT_EQ(entered, first_lossy + 7) << trace;
  for (int i = 0; i < entered; ++i) {
    EXPECT_EQ(target[i], k_live_tuning_base_kbps) << trace;
  }
  EXPECT_LT(target[entered], k_live_tuning_base_kbps) << trace;
  // The cuts stop within three seconds of the first clean report, while the verdict still holds.
  EXPECT_LE(last_cut, first_clean + 2) << trace;
  EXPECT_TRUE(elevated[first_clean + 10]) << trace;
  // A few percent for a few seconds costs a moderate step, and the bitrate comes all the way back.
  EXPECT_GT(*std::min_element(target.begin(), target.end()), k_live_tuning_base_kbps / 2) << trace;
  EXPECT_EQ(target.back(), k_live_tuning_base_kbps) << trace;
}

namespace {
  std::string live_tuning_trace(const std::vector<int> &target, std::size_t shown = 128) {
    std::string trace;
    for (std::size_t i = 0; i < target.size(); ++i) {
      if (i < shown || i + 1 == target.size()) {
        trace += std::to_string(i) + ":" + std::to_string(target[i] / 1000) + " ";
      }
    }
    return trace;
  }
}  // namespace

TEST(StreamStatsHotFieldTests, LiveTuningTestsItsHoldOnTheRecordedHevcRunAndClimbsBack) {
  // The Retroid Pocket 6's HEVC run: about 2% of its frames lost in bursts every few seconds, for as
  // long as the run lasted. Doctor calls that pressure and keeps calling it. Heard each report's loss,
  // Live Tuning cut on toward the 2 Mbps floor, and held at half its bitrate it stayed halved for the
  // whole run. Loss that comes and goes at half the bitrate gets no step below, and each step back up
  // that does not raise it is kept.
  live_tuning_stream_t stream("owner-live-hevc", 74);
  std::vector<int> target;
  for (int round = 0; round < 40; ++round) {
    for (const auto &report : k_recorded_hevc_reports) {
      target.push_back(stream.second(report.expected, report.lost));
    }
  }
  const auto trace = live_tuning_trace(target);
  RecordProperty("targets", trace);
  const int half = static_cast<int>(k_live_tuning_base_kbps * 0.5);
  EXPECT_GE(*std::min_element(target.begin(), target.end()), half) << trace;
  const auto back = std::find(target.begin() + 8, target.end(), k_live_tuning_base_kbps);
  ASSERT_NE(back, target.end()) << trace;
  EXPECT_LE(back - target.begin(), 120) << trace;
  EXPECT_TRUE(std::all_of(back, target.end(), [](int kbps) { return kbps == k_live_tuning_base_kbps; })) << trace;
  // The verdict still calls the loss pressure, and Live Tuning holds rather than cut for it.
  EXPECT_TRUE(stream_stats::current_network_verdict().loss_elevated);
  EXPECT_EQ(adaptive_bitrate::get_state().target_bitrate_kbps, k_live_tuning_base_kbps);
}

TEST(StreamStatsHotFieldTests, LiveTuningStepsUnderALinkThatShrankBelowHalfItsBitrate) {
  // 25 clean seconds at 269 Mbps, then the link drops to 100 Mbps, and 5% of the frames are lost
  // whenever the stream is above it. Moderate loss cut to half the bitrate, 134 Mbps, and the hold kept
  // the stream there, a third over the link and losing frames every second, for good. The steady loss
  // at the hold gets a step below, which lowers it, and the steps back up stop under the link.
  live_tuning_stream_t stream("owner-live-link", 75);
  constexpr int link_kbps = 100000;
  std::vector<int> target;
  int lossy_last_minute = 0;
  for (int second = 0; second < 240; ++second) {
    const bool lossy = second >= 25 && adaptive_bitrate::get_state().target_bitrate_kbps > link_kbps;
    target.push_back(stream.second(120, lossy ? 6 : 0));
    lossy_last_minute += second >= 180 && lossy ? 1 : 0;
  }
  const auto trace = live_tuning_trace(target, 240);
  RecordProperty("targets", trace);
  const auto under = std::find_if(target.begin() + 25, target.end(), [](int kbps) { return kbps <= link_kbps; });
  ASSERT_NE(under, target.end()) << trace;
  EXPECT_LE(under - target.begin(), 60) << trace;
  // One step below the hold, and no spiral under it.
  EXPECT_GE(*std::min_element(target.begin(), target.end()), k_live_tuning_base_kbps / 4) << trace;
  EXPECT_LE(lossy_last_minute, 10) << trace;
  EXPECT_LE(target.back(), link_kbps) << trace;
  EXPECT_GT(target.back(), link_kbps * 9 / 10) << trace;
}

TEST(StreamStatsHotFieldTests, LiveTuningStopsCuttingWhenReportsStopMidSession) {
  // The client's media reports stop reaching the host mid-session: it stops posting, the screen has no
  // new frames, or its reports come too far apart to count. Live Tuning's loss average stayed where the
  // last report put it and every ping cut on it: 10 seconds at 5% left a stream at half its bitrate for
  // good, and a 3 second burst at 20% took one to the 2 Mbps floor, while Doctor called the same loss
  // stale. Live Tuning stops cutting for it with the last report and comes all the way back.
  struct run_t {
    int lossy_seconds;
    std::uint64_t lost;
  };
  for (const auto run : {run_t {10, 6}, run_t {3, 24}}) {
    live_tuning_stream_t stream("owner-live-quiet-" + std::to_string(run.lost), 80 + run.lost);
    std::vector<int> target;
    for (int second = 0; second < 20; ++second) {
      target.push_back(stream.second(120, 0));
    }
    for (int second = 0; second < run.lossy_seconds; ++second) {
      target.push_back(stream.second(120, run.lost));
    }
    const std::size_t last_report = target.size() - 1;
    for (int second = 0; second < 60; ++second) {
      target.push_back(stream.quiet_second());
    }
    const auto trace = live_tuning_trace(target);
    RecordProperty("targets_" + std::to_string(run.lost), trace);
    ASSERT_LT(target[last_report], k_live_tuning_base_kbps) << trace;
    EXPECT_GE(*std::min_element(target.begin() + last_report, target.end()), target[last_report]) << trace;
    EXPECT_EQ(target.back(), k_live_tuning_base_kbps) << trace;
  }
}

TEST(StreamStatsHotFieldTests, LiveTuningStopsCuttingWithinSecondsOfABurst) {
  // Three seconds that lose a fifth of their frames, on a clean stream. Live Tuning cuts while the
  // verdict holds the loss, stops within seconds of clean reports, and comes all the way back.
  live_tuning_stream_t stream("owner-live-burst", 76);
  std::vector<int> target;
  for (int second = 0; second < 63; ++second) {
    target.push_back(stream.second(120, second >= 20 && second < 23 ? 24 : 0));
  }
  const auto trace = live_tuning_trace(target);
  RecordProperty("targets", trace);
  int last_cut = -1;
  for (std::size_t i = 1; i < target.size(); ++i) {
    if (target[i] < target[i - 1]) {
      last_cut = static_cast<int>(i);
    }
  }
  constexpr int first_clean = 23;
  ASSERT_GE(last_cut, 20) << trace;
  EXPECT_LE(last_cut, first_clean + 2) << trace;
  EXPECT_GE(*std::min_element(target.begin(), target.end()), k_live_tuning_base_kbps / 2) << trace;
  EXPECT_EQ(target.back(), k_live_tuning_base_kbps) << trace;
}

TEST(StreamStatsDoctorTests, RecordedHevcRunKeepsOneHeadline) {
  // The HEVC run's reports, a second at a time, with ten control pings between reports as Nova and
  // ENet send them, the control channel's own loss at the 7.75% the run read. Graded a report at a
  // time, Doctor went from "Control-channel retries" to "Sustained network pressure" and back twice
  // every eight seconds. Graded over the window it names the pressure once the window holds enough of
  // it and keeps it.
  adaptive_bitrate::set_enabled(false);
  stream_stats::update_stream_active(false);
  stream_stats::update_controller_input_state(false, 0, "", "", "unknown", "", false, "");
  stream_stats::update_steam_input_state("unknown", 0, 0, 0, "");
  stream_stats::start_session_timing("owner-hevc-run", 71, "app-session-hevc-run");
  stream_stats::update_stream_active(true, "RetroidPocket6", "203.0.113.80");
  const auto cleanup = util::fail_guard([] {
    stream_stats::stop_session_timing("owner-hevc-run", 71);
    stream_stats::update_stream_active(false);
  });
  stream_stats::update_video_stats(120.0, 268988, 2.0, "hevc", 3840, 2160);
  stream_stats::update_session_targets(
    120.0, 120.0, 120.0, "client_requested", "deterministic_preset_v1",
    "deterministic", "not_applicable", "Capability-validated launch profile.",
    "", 1, 268988, 268988
  );

  stream_stats::client_media_counters_t sample {
    .owner_uuid = "owner-hevc-run",
    .app_session_id = "app-session-hevc-run",
    .session_generation = 71,
    .client_monotonic_ms = 1'000,
    .frames_expected = 0,
    .frames_received = 0,
    .frames_lost = 0
  };
  ASSERT_TRUE(stream_stats::ingest_client_media_counters(sample).accepted);
  headline_run_t run;
  for (int round = 0; round < 3; ++round) {
    for (const auto &report : k_recorded_hevc_reports) {
      sample.client_monotonic_ms += 1'000;
      sample.frames_expected += report.expected;
      sample.frames_lost += report.lost;
      sample.frames_received = sample.frames_expected - sample.frames_lost;
      ASSERT_TRUE(stream_stats::ingest_client_media_counters(sample).observation_published);
      for (const auto rtt_ms : k_recorded_hevc_rtt_ms) {
        stream_stats::update_control_channel_stats(rtt_ms, 7.75, 777);
      }
      run.headlines.push_back(live_headline());
    }
  }

  RecordProperty("headlines", run.text());
  EXPECT_LE(run.changes(), 1) << run.text();
  EXPECT_EQ(run.headlines.back(), "network_jitter") << run.text();
}

TEST(StreamStatsDoctorTests, RecordedControlChannelLossKeepsOneHeadline) {
  // The HEVC run's control-channel estimate, ten pings a second, alternating between the 1.08 and 2.81
  // its polls read, on a stream whose client sends no media reports. Taken a reading at a time the
  // headline went between "none" and the control channel finding with every poll.
  // A controller an earlier test left holding a cut target would put a quality finding above it.
  adaptive_bitrate::reset();
  adaptive_bitrate::set_enabled(false);
  stream_stats::update_stream_active(false);
  stream_stats::update_controller_input_state(false, 0, "", "", "unknown", "", false, "");
  stream_stats::update_steam_input_state("unknown", 0, 0, 0, "");
  stream_stats::update_stream_active(true, "RetroidPocket6", "203.0.113.82");
  const auto cleanup = util::fail_guard([] {
    stream_stats::update_stream_active(false);
  });
  stream_stats::update_video_stats(60.0, 20000, 2.0, "hevc", 1920, 1080);
  stream_stats::update_session_targets(
    60.0, 60.0, 60.0, "client_requested", "deterministic_preset_v1",
    "deterministic", "not_applicable", "Capability-validated launch profile.",
    "", 1, 20000, 20000
  );

  headline_run_t run;
  for (int second = 0; second < 5; ++second) {
    for (int ping = 0; ping < 10; ++ping) {
      stream_stats::update_control_channel_stats(6.0, 2.81, 777);
    }
    run.headlines.push_back(live_headline());
  }
  for (int second = 0; second < 25; ++second) {
    for (int ping = 0; ping < 10; ++ping) {
      stream_stats::update_control_channel_stats(6.0, second % 2 == 0 ? 1.08 : 2.81, 777);
    }
    run.headlines.push_back(live_headline());
  }

  RecordProperty("headlines", run.text());
  EXPECT_EQ(run.changes(), 0) << run.text();
  EXPECT_EQ(run.headlines.back(), "control_channel_observation") << run.text();
}

TEST(StreamStatsDoctorTests, RecordedPyroWaveRttSpikesKeepOneHeadline) {
  // The PyroWave run's RTT, ten control pings a second, on a 1080p60 PyroWave stream well below its
  // advice. Graded a reading at a time, its Wi-Fi spikes turned "set more bitrate on a clean network"
  // into "a network warning needs more evidence" and back. Their median over the window stays a LAN's.
  PyroWaveHostGuard host;
  stream_stats::update_stream_active(false);
  config::video.adaptive_bitrate.enabled = false;
  adaptive_bitrate::set_runtime_update_supported(true, {}, 20000);
  const auto cleanup = util::fail_guard([] {
    adaptive_bitrate::set_enabled(false);
    stream_stats::update_stream_active(false);
  });
  stream_stats::update_controller_input_state(false, 0, "", "", "unknown", "", false, "");
  stream_stats::update_steam_input_state("unknown", 0, 0, 0, "");
  stream_stats::update_stream_active(true, "RetroidPocket6", "203.0.113.81");
  stream_stats::update_video_stats(60.0, 20000, 1.0, "pyrowave", 1920, 1080);
  stream_stats::update_session_targets(
    60.0, 60.0, 60.0, "client_requested", "deterministic_preset_v1",
    "deterministic", "not_applicable", "Capability-validated launch profile.",
    "", 1, 0, 20000
  );

  headline_run_t run;
  for (int round = 0; round < 3; ++round) {
    for (const auto rtt_ms : k_recorded_pyrowave_rtt_ms) {
      for (int ping = 0; ping < 10; ++ping) {
        stream_stats::update_control_channel_stats(rtt_ms, 0.0, 777);
      }
      run.headlines.push_back(live_headline());
    }
  }

  RecordProperty("headlines", run.text());
  EXPECT_EQ(run.changes(), 0) << run.text();
  EXPECT_EQ(run.headlines.back(), "pyrowave_starved") << run.text();
}

namespace {
  /// Doctor's headline for the live stream with Live Tuning owning it and holding it at target_kbps.
  std::string live_headline_under_live_tuning(int target_kbps) {
    auto live = stream_stats::get_current();
    live.capture_transport = platf::frame_transport_e::dmabuf;
    live.capture_residency = platf::frame_residency_e::gpu;
    live.encode_target_residency = platf::frame_residency_e::gpu;
    live.adaptive_bitrate_enabled = true;
    live.adaptive_runtime_update_supported = true;
    live.adaptive_target_bitrate_kbps = target_kbps;
    return stream_stats::build_doctor_json(live, nlohmann::json::object()).at("primary_issue").get<std::string>();
  }

  // Live Tuning's targets through the PyroWave run, a second at a time beside its RTT: 269 Mbps, cut
  // to 179 for the Wi-Fi spikes, then back through 188 and 194.
  constexpr std::array<int, 10> k_recorded_pyrowave_live_targets_kbps {
    268988, 268988, 179000, 179000, 179000, 188000, 188000, 194000, 194000, 194000
  };
}  // namespace

TEST(StreamStatsDoctorTests, PyroWaveAdviceHoldsWhileLiveTuningMovesTheBitrate) {
  // The PyroWave run had Live Tuning on, and Doctor judged PyroWave's bitrate advice on its moving
  // target. A 1080p120 stream set at 170 Mbps sits between the starved line, 160 Mbps at the encoder,
  // and the 178 Mbps Doctor would raise it to, so its quality restore does not cover it. Cut in the
  // run's proportions, from 269 to 179 and back through 188 and 194, the headline went from nothing to
  // "set more bitrate" and back with every cut, while Live Tuning was about to bring the bitrate back
  // on its own. Doctor judges the rate the stream is set to.
  PyroWaveHostGuard host;
  constexpr int set_kbps = 170000;
  stream_stats::update_stream_active(false);
  const auto cleanup = util::fail_guard([] {
    stream_stats::update_stream_active(false);
  });
  stream_stats::update_controller_input_state(false, 0, "", "", "unknown", "", false, "");
  stream_stats::update_steam_input_state("unknown", 0, 0, 0, "");
  stream_stats::update_stream_active(true, "RetroidPocket6", "203.0.113.84");
  stream_stats::update_video_stats(120.0, set_kbps, 1.0, "pyrowave", 1920, 1080);
  stream_stats::update_session_targets(
    120.0, 120.0, 120.0, "client_requested", "deterministic_preset_v1",
    "deterministic", "not_applicable", "Capability-validated launch profile.",
    "", 1, 0, set_kbps
  );

  headline_run_t run;
  int live_target_flips = 0;
  bool live_target_was_starved = false;
  for (int round = 0; round < 3; ++round) {
    for (std::size_t second = 0; second < k_recorded_pyrowave_rtt_ms.size(); ++second) {
      for (int ping = 0; ping < 10; ++ping) {
        stream_stats::update_control_channel_stats(k_recorded_pyrowave_rtt_ms[second], 0.0, 777);
      }
      const int target = static_cast<int>(
        static_cast<std::int64_t>(set_kbps) * k_recorded_pyrowave_live_targets_kbps[second] / 268988
      );
      run.headlines.push_back(live_headline_under_live_tuning(target));
      // What judging the live target said.
      auto live = stream_stats::get_current();
      live.adaptive_runtime_update_supported = true;
      live.adaptive_target_bitrate_kbps = target;
      const bool starved = stream_stats::evaluate_pyrowave_bitrate(live).starved;
      live_target_flips += starved != live_target_was_starved ? 1 : 0;
      live_target_was_starved = starved;
    }
  }

  RecordProperty("headlines", run.text());
  EXPECT_GE(live_target_flips, 5);
  EXPECT_EQ(run.changes(), 0) << run.text();
  EXPECT_EQ(run.headlines.back(), "none") << run.text();

  // A stream set below the line stays starved whichever way Live Tuning moves it.
  stream_stats::update_video_stats(120.0, 120000, 1.0, "pyrowave", 1920, 1080);
  stream_stats::update_session_targets(
    120.0, 120.0, 120.0, "client_requested", "deterministic_preset_v1",
    "deterministic", "not_applicable", "Capability-validated launch profile.",
    "", 1, 0, 120000
  );
  for (const int target : {120000, 80000, 84000, 120000}) {
    EXPECT_EQ(live_headline_under_live_tuning(target), "pyrowave_starved") << target;
  }
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

#ifdef __linux__
TEST(StreamStatsDoctorTests, ForecastStaysSilentUntilCaptureSourcesWereEvaluated) {
  // Nothing has been looked at: no backend, no forecast, no finding. The Doctor must never
  // report a problem it has not looked for.
  LinuxDisplayConfigGuard guard;
  config::video.encoder = "nvenc";
  stream_stats::set_build_has_cuda_for_tests(false);

  stream_stats::stats_t stats {};
  const auto profile = stream_stats::linux_gpu_profile_json(stats);

  EXPECT_EQ(profile.at("capture_forecast").at("backend"), "unevaluated");
  EXPECT_EQ(profile.at("capture_forecast").at("residency"), "unknown");
  EXPECT_EQ(profile.at("capture_forecast").at("build_has_cuda"), false);
  for (const auto &warning : profile.at("configuration_warnings")) {
    EXPECT_NE(warning.at("id"), "capture_copies_through_system_memory");
  }
}

TEST(StreamStatsDoctorTests, NamesTheBuildWhenNvidiaCaptureCannotStayOnTheGpuWithoutCuda) {
  // An NVIDIA host on a CUDA-less package copies every frame through system memory whatever
  // the mode. The GPU-native advice is a dead end there, so it has to give way to the build.
  LinuxDisplayConfigGuard guard;
  config::video.encoder = "nvenc";
  config::video.linux_display.headless_mode = true;
  config::video.linux_display.use_cage_compositor = true;
  config::video.linux_display.prefer_gpu_native_capture = false;
  platf::set_selected_capture_backend_for_tests("wlr");
  stream_stats::set_build_has_cuda_for_tests(false);

  stream_stats::stats_t stats {};
  const auto doctor = stream_stats::build_doctor_json(stats, {{"primary_issue", "steady"}, {"grade", "good"}});
  const auto &profile = doctor.at("advanced_evidence").at("linux_gpu_profile");

  bool saw_forecast = false;
  for (const auto &warning : profile.at("configuration_warnings")) {
    const auto id = warning.at("id").get<std::string>();
    EXPECT_NE(id, "nvidia_headless_gpu_native_disabled");
    if (id != "capture_copies_through_system_memory") {
      continue;
    }
    saw_forecast = true;
    EXPECT_EQ(warning.at("cause"), "build_without_cuda");
    EXPECT_EQ(warning.at("severity"), "warning");
    EXPECT_NE(warning.at("message").get<std::string>().find("cuda=disabled"), std::string::npos);
    EXPECT_NE(warning.at("action").get<std::string>().find("-DPOLARIS_ENABLE_CUDA=ON"), std::string::npos);
  }
  EXPECT_TRUE(saw_forecast);
  EXPECT_EQ(profile.at("capture_forecast").at("residency"), "system_memory");
  EXPECT_EQ(profile.at("capture_forecast").at("cause"), "build_without_cuda");
  EXPECT_EQ(profile.at("capture_forecast").at("build_has_cuda"), false);
  EXPECT_EQ(profile.at("capture_forecast").at("backend"), "wlr");
}

TEST(StreamStatsDoctorTests, KeepsTheGpuNativeAdviceWhenTheBuildHasCuda) {
  LinuxDisplayConfigGuard guard;
  config::video.encoder = "nvenc";
  config::video.linux_display.headless_mode = true;
  config::video.linux_display.use_cage_compositor = true;
  config::video.linux_display.prefer_gpu_native_capture = false;
  platf::set_selected_capture_backend_for_tests("wlr");
  stream_stats::set_build_has_cuda_for_tests(true);

  stream_stats::stats_t stats {};
  const auto profile = stream_stats::linux_gpu_profile_json(stats);

  bool saw_gpu_native_advice = false;
  for (const auto &warning : profile.at("configuration_warnings")) {
    const auto id = warning.at("id").get<std::string>();
    saw_gpu_native_advice = saw_gpu_native_advice || id == "nvidia_headless_gpu_native_disabled";
    // No probe has run on this host yet, so the forecast has nothing to warn about.
    EXPECT_NE(id, "capture_copies_through_system_memory");
  }
  EXPECT_TRUE(saw_gpu_native_advice);
  EXPECT_EQ(profile.at("capture_forecast").at("residency"), "unknown");
  EXPECT_EQ(profile.at("capture_forecast").at("build_has_cuda"), true);
}

TEST(StreamStatsDoctorTests, SaysX11CaptureCopiesThroughSystemMemory) {
  LinuxDisplayConfigGuard guard;
  config::video.encoder = "nvenc";
  config::video.linux_display.use_cage_compositor = false;
  platf::set_selected_capture_backend_for_tests("x11");
  stream_stats::set_build_has_cuda_for_tests(true);

  stream_stats::stats_t stats {};
  const auto profile = stream_stats::linux_gpu_profile_json(stats);

  bool saw_forecast = false;
  for (const auto &warning : profile.at("configuration_warnings")) {
    if (warning.at("id") != "capture_copies_through_system_memory") {
      continue;
    }
    saw_forecast = true;
    EXPECT_EQ(warning.at("cause"), "x11_capture");
    EXPECT_NE(warning.at("message").get<std::string>().find("x11grab"), std::string::npos);
    EXPECT_NE(warning.at("action").get<std::string>().find("Wayland"), std::string::npos);
  }
  EXPECT_TRUE(saw_forecast);
}

TEST(StreamStatsDoctorTests, SaysVulkanVideoOnThePortalCopiesThroughSystemMemoryByPolicy) {
  // #635: forecast_capture_path answers for Vulkan Video on the portal. This holds the route that
  // carries the answer to Doctor, and the POLARIS_PORTAL_DMABUF read that adds the sentence saying
  // the variable is VA-API's.
  LinuxDisplayConfigGuard guard;
  config::video.encoder = "vulkan";
  config::video.linux_display.use_cage_compositor = false;
  platf::set_selected_capture_backend_for_tests("portal");
  stream_stats::set_build_has_cuda_for_tests(false);

  const char *previous = std::getenv("POLARIS_PORTAL_DMABUF");
  const std::optional<std::string> saved = previous ? std::optional<std::string> {previous} : std::nullopt;
  auto restore = util::fail_guard([&saved] {
    if (saved) {
      setenv("POLARIS_PORTAL_DMABUF", saved->c_str(), 1);
    } else {
      unsetenv("POLARIS_PORTAL_DMABUF");
    }
  });
  unsetenv("POLARIS_PORTAL_DMABUF");

  const auto forecast_warning = [] {
    stream_stats::stats_t stats {};
    const auto profile = stream_stats::linux_gpu_profile_json(stats);
    EXPECT_EQ(profile.at("capture_forecast").at("backend"), "portal");
    EXPECT_EQ(profile.at("capture_forecast").at("residency"), "system_memory");
    EXPECT_EQ(profile.at("capture_forecast").at("cause"), "vulkan_portal_system_memory_by_design");
    nlohmann::json found;
    for (const auto &warning : profile.at("configuration_warnings")) {
      if (warning.at("id") == "capture_copies_through_system_memory") {
        EXPECT_TRUE(found.is_null()) << "one forecast warning per profile";
        found = warning;
      }
    }
    return found;
  };

  auto warning = forecast_warning();
  ASSERT_FALSE(warning.is_null());
  EXPECT_EQ(warning.at("cause"), "vulkan_portal_system_memory_by_design");
  EXPECT_EQ(warning.at("severity"), "info");
  EXPECT_NE(warning.at("action").get<std::string>().find("Private Stream"), std::string::npos);
  EXPECT_EQ(warning.at("action").get<std::string>().find("POLARIS_PORTAL_DMABUF"), std::string::npos);

  setenv("POLARIS_PORTAL_DMABUF", "1", 1);
  warning = forecast_warning();
  ASSERT_FALSE(warning.is_null());
  EXPECT_EQ(warning.at("cause"), "vulkan_portal_system_memory_by_design");
  EXPECT_NE(
    warning.at("action").get<std::string>().find("POLARIS_PORTAL_DMABUF=1 applies to VA-API only"),
    std::string::npos
  );
}
#endif

TEST(CaptureForecastTests, VaapiStaysInSystemMemoryByDesignOnEveryPath) {
  stream_stats::capture_forecast_inputs_t inputs;
  inputs.encoder = "vaapi";
  inputs.build_has_cuda = false;

  inputs.capture_backend = "wlr";
  inputs.use_cage_compositor = true;
  auto forecast = stream_stats::forecast_capture_path(inputs);
  EXPECT_EQ(forecast.residency, "system_memory");
  EXPECT_EQ(forecast.cause, "vaapi_system_memory_by_design");
  EXPECT_EQ(forecast.severity, "info");
  EXPECT_NE(forecast.message.find("Private Stream"), std::string::npos);
  EXPECT_EQ(forecast.action.find("POLARIS_PORTAL_DMABUF"), std::string::npos);

  inputs.capture_backend = "portal";
  inputs.use_cage_compositor = false;
  forecast = stream_stats::forecast_capture_path(inputs);
  EXPECT_EQ(forecast.cause, "vaapi_system_memory_by_design");
  EXPECT_NE(forecast.message.find("desktop portal"), std::string::npos);
  EXPECT_NE(forecast.action.find("POLARIS_PORTAL_DMABUF=1"), std::string::npos);

  // The operator opted into the unvalidated DMA-BUF path; the forecast no longer knows.
  inputs.portal_vaapi_dmabuf_opted_in = true;
  forecast = stream_stats::forecast_capture_path(inputs);
  EXPECT_EQ(forecast.residency, "unknown");
  EXPECT_TRUE(forecast.cause.empty());

  // KMS imports the framebuffer into VA-API directly.
  inputs.capture_backend = "kms";
  forecast = stream_stats::forecast_capture_path(inputs);
  EXPECT_EQ(forecast.residency, "gpu");
  EXPECT_TRUE(forecast.cause.empty());
}

TEST(CaptureForecastTests, VulkanVideoOnThePortalStaysInSystemMemoryByDesign) {
  // #635: the forecast said the portal is asked for DMA-BUF on Vulkan Video and waited for a
  // stream to show which. The portal hands Vulkan Video shared memory whatever it is asked, so
  // there was never anything to wait for.
  stream_stats::capture_forecast_inputs_t inputs;
  inputs.encoder = "vulkan";
  inputs.build_has_cuda = false;
  inputs.capture_backend = "portal";

  auto forecast = stream_stats::forecast_capture_path(inputs);
  EXPECT_EQ(forecast.residency, "system_memory");
  EXPECT_EQ(forecast.cause, "vulkan_portal_system_memory_by_design");
  EXPECT_EQ(forecast.severity, "info");
  EXPECT_NE(forecast.message.find("Gamescope Stream"), std::string::npos) << forecast.message;
  EXPECT_NE(forecast.message.find("by design"), std::string::npos) << forecast.message;
  EXPECT_NE(forecast.message.find("not a fault"), std::string::npos) << forecast.message;
  EXPECT_NE(forecast.action.find("Private Stream"), std::string::npos) << forecast.action;
  EXPECT_EQ(forecast.action.find("POLARIS_PORTAL_DMABUF"), std::string::npos) << forecast.action;

  // The VA-API opt-in does not reach Vulkan Video, so the forecast keeps its answer and says why
  // the variable made no difference, instead of going quiet the way it does for VA-API.
  inputs.portal_vaapi_dmabuf_opted_in = true;
  forecast = stream_stats::forecast_capture_path(inputs);
  EXPECT_EQ(forecast.residency, "system_memory");
  EXPECT_EQ(forecast.cause, "vulkan_portal_system_memory_by_design");
  EXPECT_NE(forecast.action.find("POLARIS_PORTAL_DMABUF=1 applies to VA-API only"), std::string::npos) << forecast.action;

  // Other backends keep their own answers for Vulkan Video.
  inputs.capture_backend = "wlr";
  forecast = stream_stats::forecast_capture_path(inputs);
  EXPECT_EQ(forecast.residency, "gpu");
  EXPECT_TRUE(forecast.cause.empty());
  inputs.capture_backend = "kms";
  forecast = stream_stats::forecast_capture_path(inputs);
  EXPECT_EQ(forecast.residency, "gpu");
  EXPECT_TRUE(forecast.cause.empty());

  // CUDA on the portal is offered DMA-BUF and still waits for a stream to show which it got.
  inputs.encoder = "nvenc";
  inputs.build_has_cuda = true;
  inputs.capture_backend = "portal";
  forecast = stream_stats::forecast_capture_path(inputs);
  EXPECT_EQ(forecast.residency, "unknown");
  EXPECT_TRUE(forecast.cause.empty());
}

TEST(CaptureForecastTests, EveryCauseHasARowInTroubleshooting) {
  // Doctor reports capture_copies_through_system_memory with a cause, and Troubleshooting promises
  // a row with the fix for each one. vulkan_portal_system_memory_by_design shipped without its row,
  // so this reads every cause the forecast can give and looks for it in the table.
  const auto read = [](const char *relative) {
    std::ifstream input(std::filesystem::path {POLARIS_SOURCE_DIR} / relative);
    EXPECT_TRUE(input.good()) << relative;
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
  };
  const auto source = read("src/stream_stats.cpp");
  const auto docs = read("docs/troubleshooting.md");

  const std::string call = "system_memory(\n";
  std::vector<std::string> causes;
  for (auto at = source.find(call); at != std::string::npos; at = source.find(call, at + call.size())) {
    const auto open = source.find_first_not_of(" \n", at + call.size());
    ASSERT_NE(open, std::string::npos);
    ASSERT_EQ(source[open], '"') << source.substr(at, 80);
    const auto close = source.find('"', open + 1);
    ASSERT_NE(close, std::string::npos);
    causes.push_back(source.substr(open + 1, close - open - 1));
  }
  EXPECT_GE(causes.size(), 6u) << "the forecast lost its causes, or this test lost track of them";
  for (const auto &cause : causes) {
    EXPECT_NE(docs.find("| `" + cause + "` |"), std::string::npos) << cause;
  }
}

TEST(CaptureForecastTests, NvidiaPrivateStreamFollowsTheLastDmabufProbe) {
  stream_stats::capture_forecast_inputs_t inputs;
  inputs.capture_backend = "wlr";
  inputs.encoder = "nvenc";
  inputs.build_has_cuda = true;
  inputs.use_cage_compositor = true;
  inputs.headless_mode = true;
  inputs.prefer_gpu_native_capture = false;

  // Hidden headless, no probe yet: the first launch decides.
  auto forecast = stream_stats::forecast_capture_path(inputs);
  EXPECT_EQ(forecast.residency, "unknown");
  EXPECT_TRUE(forecast.cause.empty());

  inputs.headless_extcopy_dmabuf_probe = true;
  forecast = stream_stats::forecast_capture_path(inputs);
  EXPECT_EQ(forecast.residency, "gpu");

  inputs.headless_extcopy_dmabuf_probe = false;
  forecast = stream_stats::forecast_capture_path(inputs);
  EXPECT_EQ(forecast.residency, "system_memory");
  EXPECT_EQ(forecast.cause, "headless_dmabuf_unavailable");
  EXPECT_EQ(forecast.severity, "warning");
  EXPECT_NE(forecast.action.find("Private Stream (GPU-native)"), std::string::npos);
  EXPECT_NE(forecast.action.find("linux_prefer_gpu_native_capture = enabled"), std::string::npos);
  // Moonlight reads this advice too and has no Play Setup, so it names where every client gets the mode.
  EXPECT_EQ(forecast.action.find("Play Setup"), std::string::npos) << forecast.action;
  EXPECT_NE(forecast.action.find("Settings, Audio/Video, Where games run"), std::string::npos) << forecast.action;
  EXPECT_NE(forecast.action.find("Nova can choose it for one launch"), std::string::npos) << forecast.action;

  // With the preference on, the private compositor runs windowed and the other probe rules.
  inputs.prefer_gpu_native_capture = true;
  forecast = stream_stats::forecast_capture_path(inputs);
  EXPECT_EQ(forecast.residency, "unknown");
  inputs.windowed_gpu_native_probe = false;
  forecast = stream_stats::forecast_capture_path(inputs);
  EXPECT_EQ(forecast.cause, "windowed_dmabuf_unavailable");
  inputs.windowed_gpu_native_probe = true;
  forecast = stream_stats::forecast_capture_path(inputs);
  EXPECT_EQ(forecast.residency, "gpu");
  EXPECT_TRUE(forecast.cause.empty());

  // A wlroots desktop captured directly is the compositor's call; nothing to forecast.
  inputs.use_cage_compositor = false;
  forecast = stream_stats::forecast_capture_path(inputs);
  EXPECT_EQ(forecast.residency, "unknown");
}

TEST(CaptureForecastTests, TheBuildOutranksEveryOtherNvidiaCause) {
  stream_stats::capture_forecast_inputs_t inputs;
  inputs.encoder = "nvenc";
  inputs.build_has_cuda = false;
  inputs.use_cage_compositor = true;
  inputs.headless_mode = true;
  inputs.headless_extcopy_dmabuf_probe = true;  // a GPU-native probe that once passed changes nothing

  for (const auto *backend : {"wlr", "kms", "portal"}) {
    inputs.capture_backend = backend;
    const auto forecast = stream_stats::forecast_capture_path(inputs);
    EXPECT_EQ(forecast.cause, "build_without_cuda") << backend;
    EXPECT_EQ(forecast.residency, "system_memory") << backend;
  }

  // X11 is system memory with or without CUDA, so it keeps its own cause.
  inputs.capture_backend = "x11";
  EXPECT_EQ(stream_stats::forecast_capture_path(inputs).cause, "x11_capture");

  // NvFBC is only compiled with CUDA and stays on the GPU.
  inputs.build_has_cuda = true;
  inputs.capture_backend = "nvfbc";
  EXPECT_EQ(stream_stats::forecast_capture_path(inputs).residency, "gpu");
}

TEST(CaptureForecastTests, HasNothingToSayWithoutABackendOrWithASoftwareEncoder) {
  stream_stats::capture_forecast_inputs_t inputs;
  inputs.encoder = "nvenc";
  inputs.build_has_cuda = false;

  for (const auto *backend : {"", "none"}) {
    inputs.capture_backend = backend;
    const auto forecast = stream_stats::forecast_capture_path(inputs);
    EXPECT_EQ(forecast.residency, "unknown") << "'" << backend << "'";
    EXPECT_TRUE(forecast.cause.empty()) << "'" << backend << "'";
  }

  inputs.capture_backend = "wlr";
  inputs.encoder = "software";
  const auto forecast = stream_stats::forecast_capture_path(inputs);
  EXPECT_EQ(forecast.residency, "system_memory");
  EXPECT_TRUE(forecast.cause.empty());

  const auto json = stream_stats::capture_forecast_json(inputs, forecast);
  EXPECT_EQ(json.at("backend"), "wlr");
  EXPECT_EQ(json.at("encoder"), "software");
  EXPECT_EQ(json.at("build_has_cuda"), false);
  EXPECT_EQ(json.at("residency"), "system_memory");
  EXPECT_EQ(json.at("cause"), "");
}

#ifdef __linux__
TEST(StreamStatsDoctorTests, NamesTheBinaryThatProducedTheReport) {
  // A support thread needs the running binary on its first line: the Bazzite
  // KMS recipe runs a copy outside the package that updates never touch.
  stream_stats::stats_t stats {};
  const auto doctor = stream_stats::build_doctor_json(stats, {{"primary_issue", "steady"}, {"grade", "good"}});

  const auto running = platf::user_unit::running_executable();
  ASSERT_TRUE(running.has_value());
  const auto binary = platf::user_unit::describe_running_binary(*running, POLARIS_EXECUTABLE_PATH);
  const bool outside_package = binary.matches_package == std::optional<bool> {false};

  bool saw_row = false;
  for (const auto &entry : doctor.at("evidence")) {
    if (entry.at("id") != "running_binary") {
      continue;
    }
    saw_row = true;
    EXPECT_EQ(entry.at("value"), binary.path);
    EXPECT_EQ(entry.at("status"), outside_package ? "watch" : "pass");
    const auto detail = entry.at("detail").get<std::string>();
    EXPECT_NE(detail.find(binary.path), std::string::npos);
    EXPECT_NE(detail.find(PROJECT_VERSION), std::string::npos);
    EXPECT_EQ(detail.find("outside") != std::string::npos || detail.find("not the packaged") != std::string::npos, outside_package);
  }
  EXPECT_TRUE(saw_row);
}
#endif

#ifdef __linux__
TEST(StreamStatsLinuxGpuProfileTests, NamesTheHybridSplitWhenPolarisChoseTheEncoderNodeItself) {
  // adapter_name unset, the desktop renders on one node and Polaris auto-picked another:
  // the reporter's laptop. Before this the Doctor called the pairing "unknown".
  LinuxDisplayConfigGuard guard;
  TempFileGuard igpu_node("hybrid-igpu");
  TempFileGuard dgpu_node("hybrid-dgpu");
  config::video.adapter_name.clear();
  platf::set_effective_encoder_render_device_for_tests(dgpu_node.string());

  stream_stats::stats_t stats {};
  stats.wayland_main_device = igpu_node.string();

  const auto profile = stream_stats::linux_gpu_profile_json(stats);

  EXPECT_EQ(profile.at("encoder_adapter"), "");
  EXPECT_EQ(profile.at("encoder_adapter_effective"), dgpu_node.string());
  EXPECT_EQ(profile.at("encoder_adapter_source"), "auto");
  EXPECT_EQ(profile.at("compositor_render_device"), igpu_node.string());
  EXPECT_EQ(profile.at("adapter_pairing_status"), "mismatched");
  EXPECT_EQ(profile.at("adapter_pairing_device_source"), "wayland_main_device");

  const auto &warnings = profile.at("configuration_warnings");
  const auto mismatch = std::find_if(warnings.begin(), warnings.end(), [](const auto &warning) {
    return warning.value("id", std::string {}) == "linux_gpu_adapter_mismatch";
  });
  ASSERT_NE(mismatch, warnings.end());
  const auto message = mismatch->at("message").get<std::string>();
  const auto action = mismatch->at("action").get<std::string>();
  EXPECT_NE(message.find("Polaris chose " + dgpu_node.string()), std::string::npos);
  EXPECT_NE(message.find(igpu_node.string()), std::string::npos);
  EXPECT_NE(message.find("whole-machine freezes"), std::string::npos);
  EXPECT_NE(action.find("adapter_name = " + igpu_node.string()), std::string::npos);
  EXPECT_NE(action.find("NVreg_DynamicPowerManagement=0x00"), std::string::npos);
}

TEST(StreamStatsLinuxGpuProfileTests, StaysUnknownWhenNeitherSideNamesANode) {
  // The test build's automatic choice is pinned to empty, so a fixture never sees the
  // host's real render nodes; with no compositor device either, nothing is compared.
  LinuxDisplayConfigGuard guard;
  config::video.adapter_name.clear();

  stream_stats::stats_t stats {};
  const auto profile = stream_stats::linux_gpu_profile_json(stats);

  EXPECT_EQ(profile.at("encoder_adapter_effective"), "");
  EXPECT_EQ(profile.at("encoder_adapter_source"), "auto");
  EXPECT_EQ(profile.at("adapter_pairing_status"), "unknown");
  for (const auto &warning : profile.at("configuration_warnings")) {
    EXPECT_NE(warning.at("id"), "linux_gpu_adapter_mismatch");
  }
}

TEST(StreamStatsLinuxGpuProfileTests, AConfiguredAdapterKeepsTheOriginalMismatchText) {
  LinuxDisplayConfigGuard guard;
  TempFileGuard adapter_node("configured-adapter");
  TempFileGuard wayland_node("configured-wayland");
  config::video.adapter_name = adapter_node.string();

  stream_stats::stats_t stats {};
  stats.wayland_main_device = wayland_node.string();

  const auto profile = stream_stats::linux_gpu_profile_json(stats);
  EXPECT_EQ(profile.at("encoder_adapter_source"), "configured");
  EXPECT_EQ(profile.at("encoder_adapter_effective"), adapter_node.string());
  const auto &warnings = profile.at("configuration_warnings");
  const auto mismatch = std::find_if(warnings.begin(), warnings.end(), [](const auto &warning) {
    return warning.value("id", std::string {}) == "linux_gpu_adapter_mismatch";
  });
  ASSERT_NE(mismatch, warnings.end());
  EXPECT_NE(mismatch->at("message").get<std::string>().find("The configured encoder adapter"), std::string::npos);
}
#endif


TEST(StreamStatsHotFieldTests, EncoderSamplesFollowCodecChangesAndResetAtStreamEnd) {
  stream_stats::update_stream_active(false);
  stream_stats::update_stream_active(true);
  stream_stats::add_client("127.0.0.1", "fixture");
  stream_stats::update_video_stats(90.0, 180000, 3.0, "pyrowave", 1280, 800, "pyrowave");
  auto stats = stream_stats::get_current();
  EXPECT_EQ(stats.codec, "pyrowave");
  EXPECT_EQ(stats.encoder_backend, "pyrowave");
  ASSERT_EQ(stats.clients.size(), 1u);
  EXPECT_EQ(stats.clients.front().encoder_backend, "pyrowave");
  stream_stats::update_video_stats("127.0.0.1", 90.0, 40000, 2.0, "h264", 1280, 800, "nvenc");
  stats = stream_stats::get_current();
  EXPECT_EQ(stats.codec, "h264");
  EXPECT_EQ(stats.encoder_backend, "nvenc");
  EXPECT_EQ(stats.clients.front().encoder_backend, "nvenc");
  stream_stats::update_stream_active(false);
  EXPECT_TRUE(stream_stats::get_current().encoder_backend.empty());
  EXPECT_TRUE(stream_stats::get_current().codec.empty());
}
