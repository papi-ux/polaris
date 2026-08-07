/**
 * @file tests/unit/test_launcher_identity.cpp
 * @brief Test the Lutris runner → platform/runtime mapping the library serves.
 */
#include "../tests_common.h"

#include <src/process.h>

TEST(LauncherIdentityTests, RunnersThatDetermineTheAnswerAreMapped) {
  using proc::launcher_identity_from_lutris_runner;

  const auto wine = launcher_identity_from_lutris_runner("wine");
  EXPECT_EQ("windows", wine.platform);
  EXPECT_EQ("wine", wine.runtime);

  const auto proton = launcher_identity_from_lutris_runner("proton");
  EXPECT_EQ("windows", proton.platform);
  EXPECT_EQ("proton", proton.runtime);

  const auto umu = launcher_identity_from_lutris_runner("umu");
  EXPECT_EQ("windows", umu.platform);
  EXPECT_EQ("umu", umu.runtime);

  const auto native = launcher_identity_from_lutris_runner("linux");
  EXPECT_EQ("linux", native.platform);
  EXPECT_EQ("native", native.runtime);
}

TEST(LauncherIdentityTests, TheSteamRunnerNamesARuntimeButNoPlatform) {
  // Lutris' steam runner launches native and Proton titles alike, so claiming
  // a platform would be a guess.
  const auto steam = proc::launcher_identity_from_lutris_runner("steam");
  EXPECT_TRUE(steam.platform.empty());
  EXPECT_EQ("steam", steam.runtime);
}

TEST(LauncherIdentityTests, UnknownStaysEmptyRatherThanGuessed) {
  // Nova renders nothing for an empty value; a wrong badge in the library is
  // worse than a missing one.
  for (const auto runner : {"", "flatpak", "mame", "dosbox", "something-new"}) {
    SCOPED_TRACE(runner);
    const auto identity = proc::launcher_identity_from_lutris_runner(runner);
    EXPECT_TRUE(identity.platform.empty());
    EXPECT_TRUE(identity.runtime.empty());
  }
}

TEST(LauncherIdentityTests, InputIsNormalizedBeforeMapping) {
  const auto identity = proc::launcher_identity_from_lutris_runner("  Wine \n");
  EXPECT_EQ("windows", identity.platform);
  EXPECT_EQ("wine", identity.runtime);
}
