#pragma once
#ifdef __linux__
#include "multiseat_container_backend.h"

namespace multiseat::container {
  [[nodiscard]] bool valid_steam_target(std::string_view target);
  /**
   * What a launcher family accepts as a target. Every family has a sentinel that
   * opens the launcher itself, and a grammar for one title. The worker rebuilds
   * the launcher's own URI from a validated token and never receives a URI, a
   * path or a command.
   *
   *   steam   big-picture-v1 | <decimal appid>
   *   heroic  library-v1     | <runner>.<appName>, runner in epic gog amazon sideload
   *   lutris  library-v1     | id.<decimal>
   */
  [[nodiscard]] bool valid_launcher_target(runtime_profile_e profile, std::string_view target);
  /** The sentinel that opens a family's launcher rather than a title. */
  [[nodiscard]] std::string_view launcher_sentinel(runtime_profile_e profile);
  /**
   * Whether any launcher family would accept this target. A game identity
   * carries the Space but not its family, so encoding and parsing one asks
   * this; which family may use which grammar is settled where a Space is
   * launched, against the Space's own family.
   */
  [[nodiscard]] bool any_launcher_target(std::string_view target);
  /**
   * Whether a family needs the private bridge and the launcher sandbox policy.
   * Every concrete launcher does: they all sign in, download and run games.
   * Only the image's own test workload does not.
   */
  [[nodiscard]] bool needs_profile_network(runtime_profile_e profile);
  [[nodiscard]] bool supported_streaming_workload(runtime_profile_e profile, const workload_plan_t &workload);
  [[nodiscard]] std::string profile_network_name(std::string_view profile_key);
  // Persistent profile bridge with exact local policy and ownership label.
  [[nodiscard]] std::optional<std::string> profile_network_id(host_t &host, std::string_view profile_key,
    bool require_empty = false, std::string_view only_container = {});
  // Prove absence before creation, then verify the immutable ID. Failure may
  // retain a network; callers must report its opaque resource name.
  [[nodiscard]] bool create_profile_network(host_t &host, std::string_view profile_key);
}
#endif
