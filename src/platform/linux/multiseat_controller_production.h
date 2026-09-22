/**
 * @file src/platform/linux/multiseat_controller_production.h
 * @brief Default-off production dependency factory for multiseat control.
 */
#pragma once

#ifdef __linux__

  #include "multiseat_controller_runtime.h"
  #include "multiseat_container_backend.h"

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
   * and the container device allowlist from this single value. Every path and
   * character-device identity in devices is exclusive to this logical GPU;
   * the render node must appear in that list. Shared global devices are not
   * represented here and require a separate budget-neutral owner.
   */
  struct production_controller_gpu_t {
    std::string logical_gpu_id;
    std::filesystem::path render_node;
    std::vector<std::filesystem::path> devices;
    std::uint32_t max_seats = 1;
    std::uint32_t max_encoder_sessions = 1;
  };

  struct production_controller_profile_route_t {
    std::string profile_key;
    std::vector<std::string> client_keys;
    workload_plan_t workload;
    std::vector<std::string> access_clients;
  };

  struct production_controller_options_t {
    bool enabled = false;
    // Optional saved Docker catalog. Mutually exclusive with inline profiles,
    // workloads and routes. Its private file lease outlives all active seats.
    std::filesystem::path profile_catalog;
    std::vector<production_controller_gpu_t> gpus;
    /**
     * The caller must leave container.gpus empty. It is derived from gpus above,
     * and container.ipc_root is also the pre-created mode-0700 authority root.
     */
    container::options_t container;
    worker_coordinator_options_t worker;
    /** Image family and GPU order are derived from the container/GPU catalogs. */
    std::vector<production_controller_profile_route_t> profile_routes;
    /**
     * True for an image built without driver libraries of its own, which is
     * what decides whether a Space gets the host's. Supplied by the launch
     * service from the compiled catalog, never persisted with a Space.
     */
    std::function<bool(std::string_view)> host_driver_image;
  };

  using production_controller_epoch_factory_t =
    std::function<std::optional<std::string>()>;
  using production_controller_moonlight_factory_t =
    std::function<input::moonlight_runtime_create_result_t()>;
  using production_controller_container_host_factory_t =
    std::function<std::unique_ptr<container::host_t>()>;
  using production_controller_kernel_probe_factory_t =
    std::function<std::unique_ptr<input::kernel_node_probe_t>()>;

  /**
   * Injectable construction boundaries for deterministic offline tests.
   *
   * Empty functions select the real UUID, inputtino, local container host, Linux
   * kernel probe, and native worker transport implementations. Every returned
   * dependency must remain inert until the controller explicitly reconciles or
   * starts a seat.
   */
  struct production_controller_factories_t {
    production_controller_epoch_factory_t controller_epoch;
    production_controller_moonlight_factory_t moonlight_runtime;
    production_controller_container_host_factory_t container_host;
    production_controller_kernel_probe_factory_t kernel_probe;
    worker_control_session_factory_t worker_session;
  };

  /**
   * Compose concrete production dependencies behind the trusted owner.
   *
   * Disabled options, or an empty profile catalog with no routes, return before
   * validating GPUs or invoking any factory. This function has no production
   * caller yet.
   */
  [[nodiscard]] controller_runtime_create_result_t
  create_production_controller_runtime(
    production_controller_options_t options,
    production_controller_factories_t factories = {}
  );

}  // namespace multiseat

#endif
