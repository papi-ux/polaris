/**
 * @file tests/unit/test_cover_sweep_predicate.cpp
 * @brief Who a cover sweep is allowed to ask a provider about.
 *
 * Getting this wrong is not a cosmetic bug in either direction. Asking about an entry Polaris ships
 * artwork for finds a coincidental game, which is how Low Res Desktop was once given Low Magic Age's
 * poster. Asking about an entry whose owner pressed Remove artwork undoes a decision they made on
 * purpose. And asking about an entry that already has a cover would replace it.
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "src/confighttp.h"
#include "src/game_artwork_override.h"
#include "src/process.h"

namespace fs = std::filesystem;

namespace confighttp {
  std::vector<artwork_sweep::candidate_t> games_without_a_cover(
    const fs::path &appdata,
    const nlohmann::json &hydrated,
    const std::vector<proc::ctx_t> &apps
  );
}

namespace {

  proc::ctx_t game(std::string uuid, std::string name) {
    proc::ctx_t app;
    app.uuid = std::move(uuid);
    app.name = std::move(name);
    // Something to launch, so it is not mistaken for an entry that streams the desktop.
    app.cmd = "/usr/bin/true";
    return app;
  }

  nlohmann::json list_with(const std::vector<std::pair<std::string, std::string>> &images) {
    auto apps = nlohmann::json::array();
    for (const auto &[uuid, image] : images) {
      apps.push_back({{"uuid", uuid}, {"image-path", image}});
    }
    return {{"apps", apps}};
  }

  std::vector<std::string> names_of(const std::vector<artwork_sweep::candidate_t> &games) {
    std::vector<std::string> names;
    names.reserve(games.size());
    for (const auto &candidate : games) {
      names.push_back(candidate.name);
    }
    return names;
  }

  constexpr auto kOne = "11111111-1111-4111-8111-111111111111";
  constexpr auto kTwo = "22222222-2222-4222-8222-222222222222";
  constexpr auto kThree = "33333333-3333-4333-8333-333333333333";

  /// An appdata directory of its own, so the host's real artwork state cannot change an answer.
  struct scoped_appdata_t {
    fs::path path;

    scoped_appdata_t():
        path(fs::temp_directory_path() / ("cover-sweep-" + std::to_string(::getpid()) + "-" +
                                          std::to_string(fs::file_time_type::clock::now().time_since_epoch().count()))) {
      fs::create_directories(path);
    }

    ~scoped_appdata_t() {
      std::error_code ignored;
      fs::remove_all(path, ignored);
    }
  };

}  // namespace

TEST(CoverSweepPredicate, AGameWithNoCoverIsAskedAboutAndOneWithACoverIsNot) {
  scoped_appdata_t appdata;
  const std::vector<proc::ctx_t> apps {game(kOne, "Blank Game"), game(kTwo, "Covered Game")};

  // The second entry names an image that does not exist, which validation answers for with the
  // generic box art, so it still counts as having no cover.
  auto games = confighttp::games_without_a_cover(
    appdata.path, list_with({{kOne, ""}, {kTwo, "/nowhere/at/all.png"}}), apps);
  EXPECT_EQ(names_of(games), (std::vector<std::string> {"Blank Game", "Covered Game"}));

  // Give the second a real image and it drops out.
  const auto cover = appdata.path / "covered.png";
  {
    // A one pixel PNG, because validation checks the bytes rather than the name.
    static constexpr unsigned char png[] = {
      0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
      0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4,
      0x89, 0x00, 0x00, 0x00, 0x0a, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x00, 0x01, 0x00, 0x00,
      0x05, 0x00, 0x01, 0x0d, 0x0a, 0x2d, 0xb4, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae,
      0x42, 0x60, 0x82};
    std::ofstream out(cover, std::ios::binary);
    out.write(reinterpret_cast<const char *>(png), sizeof(png));
  }
  games = confighttp::games_without_a_cover(
    appdata.path, list_with({{kOne, ""}, {kTwo, cover.string()}}), apps);
  EXPECT_EQ(names_of(games), (std::vector<std::string> {"Blank Game"}));
}

TEST(CoverSweepPredicate, RemoveArtworkIsNotUndoneByASweep) {
  scoped_appdata_t appdata;
  const std::vector<proc::ctx_t> apps {game(kOne, "Asked About"), game(kTwo, "Left Alone")};

  ASSERT_TRUE(game_artwork::remove_downloaded_artwork(appdata.path, kTwo));
  ASSERT_FALSE(game_artwork::automatic_artwork_lookup_enabled(appdata.path, kTwo));

  const auto games = confighttp::games_without_a_cover(
    appdata.path, list_with({{kOne, ""}, {kTwo, ""}}), apps);
  EXPECT_EQ(names_of(games), (std::vector<std::string> {"Asked About"}))
    << "an entry whose owner pressed Remove artwork was asked about anyway";

  // And Find artwork again puts it back in.
  ASSERT_TRUE(game_artwork::enable_automatic_artwork_lookup(appdata.path, kTwo));
  const auto after = confighttp::games_without_a_cover(
    appdata.path, list_with({{kOne, ""}, {kTwo, ""}}), apps);
  EXPECT_EQ(names_of(after), (std::vector<std::string> {"Asked About", "Left Alone"}));
}

TEST(CoverSweepPredicate, TheEntriesPolarisShipsArtworkForAreNeverAskedAbout) {
  scoped_appdata_t appdata;

  auto mirror = game(kOne, "Desktop");
  mirror.desktop_mirror = true;

  // No command, no detached command, no app id: an entry that launches nothing streams the desktop,
  // which is how an upgraded host's Low Res Desktop is recognised without matching its name.
  proc::ctx_t launches_nothing;
  launches_nothing.uuid = kTwo;
  launches_nothing.name = "Low Res Desktop";

  auto generated = game(VIRTUAL_DISPLAY_UUID, "Virtual Display");

  const std::vector<proc::ctx_t> apps {mirror, launches_nothing, generated, game(kThree, "A Real Game")};
  const auto games = confighttp::games_without_a_cover(
    appdata.path,
    list_with({{kOne, ""}, {kTwo, ""}, {VIRTUAL_DISPLAY_UUID, ""}, {kThree, ""}}),
    apps);

  EXPECT_EQ(names_of(games), (std::vector<std::string> {"A Real Game"}))
    << "a title search for one of these finds a coincidental game, not nothing";
}

TEST(CoverSweepPredicate, AnEntryWithoutAUsableNameOrUuidIsSkipped) {
  scoped_appdata_t appdata;
  const std::vector<proc::ctx_t> apps {
    game(kOne, "   "),  // nothing to search for
    game("not-a-uuid", "Bad Uuid"),
    game(kTwo, "Fine"),
  };
  const auto games = confighttp::games_without_a_cover(
    appdata.path, list_with({{kOne, ""}, {kTwo, ""}}), apps);
  EXPECT_EQ(names_of(games), (std::vector<std::string> {"Fine"}));
}

TEST(CoverSweepPredicate, TheHydratedListWinsOverTheEntryItself) {
  scoped_appdata_t appdata;
  // A Lutris entry carries no image of its own; the console's list gets one filled in when it is
  // read. The sweep reads that list, so the entry counts as having a cover.
  const auto cover = appdata.path / "lutris.png";
  {
    static constexpr unsigned char png[] = {
      0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
      0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1f, 0x15, 0xc4,
      0x89, 0x00, 0x00, 0x00, 0x0a, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x00, 0x01, 0x00, 0x00,
      0x05, 0x00, 0x01, 0x0d, 0x0a, 0x2d, 0xb4, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae,
      0x42, 0x60, 0x82};
    std::ofstream out(cover, std::ios::binary);
    out.write(reinterpret_cast<const char *>(png), sizeof(png));
  }

  auto lutris = game(kOne, "A Lutris Game");
  lutris.image_path.clear();
  const std::vector<proc::ctx_t> apps {lutris};

  EXPECT_TRUE(confighttp::games_without_a_cover(appdata.path, list_with({{kOne, ""}}), apps).size() == 1);
  EXPECT_TRUE(
    confighttp::games_without_a_cover(appdata.path, list_with({{kOne, cover.string()}}), apps).empty());
}
