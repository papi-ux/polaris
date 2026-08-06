/**
 * @file src/beat_times.h
 * @brief Completion estimates, read from a local dataset rather than a third party.
 */
#pragma once

#include <cstdint>
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
   * @brief The estimate for a game, preferring the id over the name.
   *
   * A Steam app id is the same answer every time; a title is a guess that happens to be
   * right most of the time, so it is only asked when there is no id to ask.
   */
  std::optional<estimate_t> lookup(const dataset_t &data, std::string_view steam_appid, std::string_view name);

}  // namespace beat_times
