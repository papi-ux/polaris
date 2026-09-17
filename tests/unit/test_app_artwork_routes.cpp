/**
 * @file tests/unit/test_app_artwork_routes.cpp
 * @brief Remove artwork and Find artwork again on the console's Apps page.
 */
#include <src/confighttp.h>
#include <src/game_artwork_override.h>
#include <src/game_library_scanner.h>

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace {
  constexpr const char *APP_UUID = "F727EEEE-A124-040A-6D03-33DF1E45E189";
  constexpr const char *OTHER_UUID = "B2A1676D-F736-E802-2E01-ACCD59D9951F";

  std::string read_source(const char *relative) {
    std::ifstream input(std::filesystem::path {POLARIS_SOURCE_DIR} / relative);
    EXPECT_TRUE(input.good()) << relative;
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
  }

  std::string handler_body(const std::string &source, const std::string &signature, const std::string &closing = "\n  }\n") {
    const auto start = source.find(signature);
    EXPECT_NE(start, std::string::npos) << signature;
    if (start == std::string::npos) return {};
    const auto end = source.find(closing, start);
    EXPECT_NE(end, std::string::npos) << signature;
    return end == std::string::npos ? std::string {} : source.substr(start, end - start);
  }
}  // namespace

TEST(AppArtworkRoutes, ReadOnlyAnAppUuidFromASmallObject) {
  using confighttp::decode_app_artwork_request;
  EXPECT_EQ(decode_app_artwork_request(std::string(R"({"uuid":")") + APP_UUID + R"("})"), APP_UUID);
  for (const std::string body : {
         std::string {}, std::string {"null"}, std::string {"[]"}, std::string {"{}"}, std::string {"{"},
         std::string {R"({"uuid":7})"}, std::string {R"({"uuid":"not-a-uuid"})"},
         std::string(R"({"UUID":")") + APP_UUID + R"("})",
         std::string(R"({"uuid":")") + APP_UUID + R"(","extra":true})",
       }) {
    EXPECT_FALSE(decode_app_artwork_request(body).has_value()) << body;
  }
  const auto padded = std::string(R"({"uuid":")") + APP_UUID + R"(")" + std::string(1024, ' ') + "}";
  EXPECT_FALSE(decode_app_artwork_request(padded).has_value());
}

TEST(AppArtworkRoutes, ListsExactlyTheAppsWhoseLookupIsOff) {
  const auto appdata = std::filesystem::temp_directory_path() / "polaris-app-artwork-routes";
  std::error_code error;
  std::filesystem::remove_all(appdata, error);
  std::filesystem::create_directories(appdata);

  const nlohmann::json apps {{"apps", {
    {{"name", "Low Res Desktop"}, {"uuid", APP_UUID}},
    {{"name", "Desktop"}, {"uuid", OTHER_UUID}},
    {{"name", "No uuid"}},
    {{"name", "Bad uuid"}, {"uuid", "not-a-uuid"}},
  }}};
  EXPECT_EQ(confighttp::apps_with_artwork_lookup_off(appdata, apps), nlohmann::json::array());
  ASSERT_TRUE(game_artwork::remove_downloaded_artwork(appdata, APP_UUID));
  EXPECT_EQ(confighttp::apps_with_artwork_lookup_off(appdata, apps), nlohmann::json::array({APP_UUID}));
  ASSERT_TRUE(game_artwork::enable_automatic_artwork_lookup(appdata, APP_UUID));
  EXPECT_EQ(confighttp::apps_with_artwork_lookup_off(appdata, apps), nlohmann::json::array());
  EXPECT_EQ(confighttp::apps_with_artwork_lookup_off(appdata, nlohmann::json::array()), nlohmann::json::array());

  std::filesystem::remove_all(appdata, error);
}

TEST(AppArtworkRoutes, RoutesNeedTheConsoleSessionCsrfAndJson) {
  const auto source = read_source("src/confighttp.cpp");
  EXPECT_NE(source.find(R"(server.resource["^/api/apps/artwork/remove$"]["POST"] = withCsrf(removeAppArtwork);)"),
            std::string::npos);
  EXPECT_NE(source.find(R"(server.resource["^/api/apps/artwork/find$"]["POST"] = withCsrf(findAppArtwork);)"),
            std::string::npos);
  for (const auto *signature : {"void removeAppArtwork(", "void findAppArtwork("}) {
    const auto body = handler_body(source, signature);
    const auto guard = body.find("validateContentType(response, request, \"application/json\") || !authenticate(response, request)");
    const auto read = body.find("read_app_artwork_uuid(response, request)");
    EXPECT_NE(guard, std::string::npos) << signature;
    EXPECT_NE(read, std::string::npos) << signature;
    EXPECT_LT(guard, read) << signature;
  }
  const auto reader = handler_body(source, "std::optional<std::string> read_app_artwork_uuid(", "\n    }\n");
  EXPECT_NE(reader.find("count > 1024"), std::string::npos);
  EXPECT_NE(reader.find("not_found(response, request)"), std::string::npos);
  EXPECT_NE(reader.find("return app->uuid;"), std::string::npos);
  EXPECT_NE(source.find(R"(file_tree["artwork_lookup_off"] = apps_with_artwork_lookup_off(platf::appdata(), file_tree);)"),
            std::string::npos);
}

TEST(AppCoverSearch, StoresAPickedPosterUnderItsUuidInTheFormatItReallyIs) {
  const auto coverdir = std::filesystem::temp_directory_path() / "polaris-cover-select";
  std::error_code error;
  std::filesystem::remove_all(coverdir, error);

  const std::vector<unsigned char> png {0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x01};
  const std::vector<unsigned char> jpeg {0xff, 0xd8, 0xff, 0x02};

  const auto first = confighttp::store_selected_cover(coverdir, APP_UUID, "image/jpeg", jpeg);
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(std::filesystem::path(*first), coverdir / (std::string(APP_UUID) + ".jpg"));

  // A second pick in another format replaces the first rather than sitting next to it.
  const auto second = confighttp::store_selected_cover(coverdir, APP_UUID, "image/png", png);
  ASSERT_TRUE(second.has_value());
  EXPECT_EQ(std::filesystem::path(*second), coverdir / (std::string(APP_UUID) + ".png"));
  EXPECT_FALSE(std::filesystem::exists(coverdir / (std::string(APP_UUID) + ".jpg")));
  std::ifstream stored(*second, std::ios::binary);
  const std::vector<unsigned char> bytes {std::istreambuf_iterator<char>(stored), std::istreambuf_iterator<char>()};
  EXPECT_EQ(bytes, png);

  // The uuid names the file, so it must be one; the bytes must be the type they claim.
  EXPECT_FALSE(confighttp::store_selected_cover(coverdir, "../escape", "image/png", png).has_value());
  EXPECT_FALSE(confighttp::store_selected_cover(coverdir, APP_UUID, "image/gif", png).has_value());
  EXPECT_FALSE(confighttp::store_selected_cover(coverdir, APP_UUID, "image/jpeg", png).has_value());
  EXPECT_FALSE(confighttp::store_selected_cover(coverdir, APP_UUID, "image/png", {}).has_value());
  EXPECT_FALSE(std::filesystem::exists(coverdir / (std::string(APP_UUID) + ".png.tmp")));
  EXPECT_FALSE(std::filesystem::exists(coverdir / (std::string(APP_UUID) + ".jpg.tmp")));

  std::filesystem::remove_all(coverdir, error);
}

TEST(AppCoverSearch, TheConsoleSearchIsNovasSearchAndNeverLoadsImagesFromOutside) {
  const auto source = read_source("src/confighttp.cpp");
  const auto search = handler_body(source, "void searchCovers(");
  // One search for both surfaces: the old console handler took SteamGridDB's first autocomplete
  // result only, so "Heroic" found nothing while Nova listed Heroic Games Launcher.
  EXPECT_NE(search.find("game_artwork::manual::search_match_candidates("), std::string::npos);
  EXPECT_EQ(search.find(R"(search_data["data"][0])"), std::string::npos);
  // A cover needs a poster, so Find Cover reads past matches without one; Nova lists as before.
  EXPECT_NE(search.find("candidate_listing_e::matches_with_posters"), std::string::npos);
  EXPECT_NE(search.find("./api/covers/preview/"), std::string::npos);
  EXPECT_NE(search.find("classify_search_failure(false, std::nullopt)"), std::string::npos);

  const auto select = handler_body(source, "void selectCover(");
  EXPECT_NE(select.find("artwork_candidate_previews().lookup("), std::string::npos);
  EXPECT_NE(select.find("store_selected_cover("), std::string::npos);
  EXPECT_EQ(select.find("download_file"), std::string::npos);

  EXPECT_NE(source.find(R"(server.resource["^/api/covers/preview/([0-9a-f]{32})$"]["GET"] = previewCover;)"), std::string::npos);
  EXPECT_NE(source.find(R"(server.resource["^/api/covers/select$"]["POST"] = withCsrf(selectCover);)"), std::string::npos);

  const auto nova = read_source("src/nvhttp.cpp");
  const auto nova_search = handler_body(nova, "auto polarisSearchGameArtworkMatches = ", "\n    };\n");
  EXPECT_NE(nova_search.find("game_artwork::manual::search_match_candidates("), std::string::npos);
  EXPECT_EQ(nova_search.find("candidate_listing_e::matches_with_posters"), std::string::npos);
}

TEST(AppCoverSearch, SavingAChosenCoverTakesThePosterBackFromNova) {
  const auto source = read_source("src/confighttp.cpp");
  const auto save = handler_body(source, "void saveApp(");
  const auto previous = save.find("stored_app_image(fileTree, ");
  const auto merge = save.find("proc::migrate_apps(&fileTree, &inputTree);");
  const auto yield = save.find("game_artwork::yield_picked_poster_to_console_cover(");
  ASSERT_NE(previous, std::string::npos);
  ASSERT_NE(merge, std::string::npos);
  ASSERT_NE(yield, std::string::npos);
  // The image the entry named is read before the save replaces it, and the pick yields after.
  EXPECT_LT(previous, merge);
  EXPECT_LT(merge, yield);
  EXPECT_NE(save.find(R"(platf::appdata() / "covers")"), std::string::npos);
}

TEST(HeroicLauncherEntry, ImportPublishesItWithTheBundledHeroicPoster) {
  nlohmann::json tree {{"apps", nlohmann::json::array()}};
  confighttp::ensure_heroic_library_app(tree, game_library::launcher_install_t::flatpak);
  ASSERT_EQ(tree["apps"].size(), 1u);
  const auto &launcher = tree["apps"][0];
  EXPECT_EQ(launcher["name"], "Heroic");
  EXPECT_EQ(launcher["source"], "heroic");
  EXPECT_EQ(launcher["image-path"], "heroic.png");
  EXPECT_EQ(launcher["detached"][0], game_library::heroic_launcher_command(game_library::launcher_install_t::flatpak));

  // Installed with every other file in the common assets, at the size of its neighbours.
  const auto png_size = [](const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    std::vector<unsigned char> header(24);
    input.read(reinterpret_cast<char *>(header.data()), static_cast<std::streamsize>(header.size()));
    EXPECT_EQ(input.gcount(), 24) << path;
    const std::vector<unsigned char> signature {0x89, 'P', 'N', 'G', '\r', '\n', 0x1a, '\n'};
    EXPECT_TRUE(std::equal(signature.begin(), signature.end(), header.begin())) << path;
    const auto big_endian = [&header](std::size_t offset) {
      return (header[offset] << 24) | (header[offset + 1] << 16) | (header[offset + 2] << 8) | header[offset + 3];
    };
    return std::pair {big_endian(16), big_endian(20)};
  };
  const auto assets = std::filesystem::path {POLARIS_SOURCE_DIR} / "src_assets/common/assets";
  EXPECT_EQ(png_size(assets / "heroic.png"), png_size(assets / "lutris.png"));
  EXPECT_EQ(png_size(assets / "heroic.png"), (std::pair {600, 900}));
}
