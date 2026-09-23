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

TEST(UserUnitOverrideTests, TeardownClaimsTheDropInThatPointsAtThePackagedHelper) {
  // --enable-kms points the service at the packaged helper instead of marking the binary it is
  // running, so --disable-kms has to recognise its own drop-in as well as the one the old recipe
  // had people write by hand. A host that keeps either one keeps running the helper.
  scratch_t scratch;
  scratch.file(
    std::string {".config/systemd/user/polaris.service.d/"} + std::string {uu::kms_drop_in_name},
    std::string {"[Service]\nExecStart=\nExecStart="} + std::string {uu::packaged_kms_helper} + "\n"
  );
  const auto override = uu::effective_exec_override(scratch.drop_ins());

  const auto plan = uu::kms_teardown_plan(override, false, false);
  EXPECT_EQ(plan.drop_in.filename(), std::filesystem::path {uu::kms_drop_in_name});
  EXPECT_FALSE(plan.empty());
  // Nothing else is claimed: the packaged helper is the package's to remove, never this command's.
  EXPECT_FALSE(plan.remove_guide_copy);
  EXPECT_FALSE(plan.clear_binary_capability);
}

TEST(UserUnitOverrideTests, TeardownLeavesADropInThatPointsSomewhereElseAlone) {
  // Someone running their own build through a drop-in is not running DRM/KMS capture, and
  // --disable-kms taking their override away would stop their service dead.
  scratch_t scratch;
  const auto mine = scratch.file("home/papi/src/polaris/build/polaris", "a build of my own", true);
  scratch.file(
    std::string {".config/systemd/user/polaris.service.d/"} + std::string {uu::kms_drop_in_name},
    "[Service]\nExecStart=\nExecStart=" + mine.string() + "\n"
  );
  const auto override = uu::effective_exec_override(scratch.drop_ins());

  EXPECT_TRUE(uu::kms_teardown_plan(override, false, false).drop_in.empty());
}

TEST(UserUnitOverrideTests, TheGuideCopyLeftBehindByAPackageUpdateIsStale) {
  // The field report: rpm says 1.4.11, the console says the version the copy
  // was made from, because the drop-in still runs that copy.
  scratch_t scratch;
  const auto packaged = scratch.file("usr/bin/polaris", "polaris 1.4.11", true);
  const auto copy = scratch.file("usr/local/bin/polaris-kms", "polaris 1.4.1", true);
  scratch.file(".config/systemd/user/polaris.service.d/10-bazzite-kms.conf", "[Service]\nExecStart=\nExecStart=" + copy.string() + "\n");
  const auto override = uu::effective_exec_override(scratch.drop_ins());

  EXPECT_EQ(uu::guide_runtime_copy_state(override, packaged, copy), uu::runtime_copy_e::stale);

  // The same length is not the same build.
  scratch.file("usr/local/bin/polaris-kms", "polaris 1.4.10", true);
  EXPECT_EQ(uu::guide_runtime_copy_state(override, packaged, copy), uu::runtime_copy_e::stale);

  // Refreshed, it is current again.
  scratch.file("usr/local/bin/polaris-kms", "polaris 1.4.11", true);
  EXPECT_EQ(uu::guide_runtime_copy_state(override, packaged, copy), uu::runtime_copy_e::current);
}

TEST(UserUnitOverrideTests, ADifferenceInTheLastBlockOfALargeBinaryIsStillStale) {
  scratch_t scratch;
  std::string build(3 * (1 << 16) + 17, 'p');
  const auto packaged = scratch.file("usr/bin/polaris", build, true);
  build.back() = 'q';
  const auto copy = scratch.file("usr/local/bin/polaris-kms", build, true);
  scratch.file(".config/systemd/user/polaris.service.d/10-bazzite-kms.conf", "[Service]\nExecStart=\nExecStart=" + copy.string() + "\n");
  const auto override = uu::effective_exec_override(scratch.drop_ins());

  EXPECT_EQ(uu::guide_runtime_copy_state(override, packaged, copy), uu::runtime_copy_e::stale);
  build.back() = 'p';
  scratch.file("usr/local/bin/polaris-kms", build, true);
  EXPECT_EQ(uu::guide_runtime_copy_state(override, packaged, copy), uu::runtime_copy_e::current);
}

TEST(UserUnitOverrideTests, OnlyTheGuidesOwnPathIsEverACopyToReplace) {
  // The drop-in belongs to the account and --setup-host runs as root, so a
  // path the drop-in names is never a path to write, however stale it is.
  scratch_t scratch;
  const auto packaged = scratch.file("usr/bin/polaris", "polaris 1.4.11", true);
  const auto guide_copy = scratch.file("usr/local/bin/polaris-kms", "polaris 1.4.1", true);
  const auto elsewhere = scratch.file("home/deck/bin/polaris", "polaris 1.4.1", true);
  scratch.file(".config/systemd/user/polaris.service.d/10-custom.conf", "[Service]\nExecStart=\nExecStart=" + elsewhere.string() + "\n");

  EXPECT_EQ(uu::guide_runtime_copy_state(uu::effective_exec_override(scratch.drop_ins()), packaged, guide_copy), uu::runtime_copy_e::none);
  // Nor is the default, which is the real /usr/local/bin/polaris-kms.
  EXPECT_EQ(uu::guide_runtime_copy_state(uu::effective_exec_override(scratch.drop_ins()), packaged), uu::runtime_copy_e::none);
  // No override at all.
  EXPECT_EQ(uu::guide_runtime_copy_state(uu::exec_override_t {}, packaged, guide_copy), uu::runtime_copy_e::none);
}

TEST(UserUnitOverrideTests, AGuidePathThatIsNotAPlainFileIsLeftAlone) {
  scratch_t scratch;
  const auto packaged = scratch.file("usr/bin/polaris", "polaris 1.4.11", true);
  const auto target = scratch.file("opt/other/polaris", "polaris 1.4.1", true);
  const auto guide_copy = scratch.root / "usr/local/bin/polaris-kms";
  fs::create_directories(guide_copy.parent_path());
  fs::create_symlink(target, guide_copy);
  scratch.file(".config/systemd/user/polaris.service.d/10-bazzite-kms.conf", "[Service]\nExecStart=\nExecStart=" + guide_copy.string() + "\n");

  EXPECT_EQ(uu::guide_runtime_copy_state(uu::effective_exec_override(scratch.drop_ins()), packaged, guide_copy), uu::runtime_copy_e::none);

  // A copy removed without its drop-in is the missing-copy advice's case, not a refresh.
  fs::remove(guide_copy);
  EXPECT_EQ(uu::guide_runtime_copy_state(uu::effective_exec_override(scratch.drop_ins()), packaged, guide_copy), uu::runtime_copy_e::none);
}

TEST(UserUnitOverrideTests, SetupHostAdviceForTheGuideCopyNamesTheCommandThatRefreshesIt) {
  scratch_t scratch;
  const auto packaged = scratch.file("usr/bin/polaris", "polaris 1.4.11", true);
  const auto copy = scratch.file("usr/local/bin/polaris-kms", "polaris 1.4.11", true);
  scratch.file(".config/systemd/user/polaris.service.d/10-bazzite-kms.conf", "[Service]\nExecStart=\nExecStart=" + copy.string() + "\n");
  const auto override = uu::effective_exec_override(scratch.drop_ins());

  const auto advice = uu::setup_host_advice(override, "deck", packaged, copy);
  EXPECT_NE(advice.find("Package updates do not change it"), std::string::npos);
  // By name, not by the versioned path the package installs: that path changes with the next update.
  EXPECT_NE(advice.find("sudo -H polaris --setup-host"), std::string::npos);
  EXPECT_EQ(advice.find(packaged.string()), std::string::npos);
  EXPECT_EQ(advice.find("sudo install"), std::string::npos);

  // A build that will not refresh it still hands over the commands.
  const auto by_hand = uu::setup_host_advice(override, "deck", packaged, fs::path {});
  EXPECT_NE(by_hand.find("sudo install -D -m 0755 " + packaged.string() + " " + copy.string()), std::string::npos);
  EXPECT_EQ(by_hand.find("--setup-host"), std::string::npos);
}

TEST(UserUnitOverrideTests, KmsTeardownTakesEveryPieceThatKeepsTheCapability) {
  scratch_t scratch;
  const auto copy = scratch.file("usr/local/bin/polaris-kms", "polaris 1.4.12", true);
  const auto drop_in = scratch.file(".config/systemd/user/polaris.service.d/10-bazzite-kms.conf", "[Service]\nExecStart=\nExecStart=" + copy.string() + "\n");
  const auto override = uu::effective_exec_override(scratch.drop_ins());

  const auto plan = uu::kms_teardown_plan(override, true, true, copy);
  EXPECT_FALSE(plan.empty());
  EXPECT_EQ(plan.drop_in, drop_in);
  EXPECT_TRUE(plan.remove_guide_copy);
  EXPECT_TRUE(plan.clear_binary_capability);
}

TEST(UserUnitOverrideTests, KmsTeardownIsEmptyOnAHostThatNeverEnabledIt) {
  scratch_t scratch;
  const auto copy = scratch.root / "usr/local/bin/polaris-kms";

  const auto plan = uu::kms_teardown_plan(uu::effective_exec_override(scratch.drop_ins()), false, false, copy);
  EXPECT_TRUE(plan.empty());
  EXPECT_TRUE(plan.drop_in.empty());
  EXPECT_FALSE(plan.remove_guide_copy);
  EXPECT_FALSE(plan.clear_binary_capability);
}

TEST(UserUnitOverrideTests, KmsTeardownLeavesSomeoneElsesDropInAlone) {
  scratch_t scratch;
  const auto copy = scratch.file("usr/local/bin/polaris-kms", "polaris 1.4.12", true);
  const auto mine = scratch.file("home/papi/build/polaris", "a build of my own", true);
  scratch.file(".config/systemd/user/polaris.service.d/50-my-build.conf", "[Service]\nExecStart=\nExecStart=" + mine.string() + "\n");
  const auto override = uu::effective_exec_override(scratch.drop_ins());

  // Pointing the service at a build tree is not the KMS recipe. Removing that drop-in would stop
  // the service running what its owner chose, which --disable-kms was never asked to do.
  const auto plan = uu::kms_teardown_plan(override, true, true, copy);
  EXPECT_TRUE(plan.drop_in.empty());
  EXPECT_TRUE(plan.remove_guide_copy);
  EXPECT_TRUE(plan.clear_binary_capability);
}

TEST(UserUnitOverrideTests, KmsTeardownStillClearsTheBinaryWhenOnlyItHoldsTheCapability) {
  scratch_t scratch;
  const auto copy = scratch.root / "usr/local/bin/polaris-kms";

  // The ordinary case: someone ran --enable-kms on a host that never followed the copy recipe.
  const auto plan = uu::kms_teardown_plan(uu::effective_exec_override(scratch.drop_ins()), true, false, copy);
  EXPECT_FALSE(plan.empty());
  EXPECT_TRUE(plan.drop_in.empty());
  EXPECT_FALSE(plan.remove_guide_copy);
  EXPECT_TRUE(plan.clear_binary_capability);
}

#endif
