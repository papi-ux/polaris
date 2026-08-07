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

TEST(BeatTimesTests, ACuratedTitleDecidesInsteadOfTheAppId) {
  const auto data = beat_times::parse(kDataset);

  // Same inputs as PrefersTheAppIdOverTheTitle, where the id wins. Once somebody has
  // said which game this is, it does not.
  const auto found = beat_times::lookup_for_identity(data, "Slay the Spire II", "870780", "Control Ultimate Edition");
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->main_seconds, 90000);
}

TEST(BeatTimesTests, ACuratedTitleTheDatasetLacksAnswersWithNothing) {
  const auto data = beat_times::parse(kDataset);

  // The case a correction is most likely made for: the curated title is not catalogued
  // yet. Falling back to the id here would serve the estimate the correction rejected,
  // and the caller would never ask about the curated title at all, so the correction
  // could never take effect.
  EXPECT_FALSE(beat_times::lookup_for_identity(data, "Some Game Nobody Catalogued", "870780", "Control Ultimate Edition").has_value());
}

TEST(BeatTimesTests, WithoutACuratedTitleIdentityLookupIsTheOrdinaryOne) {
  const auto data = beat_times::parse(kDataset);

  const auto found = beat_times::lookup_for_identity(data, "", "870780", "Slay the Spire II");
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->matched_name, "Control Ultimate Edition");
}

TEST(BeatTimesTests, SurvivesEmptyAndMalformedPayloads) {
  EXPECT_TRUE(beat_times::parse("").by_name.empty());
  EXPECT_TRUE(beat_times::parse("not json").by_name.empty());
  EXPECT_TRUE(beat_times::parse(R"json({"games":"not an array"})json").by_name.empty());
  EXPECT_TRUE(beat_times::parse(R"json([1,2,3])json").by_name.empty());
}

TEST(BeatTimesTests, MatchKeyKeepsWordBoundariesThatDistanceNeeds) {
  // normalise_name strips everything, which is right for a dictionary key and wrong for
  // a distance: "slaythespire" and "slaythespireii" are closer than the titles are.
  EXPECT_EQ(beat_times::match_key("Slay the Spire: II"), "slay the spire ii");
  EXPECT_EQ(beat_times::match_key("  Control -- Ultimate  Edition "), "control ultimate edition");
  EXPECT_EQ(beat_times::match_key("???"), "");
}

TEST(BeatTimesTests, EditDistanceCountsTheUsualEdits) {
  EXPECT_EQ(beat_times::edit_distance("control", "control"), 0);
  EXPECT_EQ(beat_times::edit_distance("", "control"), 7);
  EXPECT_EQ(beat_times::edit_distance("control", ""), 7);
  EXPECT_EQ(beat_times::edit_distance("kitten", "sitting"), 3);
}

TEST(BeatTimesTests, AcceptsAnEditionSuffixEitherWayButNotADifferentGame) {
  const auto accepts = [](std::string_view a, std::string_view b) {
    return beat_times::is_acceptable_match(a, b, beat_times::edit_distance(a, b));
  };

  const auto control = beat_times::match_key("Control");
  const auto edition = beat_times::match_key("Control Ultimate Edition");
  const auto subtitle = beat_times::match_key("Control: The Foundation");

  // A launcher name and a catalogue name disagree in both directions, so both are fine.
  EXPECT_TRUE(accepts(control, edition));
  EXPECT_TRUE(accepts(edition, control));
  EXPECT_TRUE(accepts(control, subtitle));

  // These really are in the results for "Control". Neither is the game.
  EXPECT_FALSE(accepts(control, beat_times::match_key("Air Control")));
  EXPECT_FALSE(accepts(control, beat_times::match_key("3-D Ultra Radio Control Racers Deluxe")));
}

TEST(BeatTimesTests, RejectsEmptyEitherSide) {
  EXPECT_FALSE(beat_times::is_acceptable_match("", "control", 7));
  EXPECT_FALSE(beat_times::is_acceptable_match("control", "", 7));
}

TEST(BeatTimesTests, FindsAnEntryUnderEitherSpelling) {
  // What a launcher calls it and what a catalogue calls it are both names some future
  // lookup will arrive with, so the entry answers to both.
  const auto data = beat_times::parse(R"json({
    "games": [{
      "name": "Slay the Spire 2",
      "matched_name": "Slay the Spire II",
      "main_seconds": 90000
    }]
  })json");

  ASSERT_TRUE(beat_times::lookup(data, "", "Slay the Spire 2").has_value());
  ASSERT_TRUE(beat_times::lookup(data, "", "Slay the Spire II").has_value());

  // The one shown is the catalogue's, because that is the one worth checking.
  EXPECT_EQ(beat_times::lookup(data, "", "Slay the Spire 2")->matched_name, "Slay the Spire II");
}

TEST(BeatTimesTests, AHandWrittenEntryNeedsOnlyAName) {
  const auto data = beat_times::parse(R"json({"games":[{"name":"Control","main_seconds":42166}]})json");
  const auto found = beat_times::lookup(data, "", "Control");
  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(found->matched_name, "Control");
}
