/**
 * @file tests/unit/platform/test_gamepad_isolation.cpp
 * @brief Test Linux headless gamepad isolation classification and SDL hint helpers.
 */
#include "../../tests_common.h"

#ifdef __linux__
  #include "src/platform/linux/input/inputtino_gamepad_isolation.h"

  #include <algorithm>
  #include <cstdint>
  #include <optional>
  #include <string>
  #include <string_view>
  #include <vector>

namespace {
  using platf::gamepad::isolation::build_sdl_hint_plan;
  using platf::gamepad::isolation::build_strict_gamepad_isolation_plan;
  using platf::gamepad::isolation::classify_device;
  using platf::gamepad::isolation::classify_devices;
  using platf::gamepad::isolation::command_with_headless_gamepad_isolation;
  using platf::gamepad::isolation::command_with_sdl_env_prefix;
  using platf::gamepad::isolation::device_classification_e;
  using platf::gamepad::isolation::device_snapshot_t;
  using platf::gamepad::isolation::isolation_mode_e;
  using platf::gamepad::isolation::sdl_hint_plan_t;
  using platf::gamepad::isolation::strict_isolation_options_t;

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

  bool text_contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
  }

  bool text_lacks(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) == std::string_view::npos;
  }

  strict_isolation_options_t strict_options(bool bubblewrap_available = true) {
    strict_isolation_options_t options;
    options.bubblewrap_available = bubblewrap_available;
    options.bubblewrap_path = "/usr/bin/bwrap";
    return options;
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

TEST(GamepadIsolationTests, PolarisReporterVirtualNamesClassifyAsPolarisVirtual) {
  EXPECT_EQ(device_classification_e::polaris_virtual,
            classify_device(gamepad(
              "Polaris X-Box One (virtual) pad",
              std::optional<uint16_t> {0x045e},
              std::optional<uint16_t> {0x02ea}
            )));

  EXPECT_EQ(device_classification_e::polaris_virtual,
            classify_device(gamepad(
              "Polaris Nintendo (virtual) pad",
              std::optional<uint16_t> {0x057e},
              std::optional<uint16_t> {0x2009}
            )));

  EXPECT_EQ(device_classification_e::polaris_virtual,
            classify_device(gamepad(
              "Polaris PS5 (virtual) pad",
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

TEST(GamepadIsolationTests, NoHostPhysicalControllersLeaveLaunchUnwrappedEvenWhenVirtualNodesAreRegistered) {
  const auto devices = classify_devices({
    gamepad("Sunshine X-Box One (virtual) pad", std::optional<uint16_t> {0x045e}, std::optional<uint16_t> {0x02ea}),
  });

  const auto plan = build_strict_gamepad_isolation_plan(
    devices,
    {"/dev/input/event42", "/dev/input/js42"},
    strict_options()
  );
  const auto command = command_with_headless_gamepad_isolation("steam --gamepadui", plan);

  EXPECT_EQ(isolation_mode_e::disabled, plan.mode);
  EXPECT_FALSE(plan.strict_applied());
  EXPECT_EQ("steam --gamepadui", command);
  EXPECT_TRUE(text_lacks(command, "--tmpfs /run/udev"));
  EXPECT_TRUE(text_lacks(command, "--tmpfs /sys/class/input"));
  EXPECT_TRUE(text_lacks(command, "--dev-bind /dev/input/event42 /dev/input/event42"));
  EXPECT_TRUE(text_lacks(command, "--dev-bind /dev/input/js42 /dev/input/js42"));
  EXPECT_TRUE(text_contains(plan.reason, "no host physical controllers visible"));
}

TEST(GamepadIsolationTests, StrictPlanWithNoRegisteredNodesStillHidesHostDevices) {
  auto host = gamepad("Microsoft X-Box One pad", std::optional<uint16_t> {0x045e}, std::optional<uint16_t> {0x02ea});
  host.event_node = "/dev/input/event3";

  const auto plan = build_strict_gamepad_isolation_plan(
    classify_devices({host}),
    {},
    strict_options()
  );
  const auto command = command_with_headless_gamepad_isolation("steam --gamepadui", plan);

  EXPECT_EQ(isolation_mode_e::strict_bwrap, plan.mode);
  EXPECT_TRUE(plan.strict_applied());
  EXPECT_TRUE(plan.allowed_nodes.empty());
  EXPECT_TRUE(text_contains(command, "--tmpfs /dev/input"));
  EXPECT_TRUE(text_contains(command, "--tmpfs /sys/class/input"));
  EXPECT_TRUE(text_contains(command, "--tmpfs /sys/class/hidraw"));
  EXPECT_TRUE(text_lacks(command, "/dev/input/event3"));
  EXPECT_TRUE(text_contains(plan.reason, "no registered Polaris virtual gamepad nodes"));
}

TEST(GamepadIsolationTests, StrictWrapperBindsOnlyRegisteredVirtualEventJsAndHidrawNodes) {
  auto host = gamepad("Microsoft X-Box One pad", std::optional<uint16_t> {0x045e}, std::optional<uint16_t> {0x02ea});
  host.event_node = "/dev/input/event3";
  auto virtual_pad = gamepad("Sunshine X-Box One (virtual) pad", std::optional<uint16_t> {0x045e}, std::optional<uint16_t> {0x02ea});
  virtual_pad.event_node = "/dev/input/event9";

  const auto plan = build_strict_gamepad_isolation_plan(
    classify_devices({host, virtual_pad}),
    {"/dev/input/event9", "/dev/input/js9", "/dev/hidraw9"},
    strict_options()
  );
  const auto command = command_with_headless_gamepad_isolation("steam --gamepadui", plan);

  EXPECT_TRUE(plan.strict_applied());
  EXPECT_TRUE(text_contains(command, "--dev-bind /dev/input/event9 /dev/input/event9"));
  EXPECT_TRUE(text_contains(command, "--dev-bind /dev/input/js9 /dev/input/js9"));
  EXPECT_TRUE(text_contains(command, "--dev-bind-try /dev/hidraw9 /dev/hidraw9"));
  EXPECT_TRUE(text_lacks(command, "/dev/input/event3"));
  EXPECT_TRUE(text_lacks(command, "/dev/input/js3"));
  EXPECT_TRUE(text_lacks(command, "/dev/hidraw3"));
  EXPECT_TRUE(text_lacks(command, "--dev-bind-try /dev/fd /dev/fd"));
}

TEST(GamepadIsolationTests, SameVidPidHostAndVirtualAllowsOnlyRegisteredVirtualNode) {
  auto host = gamepad("Microsoft X-Box One pad", std::optional<uint16_t> {0x045e}, std::optional<uint16_t> {0x02ea});
  host.event_node = "/dev/input/event4";
  auto virtual_pad = gamepad("Sunshine X-Box One (virtual) pad", std::optional<uint16_t> {0x045e}, std::optional<uint16_t> {0x02ea});
  virtual_pad.event_node = "/dev/input/event12";

  const auto plan = build_strict_gamepad_isolation_plan(
    classify_devices({host, virtual_pad}),
    {"/dev/input/event12"},
    strict_options()
  );
  const auto command = command_with_headless_gamepad_isolation("steam", plan);

  EXPECT_EQ(isolation_mode_e::strict_bwrap, plan.mode);
  EXPECT_TRUE(text_contains(command, "/dev/input/event12"));
  EXPECT_TRUE(text_lacks(command, "/dev/input/event4"));
}

TEST(GamepadIsolationTests, VirtualNameAloneDoesNotCreateStrictBindWhenRegistryDisagrees) {
  auto virtual_pad = gamepad("Polaris X-Box One (virtual) pad", std::optional<uint16_t> {0x045e}, std::optional<uint16_t> {0x02ea});
  virtual_pad.event_node = "/dev/input/event20";

  auto host = gamepad("Logitech Gamepad F310", std::optional<uint16_t> {0x046d}, std::optional<uint16_t> {0xc216});
  host.event_node = "/dev/input/event3";
  const auto plan = build_strict_gamepad_isolation_plan(
    classify_devices({host, virtual_pad}),
    {"/dev/input/event21"},
    strict_options()
  );
  const auto command = command_with_headless_gamepad_isolation("steam", plan);

  EXPECT_TRUE(plan.strict_applied());
  EXPECT_TRUE(text_contains(command, "/dev/input/event21"));
  EXPECT_TRUE(text_lacks(command, "/dev/input/event20"));
}

TEST(GamepadIsolationTests, UnavailableBubblewrapFallsBackHonestlyToSdlHints) {
  const auto devices = classify_devices({
    gamepad("Sunshine X-Box One (virtual) pad", std::optional<uint16_t> {0x045e}, std::optional<uint16_t> {0x02ea}),
    gamepad("Logitech Gamepad F310", std::optional<uint16_t> {0x046d}, std::optional<uint16_t> {0xc216}),
  });

  const auto plan = build_strict_gamepad_isolation_plan(
    devices,
    {"/dev/input/event9"},
    strict_options(false)
  );
  const auto command = command_with_headless_gamepad_isolation("steam --gamepadui", plan);

  EXPECT_EQ(isolation_mode_e::best_effort_sdl, plan.mode);
  EXPECT_FALSE(plan.strict_applied());
  EXPECT_TRUE(plan.fallback_sdl.applied());
  EXPECT_TRUE(text_contains(plan.reason, "strict"));
  EXPECT_TRUE(text_contains(plan.reason, "not active"));
  EXPECT_TRUE(text_contains(command, "SDL_GAMECONTROLLER_IGNORE_DEVICES="));
  EXPECT_TRUE(text_lacks(command, "bwrap"));
}


TEST(GamepadIsolationTests, FailedBubblewrapProbeFallsBackHonestlyToSdlHints) {
  const auto devices = classify_devices({
    gamepad("Sunshine X-Box One (virtual) pad", std::optional<uint16_t> {0x045e}, std::optional<uint16_t> {0x02ea}),
    gamepad("Logitech Gamepad F310", std::optional<uint16_t> {0x046d}, std::optional<uint16_t> {0xc216}),
  });

  auto options = strict_options();
  options.bubblewrap_usable = false;
  const auto plan = build_strict_gamepad_isolation_plan(
    devices,
    {"/dev/input/event9"},
    options
  );

  EXPECT_EQ(isolation_mode_e::best_effort_sdl, plan.mode);
  EXPECT_FALSE(plan.strict_applied());
  EXPECT_TRUE(plan.fallback_sdl.applied());
  EXPECT_TRUE(text_contains(plan.reason, "probe"));
  EXPECT_TRUE(text_contains(plan.reason, "not active"));
}

TEST(GamepadIsolationTests, StrictWrapperRejectsMalformedVirtualNodesAndQuotesBwrapPath) {
  auto options = strict_options();
  options.bubblewrap_path = "/tmp/bwrap helper";
  auto host = gamepad("Logitech Gamepad F310", std::optional<uint16_t> {0x046d}, std::optional<uint16_t> {0xc216});
  host.event_node = "/dev/input/event3";
  const auto plan = build_strict_gamepad_isolation_plan(
    classify_devices({host, gamepad("Sunshine X-Box One (virtual) pad", std::optional<uint16_t> {0x045e}, std::optional<uint16_t> {0x02ea})}),
    {"/dev/input/event9;touch /tmp/polaris-pwned", "/dev/input/js9", "/dev/hidraw9;echo nope", "/dev/hidraw9"},
    options
  );
  const auto command = command_with_headless_gamepad_isolation("steam", plan);
  const auto expected_bwrap = std::string(1, static_cast<char>(39)) + "/tmp/bwrap helper" + std::string(1, static_cast<char>(39));

  EXPECT_TRUE(plan.strict_applied());
  EXPECT_TRUE(text_contains(command, expected_bwrap));
  EXPECT_TRUE(text_contains(command, "--dev-bind /dev/input/js9 /dev/input/js9"));
  EXPECT_TRUE(text_contains(command, "--dev-bind-try /dev/hidraw9 /dev/hidraw9"));
  EXPECT_TRUE(text_lacks(command, "touch /tmp/polaris-pwned"));
  EXPECT_TRUE(text_lacks(command, "echo nope"));
  EXPECT_TRUE(text_lacks(command, "/dev/input/event9;"));
  EXPECT_TRUE(text_lacks(command, "/dev/hidraw9;"));
  EXPECT_TRUE(text_lacks(command, "/tmp/bwrap helper --bind"));
}

TEST(GamepadIsolationTests, StrictWrapperQuotesOriginalCommandSafely) {
  const auto quote = std::string(1, static_cast<char>(39));
  auto host = gamepad("Logitech Gamepad F310", std::optional<uint16_t> {0x046d}, std::optional<uint16_t> {0xc216});
  host.event_node = "/dev/input/event3";
  const auto plan = build_strict_gamepad_isolation_plan(
    classify_devices({host, gamepad("Sunshine X-Box One (virtual) pad", std::optional<uint16_t> {0x045e}, std::optional<uint16_t> {0x02ea})}),
    {"/dev/input/event9"},
    strict_options()
  );

  const auto command = command_with_headless_gamepad_isolation("steam -silent " + quote + "quoted game" + quote, plan);

  EXPECT_TRUE(text_contains(command, "/bin/sh -lc"));
  EXPECT_TRUE(text_contains(command, "steam -silent"));
  EXPECT_TRUE(text_contains(command, "quoted game"));
}

TEST(GamepadIsolationTests, StrictWrapperDoesNotExposeBroadInputOrHidrawPaths) {
  const auto plan = build_strict_gamepad_isolation_plan(
    classify_devices({gamepad("Sunshine X-Box One (virtual) pad", std::optional<uint16_t> {0x045e}, std::optional<uint16_t> {0x02ea})}),
    {"/dev/input/event9"},
    strict_options()
  );
  const auto command = command_with_headless_gamepad_isolation("steam", plan);

  EXPECT_TRUE(text_lacks(command, "--dev-bind /dev/input /dev/input"));
  EXPECT_TRUE(text_lacks(command, "--dev-bind /dev/hidraw"));
  EXPECT_TRUE(text_lacks(command, "--bind /dev/input/by-id /dev/input/by-id"));
  EXPECT_TRUE(text_lacks(command, "--bind /dev/input/by-path /dev/input/by-path"));
}

TEST(GamepadIsolationTests, StrictWrapperHidesHostUdevAndInputSysfsMetadata) {
  auto host = gamepad("Logitech Gamepad F310", std::optional<uint16_t> {0x046d}, std::optional<uint16_t> {0xc216});
  host.event_node = "/dev/input/event3";
  const auto plan = build_strict_gamepad_isolation_plan(
    classify_devices({host, gamepad("Sunshine X-Box One (virtual) pad", std::optional<uint16_t> {0x045e}, std::optional<uint16_t> {0x02ea})}),
    {"/dev/input/event9"},
    strict_options()
  );
  const auto command = command_with_headless_gamepad_isolation("steam", plan);

  EXPECT_TRUE(text_contains(command, "--tmpfs /run/udev"));
  EXPECT_TRUE(text_contains(command, "--tmpfs /sys/class/input"));
  EXPECT_TRUE(text_contains(command, "--tmpfs /sys/class/hidraw"));
  EXPECT_TRUE(text_lacks(command, "--bind /run/udev /run/udev"));
  EXPECT_TRUE(text_lacks(command, "--ro-bind /run/udev /run/udev"));
}


TEST(GamepadIsolationTests, StrictWrapperMasksHostSysfsInputDeviceRoots) {
  auto host = gamepad("Microsoft X-Box One pad", std::optional<uint16_t> {0x045e}, std::optional<uint16_t> {0x02ea});
  host.event_node = "/dev/input/event4";
  host.sysfs_path = "/sys/devices/pci0000:00/0000:00:14.0/usb1/1-1/1-1:1.0/input/input41/event4";
  auto virtual_pad = gamepad("Sunshine X-Box One (virtual) pad", std::optional<uint16_t> {0x045e}, std::optional<uint16_t> {0x02ea});
  virtual_pad.event_node = "/dev/input/event99";
  virtual_pad.sysfs_path = "/sys/devices/virtual/input/input99/event99";

  const auto plan = build_strict_gamepad_isolation_plan(
    classify_devices({host, virtual_pad}),
    {"/dev/input/event99", "/dev/input/js99"},
    strict_options()
  );
  const auto command = command_with_headless_gamepad_isolation("steam", plan);

  EXPECT_TRUE(text_contains(command, "--tmpfs /sys/devices/pci0000:00/0000:00:14.0/usb1/1-1/1-1:1.0/input/input41"));
  EXPECT_TRUE(text_contains(command, "--ro-bind-try /sys/devices/virtual/input/input99 /sys/devices/virtual/input/input99"));
  EXPECT_TRUE(text_contains(command, "--ro-bind-try /sys/class/input/event99 /sys/class/input/event99"));
  EXPECT_TRUE(text_contains(command, "--ro-bind-try /sys/class/input/js99 /sys/class/input/js99"));
}

TEST(GamepadIsolationTests, StrictWrapperMasksHostHidBusMetadata) {
  auto host = gamepad("Logitech Gamepad F310", std::optional<uint16_t> {0x046d}, std::optional<uint16_t> {0xc216});
  host.event_node = "/dev/input/event3";
  const auto plan = build_strict_gamepad_isolation_plan(
    classify_devices({host, gamepad("Sunshine X-Box One (virtual) pad", std::optional<uint16_t> {0x045e}, std::optional<uint16_t> {0x02ea})}),
    {"/dev/input/event9"},
    strict_options()
  );
  const auto command = command_with_headless_gamepad_isolation("steam", plan);

  EXPECT_TRUE(text_contains(command, "--tmpfs /sys/bus/hid/devices"));
}

#endif
