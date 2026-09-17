/** Read-only host prerequisites for the Spaces setup flow. */
#pragma once
#ifdef __linux__
#include "multiseat_container_host.h"
#include "spaces_runtime.h"
#include "spaces_security.h"
#include <chrono>
#include <functional>
#include <mutex>
#include <nlohmann/json.hpp>

namespace multiseat::spaces {
  // The catalog entry this PC needs: the NVIDIA runtime built for the loaded
  // NVIDIA driver, otherwise the default runtime for AMD and Intel graphics.
  // First-Space setup pairs variants and graphics cards the same way.
  struct runtime_choice_t {
    std::optional<runtime_t> runtime;
    // Why no entry fits: runtime_not_published, driver_mismatch or graphics_unsupported.
    std::string code;
  };
  // nvidia_driver is empty when no NVIDIA driver is loaded, and an empty
  // string when one is loaded but its version could not be read.
  [[nodiscard]] runtime_choice_t choose_runtime(const std::vector<runtime_t> &catalog,
    const std::optional<std::string> &nvidia_driver);

  // What Docker holds under the approved reference.
  enum class runtime_image_e { absent, verified, mismatch, unverifiable };

  // Reading the setup page must not ask Docker about the runtime every time.
  // Only definitive answers are kept, briefly; a finished or stopped download
  // forgets them so the next check asks again.
  class runtime_inspection_cache_t {
  public:
    using now_t = std::function<std::chrono::steady_clock::time_point()>;
    explicit runtime_inspection_cache_t(std::chrono::steady_clock::duration lifetime = std::chrono::seconds(15),
      now_t now = {});
    runtime_image_e remember(const std::string &reference, const std::function<runtime_image_e()> &inspect);
    void forget();

  private:
    struct entry_t {
      std::string reference;
      runtime_image_e image;
      std::chrono::steady_clock::time_point checked;
    };
    std::chrono::steady_clock::duration lifetime_;
    now_t now_;
    std::mutex mutex_;
    std::optional<entry_t> entry_;
    std::uint64_t generation_ = 0;
  };
  [[nodiscard]] runtime_inspection_cache_t &runtime_inspection_cache();

  struct runtime_facts_t {
    // not_published, unsupported, available, ready or failed.
    std::string status = "not_published";
    std::string code = "runtime_not_published";
    std::optional<runtime_t> runtime;
    std::optional<std::string> host_nvidia_driver;
    std::vector<std::string> nvidia_drivers;  ///< drivers the catalog's NVIDIA runtimes need
  };
  // Bounded: one local `docker image inspect` of the pinned reference, only
  // when the engine answered, verified against the compiled catalog entry.
  [[nodiscard]] runtime_facts_t inspect_runtime(container::host_t &host,
    const std::vector<runtime_t> &catalog, const std::optional<std::string> &nvidia_driver,
    bool engine_ready, runtime_inspection_cache_t *cache = nullptr);

  struct setup_facts_t {
    std::string distribution;
    bool immutable_host = false;
    std::uint64_t uid = 0, gid = 0;
    bool docker_cli = false, runc = false, daemon_replied = false;
    bool daemon_linux = false, daemon_rootless = false, daemon_runc = false;
    bool docker_access_pending = false;  ///< the account is in the docker group, but this Polaris started before it was
    bool input_access = false, gpu_access = false;
    security_status_t security;
    runtime_facts_t runtime;
    bool controller_enabled = false, controller_available = false;
  };
  /// The account database lists this account in the docker group, but this process does not carry the
  /// group, because it started before the change. Only starting again picks the group up.
  [[nodiscard]] bool docker_access_pending(const std::optional<container::group_membership_t> &docker,
    std::uint64_t effective_gid, const std::optional<std::vector<std::uint64_t>> &groups);
  // Pure presentation contract. Availability means a particular prerequisite
  // passed, never proof of game streaming or an audible assessment. A check an
  // administrator can fix from Polaris names that fix as host_action.
  [[nodiscard]] nlohmann::json describe_setup(const setup_facts_t &facts);
  [[nodiscard]] nlohmann::json inspect_setup(container::host_t &host,
    bool enabled, bool available,
    const std::optional<security_facts_t> &security = std::nullopt);
}
#endif
