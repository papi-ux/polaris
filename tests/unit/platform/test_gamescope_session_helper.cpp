/**
 * @file tests/unit/platform/test_gamescope_session_helper.cpp
 * @brief Test polaris-gamescope-session launcher resolution and bundle coherence checks.
 */
#include "../../tests_common.h"

#ifdef __linux__

#include <src/platform/linux/gamescope_session_helper.h>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>

using namespace std::literals;
namespace fs = std::filesystem;
namespace helper = platf::gamescope_session_helper;

namespace {
  struct scratch_t {
    fs::path root;

    scratch_t() {
      auto pattern = (fs::temp_directory_path() / "polaris-gamescope-helper-XXXXXX").string();
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

    fs::path file(const std::string &relative, const std::string &content, bool executable) const {
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
  };

  const std::string module_body = "#!/bin/bash\n# shared body\nset -euo pipefail\necho session\n"s;
  const std::string library_body = "polaris_validate_marker() { :; }\n"s;
}  // namespace

TEST(GamescopeSessionHelperTests, PrefersLauncherBesideBinaryOverPath) {
  scratch_t scratch;
  const auto beside = scratch.file("usr/bin/polaris-gamescope-session", module_body, true);
  const auto stale = scratch.file("home/.local/bin/polaris-gamescope-session", "old\n", true);

  const auto resolution = helper::resolve(beside.parent_path(), stale, {}, {});

  EXPECT_EQ(resolution.helper, beside);
  EXPECT_EQ(resolution.shadowed, stale);
  EXPECT_EQ(resolution.session_match, helper::bundle_match_t::unknown);
}

TEST(GamescopeSessionHelperTests, FallsBackToPathWhenNothingIsBeside) {
  scratch_t scratch;
  fs::create_directories(scratch.root / "usr/bin");
  const auto on_path = scratch.file("usr/local/bin/polaris-gamescope-session", module_body, true);

  const auto resolution = helper::resolve(scratch.root / "usr/bin", on_path, {}, {});

  EXPECT_EQ(resolution.helper, on_path);
  EXPECT_TRUE(resolution.shadowed.empty());
}

TEST(GamescopeSessionHelperTests, IgnoresNonExecutableSibling) {
  scratch_t scratch;
  const auto beside = scratch.file("usr/bin/polaris-gamescope-session", module_body, false);
  const auto on_path = scratch.file("usr/local/bin/polaris-gamescope-session", module_body, true);

  const auto resolution = helper::resolve(beside.parent_path(), on_path, {}, {});

  EXPECT_EQ(resolution.helper, on_path);
  EXPECT_TRUE(resolution.shadowed.empty());
}

TEST(GamescopeSessionHelperTests, ReportsNothingWhenNoLauncherExists) {
  scratch_t scratch;
  fs::create_directories(scratch.root / "usr/bin");

  const auto resolution = helper::resolve(scratch.root / "usr/bin", {}, {}, {});

  EXPECT_TRUE(resolution.helper.empty());
  EXPECT_TRUE(resolution.shadowed.empty());
  EXPECT_TRUE(helper::advisories(resolution).empty());
  EXPECT_NE(helper::summary(resolution).find("no polaris-gamescope-session launcher"), std::string::npos);
}

TEST(GamescopeSessionHelperTests, UnknownBinaryDirectoryUsesPath) {
  scratch_t scratch;
  const auto on_path = scratch.file("usr/local/bin/polaris-gamescope-session", module_body, true);

  const auto resolution = helper::resolve(std::nullopt, on_path, {}, {});

  EXPECT_EQ(resolution.helper, on_path);
}

TEST(GamescopeSessionHelperTests, PathHitOnTheSameFileIsNotShadowing) {
  scratch_t scratch;
  const auto beside = scratch.file("usr/bin/polaris-gamescope-session", module_body, true);
  fs::create_directories(scratch.root / "alias");
  const auto alias = scratch.root / "alias/polaris-gamescope-session";
  fs::create_symlink(beside, alias);

  const auto resolution = helper::resolve(beside.parent_path(), alias, {}, {});

  EXPECT_EQ(resolution.helper, beside);
  EXPECT_TRUE(resolution.shadowed.empty());
  EXPECT_TRUE(helper::advisories(resolution).empty());
}

TEST(GamescopeSessionHelperTests, PackagedCopyMatchesBundleExactly) {
  scratch_t scratch;
  const auto beside = scratch.file("usr/bin/polaris-gamescope-session", module_body, true);
  const auto lib = scratch.file("usr/bin/polaris-gamescope-runtime-lib.sh", library_body, false);
  const auto bundled = scratch.file("assets/gamescope/polaris-gamescope-session.sh", module_body, false);
  const auto bundled_lib = scratch.file("assets/gamescope/polaris-gamescope-runtime-lib.sh", library_body, false);

  const auto resolution = helper::resolve(beside.parent_path(), {}, bundled, bundled_lib);

  EXPECT_EQ(resolution.bundled, bundled);
  EXPECT_EQ(resolution.session_match, helper::bundle_match_t::exact);
  EXPECT_EQ(resolution.runtime_lib, lib);
  EXPECT_EQ(resolution.runtime_lib_match, helper::bundle_match_t::exact);
  EXPECT_TRUE(helper::advisories(resolution).empty());
  EXPECT_NE(helper::summary(resolution).find("identical to the module this build ships"), std::string::npos);
}

TEST(GamescopeSessionHelperTests, WrapperEmbeddingTheBundledModuleIsCurrent) {
  scratch_t scratch;
  const auto wrapper = "#!/usr/bin/env bash\nexport POLARIS_GAMESCOPE_BIN=gamescope\n"s + library_body + module_body;
  const auto on_path = scratch.file("nix/bin/polaris-gamescope-session", wrapper, true);
  const auto bundled = scratch.file("assets/gamescope/polaris-gamescope-session.sh", module_body, false);
  const auto bundled_lib = scratch.file("assets/gamescope/polaris-gamescope-runtime-lib.sh", library_body, false);

  const auto resolution = helper::resolve(std::nullopt, on_path, bundled, bundled_lib);

  EXPECT_EQ(resolution.session_match, helper::bundle_match_t::wrapped);
  // Nix inlines the library, so no sibling exists and nothing is reported.
  EXPECT_TRUE(resolution.runtime_lib.empty());
  EXPECT_EQ(resolution.runtime_lib_match, helper::bundle_match_t::unknown);
  EXPECT_TRUE(helper::advisories(resolution).empty());
}

TEST(GamescopeSessionHelperTests, LauncherFromAnotherCheckoutIsFlagged) {
  scratch_t scratch;
  const auto stale = scratch.file("usr/local/bin/polaris-gamescope-session", "#!/usr/bin/env bash\n# header\nold body\n", true);
  const auto stale_lib = scratch.file("usr/local/bin/polaris-gamescope-runtime-lib.sh", "old library\n", false);
  const auto bundled = scratch.file("assets/gamescope/polaris-gamescope-session.sh", module_body, false);
  const auto bundled_lib = scratch.file("assets/gamescope/polaris-gamescope-runtime-lib.sh", library_body, false);
  fs::create_directories(scratch.root / "usr/bin");

  const auto resolution = helper::resolve(scratch.root / "usr/bin", stale, bundled, bundled_lib);

  EXPECT_EQ(resolution.helper, stale);
  EXPECT_EQ(resolution.session_match, helper::bundle_match_t::mismatch);
  EXPECT_EQ(resolution.runtime_lib, stale_lib);
  EXPECT_EQ(resolution.runtime_lib_match, helper::bundle_match_t::mismatch);

  const auto advisories = helper::advisories(resolution);
  ASSERT_EQ(advisories.size(), 2u);
  EXPECT_NE(advisories[0].find(stale.string()), std::string::npos);
  EXPECT_NE(advisories[0].find(bundled.string()), std::string::npos);
  EXPECT_NE(advisories[0].find("different checkout"), std::string::npos);
  EXPECT_NE(advisories[1].find(stale_lib.string()), std::string::npos);
}

TEST(GamescopeSessionHelperTests, ShadowedStaleCopyIsNamedButNotUsed) {
  scratch_t scratch;
  const auto beside = scratch.file("usr/bin/polaris-gamescope-session", module_body, true);
  const auto stale = scratch.file("home/.local/bin/polaris-gamescope-session", "old\n", true);
  const auto bundled = scratch.file("assets/gamescope/polaris-gamescope-session.sh", module_body, false);

  const auto resolution = helper::resolve(beside.parent_path(), stale, bundled, {});

  EXPECT_EQ(resolution.session_match, helper::bundle_match_t::exact);
  const auto advisories = helper::advisories(resolution);
  ASSERT_EQ(advisories.size(), 1u);
  EXPECT_NE(advisories[0].find(beside.string()), std::string::npos);
  EXPECT_NE(advisories[0].find(stale.string()), std::string::npos);
  EXPECT_NE(advisories[0].find("scripts/install"), std::string::npos);
}

TEST(GamescopeSessionHelperTests, MatchKeysAreStableWords) {
  // The Doctor probe publishes these words; the web report switches on them.
  EXPECT_EQ(helper::match_key(helper::bundle_match_t::exact), "exact");
  EXPECT_EQ(helper::match_key(helper::bundle_match_t::wrapped), "wrapped");
  EXPECT_EQ(helper::match_key(helper::bundle_match_t::mismatch), "mismatch");
  EXPECT_EQ(helper::match_key(helper::bundle_match_t::unknown), "unknown");
}

TEST(GamescopeSessionHelperTests, CompareWithBundleClassifiesContent) {
  EXPECT_EQ(helper::compare_with_bundle("", module_body), helper::bundle_match_t::unknown);
  EXPECT_EQ(helper::compare_with_bundle(module_body, ""), helper::bundle_match_t::unknown);
  EXPECT_EQ(helper::compare_with_bundle(module_body, module_body), helper::bundle_match_t::exact);
  EXPECT_EQ(helper::compare_with_bundle("prefix\n" + module_body, module_body), helper::bundle_match_t::wrapped);
  EXPECT_EQ(helper::compare_with_bundle(module_body.substr(0, module_body.size() - 1), module_body), helper::bundle_match_t::mismatch);
}

#endif  // __linux__
