/**
 * @file tests/unit/test_stream_display_policy.cpp
 * @brief Test Linux stream display policy user-facing capability contract.
 */

#include <src/platform/linux/stream_display_policy.h>
#include <src/platform/linux/game_mode_host.h>
#include <src/platform/linux/stream_path.h>
#include <src/platform/linux/virtual_display.h>
#include <src/platform/linux/display_topology.h>
#include <src/config.h>
#include <src/video.h>
#include <src/logging.h>
#include <src/nvhttp.h>
#include <src/platform/common.h>
#include <src/stream_stats.h>
#include <src/utility.h>
#include <src/verified_action.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <initializer_list>
#include <sstream>
#include <string>
#include <unordered_map>
#include <vector>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <stdexcept>
#include <sys/stat.h>
#include <unistd.h>

#include <boost/core/null_deleter.hpp>
#include <boost/log/core.hpp>
#include <boost/log/sinks/sync_frontend.hpp>
#include <boost/log/sinks/text_ostream_backend.hpp>
#include <boost/smart_ptr/make_shared_object.hpp>
#include <boost/smart_ptr/shared_ptr.hpp>

namespace platf {
  std::string capture_backend_dispatch_for_tests(
    std::string_view capture_backend,
    bool nvfbc_available,
    bool wayland_available,
    bool portal_available,
    bool kms_available,
    bool x11_available,
    bool cuda_memory
  );
  void log_capture_mode_override();
}

namespace {
  class ScopedPrivateRuntimePath {
  public:
    /// PATH holds only these stand-ins, labwc and wlr-randr unless a test names others.
    explicit ScopedPrivateRuntimePath(std::initializer_list<const char *> binaries = {"labwc", "wlr-randr"}) {
      if (const char *current = std::getenv("PATH")) {
        had_previous = true;
        previous = current;
      }

      char path_template[] = "/tmp/polaris-stream-policy-runtime-XXXXXX";
      const char *created = mkdtemp(path_template);
      if (!created) {
        throw std::runtime_error("failed to create stream-policy runtime test directory");
      }
      directory = created;

      for (const char *binary : binaries) {
        const auto path = std::filesystem::path {directory} / binary;
        std::ofstream script {path};
        script << "#!/bin/sh\nexit 0\n";
        script.close();
        if (!script || chmod(path.c_str(), 0700) != 0) {
          throw std::runtime_error("failed to create stream-policy runtime test executable");
        }
      }
      if (setenv("PATH", directory.c_str(), 1) != 0) {
        throw std::runtime_error("failed to set stream-policy runtime test PATH");
      }
    }

    ~ScopedPrivateRuntimePath() {
      if (had_previous) {
        setenv("PATH", previous.c_str(), 1);
      } else {
        unsetenv("PATH");
      }
      std::error_code ignored;
      std::filesystem::remove_all(directory, ignored);
    }

  private:
    bool had_previous = false;
    std::string previous;
    std::string directory;
  };

  struct LinuxDisplayPolicyGuard {
    LinuxDisplayPolicyGuard():
        headless_mode {config::video.linux_display.headless_mode},
        use_cage_compositor {config::video.linux_display.use_cage_compositor},
        prefer_gpu_native_capture {config::video.linux_display.prefer_gpu_native_capture},
        auto_manage_displays {config::video.linux_display.auto_manage_displays},
        stream_mode {config::video.linux_display.stream_mode},
        private_runtime {config::video.linux_display.private_runtime},
        headless_swap_mode {config::video.linux_display.headless_swap_mode},
        streaming_output {config::video.linux_display.streaming_output},
        primary_output {config::video.linux_display.primary_output},
        saved_streaming_output {config::video.linux_display.saved_streaming_output},
        capture {config::video.capture},
        output_name {config::video.output_name} {
    }

    ~LinuxDisplayPolicyGuard() {
      // A hold is process state too, and an ASSERT that stops a test halfway must not hand it on.
      stream_display_policy::forget_game_mode_hold();
      config::video.linux_display.headless_mode = headless_mode;
      config::video.linux_display.use_cage_compositor = use_cage_compositor;
      config::video.linux_display.prefer_gpu_native_capture = prefer_gpu_native_capture;
      config::video.linux_display.auto_manage_displays = auto_manage_displays;
      config::video.linux_display.stream_mode = stream_mode;
      config::video.linux_display.private_runtime = private_runtime;
      config::video.linux_display.headless_swap_mode = headless_swap_mode;
      config::video.linux_display.streaming_output = streaming_output;
      config::video.linux_display.primary_output = primary_output;
      config::video.linux_display.saved_streaming_output = saved_streaming_output;
      config::video.capture = capture;
      config::video.output_name = output_name;
    }

    bool headless_mode;
    bool use_cage_compositor;
    bool prefer_gpu_native_capture;
    bool auto_manage_displays;
    std::string stream_mode;
    std::string private_runtime;
    std::string headless_swap_mode;
    std::string streaming_output;
    std::string primary_output;
    std::string saved_streaming_output;
    std::string capture;
    std::string output_name;
  };

  /** A remembered host default is process-wide; never let one leak out of a test. */
  struct HeldHostDefaultGuard {
    ~HeldHostDefaultGuard() {
      stream_display_policy::forget_host_default();
    }
  };

  void configure_headless_cage(bool prefer_gpu_native_capture) {
    config::video.linux_display.headless_mode = true;
    config::video.linux_display.use_cage_compositor = true;
    config::video.linux_display.prefer_gpu_native_capture = prefer_gpu_native_capture;
    config::video.linux_display.stream_mode.clear();
    config::video.linux_display.private_runtime = "labwc";
  }
}  // namespace

TEST(StreamDisplayPolicyTests, APrivateHostIsNeverMovedOffItsOwnTopologyWithoutBeingAsked) {
  std::vector<std::string> candidates {""};
  for (const auto &path : stream_path::registry()) candidates.emplace_back(path.id);
  ASSERT_GT(candidates.size(), 1u);
  size_t checked = 0;
  for (const auto &requested : candidates) {
    for (int bits = 0; bits < 16; ++bits) {
      const bool mirror = bits & 1;
      const bool launch_vd = bits & 2;
      const bool locked = bits & 4;
      const bool optimized = bits & 8;
      const auto selection = stream_display_policy::host_default_launch_selection({
        .requested_selection = requested,
        .mirror_desktop = mirror,
        .launch_virtual_display = launch_vd,
        .virtual_display_user_locked = locked,
        .virtual_display_optimization_present = optimized,
        .host_provides_private_display = true,
      });
      ++checked;
      EXPECT_TRUE(selection.empty() || (!requested.empty() && selection == requested) ||
        (mirror && selection == "desktop_display") || (locked && launch_vd && selection == "host_virtual_display"))
        << "requested=" << requested << " bits=" << bits << " resolved=" << selection;
    }
  }
  EXPECT_EQ(checked, candidates.size() * 16);
}

TEST(StreamDisplayPolicyTests, APrivateHostRefusesAnUnlockedVirtualDisplayPreference) {
  using stream_display_policy::host_default_launch_selection;
  EXPECT_EQ(host_default_launch_selection({.launch_virtual_display = true, .host_provides_private_display = true}), "");
  EXPECT_EQ(host_default_launch_selection({.launch_virtual_display = true, .virtual_display_user_locked = true, .host_provides_private_display = true}), "host_virtual_display");
  EXPECT_EQ(host_default_launch_selection({.requested_selection = "host_virtual_display", .host_provides_private_display = true}), "host_virtual_display");
  EXPECT_EQ(host_default_launch_selection({.requested_selection = "headless_stream", .host_provides_private_display = true}), "headless_stream");
  EXPECT_EQ(host_default_launch_selection({.mirror_desktop = true, .launch_virtual_display = true, .host_provides_private_display = true}), "desktop_display");
}

TEST(StreamDisplayPolicyTests, PrivateHostAnswerReadsTheHostNotTheParkedSession) {
  LinuxDisplayPolicyGuard guard;
  HeldHostDefaultGuard held;
  configure_headless_cage(true);

  EXPECT_TRUE(stream_display_policy::host_default_provides_private_display());

  // Park a virtual-display session on it, exactly as a launch override does.
  stream_display_policy::remember_host_default(
    stream_display_policy::legacy_booleans_t {true, true, true},
    "",
    "wlr"
  );
  config::video.linux_display.stream_mode =
    std::string {stream_display_policy::k_host_virtual_display};
  config::video.linux_display.use_cage_compositor = false;
  config::video.capture = "portal";

  EXPECT_TRUE(stream_display_policy::host_default_provides_private_display())
    << "a session parked on a virtual display does not stop the host being a private host";
}

TEST(StreamDisplayPolicyTests, HostDefaultOutlivesASessionScopedVirtualDisplayOverride) {
  LinuxDisplayPolicyGuard guard;
  HeldHostDefaultGuard held;
  configure_headless_cage(true);

  // What this host is, before any client launches anything on it.
  ASSERT_EQ(stream_display_policy::configured_selection(), "windowed_stream");

  // A launch takes the virtual display path. apply_selection rewrites the live
  // config in place and normalize_host_virtual_display_state_for_backend
  // replaces the capture backend, and a paused session keeps both rewritten
  // until its resume window closes. Reproduce that state exactly.
  stream_display_policy::remember_host_default(
    stream_display_policy::legacy_booleans_t {true, true, true},
    "",
    "wlr"
  );
  config::video.linux_display.stream_mode =
    std::string {stream_display_policy::k_host_virtual_display};
  config::video.linux_display.use_cage_compositor = false;
  config::video.capture = "portal";

  EXPECT_EQ(stream_display_policy::configured_selection(), "host_virtual_display")
    << "the live config is the running session's topology while the override stands";
  EXPECT_EQ(stream_display_policy::host_default_selection(), "windowed_stream")
    << "the host default must not become whatever the last client asked for";

  const auto host_default = stream_display_policy::resolve_host_default(
    stream_display_policy::input_t {}
  );
  EXPECT_TRUE(host_default.uses_labwc());
  EXPECT_TRUE(host_default.requested_headless);
  EXPECT_FALSE(stream_display_policy::resolve(stream_display_policy::input_t {}).uses_labwc())
    << "the session's own resolution still follows the live override";

  // Teardown retires the override, and the two answers agree again.
  stream_display_policy::forget_host_default();
  EXPECT_EQ(stream_display_policy::host_default_selection(), "host_virtual_display")
    << "with nothing held, the host default is simply the live configuration";
}

TEST(StreamDisplayPolicyTests, GpuNativePreferenceLabelsPrivateStreamCaptureCapability) {
  LinuxDisplayPolicyGuard guard;
  configure_headless_cage(true);

  const auto resolved = stream_display_policy::resolve(stream_display_policy::input_t {});

  EXPECT_EQ(resolved.selection, "windowed_stream");
  EXPECT_EQ(resolved.label, "Private Stream (GPU-native)");
  // selection string is the mode id (no parallel mode_e).
  EXPECT_TRUE(resolved.prefer_gpu_native_capture);
  EXPECT_TRUE(resolved.requested_headless);
  EXPECT_TRUE(resolved.uses_labwc());
  EXPECT_TRUE(resolved.use_private_runtime);
  EXPECT_EQ(resolved.runtime, stream_path::runtime_kind_e::LABWC);
}

TEST(StreamDisplayPolicyTests, EncoderGpuNativeRequirementPromotesCapableHostPath) {
  LinuxDisplayPolicyGuard guard;
  configure_headless_cage(false);

  const auto resolved = stream_display_policy::resolve(stream_display_policy::input_t {
    false,
    true,
    false,
  });

  EXPECT_EQ(resolved.selection, "windowed_stream");
  EXPECT_EQ(resolved.label, "Private Stream (GPU-native)");
  EXPECT_EQ(resolved.reason, "Polaris can force a windowed private compositor when hidden Private Stream capture cannot stay GPU-native.");
}

TEST(StreamDisplayPolicyTests, WindowedCageDefersEncoderProbeUntilRuntimeExists) {
  LinuxDisplayPolicyGuard guard;
  config::video.linux_display.headless_mode = false;
  config::video.linux_display.use_cage_compositor = true;
  config::video.linux_display.prefer_gpu_native_capture = false;
  config::video.linux_display.stream_mode.clear();

  const auto resolved = stream_display_policy::resolve(stream_display_policy::input_t {});

  EXPECT_EQ(resolved.selection, "windowed_stream");
  EXPECT_EQ(resolved.label, "Private Stream (windowed)");
  EXPECT_FALSE(resolved.requested_headless);
  EXPECT_FALSE(resolved.effective_headless);
  EXPECT_TRUE(resolved.uses_labwc());
  EXPECT_TRUE(resolved.should_defer_encoder_probe);
}

TEST(AppLaunchAsPolicyTests, ExactSharedModesPinEveryClientUnlessItNamesAConflict) {
  using verdict = stream_display_policy::app_launch_as_t::verdict_e;
  std::ifstream source {std::filesystem::path {POLARIS_SOURCE_DIR} / "tests/fixtures/app-launch-as-v1.json"};
  ASSERT_TRUE(source.good());
  const auto fixture = nlohmann::json::parse(source);
  std::vector<std::string> client_modes {"", "headless_dongle"};
  for (const auto &row : fixture.at("values")) {
    const auto mode = row.at("id").get<std::string>();
    if (mode != "host_default") client_modes.push_back(mode);
  }
  size_t pinned = 0;
  for (const auto &row : fixture.at("values")) {
    const auto mode = row.at("id").get<std::string>();
    const bool follows = mode == "host_default" || mode == "desktop_display";
    for (const auto &named : client_modes) {
      SCOPED_TRACE(mode + " / named=" + named);
      const auto result = stream_display_policy::resolve_app_launch_as(mode, named);
      EXPECT_EQ(result.verdict, follows ? verdict::follow :
        (!named.empty() && named != mode ? verdict::conflict : verdict::pinned));
      EXPECT_EQ(result.selection, follows ? "" : mode);
    }
    if (!follows) ++pinned;
  }
  EXPECT_EQ(pinned, 5u);
}

TEST(AppLaunchAsPolicyTests, MisspellingsWhitespaceAndHostOnlyModesNeverBecomePins) {
  using verdict = stream_display_policy::app_launch_as_t::verdict_e;
  for (const auto value : {"headless_dongle", "", "Host_Default", "Headless_Stream", " host_default", "host_default ", "mirror_desktop", "private_stream", "invalid", "turbo"}) {
    SCOPED_TRACE(value);
    const auto result = stream_display_policy::resolve_app_launch_as(value, "");
    EXPECT_EQ(result.verdict, verdict::not_a_launch_mode);
    EXPECT_TRUE(result.selection.empty());
  }
}

TEST(AppLaunchAsPolicyTests, SharedVocabularyAndLabelsMatchAllSessionOverridablePaths) {
  std::ifstream source {std::filesystem::path {POLARIS_SOURCE_DIR} / "tests/fixtures/app-launch-as-v1.json"};
  ASSERT_TRUE(source.good());
  const auto fixture = nlohmann::json::parse(source);
  std::vector<std::string> expected;
  for (const auto &row : fixture.at("values")) {
    const auto mode = row.at("id").get<std::string>();
    if (mode == "host_default") continue;
    expected.push_back(mode);
    EXPECT_EQ(stream_display_policy::label_for_selection(mode), row.at("label").get<std::string>());
  }
  std::vector<std::string> actual;
  for (const auto &path : stream_path::registry()) {
    if (stream_display_policy::selection_session_overridable(path.id)) actual.emplace_back(path.id);
  }
  std::sort(expected.begin(), expected.end());
  std::sort(actual.begin(), actual.end());
  EXPECT_EQ(actual, expected);
  EXPECT_EQ(actual.size(), 6u);
}

TEST(StreamDisplayPolicyTests, LegacyBooleansMapToSelections) {
  using stream_display_policy::selection_from_legacy_booleans;
  using stream_display_policy::legacy_booleans_t;

  EXPECT_EQ(selection_from_legacy_booleans({true, true, false}), "headless_stream");
  EXPECT_EQ(selection_from_legacy_booleans({true, true, true}), "windowed_stream");
  EXPECT_EQ(selection_from_legacy_booleans({true, false, false}), "host_virtual_display");
  EXPECT_EQ(selection_from_legacy_booleans({false, false, false}), "desktop_display");
}

TEST(StreamDisplayPolicyTests, AnUnlockedClientVirtualDisplayPromotesOnlyWhenNothingWasChosen) {
  using stream_display_policy::host_default_launch_selection;
  EXPECT_EQ(host_default_launch_selection({}), "") << "Host default carries no app display preference";
  EXPECT_EQ(host_default_launch_selection({.launch_virtual_display = true, .virtual_display_user_locked = true}), "host_virtual_display");
  EXPECT_EQ(host_default_launch_selection({.launch_virtual_display = true, .virtual_display_optimization_present = true}), "host_virtual_display");
  EXPECT_EQ(host_default_launch_selection({.mirror_desktop = true, .launch_virtual_display = true}), "desktop_display");
  EXPECT_EQ(host_default_launch_selection({.requested_selection = "headless_stream", .launch_virtual_display = true, .virtual_display_user_locked = true}), "headless_stream");
  EXPECT_EQ(host_default_launch_selection({.requested_selection = "gamescope_stream", .mirror_desktop = true, .virtual_display_user_locked = true}), "desktop_display");
}

TEST(StreamDisplayPolicyTests, PrivateAndVirtualModesOwnTheirLaunchRefreshRate) {
  using stream_display_policy::selection_owns_launch_refresh_rate;

  EXPECT_TRUE(selection_owns_launch_refresh_rate("headless_stream"));
  EXPECT_TRUE(selection_owns_launch_refresh_rate("windowed_stream"));
  EXPECT_TRUE(selection_owns_launch_refresh_rate("host_virtual_display"));
  EXPECT_TRUE(selection_owns_launch_refresh_rate("desktop_takeover"));
  EXPECT_TRUE(selection_owns_launch_refresh_rate("gamescope_stream"));
  EXPECT_FALSE(selection_owns_launch_refresh_rate("desktop_display"));
  EXPECT_FALSE(selection_owns_launch_refresh_rate("headless_dongle"));
}

TEST(StreamDisplayPolicyTests, HostVirtualClearsStaleAutoManage) {
  if (!virtual_display::host_stream_readiness(true).available) {
    GTEST_SKIP() << "host virtual display normalization requires a ready capture provider and creator";
  }
  LinuxDisplayPolicyGuard guard;
  std::string error;
  ASSERT_TRUE(stream_display_policy::apply_selection("host_virtual_display", error)) << error;
  config::video.linux_display.auto_manage_displays = true;
  EXPECT_FALSE(stream_display_policy::selection_companion_state_matches("host_virtual_display"));
  ASSERT_TRUE(stream_display_policy::apply_selection("host_virtual_display", error)) << error;
  EXPECT_FALSE(config::video.linux_display.auto_manage_displays);
}

TEST(StreamDisplayPolicyTests, VirtualCaptureOutputNameNeverLosesTheCreatedConnector) {
  using stream_display_policy::capture_output_name_for_virtual_display;

  EXPECT_EQ(
    capture_output_name_for_virtual_display("POLARIS-HEADLESS-512536-0", ""),
    "POLARIS-HEADLESS-512536-0"
  );
  EXPECT_EQ(
    capture_output_name_for_virtual_display("POLARIS-HEADLESS-512536-0", "1"),
    "1"
  );
}

TEST(StreamDisplayPolicyTests, HostVirtualCaptureFollowsTheVirtualDisplayBackend) {
  using stream_display_policy::capture_for_host_virtual_display_backend;
  using virtual_display::backend_e;

  for (const auto current : {"", "auto", "portal", "kms"}) {
    EXPECT_EQ(capture_for_host_virtual_display_backend(backend_e::WAYLAND_WLR, current), "wlr")
      << current;
  }

  // A KWin-created screen is an ordinary KWin monitor, captured like EVDI's.
  for (const auto backend : {backend_e::EVDI, backend_e::KSCREEN_DOCTOR, backend_e::KWIN_VIRTUAL_OUTPUT}) {
    EXPECT_EQ(capture_for_host_virtual_display_backend(backend, ""), "portal");
    EXPECT_EQ(capture_for_host_virtual_display_backend(backend, "auto"), "portal");
    EXPECT_EQ(capture_for_host_virtual_display_backend(backend, "portal"), "portal");
    EXPECT_EQ(capture_for_host_virtual_display_backend(backend, "wlr"), "portal");
    EXPECT_EQ(capture_for_host_virtual_display_backend(backend, "kms"), "portal");
  }

  EXPECT_EQ(capture_for_host_virtual_display_backend(backend_e::NONE, "portal"), "portal");
}

TEST(StreamDisplayPolicyTests, SessionTransitionUsesACaptureBackendThatCanAddressTheRuntime) {
  using stream_display_policy::capture_for_session_transition;

  EXPECT_EQ(
    capture_for_session_transition("headless_stream", "desktop_display", "wlr"),
    ""
  ) << "host desktop discovery must not remain pinned to private wlroots capture";
  EXPECT_EQ(
    capture_for_session_transition("windowed_stream", "desktop_display", "wlroots"),
    ""
  );
  EXPECT_EQ(
    capture_for_session_transition("headless_stream", "desktop_display", "kms"),
    "kms"
  ) << "an explicit Desktop-compatible KMS choice remains authoritative";
  EXPECT_EQ(
    capture_for_session_transition("headless_stream", "desktop_display", "portal"),
    "portal"
  );
  EXPECT_EQ(
    capture_for_session_transition("desktop_display", "headless_stream", "portal"),
    "wlr"
  ) << "private labwc outputs require the wlroots capture path";
  EXPECT_EQ(
    capture_for_session_transition("headless_stream", "gamescope_stream", "wlr"),
    "portal"
  );
  EXPECT_EQ(
    capture_for_session_transition("headless_stream", "headless_stream", "wlr"),
    "wlr"
  ) << "no session transition means no capture override";
}

TEST(StreamDisplayPolicyTests, GenerationCaptureFollowsThePrivateRuntimeAndTheSubstitution) {
  using stream_display_policy::capture_for_mode;

  // A private labwc session can only be captured through wlroots, whatever the preference says.
  EXPECT_EQ(capture_for_mode("kms", "headless_stream", true, false, false), "wlr");
  EXPECT_EQ(capture_for_mode("", "windowed_stream", true, true, false), "wlr");
  EXPECT_EQ(capture_for_mode("portal", "headless_stream", true, false, true), "wlr");

  // #739: Mirror Desktop with capture = wlr on KDE. The evaluation substituted kms, the encoder
  // probe asked for auto and passed, and every stream asked for wlr by name and found nothing.
  EXPECT_EQ(capture_for_mode("wlr", "desktop_display", false, true, false), "")
    << "a substituted backend must be what the stream asks for, through auto";
  EXPECT_EQ(capture_for_mode("portal", "headless_dongle", false, true, false), "");

  // Without a substitution the preference stands, including wlr on a wlroots desktop.
  EXPECT_EQ(capture_for_mode("wlr", "desktop_display", false, false, false), "wlr");
  EXPECT_EQ(capture_for_mode("kms", "desktop_display", false, false, false), "kms");
  EXPECT_EQ(capture_for_mode("", "desktop_display", false, false, false), "");

  // Auto cannot address an exact output, and on Gamescope it would capture the desktop instead.
  EXPECT_EQ(capture_for_mode("portal", "host_virtual_display", false, true, true), "portal");
  EXPECT_EQ(capture_for_mode("portal", "gamescope_stream", false, true, false), "portal");
  EXPECT_EQ(capture_for_mode("portal", "GAMESCOPE_STREAM", false, true, false), "portal");
}

TEST(StreamDisplayPolicyTests, CurrentModeCaptureReadsTheLiveConfigAndTheLastEvaluation) {
  LinuxDisplayPolicyGuard guard;
  struct SubstitutionGuard {
    ~SubstitutionGuard() {
      platf::set_capture_backend_substitution_for_tests("");
    }
  } substitution_guard;
  auto &d = config::video.linux_display;
  d.stream_mode = "desktop_display";
  d.headless_mode = false;
  d.use_cage_compositor = false;
  d.private_runtime.clear();
  config::video.capture = "wlr";

  platf::set_capture_backend_substitution_for_tests("");
  EXPECT_EQ(stream_display_policy::capture_for_current_mode(), "wlr");

  platf::set_capture_backend_substitution_for_tests("wlr -> kms");
  EXPECT_EQ(stream_display_policy::capture_for_current_mode(), "");
  EXPECT_EQ(stream_display_policy::capture_for_current_mode(true), "wlr");

  d.stream_mode = "headless_stream";
  d.headless_mode = true;
  d.use_cage_compositor = true;
  d.private_runtime = "labwc";
  config::video.capture = "kms";
  EXPECT_EQ(stream_display_policy::capture_for_current_mode(), "wlr");
}

TEST(StreamDisplayPolicyTests, AHostDefaultModeChangeIsReportedSoCaptureIsReevaluated) {
  // #739: a client moved the host default from Mirror Desktop to Private Stream without a
  // restart, the capture sources stayed as evaluated for Mirror Desktop, and every Private
  // Stream launch asked that list for wlr and found nothing.
  ScopedPrivateRuntimePath runtime_path;
  LinuxDisplayPolicyGuard guard;
  auto &d = config::video.linux_display;
  d.stream_mode = "desktop_display";
  d.headless_mode = false;
  d.use_cage_compositor = false;
  d.prefer_gpu_native_capture = false;
  d.private_runtime.clear();
  d.auto_manage_displays = false;
  d.headless_swap_mode.clear();
  d.streaming_output.clear();
  d.primary_output.clear();
  config::video.capture = "wlr";
  config::video.output_name.clear();

  std::vector<std::pair<std::string, std::string>> changes;
  std::string error;
  ASSERT_TRUE(nvhttp::apply_stream_display_mode_selection_for_tests("headless_stream", true, changes, error)) << error;
  ASSERT_EQ(changes.size(), 1u);
  EXPECT_EQ(changes[0].first, "desktop_display");
  EXPECT_EQ(changes[0].second, "headless_stream");
  EXPECT_EQ(config::video.capture, "wlr") << "the mode change still leaves the capture preference alone";

  // Saving the mode the host already has changes nothing, so nothing needs re-evaluating.
  ASSERT_TRUE(nvhttp::apply_stream_display_mode_selection_for_tests("headless_stream", true, changes, error)) << error;
  EXPECT_EQ(changes.size(), 1u);

  // A change that could not be saved is rolled back and reports nothing either.
  EXPECT_FALSE(nvhttp::apply_stream_display_mode_selection_for_tests("desktop_display", false, changes, error));
  EXPECT_EQ(changes.size(), 1u);
  EXPECT_EQ(d.stream_mode, "headless_stream");

  ASSERT_TRUE(nvhttp::apply_stream_display_mode_selection_for_tests("desktop_display", true, changes, error)) << error;
  ASSERT_EQ(changes.size(), 2u);
  EXPECT_EQ(changes[1].first, "headless_stream");
  EXPECT_EQ(changes[1].second, "desktop_display");
}

TEST(StreamDisplayPolicyTests, CaptureSourcesGoStaleOnceTheModeOrCaptureSettingMoves) {
  auto &d = config::video.linux_display;
  const auto saved_display = d;
  const auto saved_capture = config::video.capture;
  struct Restore {
    config::video_t::linux_display_t &display;
    const config::video_t::linux_display_t &saved_display;
    const std::string &saved_capture;
    ~Restore() {
      display = saved_display;
      config::video.capture = saved_capture;
      // Leave the evaluation state as a test binary finds it: nothing evaluated.
      platf::set_capture_sources_missing_for_tests(false);
    }
  } restore {d, saved_display, saved_capture};

  d.stream_mode = "desktop_display";
  d.use_cage_compositor = false;
  d.headless_mode = false;
  d.private_runtime.clear();
  config::video.capture = "wlr";

  platf::set_capture_sources_missing_for_tests(false);
  EXPECT_TRUE(platf::capture_sources_stale()) << "nothing has been evaluated yet";

  platf::mark_capture_sources_evaluated_for_current_config_for_tests();
  EXPECT_FALSE(platf::capture_sources_stale());

  d.stream_mode = "headless_stream";
  d.use_cage_compositor = true;
  d.headless_mode = true;
  d.private_runtime = "labwc";
  EXPECT_TRUE(platf::capture_sources_stale()) << "a Mirror Desktop list serving Private Stream is #739";

  d.stream_mode = "desktop_display";
  d.use_cage_compositor = false;
  d.headless_mode = false;
  d.private_runtime.clear();
  EXPECT_FALSE(platf::capture_sources_stale());

  config::video.capture = "kms";
  EXPECT_TRUE(platf::capture_sources_stale());
}

TEST(StreamDisplayPolicyTests, ApplySelectionSyncsModeAndLegacyBooleans) {
  ScopedPrivateRuntimePath runtime_path;
  LinuxDisplayPolicyGuard guard;
  std::string error;

  ASSERT_TRUE(stream_display_policy::apply_selection("headless_stream", error)) << error;
  EXPECT_EQ(config::video.linux_display.stream_mode, "headless_stream");
  EXPECT_TRUE(config::video.linux_display.headless_mode);
  EXPECT_TRUE(config::video.linux_display.use_cage_compositor);
  EXPECT_FALSE(config::video.linux_display.prefer_gpu_native_capture);
  EXPECT_EQ(config::video.linux_display.private_runtime, "labwc");

  ASSERT_TRUE(stream_display_policy::apply_selection("desktop_display", error)) << error;
  EXPECT_EQ(config::video.linux_display.stream_mode, "desktop_display");
  EXPECT_FALSE(config::video.linux_display.headless_mode);
  EXPECT_FALSE(config::video.linux_display.use_cage_compositor);
}

// polaris#626. A Deck probe configured for Private Stream never reached a launch: every serverinfo poll
// tried to start a private compositor to probe encoders, and said "labwc not found" six times in a row.
// A host in Steam Game Mode has one screen, so while the session is live its mode is a mirror of it.
TEST(StreamDisplayPolicyTests, AGameModeSessionHoldsTheConfiguredModeAndGivesItBack) {
  using stream_display_policy::game_mode_reconcile_e;
  ScopedPrivateRuntimePath runtime_path;
  LinuxDisplayPolicyGuard guard;
  std::string error;
  ASSERT_TRUE(stream_display_policy::apply_selection("headless_stream", error)) << error;
  config::video.capture = "wlr";

  EXPECT_EQ(stream_display_policy::reconcile_game_mode(true, true), game_mode_reconcile_e::unchanged)
    << "nothing moves under a stream that is already up";
  EXPECT_EQ(stream_display_policy::configured_selection(), "headless_stream");

  EXPECT_EQ(stream_display_policy::reconcile_game_mode(true, false), game_mode_reconcile_e::entered);
  EXPECT_EQ(stream_display_policy::configured_selection(), "desktop_display");
  EXPECT_FALSE(config::video.linux_display.use_cage_compositor) << "so no poll starts a private compositor to probe with";
  EXPECT_FALSE(config::video.linux_display.headless_mode);
  EXPECT_NE(config::video.capture, "wlr") << "there is no wlroots compositor to capture from";
  EXPECT_EQ(stream_display_policy::game_mode_held_selection(), "headless_stream");
  EXPECT_EQ(stream_display_policy::reconcile_game_mode(true, false), game_mode_reconcile_e::unchanged)
    << "asked again, it is already there";

  EXPECT_EQ(stream_display_policy::reconcile_game_mode(false, true), game_mode_reconcile_e::unchanged)
    << "the session ended under a live stream: wait for the stream";
  EXPECT_EQ(stream_display_policy::reconcile_game_mode(false, false), game_mode_reconcile_e::left);
  EXPECT_EQ(stream_display_policy::configured_selection(), "headless_stream");
  EXPECT_TRUE(config::video.linux_display.use_cage_compositor);
  EXPECT_EQ(config::video.linux_display.private_runtime, "labwc");
  EXPECT_EQ(config::video.capture, "wlr");
  EXPECT_TRUE(stream_display_policy::game_mode_held_selection().empty());
  EXPECT_EQ(stream_display_policy::reconcile_game_mode(false, false), game_mode_reconcile_e::unchanged);
}

// #635: Auto tries Vulkan Video first on AMD Gamescope Stream and on no other host mode, Steam Game
// Mode's own screen included. Auto reads the route from the state a mode writes when it is loaded or
// applied, so walk every mode through the load, and Gamescope Stream through the Game Mode hold.
TEST(StreamDisplayPolicyTests, OnlyGamescopeStreamPutsAmdAutoOnTheGamescopeRoute) {
  using stream_display_policy::game_mode_reconcile_e;
  ScopedPrivateRuntimePath runtime_path({"labwc", "wlr-randr", "gamescope"});
  LinuxDisplayPolicyGuard guard;
  auto &d = config::video.linux_display;
  const auto codecs = std::pair {config::video.hevc_mode, config::video.av1_mode};
  config::video.hevc_mode = 0;
  config::video.av1_mode = 0;
  auto restore_codecs = util::fail_guard([codecs] {
    config::video.hevc_mode = codecs.first;
    config::video.av1_mode = codecs.second;
  });
  // The plan the host makes, for an AMD card, from the state the mode wrote.
  const auto amd_policy = [] {
    return video::planned_encoder_selection_info_for_tests("amdgpu").policy;
  };

  config::video.capture.clear();
  for (const auto &path : stream_path::registry()) {
    d.stream_mode = std::string {path.id};
    stream_display_policy::normalize_config_from_load();
    const std::string expected =
      path.id == stream_path::k_gamescope_stream              ? "amd_gamescope_vulkan_ram" :
      path.runtime == stream_path::runtime_kind_e::LABWC ? "amd_private_vulkan_live_probe" :
                                                           "amd_established_desktop";
    EXPECT_EQ(amd_policy(), expected) << path.id;
  }

  std::string error;
  ASSERT_TRUE(stream_display_policy::apply_selection("gamescope_stream", error)) << error;
  EXPECT_EQ(amd_policy(), "amd_gamescope_vulkan_ram");

  ASSERT_EQ(stream_display_policy::reconcile_game_mode(true, false), game_mode_reconcile_e::entered);
  EXPECT_TRUE(platf::game_mode_host::streams_session_screen(d.stream_mode, d.use_cage_compositor, false, true))
    << "the hold is the Game Mode screen route";
  EXPECT_EQ(amd_policy(), "amd_established_desktop") << "Game Mode's screen is not Gamescope Stream";

  ASSERT_EQ(stream_display_policy::reconcile_game_mode(false, false), game_mode_reconcile_e::left);
  EXPECT_EQ(amd_policy(), "amd_gamescope_vulkan_ram") << "the mode comes back when Game Mode ends";

  // Gamescope Stream loaded with a capture it keeps and the portal does not serve stays on VA-API;
  // unset and kwin are the portal.
  for (const auto &[capture, expected] : std::initializer_list<std::pair<const char *, const char *>> {
         {"kms", "amd_established_desktop"},
         {"wlr", "amd_established_desktop"},
         {"x11", "amd_established_desktop"},
         {"auto", "amd_established_desktop"},
         {"", "amd_gamescope_vulkan_ram"},
         {"portal", "amd_gamescope_vulkan_ram"},
         {"kwin", "amd_gamescope_vulkan_ram"},
       }) {
    d.stream_mode = "gamescope_stream";
    config::video.capture = capture;
    stream_display_policy::normalize_config_from_load();
    EXPECT_EQ(amd_policy(), expected) << "capture=" << capture;
  }
}

TEST(StreamDisplayPolicyTests, AGameModeSessionLeavesAMirrorHostAndAReloadedConfigAlone) {
  using stream_display_policy::game_mode_reconcile_e;
  ScopedPrivateRuntimePath runtime_path;
  LinuxDisplayPolicyGuard guard;
  std::string error;

  ASSERT_TRUE(stream_display_policy::apply_selection("desktop_display", error)) << error;
  EXPECT_EQ(stream_display_policy::reconcile_game_mode(true, false), game_mode_reconcile_e::unchanged)
    << "a host already set to Mirror Desktop has nothing to hold";
  EXPECT_EQ(stream_display_policy::reconcile_game_mode(false, false), game_mode_reconcile_e::unchanged);

  // Held, and then the console saves a new mode, which reloads the config over the held state.
  ASSERT_TRUE(stream_display_policy::apply_selection("headless_stream", error)) << error;
  ASSERT_EQ(stream_display_policy::reconcile_game_mode(true, false), game_mode_reconcile_e::entered);
  ASSERT_TRUE(stream_display_policy::apply_selection("windowed_stream", error)) << error;
  EXPECT_EQ(stream_display_policy::reconcile_game_mode(true, false), game_mode_reconcile_e::entered)
    << "the session is still live, so the reloaded mode is held in turn";
  EXPECT_EQ(stream_display_policy::game_mode_held_selection(), "windowed_stream");

  // The same reload after the hold was taken, with the session gone by the next call.
  ASSERT_TRUE(stream_display_policy::apply_selection("headless_stream", error)) << error;
  EXPECT_EQ(stream_display_policy::reconcile_game_mode(false, false), game_mode_reconcile_e::left);
  EXPECT_EQ(stream_display_policy::configured_selection(), "headless_stream")
    << "what was reloaded is newer than what was held, and it stays";
}

TEST(StreamDisplayPolicyTests, AGameModeSessionGivesBackTheConnectorsTheMirrorCleared) {
  // The switch to the mirror clears display management, the swap mode and the connector pair, and
  // the output name when it named the streaming connector. A dongle host came back from Game Mode
  // without them and re-detected connectors on the next launch.
  using stream_display_policy::game_mode_reconcile_e;
  ScopedPrivateRuntimePath runtime_path;
  LinuxDisplayPolicyGuard guard;
  std::string error;
  ASSERT_TRUE(stream_display_policy::apply_selection("headless_stream", error)) << error;
  auto &linux_display = config::video.linux_display;
  linux_display.auto_manage_displays = true;
  linux_display.headless_swap_mode = "privacy";
  linux_display.streaming_output = "HDMI-A-1";
  linux_display.primary_output = "DP-2";
  config::video.output_name = "HDMI-A-1";

  ASSERT_EQ(stream_display_policy::reconcile_game_mode(true, false), game_mode_reconcile_e::entered);
  ASSERT_FALSE(linux_display.auto_manage_displays) << "the mirror did clear them, which is what has to be given back";
  ASSERT_TRUE(linux_display.streaming_output.empty());

  ASSERT_EQ(stream_display_policy::reconcile_game_mode(false, false), game_mode_reconcile_e::left);
  EXPECT_TRUE(linux_display.auto_manage_displays);
  EXPECT_EQ(linux_display.headless_swap_mode, "privacy");
  EXPECT_EQ(linux_display.streaming_output, "HDMI-A-1");
  EXPECT_EQ(linux_display.primary_output, "DP-2");
  EXPECT_EQ(config::video.output_name, "HDMI-A-1");
}

TEST(StreamDisplayPolicyTests, AModeChosenDuringGameModeIsTheOneThatStays) {
  // Chosen through the same writer the console and clients use, which also saves it.
  using stream_display_policy::game_mode_reconcile_e;
  ScopedPrivateRuntimePath runtime_path;
  LinuxDisplayPolicyGuard guard;
  std::string error;
  ASSERT_TRUE(stream_display_policy::apply_selection("headless_stream", error)) << error;
  ASSERT_EQ(stream_display_policy::reconcile_game_mode(true, false), game_mode_reconcile_e::entered);

  ASSERT_TRUE(nvhttp::apply_stream_display_mode_selection_for_tests("desktop_display", true, error)) << error;
  EXPECT_TRUE(stream_display_policy::game_mode_held_selection().empty())
    << "a mode chosen on purpose is newer than the one held";
  EXPECT_EQ(stream_display_policy::reconcile_game_mode(false, false), game_mode_reconcile_e::unchanged);
  EXPECT_EQ(stream_display_policy::configured_selection(), "desktop_display")
    << "the file says Mirror Desktop, so the process runs Mirror Desktop";

  // A Private Stream chosen during Game Mode is held in its turn and comes back after it.
  ASSERT_TRUE(stream_display_policy::apply_selection("desktop_display", error)) << error;
  ASSERT_TRUE(nvhttp::apply_stream_display_mode_selection_for_tests("windowed_stream", true, error)) << error;
  EXPECT_EQ(stream_display_policy::reconcile_game_mode(true, false), game_mode_reconcile_e::entered);
  EXPECT_EQ(stream_display_policy::game_mode_held_selection(), "windowed_stream");
  EXPECT_EQ(stream_display_policy::reconcile_game_mode(false, false), game_mode_reconcile_e::left);
  EXPECT_EQ(stream_display_policy::configured_selection(), "windowed_stream");

  // A config reload is the same: what was loaded is what the host is configured for now.
  ASSERT_EQ(stream_display_policy::reconcile_game_mode(true, false), game_mode_reconcile_e::entered);
  stream_display_policy::normalize_config_from_load();
  EXPECT_TRUE(stream_display_policy::game_mode_held_selection().empty());
}

TEST(StreamDisplayPolicyTests, ReapplyingHeadlessSelectionClearsStaleCompanionState) {
  ScopedPrivateRuntimePath runtime_path;
  LinuxDisplayPolicyGuard guard;
  auto &linux_display = config::video.linux_display;

  linux_display.stream_mode = "headless_stream";
  linux_display.headless_mode = true;
  linux_display.use_cage_compositor = true;
  linux_display.prefer_gpu_native_capture = true;
  linux_display.private_runtime = "labwc";

  EXPECT_FALSE(stream_display_policy::selection_companion_state_matches("headless_stream"));

  std::string error;
  ASSERT_TRUE(stream_display_policy::apply_selection("headless_stream", error)) << error;
  EXPECT_EQ(linux_display.stream_mode, "headless_stream");
  EXPECT_TRUE(linux_display.headless_mode);
  EXPECT_TRUE(linux_display.use_cage_compositor);
  EXPECT_FALSE(linux_display.prefer_gpu_native_capture);
  EXPECT_TRUE(stream_display_policy::selection_companion_state_matches("headless_stream"));
  linux_display.stream_mode = "HEADLESS_STREAM";
  EXPECT_FALSE(stream_display_policy::selection_companion_state_matches("headless_stream"));
  linux_display.stream_mode = "headless_stream";

  const auto resolved = stream_display_policy::resolve(stream_display_policy::input_t {});
  EXPECT_EQ(resolved.selection, "headless_stream");
}

TEST(StreamDisplayPolicyTests, CommonModeCompanionStateMatchesAllRegisteredSelections) {
  LinuxDisplayPolicyGuard guard;
  auto &linux_display = config::video.linux_display;
  struct mode_case_t {
    const char *selection;
    const char *runtime;
  };
  const mode_case_t cases[] = {
    {"headless_stream", "labwc"},
    {"windowed_stream", "labwc"},
    {"desktop_display", ""},
    {"host_virtual_display", ""},
    {"desktop_takeover", ""},
  };

  for (const auto &test_case : cases) {
    SCOPED_TRACE(test_case.selection);
    const auto expected = stream_display_policy::legacy_booleans_for_selection(test_case.selection);
    linux_display.stream_mode = test_case.selection;
    linux_display.headless_mode = expected.headless_mode;
    linux_display.use_cage_compositor = expected.use_cage_compositor;
    linux_display.prefer_gpu_native_capture = expected.prefer_gpu_native_capture;
    linux_display.private_runtime = test_case.runtime;
    const bool virtual_mode =
      test_case.selection == std::string_view {"host_virtual_display"} ||
      test_case.selection == std::string_view {"desktop_takeover"};
    config::video.capture = virtual_mode ?
                              stream_display_policy::capture_for_host_virtual_display_backend(
                                virtual_display::detect_backend(),
                                ""
                              ) :
                              std::string {};
    EXPECT_TRUE(stream_display_policy::selection_companion_state_matches(test_case.selection));

    linux_display.prefer_gpu_native_capture = !expected.prefer_gpu_native_capture;
    EXPECT_FALSE(stream_display_policy::selection_companion_state_matches(test_case.selection));
  }
}

TEST(StreamDisplayPolicyTests, GamescopeCompanionStateIncludesCaptureDefault) {
  LinuxDisplayPolicyGuard guard;
  auto &linux_display = config::video.linux_display;

  linux_display.stream_mode = "gamescope_stream";
  linux_display.headless_mode = true;
  linux_display.use_cage_compositor = false;
  linux_display.prefer_gpu_native_capture = false;
  linux_display.private_runtime = "gamescope";
  config::video.capture.clear();
  EXPECT_FALSE(stream_display_policy::selection_companion_state_matches("gamescope_stream"));

  config::video.capture = "portal";
  EXPECT_TRUE(stream_display_policy::selection_companion_state_matches("gamescope_stream"));

  const char *prior_path = std::getenv("PATH");
  const std::string saved_path = prior_path ? prior_path : "";
  const bool path_was_set = prior_path != nullptr;
  EXPECT_EQ(setenv("PATH", "/polaris-test-no-gamescope", 1), 0);
  std::string error;
  EXPECT_FALSE(stream_display_policy::selection_valid("gamescope_stream", error))
    << "matching companion state must not bypass current availability";
  EXPECT_FALSE(error.empty());
  if (path_was_set) {
    EXPECT_EQ(setenv("PATH", saved_path.c_str(), 1), 0);
  } else {
    EXPECT_EQ(unsetenv("PATH"), 0);
  }
}

TEST(StreamDisplayPolicyTests, HeadlessDongleCompanionStateIncludesConditionalDefaults) {
  LinuxDisplayPolicyGuard guard;
  auto &linux_display = config::video.linux_display;

  linux_display.stream_mode = "headless_dongle";
  linux_display.headless_mode = true;
  linux_display.use_cage_compositor = false;
  linux_display.prefer_gpu_native_capture = false;
  linux_display.private_runtime.clear();
  linux_display.auto_manage_displays = true;
  linux_display.headless_swap_mode = "privacy";
  linux_display.streaming_output = "DP-1";
  linux_display.primary_output = "eDP-1";
  config::video.capture = "portal";
  config::video.output_name = "DP-1";
  EXPECT_TRUE(stream_display_policy::selection_companion_state_matches("headless_dongle"));

  linux_display.auto_manage_displays = false;
  EXPECT_FALSE(stream_display_policy::selection_companion_state_matches("headless_dongle"));
  linux_display.auto_manage_displays = true;
  linux_display.headless_swap_mode.clear();
  EXPECT_FALSE(stream_display_policy::selection_companion_state_matches("headless_dongle"));
  linux_display.headless_swap_mode = "privacy";
  linux_display.streaming_output.clear();
  EXPECT_FALSE(stream_display_policy::selection_companion_state_matches("headless_dongle"));
  linux_display.streaming_output = "DP-1";
  linux_display.primary_output.clear();
  EXPECT_FALSE(stream_display_policy::selection_companion_state_matches("headless_dongle"));
  linux_display.primary_output = "DP-1";
  EXPECT_FALSE(stream_display_policy::selection_companion_state_matches("headless_dongle"));
  linux_display.primary_output = "eDP-1";
  config::video.capture = "auto";
  EXPECT_FALSE(stream_display_policy::selection_companion_state_matches("headless_dongle"));
  config::video.capture = "portal";
  config::video.output_name.clear();
  EXPECT_FALSE(stream_display_policy::selection_companion_state_matches("headless_dongle"));
}

TEST(StreamDisplayPolicyTests, LeavingDongleMakesStoredConnectorsInert) {
  LinuxDisplayPolicyGuard guard;
  auto &linux_display = config::video.linux_display;
  linux_display.stream_mode = "headless_dongle";
  linux_display.headless_mode = true;
  linux_display.use_cage_compositor = false;
  linux_display.auto_manage_displays = true;
  linux_display.headless_swap_mode = "privacy";
  linux_display.streaming_output = "DP-1";
  linux_display.primary_output = "eDP-1";
  config::video.capture = "portal";
  config::video.output_name = "DP-1";

  std::string error;
  ASSERT_TRUE(stream_display_policy::apply_selection("desktop_display", error)) << error;
  EXPECT_FALSE(linux_display.auto_manage_displays);
  EXPECT_TRUE(linux_display.headless_swap_mode.empty());
  EXPECT_TRUE(linux_display.streaming_output.empty());
  EXPECT_TRUE(linux_display.primary_output.empty());
  EXPECT_TRUE(config::video.output_name.empty());
  EXPECT_EQ(config::video.capture, "portal")
    << "switching topology must not rewrite a user-selected capture backend";
  EXPECT_FALSE(display_topology::should_manage_host_topology());
  EXPECT_TRUE(stream_display_policy::selection_companion_state_matches("desktop_display"));
}

TEST(StreamDisplayPolicyTests, FailedDurableSelectionRestoresEveryLiveField) {
  LinuxDisplayPolicyGuard guard;
  auto &linux_display = config::video.linux_display;
  linux_display.stream_mode = "headless_dongle";
  linux_display.private_runtime = "legacy-runtime";
  linux_display.headless_mode = true;
  linux_display.use_cage_compositor = false;
  linux_display.prefer_gpu_native_capture = true;
  linux_display.auto_manage_displays = true;
  linux_display.headless_swap_mode = "privacy";
  linux_display.streaming_output = "DP-1";
  linux_display.primary_output = "eDP-1";
  config::video.capture = "portal";
  config::video.output_name = "DP-1";
  const auto before_linux_display = linux_display;
  const auto before_capture = config::video.capture;
  const auto before_output_name = config::video.output_name;

  std::string error;
  EXPECT_FALSE(nvhttp::apply_stream_display_mode_selection_for_tests(
    "desktop_display",
    false,
    error
  ));
  EXPECT_FALSE(error.empty());
  EXPECT_EQ(linux_display.stream_mode, before_linux_display.stream_mode);
  EXPECT_EQ(linux_display.private_runtime, before_linux_display.private_runtime);
  EXPECT_EQ(linux_display.headless_mode, before_linux_display.headless_mode);
  EXPECT_EQ(linux_display.use_cage_compositor, before_linux_display.use_cage_compositor);
  EXPECT_EQ(
    linux_display.prefer_gpu_native_capture,
    before_linux_display.prefer_gpu_native_capture
  );
  EXPECT_EQ(linux_display.auto_manage_displays, before_linux_display.auto_manage_displays);
  EXPECT_EQ(linux_display.headless_swap_mode, before_linux_display.headless_swap_mode);
  EXPECT_EQ(linux_display.streaming_output, before_linux_display.streaming_output);
  EXPECT_EQ(linux_display.primary_output, before_linux_display.primary_output);
  EXPECT_EQ(config::video.capture, before_capture);
  EXPECT_EQ(config::video.output_name, before_output_name);
}

TEST(StreamDisplayPolicyTests, SuccessfulDurableSwitchRetiresOwnedDongleCaptureTarget) {
  LinuxDisplayPolicyGuard guard;
  auto &linux_display = config::video.linux_display;
  linux_display.stream_mode = "headless_dongle";
  linux_display.headless_mode = true;
  linux_display.auto_manage_displays = true;
  linux_display.headless_swap_mode = "privacy";
  linux_display.streaming_output = "DP-1";
  linux_display.primary_output = "eDP-1";
  config::video.capture = "portal";
  config::video.output_name = "DP-1";

  std::string error;
  ASSERT_TRUE(nvhttp::apply_stream_display_mode_selection_for_tests(
    "desktop_display",
    true,
    error
  )) << error;
  EXPECT_EQ(linux_display.stream_mode, "desktop_display");
  EXPECT_FALSE(linux_display.auto_manage_displays);
  EXPECT_TRUE(linux_display.headless_swap_mode.empty());
  EXPECT_TRUE(linux_display.streaming_output.empty());
  EXPECT_TRUE(linux_display.primary_output.empty());
  EXPECT_TRUE(config::video.output_name.empty());
  EXPECT_EQ(config::video.capture, "portal");
}

TEST(StreamDisplayPolicyTests, HostVirtualKeepsAConfiguredConnectorOnlyForKScreen) {
  EXPECT_FALSE(stream_display_policy::host_virtual_backend_creates_output(
    virtual_display::backend_e::KSCREEN_DOCTOR
  ));
  EXPECT_TRUE(stream_display_policy::host_virtual_backend_creates_output(
    virtual_display::backend_e::EVDI
  ));
  EXPECT_TRUE(stream_display_policy::host_virtual_backend_creates_output(
    virtual_display::backend_e::WAYLAND_WLR
  ));
  EXPECT_FALSE(stream_display_policy::host_virtual_backend_creates_output(
    virtual_display::backend_e::NONE
  ));

  EXPECT_TRUE(stream_display_policy::host_virtual_connector_state_matches(
    virtual_display::backend_e::KSCREEN_DOCTOR,
    "DP-1",
    "DP-1"
  ));
  EXPECT_FALSE(stream_display_policy::host_virtual_connector_state_matches(
    virtual_display::backend_e::EVDI,
    "DP-1",
    "DP-1"
  ));
  EXPECT_FALSE(stream_display_policy::host_virtual_connector_state_matches(
    virtual_display::backend_e::WAYLAND_WLR,
    "",
    "DP-1"
  ));
  EXPECT_TRUE(stream_display_policy::host_virtual_connector_state_matches(
    virtual_display::backend_e::EVDI,
    "",
    ""
  ));
}

TEST(StreamDisplayPolicyTests, CreatedHostVirtualBackendOwnsFinalConnectorAndCaptureState) {
  LinuxDisplayPolicyGuard guard;
  auto &linux_display = config::video.linux_display;

  const auto set_stale_kscreen_state = [&]() {
    linux_display.auto_manage_displays = true;
    linux_display.headless_swap_mode = "privacy";
    linux_display.streaming_output = "DP-1";
    linux_display.primary_output = "eDP-1";
    config::video.capture = "portal";
    config::video.output_name = "DP-1";
  };

  set_stale_kscreen_state();
  stream_display_policy::normalize_host_virtual_display_state_for_backend(
    virtual_display::backend_e::KSCREEN_DOCTOR
  );
  EXPECT_FALSE(linux_display.auto_manage_displays);
  EXPECT_TRUE(linux_display.headless_swap_mode.empty());
  EXPECT_EQ(linux_display.streaming_output, "DP-1");
  EXPECT_TRUE(linux_display.primary_output.empty());
  EXPECT_EQ(config::video.output_name, "DP-1");
  EXPECT_EQ(config::video.capture, "portal");

  set_stale_kscreen_state();
  stream_display_policy::normalize_host_virtual_display_state_for_backend(
    virtual_display::backend_e::EVDI
  );
  EXPECT_FALSE(linux_display.auto_manage_displays);
  EXPECT_TRUE(linux_display.headless_swap_mode.empty());
  EXPECT_TRUE(linux_display.streaming_output.empty());
  EXPECT_TRUE(linux_display.primary_output.empty());
  EXPECT_TRUE(config::video.output_name.empty());
  EXPECT_EQ(config::video.capture, "portal");

  set_stale_kscreen_state();
  stream_display_policy::normalize_host_virtual_display_state_for_backend(
    virtual_display::backend_e::WAYLAND_WLR
  );
  EXPECT_TRUE(linux_display.streaming_output.empty());
  EXPECT_TRUE(linux_display.primary_output.empty());
  EXPECT_TRUE(config::video.output_name.empty());
  EXPECT_EQ(config::video.capture, "wlr");
}

TEST(StreamDisplayPolicyTests, LeavingKScreenHostVirtualRetiresItsConnectorAuthority) {
  LinuxDisplayPolicyGuard guard;
  auto &linux_display = config::video.linux_display;
  linux_display.stream_mode = "host_virtual_display";
  linux_display.auto_manage_displays = false;
  linux_display.headless_swap_mode.clear();
  linux_display.streaming_output = "DP-1";
  linux_display.primary_output.clear();
  config::video.capture = "portal";
  config::video.output_name = "DP-1";

  std::string error;
  ASSERT_TRUE(stream_display_policy::apply_selection("desktop_display", error)) << error;
  EXPECT_TRUE(linux_display.streaming_output.empty());
  EXPECT_TRUE(linux_display.primary_output.empty());
  EXPECT_TRUE(config::video.output_name.empty());
}

TEST(StreamDisplayPolicyTests, GamescopeStreamRegisteredWithGamescopeRuntime) {
  const auto options = stream_display_policy::mode_options(false);
  const auto gamescope = std::find_if(options.begin(), options.end(), [](const auto &opt) {
    return opt.value == "gamescope_stream";
  });
  ASSERT_NE(gamescope, options.end());
  EXPECT_EQ(gamescope->runtime, "gamescope");
  EXPECT_EQ(gamescope->capture, "portal");
  // Availability depends on PATH; either way apply must not crash.
  std::string error;
  stream_display_policy::apply_selection("gamescope_stream", error);
}

TEST(StreamDisplayPolicyTests, ExplicitStreamModeWinsOverBooleans) {
  LinuxDisplayPolicyGuard guard;
  config::video.linux_display.stream_mode = "host_virtual_display";
  config::video.linux_display.headless_mode = true;
  config::video.linux_display.use_cage_compositor = true;  // would be private stream if mode empty
  config::video.linux_display.prefer_gpu_native_capture = false;

  const auto resolved = stream_display_policy::resolve(stream_display_policy::input_t {true, false, false});
  EXPECT_EQ(resolved.selection, "host_virtual_display");
  EXPECT_TRUE(resolved.use_host_virtual_display);
  EXPECT_FALSE(resolved.use_private_runtime);
}

TEST(StreamDisplayPolicyTests, NormalizeConfigClearsStaleGamescopeRuntime) {
  LinuxDisplayPolicyGuard guard;
  auto &d = config::video.linux_display;
  for (const auto mode : {"desktop_display", "host_virtual_display", "headless_dongle"}) {
    d.stream_mode = mode;
    d.private_runtime = "gamescope";
    stream_display_policy::normalize_config_from_load();
    EXPECT_TRUE(d.private_runtime.empty()) << mode;
  }
}

TEST(StreamDisplayPolicyTests, PrivateStreamLoadRetiresTheConnectorButKeepsHostVirtualDisplayOffered) {
  // #633: a Private Stream host saved linux_streaming_output = DP-1 for the
  // kscreen-doctor fallback. The default headless_swap_mode ("privacy") made
  // the load retire it, Host Virtual Display then read as unconfigured, and the
  // card stayed greyed out asking for the connector the file already had.
  LinuxDisplayPolicyGuard guard;
  auto &d = config::video.linux_display;
  d.stream_mode = "headless_stream";
  d.streaming_output = "DP-1";
  d.saved_streaming_output = "DP-1";
  d.primary_output = "DP-1";
  d.auto_manage_displays = false;
  d.headless_swap_mode = "privacy";
  config::video.output_name.clear();

  stream_display_policy::normalize_config_from_load();
  EXPECT_TRUE(d.streaming_output.empty()) << "a private mode must not pin capture to the connector";
  EXPECT_TRUE(d.primary_output.empty());
  EXPECT_EQ(d.saved_streaming_output, "DP-1");
  EXPECT_EQ(virtual_display::host_virtual_display_connector(), "DP-1");
  EXPECT_TRUE(virtual_display::backend_has_required_configuration(
    virtual_display::backend_e::KSCREEN_DOCTOR,
    virtual_display::host_virtual_display_connector()
  ));
}

TEST(StreamDisplayPolicyTests, ConnectorlessModesRetireTheConnectorWhateverTheSwapFlagsSay) {
  // The old condition (leftover auto-management or headless_swap_mode) could
  // not be false on load, because headless_swap_mode cannot load empty. Pin
  // that retirement no longer depends on it at all.
  LinuxDisplayPolicyGuard guard;
  auto &d = config::video.linux_display;
  for (const auto mode : {"headless_stream", "windowed_stream", "desktop_display"}) {
    d.stream_mode = mode;
    d.streaming_output = "HDMI-A-2";
    d.saved_streaming_output = "HDMI-A-2";
    d.primary_output.clear();
    d.auto_manage_displays = false;
    d.headless_swap_mode.clear();
    config::video.output_name = "HDMI-A-2";

    stream_display_policy::normalize_config_from_load();
    EXPECT_TRUE(d.streaming_output.empty()) << mode;
    EXPECT_TRUE(config::video.output_name.empty()) << mode;
    EXPECT_EQ(d.saved_streaming_output, "HDMI-A-2") << mode;
  }
}

TEST(StreamDisplayPolicyTests, EnteringKScreenHostVirtualBorrowsTheSavedConnector) {
  LinuxDisplayPolicyGuard guard;
  auto &d = config::video.linux_display;

  d.streaming_output.clear();
  d.saved_streaming_output = "DP-1";
  stream_display_policy::normalize_host_virtual_display_state_for_backend(
    virtual_display::backend_e::KSCREEN_DOCTOR
  );
  EXPECT_EQ(d.streaming_output, "DP-1");

  // An active connector wins over the saved one.
  d.streaming_output = "HDMI-A-2";
  stream_display_policy::normalize_host_virtual_display_state_for_backend(
    virtual_display::backend_e::KSCREEN_DOCTOR
  );
  EXPECT_EQ(d.streaming_output, "HDMI-A-2");

  // Backends that create their own output never take one.
  for (const auto backend : {virtual_display::backend_e::EVDI, virtual_display::backend_e::WAYLAND_WLR,
                             virtual_display::backend_e::KWIN_VIRTUAL_OUTPUT}) {
    d.streaming_output.clear();
    stream_display_policy::normalize_host_virtual_display_state_for_backend(backend);
    EXPECT_TRUE(d.streaming_output.empty());
  }
  EXPECT_EQ(d.saved_streaming_output, "DP-1");
}

TEST(StreamDisplayPolicyTests, ModeSwitchLeavesTheConnectorInTheFile) {
  LinuxDisplayPolicyGuard guard;
  auto &d = config::video.linux_display;
  d.stream_mode = "headless_stream";
  d.headless_mode = true;
  d.use_cage_compositor = true;
  d.streaming_output.clear();  // retired on load
  d.saved_streaming_output = "DP-1";
  d.primary_output.clear();
  d.auto_manage_displays = false;
  d.headless_swap_mode.clear();
  config::video.output_name.clear();

  std::unordered_map<std::string, std::string> persisted;
  std::string error;
  ASSERT_TRUE(nvhttp::apply_stream_display_mode_selection_for_tests("desktop_display", persisted, error)) << error;
  EXPECT_EQ(persisted.at("linux_stream_mode"), "desktop_display");
  // Writing "" would delete the key (the #633 erasure); writing the saved copy
  // would overwrite a web edit still waiting for a restart. Neither is sent.
  EXPECT_FALSE(persisted.contains("linux_streaming_output"));
  EXPECT_TRUE(d.streaming_output.empty());
  EXPECT_EQ(d.saved_streaming_output, "DP-1");

  // Leaving a dongle retires its live connector and leaves the file's alone.
  d.stream_mode = "headless_dongle";
  d.auto_manage_displays = true;
  d.headless_swap_mode = "privacy";
  d.streaming_output = "HDMI-A-2";
  d.saved_streaming_output = "HDMI-A-2";
  d.primary_output = "eDP-1";
  config::video.output_name = "HDMI-A-2";
  persisted.clear();
  ASSERT_TRUE(nvhttp::apply_stream_display_mode_selection_for_tests("desktop_display", persisted, error)) << error;
  EXPECT_FALSE(persisted.contains("linux_streaming_output"));
  EXPECT_EQ(persisted.at("output_name"), "");
  EXPECT_EQ(d.saved_streaming_output, "HDMI-A-2");
  EXPECT_TRUE(d.streaming_output.empty());
  EXPECT_TRUE(config::video.output_name.empty());
}

TEST(StreamDisplayPolicyTests, NormalizeConfigRepairsHostVirtualState) {
  LinuxDisplayPolicyGuard guard;
  auto &d = config::video.linux_display;
  d.stream_mode = "host_virtual_display";
  d.auto_manage_displays = true;
  stream_display_policy::normalize_config_from_load();
  EXPECT_FALSE(d.auto_manage_displays);

  d.stream_mode.clear();
  d.headless_mode = true;
  d.use_cage_compositor = false;
  d.auto_manage_displays = true;
  stream_display_policy::normalize_config_from_load();
  EXPECT_EQ(d.stream_mode, "host_virtual_display");
  EXPECT_FALSE(d.auto_manage_displays);
}

TEST(StreamDisplayPolicyTests, NormalizeConfigDerivesStreamModeFromLegacyBooleans) {
  LinuxDisplayPolicyGuard guard;
  config::video.linux_display.stream_mode.clear();
  config::video.linux_display.headless_mode = true;
  config::video.linux_display.use_cage_compositor = true;
  config::video.linux_display.prefer_gpu_native_capture = false;
  config::video.linux_display.private_runtime.clear();

  stream_display_policy::normalize_config_from_load();

  EXPECT_EQ(config::video.linux_display.stream_mode, "headless_stream");
  EXPECT_EQ(config::video.linux_display.private_runtime, "labwc");
}

TEST(StreamDisplayPolicyTests, AllowedLaunchModesExcludeUnavailableByDefault) {
  const auto allowed = stream_display_policy::allowed_launch_modes(true, false);
  const bool headless_listed =
    std::find(allowed.begin(), allowed.end(), "headless_stream") != allowed.end();
  EXPECT_EQ(headless_listed, stream_display_policy::selection_available("headless_stream"));
  EXPECT_NE(std::find(allowed.begin(), allowed.end(), "host_virtual_display"), allowed.end());
  // gamescope_stream is available when gamescope is on PATH (may or may not be listed).
  // Unwired reserved path ids are not registered.
  EXPECT_EQ(std::find(allowed.begin(), allowed.end(), "family_isolated"), allowed.end());
  EXPECT_EQ(std::find(allowed.begin(), allowed.end(), "headless_evdi"), allowed.end());
}

TEST(StreamDisplayPolicyTests, LabwcPathsRequireLabwcAndWlrRandr) {
  stream_path::host_capabilities_t caps;
  caps.labwc_present = true;
  caps.wlr_randr_present = false;

  auto options = stream_path::options_for_host(caps);
  auto headless = std::find_if(options.begin(), options.end(), [](const auto &opt) {
    return opt.id == stream_path::k_headless_stream;
  });
  ASSERT_NE(headless, options.end());
  EXPECT_FALSE(headless->available);
  EXPECT_EQ(headless->unavailable_reason, "wlr-randr binary not found on PATH");

  caps.labwc_present = false;
  caps.wlr_randr_present = true;
  options = stream_path::options_for_host(caps);
  headless = std::find_if(options.begin(), options.end(), [](const auto &opt) {
    return opt.id == stream_path::k_headless_stream;
  });
  ASSERT_NE(headless, options.end());
  EXPECT_FALSE(headless->available);
  EXPECT_EQ(headless->unavailable_reason, "labwc binary not found on PATH");

  caps.labwc_present = true;
  options = stream_path::options_for_host(caps);
  headless = std::find_if(options.begin(), options.end(), [](const auto &opt) {
    return opt.id == stream_path::k_headless_stream;
  });
  ASSERT_NE(headless, options.end());
  EXPECT_TRUE(headless->available);
  EXPECT_TRUE(headless->unavailable_reason.empty());
}

TEST(StreamDisplayPolicyTests, ModeOptionsOwnDynamicRuntimeUnavailableReasons) {
  stream_path::host_capabilities_t caps;
  caps.labwc_present = false;
  caps.wlr_randr_present = false;

  const auto options = stream_path::options_for_host(caps);
  const auto headless = std::find_if(options.begin(), options.end(), [](const auto &opt) {
    return opt.id == stream_path::k_headless_stream;
  });
  ASSERT_NE(headless, options.end());

  // The catalog outlives the probe's temporary result and is serialized later
  // by the HTTP/UI layers. This copy must never read a dangling string_view.
  const std::string serialized_reason = headless->unavailable_reason;
  EXPECT_EQ(serialized_reason, "labwc and wlr-randr binaries not found on PATH");
}

TEST(StreamDisplayPolicyTests, ModeOptionsMatchSelectionAvailableForGamescope) {
  // Dual-truth footgun: mode_options must apply the same gamescope_present
  // probe as selection_available / apply_selection.
  const auto options = stream_display_policy::mode_options(false);
  const auto gamescope = std::find_if(options.begin(), options.end(), [](const auto &opt) {
    return opt.value == "gamescope_stream";
  });
  ASSERT_NE(gamescope, options.end());
  EXPECT_EQ(gamescope->available, stream_display_policy::selection_available("gamescope_stream"));
  if (!gamescope->available) {
    EXPECT_FALSE(gamescope->unavailable_reason.empty());
  }

  const auto allowed = stream_display_policy::allowed_launch_modes(true, false);
  const bool listed = std::find(allowed.begin(), allowed.end(), "gamescope_stream") != allowed.end();
  EXPECT_EQ(listed, gamescope->available);
}

TEST(StreamDisplayPolicyTests, ModeOptionsExposeRuntimeCaptureTopologyForPlugins) {
  const auto options = stream_display_policy::mode_options(false);
  const auto gamescope = std::find_if(options.begin(), options.end(), [](const auto &opt) {
    return opt.value == "gamescope_stream";
  });
  ASSERT_NE(gamescope, options.end());
  EXPECT_EQ(gamescope->runtime, "gamescope");
  EXPECT_EQ(gamescope->capture, "portal");

  const auto headless = std::find_if(options.begin(), options.end(), [](const auto &opt) {
    return opt.value == "headless_stream";
  });
  ASSERT_NE(headless, options.end());
  EXPECT_EQ(headless->runtime, "labwc");
  EXPECT_EQ(headless->capture, "wlroots");
  EXPECT_EQ(headless->badge, "Recommended");
  EXPECT_EQ(headless->available, stream_display_policy::selection_available("headless_stream"));
  if (!headless->available) {
    EXPECT_FALSE(headless->unavailable_reason.empty());
  }
}

TEST(StreamDisplayPolicyTests, DesktopPathReportsHonestPortalOrHostBackend) {
  LinuxDisplayPolicyGuard guard;
  ASSERT_TRUE([&] {
    std::string error;
    return stream_display_policy::apply_selection("desktop_display", error);
  }());

  const auto resolved = stream_display_policy::resolve(stream_display_policy::input_t {});
  EXPECT_EQ(resolved.selection, "desktop_display");
  EXPECT_FALSE(resolved.backend_name.empty());
  EXPECT_NE(resolved.backend_name, "labwc");
}

namespace {
  /// What an operator reads in the log, severity included: whether a line is a warning is the point.
  class CaptureLogCapture {
  public:
    CaptureLogCapture():
        stream_ {boost::make_shared<std::ostringstream>()} {
      auto backend = boost::make_shared<boost::log::sinks::text_ostream_backend>();
      backend->add_stream(boost::shared_ptr<std::ostream> {stream_.get(), boost::null_deleter {}});
      backend->auto_flush(true);
      sink_ = boost::make_shared<sink_t>(backend);
      sink_->set_formatter(&logging::formatter);
      boost::log::core::get()->add_sink(sink_);
    }

    ~CaptureLogCapture() {
      boost::log::core::get()->remove_sink(sink_);
    }

    CaptureLogCapture(const CaptureLogCapture &) = delete;
    CaptureLogCapture &operator=(const CaptureLogCapture &) = delete;

    [[nodiscard]] std::string text() const {
      return stream_->str();
    }

  private:
    using sink_t = boost::log::sinks::synchronous_sink<boost::log::sinks::text_ostream_backend>;
    boost::shared_ptr<std::ostringstream> stream_;
    boost::shared_ptr<sink_t> sink_;
  };

  /// Every captured line that mentions needle, so another thread's logging cannot pass for ours.
  std::string lines_with(const std::string &logged, std::string_view needle) {
    std::istringstream input {logged};
    std::string out;
    for (std::string line; std::getline(input, line);) {
      if (line.find(needle) != std::string::npos) {
        out += line;
        out += '\n';
      }
    }
    return out;
  }

  const std::vector<std::string> k_capture_values {"", "auto", "wlr", "wlroots", "portal", "kms", "kwin", "drm", "nvfbc", "x11"};
  const std::vector<std::string> k_stream_modes {
    "headless_stream",
    "windowed_stream",
    "desktop_display",
    "host_virtual_display",
    "desktop_takeover",
    "gamescope_stream",
    "headless_dongle",
  };

  bool capture_is_auto(std::string_view capture) {
    return capture.empty() || capture == "auto";
  }

  /// kwin replaced by portal is the same backend under another name, not a rewrite.
  bool same_backend(std::string_view a, std::string_view b) {
    return stream_display_policy::canonical_capture_backend(a) == stream_display_policy::canonical_capture_backend(b);
  }

  /**
   * Make value the capture setting polaris.conf was loaded with, and leave the live configuration
   * as the test set it. A rewrite names an explicit choice by this, not by the live value.
   */
  void set_loaded_capture_setting(std::string value) {
    const auto linux_display = config::video.linux_display;
    const auto capture = config::video.capture;
    const auto output_name = config::video.output_name;
    config::video.linux_display.stream_mode = "desktop_display";
    config::video.capture = std::move(value);
    stream_display_policy::normalize_config_from_load();
    {
      CaptureLogCapture quiet;
      stream_display_policy::log_config_load_notes();
    }
    config::video.linux_display = linux_display;
    config::video.capture = capture;
    config::video.output_name = output_name;
  }

  /// The loaded setting is process state too, so a test that sets it puts the old one back.
  struct LoadedCaptureGuard {
    std::string previous {stream_display_policy::loaded_capture_setting()};

    LoadedCaptureGuard() = default;
    LoadedCaptureGuard(const LoadedCaptureGuard &) = delete;
    LoadedCaptureGuard &operator=(const LoadedCaptureGuard &) = delete;

    ~LoadedCaptureGuard() {
      set_loaded_capture_setting(previous);
    }
  };
}  // namespace

TEST(StreamDisplayPolicyTests, CaptureModeOverrideNamesTheExplicitChoiceAModeSetsAside) {
  using stream_display_policy::capture_mode_override;

  // capture = kms in windowed_stream streams through wlr, and until now only the rewritten value
  // reached the log, so the host read as being on the KMS path.
  const auto windowed = capture_mode_override("kms", "windowed_stream", true, false, false);
  ASSERT_TRUE(windowed.has_value());
  EXPECT_EQ(windowed->configured, "kms");
  EXPECT_EQ(windowed->effective, "wlr");
  EXPECT_EQ(windowed->reason, stream_display_policy::k_capture_override_private_compositor);

  // Auto asked the host to choose, so the host choosing is not a rewrite. Nor is a choice that stands.
  EXPECT_FALSE(capture_mode_override("", "headless_stream", true, false, false).has_value());
  EXPECT_FALSE(capture_mode_override("auto", "headless_stream", true, false, false).has_value());
  EXPECT_FALSE(capture_mode_override("wlr", "headless_stream", true, false, false).has_value());
  EXPECT_FALSE(capture_mode_override("portal", "desktop_display", false, false, false).has_value());
  EXPECT_FALSE(capture_mode_override("kms", "desktop_display", false, false, false).has_value());

  // #739: a substituted backend is asked for through auto.
  const auto substituted = capture_mode_override("wlr", "desktop_display", false, true, false);
  ASSERT_TRUE(substituted.has_value());
  EXPECT_EQ(substituted->configured, "wlr");
  EXPECT_EQ(substituted->effective, "");
  EXPECT_EQ(substituted->reason, stream_display_policy::k_capture_override_substituted);

  // An output the generation owns keeps the configured backend through a substitution, because
  // auto cannot address it. Without that ownership the same substitution rewrites it.
  EXPECT_FALSE(capture_mode_override("portal", "host_virtual_display", false, true, true).has_value());
  const auto unowned = capture_mode_override("portal", "host_virtual_display", false, true, false);
  ASSERT_TRUE(unowned.has_value());
  EXPECT_EQ(unowned->effective, "");
  EXPECT_EQ(unowned->reason, stream_display_policy::k_capture_override_substituted);
  // The private compositor wins over an owned output: nothing else can capture it.
  const auto owned_private = capture_mode_override("portal", "headless_stream", true, false, true);
  ASSERT_TRUE(owned_private.has_value());
  EXPECT_EQ(owned_private->effective, "wlr");

  // Gamescope keeps the configured backend, so a substitution there rewrites nothing.
  EXPECT_FALSE(capture_mode_override("portal", "gamescope_stream", false, true, false).has_value());

  // An alias is an explicit choice like any other.
  const auto drm = capture_mode_override("drm", "headless_stream", true, false, false);
  ASSERT_TRUE(drm.has_value());
  EXPECT_EQ(drm->configured, "drm");
  EXPECT_EQ(drm->effective, "wlr");
}

TEST(StreamDisplayPolicyTests, CaptureModeOverrideNeverDisagreesWithCaptureForMode) {
  using stream_display_policy::capture_for_mode;
  using stream_display_policy::capture_mode_override;

  for (const auto &capture : k_capture_values) {
    for (const auto &mode : k_stream_modes) {
      for (const bool cage : {false, true}) {
        for (const bool substitution : {false, true}) {
          for (const bool exact : {false, true}) {
            const auto effective = capture_for_mode(capture, mode, cage, substitution, exact);
            const auto override = capture_mode_override(capture, mode, cage, substitution, exact);
            const auto where = "capture=[" + capture + "] mode=[" + mode + "] cage=" + std::to_string(cage) +
                               " substitution=" + std::to_string(substitution) + " exact=" + std::to_string(exact);
            if (override) {
              EXPECT_EQ(override->configured, capture) << where;
              EXPECT_EQ(override->effective, effective) << where;
              EXPECT_FALSE(capture_is_auto(capture)) << where;
              EXPECT_FALSE(override->reason.empty()) << where;
            } else {
              EXPECT_TRUE(capture_is_auto(capture) || same_backend(effective, capture))
                << where << " rewrote the configured backend to [" << effective << "] without saying so";
            }
          }
        }
      }
    }
  }
}

TEST(StreamDisplayPolicyTests, CurrentModeOverrideReadsWhatCurrentModeCaptureReads) {
  LinuxDisplayPolicyGuard guard;
  LoadedCaptureGuard loaded_guard;
  struct SubstitutionGuard {
    ~SubstitutionGuard() {
      platf::set_capture_backend_substitution_for_tests("");
    }
  } substitution_guard;
  auto &d = config::video.linux_display;
  set_loaded_capture_setting("kms");
  d.stream_mode = "windowed_stream";
  d.headless_mode = false;
  d.use_cage_compositor = true;
  config::video.capture = "kms";
  platf::set_capture_backend_substitution_for_tests("");

  const auto cage = stream_display_policy::capture_mode_override_for_current_mode();
  ASSERT_TRUE(cage.has_value());
  EXPECT_EQ(cage->effective, stream_display_policy::capture_for_current_mode());

  set_loaded_capture_setting("portal");
  d.stream_mode = "host_virtual_display";
  d.use_cage_compositor = false;
  config::video.capture = "portal";
  platf::set_capture_backend_substitution_for_tests("portal -> kms");
  const auto unowned = stream_display_policy::capture_mode_override_for_current_mode();
  ASSERT_TRUE(unowned.has_value());
  EXPECT_EQ(unowned->reason, stream_display_policy::k_capture_override_substituted);
  EXPECT_FALSE(stream_display_policy::capture_mode_override_for_current_mode(true).has_value())
    << "an owned output keeps the configured backend";
}

TEST(StreamDisplayPolicyTests, SessionTransitionOverrideNamesTheExplicitChoiceItDiscards) {
  using stream_display_policy::capture_session_transition_override;

  // A Gamescope or dongle session forces portal even over an explicit kms, which apply and load keep.
  const auto gamescope = capture_session_transition_override("desktop_display", "gamescope_stream", "kms");
  ASSERT_TRUE(gamescope.has_value());
  EXPECT_EQ(gamescope->configured, "kms");
  EXPECT_EQ(gamescope->effective, "portal");
  EXPECT_EQ(gamescope->reason, stream_display_policy::k_capture_override_gamescope_session);

  const auto dongle = capture_session_transition_override("desktop_display", "headless_dongle", "kms");
  ASSERT_TRUE(dongle.has_value());
  EXPECT_EQ(dongle->effective, "portal");
  EXPECT_EQ(dongle->reason, stream_display_policy::k_capture_override_dongle_session);

  // Mirror Desktop clears an explicit wlroots choice so desktop discovery can pick.
  for (const auto capture : {"wlr", "wlroots"}) {
    const auto mirror = capture_session_transition_override("headless_stream", "desktop_display", capture);
    ASSERT_TRUE(mirror.has_value()) << capture;
    EXPECT_EQ(mirror->configured, capture);
    EXPECT_EQ(mirror->effective, "");
    EXPECT_EQ(mirror->reason, stream_display_policy::k_capture_override_desktop_discovery);
  }

  const auto private_session = capture_session_transition_override("desktop_display", "headless_stream", "kms");
  ASSERT_TRUE(private_session.has_value());
  EXPECT_EQ(private_session->effective, "wlr");
  EXPECT_EQ(private_session->reason, stream_display_policy::k_capture_override_private_compositor);

  // Auto stays auto, compatible choices stand, and no transition means no override.
  EXPECT_FALSE(capture_session_transition_override("headless_stream", "desktop_display", "auto").has_value());
  EXPECT_FALSE(capture_session_transition_override("desktop_display", "headless_stream", "").has_value());
  EXPECT_FALSE(capture_session_transition_override("desktop_display", "gamescope_stream", "").has_value());
  EXPECT_FALSE(capture_session_transition_override("headless_stream", "desktop_display", "kms").has_value());
  EXPECT_FALSE(capture_session_transition_override("desktop_display", "gamescope_stream", "portal").has_value());
  EXPECT_FALSE(capture_session_transition_override("headless_stream", "headless_stream", "kms").has_value());
}

TEST(StreamDisplayPolicyTests, SessionTransitionOverrideNeverDisagreesWithTheTransition) {
  using stream_display_policy::capture_for_session_transition;
  using stream_display_policy::capture_session_transition_override;

  for (const auto &configured : k_stream_modes) {
    for (const auto &session : k_stream_modes) {
      for (const auto &capture : k_capture_values) {
        const auto effective = capture_for_session_transition(configured, session, capture);
        const auto override = capture_session_transition_override(configured, session, capture);
        const auto where = configured + " -> " + session + " capture=[" + capture + "]";
        if (override) {
          EXPECT_EQ(override->configured, capture) << where;
          EXPECT_EQ(override->effective, effective) << where;
          EXPECT_FALSE(capture_is_auto(capture)) << where;
        } else {
          EXPECT_TRUE(capture_is_auto(capture) || same_backend(effective, capture))
            << where << " rewrote the configured backend to [" << effective << "] without saying so";
        }
      }
    }
  }
}

TEST(StreamDisplayPolicyTests, HostVirtualDisplayOverrideNamesTheBackendsCapture) {
  using stream_display_policy::capture_for_host_virtual_display_backend;
  using stream_display_policy::capture_host_virtual_display_override;
  using virtual_display::backend_e;

  const auto evdi = capture_host_virtual_display_override(backend_e::EVDI, "kms");
  ASSERT_TRUE(evdi.has_value());
  EXPECT_EQ(evdi->configured, "kms");
  EXPECT_EQ(evdi->effective, "portal");
  EXPECT_EQ(evdi->reason, stream_display_policy::k_capture_override_virtual_display_backend);

  const auto wlr = capture_host_virtual_display_override(backend_e::WAYLAND_WLR, "portal");
  ASSERT_TRUE(wlr.has_value());
  EXPECT_EQ(wlr->effective, "wlr");

  EXPECT_FALSE(capture_host_virtual_display_override(backend_e::EVDI, "").has_value());
  EXPECT_FALSE(capture_host_virtual_display_override(backend_e::EVDI, "kwin").has_value())
    << "kwin is the portal under another name";
  EXPECT_FALSE(capture_host_virtual_display_override(backend_e::KWIN_VIRTUAL_OUTPUT, "portal").has_value());
  EXPECT_FALSE(capture_host_virtual_display_override(backend_e::NONE, "kms").has_value());

  for (const auto backend : {backend_e::NONE, backend_e::EVDI, backend_e::WAYLAND_WLR,
                             backend_e::KSCREEN_DOCTOR, backend_e::KWIN_VIRTUAL_OUTPUT}) {
    for (const auto &capture : k_capture_values) {
      const auto effective = capture_for_host_virtual_display_backend(backend, capture);
      const auto override = capture_host_virtual_display_override(backend, capture);
      const auto where = std::string {virtual_display::backend_name(backend)} + " capture=[" + capture + "]";
      if (override) {
        EXPECT_EQ(override->effective, effective) << where;
        EXPECT_FALSE(capture_is_auto(capture)) << where;
      } else {
        EXPECT_TRUE(capture_is_auto(capture) || same_backend(effective, capture)) << where;
      }
    }
  }
}

TEST(StreamDisplayPolicyTests, ACaptureOverrideLineNamesModeConfiguredEffectiveAndReason) {
  using stream_display_policy::capture_override_t;
  using stream_display_policy::describe_capture_override;

  const auto windowed = describe_capture_override(
    capture_override_t {"kms", "wlr", std::string {stream_display_policy::k_capture_override_private_compositor}},
    "windowed_stream"
  );
  EXPECT_NE(windowed.find("capture override:"), std::string::npos) << windowed;
  EXPECT_NE(windowed.find("[windowed_stream]"), std::string::npos) << windowed;
  EXPECT_NE(windowed.find("[kms]"), std::string::npos) << windowed;
  EXPECT_NE(windowed.find("[wlr]"), std::string::npos) << windowed;
  EXPECT_NE(windowed.find("labwc"), std::string::npos) << windowed;

  const auto substituted = describe_capture_override(
    capture_override_t {"wlr", "", std::string {stream_display_policy::k_capture_override_substituted}},
    "desktop_display"
  );
  EXPECT_NE(substituted.find("[auto]"), std::string::npos) << "an empty backend is auto: " << substituted;

  for (const auto reason : {
         stream_display_policy::k_capture_override_private_compositor,
         stream_display_policy::k_capture_override_substituted,
         stream_display_policy::k_capture_override_gamescope_session,
         stream_display_policy::k_capture_override_dongle_session,
         stream_display_policy::k_capture_override_desktop_discovery,
         stream_display_policy::k_capture_override_virtual_display_backend,
       }) {
    const auto line = describe_capture_override(capture_override_t {"kms", "portal", std::string {reason}}, "");
    EXPECT_NE(line.find("[unset]"), std::string::npos) << line;
    EXPECT_EQ(line.find(reason), std::string::npos) << "the reason is said in words, not as its id: " << line;
    EXPECT_EQ(line.find(" - "), std::string::npos) << line;
    EXPECT_EQ(line.find("\xE2\x80\x94"), std::string::npos) << line;
    EXPECT_EQ(line.find("\xE2\x80\x93"), std::string::npos) << line;
  }
}

TEST(StreamDisplayPolicyTests, AHostVirtualDisplayThatSetsAsideAnExplicitCaptureSaysSoAsAWarning) {
  LinuxDisplayPolicyGuard guard;
  LoadedCaptureGuard loaded_guard;
  auto &d = config::video.linux_display;
  set_loaded_capture_setting("kms");
  d.stream_mode = "host_virtual_display";
  config::video.capture = "kms";

  std::string logged;
  {
    CaptureLogCapture log;
    stream_display_policy::normalize_host_virtual_display_state_for_backend(virtual_display::backend_e::EVDI);
    logged = log.text();
  }
  EXPECT_EQ(config::video.capture, "portal");
  const auto warning = lines_with(logged, "[kms]");
  EXPECT_NE(warning.find("Warning: "), std::string::npos) << logged;
  EXPECT_NE(warning.find("[host_virtual_display]"), std::string::npos) << warning;
  EXPECT_NE(warning.find("[portal]"), std::string::npos) << warning;
  EXPECT_NE(warning.find("[EVDI]"), std::string::npos) << warning;
  // A launch entering the mode puts the host setting back at teardown, and the line says so.
  EXPECT_NE(warning.find("for this session only"), std::string::npos) << warning;
  EXPECT_NE(warning.find("comes back at teardown"), std::string::npos) << warning;

  // apply_selection normalizes before it records the mode it is entering, so it names the mode.
  d.stream_mode = "desktop_display";
  config::video.capture = "kms";
  {
    CaptureLogCapture log;
    stream_display_policy::normalize_host_virtual_display_state_for_backend(
      virtual_display::backend_e::EVDI,
      stream_display_policy::capture_rewrite_scope_e::session,
      "desktop_takeover"
    );
    logged = log.text();
  }
  EXPECT_NE(lines_with(logged, "[kms]").find("[desktop_takeover]"), std::string::npos) << logged;

  // Auto asked the host to choose: that is worth a line, not a warning.
  set_loaded_capture_setting("");
  d.stream_mode = "host_virtual_display";
  config::video.capture.clear();
  {
    CaptureLogCapture log;
    stream_display_policy::normalize_host_virtual_display_state_for_backend(virtual_display::backend_e::EVDI);
    logged = log.text();
  }
  EXPECT_EQ(config::video.capture, "portal");
  const auto auto_line = lines_with(logged, "[portal]");
  EXPECT_NE(auto_line.find("Info: "), std::string::npos) << logged;
  EXPECT_EQ(auto_line.find("Warning: "), std::string::npos) << logged;
}

TEST(StreamDisplayPolicyTests, EachEvaluationSaysOnceWhenTheModeSetsAnExplicitCaptureAside) {
  LinuxDisplayPolicyGuard guard;
  LoadedCaptureGuard loaded_guard;
  struct SubstitutionGuard {
    ~SubstitutionGuard() {
      platf::set_capture_backend_substitution_for_tests("");
    }
  } substitution_guard;
  auto &d = config::video.linux_display;
  const auto said = []() {
    CaptureLogCapture log;
    platf::log_capture_mode_override();
    return lines_with(log.text(), "capture override:");
  };

  // A private compositor mode streams through wlr whatever capture says: one warning, naming the
  // mode, the configured backend and the one it asks for instead.
  set_loaded_capture_setting("kms");
  d.stream_mode = "windowed_stream";
  d.use_cage_compositor = true;
  config::video.capture = "kms";
  platf::set_capture_backend_substitution_for_tests("");
  const auto windowed = said();
  EXPECT_EQ(std::count(windowed.begin(), windowed.end(), '\n'), 1) << windowed;
  EXPECT_NE(windowed.find("Warning: "), std::string::npos) << windowed;
  EXPECT_NE(windowed.find("[windowed_stream]"), std::string::npos) << windowed;
  EXPECT_NE(windowed.find("asks for [wlr], not the configured [kms]"), std::string::npos) << windowed;

  // A substitution is an override too, and it has its own warning, which names the substitute.
  // Saying it here as well gave one substitution two warnings.
  set_loaded_capture_setting("wlr");
  d.stream_mode = "desktop_display";
  d.use_cage_compositor = false;
  config::video.capture = "wlr";
  platf::set_capture_backend_substitution_for_tests("wlr -> kms");
  ASSERT_TRUE(stream_display_policy::capture_mode_override_for_current_mode().has_value());
  EXPECT_TRUE(said().empty());

  // Auto asked the host to choose, so the host choosing is nothing to warn about.
  set_loaded_capture_setting("");
  d.stream_mode = "headless_stream";
  d.use_cage_compositor = true;
  config::video.capture.clear();
  platf::set_capture_backend_substitution_for_tests("");
  EXPECT_TRUE(said().empty());
}

TEST(StreamDisplayPolicyTests, AnEvaluationNamesPolarisConfNotTheReplacementALoadMade) {
  // A Host Virtual Display load puts portal in place of capture in memory, and a client's mode
  // switch puts back the value it found, which is that replacement. Named as the configured backend,
  // the replacement told a host set to kms that it had chosen portal, and warned a host with capture
  // unset, or set to the very wlr the mode asks for, that an explicit portal was set aside.
  LinuxDisplayPolicyGuard guard;
  LoadedCaptureGuard loaded_guard;
  struct SubstitutionGuard {
    ~SubstitutionGuard() {
      platf::set_capture_backend_substitution_for_tests("");
    }
  } substitution_guard;
  platf::set_capture_backend_substitution_for_tests("");
  auto &d = config::video.linux_display;
  const auto load_host_virtual_display = [&d](const std::string &capture) {
    set_loaded_capture_setting(capture);
    d.stream_mode = "host_virtual_display";
    d.use_cage_compositor = false;
    config::video.capture = capture;
    CaptureLogCapture quiet;
    stream_display_policy::normalize_host_virtual_display_state_for_backend(
      virtual_display::backend_e::EVDI,
      stream_display_policy::capture_rewrite_scope_e::load
    );
    stream_display_policy::log_config_load_notes();
  };
  const auto evaluate_in_windowed_stream = [&d]() {
    d.stream_mode = "windowed_stream";
    d.use_cage_compositor = true;
    CaptureLogCapture log;
    platf::log_capture_mode_override();
    return lines_with(log.text(), "capture override:");
  };

  load_host_virtual_display("kms");
  ASSERT_EQ(config::video.capture, "portal");
  const auto explicit_kms = evaluate_in_windowed_stream();
  EXPECT_EQ(std::count(explicit_kms.begin(), explicit_kms.end(), '\n'), 1) << explicit_kms;
  EXPECT_NE(explicit_kms.find("Warning: "), std::string::npos) << explicit_kms;
  EXPECT_NE(explicit_kms.find("asks for [wlr], not the configured [kms]"), std::string::npos) << explicit_kms;
  EXPECT_EQ(explicit_kms.find("[portal]"), std::string::npos) << explicit_kms;

  load_host_virtual_display("");
  ASSERT_EQ(config::video.capture, "portal");
  EXPECT_TRUE(evaluate_in_windowed_stream().empty()) << "auto asked the host to choose";

  load_host_virtual_display("wlr");
  ASSERT_EQ(config::video.capture, "portal");
  EXPECT_TRUE(evaluate_in_windowed_stream().empty()) << "the mode asks for the wlr polaris.conf names";
}

TEST(StreamDisplayPolicyTests, ASessionTransitionMeasuresItsRewriteAgainstPolarisConf) {
  // A launch into another mode rewrites capture for the session. It named the live value as the
  // configured one, so after a Host Virtual Display load a host set to kms read that it had chosen
  // portal, and a host with capture unset was warned about a portal it never chose.
  LinuxDisplayPolicyGuard guard;
  LoadedCaptureGuard loaded_guard;
  auto &d = config::video.linux_display;
  const auto enter = [](std::string_view configured, std::string_view session) {
    CaptureLogCapture log;
    stream_display_policy::apply_capture_for_session_transition(configured, session);
    return log.text();
  };

  // polaris.conf says kms, and the Host Virtual Display load put portal in its place.
  set_loaded_capture_setting("kms");
  d.stream_mode = "host_virtual_display";
  config::video.capture = "portal";
  auto logged = enter("host_virtual_display", "headless_stream");
  EXPECT_EQ(config::video.capture, "wlr");
  const auto warning = lines_with(logged, "capture override:");
  EXPECT_NE(warning.find("Warning: "), std::string::npos) << logged;
  EXPECT_NE(warning.find("stream mode [headless_stream] asks for [wlr], not the configured [kms]"), std::string::npos)
    << warning;
  EXPECT_NE(warning.find("for this session only"), std::string::npos) << warning;
  EXPECT_EQ(warning.find("[portal]"), std::string::npos) << warning;

  // With capture unset the portal was the load's own fill, so replacing it is a note.
  set_loaded_capture_setting("");
  config::video.capture = "portal";
  logged = enter("host_virtual_display", "headless_stream");
  EXPECT_EQ(config::video.capture, "wlr");
  EXPECT_TRUE(lines_with(logged, "capture override:").empty()) << logged;
  const auto note = lines_with(logged, "session capture backend override");
  EXPECT_NE(note.find("Info: "), std::string::npos) << logged;
  EXPECT_NE(note.find("[portal] -> [wlr] for stream mode [headless_stream]"), std::string::npos) << note;

  // An explicit kms the live setting still holds is set aside by a Gamescope session, as before.
  set_loaded_capture_setting("kms");
  d.stream_mode = "desktop_display";
  config::video.capture = "kms";
  logged = enter("desktop_display", "gamescope_stream");
  EXPECT_EQ(config::video.capture, "portal");
  EXPECT_NE(lines_with(logged, "capture override:").find("asks for [portal], not the configured [kms]"), std::string::npos)
    << logged;

  // A transition that leaves capture as it is says nothing.
  logged = enter("desktop_display", "gamescope_stream");
  EXPECT_EQ(config::video.capture, "portal");
  EXPECT_TRUE(lines_with(logged, "capture override:").empty()) << logged;
  EXPECT_TRUE(lines_with(logged, "session capture backend override").empty()) << logged;
}

TEST(StreamDisplayPolicyTests, AGameModeHoldSaysWhichCaptureChoiceItSetsAside) {
  using stream_display_policy::game_mode_reconcile_e;
  ScopedPrivateRuntimePath runtime_path;
  LinuxDisplayPolicyGuard guard;
  LoadedCaptureGuard loaded_guard;
  set_loaded_capture_setting("wlr");
  std::string error;
  ASSERT_TRUE(stream_display_policy::apply_selection("headless_stream", error)) << error;
  config::video.capture = "wlr";

  std::string logged;
  {
    CaptureLogCapture log;
    ASSERT_EQ(stream_display_policy::reconcile_game_mode(true, false), game_mode_reconcile_e::entered);
    logged = log.text();
  }
  const auto line = lines_with(logged, "capture override:");
  EXPECT_NE(line.find("Warning: "), std::string::npos) << logged;
  EXPECT_NE(line.find("[desktop_display]"), std::string::npos) << line;
  EXPECT_NE(line.find("[wlr]"), std::string::npos) << line;
  EXPECT_NE(line.find("Game Mode"), std::string::npos) << line;
  EXPECT_EQ(stream_display_policy::reconcile_game_mode(false, false), game_mode_reconcile_e::left);
  EXPECT_EQ(config::video.capture, "wlr");
}

TEST(StreamDisplayPolicyTests, AGameModeHoldNamesPolarisConfNotTheReplacementALoadMade) {
  // A Host Virtual Display host on a wlroots backend holds wlr where polaris.conf says kms or
  // nothing, because its load put wlr in place. Game Mode measured its rewrite against that held
  // value, so it told a host set to kms that it had configured wlr, and warned a host with capture
  // unset about a wlr it never chose.
  using stream_display_policy::game_mode_reconcile_e;
  LinuxDisplayPolicyGuard guard;
  LoadedCaptureGuard loaded_guard;
  auto &d = config::video.linux_display;
  const auto enter_game_mode = [&d](const std::string &loaded) {
    set_loaded_capture_setting(loaded);
    d.stream_mode = "host_virtual_display";
    d.use_cage_compositor = false;
    config::video.capture = "wlr";  // what the load put in place for a wlroots virtual display
    std::string logged;
    {
      CaptureLogCapture log;
      EXPECT_EQ(stream_display_policy::reconcile_game_mode(true, false), game_mode_reconcile_e::entered);
      logged = log.text();
    }
    EXPECT_EQ(stream_display_policy::reconcile_game_mode(false, false), game_mode_reconcile_e::left);
    EXPECT_EQ(config::video.capture, "wlr") << "Game Mode gives back the setting it held";
    return logged;
  };

  auto logged = enter_game_mode("kms");
  const auto warning = lines_with(logged, "capture override:");
  EXPECT_NE(warning.find("Warning: "), std::string::npos) << logged;
  EXPECT_NE(warning.find("asks for [auto], not the configured [kms]"), std::string::npos) << warning;
  EXPECT_NE(warning.find("[wlr] comes back when the Game Mode session ends"), std::string::npos) << warning;

  logged = enter_game_mode("");
  EXPECT_TRUE(lines_with(logged, "capture override:").empty()) << logged;
  const auto note = lines_with(logged, "game_mode: capture");
  EXPECT_NE(note.find("Info: "), std::string::npos) << logged;
  EXPECT_NE(note.find("[wlr] is [auto]"), std::string::npos) << note;
}

TEST(StreamDisplayPolicyTests, ALaunchIntoHostVirtualDisplayNamesPolarisConfNotTheLoadsReplacement) {
  // An EVDI load put portal in place of capture. The virtual display backend applies at once when it
  // is changed in Settings, so the next launch into the mode rewrites capture for the new backend,
  // and it measured that against the portal it found: a host set to kms read that it had configured
  // portal, and a host with capture unset was warned about a portal it never chose.
  LinuxDisplayPolicyGuard guard;
  LoadedCaptureGuard loaded_guard;
  auto &d = config::video.linux_display;
  const auto launch_on_wlroots = [&d](const std::string &loaded) {
    set_loaded_capture_setting(loaded);
    d.stream_mode = "host_virtual_display";
    d.use_cage_compositor = false;
    config::video.capture = "portal";  // the EVDI load's replacement
    CaptureLogCapture log;
    stream_display_policy::normalize_host_virtual_display_state_for_backend(
      virtual_display::backend_e::WAYLAND_WLR,
      stream_display_policy::capture_rewrite_scope_e::session,
      "host_virtual_display"
    );
    return log.text();
  };
  const std::string wlroots = std::string {"["} + virtual_display::backend_name(virtual_display::backend_e::WAYLAND_WLR) + "]";

  auto logged = launch_on_wlroots("kms");
  EXPECT_EQ(config::video.capture, "wlr");
  const auto warning = lines_with(logged, "capture override:");
  EXPECT_NE(warning.find("Warning: "), std::string::npos) << logged;
  EXPECT_NE(warning.find("asks for [wlr], not the configured [kms]"), std::string::npos) << warning;
  EXPECT_NE(warning.find("for this session only"), std::string::npos) << warning;
  EXPECT_EQ(warning.find("[portal]"), std::string::npos) << warning;

  logged = launch_on_wlroots("");
  EXPECT_EQ(config::video.capture, "wlr");
  EXPECT_TRUE(lines_with(logged, "capture override:").empty()) << logged;
  const auto note = lines_with(logged, wlroots);
  EXPECT_NE(note.find("Info: "), std::string::npos) << logged;
  EXPECT_NE(note.find("through [wlr], with capture set to [auto]"), std::string::npos) << note;
}

TEST(StreamDisplayPolicyTests, CaptureAliasesReadTheWayDispatchReadsThem) {
  using stream_display_policy::canonical_capture_backend;

  EXPECT_EQ(canonical_capture_backend("kwin"), "portal");
  EXPECT_EQ(canonical_capture_backend("drm"), "kms");
  EXPECT_EQ(canonical_capture_backend("auto"), "");
  for (const auto value : {"", "wlr", "portal", "kms", "x11", "nvfbc", "bogus"}) {
    EXPECT_EQ(canonical_capture_backend(value), value);
  }

  // Dispatch is the authority, and it is not changed: every value has to dispatch exactly like
  // the backend this reads it as, on every combination of available sources.
  for (const auto &value : k_capture_values) {
    const auto canonical = canonical_capture_backend(value);
    for (int mask = 0; mask < 64; ++mask) {
      const bool flags[6] = {bool(mask & 1), bool(mask & 2), bool(mask & 4), bool(mask & 8), bool(mask & 16), bool(mask & 32)};
      EXPECT_EQ(
        platf::capture_backend_dispatch_for_tests(value, flags[0], flags[1], flags[2], flags[3], flags[4], flags[5]),
        platf::capture_backend_dispatch_for_tests(canonical, flags[0], flags[1], flags[2], flags[3], flags[4], flags[5])
      ) << "[" << value << "] read as [" << canonical << "], sources mask " << mask;
    }
  }
}

TEST(StreamDisplayPolicyTests, TheRuntimeBackendNamesTheBackendAnAliasOpens) {
  // runtime_backend, and session_capture.backend for a host mode, say what the mode captures
  // through. A host set to kwin read kwin, a name dispatch never opens: it opens the portal. A
  // literal auto read auto, which is no backend at all.
  const auto *mirror = stream_path::find(stream_path::k_desktop_display);
  ASSERT_NE(mirror, nullptr);
  stream_path::host_capabilities_t caps {};
  const auto backend_for = [&caps, mirror](std::string capture) {
    caps.configured_capture = std::move(capture);
    return stream_path::backend_name_for_path(*mirror, caps);
  };
  EXPECT_EQ(backend_for("portal"), "portal");
  EXPECT_EQ(backend_for("kwin"), "portal");
  EXPECT_EQ(backend_for("KWin"), "portal");
  EXPECT_EQ(backend_for("kms"), "kms");
  EXPECT_EQ(backend_for("drm"), "kms");
  EXPECT_EQ(backend_for("auto"), backend_for("")) << "auto asks the host to choose";
  EXPECT_EQ(backend_for(""), "host");
}

TEST(StreamDisplayPolicyTests, AHostSetToDrmAskedForKmsWhenTheCapabilityIsRefused) {
  struct Restore {
    std::string capture {config::video.capture};

    ~Restore() {
      config::video.capture = capture;
      platf::set_kms_capture_refused_for_tests(false);
      verified_action::clear();
    }
  } restore;
  const auto kms_capability_failures = []() {
    const auto records = verified_action::silent_failures();
    return std::count_if(records.begin(), records.end(), [](const verified_action::record_t &record) {
      return record.id == "video.kms_capability";
    });
  };

  for (const auto value : {"kms", "drm"}) {
    config::video.capture = value;
    verified_action::clear();
    platf::note_kms_capture_refused_for_capability();
    EXPECT_EQ(kms_capability_failures(), 1) << "[" << value << "] asked for KMS, and KMS did not land";
  }
  // Every other host never asked for KMS, and the probe says so itself, once, at info.
  for (const auto value : {"", "portal", "kwin", "wlr"}) {
    config::video.capture = value;
    verified_action::clear();
    platf::note_kms_capture_refused_for_capability();
    EXPECT_EQ(kms_capability_failures(), 0) << "[" << value << "] never asked for KMS";
  }
}

TEST(StreamDisplayPolicyTests, TheLoadedCaptureSettingSurvivesAModeThatRewritesIt) {
  LinuxDisplayPolicyGuard guard;
  auto &d = config::video.linux_display;

  d.stream_mode = "headless_stream";
  config::video.capture = "kms";
  stream_display_policy::normalize_config_from_load();
  EXPECT_EQ(stream_display_policy::loaded_capture_setting(), "kms");

  // The dongle fills an unset capture with portal in memory; what was loaded is still unset.
  d.stream_mode = "headless_dongle";
  config::video.capture.clear();
  stream_display_policy::normalize_config_from_load();
  EXPECT_EQ(config::video.capture, "portal");
  EXPECT_EQ(stream_display_policy::loaded_capture_setting(), "");

  // An explicit kms that a Host Virtual Display load rewrites is still kms in the snapshot. The
  // rewrite depends on the backend this machine detects, and follows it.
  d.stream_mode = "host_virtual_display";
  config::video.capture = "kms";
  stream_display_policy::normalize_config_from_load();
  EXPECT_EQ(stream_display_policy::loaded_capture_setting(), "kms");
  EXPECT_EQ(
    config::video.capture,
    stream_display_policy::capture_for_host_virtual_display_backend(virtual_display::detect_backend(), "kms")
  );
  {
    CaptureLogCapture quiet;
    stream_display_policy::log_config_load_notes();
  }
}

TEST(StreamDisplayPolicyTests, ARewriteAtLoadIsSaidOnceLoggingIsUp) {
  LinuxDisplayPolicyGuard guard;
  auto &d = config::video.linux_display;
  const auto say_load_notes = []() {
    CaptureLogCapture log;
    stream_display_policy::log_config_load_notes();
    return log.text();
  };
  say_load_notes();  // whatever an earlier load in this process kept

  // polaris.conf is parsed inside config::parse, before logging::init. A line logged there reaches
  // stdout and never polaris.log, so the load keeps it and main() says it once logging is up.
  d.stream_mode = "host_virtual_display";
  config::video.capture = "kms";
  std::string during;
  {
    CaptureLogCapture log;
    stream_display_policy::normalize_host_virtual_display_state_for_backend(
      virtual_display::backend_e::EVDI,
      stream_display_policy::capture_rewrite_scope_e::load
    );
    during = log.text();
  }
  EXPECT_EQ(config::video.capture, "portal");
  EXPECT_TRUE(lines_with(during, "[kms]").empty()) << during;
  const auto after = say_load_notes();
  const auto warning = lines_with(after, "[kms]");
  EXPECT_NE(warning.find("Warning: "), std::string::npos) << after;
  EXPECT_NE(warning.find("[host_virtual_display]"), std::string::npos) << warning;
  EXPECT_NE(warning.find("[portal]"), std::string::npos) << warning;
  // Nothing puts a loaded setting back at a teardown. A later mode switch restores the value it
  // found, which is already the replacement, so it lasts until Polaris restarts.
  EXPECT_NE(warning.find("until Polaris restarts"), std::string::npos) << warning;
  EXPECT_EQ(warning.find("teardown"), std::string::npos) << warning;
  EXPECT_TRUE(lines_with(say_load_notes(), "[kms]").empty()) << "each line is said once";

  // The dongle fill of an unset capture waits the same way, at info.
  d.stream_mode = "headless_dongle";
  config::video.capture.clear();
  {
    CaptureLogCapture log;
    stream_display_policy::normalize_config_from_load();
    during = log.text();
  }
  EXPECT_EQ(config::video.capture, "portal");
  EXPECT_TRUE(lines_with(during, "[headless_dongle]").empty()) << during;
  const auto dongle = lines_with(say_load_notes(), "[headless_dongle]");
  EXPECT_NE(dongle.find("Info: "), std::string::npos) << dongle;
  EXPECT_NE(dongle.find("[portal]"), std::string::npos) << dongle;

  // A load starts over: what an older load kept and nobody said belongs to a configuration that is gone.
  d.stream_mode = "headless_dongle";
  config::video.capture.clear();
  stream_display_policy::normalize_config_from_load();
  d.stream_mode = "headless_stream";
  config::video.capture = "kms";
  stream_display_policy::normalize_config_from_load();
  EXPECT_TRUE(lines_with(say_load_notes(), "[headless_dongle]").empty());
}

TEST(StreamDisplayPolicyTests, ALaunchThatFillsAnUnsetCaptureWithPortalSaysSo) {
  // A dongle load that fills an unset capture with portal says so. A launch entering Gamescope
  // Stream or the dongle made the same fill and said nothing, so the per-open line was the first
  // trace of a portal nobody configured.
  ScopedPrivateRuntimePath runtime_path {"gamescope"};
  LinuxDisplayPolicyGuard guard;
  LoadedCaptureGuard loaded_guard;
  auto &d = config::video.linux_display;
  const auto apply = [](std::string_view mode, stream_display_policy::capture_rewrite_scope_e scope) {
    CaptureLogCapture log;
    std::string error;
    EXPECT_TRUE(stream_display_policy::apply_selection(mode, error, scope)) << mode << ": " << error;
    return log.text();
  };
  const auto said = [](const std::string &line) {
    const auto at = line.find("Info: ");
    return at == std::string::npos ? std::string {} : line.substr(at);
  };
  const auto session = stream_display_policy::capture_rewrite_scope_e::session;

  d.stream_mode = "desktop_display";
  config::video.capture.clear();
  auto logged = apply("gamescope_stream", session);
  EXPECT_EQ(config::video.capture, "portal");
  const auto gamescope = lines_with(logged, "[gamescope_stream]");
  EXPECT_NE(gamescope.find("Info: "), std::string::npos) << logged;
  EXPECT_NE(gamescope.find("captures the Gamescope session through [portal], with capture set to [auto]"), std::string::npos)
    << gamescope;

  d.stream_mode = "desktop_display";
  d.streaming_output = "DP-1";
  d.primary_output = "eDP-1";
  config::video.capture.clear();
  config::video.output_name.clear();
  logged = apply("headless_dongle", session);
  EXPECT_EQ(config::video.capture, "portal");
  const auto dongle = lines_with(logged, "[headless_dongle]");
  EXPECT_NE(dongle.find("Info: "), std::string::npos) << logged;

  // It is the line the load says for the same fill.
  d.stream_mode = "headless_dongle";
  config::video.capture.clear();
  stream_display_policy::normalize_config_from_load();
  std::string loaded;
  {
    CaptureLogCapture log;
    stream_display_policy::log_config_load_notes();
    loaded = lines_with(log.text(), "[headless_dongle]");
  }
  ASSERT_FALSE(said(loaded).empty()) << loaded;
  EXPECT_EQ(said(dongle), said(loaded));

  // An explicit backend is not filled, and the session transition speaks for what happens to it.
  d.stream_mode = "desktop_display";
  config::video.capture = "kms";
  logged = apply("gamescope_stream", session);
  EXPECT_EQ(config::video.capture, "kms");
  EXPECT_TRUE(lines_with(logged, "[gamescope_stream]").empty()) << logged;

  // A client's mode switch puts capture back itself, so its preview says nothing.
  d.stream_mode = "desktop_display";
  config::video.capture.clear();
  logged = apply("gamescope_stream", stream_display_policy::capture_rewrite_scope_e::preview);
  EXPECT_EQ(config::video.capture, "portal");
  EXPECT_TRUE(lines_with(logged, "[gamescope_stream]").empty()) << logged;
}

TEST(StreamDisplayPolicyTests, AModeFillsAnUnsetCaptureBeforeALaunchAsksForIt) {
  using stream_display_policy::capture_filled_for_mode;
  // Gamescope Stream fills an unset capture, and the dongle an unset or auto one. An explicit
  // backend is kept in both, and every other mode keeps what it is given.
  EXPECT_EQ(capture_filled_for_mode("gamescope_stream", ""), "portal");
  EXPECT_EQ(capture_filled_for_mode("headless_dongle", ""), "portal");
  EXPECT_EQ(capture_filled_for_mode("headless_dongle", "auto"), "portal");
  for (const auto mode : {"gamescope_stream", "headless_dongle"}) {
    for (const auto capture : {"kms", "drm", "wlr", "kwin"}) {
      EXPECT_EQ(capture_filled_for_mode(mode, capture), capture) << mode << " capture=[" << capture << "]";
    }
  }
  for (const auto mode : {"desktop_display", "headless_stream", "windowed_stream", "host_virtual_display", "desktop_takeover"}) {
    EXPECT_EQ(capture_filled_for_mode(mode, ""), "") << mode;
  }

  // What a launch into the live mode asks for reads the fill first and the mode's rule after it.
  LinuxDisplayPolicyGuard guard;
  struct SubstitutionGuard {
    ~SubstitutionGuard() {
      platf::set_capture_backend_substitution_for_tests("");
    }
  } substitution_guard;
  auto &d = config::video.linux_display;
  d.use_cage_compositor = false;
  platf::set_capture_backend_substitution_for_tests("");
  d.stream_mode = "gamescope_stream";
  config::video.capture = "";
  EXPECT_EQ(stream_display_policy::capture_for_current_mode(), "");
  EXPECT_EQ(stream_display_policy::capture_for_launch_into_current_mode(), "portal");
  d.stream_mode = "desktop_display";
  EXPECT_EQ(stream_display_policy::capture_for_launch_into_current_mode(), "");
  // An explicit backend the evaluation replaced asks for auto in the dongle, filled or not.
  platf::set_capture_backend_substitution_for_tests("kms -> portal");
  d.stream_mode = "headless_dongle";
  config::video.capture = "kms";
  EXPECT_EQ(stream_display_policy::capture_for_launch_into_current_mode(), "");
}

TEST(StreamDisplayPolicyTests, AModeSwitchThatPutsCaptureBackSaysNothingAboutIt) {
  // A client switching the host mode applies the selection, then puts the capture setting back
  // itself. A warning that the host set kms aside described a state that did not exist, until the
  // launch that enters the mode applies it again and says so then.
  LinuxDisplayPolicyGuard guard;
  config::video.linux_display.stream_mode = "desktop_display";
  config::video.capture = "kms";
  std::string logged;
  {
    CaptureLogCapture log;
    stream_display_policy::normalize_host_virtual_display_state_for_backend(
      virtual_display::backend_e::EVDI,
      stream_display_policy::capture_rewrite_scope_e::preview,
      "host_virtual_display"
    );
    logged = log.text();
  }
  EXPECT_EQ(config::video.capture, "portal") << "the rewrite itself still happens, and the caller undoes it";
  EXPECT_TRUE(lines_with(logged, "[kms]").empty()) << logged;
  EXPECT_TRUE(lines_with(logged, "[host_virtual_display]").empty()) << logged;
}

TEST(StreamDisplayPolicyTests, AVirtualDisplayOnAnotherBackendIsMeasuredAgainstPolarisConf) {
  // The launch checked one backend and rewrote capture for it, then the display came up on another.
  // What the launch put in place is not the operator's choice, and measured against it, a host with
  // capture unset was warned that its explicit portal had been set aside.
  LinuxDisplayPolicyGuard guard;
  auto &d = config::video.linux_display;
  const auto load = [&d](std::string capture) {
    d.stream_mode = "headless_stream";
    config::video.capture = std::move(capture);
    stream_display_policy::normalize_config_from_load();
  };
  const auto comes_up_on_wlroots = [&d]() {
    d.stream_mode = "host_virtual_display";
    d.use_cage_compositor = false;
    config::video.capture = "portal";  // what the launch set for the EVDI display it checked
    CaptureLogCapture log;
    stream_display_policy::normalize_host_virtual_display_state_for_backend(
      virtual_display::backend_e::WAYLAND_WLR,
      stream_display_policy::capture_rewrite_scope_e::backend_change
    );
    return log.text();
  };
  const std::string wlroots = std::string {"["} + virtual_display::backend_name(virtual_display::backend_e::WAYLAND_WLR) + "]";

  load("");
  auto logged = comes_up_on_wlroots();
  EXPECT_EQ(config::video.capture, "wlr");
  EXPECT_TRUE(lines_with(logged, "capture override:").empty()) << logged;
  const auto moved = lines_with(logged, wlroots);
  EXPECT_NE(moved.find("Info: "), std::string::npos) << logged;
  EXPECT_NE(moved.find("[host_virtual_display]"), std::string::npos) << moved;
  EXPECT_NE(moved.find("through [wlr] instead of [portal]"), std::string::npos) << moved;

  // An explicit kms is named as kms, not as the portal the launch put in its place.
  load("kms");
  logged = comes_up_on_wlroots();
  const auto warning = lines_with(logged, "capture override:");
  EXPECT_NE(warning.find("Warning: "), std::string::npos) << logged;
  EXPECT_NE(warning.find("asks for [wlr], not the configured [kms]"), std::string::npos) << warning;
  EXPECT_NE(warning.find(wlroots), std::string::npos) << warning;
  EXPECT_NE(warning.find("for this session only"), std::string::npos) << warning;

  // An operator who chose wlr gets it back. That is a backend change, not a choice set aside.
  load("wlr");
  logged = comes_up_on_wlroots();
  EXPECT_EQ(config::video.capture, "wlr");
  EXPECT_TRUE(lines_with(logged, "capture override:").empty()) << logged;
  EXPECT_NE(lines_with(logged, wlroots).find("Info: "), std::string::npos) << logged;
  {
    CaptureLogCapture quiet;
    stream_display_policy::log_config_load_notes();
  }
}

namespace {
  struct SubstitutionNoteGuard {
    ~SubstitutionNoteGuard() {
      platf::set_capture_backend_substitution_for_tests("");
    }
  };
}  // namespace

TEST(StreamDisplayPolicyTests, ARequestNamesTheRuleThatSetPolarisConfAside) {
  using stream_display_policy::capture_request_override_reason;
  LinuxDisplayPolicyGuard guard;
  LoadedCaptureGuard loaded_guard;
  HeldHostDefaultGuard held_guard;
  SubstitutionNoteGuard substitution_guard;
  platf::set_capture_backend_substitution_for_tests("");
  set_loaded_capture_setting("");

  // Asking the host to choose is never set aside, and an alias is the backend it names.
  config::video.capture.clear();
  EXPECT_EQ(capture_request_override_reason("", "portal", "gamescope_stream", false, false), "");
  config::video.capture = "drm";
  EXPECT_EQ(capture_request_override_reason("drm", "kms", "desktop_display", false, false), "");
  config::video.capture = "kwin";
  EXPECT_EQ(capture_request_override_reason("kwin", "portal", "desktop_display", false, false), "");

  // A private compositor asks for wlr whatever polaris.conf names.
  config::video.capture = "kms";
  EXPECT_EQ(capture_request_override_reason("kms", "wlr", "headless_stream", true, false), "private_compositor");

  // A substitution sends a request to auto. A generation that owns an exact output is never
  // substituted, so a difference there is not put down to one.
  platf::set_capture_backend_substitution_for_tests("kms -> portal");
  EXPECT_EQ(capture_request_override_reason("kms", "", "desktop_display", false, false), "substituted");
  EXPECT_EQ(capture_request_override_reason("kms", "", "desktop_display", false, true), "");
  platf::set_capture_backend_substitution_for_tests("");

  // A launch into a Gamescope session rewrote the live setting. Its rule answers until teardown puts
  // the host setting back.
  set_loaded_capture_setting("kms");
  config::video.linux_display.stream_mode = "desktop_display";
  config::video.capture = "kms";
  {
    CaptureLogCapture quiet;
    stream_display_policy::apply_capture_for_session_transition("desktop_display", "gamescope_stream");
  }
  ASSERT_EQ(config::video.capture, "portal");
  EXPECT_EQ(capture_request_override_reason("kms", "portal", "gamescope_stream", false, false), "gamescope_session");
  config::video.capture = "kms";
  stream_display_policy::forget_host_default();
  config::video.capture = "portal";
  EXPECT_EQ(capture_request_override_reason("kms", "portal", "gamescope_stream", false, false), "")
    << "the rewrite ended at teardown, and no reason is guessed for a portal nothing explains";

  // A Host Virtual Display load lasts past teardown, until the next load.
  config::video.linux_display.stream_mode = "host_virtual_display";
  config::video.capture = "kms";
  {
    CaptureLogCapture quiet;
    stream_display_policy::normalize_host_virtual_display_state_for_backend(
      virtual_display::backend_e::EVDI,
      stream_display_policy::capture_rewrite_scope_e::load
    );
    stream_display_policy::log_config_load_notes();
  }
  ASSERT_EQ(config::video.capture, "portal");
  stream_display_policy::forget_host_default();
  EXPECT_EQ(capture_request_override_reason("kms", "portal", "host_virtual_display", false, false), "virtual_display_backend");
  set_loaded_capture_setting("kms");
  config::video.capture = "portal";
  EXPECT_EQ(capture_request_override_reason("kms", "portal", "host_virtual_display", false, false), "")
    << "a reload drops what an older load rewrote";
}

TEST(StreamDisplayPolicyTests, ASessionNamesItsOwnRuleWhenAnOlderRewriteAlreadyWroteTheSameBackend) {
  using stream_display_policy::capture_request_override_reason;
  LinuxDisplayPolicyGuard guard;
  LoadedCaptureGuard loaded_guard;
  HeldHostDefaultGuard held_guard;
  SubstitutionNoteGuard substitution_guard;
  platf::set_capture_backend_substitution_for_tests("");
  set_loaded_capture_setting("kms");

  // polaris.conf holds capture = kms and linux_stream_mode = host_virtual_display on EVDI, and the
  // load rewrites the live setting to the portal.
  config::video.linux_display.stream_mode = "host_virtual_display";
  config::video.capture = "kms";
  {
    CaptureLogCapture quiet;
    stream_display_policy::normalize_host_virtual_display_state_for_backend(
      virtual_display::backend_e::EVDI,
      stream_display_policy::capture_rewrite_scope_e::load
    );
    stream_display_policy::log_config_load_notes();
  }
  ASSERT_EQ(config::video.capture, "portal");
  EXPECT_EQ(capture_request_override_reason("kms", "portal", "host_virtual_display", false, false), "virtual_display_backend");

  // A launch into a Gamescope session asks for the portal as well, so nothing is rewritten, and
  // the session is in no Host Virtual Display: the Gamescope rule is why it asked for the portal.
  {
    CaptureLogCapture quiet;
    stream_display_policy::apply_capture_for_session_transition("host_virtual_display", "gamescope_stream");
  }
  ASSERT_EQ(config::video.capture, "portal");
  EXPECT_EQ(capture_request_override_reason("kms", "portal", "gamescope_stream", false, false), "gamescope_session");

  // Teardown ends the session's rule, and the load's answers again.
  stream_display_policy::forget_host_default();
  EXPECT_EQ(capture_request_override_reason("kms", "portal", "host_virtual_display", false, false), "virtual_display_backend");

  // A mode rule answers only for the backend it asked for. A private compositor asks for wlr, so a
  // request for the portal is not its doing, and the rule that wrote the portal answers.
  EXPECT_EQ(capture_request_override_reason("kms", "portal", "headless_stream", true, false), "virtual_display_backend");
  // A rewrite answers only for the backend it wrote. The load wrote the portal, so it does not
  // explain a request for wlr, and nothing is guessed for one.
  EXPECT_EQ(capture_request_override_reason("kms", "wlr", "desktop_display", false, false), "");
}

TEST(StreamDisplayPolicyTests, ARequestUnderSteamGameModeNamesDesktopDiscoveryUntilTheHoldIsGivenBack) {
  using stream_display_policy::capture_request_override_reason;
  using stream_display_policy::game_mode_reconcile_e;
  ScopedPrivateRuntimePath runtime_path;
  LinuxDisplayPolicyGuard guard;
  LoadedCaptureGuard loaded_guard;
  SubstitutionNoteGuard substitution_guard;
  platf::set_capture_backend_substitution_for_tests("");
  set_loaded_capture_setting("wlr");
  std::string error;
  ASSERT_TRUE(stream_display_policy::apply_selection("headless_stream", error)) << error;
  config::video.capture = "wlr";
  {
    CaptureLogCapture quiet;
    ASSERT_EQ(stream_display_policy::reconcile_game_mode(true, false), game_mode_reconcile_e::entered);
  }
  ASSERT_TRUE(config::video.capture.empty());
  EXPECT_EQ(capture_request_override_reason("wlr", "", "desktop_display", false, false), "desktop_discovery");
  ASSERT_EQ(stream_display_policy::reconcile_game_mode(false, false), game_mode_reconcile_e::left);
  config::video.capture.clear();
  EXPECT_EQ(capture_request_override_reason("wlr", "", "desktop_display", false, false), "");
}

namespace portal {
  platf::capture_route_t portal_capture_route_for_tests(std::string_view gamescope, std::string_view kwin);
}

TEST(StreamDisplayPolicyTests, AModeRewriteAndARouteFallbackReachTheSessionsPublishedCapture) {
  // polaris.conf says kms. A launch into a Gamescope session sets it aside for the portal, the
  // portal finds no gamescope node and takes a ScreenCast, and the session publishes both reasons
  // through the same path its encode loop takes.
  LinuxDisplayPolicyGuard guard;
  LoadedCaptureGuard loaded_guard;
  HeldHostDefaultGuard held_guard;
  SubstitutionNoteGuard substitution_guard;
  platf::set_capture_backend_substitution_for_tests("");
  stream_stats::update_stream_active(false);
  auto reset = util::fail_guard([] { stream_stats::update_stream_active(false); });
  set_loaded_capture_setting("kms");
  config::video.linux_display.stream_mode = "desktop_display";
  config::video.capture = "kms";
  {
    CaptureLogCapture quiet;
    stream_display_policy::apply_capture_for_session_transition("desktop_display", "gamescope_stream");
  }
  ASSERT_EQ(config::video.capture, "portal");

  video::config_t session {};
  session.session_generation = 398;
  session.capture_generation.stream_mode = "gamescope_stream";
  session.capture_generation.private_runtime = "gamescope";
  session.capture_generation.capture_backend = "portal";
  session.capture_request = video::capture_request_for_session_for_tests(session.capture_generation);
  const auto route = portal::portal_capture_route_for_tests("missing", "not_asked");
  stream_stats::add_client("10.0.0.5", "Client", 398);
  bool published = false;
  ASSERT_TRUE(video::publish_capture_backend_for_tests(session, route, published));

  const auto json = nlohmann::json::parse(stream_stats::get_current().to_json());
  const auto &capture = json["clients"][0]["capture"];
  EXPECT_EQ(capture["preference"], "kms");
  EXPECT_EQ(capture["requested"], "portal");
  EXPECT_EQ(capture["opened"], "portal");
  EXPECT_EQ(capture["route"], "portal_screencast");
  EXPECT_EQ(capture["mode_override_reason"], "gamescope_session");
  EXPECT_EQ(capture["route_fallback_reason"], "gamescope_node_missing");
  EXPECT_EQ(json["capture_mode_override_reason"], "gamescope_session");
  EXPECT_EQ(json["capture_route_fallback_reason"], "gamescope_node_missing");
}

TEST(StreamDisplayPolicyTests, ASessionTakesPolarisConfAndTheRuleItsOwnGenerationMet) {
  LinuxDisplayPolicyGuard guard;
  LoadedCaptureGuard loaded_guard;
  SubstitutionNoteGuard substitution_guard;
  platf::set_capture_backend_substitution_for_tests("");
  set_loaded_capture_setting("drm");
  // The live mode is Mirror Desktop; the generation was built for Private Stream and says so.
  config::video.linux_display.stream_mode = "desktop_display";
  config::video.linux_display.use_cage_compositor = false;
  config::video.capture = "drm";
  capture_generation::identity_t generation;
  generation.stream_mode = "headless_stream";
  generation.use_cage_compositor = true;
  generation.capture_backend = "wlr";
  auto request = video::capture_request_for_session_for_tests(generation);
  EXPECT_EQ(request.preference, "drm") << "polaris.conf as written, before any rewrite";
  EXPECT_EQ(request.mode_override_reason, "private_compositor");

  // The generation's own exact output is asked, not the default.
  platf::set_capture_backend_substitution_for_tests("kms -> portal");
  generation = {};
  generation.stream_mode = "desktop_display";
  EXPECT_EQ(video::capture_request_for_session_for_tests(generation).mode_override_reason, "substituted");
  generation.exact_display_name = "DP-2";
  generation.requested_output_name = "DP-2";
  EXPECT_EQ(video::capture_request_for_session_for_tests(generation).mode_override_reason, "");

  // A request for the backend polaris.conf names has nothing set aside.
  generation = {};
  generation.stream_mode = "desktop_display";
  generation.capture_backend = "kms";
  EXPECT_EQ(video::capture_request_for_session_for_tests(generation).mode_override_reason, "");
}
