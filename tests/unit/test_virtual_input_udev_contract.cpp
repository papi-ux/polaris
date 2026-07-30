#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <gtest/gtest.h>

#include "src/platform/linux/input/inputtino_gamepad_isolation.h"

namespace {
  std::string read_source_file(std::string_view relative_path) {
    const auto path = std::filesystem::path {POLARIS_SOURCE_DIR} / relative_path;
    std::ifstream input {path};
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
  }

  std::string rule_for_name(const std::string &rules, std::string_view device_name) {
    std::istringstream lines {rules};
    std::string line;
    while (std::getline(lines, line)) {
      const auto first = line.find_first_not_of(" \t");
      if (first == std::string::npos || line[first] == '#') {
        continue;
      }
      if (line.find(device_name) != std::string::npos) {
        return line;
      }
    }
    return {};
  }

  std::vector<std::string> rules_for_name(const std::string &rules, std::string_view device_name) {
    std::vector<std::string> matches;
    std::istringstream lines(rules);
    std::string line;
    while (std::getline(lines, line)) {
      const auto first = line.find_first_not_of(" \t");
      if (first == std::string::npos || line[first] == '#') {
        continue;
      }
      if (line.find(device_name) != std::string::npos) {
        matches.push_back(line);
      }
    }
    return matches;
  }

  bool contains(std::string_view text, std::string_view needle) {
    return text.find(needle) != std::string_view::npos;
  }
}  // namespace

TEST(VirtualInputUdevContract, CreatorDevicesRetainInteractiveUserAccess) {
  const auto rules = read_source_file("src_assets/linux/misc/60-polaris.rules");
  ASSERT_FALSE(rules.empty());

  const auto uinput = rule_for_name(rules, "uinput");
  const auto uhid = rule_for_name(rules, "uhid");

  ASSERT_FALSE(uinput.empty());
  ASSERT_FALSE(uhid.empty());
  EXPECT_TRUE(contains(uinput, "TAG+=\"uaccess\""));
  EXPECT_TRUE(contains(uhid, "TAG+=\"uaccess\""));
}

TEST(VirtualInputUdevContract, LegacyNamesRetainActiveSeatAccess) {
  const auto rules = read_source_file("src_assets/linux/misc/60-polaris.rules");
  ASSERT_FALSE(rules.empty());

  for (const auto name : {"Sunshine X-Box One (virtual) pad", "Sunshine Nintendo (virtual) pad", "Sunshine PS5 (virtual) pad"}) {
    SCOPED_TRACE(name);
    const auto rule = rule_for_name(rules, name);
    ASSERT_FALSE(rule.empty());
    EXPECT_TRUE(contains(rule, "TAG+=\"uaccess\""));
    EXPECT_FALSE(contains(rule, "seat-polaris"));
  }
}

TEST(VirtualInputUdevContract, IsolatedNamesUseDedicatedSeat) {
  const auto rules = read_source_file("src_assets/linux/misc/60-polaris.rules");
  const auto creator = read_source_file("src/platform/linux/input/inputtino_gamepad.cpp");
  ASSERT_FALSE(rules.empty());
  ASSERT_FALSE(creator.empty());

  for (const auto name : {"Polaris X-Box One (virtual) pad", "Polaris Nintendo (virtual) pad", "Polaris PS5 (virtual) pad"}) {
    SCOPED_TRACE(name);
    const auto rule = rule_for_name(rules, name);
    ASSERT_FALSE(rule.empty());
    EXPECT_TRUE(contains(rule, "ENV{ID_SEAT}=\"seat-polaris\""));
    EXPECT_FALSE(contains(rule, "TAG+=\"uaccess\""));
    EXPECT_TRUE(contains(creator, name));
  }
}

TEST(VirtualInputUdevContract, SeatIsolationIdentityHelpersPreserveEnabledAndDisabledBehavior) {
  using platf::gamepad::isolation::client_gamepad_device_name;
  using platf::gamepad::isolation::client_gamepad_identity;
  using platf::gamepad::isolation::client_gamepad_phys_marker;

  EXPECT_EQ("polaris/client-gamepad-seat-isolated/0", client_gamepad_phys_marker(true, 0));
  EXPECT_EQ("polaris/client-gamepad-seat-isolated/3", client_gamepad_phys_marker(true, 3));
  EXPECT_TRUE(client_gamepad_phys_marker(false, 0).empty());
  EXPECT_EQ("Polaris pad", client_gamepad_device_name(true, "Sunshine pad", "Polaris pad"));
  EXPECT_EQ("Sunshine pad", client_gamepad_device_name(false, "Sunshine pad", "Polaris pad"));

  const auto isolated = client_gamepad_identity(true, 3, "Sunshine pad", "Polaris pad", "02:00:00:00:10:03");
  EXPECT_EQ("Polaris pad", isolated.name);
  EXPECT_EQ("polaris/client-gamepad-seat-isolated/3", isolated.phys);

  const auto legacy = client_gamepad_identity(false, 3, "Sunshine pad", "Polaris pad", "02:00:00:00:10:03");
  EXPECT_EQ("Sunshine pad", legacy.name);
  EXPECT_EQ("02:00:00:00:10:03", legacy.phys);

  const auto randomized_ds5 = client_gamepad_identity(false, 3, "Sunshine PS5", "Polaris PS5", "");
  EXPECT_EQ("Sunshine PS5", randomized_ds5.name);
  EXPECT_TRUE(randomized_ds5.phys.empty());
}

TEST(VirtualInputUdevContract, DocumentationStatesTheEnforceableBoundary) {
  const auto docs = read_source_file("docs/configuration.md");
  ASSERT_FALSE(docs.empty());

  EXPECT_TRUE(contains(docs, "controls the **opposite** direction"));
  EXPECT_TRUE(contains(docs, "This is a Unix-user boundary, not a same-account process sandbox."));
  EXPECT_TRUE(contains(docs, "members of `input`"));
  EXPECT_TRUE(contains(docs, "requires a privileged broker"));
}

TEST(VirtualInputUdevContract, HostSetupExplainsThatExistingNodesMustBeRecreated) {
  const auto docs = read_source_file("docs/configuration.md");
  const auto setup = read_source_file("src/entry_handler.cpp");

  EXPECT_TRUE(contains(docs, "Existing virtual controller nodes keep their previous access policy"));
  EXPECT_TRUE(contains(setup, "Existing virtual gamepad nodes keep their previous access policy"));
}

TEST(VirtualInputUdevContract, SeatIsolationIsAnExplicitLinuxInputOption) {
  const std::vector<std::string_view> files {
    "src/config.h",
    "src/config.cpp",
    "src/confighttp_validation.cpp",
    "src/platform/linux/input/inputtino_gamepad.cpp",
    "src_assets/common/assets/web/views/ConfigView.vue",
    "src_assets/common/assets/web/configs/tabs/Inputs.vue",
    "src_assets/common/assets/web/public/assets/locale/en.json",
    "docs/configuration.md",
  };

  for (const auto file : files) {
    SCOPED_TRACE(file);
    const auto source = read_source_file(file);
    ASSERT_FALSE(source.empty());
    EXPECT_TRUE(contains(source, "client_gamepad_seat_isolation"));
  }
}

TEST(VirtualInputUdevContract, SeatIsolationIsOptInForBackwardCompatibility) {
  const auto native_config = read_source_file("src/config.cpp");
  const auto web_config = read_source_file("src_assets/common/assets/web/views/ConfigView.vue");
  const auto inputs_tab = read_source_file("src_assets/common/assets/web/configs/tabs/Inputs.vue");

  EXPECT_TRUE(contains(native_config, "false,  // client_gamepad_seat_isolation"));
  EXPECT_TRUE(contains(web_config, "\"client_gamepad_seat_isolation\": \"disabled\""));
  EXPECT_TRUE(contains(inputs_tab, "v-model=\"config.client_gamepad_seat_isolation\""));
  EXPECT_TRUE(contains(inputs_tab, "default=\"false\""));
}

TEST(VirtualInputUdevContract, IsolatedGamepadsUseDedicatedSeatWithoutLocalSeatAcl) {
  const auto rules = read_source_file("src_assets/linux/misc/60-polaris.rules");
  ASSERT_FALSE(rules.empty());

  const auto marker_rules = rules_for_name(rules, "polaris/client-gamepad-seat-isolated");
  ASSERT_EQ(2U, marker_rules.size());
  EXPECT_TRUE(contains(marker_rules[0], "KERNEL==\"hidraw*\""));
  EXPECT_TRUE(contains(marker_rules[1], "SUBSYSTEMS==\"input\""));

  for (const auto &rule : marker_rules) {
    EXPECT_TRUE(contains(rule, "ATTRS{phys}==\"polaris/client-gamepad-seat-isolated/*\""));
    EXPECT_TRUE(contains(rule, "ENV{ID_SEAT}=\"seat-polaris\""));
    EXPECT_TRUE(contains(rule, "GROUP=\"input\""));
    EXPECT_TRUE(contains(rule, "MODE=\"0660\""));
    EXPECT_FALSE(contains(rule, "TAG+=\"uaccess\""));
  }
}
