#include "../tests_common.h"

#include <src/config.h>
#include <src/device_db.h>

namespace {
  struct linux_display_guard_t {
    bool use_cage_compositor;
    bool headless_mode;

    linux_display_guard_t():
        use_cage_compositor(config::video.linux_display.use_cage_compositor),
        headless_mode(config::video.linux_display.headless_mode) {}

    ~linux_display_guard_t() {
      config::video.linux_display.use_cage_compositor = use_cage_compositor;
      config::video.linux_display.headless_mode = headless_mode;
    }
  };
}  // namespace

TEST(DeviceDbCodecPolicyTests, HeadlessHandheldBigPicturePrefersH264AtTrulyModestBitrates) {
  linux_display_guard_t guard;
  config::video.linux_display.use_cage_compositor = true;
  config::video.linux_display.headless_mode = true;

  const auto normalized = device_db::normalize_preferred_codec(
    "RetroidPocketFlip2",
    "Steam Big Picture",
    std::optional<std::string> {"hevc"},
    std::optional<int> {9000},
    false
  );

  ASSERT_TRUE(normalized.has_value());
  EXPECT_EQ(*normalized, "h264");
}

TEST(DeviceDbCodecPolicyTests, HeadlessHandheldBigPictureKeepsHevcAtRetroidSessionBitrate) {
  linux_display_guard_t guard;
  config::video.linux_display.use_cage_compositor = true;
  config::video.linux_display.headless_mode = true;

  const auto normalized = device_db::normalize_preferred_codec(
    "RetroidPocketFlip2",
    "Steam Big Picture",
    std::optional<std::string> {"hevc"},
    std::optional<int> {15000},
    false
  );

  ASSERT_TRUE(normalized.has_value());
  EXPECT_EQ(*normalized, "hevc");
}

TEST(DeviceDbCodecPolicyTests, HeadlessHandheldBigPictureKeepsHevcAtObservedAiBitrate) {
  linux_display_guard_t guard;
  config::video.linux_display.use_cage_compositor = true;
  config::video.linux_display.headless_mode = true;

  const auto normalized = device_db::normalize_preferred_codec(
    "RetroidPocketFlip2",
    "Steam Big Picture",
    std::optional<std::string> {"hevc"},
    std::optional<int> {10000},
    false
  );

  ASSERT_TRUE(normalized.has_value());
  EXPECT_EQ(*normalized, "hevc");
}

TEST(DeviceDbCodecPolicyTests, NonUiAppsDoNotGetForcedBackToH264) {
  linux_display_guard_t guard;
  config::video.linux_display.use_cage_compositor = true;
  config::video.linux_display.headless_mode = true;

  const auto normalized = device_db::normalize_preferred_codec(
    "RetroidPocketFlip2",
    "Phasmophobia",
    std::optional<std::string> {"hevc"},
    std::optional<int> {15000},
    false
  );

  ASSERT_TRUE(normalized.has_value());
  EXPECT_EQ(*normalized, "hevc");
}

TEST(DeviceDbProfileTests, Quest3UsesHighThroughputVrDefaults) {
  const auto profile = device_db::get_device("Quest 3");

  ASSERT_TRUE(profile.has_value());
  EXPECT_EQ(profile->type, "vr");
  EXPECT_EQ(profile->display_mode, "3840x2160x90");
  EXPECT_EQ(profile->preferred_codec, "av1");
  EXPECT_EQ(profile->ideal_bitrate_kbps, 60000);
  EXPECT_FALSE(profile->virtual_display);
  EXPECT_EQ(device_db::canonicalize_name("Quest 3"), "Meta Quest 3");
  EXPECT_EQ(device_db::friendly_name("Quest 3"), "Meta Quest 3");
}
