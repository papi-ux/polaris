/** Verified Docker runtime acquisition. No containers or player storage. */
#pragma once
#ifdef __linux__
#include "multiseat_container_host.h"
#include <nlohmann/json.hpp>

namespace multiseat::spaces {
  struct runtime_t {
    std::string id, variant, source_revision, registry_digest, config_digest, nvidia_driver;
    [[nodiscard]] std::string reference() const;
  };
  // The production caller only consumes the catalog compiled into Polaris.
  // Parsing is exposed to exercise rejection and compatibility in unit tests.
  [[nodiscard]] std::optional<std::vector<runtime_t>> decode_runtime_catalog(std::string_view payload);
  [[nodiscard]] const std::optional<std::vector<runtime_t>> &trusted_runtimes();
  [[nodiscard]] bool matches_runtime_image(const runtime_t &runtime, std::string_view inspection);
  struct runtime_install_result_t {
    bool ready = false;
    std::string code, message, image;
  };
  // Always re-inspects the exact digest. Retrying after interruption can reuse
  // Docker's verified layers; a failed pull never starts or activates anything.
  [[nodiscard]] runtime_install_result_t install_runtime(container::host_t &host,
    std::string_view id, const std::vector<runtime_t> &catalog);
  int runtime_command(int argc, char **argv);
}
#endif
