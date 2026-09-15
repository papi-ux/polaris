/**
 * @file tests/unit/platform/test_user_unit_override.cpp
 * @brief Test what the polaris user service really runs, and what --setup-host says about it.
 */
#include "../../tests_common.h"

#ifdef __linux__

#include <src/platform/linux/user_unit_override.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;
namespace uu = platf::user_unit;

namespace {
  struct scratch_t {
    fs::path root;

    scratch_t() {
      auto pattern = (fs::temp_directory_path() / "polaris-user-unit-XXXXXX").string();
      const char *made = mkdtemp(pattern.data());
      if (made == nullptr) {
        throw std::runtime_error("mkdtemp failed");
      }
      root = made;
    }

    ~scratch_t() {
      std::error_code ec;
      fs::remove_all(root, ec);
    }

    fs::path file(const std::string &relative, const std::string &content, bool executable = false) const {
      const auto path = root / relative;
      fs::create_directories(path.parent_path());
      {
        std::ofstream out(path, std::ios::binary);
        out << content;
      }
      fs::permissions(
        path,
        executable ? fs::perms::owner_all | fs::perms::group_read | fs::perms::group_exec | fs::perms::others_read | fs::perms::others_exec :
                     fs::perms::owner_read | fs::perms::owner_write | fs::perms::group_read | fs::perms::others_read,
        fs::perm_options::replace
      );
      return path;
    }

    fs::path drop_ins() const {
      return root / ".config/systemd/user/polaris.service.d";
    }
  };
}  // namespace

TEST(UserUnitOverrideTests, NothingWithoutADropInDirectory) {
  scratch_t scratch;
  const auto override = uu::effective_exec_override(scratch.drop_ins());
  EXPECT_FALSE(override.active());
  EXPECT_TRUE(override.drop_in.empty());
  EXPECT_TRUE(override.binary.empty());
  EXPECT_FALSE(override.binary_missing);
}

TEST(UserUnitOverrideTests, TheBazziteDropInPointingAtARemovedCopyIsMissing) {
  // The recipe writes exactly this: a reset, then the copy. Delete the copy and
  // the service execs a path that is not there.
  scratch_t scratch;
  const auto drop_in = scratch.file(
    ".config/systemd/user/polaris.service.d/10-bazzite-kms.conf",
    "[Service]\nExecStart=\nExecStart=" + (scratch.root / "usr/local/bin/polaris-kms").string() + "\n"
  );

  const auto override = uu::effective_exec_override(scratch.drop_ins());
  EXPECT_TRUE(override.active());
  EXPECT_EQ(override.drop_in, drop_in);
  EXPECT_EQ(override.binary, scratch.root / "usr/local/bin/polaris-kms");
  EXPECT_TRUE(override.binary_missing);
}

TEST(UserUnitOverrideTests, AnExistingCopyIsNotMissing) {
  scratch_t scratch;
  const auto copy = scratch.file("usr/local/bin/polaris-kms", "#!/bin/sh\n", true);
  scratch.file(".config/systemd/user/polaris.service.d/10-bazzite-kms.conf", "[Service]\nExecStart=\nExecStart=" + copy.string() + " --flag\n");

  const auto override = uu::effective_exec_override(scratch.drop_ins());
  EXPECT_EQ(override.binary, copy);
  EXPECT_FALSE(override.binary_missing);
}

TEST(UserUnitOverrideTests, AResetAloneLeavesNothingActive) {
  scratch_t scratch;
  scratch.file(".config/systemd/user/polaris.service.d/10-reset.conf", "[Service]\nExecStart=\n");
  const auto override = uu::effective_exec_override(scratch.drop_ins());
  EXPECT_FALSE(override.active());
  EXPECT_TRUE(override.binary.empty());
}

TEST(UserUnitOverrideTests, TheLexicallyLastDropInWins) {
  scratch_t scratch;
  scratch.file(".config/systemd/user/polaris.service.d/10-first.conf", "[Service]\nExecStart=\nExecStart=/nonexistent/first\n");
  scratch.file(".config/systemd/user/polaris.service.d/20-second.conf", "[Service]\nExecStart=\nExecStart=/nonexistent/second\n");
  scratch.file(".config/systemd/user/polaris.service.d/notes.txt", "[Service]\nExecStart=/nonexistent/ignored\n");

  const auto override = uu::effective_exec_override(scratch.drop_ins());
  EXPECT_EQ(override.exec_start, "/nonexistent/second");
  EXPECT_EQ(override.drop_in.filename(), "20-second.conf");
}

TEST(UserUnitOverrideTests, PrefixesAreStrippedAndOtherSectionsIgnored) {
  scratch_t scratch;
  scratch.file(
    ".config/systemd/user/polaris.service.d/10-prefixed.conf",
    "[Unit]\nExecStart=/nonexistent/wrong-section\n[Service]\n  ExecStart = -@/nonexistent/prefixed arg1 arg2\n"
  );
  const auto override = uu::effective_exec_override(scratch.drop_ins());
  EXPECT_EQ(override.binary, fs::path {"/nonexistent/prefixed"});
  EXPECT_TRUE(override.binary_missing);
}

TEST(UserUnitOverrideTests, ARelativeCommandIsNotJudged) {
  // "ExecStart=polaris" resolves through PATH inside systemd; without the
  // absolute path there is nothing this side can honestly check.
  scratch_t scratch;
  scratch.file(".config/systemd/user/polaris.service.d/10-relative.conf", "[Service]\nExecStart=\nExecStart=polaris --verbose\n");
  const auto override = uu::effective_exec_override(scratch.drop_ins());
  EXPECT_TRUE(override.active());
  EXPECT_TRUE(override.binary.empty());
  EXPECT_FALSE(override.binary_missing);
}

TEST(UserUnitOverrideTests, DescribesWhetherTheRunningBinaryIsThePackagedOne) {
  scratch_t scratch;
  const auto packaged = scratch.file("usr/bin/polaris", "#!/bin/sh\n", true);
  const auto copy = scratch.file("usr/local/bin/polaris-kms", "#!/bin/sh\n", true);
  fs::create_symlink(packaged, scratch.root / "usr/bin/polaris-link");

  // A non-packaged build declares no absolute path: nothing to compare.
  auto described = uu::describe_running_binary(copy, "polaris");
  EXPECT_EQ(described.path, copy.string());
  EXPECT_TRUE(described.packaged_path.empty());
  EXPECT_FALSE(described.matches_package.has_value());

  // The package is not installed here.
  described = uu::describe_running_binary(copy, (scratch.root / "usr/bin/absent").string());
  EXPECT_FALSE(described.matches_package.has_value());

  // Running the packaged binary, through its symlink.
  described = uu::describe_running_binary(scratch.root / "usr/bin/polaris-link", packaged.string());
  EXPECT_EQ(described.path, packaged.string());
  EXPECT_EQ(described.matches_package, std::optional<bool> {true});

  // Running a copy.
  described = uu::describe_running_binary(copy, packaged.string());
  EXPECT_EQ(described.matches_package, std::optional<bool> {false});
  EXPECT_EQ(described.packaged_path, packaged.string());
}

TEST(UserUnitOverrideTests, SetupHostAdviceNamesTheMissingCopyAndBothWaysOut) {
  scratch_t scratch;
  const auto packaged = scratch.file("usr/bin/polaris", "#!/bin/sh\n", true);
  const auto drop_in = scratch.file(
    ".config/systemd/user/polaris.service.d/10-bazzite-kms.conf",
    "[Service]\nExecStart=\nExecStart=" + (scratch.root / "usr/local/bin/polaris-kms").string() + "\n"
  );

  const auto advice = uu::setup_host_advice(uu::effective_exec_override(scratch.drop_ins()), "deck", packaged);
  EXPECT_NE(advice.find("[deck]"), std::string::npos);
  EXPECT_NE(advice.find("which does not exist"), std::string::npos);
  EXPECT_NE(advice.find("status=203/EXEC"), std::string::npos);
  EXPECT_NE(advice.find("rm " + drop_in.string()), std::string::npos);
  EXPECT_NE(advice.find("systemctl --user daemon-reload"), std::string::npos);
  EXPECT_NE(advice.find("sudo install -D -m 0755 " + packaged.string() + " " + (scratch.root / "usr/local/bin/polaris-kms").string()), std::string::npos);
  EXPECT_NE(advice.find("setcap cap_sys_admin+ep"), std::string::npos);
}

TEST(UserUnitOverrideTests, SetupHostAdviceWarnsAboutACopyThatUpdatesWillNotTouch) {
  scratch_t scratch;
  const auto packaged = scratch.file("usr/bin/polaris", "#!/bin/sh\n", true);
  const auto copy = scratch.file("usr/local/bin/polaris-kms", "#!/bin/sh\n", true);
  scratch.file(".config/systemd/user/polaris.service.d/10-bazzite-kms.conf", "[Service]\nExecStart=\nExecStart=" + copy.string() + "\n");

  const auto advice = uu::setup_host_advice(uu::effective_exec_override(scratch.drop_ins()), "deck", packaged);
  EXPECT_NE(advice.find("a copy outside the package"), std::string::npos);
  EXPECT_NE(advice.find("Package updates do not change it"), std::string::npos);
  EXPECT_EQ(advice.find("does not exist"), std::string::npos);
}

TEST(UserUnitOverrideTests, SetupHostStaysQuietWhenTheServiceRunsThePackagedBinary) {
  scratch_t scratch;
  const auto packaged = scratch.file("usr/bin/polaris", "#!/bin/sh\n", true);
  fs::create_symlink(packaged, scratch.root / "usr/bin/polaris-link");

  // No override at all.
  EXPECT_TRUE(uu::setup_host_advice(uu::effective_exec_override(scratch.drop_ins()), "deck", packaged).empty());

  // An override that resolves to the packaged binary anyway.
  scratch.file(".config/systemd/user/polaris.service.d/10-same.conf", "[Service]\nExecStart=\nExecStart=" + (scratch.root / "usr/bin/polaris-link").string() + "\n");
  EXPECT_TRUE(uu::setup_host_advice(uu::effective_exec_override(scratch.drop_ins()), "deck", packaged).empty());

  // A relative command is not judged.
  scratch.file(".config/systemd/user/polaris.service.d/20-relative.conf", "[Service]\nExecStart=\nExecStart=polaris\n");
  EXPECT_TRUE(uu::setup_host_advice(uu::effective_exec_override(scratch.drop_ins()), "deck", packaged).empty());
}

#endif
