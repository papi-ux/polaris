#include <src/update_status.h>

#include <gtest/gtest.h>

TEST(UpdateStatusTests, ParsesQuotedOsReleaseFields) {
  const auto distro = update_status::parse_os_release_for_tests(
    "NAME=\"CachyOS Linux\"\n"
    "ID=cachyos\n"
    "ID_LIKE=\"arch\"\n"
    "VERSION_ID=2026\n"
  );

  EXPECT_EQ(distro.id, "cachyos");
  EXPECT_EQ(distro.id_like, "arch");
  EXPECT_EQ(distro.version_id, "2026");
  EXPECT_EQ(distro.pretty_name, "CachyOS Linux");
}

TEST(UpdateStatusTests, RecommendsArchAssetForCachyOsAndArchLikeDistros) {
  const auto distro = update_status::parse_os_release_for_tests(
    "ID=cachyos\n"
    "ID_LIKE=\"arch\"\n"
  );

  EXPECT_EQ(update_status::recommended_release_asset_for_tests(distro), "Polaris-arch-x86_64.pkg.tar.zst");
  EXPECT_EQ(update_status::package_family_for_tests(distro), "arch");
}

TEST(UpdateStatusTests, RecommendsFedoraAssetByVersionForBazzite) {
  const auto distro = update_status::parse_os_release_for_tests(
    "ID=bazzite\n"
    "ID_LIKE=\"fedora\"\n"
    "VERSION_ID=44\n"
  );

  EXPECT_EQ(update_status::recommended_release_asset_for_tests(distro), "Polaris-fedora44-x86_64.rpm");
  EXPECT_EQ(update_status::package_family_for_tests(distro), "fedora");
}

TEST(UpdateStatusTests, RecommendsUbuntu2404DebAsset) {
  const auto distro = update_status::parse_os_release_for_tests(
    "ID=ubuntu\n"
    "ID_LIKE=\"debian\"\n"
    "VERSION_ID=\"24.04\"\n"
  );

  EXPECT_EQ(update_status::recommended_release_asset_for_tests(distro), "Polaris-ubuntu24.04-x86_64.deb");
  EXPECT_EQ(update_status::package_family_for_tests(distro), "ubuntu");
}

TEST(UpdateStatusTests, RepositorySectionWithNoEnabledKeyCountsAsEnabled) {
  // dnf and pacman both treat a section with no `enabled` key as enabled, so
  // absence has to mean yes. Defaulting to no would tell every correctly
  // configured host that it has no repository.
  EXPECT_TRUE(update_status::repository_section_enabled_for_tests(
    "[polaris]\nname=Polaris\nbaseurl=https://example.invalid/fedora/$basearch\n"
  ));
}

TEST(UpdateStatusTests, RepositorySectionHonoursExplicitEnabledValues) {
  EXPECT_TRUE(update_status::repository_section_enabled_for_tests("[polaris]\nenabled=1\n"));
  EXPECT_TRUE(update_status::repository_section_enabled_for_tests("[polaris]\nenabled=true\n"));
  EXPECT_FALSE(update_status::repository_section_enabled_for_tests("[polaris]\nenabled=0\n"));
}

TEST(UpdateStatusTests, RepositorySectionIgnoresOtherSections) {
  // The disabled repository here is somebody else's. Reading `enabled` without
  // tracking which section it belongs to would report Polaris as disabled.
  EXPECT_TRUE(update_status::repository_section_enabled_for_tests(
    "[polaris]\nenabled=1\n\n[other]\nenabled=0\n"
  ));
  EXPECT_FALSE(update_status::repository_section_enabled_for_tests(
    "[other]\nenabled=1\n"
  ));
}

TEST(UpdateStatusTests, RepositorySectionIgnoresCommentedConfiguration) {
  // A commented-out repository is exactly the host that cannot upgrade from
  // one, so it must not be read as configured.
  EXPECT_FALSE(update_status::repository_section_enabled_for_tests(
    "#[polaris]\n#enabled=1\n"
  ));
}

TEST(UpdateStatusTests, RepositoryUpgradeCommandFollowsTheHostShape) {
  const auto fedora = update_status::parse_os_release_for_tests("ID=fedora\nVERSION_ID=44\n");
  const auto arch = update_status::parse_os_release_for_tests("ID=arch\n");
  const auto ubuntu = update_status::parse_os_release_for_tests("ID=ubuntu\nVERSION_ID=\"24.04\"\n");

  EXPECT_EQ(update_status::repository_upgrade_command_for_tests(fedora, false), "sudo dnf upgrade polaris");
  EXPECT_EQ(update_status::repository_upgrade_command_for_tests(arch, false), "sudo pacman -Syu polaris");

  // On an ostree host dnf does not change the system. Serving `dnf upgrade`
  // there is the same class of unusable advice as the `usermod -aG input` that
  // v1.3.6 had to correct.
  EXPECT_EQ(update_status::repository_upgrade_command_for_tests(fedora, true), "rpm-ostree upgrade");

  // No repository is served for Ubuntu, so there is no command to offer.
  EXPECT_EQ(update_status::repository_upgrade_command_for_tests(ubuntu, false), "");
}
