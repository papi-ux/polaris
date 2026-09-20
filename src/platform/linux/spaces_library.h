#pragma once
#ifdef __linux__
#include "multiseat_container_backend.h"
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace multiseat::spaces {
  struct library_game_t {
    std::string target, name;
    bool operator==(const library_game_t &) const = default;
  };
  struct library_t {
    bool available = false;
    std::vector<library_game_t> games;
  };
  using library_reader_t = std::function<library_t(std::string_view)>;
  [[nodiscard]] library_reader_t make_library_reader(std::vector<container::profile_t> profiles,
    std::function<std::unique_ptr<container::host_t>()> factory);
  struct game_identity_t {
    std::string profile, target;
    bool operator==(const game_identity_t &) const = default;
  };
  [[nodiscard]] std::string game_identity(std::string_view profile, std::string_view target);
  [[nodiscard]] std::optional<game_identity_t> parse_game_identity(std::string_view identity);
  [[nodiscard]] std::optional<library_t> decode_library(std::string_view payload);
  // Reads only a validated profile volume using its immutable runtime image.
  // The helper has no network, devices, capabilities, or writable mounts.
  [[nodiscard]] library_t read_steam_library(container::host_t &host, const container::profile_t &profile);
  /** Read a Space's own library, whichever launcher family it belongs to. */
  [[nodiscard]] library_t read_profile_library(container::host_t &host, const container::profile_t &profile);
  /** Families whose library Polaris can read from a Space's home. */
  [[nodiscard]] bool has_library(runtime_profile_e profile);
  [[nodiscard]] std::string_view steam_library_scanner();
}
#endif
