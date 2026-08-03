#include <gtest/gtest.h>

#include <src/game_artwork.h>
#include <src/game_artwork_override.h>

#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <string_view>
#include <thread>
#include <chrono>

namespace {
  namespace fs = std::filesystem;

  constexpr std::string_view GAME_UUID = "123e4567-e89b-12d3-a456-426614174000";
  constexpr std::string_view OTHER_UUID = "123e4567-e89b-12d3-a456-426614174001";

  struct temp_dir_t {
    fs::path path;

    explicit temp_dir_t(std::string_view name):
        path(fs::temp_directory_path() / ("polaris-game-artwork-override-" + std::string(name))) {
      std::error_code error;
      fs::remove_all(path, error);
      fs::create_directories(path);
    }

    ~temp_dir_t() {
      std::error_code error;
      fs::remove_all(path, error);
    }
  };

  game_artwork::artwork_override_t valid_override() {
    return {
      .uuid = std::string(GAME_UUID),
      .provider = "steamgriddb",
      .provider_game_id = "12345",
      .title = "Portal 2",
      .steam_appid = "620",
      .manual = true,
      .updated_at = 1720000000123,
    };
  }

  std::string valid_override_json() {
    const auto value = valid_override();
    return nlohmann::json {
      {"version", game_artwork::artwork_override_version}, {"uuid", value.uuid},
      {"provider", value.provider}, {"provider_game_id", value.provider_game_id},
      {"title", value.title}, {"steam_appid", *value.steam_appid},
      {"manual", value.manual}, {"updated_at", value.updated_at}
    }.dump();
  }

  fs::path metadata_path(const fs::path &appdata) {
    return game_artwork::cache_root(appdata) / GAME_UUID / "override.json";
  }

  void write_text(const fs::path &path, std::string_view text) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output.good());
    output << text;
    ASSERT_TRUE(output.good());
  }

  std::string read_text(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
  }

  std::string png_bytes(char marker) {
    return std::string {static_cast<char>(0x89), 'P', 'N', 'G', '\r', '\n', static_cast<char>(0x1A), '\n', marker};
  }

  std::string jpeg_bytes(char marker) {
    return std::string {static_cast<char>(0xFF), static_cast<char>(0xD8), static_cast<char>(0xFF), marker};
  }

  void expect_live_symlink_rejected(const fs::path &appdata, const fs::path &outside_game) {
    const auto outside_metadata = outside_game / "override.json";
    const auto outside_poster = outside_game / "poster.override.jpg";
    write_text(outside_metadata, valid_override_json());
    write_text(outside_poster, jpeg_bytes('O'));
    EXPECT_FALSE(game_artwork::load_artwork_override(appdata, GAME_UUID).has_value());
    EXPECT_FALSE(game_artwork::clear_artwork_override(appdata, GAME_UUID));
    EXPECT_TRUE(fs::is_regular_file(outside_metadata));
    EXPECT_TRUE(fs::is_regular_file(outside_poster));
  }
}  // namespace

TEST(GameArtworkOverrideStore, RoundTripsOnlySanitizedMetadata) {
  temp_dir_t temp("round-trip");
  const auto expected = valid_override();

  ASSERT_TRUE(game_artwork::save_artwork_override(temp.path, expected));
  const auto loaded = game_artwork::load_artwork_override(temp.path, GAME_UUID);
  ASSERT_TRUE(loaded.has_value());
  EXPECT_EQ(*loaded, expected);

  std::ifstream input(metadata_path(temp.path), std::ios::binary);
  ASSERT_TRUE(input.good());
  const auto persisted = nlohmann::json::parse(input);
  EXPECT_EQ(persisted.at("version"), game_artwork::artwork_override_version);
  EXPECT_EQ(persisted.at("uuid"), GAME_UUID);
  EXPECT_EQ(persisted.at("provider"), "steamgriddb");
  EXPECT_EQ(persisted.at("provider_game_id"), "12345");
  EXPECT_EQ(persisted.at("title"), "Portal 2");
  EXPECT_EQ(persisted.at("steam_appid"), "620");
  EXPECT_EQ(persisted.at("manual"), true);
  EXPECT_EQ(persisted.at("updated_at"), 1720000000123);
  EXPECT_EQ(persisted.size(), 8);

  const auto serialized = persisted.dump();
  EXPECT_EQ(serialized.find("api_key"), std::string::npos);
  EXPECT_EQ(serialized.find("authorization"), std::string::npos);
  EXPECT_EQ(serialized.find("https://"), std::string::npos);
  EXPECT_EQ(serialized.find("payload"), std::string::npos);
}

TEST(GameArtworkOverrideStore, RejectsInvalidMetadataBeforePersistence) {
  temp_dir_t temp("validation");
  const auto expect_rejected = [&](game_artwork::artwork_override_t candidate) {
    EXPECT_FALSE(game_artwork::save_artwork_override(temp.path, candidate));
    EXPECT_FALSE(fs::exists(metadata_path(temp.path)));
  };

  auto candidate = valid_override();
  candidate.uuid = "../123e4567-e89b-12d3-a456-426614174000";
  expect_rejected(candidate);

  candidate = valid_override();
  candidate.provider = "gog";
  expect_rejected(candidate);

  candidate = valid_override();
  candidate.provider_game_id = "12x45";
  expect_rejected(candidate);

  candidate = valid_override();
  candidate.provider_game_id.assign(game_artwork::maximum_provider_game_id_length + 1, '1');
  expect_rejected(candidate);

  candidate = valid_override();
  candidate.title.clear();
  expect_rejected(candidate);

  candidate = valid_override();
  candidate.title.assign(game_artwork::maximum_override_title_bytes + 1, 'a');
  expect_rejected(candidate);

  candidate = valid_override();
  candidate.title = "Portal\n2";
  expect_rejected(candidate);

  candidate = valid_override();
  candidate.steam_appid = "620/path";
  expect_rejected(candidate);

  candidate = valid_override();
  candidate.manual = false;
  expect_rejected(candidate);

  candidate = valid_override();
  candidate.updated_at = -1;
  expect_rejected(candidate);
}

TEST(GameArtworkOverrideStore, MalformedAndTamperedFilesFailClosed) {
  temp_dir_t temp("fail-closed");
  const auto path = metadata_path(temp.path);

  write_text(path, "{not-json");
  EXPECT_FALSE(game_artwork::load_artwork_override(temp.path, GAME_UUID).has_value());

  auto persisted = nlohmann::json {
    {"version", game_artwork::artwork_override_version},
    {"uuid", GAME_UUID},
    {"provider", "steam"},
    {"provider_game_id", "620"},
    {"title", "Portal 2"},
    {"steam_appid", "620"},
    {"manual", true},
    {"updated_at", 10},
  };

  auto tampered = persisted;
  tampered["uuid"] = OTHER_UUID;
  write_text(path, tampered.dump());
  EXPECT_FALSE(game_artwork::load_artwork_override(temp.path, GAME_UUID).has_value());

  tampered = persisted;
  tampered["provider"] = "https://evil.example";
  write_text(path, tampered.dump());
  EXPECT_FALSE(game_artwork::load_artwork_override(temp.path, GAME_UUID).has_value());

  tampered = persisted;
  tampered["manual"] = false;
  write_text(path, tampered.dump());
  EXPECT_FALSE(game_artwork::load_artwork_override(temp.path, GAME_UUID).has_value());

  tampered = persisted;
  tampered["updated_at"] = -1;
  write_text(path, tampered.dump());
  EXPECT_FALSE(game_artwork::load_artwork_override(temp.path, GAME_UUID).has_value());

  tampered = persisted;
  tampered["raw_payload"] = {{"secret", "credential"}};
  write_text(path, tampered.dump());
  EXPECT_FALSE(game_artwork::load_artwork_override(temp.path, GAME_UUID).has_value());

  write_text(path, std::string(game_artwork::maximum_override_file_bytes + 1, 'x'));
  EXPECT_FALSE(game_artwork::load_artwork_override(temp.path, GAME_UUID).has_value());
}

TEST(GameArtworkOverrideStore, RejectsSymlinkedLiveArtworkAncestor) {
  temp_dir_t appdata("live-artwork-symlink");
  temp_dir_t outside("live-artwork-symlink-outside");
  fs::create_directory_symlink(outside.path, appdata.path / "artwork");
  expect_live_symlink_rejected(appdata.path, outside.path / "v1" / GAME_UUID);
}

TEST(GameArtworkOverrideStore, RejectsSymlinkedLiveVersionAncestor) {
  temp_dir_t appdata("live-version-symlink");
  temp_dir_t outside("live-version-symlink-outside");
  fs::create_directory(appdata.path / "artwork");
  fs::create_directory_symlink(outside.path, appdata.path / "artwork" / "v1");
  expect_live_symlink_rejected(appdata.path, outside.path / GAME_UUID);
}

TEST(GameArtworkOverrideStore, RejectsSymlinkedLiveGameDirectory) {
  temp_dir_t appdata("live-game-symlink");
  temp_dir_t outside("live-game-symlink-outside");
  fs::create_directories(game_artwork::cache_root(appdata.path));
  fs::create_directory_symlink(outside.path, game_artwork::cache_root(appdata.path) / GAME_UUID);
  expect_live_symlink_rejected(appdata.path, outside.path);
}

TEST(GameArtworkOverrideStore, AtomicWritesCleanTemporaryFilesOnSuccessAndFailure) {
  temp_dir_t temp("atomic-cleanup");
  const auto path = metadata_path(temp.path);
  const auto temporary = fs::path(path.string() + ".tmp");

  write_text(temporary, "stale partial write");
  ASSERT_TRUE(game_artwork::save_artwork_override(temp.path, valid_override()));
  EXPECT_TRUE(fs::is_regular_file(path));
  EXPECT_FALSE(fs::exists(temporary));

  std::error_code error;
  fs::remove(path, error);
  ASSERT_FALSE(error);
  fs::create_directory(path, error);
  ASSERT_FALSE(error);
  EXPECT_FALSE(game_artwork::save_artwork_override(temp.path, valid_override()));
  EXPECT_TRUE(fs::is_directory(path));
  EXPECT_FALSE(fs::exists(temporary));
}

TEST(GameArtworkOverrideStaging, CreatesOnlyInsideSafeArtworkAncestors) {
  temp_dir_t temp("safe-staging");
  const std::string token(32, 'a');
  const auto staging = game_artwork::create_artwork_staging_directory(temp.path, token);
  ASSERT_TRUE(staging.has_value());
  EXPECT_EQ(*staging, game_artwork::cache_root(temp.path) / ".transactions" / token);
  EXPECT_TRUE(fs::is_directory(*staging));
  EXPECT_FALSE(game_artwork::create_artwork_staging_directory(temp.path, "../escape").has_value());
}

TEST(GameArtworkOverrideStaging, AcceptsUppercaseHexTokensFromRuntimeGenerator) {
  temp_dir_t temp("uppercase-runtime-token");
  const std::string token = "0123456789ABCDEF0123456789ABCDEF";
  const auto staging = game_artwork::create_artwork_staging_directory(temp.path, token);
  ASSERT_TRUE(staging.has_value());
  EXPECT_EQ(*staging, game_artwork::cache_root(temp.path) / ".transactions" / token);
  EXPECT_TRUE(fs::is_directory(*staging));
}

TEST(GameArtworkOverrideStaging, RejectsSymlinkedArtworkAncestors) {
  temp_dir_t temp("symlink-staging");
  temp_dir_t outside("symlink-staging-outside");
  fs::create_directory_symlink(outside.path, temp.path / "artwork");
  EXPECT_FALSE(game_artwork::create_artwork_staging_directory(temp.path, std::string(32, 'b')).has_value());
  EXPECT_FALSE(fs::exists(outside.path / "v1" / ".transactions"));
}

TEST(GameArtworkOverrideStaging, RejectsSymlinkedTransactionDirectory) {
  temp_dir_t temp("symlink-transactions");
  temp_dir_t outside("symlink-transactions-outside");
  fs::create_directories(game_artwork::cache_root(temp.path));
  fs::create_directory_symlink(outside.path, game_artwork::cache_root(temp.path) / ".transactions");
  EXPECT_FALSE(game_artwork::create_artwork_staging_directory(temp.path, std::string(32, 'c')).has_value());
  EXPECT_TRUE(fs::is_empty(outside.path));
}

TEST(GameArtworkOverrideTransaction, MetadataFailureRestoresPriorMetadataAndAssets) {
  temp_dir_t live("transaction-rollback-live");
  temp_dir_t staging("transaction-rollback-stage");
  const auto old_metadata = valid_override();
  ASSERT_TRUE(game_artwork::save_artwork_override(live.path, old_metadata));
  const auto live_poster = game_artwork::cache_root(live.path) / GAME_UUID / "poster.override.jpg";
  const auto staged_poster = game_artwork::cache_root(staging.path) / GAME_UUID / "poster.override.png";
  write_text(live_poster, jpeg_bytes('O'));
  write_text(staged_poster, png_bytes('N'));

  auto replacement = old_metadata;
  replacement.provider_game_id = "99999";
  replacement.title = "Replacement";
  replacement.updated_at++;
  game_artwork::staged_override_commit_options_t options;
  options.save_metadata = [](const fs::path &, const game_artwork::artwork_override_t &) { return false; };

  EXPECT_FALSE(game_artwork::commit_staged_artwork_override(live.path, staging.path, replacement, options));
  EXPECT_TRUE(fs::is_regular_file(live_poster));
  EXPECT_EQ(read_text(live_poster), jpeg_bytes('O'));
  EXPECT_FALSE(fs::exists(game_artwork::cache_root(live.path) / GAME_UUID / "poster.override.png"));
  EXPECT_EQ(game_artwork::load_artwork_override(live.path, GAME_UUID), old_metadata);
}

TEST(GameArtworkOverrideTransaction, SuccessfulRematchReplacesTheWholeOverrideGeneration) {
  temp_dir_t live("transaction-generation-live");
  temp_dir_t staging("transaction-generation-stage");
  auto old_metadata = valid_override();
  ASSERT_TRUE(game_artwork::save_artwork_override(live.path, old_metadata));
  const auto live_dir = game_artwork::cache_root(live.path) / GAME_UUID;
  const auto staged_dir = game_artwork::cache_root(staging.path) / GAME_UUID;
  write_text(live_dir / "poster.override.jpg", jpeg_bytes('O'));
  write_text(live_dir / "hero.override.jpg", jpeg_bytes('H'));
  write_text(staged_dir / "poster.override.png", png_bytes('N'));
  auto next_metadata = old_metadata;
  next_metadata.provider_game_id = "99999";
  next_metadata.title = "Replacement";
  next_metadata.updated_at++;
  ASSERT_TRUE(game_artwork::commit_staged_artwork_override(live.path, staging.path, next_metadata));
  EXPECT_FALSE(fs::exists(live_dir / "poster.override.jpg"));
  EXPECT_TRUE(fs::is_regular_file(live_dir / "poster.override.png"));
  EXPECT_FALSE(fs::exists(live_dir / "hero.override.jpg"));
  EXPECT_EQ(game_artwork::load_artwork_override(live.path, GAME_UUID), next_metadata);
}

TEST(GameArtworkOverrideRecovery, RollsBackInterruptedPublication) {
  temp_dir_t live("recovery-rollback");
  const auto old_metadata = valid_override();
  ASSERT_TRUE(game_artwork::save_artwork_override(live.path, old_metadata));
  const auto directory = metadata_path(live.path).parent_path();
  const auto old_poster = directory / "poster.override.jpg";
  const auto new_poster = directory / "poster.override.png";
  write_text(old_poster, jpeg_bytes('O'));
  fs::rename(metadata_path(live.path), fs::path(metadata_path(live.path).string() + ".override-rollback"));
  fs::rename(old_poster, fs::path(old_poster.string() + ".override-rollback"));
  write_text(new_poster, png_bytes('N'));
  write_text(directory / "override.transaction", "1\n");

  EXPECT_TRUE(game_artwork::recover_interrupted_artwork_override(live.path, GAME_UUID));
  EXPECT_EQ(game_artwork::load_artwork_override(live.path, GAME_UUID), old_metadata);
  EXPECT_EQ(read_text(old_poster), jpeg_bytes('O'));
  EXPECT_FALSE(fs::exists(new_poster));
  EXPECT_FALSE(fs::exists(directory / "override.transaction"));
}

TEST(GameArtworkOverrideRecovery, FinalizesCommittedPublication) {
  temp_dir_t live("recovery-finalize");
  auto old_metadata = valid_override();
  ASSERT_TRUE(game_artwork::save_artwork_override(live.path, old_metadata));
  const auto directory = metadata_path(live.path).parent_path();
  const auto old_poster = directory / "poster.override.jpg";
  const auto new_poster = directory / "poster.override.png";
  write_text(old_poster, jpeg_bytes('O'));
  fs::rename(metadata_path(live.path), fs::path(metadata_path(live.path).string() + ".override-rollback"));
  fs::rename(old_poster, fs::path(old_poster.string() + ".override-rollback"));
  auto next_metadata = old_metadata;
  next_metadata.provider_game_id = "99999";
  next_metadata.updated_at++;
  ASSERT_TRUE(game_artwork::save_artwork_override(live.path, next_metadata));
  write_text(new_poster, png_bytes('N'));
  write_text(directory / "override.transaction", "1\n");

  EXPECT_TRUE(game_artwork::recover_interrupted_artwork_override(live.path, GAME_UUID));
  EXPECT_EQ(game_artwork::load_artwork_override(live.path, GAME_UUID), next_metadata);
  EXPECT_EQ(read_text(new_poster), png_bytes('N'));
  EXPECT_FALSE(fs::exists(fs::path(old_poster.string() + ".override-rollback")));
  EXPECT_FALSE(fs::exists(directory / "override.transaction"));
}

TEST(GameArtworkOverrideRecovery, RestoresPrePublicationBackupsWithoutMarker) {
  temp_dir_t live("recovery-prepare");
  const auto old_metadata = valid_override();
  ASSERT_TRUE(game_artwork::save_artwork_override(live.path, old_metadata));
  const auto old_poster = metadata_path(live.path).parent_path() / "poster.override.jpg";
  const auto backup = fs::path(old_poster.string() + ".override-rollback");
  write_text(old_poster, jpeg_bytes('O'));
  fs::rename(old_poster, backup);

  EXPECT_TRUE(game_artwork::recover_interrupted_artwork_override(live.path, GAME_UUID));
  EXPECT_EQ(game_artwork::load_artwork_override(live.path, GAME_UUID), old_metadata);
  EXPECT_EQ(read_text(old_poster), jpeg_bytes('O'));
  EXPECT_FALSE(fs::exists(backup));
}

TEST(GameArtworkOverrideRecovery, SubsequentCommitRecoversInterruptedState) {
  temp_dir_t live("recovery-next-commit");
  temp_dir_t staging("recovery-next-commit-stage");
  auto old_metadata = valid_override();
  ASSERT_TRUE(game_artwork::save_artwork_override(live.path, old_metadata));
  const auto live_dir = game_artwork::cache_root(live.path) / GAME_UUID;
  const auto staged_dir = game_artwork::cache_root(staging.path) / GAME_UUID;
  const auto old_poster = live_dir / "poster.override.jpg";
  write_text(old_poster, jpeg_bytes('O'));
  fs::rename(metadata_path(live.path), fs::path(metadata_path(live.path).string() + ".override-rollback"));
  fs::rename(old_poster, fs::path(old_poster.string() + ".override-rollback"));
  write_text(live_dir / "poster.override.png", png_bytes('N'));
  write_text(live_dir / "override.transaction", "1\n");
  write_text(staged_dir / "poster.override.png", png_bytes('F'));
  auto next_metadata = old_metadata;
  next_metadata.provider_game_id = "99999";
  next_metadata.updated_at++;
  ASSERT_TRUE(game_artwork::commit_staged_artwork_override(live.path, staging.path, next_metadata));
  EXPECT_EQ(game_artwork::load_artwork_override(live.path, GAME_UUID), next_metadata);
  EXPECT_EQ(read_text(live_dir / "poster.override.png"), png_bytes('F'));
  EXPECT_FALSE(fs::exists(fs::path(old_poster.string() + ".override-rollback")));
}

TEST(GameArtworkOverrideTransaction, ReadersCannotObservePartialMultiKindPublication) {
  using namespace std::chrono_literals;
  temp_dir_t live("transaction-reader-live");
  temp_dir_t staging("transaction-reader-stage");
  ASSERT_TRUE(game_artwork::save_artwork_override(live.path, valid_override()));
  const auto live_dir = game_artwork::cache_root(live.path) / GAME_UUID;
  const auto staged_dir = game_artwork::cache_root(staging.path) / GAME_UUID;
  write_text(live_dir / "poster.override.jpg", jpeg_bytes('O'));
  write_text(live_dir / "hero.override.jpg", jpeg_bytes('o'));
  write_text(staged_dir / "poster.override.png", png_bytes('N'));
  write_text(staged_dir / "hero.override.png", png_bytes('n'));

  std::promise<void> first_published;
  auto first_ready = first_published.get_future();
  std::promise<void> release_writer;
  auto release = release_writer.get_future().share();
  game_artwork::staged_override_commit_options_t options;
  options.after_first_asset_published = [&] {
    first_published.set_value();
    release.wait();
  };
  auto writer = std::async(std::launch::async, [&] {
    return game_artwork::commit_staged_artwork_override(live.path, staging.path, valid_override(), options);
  });
  ASSERT_EQ(first_ready.wait_for(2s), std::future_status::ready);

  auto reader = std::async(std::launch::async, [&] {
    auto lock = game_artwork::acquire_artwork_override_read_lock();
    return std::pair {read_text(live_dir / "poster.override.png"), read_text(live_dir / "hero.override.png")};
  });
  EXPECT_EQ(reader.wait_for(100ms), std::future_status::timeout);
  release_writer.set_value();
  EXPECT_TRUE(writer.get());
  EXPECT_EQ(reader.get(), (std::pair {png_bytes('N'), png_bytes('n')}));
}

TEST(GameArtworkOverrideStore, ClearRemovesOverrideMetadataAndFilesButPreservesReusableAssets) {
  temp_dir_t temp("clear");
  ASSERT_TRUE(game_artwork::save_artwork_override(temp.path, valid_override()));
  const auto directory = metadata_path(temp.path).parent_path();
  const auto poster = directory / "poster.local.png";
  const auto hero = directory / "hero.steam.jpg";
  const auto reusable = directory / "poster.steamgriddb.jpg";
  const auto override_poster = directory / "poster.override.png";
  const auto override_logo = directory / "logo.override.webp";
  write_text(poster, "poster bytes");
  write_text(hero, "hero bytes");
  write_text(reusable, "provider bytes");
  write_text(override_poster, "override poster");
  write_text(override_logo, "override logo");
  write_text(fs::path(metadata_path(temp.path).string() + ".tmp"), "stale metadata");

  EXPECT_TRUE(game_artwork::clear_artwork_override(temp.path, GAME_UUID));
  EXPECT_FALSE(game_artwork::load_artwork_override(temp.path, GAME_UUID).has_value());
  EXPECT_FALSE(fs::exists(metadata_path(temp.path)));
  EXPECT_FALSE(fs::exists(fs::path(metadata_path(temp.path).string() + ".tmp")));
  EXPECT_TRUE(fs::is_regular_file(poster));
  EXPECT_TRUE(fs::is_regular_file(hero));
  EXPECT_TRUE(fs::is_regular_file(reusable));
  EXPECT_FALSE(fs::exists(override_poster));
  EXPECT_FALSE(fs::exists(override_logo));
  EXPECT_TRUE(fs::is_directory(directory));

  EXPECT_TRUE(game_artwork::clear_artwork_override(temp.path, GAME_UUID));
  EXPECT_FALSE(game_artwork::clear_artwork_override(temp.path, "../../etc"));
}

TEST(GameArtworkOverrideManifest, DecoratesWithSanitizedMatchAndDeterministicRevision) {
  auto metadata = valid_override();
  const nlohmann::json base {
    {"version", 1},
    {"revision", "1111222233334444"},
    {"state", "fallback"},
    {"cached_at", 0},
    {"override", {{"active", false}, {"kinds", nlohmann::json::array()}}},
    {"assets", {
      {"poster", {{"source", "override"}}},
      {"hero", {{"source", "steam"}}},
    }},
  };

  const auto first = game_artwork::decorate_manifest_with_artwork_override(base, metadata);
  const auto again = game_artwork::decorate_manifest_with_artwork_override(base, metadata);
  EXPECT_EQ(first.at("revision"), again.at("revision"));
  EXPECT_NE(first.at("revision"), base.at("revision"));
  EXPECT_EQ(first.at("match"), (nlohmann::json {
    {"provider", "steamgriddb"},
    {"provider_game_id", "12345"},
    {"title", "Portal 2"},
    {"steam_appid", "620"},
  }));
  EXPECT_EQ(first.at("override").at("active"), true);
  EXPECT_EQ(first.at("override").at("manual"), true);
  EXPECT_EQ(first.at("override").at("updated_at"), 1720000000123);
  EXPECT_EQ(first.at("override").at("kinds"), nlohmann::json::array({"poster"}));

  auto changed_metadata = metadata;
  changed_metadata.updated_at++;
  const auto changed = game_artwork::decorate_manifest_with_artwork_override(base, changed_metadata);
  EXPECT_NE(first.at("revision"), changed.at("revision"));

  metadata.steam_appid.reset();
  const auto without_steam = game_artwork::decorate_manifest_with_artwork_override(base, metadata);
  EXPECT_FALSE(without_steam.at("match").contains("steam_appid"));
  EXPECT_NE(first.at("revision"), without_steam.at("revision"));

  auto invalid = valid_override();
  invalid.title = "bad\ttitle";
  EXPECT_EQ(game_artwork::decorate_manifest_with_artwork_override(base, invalid), base);

  const auto serialized = first.dump();
  EXPECT_EQ(serialized.find("api_key"), std::string::npos);
  EXPECT_EQ(serialized.find("https://"), std::string::npos);
  EXPECT_EQ(serialized.find("payload"), std::string::npos);
}
