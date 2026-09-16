/** Read-only host prerequisites for the Spaces setup flow. */
#pragma once
#ifdef __linux__
#include "multiseat_container_host.h"
#include "spaces_security.h"
#include <nlohmann/json.hpp>

namespace multiseat::spaces {
  struct setup_facts_t {
    std::string distribution;
    bool immutable_host = false;
    std::uint64_t uid = 0, gid = 0;
    bool docker_cli = false, runc = false, daemon_replied = false;
    bool daemon_linux = false, daemon_rootless = false, daemon_runc = false;
    bool input_access = false, gpu_access = false;
    security_status_t security;
    bool controller_enabled = false, controller_available = false;
  };
  // Pure presentation contract. Availability means a particular prerequisite
  // passed, never proof of game streaming or an audible assessment.
  [[nodiscard]] nlohmann::json describe_setup(const setup_facts_t &facts);
  [[nodiscard]] nlohmann::json inspect_setup(container::host_t &host,
    bool enabled, bool available,
    const std::optional<security_facts_t> &security = std::nullopt);
}
#endif
