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

  std::filesystem::remove_all(dir);
}
#endif
