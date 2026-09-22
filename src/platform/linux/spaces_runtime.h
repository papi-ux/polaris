/** Verified Docker runtime acquisition. No containers or player storage. */
#pragma once
#ifdef __linux__
#include "multiseat_container_host.h"
#include <nlohmann/json.hpp>

namespace multiseat::spaces {
  struct runtime_t {
    std::string id, variant, source_revision, registry_digest, config_digest, nvidia_driver;
    /**
     * Set only for the nvidia-host variant, which carries no driver of its own
     * and borrows the machine's. The floor exists because the image's own NVENC
     * and CUDA consumers are built against a pinned API.
     */
    std::string nvidia_minimum_driver;
    // What the Space's home has to agree with. The catalog admits the three
    // launcher families, contract 1 and 1000:1000, and these defaults are the
    // family every Space had before there was more than one.
    std::string profile = "steam";
    std::string media_contract = "1";
    std::uint32_t uid = 1000, gid = 1000;
    [[nodiscard]] std::string reference() const;
    [[nodiscard]] bool matches_image_id(std::string_view image) const;
  };
  /**
   * The launcher families a runtime may be built for. Each carries its own
   * launcher in its own image, so a catalog entry names one family and the
   * image is pulled from that family's repository.
   */
  [[nodiscard]] bool admitted_runtime_profile(std::string_view value);

  // The production caller only consumes the catalog compiled into Polaris.
  // Parsing is exposed to exercise rejection and compatibility in unit tests.
  [[nodiscard]] std::optional<std::vector<runtime_t>> decode_runtime_catalog(std::string_view payload);
  [[nodiscard]] const std::optional<std::vector<runtime_t>> &trusted_runtimes();
  [[nodiscard]] bool matches_runtime_image(const runtime_t &runtime, std::string_view inspection);
  /**
   * True when a Space's image is a catalog runtime that carries no driver of
   * its own. A Space names the image it launches by its own identity, which is
   * the config digest Docker reports, never the reference it was pulled by.
   */
  [[nodiscard]] bool borrows_host_driver(std::string_view image, const std::vector<runtime_t> &catalog);

  /**
   * The launcher family of the catalog runtime this image is, or empty when no
   * entry claims it. A first Space takes its family from here, never from a
   * name on the wire.
   */
  [[nodiscard]] std::string_view runtime_profile_for_image(
    std::string_view image, const std::vector<runtime_t> &catalog);
  // Docker's classic store addresses configurations; its containerd store
  // addresses manifests. Both identities must belong to the compiled catalog.
  [[nodiscard]] std::optional<std::string> verified_runtime_image(const runtime_t &runtime, std::string_view inspection);
  struct runtime_install_result_t {
    bool ready = false;
    std::string code, message, image;
  };
  // Always re-inspects the exact digest. Retrying after interruption can reuse
  // Docker's verified layers; a failed pull never starts or activates anything.
  [[nodiscard]] runtime_install_result_t install_runtime(container::host_t &host,
    std::string_view id, const std::vector<runtime_t> &catalog, std::stop_token stop = {});
  int runtime_command(int argc, char **argv);
}
#endif
