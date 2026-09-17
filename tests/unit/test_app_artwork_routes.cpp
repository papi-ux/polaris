/**
 * @file tests/unit/test_app_artwork_routes.cpp
 * @brief Remove artwork and Find artwork again on the console's Apps page.
 */
#include <src/confighttp.h>
#include <src/game_artwork_override.h>

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

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
