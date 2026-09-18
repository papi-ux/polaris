/**
 * @file tests/unit/platform/test_private_session_input.cpp
 * @brief Test private labwc session input isolation.
 */
#include "../../tests_common.h"

#ifdef __linux__
  #include <src/platform/linux/private_session_input.h>

  #include <filesystem>
  #include <fstream>
  #include <sstream>
  #include <string>

namespace {
  using platf::private_session_input::input_device_t;

  std::string read_file(const std::filesystem::path &path) {
    std::ifstream input {path};
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
  }

  std::filesystem::path make_temp_dir(std::string_view label) {
    const auto dir = std::filesystem::temp_directory_path() /
                     ("polaris-" + std::string {label} + "-" + std::to_string(::getpid()));
    std::filesystem::remove_all(dir);
    return dir;
  }
}  // namespace

TEST(PrivateSessionInputTests, VirtualDevicesAreRecognizedIncludingTheAbsoluteMouseNode) {
  using platf::private_session_input::is_polaris_virtual_device;

  EXPECT_TRUE(is_polaris_virtual_device("Polaris Mouse passthrough"));
  // inputtino creates a second mouse node with a suffixed name.
  EXPECT_TRUE(is_polaris_virtual_device("Polaris Mouse passthrough (absolute)"));
  EXPECT_TRUE(is_polaris_virtual_device("Polaris Keyboard passthrough"));
  EXPECT_TRUE(is_polaris_virtual_device("Touch passthrough"));
  EXPECT_TRUE(is_polaris_virtual_device("Pen passthrough"));
  EXPECT_TRUE(is_polaris_virtual_device("Sunshine PS5 (virtual) pad Motion Sensors"));

  EXPECT_FALSE(is_polaris_virtual_device("Logitech G502"));
  EXPECT_FALSE(is_polaris_virtual_device("AT Translated Set 2 keyboard"));
}

TEST(PrivateSessionInputTests, HostDevicesAreIgnoredAndVirtualDevicesAreNot) {
  const std::vector<input_device_t> devices {
    {"AT Translated Set 2 keyboard", "/sys/class/input/event0"},
    {"Polaris Keyboard passthrough", "/sys/class/input/event20"},
    {"Logitech G502", "/sys/class/input/event1"},
    {"Polaris Mouse passthrough (absolute)", "/sys/class/input/event21"},
  };

  const auto block = platf::private_session_input::build_libinput_isolation_block(devices);

  EXPECT_NE(std::string::npos, block.find("<device category=\"AT Translated Set 2 keyboard\">"));
  EXPECT_NE(std::string::npos, block.find("<device category=\"Logitech G502\">"));
  EXPECT_NE(std::string::npos, block.find("<sendEventsMode>no</sendEventsMode>"));

  // Ignoring Polaris' own devices would leave the streamed session with no input
  // at all, which is the failure this whole file exists to avoid.
  EXPECT_EQ(std::string::npos, block.find("Polaris Keyboard passthrough"));
  EXPECT_EQ(std::string::npos, block.find("Polaris Mouse passthrough"));
}

TEST(PrivateSessionInputTests, DeviceNamesAreXmlEscaped) {
  const std::vector<input_device_t> devices {
    {"Weird & \"quoted\" <device>", "/sys/class/input/event0"},
  };

  const auto block = platf::private_session_input::build_libinput_isolation_block(devices);

  EXPECT_NE(std::string::npos, block.find("Weird &amp; &quot;quoted&quot; &lt;device&gt;"));
  EXPECT_EQ(std::string::npos, block.find("<device category=\"Weird & \""));
}

TEST(PrivateSessionInputTests, NothingToIgnoreProducesNoLibinputBlock) {
  const std::vector<input_device_t> devices {
    {"Polaris Keyboard passthrough", "/sys/class/input/event20"},
  };

  EXPECT_TRUE(platf::private_session_input::build_libinput_isolation_block(devices).empty());

  const auto rc = platf::private_session_input::build_rc_xml(devices);
  EXPECT_EQ(std::string::npos, rc.find("<libinput>"));
  EXPECT_NE(std::string::npos, rc.find("<labwc_config>"));
  EXPECT_NE(std::string::npos, rc.find("</labwc_config>"));
}

TEST(PrivateSessionInputTests, GeneratedRcXmlIsWrittenAndRefreshed) {
  const auto dir = make_temp_dir("rc-generated");
  std::string status;

  ASSERT_TRUE(platf::private_session_input::ensure_generated_rc_xml(
    dir,
    {{"Logitech G502", "/sys/class/input/event1"}},
    status
  ));
  EXPECT_NE(std::string::npos, read_file(dir / "rc.xml").find("Logitech G502"));
  EXPECT_NE(std::string::npos, status.find("~/.config/labwc-polaris/rc.xml"));
  EXPECT_EQ(std::string::npos, status.find(dir.string()));

  // A later session sees different hardware; the generated file follows it.
  ASSERT_TRUE(platf::private_session_input::ensure_generated_rc_xml(
    dir,
    {{"Keychron K2", "/sys/class/input/event2"}},
    status
  ));
  const auto refreshed = read_file(dir / "rc.xml");
  EXPECT_NE(std::string::npos, refreshed.find("Keychron K2"));
  EXPECT_EQ(std::string::npos, refreshed.find("Logitech G502"));
  EXPECT_FALSE(std::filesystem::exists(dir / "rc.xml.tmp"));

  std::filesystem::remove_all(dir);
}

TEST(PrivateSessionInputTests, UnchangedHardwareLeavesTheGeneratedFileUntouched) {
  const auto dir = make_temp_dir("rc-stable");
  const std::vector<input_device_t> devices {{"Logitech G502", "/sys/class/input/event1"}};

  std::string status;
  ASSERT_TRUE(platf::private_session_input::ensure_generated_rc_xml(dir, devices, status));
  const auto first_write = std::filesystem::last_write_time(dir / "rc.xml");

  ASSERT_TRUE(platf::private_session_input::ensure_generated_rc_xml(dir, devices, status));
  EXPECT_EQ(first_write, std::filesystem::last_write_time(dir / "rc.xml"))
    << "an identical config should not be rewritten";
  EXPECT_NE(std::string::npos, status.find("already current"));

  std::filesystem::remove_all(dir);
}

TEST(PrivateSessionInputTests, UserAuthoredRcXmlIsNeverOverwritten) {
  const auto dir = make_temp_dir("rc-user");
  std::filesystem::create_directories(dir);

  const std::string user_config = "<?xml version=\"1.0\"?>\n<labwc_config><!-- mine --></labwc_config>\n";
  {
    std::ofstream out {dir / "rc.xml"};
    out << user_config;
  }

  std::string status;
  EXPECT_FALSE(platf::private_session_input::ensure_generated_rc_xml(
    dir,
    {{"Logitech G502", "/sys/class/input/event1"}},
    status
  ));
  EXPECT_EQ(user_config, read_file(dir / "rc.xml"));
  EXPECT_NE(std::string::npos, status.find("not generated by Polaris"));
  EXPECT_NE(std::string::npos, status.find("Delete it"))
    << "the message has to say how to hand the file back";
  EXPECT_NE(std::string::npos, status.find("~/.config/labwc-polaris/rc.xml"));
  EXPECT_EQ(std::string::npos, status.find(dir.string()))
    << "log status must not expose the concrete home or temporary root";

  std::filesystem::remove_all(dir);
}

TEST(PrivateSessionInputTests, GeneratedMenuExplainsTheSessionAndKeepsUsefulActions) {
  const auto menu = platf::private_session_input::build_menu_xml();

  EXPECT_NE(std::string::npos, menu.find(platf::private_session_input::generated_marker));
  EXPECT_NE(std::string::npos, menu.find("<menu id=\"root-menu\""));
  // Without these, a user who lands in an empty private session is back to
  // labwc's built-in Terminal + Exit fallback that explains nothing.
  EXPECT_NE(std::string::npos, menu.find("Your stream's own screen"));
  EXPECT_NE(std::string::npos, menu.find("Start games from your client"));
  EXPECT_NE(std::string::npos, menu.find("Desktop? Use Mirror Desktop"));
  EXPECT_NE(std::string::npos, menu.find("or Host Virtual Display"));
  EXPECT_NE(std::string::npos, menu.find("lab-sensible-terminal"));
  EXPECT_NE(std::string::npos, menu.find("<action name=\"Exit\""));

  // labwc cuts a label off at the menu's width. A 1.4.9 user could read none
  // of the notes and took them for broken buttons. About 28 characters fit
  // labwc's default 200 px, so every label stays that short even where the
  // wider menu from themerc-override is ignored.
  std::size_t at = 0;
  while ((at = menu.find("label=\"", at)) != std::string::npos) {
    at += 7;
    const auto label = menu.substr(at, menu.find('"', at) - at);
    EXPECT_LE(label.size(), 28U) << label;
  }
}

TEST(PrivateSessionInputTests, GeneratedThemeGivesTheMenuRoomAndYieldsToTheUsersOwn) {
  const auto theme = platf::private_session_input::build_themerc_override();
  EXPECT_EQ(theme.rfind(platf::private_session_input::generated_shell_marker, 0), 0U);
  EXPECT_NE(std::string::npos, theme.find("\nmenu.width.max: 400\n"));

  const auto dir = make_temp_dir("theme-generated");
  std::string status;
  ASSERT_TRUE(platf::private_session_input::ensure_generated_themerc_override(dir, status));
  EXPECT_EQ(read_file(dir / "themerc-override"), theme);
  EXPECT_NE(std::string::npos, status.find("~/.config/labwc-polaris/themerc-override"));

  // A themerc-override without the marker is the user's own.
  {
    std::ofstream own(dir / "themerc-override", std::ios::trunc);
    own << "menu.width.max: 300\n";
  }
  EXPECT_FALSE(platf::private_session_input::ensure_generated_themerc_override(dir, status));
  EXPECT_EQ(read_file(dir / "themerc-override"), "menu.width.max: 300\n");
  std::filesystem::remove_all(dir);
}

TEST(PrivateSessionInputTests, GeneratedMenuAndAutostartAreWrittenBesideRcXml) {
  const auto dir = make_temp_dir("menu-generated");
  std::string status;

  ASSERT_TRUE(platf::private_session_input::ensure_generated_menu_xml(dir, status));
  EXPECT_NE(std::string::npos, read_file(dir / "menu.xml").find("root-menu"));
  EXPECT_NE(std::string::npos, status.find("~/.config/labwc-polaris/menu.xml"));
  EXPECT_EQ(std::string::npos, status.find(dir.string()));
  ASSERT_TRUE(platf::private_session_input::ensure_generated_menu_xml(dir, status));
  EXPECT_NE(std::string::npos, status.find("already current"));

  ASSERT_TRUE(platf::private_session_input::ensure_generated_autostart(dir, status));
  EXPECT_NE(std::string::npos, status.find("~/.config/labwc-polaris/autostart"));
  EXPECT_EQ(std::string::npos, status.find(dir.string()));
  const auto autostart = read_file(dir / "autostart");
  EXPECT_NE(std::string::npos, autostart.find(platf::private_session_input::generated_shell_marker));
  // swaybg is optional; the script must not fail when it is absent.
  EXPECT_NE(std::string::npos, autostart.find("command -v swaybg"));
  EXPECT_FALSE(std::filesystem::exists(dir / "menu.xml.tmp"));
  EXPECT_FALSE(std::filesystem::exists(dir / "autostart.tmp"));

  std::filesystem::remove_all(dir);
}

TEST(PrivateSessionInputTests, UserAuthoredMenuAndAutostartAreNeverOverwritten) {
  const auto dir = make_temp_dir("menu-user");
  std::filesystem::create_directories(dir);

  const std::string user_menu = "<?xml version=\"1.0\"?>\n<openbox_menu><!-- mine --></openbox_menu>\n";
  const std::string user_autostart = "#!/bin/sh\nexec my-own-setup\n";
  {
    std::ofstream menu_out {dir / "menu.xml"};
    menu_out << user_menu;
    std::ofstream autostart_out {dir / "autostart"};
    autostart_out << user_autostart;
  }

  std::string status;
  EXPECT_FALSE(platf::private_session_input::ensure_generated_menu_xml(dir, status));
  EXPECT_EQ(user_menu, read_file(dir / "menu.xml"));
  EXPECT_NE(std::string::npos, status.find("not generated by Polaris"));
  EXPECT_NE(std::string::npos, status.find("~/.config/labwc-polaris/menu.xml"));
  EXPECT_EQ(std::string::npos, status.find(dir.string()));

  EXPECT_FALSE(platf::private_session_input::ensure_generated_autostart(dir, status));
  EXPECT_EQ(user_autostart, read_file(dir / "autostart"));
  EXPECT_NE(std::string::npos, status.find("not generated by Polaris"));
  EXPECT_NE(std::string::npos, status.find("~/.config/labwc-polaris/autostart"));
  EXPECT_EQ(std::string::npos, status.find(dir.string()));

  std::filesystem::remove_all(dir);
}
#endif
