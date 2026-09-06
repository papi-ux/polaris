/**
 * @file src/platform/linux/multiseat_controller_production.h
 * @brief Default-off production dependency factory for multiseat control.
 */
#pragma once

#ifdef __linux__

  #include "multiseat_controller_runtime.h"
  #include "multiseat_podman_backend.h"

  #include <cstdint>
  #include <filesystem>
  #include <functional>
  #include <memory>
  #include <optional>
  #include <string>
  #include <vector>

namespace multiseat {

  /**
   * One trusted GPU catalog entry. The factory derives both registry capacity
   * and the Podman device allowlist from this single value.
   */
  struct production_controller_gpu_t {
    std::string logical_gpu_id;
    std::filesystem::path render_node;
    std::vector<std::filesystem::path> devices;
    std::uint32_t max_seats = 1;
    std::uint32_t max_encoder_sessions = 1;
  };

  struct production_controller_options_t {
    bool enabled = false;
    std::vector<production_controller_gpu_t> gpus;
    /**
     * The caller must leave podman.gpus empty. It is derived from gpus above,
     * and podman.ipc_root is also the pre-created mode-0700 authority root.
     */
    podman::options_t podman;
    worker_coordinator_options_t worker;
  };

  using production_controller_epoch_factory_t =
    std::function<std::optional<std::string>()>;
  using production_controller_moonlight_factory_t =
    std::function<input::moonlight_runtime_create_result_t()>;
  using production_controller_podman_host_factory_t =
    std::function<std::unique_ptr<podman::host_t>()>;
  using production_controller_kernel_probe_factory_t =
    std::function<std::unique_ptr<input::kernel_node_probe_t>()>;

  /**
   * Injectable construction boundaries for deterministic offline tests.
   *
   * Empty functions select the real UUID, inputtino, local Podman host, Linux
   * kernel probe, and native worker transport implementations. Every returned
   * dependency must remain inert until the controller explicitly reconciles or
   * starts a seat.
   */
  struct production_controller_factories_t {
    production_controller_epoch_factory_t controller_epoch;
    production_controller_moonlight_factory_t moonlight_runtime;
    production_controller_podman_host_factory_t podman_host;
    production_controller_kernel_probe_factory_t kernel_probe;
    worker_control_session_factory_t worker_session;
  };

  /**
   * Compose concrete production dependencies behind the trusted owner.
   *
   * Disabled options return before validating the catalog or invoking any
   * factory. This function has no production caller yet.
   */
  [[nodiscard]] controller_runtime_create_result_t
  create_production_controller_runtime(
    production_controller_options_t options,
    production_controller_factories_t factories = {}
  );

}  // namespace multiseat

#endif
