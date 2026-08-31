/**
 * @file src/config_file_update.h
 * @brief Lossless text updates for the Polaris configuration file.
 */
#pragma once

#include <string>
#include <string_view>
#include <unordered_map>

namespace config_file_update {

  struct result_t {
    std::string content;
    bool changed = false;
  };

  /**
   * Update scalar configuration values without reserializing unrelated text.
   *
   * Existing comments, ordering, whitespace, newline style, unknown settings,
   * and list bodies remain byte-for-byte unchanged. An empty value removes all
   * matching assignments so a duplicate cannot unexpectedly become active.
   */
  result_t apply(
    std::string_view existing,
    const std::unordered_map<std::string, std::string> &updates
  );

}  // namespace config_file_update
