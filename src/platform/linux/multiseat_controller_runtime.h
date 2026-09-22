/**
 * @file src/platform/linux/multiseat_controller_runtime.h
 * @brief Default-off owner for trusted multiseat launch composition.
 */
#pragma once

#ifdef __linux__
#include "spaces_library.h"

  #include "multiseat_moonlight_worker_adapter.h"
  #include "multiseat_worker_coordinator.h"

  #include <cstddef>
  #include <chrono>
  #include <functional>
  #include <memory>
  #include <optional>
  #include <string>
  #include <vector>

namespace multiseat {

  struct profile_summary_t {
    std::string id;
    std::string name;
    std::vector<std::string> clients;
    /// The launcher family this Space runs, empty when its workload is not one
    /// this build can stream: "steam", "heroic" or "lutris".
    std::string family;
    bool archived = false;
    std::vector<std::string> access_clients;
    bool library_enabled = false;
    /// The local image ID the Space launches, exactly as its catalog entry pins it.
    std::string image;
  };

  struct profile_activity_t {
    std::string profile, client, state;
  };

  /** Immutable operator routing for one persistent profile in this epoch. */
  struct controller_profile_route_t {
    std::string profile_key;
    std::vector<std::string> client_keys;
    runtime_profile_e runtime_profile = runtime_profile_e::unknown;
    workload_plan_t workload;
    std::vector<std::string> logical_gpu_ids;
    std::vector<std::string> access_clients;
    bool library_enabled = false;
  };

  struct controller_runtime_options_t {
    bool enabled = false;
    /** Selected launches require their exact worker's authenticated media lease. */
    bool worker_media_enabled = false;
    std::vector<controller_profile_route_t> profile_routes;
    std::vector<profile_summary_t> profile_catalog;
    /** Deadline from profile reservation through successful RTSP setup. */
    std::chrono::milliseconds profile_launch_timeout {30000};
    spaces::library_reader_t library_reader;
    std::vector<std::string> desktop_clients;
    /** Devices whose Default Space is Desktop. */
    std::vector<std::string> desktop_default_clients;
  };

  /**
   * Move-only dependencies produced only after the controller is enabled.
   *
   * Every backend must be inert at construction. The optional opaque owner
   * outlives the worker backend, allowing the container adapter to retain its
   * input-manifest and kernel-probe dependencies without exposing them here.
   */
  struct controller_runtime_dependencies_t {
    std::shared_ptr<void> profile_catalog_lease;
    std::unique_ptr<registry_t> registry;
    std::unique_ptr<worker_ipc::authority_store_t> worker_authority_store;
    std::unique_ptr<input::moonlight_session_runtime_t> moonlight_runtime;
    std::shared_ptr<void> worker_backend_dependencies;
    std::unique_ptr<worker_backend_t> worker_backend;
    std::vector<input::expectation_t> recovered_input_expectations;
    worker_coordinator_options_t worker_options;
    worker_broker_t::now_fn_t now;
    worker_control_session_factory_t session_factory;
  };

  using controller_runtime_dependencies_factory_t = std::function<
    std::optional<controller_runtime_dependencies_t>()
  >;

  enum class controller_runtime_create_status_e {
    ready_disabled,
    ready_enabled,
    invalid_factory,
    dependencies_unavailable,
    invalid_dependencies,
    construction_failed,
  };

  enum class controller_reconcile_status_e {
    ready,
    controller_shutting_down,
    invalid_input_expectations,
    worker_reconciliation_required,
    input_reconciliation_required,
  };

  struct controller_reconcile_result_t {
    controller_reconcile_status_e status =
      controller_reconcile_status_e::worker_reconciliation_required;
    coordinator_reconciliation_report_t worker;
    std::optional<input::moonlight_coordinator_reconcile_result_t> input;

    [[nodiscard]] bool ready() const {
      return status == controller_reconcile_status_e::ready &&
             worker.admission_ready && input &&
             input->status ==
               input::moonlight_coordinator_operation_status_e::applied &&
             input->report.admission_ready;
    }
  };

  enum class controller_start_status_e {
    started,
    controller_not_ready,
    invalid_request,
    input_rejected,
    worker_rejected,
    worker_indeterminate,
    input_cleanup_incomplete,
    launch_cancelled,
  };

  enum class controller_profile_admission_status_e {
    unselected,
    invalid_launch,
    controller_not_ready,
    admitted,
    rejected,
  };

  struct controller_profile_admission_result_t {
    controller_profile_admission_status_e status =
      controller_profile_admission_status_e::invalid_launch;
    admission_result_t admission;

    /** Only unselected permits the caller to continue ordinary host launch. */
    [[nodiscard]] bool use_host_launch() const {
      return status == controller_profile_admission_status_e::unselected;
    }
    [[nodiscard]] bool admitted() const {
      return status == controller_profile_admission_status_e::admitted &&
             admission.accepted();
    }
  };

  struct controller_start_result_t {
    controller_start_status_e status =
      controller_start_status_e::invalid_request;
    input::moonlight_coordinator_prepare_result_t input;
    std::optional<coordinator_start_result_e> worker;
    std::optional<input::moonlight_coordinator_release_result_t> rollback;

    [[nodiscard]] bool started() const {
      return status == controller_start_status_e::started &&
             worker == coordinator_start_result_e::started &&
             input.status ==
               input::moonlight_coordinator_operation_status_e::applied &&
             input.input.prepared();
    }
  };

  enum class controller_stop_status_e {
    stopping,
    released,
    cleanup_pending,
    invalid_request,
    controller_shutting_down,
    streams_pending,
  };

  struct controller_stop_result_t {
    controller_stop_status_e status =
      controller_stop_status_e::invalid_request;
    coordinator_stop_result_t worker;
    std::optional<input::moonlight_coordinator_release_result_t> input;
  };

  enum class controller_shutdown_status_e {
    closed,
    already_closed,
    workers_pending,
    streams_pending,
    input_cleanup_incomplete,
  };

  struct controller_shutdown_report_t {
    controller_shutdown_status_e status =
      controller_shutdown_status_e::workers_pending;
    std::size_t stop_requests = 0;
    std::optional<coordinator_reconciliation_report_t> worker;
    std::optional<input::moonlight_coordinator_shutdown_report_t> input;

    [[nodiscard]] bool closed() const {
      return status == controller_shutdown_status_e::closed ||
             status == controller_shutdown_status_e::already_closed;
    }
  };

  class controller_runtime_t;

  struct controller_runtime_create_result_t {
    controller_runtime_create_status_e status =
      controller_runtime_create_status_e::dependencies_unavailable;
    std::unique_ptr<controller_runtime_t> runtime;
  };

  /**
   * Trusted composition owner for one controller epoch.
   *
   * Disabled creation never invokes the dependency factory. Enabled creation
   * owns the seat registry, worker authority/backend/coordinator, Moonlight
   * input runtime, and authenticated launch adapter in dependency-safe order.
   * It is not a request handler: callers may supply only typed seat requests,
   * exact handles returned by this owner, and retained authenticated RTSP
   * launch objects.
   *
   * Input is prepared before worker launch. Indeterminate worker ownership
   * retains input until authoritative reconciliation proves absence. shutdown
   * first quiesces new Moonlight activation without waiting, refuses to begin
   * worker teardown while an activation or selected stream may still own the
   * seat, and then closes input only after worker absence is authoritative.
   * Callers must retain the owner and retry an incomplete shutdown; as a last
   * resort the destructor detaches the process-global Moonlight entry points
   * and retains the closed-over dependencies rather than destroying authority
   * beneath a live worker or bound stream.
   */
  class controller_runtime_t final {
  public:
    static controller_runtime_create_result_t create(
      controller_runtime_options_t options,
      controller_runtime_dependencies_factory_t dependencies_factory
    );

    ~controller_runtime_t();

    controller_runtime_t(const controller_runtime_t &) = delete;
    controller_runtime_t &operator=(const controller_runtime_t &) = delete;
    controller_runtime_t(controller_runtime_t &&) = delete;
    controller_runtime_t &operator=(controller_runtime_t &&) = delete;

    [[nodiscard]] controller_reconcile_result_t reconcile();
    [[nodiscard]] admission_result_t admit(const seat_request_t &request);
    /**
     * Resolve only the paired UUID retained by an authenticated pending launch.
     * Profile, workload, image family and GPU order come from immutable trusted
     * routing. The controller retains ownership until reconciliation proves
     * cleanup. Cancellation, abandonment and setup timeout queue automatic
     * teardown; callers must keep polling reconcile(). No worker starts here.
     */
    [[nodiscard]] controller_profile_admission_result_t
    admit_authenticated_profile_launch(
      const std::shared_ptr<rtsp_stream::launch_session_t> &launch,
      seat_display_mode_t display_mode
    );
    [[nodiscard]] mutation_result_e bind_runtime(
      const seat_handle_t &handle,
      compositor_e selected,
      std::string selection_reason
    );
    [[nodiscard]] controller_start_result_t start_seat(
      const seat_handle_t &handle,
      input::plan_t input_plan
    );
    [[nodiscard]] input::moonlight_worker_selection_result_t
    select_authenticated_launch(
      const std::shared_ptr<rtsp_stream::launch_session_t> &launch,
      const seat_handle_t &handle
    );
    [[nodiscard]] controller_stop_result_t stop_seat(
      const seat_handle_t &handle
    );
    [[nodiscard]] controller_shutdown_report_t shutdown() noexcept;

    [[nodiscard]] bool admission_ready() const;
    [[nodiscard]] bool routes_client(std::string_view client_key) const;
    [[nodiscard]] std::optional<std::string> profile_for_client(std::string_view client_key) const;
    [[nodiscard]] std::vector<profile_summary_t> profile_catalog() const;
    [[nodiscard]] spaces::library_reader_t library_reader() const;
    [[nodiscard]] std::vector<std::string> desktop_clients() const;
    [[nodiscard]] std::vector<std::string> desktop_default_clients() const;
    [[nodiscard]] std::vector<profile_activity_t> profile_activity() const;
    [[nodiscard]] std::optional<seat_state_e> seat_state(const seat_handle_t &handle) const;
    [[nodiscard]] bool shutting_down() const;
    [[nodiscard]] bool closed() const;
    [[nodiscard]] std::size_t seats() const;
    [[nodiscard]] gpu_usage_t capacity() const;
    [[nodiscard]] std::size_t managed_workers() const;
    [[nodiscard]] std::size_t input_allocations() const;
    [[nodiscard]] std::size_t tracked_launches() const;
    [[nodiscard]] std::size_t owned_profile_launches() const;

  private:
    struct impl_t;

    explicit controller_runtime_t(std::unique_ptr<impl_t> impl);
    [[nodiscard]] controller_start_result_t start_seat_locked(
      const seat_handle_t &handle, input::plan_t input_plan
    );

    std::unique_ptr<impl_t> impl_;
  };

}  // namespace multiseat

#endif
