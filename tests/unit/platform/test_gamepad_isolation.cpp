/**
 * @file tests/unit/platform/test_gamepad_isolation.cpp
 * @brief Test Linux headless gamepad isolation classification and SDL hint helpers.
 */
#include "../../tests_common.h"

#ifdef __linux__
  #include "src/platform/linux/input/inputtino_gamepad_isolation.h"

  #include <cstdint>
  #include <optional>
  #include <string>
  #include <string_view>

namespace {
  using platf::gamepad::isolation::build_sdl_hint_plan;
  using platf::gamepad::isolation::classify_device;
  using platf::gamepad::isolation::classify_devices;
  using platf::gamepad::isolation::command_with_sdl_env_prefix;
  using platf::gamepad::isolation::device_classification_e;
  using platf::gamepad::isolation::device_snapshot_t;
  using platf::gamepad::isolation::sdl_hint_plan_t;

  device_snapshot_t gamepad(std::string name,
                            std::optional<uint16_t> vendor_id,
                            std::optional<uint16_t> product_id,
                            std::string phys = {},
                            std::string uniq = {}) {
    return device_snapshot_t {
      .event_node = "/dev/input/event9",
      .name = std::move(name),
      .vendor_id = vendor_id,
      .product_id = product_id,
      .version = std::optional<uint16_t> {0x0001},
      .phys = std::move(phys),
      .uniq = std::move(uniq),
      .gamepad_class = true,
    };
  }

  bool reason_contains(const sdl_hint_plan_t &plan, std::string_view needle) {
    return plan.reason.find(needle) != std::string::npos;
  }
}  // namespace

TEST(GamepadIsolationTests, SunshineVirtualNamesClassifyAsPolarisVirtual) {
  EXPECT_EQ(device_classification_e::polaris_virtual,
            classify_device(gamepad(
              "Sunshine X-Box One (virtual) pad",
              std::optional<uint16_t> {0x045e},
              std::optional<uint16_t> {0x02ea}
            )));

  EXPECT_EQ(device_classification_e::polaris_virtual,
            classify_device(gamepad(
              "Sunshine Nintendo (virtual) pad",
              std::optional<uint16_t> {0x057e},
              std::optional<uint16_t> {0x2009}
            )));

  EXPECT_EQ(device_classification_e::polaris_virtual,
            classify_device(gamepad(
              "Sunshine PS5 (virtual) pad",
              std::optional<uint16_t> {0x054c},
              std::optional<uint16_t> {0x0ce6}
            )));
}

TEST(GamepadIsolationTests, PrivateInputtinoPhysOrUniqClassifiesAsPolarisVirtual) {
  EXPECT_EQ(device_classification_e::polaris_virtual,
            classify_device(gamepad(
              "inputtino xbox compatible",
              std::optional<uint16_t> {0x045e},
              std::optional<uint16_t> {0x02ea},
              "02:00:00:00:10:00",
              ""
            )));

  EXPECT_EQ(device_classification_e::polaris_virtual,
            classify_device(gamepad(
              "inputtino dualsense compatible",
              std::optional<uint16_t> {0x054c},
              std::optional<uint16_t> {0x0ce6},
              "",
              "02:00:00:00:00:7f"
            )));
}

TEST(GamepadIsolationTests, PhysicalControllerWithVirtualVidPidRemainsHostVisible) {
  const auto physical_xbox = gamepad(
    "Microsoft X-Box One pad",
    std::optional<uint16_t> {0x045e},
    std::optional<uint16_t> {0x02ea},
    "usb-0000:07:00.3-1/input0",
    ""
  );

  EXPECT_EQ(device_classification_e::host_visible, classify_device(physical_xbox));
}

TEST(GamepadIsolationTests, NonGamepadClassDeviceIsIgnored) {
  auto keyboard = gamepad(
    "OwLab SUIT80 Keyboard",
    std::optional<uint16_t> {0x4f53},
    std::optional<uint16_t> {0x5355}
  );
  keyboard.gamepad_class = false;

  EXPECT_EQ(device_classification_e::ignored, classify_device(keyboard));
}

TEST(GamepadIsolationTests, DisjointHostAndVirtualControllersGenerateSdlHints) {
  const auto devices = classify_devices({
    gamepad("Sunshine X-Box One (virtual) pad", std::optional<uint16_t> {0x045e}, std::optional<uint16_t> {0x02ea}),
    gamepad("Logitech Gamepad F310", std::optional<uint16_t> {0x046d}, std::optional<uint16_t> {0xc216}),
  });

  const auto plan = build_sdl_hint_plan(devices);

  ASSERT_TRUE(plan.applied());
  EXPECT_EQ("0x046d/0xc216", plan.env.at("SDL_GAMECONTROLLER_IGNORE_DEVICES"));
  EXPECT_EQ("0x045e/0x02ea", plan.env.at("SDL_GAMECONTROLLER_IGNORE_DEVICES_EXCEPT"));
  EXPECT_TRUE(reason_contains(plan, "best-effort"));
}

TEST(GamepadIsolationTests, MissingHostVidPidSkipsSdlHints) {
  const auto devices = classify_devices({
    gamepad("Sunshine X-Box One (virtual) pad", std::optional<uint16_t> {0x045e}, std::optional<uint16_t> {0x02ea}),
    gamepad("Mystery Controller", std::nullopt, std::nullopt),
  });

  const auto plan = build_sdl_hint_plan(devices);

  EXPECT_FALSE(plan.applied());
  EXPECT_TRUE(reason_contains(plan, "missing VID/PID"));
}

TEST(GamepadIsolationTests, OverlappingHostAndVirtualVidPidSkipsSdlHints) {
  const auto devices = classify_devices({
    gamepad("Sunshine X-Box One (virtual) pad", std::optional<uint16_t> {0x045e}, std::optional<uint16_t> {0x02ea}),
    gamepad("Microsoft X-Box One pad", std::optional<uint16_t> {0x045e}, std::optional<uint16_t> {0x02ea}),
  });

  const auto plan = build_sdl_hint_plan(devices);

  EXPECT_FALSE(plan.applied());
  EXPECT_TRUE(reason_contains(plan, "overlaps"));
}

TEST(GamepadIsolationTests, SdlEnvPrefixUsesShellSafeQuoting) {
  const auto quote = std::string(1, static_cast<char>(39));
  const auto escaped_quote = std::string(1, static_cast<char>(92)) + quote;

  sdl_hint_plan_t plan;
  plan.env["SDL_GAMECONTROLLER_IGNORE_DEVICES"] = "0x1234/0xab" + quote + "cd";

  const auto expected = "env SDL_GAMECONTROLLER_IGNORE_DEVICES=" + quote +
                        "0x1234/0xab" + quote + escaped_quote + quote + "cd" + quote +
                        " steam --gamepadui";

  EXPECT_EQ(expected, command_with_sdl_env_prefix("steam --gamepadui", plan));
}
#endif
