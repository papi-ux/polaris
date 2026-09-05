#include "../tests_common.h"

#include <src/launch_profile.h>

TEST(LaunchProfileTests, NonLinuxTopologyUsesDeterministicPrecedence) {
  const auto explicit_desktop = launch_profile::resolve_non_linux_topology(
    "desktop_display", true, true, true, true, false
  );
  EXPECT_EQ(explicit_desktop.topology, "desktop_display");
  EXPECT_FALSE(explicit_desktop.launch_owns_refresh_rate);
  EXPECT_EQ(explicit_desktop.source, "client_launch_request");

  const auto blank_explicit_desktop =
    launch_profile::resolve_non_linux_topology(
      "", true, true, true, true, false
    );
  EXPECT_EQ(blank_explicit_desktop.topology, "desktop_display");
  EXPECT_FALSE(blank_explicit_desktop.launch_owns_refresh_rate);
  EXPECT_EQ(
    blank_explicit_desktop.reason_code,
    "explicit_desktop_lock"
  );

  const auto paired_virtual = launch_profile::resolve_non_linux_topology(
    "desktop_display", false, true, false, true, false
  );
  EXPECT_EQ(paired_virtual.topology, "host_virtual_display");
  EXPECT_TRUE(paired_virtual.launch_owns_refresh_rate);
  EXPECT_EQ(paired_virtual.source, "paired_client_settings");

  const auto app_virtual = launch_profile::resolve_non_linux_topology(
    "desktop_display", false, false, true, true, false
  );
  EXPECT_EQ(app_virtual.topology, "host_virtual_display");
  EXPECT_TRUE(app_virtual.launch_owns_refresh_rate);
  EXPECT_EQ(app_virtual.source, "app_configuration");

  const auto unsupported_private = launch_profile::resolve_non_linux_topology(
    "windowed_stream", true, false, false, true, false
  );
  EXPECT_EQ(unsupported_private.topology, "desktop_display");
  EXPECT_FALSE(unsupported_private.launch_owns_refresh_rate);
  EXPECT_TRUE(unsupported_private.normalized);
  EXPECT_EQ(
    unsupported_private.reason_code,
    "unsupported_topology_normalized"
  );

  const auto desktop_only = launch_profile::resolve_non_linux_topology(
    "host_virtual_display", true, true, true, false, false
  );
  EXPECT_EQ(desktop_only.topology, "desktop_display");
  EXPECT_FALSE(desktop_only.launch_owns_refresh_rate);
  EXPECT_TRUE(desktop_only.normalized);

  const auto forced_virtual = launch_profile::resolve_non_linux_topology(
    "desktop_display", true, false, false, true, true
  );
  EXPECT_EQ(forced_virtual.topology, "host_virtual_display");
  EXPECT_TRUE(forced_virtual.launch_owns_refresh_rate);
  EXPECT_EQ(forced_virtual.reason_code, "host_requires_virtual_display");
}

TEST(LaunchProfileTests, AutoPreservesUnlockedClientRequestWithoutDeviceMutation) {
  launch_profile::request_t request;
  request.device_name = "RetroidPocket6";
  request.app_name = "Control Ultimate Edition";
  request.preset = "auto";
  request.requested_width = 1920;
  request.requested_height = 1080;
  request.requested_fps = 120000;
  request.hdr_requested = false;

  const auto resolved = launch_profile::resolve(request);

  EXPECT_EQ(resolved.width, 1920);
  EXPECT_EQ(resolved.height, 1080);
  EXPECT_EQ(resolved.fps, 120000);
  EXPECT_FALSE(resolved.target_bitrate_kbps.has_value());
  EXPECT_FALSE(resolved.preferred_codec.has_value());
  EXPECT_EQ(resolved.fields.at("display_mode").at("source"), "client_launch_request");
  EXPECT_FALSE(resolved.fields.at("display_mode").at("locked").get<bool>());
  EXPECT_EQ(resolved.fields.at("display_width").at("source"), "client_launch_request");
  EXPECT_EQ(resolved.fields.at("display_height").at("source"), "client_launch_request");
  EXPECT_EQ(resolved.fields.at("target_fps").at("source"), "client_launch_request");
}

TEST(LaunchProfileTests, StabilityNormalizesPairedBitrateButPreservesExplicitDisplayLock) {
  launch_profile::request_t request;
  request.device_name = "RetroidPocket6";
  request.preset = "stability";
  request.requested_width = 1920;
  request.requested_height = 1080;
  request.requested_fps = 120000;
  request.display_locked = true;
  request.paired_width = 1280;
  request.paired_height = 720;
  request.paired_fps = 60000;
  request.paired_bitrate_kbps = 24000;

  const auto resolved = launch_profile::resolve(request);

  EXPECT_EQ(resolved.fps, 120000);
  ASSERT_TRUE(resolved.target_bitrate_kbps.has_value());
  EXPECT_LE(*resolved.target_bitrate_kbps, 15000);
  EXPECT_TRUE(resolved.fields.at("display_mode").at("locked"));
  EXPECT_FALSE(resolved.fields.at("target_bitrate_kbps").at("locked"));
  EXPECT_EQ(resolved.fields.at("target_bitrate_kbps").at("source"), "device_profile_v1");
}

TEST(LaunchProfileTests, ExplicitBitrateWinsOverPairedAndReportsProvenance) {
  launch_profile::request_t request;
  request.device_name = "RetroidPocket6";
  request.preset = "quality";
  request.requested_width = 1920;
  request.requested_height = 1080;
  request.requested_fps = 120000;
  request.explicit_bitrate_kbps = 32000;
  request.bitrate_locked = true;
  request.paired_bitrate_kbps = 24000;

  const auto resolved = launch_profile::resolve(request);

  ASSERT_EQ(resolved.target_bitrate_kbps, 32000);
  const auto &field = resolved.fields.at("target_bitrate_kbps");
  EXPECT_EQ(field.at("source"), "explicit_launch_request");
  EXPECT_EQ(field.at("reason_code"), "requested_bitrate_lock");
  EXPECT_TRUE(field.at("locked"));
  EXPECT_FALSE(field.at("normalized"));
}

TEST(LaunchProfileTests, UnlockedRequestYieldsToPairedDisplayAndBitrate) {
  launch_profile::request_t request;
  request.device_name = "RetroidPocket6";
  request.preset = "auto";
  request.requested_width = 3840;
  request.requested_height = 2160;
  request.requested_fps = 120000;
  request.display_locked = false;
  request.paired_width = 1920;
  request.paired_height = 1080;
  request.paired_fps = 60000;
  request.explicit_bitrate_kbps = 50000;
  request.bitrate_locked = false;
  request.paired_bitrate_kbps = 24000;

  const auto resolved = launch_profile::resolve(request);

  EXPECT_EQ(resolved.width, 1920);
  EXPECT_EQ(resolved.height, 1080);
  EXPECT_EQ(resolved.fps, 60000);
  ASSERT_EQ(resolved.target_bitrate_kbps, 24000);
  EXPECT_EQ(resolved.fields.at("display_mode").at("source"), "paired_client");
  EXPECT_EQ(resolved.fields.at("target_bitrate_kbps").at("source"), "paired_client");
  EXPECT_FALSE(resolved.fields.at("display_mode").at("locked"));
  EXPECT_FALSE(resolved.fields.at("target_bitrate_kbps").at("locked"));
}

TEST(LaunchProfileTests, HardHostBitrateCapNormalizesExplicitRequestLast) {
  launch_profile::request_t request;
  request.device_name = "RetroidPocket6";
  request.preset = "quality";
  request.requested_width = 1920;
  request.requested_height = 1080;
  request.requested_fps = 120000;
  request.explicit_bitrate_kbps = 50000;
  request.bitrate_locked = true;
  request.configured_bitrate_kbps = 40000;

  const auto resolved = launch_profile::resolve(request);

  ASSERT_EQ(resolved.target_bitrate_kbps, 40000);
  const auto &field = resolved.fields.at("target_bitrate_kbps");
  EXPECT_EQ(field.at("source"), "capability_validation");
  EXPECT_EQ(field.at("reason_code"), "host_bitrate_cap");
  EXPECT_TRUE(field.at("locked"));
  EXPECT_TRUE(field.at("normalized"));
}

TEST(LaunchProfileTests, HardHdrCapabilityNormalizesAnExplicitLockLast) {
  launch_profile::request_t request;
  request.device_name = "RetroidPocket6";
  request.preset = "quality";
  request.requested_width = 1920;
  request.requested_height = 1080;
  request.requested_fps = 120000;
  request.hdr_requested = true;
  request.hdr_locked = true;

  const auto resolved = launch_profile::resolve(request);

  EXPECT_FALSE(resolved.hdr);
  const auto &field = resolved.fields.at("hdr");
  EXPECT_EQ(field.at("source"), "capability_validation");
  EXPECT_EQ(field.at("reason_code"), "paired_device_hdr_unsupported");
  EXPECT_TRUE(field.at("locked"));
  EXPECT_TRUE(field.at("normalized"));
}

TEST(LaunchProfileTests, MeteredBitrateLockWinsOverStabilityPreset) {
  launch_profile::request_t request;
  request.device_name = "RetroidPocket6";
  request.preset = "stability";
  request.requested_width = 1920;
  request.requested_height = 1080;
  request.requested_fps = 120000;
  request.explicit_bitrate_kbps = 6000;
  request.bitrate_locked = true;
  request.configured_bitrate_kbps = 100000;

  const auto resolved = launch_profile::resolve(request);

  ASSERT_EQ(resolved.target_bitrate_kbps, 6000);
  const auto &field = resolved.fields.at("target_bitrate_kbps");
  EXPECT_EQ(field.at("source"), "explicit_launch_request");
  EXPECT_EQ(field.at("reason_code"), "requested_bitrate_lock");
  EXPECT_TRUE(field.at("locked"));
  EXPECT_FALSE(field.at("normalized"));
}

TEST(LaunchProfileTests, HostMaximumAloneNeverBecomesALaunchTarget) {
  launch_profile::request_t request;
  request.device_name = "RetroidPocket6";
  request.preset = "auto";
  request.requested_width = 1920;
  request.requested_height = 1080;
  request.requested_fps = 120000;
  request.configured_bitrate_kbps = 100000;

  const auto resolved = launch_profile::resolve(request);

  EXPECT_FALSE(resolved.target_bitrate_kbps.has_value());
  EXPECT_FALSE(resolved.fields.contains("target_bitrate_kbps"));
}

TEST(LaunchProfileTests, StabilityIsVersionedConservativeAndNeverContainsTopology) {
  launch_profile::request_t request;
  request.device_name = "ROG Ally";
  request.preset = "stability";
  request.requested_width = 2560;
  request.requested_height = 1440;
  request.requested_fps = 120000;
  request.configured_bitrate_kbps = 100000;

  const auto resolved = launch_profile::resolve(request);

  EXPECT_LE(resolved.width, 1920);
  EXPECT_LE(resolved.height, 1080);
  EXPECT_LE(resolved.fps, 60000);
  ASSERT_TRUE(resolved.target_bitrate_kbps.has_value());
  EXPECT_LE(*resolved.target_bitrate_kbps, 15000);
  EXPECT_FALSE(resolved.fields.contains("virtual_display"));
  EXPECT_FALSE(resolved.fields.contains("stream_mode"));
}

TEST(LaunchProfileTests, HighFpsLocksRequestedCadence) {
  launch_profile::request_t request;
  request.device_name = "RetroidPocket6";
  request.preset = "high_fps";
  request.requested_width = 1920;
  request.requested_height = 1080;
  request.requested_fps = 120000;
  request.paired_width = 1280;
  request.paired_height = 720;
  request.paired_fps = 60000;

  const auto resolved = launch_profile::resolve(request);

  EXPECT_EQ(resolved.width, 1280);
  EXPECT_EQ(resolved.height, 720);
  EXPECT_EQ(resolved.fps, 120000);
  EXPECT_FALSE(resolved.fields.at("display_mode").at("locked"));
  EXPECT_EQ(resolved.fields.at("display_mode").at("source"), "composed_display_components");
  EXPECT_EQ(resolved.fields.at("display_mode").at("reason_code"), "mixed_display_provenance");
  EXPECT_EQ(resolved.fields.at("display_width").at("source"), "paired_client");
  EXPECT_EQ(resolved.fields.at("display_height").at("source"), "paired_client");
  EXPECT_FALSE(resolved.fields.at("display_width").at("locked").get<bool>());
  EXPECT_FALSE(resolved.fields.at("display_height").at("locked").get<bool>());
  EXPECT_EQ(resolved.fields.at("target_fps").at("source"), "client_launch_request");
  EXPECT_EQ(resolved.fields.at("target_fps").at("reason_code"), "high_fps_cadence_lock");
  EXPECT_TRUE(resolved.fields.at("target_fps").at("locked").get<bool>());
}

TEST(LaunchProfileTests, ClientProfileHdrWinsOnlyWhenLaunchDoesNotLockHdr) {
  launch_profile::request_t request;
  request.device_name = "HDR-capable client";
  request.requested_width = 1920;
  request.requested_height = 1080;
  request.requested_fps = 60000;
  request.hdr_requested = false;
  request.client_profile_hdr = true;

  const auto resolved = launch_profile::resolve(request);

  EXPECT_TRUE(resolved.hdr);
  EXPECT_EQ(resolved.fields.at("hdr").at("source"), "client_profile");
  EXPECT_EQ(resolved.fields.at("hdr").at("reason_code"), "client_profile_hdr_lock");
  EXPECT_TRUE(resolved.fields.at("hdr").at("locked").get<bool>());
}

TEST(LaunchProfileTests, ExplicitResolvedHdrWinsOverClientProfileHdr) {
  launch_profile::request_t request;
  request.device_name = "HDR-capable client";
  request.requested_width = 1920;
  request.requested_height = 1080;
  request.requested_fps = 60000;
  request.hdr_requested = false;
  request.hdr_locked = true;
  request.client_profile_hdr = true;

  const auto resolved = launch_profile::resolve(request);

  EXPECT_FALSE(resolved.hdr);
  EXPECT_EQ(resolved.fields.at("hdr").at("source"), "explicit_launch_request");
  EXPECT_EQ(resolved.fields.at("hdr").at("reason_code"), "requested_hdr_lock");
  EXPECT_TRUE(resolved.fields.at("hdr").at("locked").get<bool>());
}

TEST(LaunchProfileTests, HardRefreshCapsNormalizeTheFinalFpsWithProvenance) {
  launch_profile::request_t request;
  request.requested_width = 1920;
  request.requested_height = 1080;
  request.requested_fps = 60000;
  request.paired_width = 1920;
  request.paired_height = 1080;
  request.paired_fps = 120000;
  request.client_max_fps = 90000;
  request.host_max_fps = 60000;

  const auto resolved = launch_profile::resolve(request);

  EXPECT_EQ(resolved.fps, 60000);
  ASSERT_TRUE(resolved.fields.contains("target_fps"));
  EXPECT_EQ(resolved.fields.at("target_fps").at("source"), "capability_validation");
  EXPECT_EQ(resolved.fields.at("target_fps").at("reason_code"), "host_refresh_cap");
  EXPECT_TRUE(resolved.fields.at("target_fps").at("normalized").get<bool>());
}

TEST(LaunchProfileTests, HostHdrCapabilityCanNormalizeAnExplicitRequest) {
  launch_profile::request_t request;
  request.device_name = "HDR-capable client";
  request.requested_width = 1920;
  request.requested_height = 1080;
  request.requested_fps = 60000;
  request.display_locked = true;
  request.hdr_requested = true;
  request.hdr_locked = true;
  request.host_hdr_capable = false;

  const auto resolved = launch_profile::resolve(request);

  EXPECT_FALSE(resolved.hdr);
  EXPECT_EQ(resolved.fields.at("hdr").at("source"), "capability_validation");
  EXPECT_EQ(resolved.fields.at("hdr").at("reason_code"), "host_encoder_hdr_unsupported");
  EXPECT_TRUE(resolved.fields.at("hdr").at("locked").get<bool>());
  EXPECT_TRUE(resolved.fields.at("hdr").at("normalized").get<bool>());
}

namespace {
  launch_profile::explicit_field_lookup_t lookup_from(std::vector<std::pair<std::string, std::string>> args) {
    return [args = std::move(args)](std::string_view name) -> std::optional<std::string> {
      for (const auto &[key, value] : args) {
        if (key == name) {
          return value;
        }
      }
      return std::nullopt;
    };
  }
}  // namespace

TEST(LaunchProfileExplicitFields, NamesACommaDecimalFpsInsteadOfTheGenericSentence) {
  // nova#275: a host running under a comma-decimal locale rejected every
  // launch with "must be complete and within supported bounds".
  const auto fields = launch_profile::parse_explicit_launch_fields(
    lookup_from({{"width", "1920"}, {"height", "1080"}, {"fps", "60,0"}, {"bitrate_kbps", "20000"}})
  );
  ASSERT_EQ(fields.problems.size(), 1u);
  EXPECT_EQ(fields.problems[0].field, "fps");
  EXPECT_NE(fields.problems[0].reason.find("got '60,0'"), std::string::npos);
  EXPECT_NE(fields.problems[0].reason.find("dot as the decimal separator"), std::string::npos);
  const auto text = launch_profile::describe_explicit_launch_rejection(fields);
  EXPECT_NE(text.find("Explicit launch fields were rejected: fps"), std::string::npos);
  EXPECT_EQ(text.find("must be complete and within supported bounds"), std::string::npos);
}

TEST(LaunchProfileExplicitFields, AcceptsACompleteRequestAndKeepsTheOldUnits) {
  const auto fields = launch_profile::parse_explicit_launch_fields(lookup_from({
    {"width", "2560"}, {"height", "1440"}, {"fps", "119.88"}, {"bitrate_kbps", "35000"},
    {"client_max_fps", "120"}, {"display_locked", "TRUE"}, {"bitrate_locked", "0"}, {"topology_locked", "1"}, {"hdr", "on"},
  }));
  EXPECT_TRUE(fields.problems.empty());
  EXPECT_EQ(fields.width, 2560);
  EXPECT_EQ(fields.height, 1440);
  EXPECT_EQ(fields.fps_millihertz, 119880);
  EXPECT_EQ(fields.client_max_fps_millihertz, 120000);
  EXPECT_EQ(fields.bitrate_kbps, 35000);
  EXPECT_TRUE(fields.display_locked);
  EXPECT_FALSE(fields.bitrate_locked);
  EXPECT_TRUE(fields.topology_locked);
  EXPECT_EQ(fields.hdr, true);
  EXPECT_TRUE(fields.complete_explicit_mode());
  EXPECT_EQ(launch_profile::describe_explicit_launch_rejection(fields), "Explicit launch fields must be complete and within supported bounds.");
}

TEST(LaunchProfileExplicitFields, NamesMissingModeFieldsAndLockPrerequisites) {
  const auto partial = launch_profile::parse_explicit_launch_fields(lookup_from({{"width", "1920"}, {"display_locked", "1"}}));
  ASSERT_EQ(partial.problems.size(), 2u);
  EXPECT_EQ(partial.problems[0].field, "width/height/fps");
  EXPECT_NE(partial.problems[0].reason.find("missing height, fps"), std::string::npos);
  EXPECT_EQ(partial.problems[1].field, "display_locked");

  const auto locked = launch_profile::parse_explicit_launch_fields(lookup_from({{"bitrate_locked", "true"}}));
  ASSERT_EQ(locked.problems.size(), 1u);
  EXPECT_EQ(locked.problems[0].reason, "bitrate_locked requires bitrate_kbps");

  const auto flag = launch_profile::parse_explicit_launch_fields(lookup_from({{"topology_locked", "maybe"}}));
  ASSERT_EQ(flag.problems.size(), 1u);
  EXPECT_NE(flag.problems[0].reason.find("topology_locked must be 1 or 0; got 'maybe'"), std::string::npos);
}

TEST(LaunchProfileExplicitFields, BoundsAndEchoesOnlyASafeSliceOfTheValue) {
  const std::string hostile = std::string(40, 'x') + "\x01\n";
  const auto fields = launch_profile::parse_explicit_launch_fields(lookup_from({{"bitrate_kbps", hostile}, {"width", "100000"}}));
  ASSERT_EQ(fields.problems.size(), 2u);
  EXPECT_EQ(fields.problems[0].field, "width");
  EXPECT_NE(fields.problems[0].reason.find("between 320 and 16384; got '100000'"), std::string::npos);
  EXPECT_EQ(fields.problems[1].field, "bitrate_kbps");
  EXPECT_NE(fields.problems[1].reason.find(std::string(32, 'x') + "...'"), std::string::npos);
  EXPECT_EQ(fields.problems[1].reason.find('\n'), std::string::npos);
  EXPECT_EQ(fields.problems[1].reason.find('\x01'), std::string::npos);
}
