/**
 * @file src/beat_times.h
 * @brief Completion estimates, read from a local dataset rather than a third party.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>

namespace beat_times {

  /** What the dataset says about finishing one game. */
  struct estimate_t {
    std::string matched_name;  // carried so a wrong match is visible, as artwork matches are
    std::string url;           // the game's page; without it the estimate is a readable, not a link
    int64_t main_seconds = 0;
    int64_t extras_seconds = 0;
    int64_t completionist_seconds = 0;

    /** An entry with no figure at all has nothing to draw. */
    bool has_any() const {
      return main_seconds > 0 || extras_seconds > 0 || completionist_seconds > 0;
    }
  };

  /** One parsed dataset, indexed both ways it can be matched. */
  struct dataset_t {
    std::map<std::string, estimate_t> by_steam_appid;
    std::map<std::string, estimate_t> by_name;
    int64_t generated_at = 0;
  };

  /**
   * @brief Fold a title down to what two spellings of the same game share.
   *
   * Case and punctuation differ constantly between a launcher's name and a catalogue's,
   * so neither survives. Everything else does, which keeps the match conservative:
   * an edition suffix still makes two different keys, and a wrong match is worse than
   * no match when the number is presented as fact.
   */
  std::string normalise_name(std::string_view name);

  /** @brief Parse a dataset payload. Pure, so it needs no file to be tested. */
  dataset_t parse(std::string_view json_payload);

  /** @brief The dataset on disk, re-read when the file changes underneath. */
  const dataset_t &dataset();

  /**
   * @brief Tell this where its dataset lives and whether it may go looking.
   *
   * Taken as arguments rather than read from globals: where the file sits and who is
   * allowed to fetch both belong to whoever starts the process, and a module that
   * reaches for them cannot be linked into a unit test without dragging the whole
   * configuration layer along.
   *
   * Until this is called nothing is served and nothing is requested.
   */
  void configure(std::filesystem::path dataset, bool lookups_enabled);

  /**
   * @brief Fold a title to what a fuzzy match can work with.
   *
   * Unlike normalise_name this keeps word boundaries, because the distance between two
   * titles is only meaningful while the words are still separable.
   */
  std::string match_key(std::string_view name);

  /** @brief Edit distance, for choosing between candidates that all nearly match. */
  int edit_distance(std::string_view left, std::string_view right);

  /**
   * @brief Whether a candidate is close enough to be believed.
   *
   * A confident wrong number is worse than none, so a near miss is rejected rather than
   * rounded up to a match.
   */
  bool is_acceptable_match(std::string_view query, std::string_view candidate, int distance);

  /**
   * @brief Ask the network about a title, some time soon, once.
   *
   * Returns immediately. The answer lands in the dataset file and is served by the next
   * request that asks for it; nothing waits on the network while a library is being
   * serialised.
   */
  void request_lookup(const std::string &steam_appid, const std::string &name);

  /** @brief Stop the background worker. Safe to call when it never started. */
  void shutdown();

  /**
   * @brief The estimate for a game, preferring the id over the name.
   *
   * A Steam app id is the same answer every time; a title is a guess that happens to be
   * right most of the time, so it is only asked when there is no id to ask.
   */
  std::optional<estimate_t> lookup(const dataset_t &data, std::string_view steam_appid, std::string_view name);

  /**
   * @brief The estimate for a game somebody has told us the identity of.
   *
   * A curated title is a correction, and the app id is usually what made the wrong
   * estimate confident in the first place, so a curated entry never falls back to it:
   * it answers with its own title or answers with nothing. Falling back would serve the
   * exact estimate the correction rejected, and it would do so most often when the
   * curated title is one the dataset does not carry yet — which is the case somebody is
   * most likely correcting.
   *
   * @param curated_title Title from a manual match, or empty when there is none.
   */
  std::optional<estimate_t> lookup_for_identity(const dataset_t &data,
                                                std::string_view curated_title,
                                                std::string_view steam_appid,
                                                std::string_view name);

}  // namespace beat_times
