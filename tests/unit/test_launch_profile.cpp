#include "../tests_common.h"

#include <src/launch_profile.h>

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
