#include "../tests_common.h"

#include <src/beat_times.h>

namespace {
  constexpr const char *kDataset = R"json({
    "version": 1,
    "generated_at": 1754470000,
    "games": [
      {
        "name": "Control Ultimate Edition",
        "steam_appid": "870780",
        "main_seconds": 41400,
        "extras_seconds": 68400,
        "completionist_seconds": 104400,
        "url": "https://example.invalid/control"
      },
      {
        "name": "Slay the Spire II",
        "main_seconds": 90000
      },
      {
        "name": "Nothing Known",
        "steam_appid": "1"
      }
    ]
  })json";
}  // namespace

TEST(BeatTimesTests, ParsesEstimatesAndIndexesThemBothWays) {
  const auto data = beat_times::parse(kDataset);

  EXPECT_EQ(data.generated_at, 1754470000);
  ASSERT_EQ(data.by_steam_appid.count("870780"), 1u);

  const auto &control = data.by_steam_appid.at("870780");
  EXPECT_EQ(control.main_seconds, 41400);
  EXPECT_EQ(control.extras_seconds, 68400);
  EXPECT_EQ(control.completionist_seconds, 104400);
  EXPECT_EQ(control.matched_name, "Control Ultimate Edition");
  EXPECT_EQ(control.url, "https://example.invalid/control");
}

TEST(BeatTimesTests, DropsRowsWithNoFigureAtAll) {
  const auto data = beat_times::parse(kDataset);

  // A name with no durations would draw an empty gauge, which asserts nothing.
  EXPECT_EQ(data.by_steam_appid.count("1"), 0u);
  EXPECT_EQ(data.by_name.count(beat_times::normalise_name("Nothing Known")), 0u);
}

TEST(BeatTimesTests, MatchesOnCaseAndPunctuationButNotOnEdition) {
  const auto data = beat_times::parse(kDataset);

  // The launcher and the catalogue rarely agree on punctuation or case.
  EXPECT_EQ(beat_times::normalise_name("Slay the Spire II"), beat_times::normalise_name("slay  the-spire, II"));

  // An edition suffix is a different game as far as this is concerned, because a
  // confident wrong number is worse than no number.
  EXPECT_NE(beat_times::normalise_name("Control"), beat_times::normalise_name("Control Ultimate Edition"));
}

TEST(BeatTimesTests, PrefersTheAppIdOverTheTitle) {
  const auto data = beat_times::parse(kDataset);

  // The id is exact; the title is a guess that is usually right. Given both, and a title
  // that would match something else, the id still decides.
  const auto found = beat_times::lookup(data, "870780", "Slay the Spire II");
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->matched_name, "Control Ultimate Edition");
}

TEST(BeatTimesTests, FallsBackToTheTitleWhenThereIsNoAppId) {
  const auto data = beat_times::parse(kDataset);

  const auto found = beat_times::lookup(data, "", "SLAY THE SPIRE II");
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->main_seconds, 90000);

  EXPECT_FALSE(beat_times::lookup(data, "", "Some Game Nobody Catalogued").has_value());
  EXPECT_FALSE(beat_times::lookup(data, "999999", "").has_value());
}

TEST(BeatTimesTests, SurvivesEmptyAndMalformedPayloads) {
  EXPECT_TRUE(beat_times::parse("").by_name.empty());
  EXPECT_TRUE(beat_times::parse("not json").by_name.empty());
  EXPECT_TRUE(beat_times::parse(R"json({"games":"not an array"})json").by_name.empty());
  EXPECT_TRUE(beat_times::parse(R"json([1,2,3])json").by_name.empty());
}
