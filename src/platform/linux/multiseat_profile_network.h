#pragma once
#ifdef __linux__
#include "multiseat_container_backend.h"

namespace multiseat::container {
  [[nodiscard]] bool valid_steam_target(std::string_view target);
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
