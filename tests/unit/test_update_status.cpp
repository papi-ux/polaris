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
