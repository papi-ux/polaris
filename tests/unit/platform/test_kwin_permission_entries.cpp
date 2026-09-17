/**
 * @file tests/unit/platform/test_kwin_permission_entries.cpp
 * @brief Test the KWin screencast permission entry names and the cleanup plan.
 */
#include "../../tests_common.h"

#include <src/platform/linux/kwin_permission_entries.h>

#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace permission = kwingrab::permission;

TEST(KwinPermissionEntries, NameIsStablePerBinaryAndDiffersBetweenBinaries) {
  const auto first = permission::desktop_file_name("/usr/bin/polaris-1.4.8");
  EXPECT_EQ(first, permission::desktop_file_name("/usr/bin/polaris-1.4.8"));
  EXPECT_NE(first, permission::desktop_file_name("/usr/bin/polaris-1.4.9"));
  EXPECT_EQ(first.rfind(std::string {permission::desktop_prefix} + ".", 0), 0u);
  EXPECT_TRUE(first.ends_with(".desktop"));
  EXPECT_EQ(first.size(), permission::desktop_prefix.size() + 1 + 16 + 8);
}

TEST(KwinPermissionEntries, EntriesForBinariesThatAreGoneAreDeleted) {
  const std::set<std::string> present {"/usr/bin/polaris-1.4.9", "/opt/dev/polaris/build/polaris"};
  const auto exists = [&](const std::string &binary) {
    return present.contains(binary);
  };
  const std::vector<permission::entry_t> entries {
    {"/apps/dev.polaris-stream.app.Polaris.kwin.1786404429246439102.desktop", "/usr/bin/polaris-1.4.5", true},
    {"/apps/dev.polaris-stream.app.Polaris.kwin.1787867848727005434.desktop", "/usr/bin/polaris-1.4.8", true},
    {"/apps/dev.polaris-stream.app.Polaris.kwin.1788028625858250268.desktop", "/opt/dev/polaris/build/polaris", true},
    {"/apps/dev.polaris-stream.app.Polaris.kwin.1789096076530426594.desktop", "", true},
  };
  const auto plan = permission::plan_entries(entries, "/usr/bin/polaris-1.4.9", exists);
  EXPECT_FALSE(plan.keep.has_value());
  EXPECT_EQ(plan.remove, (std::vector<std::filesystem::path> {entries[0].path, entries[1].path, entries[3].path}));
}

TEST(KwinPermissionEntries, TheRunningBinaryKeepsOneEntryPreferringTheStableName) {
  const std::string exe {"/usr/bin/polaris-1.4.9"};
  const auto stable = std::filesystem::path {"/apps"} / permission::desktop_file_name(exe);
  const std::vector<permission::entry_t> entries {
    {"/apps/dev.polaris-stream.app.Polaris.kwin.1789096076530426594.desktop", exe, true},
    {stable, exe, true},
  };
  const auto plan = permission::plan_entries(entries, exe, [](const std::string &) {
    return true;
  });
  ASSERT_TRUE(plan.keep.has_value());
  EXPECT_EQ(*plan.keep, stable);
  EXPECT_EQ(plan.remove, (std::vector<std::filesystem::path> {entries[0].path}));
}

TEST(KwinPermissionEntries, AnEntryPolarisDidNotWriteIsNeverDeleted) {
  const std::vector<permission::entry_t> entries {
    {"/apps/dev.polaris-stream.app.Polaris.kwin.desktop", "/usr/bin/polaris --verbose", false},
    {"/apps/dev.polaris-stream.app.Polaris.kwin.custom.desktop", "/usr/bin/polaris-1.4.9", false},
    {"/apps/dev.polaris-stream.app.Polaris.kwin.gone.desktop", "/usr/bin/polaris-1.4.5", false},
  };
  const auto plan = permission::plan_entries(entries, "/usr/bin/polaris-1.4.9", [](const std::string &binary) {
    return binary == "/usr/bin/polaris-1.4.9";
  });
  ASSERT_TRUE(plan.keep.has_value());
  EXPECT_EQ(*plan.keep, entries[1].path);
  EXPECT_TRUE(plan.remove.empty());
}
