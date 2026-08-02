#include <gtest/gtest.h>

#include <src/game_artwork_provider.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string_view>
#include <vector>

namespace {
  namespace fs = std::filesystem;
  using game_artwork::kind_e;
  using game_artwork::provider_e;
  using game_artwork::source_e;
  using game_artwork::providers::operation_e;
  using game_artwork::providers::request_t;
  using game_artwork::providers::transport_response_t;

  constexpr std::string_view GAME_UUID = "123e4567-e89b-12d3-a456-426614174000";

  struct temp_dir_t {
    fs::path path;

    explicit temp_dir_t(std::string_view name):
        path(fs::temp_directory_path() / ("polaris-artwork-executor-" + std::string(name))) {
      std::error_code error;
      fs::remove_all(path, error);
      fs::create_directories(path);
    }

    ~temp_dir_t() {
      std::error_code error;
      fs::remove_all(path, error);
    }
  };

  std::vector<unsigned char> jpeg(unsigned char marker = 0) {
    return {0xff, 0xd8, 0xff, 0xe0, marker};
  }

  void write_jpeg(const fs::path &path, unsigned char marker) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    const auto bytes = jpeg(marker);
    output.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
  }

  const game_artwork::asset_t *find_kind(
    const std::vector<game_artwork::asset_t> &assets,
    kind_e kind
  ) {
    const auto found = std::find_if(assets.begin(), assets.end(), [kind](const auto &asset) {
      return asset.kind == kind;
    });
    return found == assets.end() ? nullptr : &*found;
  }
}

TEST(GameArtworkDownloadExecutor, CommitsSuccessfulKindsAndCleansFailedPartials) {
  temp_dir_t temp("partial");
  const std::vector<request_t> plan {
    {provider_e::steam, operation_e::download, kind_e::poster,
     "https://cdn.cloudflare.steamstatic.com/steam/apps/620/library_600x900.jpg", false},
    {provider_e::steam, operation_e::download, kind_e::hero,
     "https://cdn.cloudflare.steamstatic.com/steam/apps/620/library_hero.jpg", false},
    {provider_e::steam, operation_e::download, kind_e::logo,
     "https://cdn.cloudflare.steamstatic.com/steam/apps/620/logo.png", false},
  };

  std::vector<kind_e> attempted;
  const auto assets = game_artwork::providers::execute_download_plan(
    temp.path,
    GAME_UUID,
    plan,
    [&](const request_t &request, std::uintmax_t maximum_bytes)
      -> std::optional<transport_response_t> {
      EXPECT_EQ(maximum_bytes, game_artwork::maximum_asset_bytes);
      attempted.push_back(*request.kind);
      if (request.kind == kind_e::poster) return transport_response_t {200, jpeg(1), request.url};
      if (request.kind == kind_e::hero) {
        return transport_response_t {200, {'n', 'o', 't', '-', 'i', 'm', 'a', 'g', 'e'}, request.url};
      }
      return std::nullopt;
    }
  );

  EXPECT_EQ(attempted, (std::vector<kind_e> {kind_e::poster, kind_e::hero, kind_e::logo}));
  ASSERT_EQ(assets.size(), 1);
  ASSERT_NE(find_kind(assets, kind_e::poster), nullptr);
  EXPECT_EQ(find_kind(assets, kind_e::poster)->source, source_e::steam);
  EXPECT_EQ(find_kind(assets, kind_e::poster)->mime_type, "image/jpeg");
  EXPECT_EQ(find_kind(assets, kind_e::hero), nullptr);
  EXPECT_EQ(find_kind(assets, kind_e::logo), nullptr);

  const auto cache_dir = game_artwork::cache_root(temp.path) / GAME_UUID;
  for (const auto &entry : fs::directory_iterator(cache_dir)) {
    EXPECT_EQ(entry.path().filename().string().find(".tmp"), std::string::npos);
  }
}

TEST(GameArtworkDownloadExecutor, PreservesExistingAssetsAndNeverFetchesUnsafePlanEntries) {
  temp_dir_t temp("preserve");
  const auto existing = game_artwork::cache_asset_path(
    temp.path, GAME_UUID, kind_e::poster, source_e::host, ".jpg");
  ASSERT_TRUE(existing.has_value());
  write_jpeg(*existing, 7);

  const std::vector<request_t> plan {
    {provider_e::steam, operation_e::download, kind_e::poster,
     "https://cdn.cloudflare.steamstatic.com/steam/apps/620/library_600x900.jpg", false},
    {provider_e::steam, operation_e::download, kind_e::hero,
     "https://cdn.cloudflare.steamstatic.com.evil.example/hero.jpg", false},
    {provider_e::steam, operation_e::search, kind_e::logo,
     "https://cdn.cloudflare.steamstatic.com/steam/apps/620/logo.png", false},
    {provider_e::steamgriddb, operation_e::download, kind_e::icon,
     "https://cdn.steamgriddb.com/icon/valid.png", true},
  };

  int calls = 0;
  const auto assets = game_artwork::providers::execute_download_plan(
    temp.path,
    GAME_UUID,
    plan,
    [&](const request_t &request, std::uintmax_t) -> std::optional<transport_response_t> {
      ++calls;
      EXPECT_EQ(request.kind, kind_e::icon);
      return transport_response_t {
        200,
        jpeg(9),
        "https://evil.example/redirected.jpg",
      };
    }
  );

  EXPECT_EQ(calls, 1);
  ASSERT_EQ(assets.size(), 1);
  ASSERT_NE(find_kind(assets, kind_e::poster), nullptr);
  EXPECT_EQ(find_kind(assets, kind_e::poster)->source, source_e::host);

  std::ifstream input(*existing, std::ios::binary);
  std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(input)), {});
  EXPECT_EQ(bytes, jpeg(7));
  EXPECT_EQ(find_kind(assets, kind_e::icon), nullptr);
}

TEST(GameArtworkDownloadExecutor, PromotesSteamOverCachedSteamGridDb) {
  temp_dir_t temp("priority-upgrade");
  const auto old = game_artwork::cache_asset_path(temp.path, GAME_UUID, kind_e::poster, source_e::steamgriddb, ".jpg");
  ASSERT_TRUE(old.has_value());
  write_jpeg(*old, 1);
  const auto plan = game_artwork::providers::plan_steam_assets("620");
  int calls = 0;
  const auto assets = game_artwork::providers::execute_download_plan(temp.path, GAME_UUID, plan,
    [&](const request_t &request, std::uintmax_t) -> std::optional<transport_response_t> {
      ++calls;
      return request.kind == kind_e::poster ? std::optional {transport_response_t {200, jpeg(2), request.url}} : std::nullopt;
    });
  EXPECT_EQ(calls, 3);
  ASSERT_NE(find_kind(assets, kind_e::poster), nullptr);
  EXPECT_EQ(find_kind(assets, kind_e::poster)->source, source_e::steam);
  EXPECT_TRUE(fs::exists(*old));
}

TEST(GameArtworkDownloadExecutor, TriesFallbacksPerKindAndRejectsOversizedBodiesAtomically) {
  temp_dir_t temp("fallback");
  const std::vector<request_t> plan {
    {provider_e::steamgriddb, operation_e::download, kind_e::hero,
     "https://cdn.steamgriddb.com/hero/first.jpg", true},
    {provider_e::steamgriddb, operation_e::download, kind_e::hero,
     "https://cdn2.steamgriddb.com/hero/second.jpg", true},
    {provider_e::steamgriddb, operation_e::download, kind_e::logo,
     "https://cdn.steamgriddb.com/logo/large.png", true},
  };

  int calls = 0;
  const auto assets = game_artwork::providers::execute_download_plan(
    temp.path,
    GAME_UUID,
    plan,
    [&](const request_t &request, std::uintmax_t) -> std::optional<transport_response_t> {
      ++calls;
      if (request.url.find("first") != std::string::npos) return transport_response_t {503, jpeg(), request.url};
      if (request.kind == kind_e::hero) return transport_response_t {200, jpeg(4), request.url};
      return transport_response_t {
        200,
        std::vector<unsigned char>(game_artwork::maximum_asset_bytes + 1, 0),
        request.url,
      };
    }
  );

  EXPECT_EQ(calls, 3);
  ASSERT_EQ(assets.size(), 1);
  ASSERT_NE(find_kind(assets, kind_e::hero), nullptr);
  EXPECT_EQ(find_kind(assets, kind_e::hero)->source, source_e::steamgriddb);
  EXPECT_EQ(find_kind(assets, kind_e::logo), nullptr);
}

TEST(GameArtworkDownloadExecutor, RejectsInvalidUuidWithoutInvokingTransport) {
  temp_dir_t temp("invalid-uuid");
  const auto plan = game_artwork::providers::plan_steam_assets("620");
  int calls = 0;
  const auto assets = game_artwork::providers::execute_download_plan(
    temp.path,
    "../../etc/passwd",
    plan,
    [&](const request_t &, std::uintmax_t) -> std::optional<transport_response_t> {
      ++calls;
      return transport_response_t {200, jpeg(), {}};
    }
  );
  EXPECT_TRUE(assets.empty());
  EXPECT_EQ(calls, 0);
}
