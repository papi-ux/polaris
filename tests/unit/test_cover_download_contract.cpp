/**
 * @file tests/unit/test_cover_download_contract.cpp
 * @brief POST /api/covers/download takes a URL to fetch and a uuid that lands
 *        in a filesystem path. Both come from the request body, so the handler
 *        has to check the URL against a parsed host allowlist rather than by
 *        substring, and build the path from an app it actually resolved.
 *
 *        The allowlist itself is covered behaviourally by GameArtworkAllowlist;
 *        what these assert is that the handler reaches for it.
 */
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

#include <gtest/gtest.h>

namespace {

  std::string read_source(const std::filesystem::path &relative_path) {
    std::ifstream input(std::filesystem::path {POLARIS_SOURCE_DIR} / relative_path);
    EXPECT_TRUE(input.good()) << relative_path;
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
  }

  std::string download_cover_body() {
    const auto source = read_source("src/confighttp.cpp");
    const auto handler = source.find("void downloadCover(");
    EXPECT_NE(handler, std::string::npos);
    if (handler == std::string::npos) {
      return {};
    }
    const auto next_handler = source.find("\n  void ", handler + 1);
    return source.substr(handler, next_handler - handler);
  }

}  // namespace

TEST(CoverDownloadContract, TheUrlIsCheckedAgainstAParsedHostAllowlist) {
  const auto body = download_cover_body();
  ASSERT_FALSE(body.empty());

  // A substring match passed anything merely containing the name, including
  // "http://10.0.0.1/?x=steamgriddb.com", and download_file follows redirects.
  EXPECT_EQ(body.find(R"(url.find("steamgriddb.com"))"), std::string::npos);
  EXPECT_EQ(body.find(R"(url.find("steamcdn"))"), std::string::npos);

  EXPECT_NE(body.find("game_artwork::is_allowed_provider_url"), std::string::npos);
}

TEST(CoverDownloadContract, TheCoverPathIsBuiltFromAResolvedApp) {
  const auto body = download_cover_body();
  ASSERT_FALSE(body.empty());

  // The request's uuid must not reach the path: "../.." escaped the covers
  // directory, and download_file creates parent directories on the way.
  EXPECT_EQ(body.find("coverdir + app_uuid"), std::string::npos);

  EXPECT_NE(body.find("proc::proc.get_apps()"), std::string::npos);
  EXPECT_NE(body.find("http::url_escape(app_entry->uuid)"), std::string::npos);
}

TEST(CoverDownloadContract, TheUploadHandlerStillEscapesItsKey) {
  // uploadCover was already correct; keep it that way, since it is the sibling
  // this handler should have matched in the first place.
  const auto source = read_source("src/confighttp.cpp");
  const auto handler = source.find("void uploadCover(");
  ASSERT_NE(handler, std::string::npos);
  const auto body = source.substr(handler, source.find("\n  void ", handler + 1) - handler);

  EXPECT_NE(body.find("http::url_escape(key)"), std::string::npos);
  EXPECT_NE(source.find(R"(return http::url_is_https_host(url, "images.igdb.com");)"), std::string::npos);
  EXPECT_NE(body.find("http::download_file(url, path, igdb_image_url_allowed)"), std::string::npos);
}
