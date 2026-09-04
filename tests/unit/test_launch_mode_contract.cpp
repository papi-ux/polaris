/**
 * @file tests/unit/test_launch_mode_contract.cpp
 * @brief Tests for the Polaris v1 game launch-mode recommendation contract.
 */

#include <src/crypto.h>
#include <src/nvhttp.h>
#include <src/video.h>

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <unordered_set>

#include <gtest/gtest.h>

TEST(SessionEncoderContract, AdvertisesOnlyCanonicalBuildSelectableBackends) {
  const auto backends = video::selectable_encoder_backends();
  ASSERT_FALSE(backends.empty());
  EXPECT_EQ(backends.front(), "auto");
  EXPECT_NE(std::find(backends.begin(), backends.end(), "software"), backends.end());

  const std::unordered_set<std::string> unique {backends.begin(), backends.end()};
  EXPECT_EQ(unique.size(), backends.size());
  for (const auto &backend : backends) {
    EXPECT_TRUE(video::encoder_backend_selectable(backend)) << backend;
    EXPECT_EQ(nvhttp::normalize_encoder_backend(backend), backend);
  }
  EXPECT_FALSE(video::encoder_backend_selectable("host_default"));
  EXPECT_EQ(nvhttp::normalize_encoder_backend("not-an-encoder"), std::nullopt);
}

TEST(SessionEncoderContract, CapabilityRowsDescribeRuntimeValidationAndFallback) {
  const auto options = nvhttp::encoder_backend_options_json();
  ASSERT_TRUE(options.is_array());
  ASSERT_FALSE(options.empty());
  EXPECT_EQ(options.front().at("value"), "auto");
  EXPECT_TRUE(options.front().at("fallback_allowed").get<bool>());
  EXPECT_EQ(options.front().at("runtime_validation"), "launch");

  for (const auto &option : options) {
    const auto backend = option.at("value").get<std::string>();
    EXPECT_TRUE(video::encoder_backend_selectable(backend));
    EXPECT_EQ(option.at("available"), true);
    EXPECT_EQ(option.at("fallback_allowed"), backend == "auto");
  }
}

TEST(SessionEncoderContract, FallbackPolicyDistinguishesSessionLocksFromHostDefaults) {
  EXPECT_TRUE(nvhttp::encoder_backend_fallback_allowed("auto", true));
  EXPECT_FALSE(nvhttp::encoder_backend_fallback_allowed("nvenc", true));
  EXPECT_FALSE(nvhttp::encoder_backend_fallback_allowed("vulkan", true));

  EXPECT_TRUE(nvhttp::encoder_backend_fallback_allowed("auto", false));
  EXPECT_TRUE(nvhttp::encoder_backend_fallback_allowed("nvenc", false));
  EXPECT_TRUE(nvhttp::encoder_backend_fallback_allowed("vaapi", false));
  EXPECT_FALSE(nvhttp::encoder_backend_fallback_allowed("vulkan", false));
}

#ifdef __linux__
  #include <filesystem>
  #include <fstream>
  #include <stdexcept>
  #include <sys/stat.h>
  #include <unistd.h>

namespace {
  class ScopedPrivateRuntimePath {
  public:
    ScopedPrivateRuntimePath() {
      if (const char *current = std::getenv("PATH")) {
        had_previous = true;
        previous = current;
      }

      char path_template[] = "/tmp/polaris-private-runtime-XXXXXX";
      const char *created = mkdtemp(path_template);
      if (!created) {
        throw std::runtime_error("failed to create private-runtime test directory");
      }
      directory = created;

      for (const char *binary : {"labwc", "wlr-randr"}) {
        const auto path = std::filesystem::path {directory} / binary;
        std::ofstream script {path};
        script << "#!/bin/sh\nexit 0\n";
        script.close();
        if (!script || chmod(path.c_str(), 0700) != 0) {
          throw std::runtime_error("failed to create private-runtime test executable");
        }
      }
      if (setenv("PATH", directory.c_str(), 1) != 0) {
        throw std::runtime_error("failed to set private-runtime test PATH");
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
}
#endif

TEST(LaunchModeContractTests, HostHeadlessConfigurationWinsOverPerGameVirtualDisplayPreference) {
#ifdef __linux__
  ScopedPrivateRuntimePath runtime_path;
#endif
  const auto contract = nvhttp::build_launch_mode_contract_for_tests(
    true,
    "Indiana Jones and the Great Circle",
    true,
    true
  );

  EXPECT_EQ(contract.at("preferred_mode"), "host_virtual_display");
  EXPECT_EQ(contract.at("recommended_mode"), "headless_stream");
  bool allowed_host_virtual_display = false;
  for (const auto &mode : contract.at("allowed_modes")) {
    allowed_host_virtual_display = allowed_host_virtual_display || mode == "host_virtual_display";
  }
  EXPECT_TRUE(allowed_host_virtual_display);
  EXPECT_NE(contract.at("mode_reason").get<std::string>().find("already configured for Private Stream"), std::string::npos);
}

TEST(LaunchModeContractTests, SteamBigPictureOnHeadlessHostExplainsPrivateDesktopSafety) {
#ifdef __linux__
  ScopedPrivateRuntimePath runtime_path;
#endif
  const auto contract = nvhttp::build_launch_mode_contract_for_tests(
    true,
    "Steam Big Picture",
    true,
    true
  );

  EXPECT_EQ(contract.at("preferred_mode"), "host_virtual_display");
  EXPECT_EQ(contract.at("recommended_mode"), "headless_stream");
  const auto reason = contract.at("mode_reason").get<std::string>();
  EXPECT_NE(reason.find("Private Stream session"), std::string::npos);
  EXPECT_NE(reason.find("physical desktop"), std::string::npos);
}

TEST(LaunchModeContractTests, PerGameVirtualDisplayPreferenceIsRecommendedWhenHostIsNotHeadless) {
  const auto contract = nvhttp::build_launch_mode_contract_for_tests(
    true,
    "Indiana Jones and the Great Circle",
    true,
    false
  );

  EXPECT_EQ(contract.at("preferred_mode"), "host_virtual_display");
  EXPECT_EQ(contract.at("recommended_mode"), "host_virtual_display");
}

#ifdef __linux__
  #include <src/platform/linux/stream_display_policy.h>

namespace {
  class ScopedPath {
  public:
    explicit ScopedPath(const char *replacement) {
      if (const char *current = std::getenv("PATH")) {
        had_previous = true;
        previous = current;
      }
      setenv("PATH", replacement, 1);
    }

    ~ScopedPath() {
      if (had_previous) {
        setenv("PATH", previous.c_str(), 1);
      } else {
        unsetenv("PATH");
      }
    }

  private:
    bool had_previous = false;
    std::string previous;
  };

  std::unique_ptr<crypto::named_cert_t> launch_client_cert() {
    auto cert = std::make_unique<crypto::named_cert_t>();
    cert->name = "topology-test-client";
    cert->uuid = "topology-test-client-uuid";
    cert->perm = crypto::PERM::_game_control;
    cert->enable_legacy_ordering = false;
    cert->allow_client_commands = false;
    cert->always_use_virtual_display = false;
    return cert;
  }

  nvhttp::args_t resolved_launch_args(
      std::string stream_mode = {},
      std::string expected_topology = "desktop_display") {
    nvhttp::args_t args;
    args.emplace("rikey", std::string(crypto::cipher::key_size * 2, '0'));
    args.emplace("rikeyid", "1");
    args.emplace("mode", "1920x1080x60");
    args.emplace("resolvedProfile", "1");
    args.emplace("bitrateKbps", "20000");
    args.emplace("resolvedHdr", "0");
    args.emplace("expectedTopology", std::move(expected_topology));
    if (!stream_mode.empty()) {
      args.emplace("streamMode", std::move(stream_mode));
    }
    return args;
  }
}

TEST(SessionEncoderContract, ExactLaunchCarriesAutoAsAnExplicitSessionChoice) {
  auto cert = launch_client_cert();
  auto args = resolved_launch_args();
  args.emplace("encoderBackend", "AUTO");
  args.emplace("expectedEncoder", "auto");

  const auto session = nvhttp::make_launch_session(true, false, args, cert.get());
  ASSERT_NE(session, nullptr);
  EXPECT_TRUE(session->encoder_backend_explicit);
  EXPECT_EQ(session->encoder_backend, "auto");
  EXPECT_EQ(session->expected_encoder_backend, "auto");
}

TEST(SessionEncoderContract, ExactLaunchRequiresMatchingEncoderAssertion) {
  auto cert = launch_client_cert();

  auto missing = resolved_launch_args();
  missing.emplace("encoderBackend", "software");
  EXPECT_EQ(nvhttp::make_launch_session(true, false, missing, cert.get()), nullptr);

  auto mismatch = resolved_launch_args();
  mismatch.emplace("encoderBackend", "software");
  mismatch.emplace("expectedEncoder", "auto");
  EXPECT_EQ(nvhttp::make_launch_session(true, false, mismatch, cert.get()), nullptr);

  auto unknown = resolved_launch_args();
  unknown.emplace("encoderBackend", "not-an-encoder");
  unknown.emplace("expectedEncoder", "not-an-encoder");
  EXPECT_EQ(nvhttp::make_launch_session(true, false, unknown, cert.get()), nullptr);
}

TEST(SessionEncoderContract, LegacyResolvedLaunchWithoutEncoderEnvelopeStillParses) {
  auto cert = launch_client_cert();
  const auto session = nvhttp::make_launch_session(
    true,
    false,
    resolved_launch_args(),
    cert.get()
  );

  ASSERT_NE(session, nullptr);
  EXPECT_FALSE(session->encoder_backend_explicit);
  EXPECT_TRUE(session->encoder_backend.empty());
  EXPECT_TRUE(session->expected_encoder_backend.empty());
}

TEST(LaunchModeContractTests, RecommendationAlwaysBelongsToAllowedModesWithoutPrivateRuntime) {
  ScopedPath path {"/polaris-test/no-runtime-binaries"};
  const auto contract = nvhttp::build_launch_mode_contract_for_tests(
    false,
    "Control",
    false,
    false
  );

  EXPECT_EQ(contract.at("recommended_mode"), "desktop_display");
  bool recommendation_allowed = false;
  for (const auto &mode : contract.at("allowed_modes")) {
    recommendation_allowed = recommendation_allowed ||
      mode == contract.at("recommended_mode");
  }
  EXPECT_TRUE(recommendation_allowed);
  EXPECT_NE(
    contract.at("mode_reason").get<std::string>().find("not launch-ready"),
    std::string::npos
  );
}

// The client-settings POST validator used to hardcode four ids while
// allowed_modes advertised the full registry, so gamescope_stream and
// headless_dongle were advertised and then rejected with 400. The validator now
// delegates to stream_display_policy::selection_valid - the same check
// apply_selection itself runs - and this pins that they can never disagree.
TEST(LaunchModeContractTests, EveryAdvertisedModeVerdictMatchesTheValidator) {
  for (const bool virtual_display_available : {false, true}) {
    for (const auto &option : stream_display_policy::mode_options(virtual_display_available)) {
      std::string error;
      const bool valid = stream_display_policy::selection_valid_for_capabilities(
        option.value,
        virtual_display_available,
        error
      );
      if (option.available) {
        EXPECT_TRUE(valid) << option.value << ": advertised available but rejected: " << error;
      } else {
        EXPECT_FALSE(valid) << option.value << ": advertised unavailable but accepted";
        EXPECT_FALSE(error.empty()) << option.value << ": rejection must carry the host's reason";
      }
    }
  }
}

TEST(LaunchModeContractTests, MissingVirtualBackendFailsTheLaunchValidator) {
  std::string unavailable_error;
  EXPECT_FALSE(stream_display_policy::selection_valid_for_capabilities(
    "host_virtual_display",
    false,
    unavailable_error
  ));
  EXPECT_FALSE(unavailable_error.empty());

  std::string available_error;
  EXPECT_TRUE(stream_display_policy::selection_valid_for_capabilities(
    "host_virtual_display",
    true,
    available_error
  )) << available_error;
}

TEST(LaunchModeContractTests, ReservedAndUnknownIdsAreRejectedWithGuidance) {
  for (const auto *id : {"family_isolated", "headless_evdi", "not_a_mode"}) {
    std::string error;
    EXPECT_FALSE(stream_display_policy::selection_valid(id, error)) << id;
    EXPECT_NE(error.find("known stream path id"), std::string::npos) << id;
  }
}

TEST(SessionStreamMode, AcceptsRegistryModesItCanRunPerSession) {
  ScopedPrivateRuntimePath runtime_path;
  // Private modes are accepted only when both binaries used by their launch
  // path are executable. The fixture makes that capability deterministic.
  EXPECT_EQ(nvhttp::accepted_session_stream_mode_for_tests("headless_stream"), "headless_stream");
  EXPECT_EQ(nvhttp::accepted_session_stream_mode_for_tests("windowed_stream"), "windowed_stream");
  EXPECT_EQ(nvhttp::accepted_session_stream_mode_for_tests("desktop_display"), "desktop_display");
}

TEST(SessionStreamMode, DerivesSessionOverridabilityFromTopologyNotAnIdList) {
  // The rule a client is told and the rule the gate enforces have to be the
  // same rule, or the client offers a mode the host silently drops. It is
  // derived from the path's topology so a future swapping path inherits it
  // without editing a list: swapping the host's primary output rearranges the
  // machine itself, which is a host decision rather than a per-launch one.
  EXPECT_FALSE(stream_display_policy::selection_session_overridable("headless_dongle"));

  EXPECT_TRUE(stream_display_policy::selection_session_overridable("headless_stream"));
  EXPECT_TRUE(stream_display_policy::selection_session_overridable("windowed_stream"));
  EXPECT_TRUE(stream_display_policy::selection_session_overridable("desktop_display"));
  EXPECT_TRUE(stream_display_policy::selection_session_overridable("desktop_takeover"));

  // Unknown ids are not "restricted", they are simply not paths. The gate
  // checks validity first so they report as unknown rather than as reserved.
  EXPECT_FALSE(stream_display_policy::selection_session_overridable("garbage_mode"));
}

TEST(SessionStreamMode, RejectsDongleReservedUnknownAndEmpty) {
  // headless_dongle swaps host output topology: host-default-only by design.
  EXPECT_EQ(nvhttp::accepted_session_stream_mode_for_tests("headless_dongle"), "");
  // Reserved/unknown ids and absence all resolve to "host default applies".
  EXPECT_EQ(nvhttp::accepted_session_stream_mode_for_tests("family_isolated"), "");
  EXPECT_EQ(nvhttp::accepted_session_stream_mode_for_tests("garbage_mode"), "");
  EXPECT_EQ(nvhttp::accepted_session_stream_mode_for_tests(""), "");
}

TEST(SessionStreamMode, RejectsGamescopeWhenItIsNoLongerOnPath) {
  const char *prior_path = std::getenv("PATH");
  const std::string saved_path = prior_path ? prior_path : "";
  const bool path_was_set = prior_path != nullptr;

  EXPECT_EQ(setenv("PATH", "/polaris-test-no-gamescope", 1), 0);
  EXPECT_EQ(
    nvhttp::accepted_session_stream_mode_for_tests("gamescope_stream"),
    ""
  );

  if (path_was_set) {
    EXPECT_EQ(setenv("PATH", saved_path.c_str(), 1), 0);
  } else {
    EXPECT_EQ(unsetenv("PATH"), 0);
  }
}

TEST(SessionStreamMode, ExactResolvedLaunchRejectsUnavailableOrHostOnlyModes) {
  const char *prior_path = std::getenv("PATH");
  const std::string saved_path = prior_path ? prior_path : "";
  const bool path_was_set = prior_path != nullptr;
  EXPECT_EQ(setenv("PATH", "/polaris-test-no-gamescope", 1), 0);

  auto cert = launch_client_cert();
  EXPECT_EQ(
    nvhttp::make_launch_session(
      true,
      false,
      resolved_launch_args("gamescope_stream", "gamescope_stream"),
      cert.get()
    ),
    nullptr
  );
  EXPECT_EQ(
    nvhttp::make_launch_session(
      true,
      false,
      resolved_launch_args("headless_dongle", "headless_dongle"),
      cert.get()
    ),
    nullptr
  );

  if (path_was_set) {
    EXPECT_EQ(setenv("PATH", saved_path.c_str(), 1), 0);
  } else {
    EXPECT_EQ(unsetenv("PATH"), 0);
  }
}

TEST(SessionStreamMode, ExactParserDefersAvailabilityForALosingAppTopologyRequest) {
  const char *prior_path = std::getenv("PATH");
  const std::string saved_path = prior_path ? prior_path : "";
  const bool path_was_set = prior_path != nullptr;
  EXPECT_EQ(setenv("PATH", "/polaris-test-no-gamescope", 1), 0);

  auto cert = launch_client_cert();
  const auto session = nvhttp::make_launch_session(
    true,
    false,
    resolved_launch_args("gamescope_stream", "desktop_display"),
    cert.get()
  );
  EXPECT_NE(session, nullptr)
    << "parser availability must not outrank the app-aware final topology resolver";
  if (session) {
    EXPECT_EQ(session->stream_mode, "gamescope_stream");
    EXPECT_EQ(session->expected_stream_mode, "desktop_display");
  }

  if (path_was_set) {
    EXPECT_EQ(setenv("PATH", saved_path.c_str(), 1), 0);
  } else {
    EXPECT_EQ(unsetenv("PATH"), 0);
  }
}

TEST(SessionStreamMode, LegacyMirrorAndHostDefaultKeepDocumentedSemantics) {
  const char *prior_path = std::getenv("PATH");
  const std::string saved_path = prior_path ? prior_path : "";
  const bool path_was_set = prior_path != nullptr;
  EXPECT_EQ(setenv("PATH", "/polaris-test-no-gamescope", 1), 0);

  auto cert = launch_client_cert();
  auto legacy_args = resolved_launch_args("gamescope_stream");
  legacy_args.erase("resolvedProfile");
  legacy_args.erase("bitrateKbps");
  legacy_args.erase("resolvedHdr");
  legacy_args.erase("expectedTopology");
  const auto legacy_session = nvhttp::make_launch_session(
    true,
    false,
    legacy_args,
    cert.get()
  );
  EXPECT_NE(legacy_session, nullptr);
  if (legacy_session) {
    EXPECT_TRUE(legacy_session->stream_mode.empty());
  }

  auto mirror_args = resolved_launch_args("gamescope_stream", "desktop_display");
  mirror_args.emplace("mirrorDesktop", "1");
  const auto mirror_session = nvhttp::make_launch_session(
    true,
    false,
    mirror_args,
    cert.get()
  );
  EXPECT_NE(mirror_session, nullptr);
  if (mirror_session) {
    EXPECT_TRUE(mirror_session->mirror_desktop);
    EXPECT_TRUE(mirror_session->stream_mode.empty());
  }

  const auto host_default_session = nvhttp::make_launch_session(
    true,
    false,
    resolved_launch_args(),
    cert.get()
  );
  EXPECT_NE(host_default_session, nullptr);
  if (host_default_session) {
    EXPECT_TRUE(host_default_session->stream_mode.empty());
    EXPECT_EQ(host_default_session->expected_stream_mode, "desktop_display");
  }

  if (path_was_set) {
    EXPECT_EQ(setenv("PATH", saved_path.c_str(), 1), 0);
  } else {
    EXPECT_EQ(unsetenv("PATH"), 0);
  }
}

TEST(SessionStreamMode, ExactResolvedLaunchRequiresTopologyAssertion) {
  auto cert = launch_client_cert();
  auto args = resolved_launch_args();
  args.erase("expectedTopology");
  EXPECT_EQ(
    nvhttp::make_launch_session(true, false, args, cert.get()),
    nullptr
  );
}
#endif
