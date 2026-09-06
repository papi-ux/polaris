/**
 * @file src/platform/linux/multiseat_moonlight_coordinator.h
 * @brief Default-off owner for multiseat Moonlight input dependencies.
 */
#pragma once

#ifdef __linux__

  #include "multiseat_moonlight_activation.h"

  #include <cstddef>
  #include <functional>
  #include <memory>
  #include <mutex>
  #include <optional>
  #include <vector>

namespace rtsp_stream {
  struct launch_session_t;
}

namespace multiseat::input {

  struct moonlight_session_coordinator_options_t {
    bool enabled = false;
  };

  using moonlight_input_backend_factory_t = std::function<
    std::unique_ptr<backend_t>(moonlight_controller_feedback_sink_t)
  >;

  enum class moonlight_coordinator_create_status_e {
    ready_disabled,
    ready_enabled,
    invalid_factory,
    backend_unavailable,
    activation_install_rejected,
  };

  enum class moonlight_coordinator_operation_status_e {
    applied,
    disabled,
    shutting_down,
    selection_retained,
  };

  struct moonlight_coordinator_reconcile_result_t {
    moonlight_coordinator_operation_status_e status =
      moonlight_coordinator_operation_status_e::disabled;
    reconciliation_report_t report;
  };

  struct moonlight_coordinator_prepare_result_t {
    moonlight_coordinator_operation_status_e status =
      moonlight_coordinator_operation_status_e::disabled;
    prepare_result_t input;
  };

  struct moonlight_coordinator_release_result_t {
    moonlight_coordinator_operation_status_e status =
      moonlight_coordinator_operation_status_e::disabled;
    status_e input_status = status_e::invalid_request;
  };

  enum class moonlight_coordinator_cancel_status_e {
    cancelled,
    already_cancelled,
    coordinator_disabled,
    coordinator_shutting_down,
    invalid_launch,
    launch_not_found,
  };

  enum class moonlight_coordinator_retire_status_e {
    retired,
    coordinator_disabled,
    coordinator_shutting_down,
    invalid_launch,
    launch_still_admissible,
    stream_still_bound,
    launch_not_found,
  };

  enum class moonlight_coordinator_shutdown_status_e {
    closed,
    already_closed,
    input_cleanup_incomplete,
  };

  struct moonlight_coordinator_shutdown_report_t {
    moonlight_coordinator_shutdown_status_e status =
      moonlight_coordinator_shutdown_status_e::input_cleanup_incomplete;
    std::size_t released_allocations = 0;
    std::size_t cleanup_failures = 0;
  };

  class moonlight_session_coordinator_t;

  struct moonlight_coordinator_create_result_t {
    moonlight_coordinator_create_status_e status =
      moonlight_coordinator_create_status_e::invalid_factory;
    std::optional<moonlight_activation_install_status_e> activation_status;
    std::unique_ptr<moonlight_session_coordinator_t> coordinator;
  };

  /**
   * Owns the complete host-input lifetime used by selected Moonlight launches.
   *
   * The backend factory receives the retained feedback hub's safe sink and
   * must return an inert backend. Its own referenced factory/probe dependencies
   * must outlive this coordinator. Default options do not install a gate and
   * every mutating operation remains disabled.
   *
   * shutdown() closes the activation gate first, then waits for selected
   * stream owners to detach from the registry before closing feedback and
   * releasing input allocations. The coordinator must therefore outlive every
   * selected stream, and callers must not invoke shutdown from a stream close
   * callback which is needed to release that same claim. Enabled owners must
   * observe a successful shutdown report before destruction; the destructor
   * performs one best-effort shutdown pass but cannot retry rejected cleanup.
   */
  class moonlight_session_coordinator_t final {
  public:
    static moonlight_coordinator_create_result_t create(
      moonlight_session_coordinator_options_t options,
      moonlight_input_backend_factory_t backend_factory
    );

    ~moonlight_session_coordinator_t();

    moonlight_session_coordinator_t(
      const moonlight_session_coordinator_t &
    ) = delete;
    moonlight_session_coordinator_t &operator=(
      const moonlight_session_coordinator_t &
    ) = delete;
    moonlight_session_coordinator_t(
      moonlight_session_coordinator_t &&
    ) = delete;
    moonlight_session_coordinator_t &operator=(
      moonlight_session_coordinator_t &&
    ) = delete;

    [[nodiscard]] moonlight_coordinator_reconcile_result_t reconcile_inputs(
      const std::vector<expectation_t> &expected
    );
    [[nodiscard]] moonlight_coordinator_prepare_result_t prepare_input(
      const expectation_t &expectation
    );
    [[nodiscard]] moonlight_coordinator_release_result_t release_input(
      const seat_handle_t &handle
    );

    [[nodiscard]] moonlight_launch_selection_status_e select_launch(
      const std::shared_ptr<rtsp_stream::launch_session_t> &launch,
      seat_handle_t handle,
      bool controller_feedback
    );
    [[nodiscard]] moonlight_coordinator_cancel_status_e cancel_launch(
      const std::shared_ptr<rtsp_stream::launch_session_t> &launch
    );

    /** Retire only after the RTSP launch object is atomically cancelled. */
    [[nodiscard]] moonlight_coordinator_retire_status_e
    retire_cancelled_launch(
      const std::shared_ptr<rtsp_stream::launch_session_t> &launch
    );

    /**
     * Synchronously quiesce and release owned dependencies.
     *
     * This waits for already-bound stream owners. A failed input teardown
     * leaves the closed gate installed and can be retried safely.
     */
    [[nodiscard]] moonlight_coordinator_shutdown_report_t shutdown() noexcept;

    [[nodiscard]] bool enabled() const;
    [[nodiscard]] bool activation_installed() const;
    [[nodiscard]] bool shutting_down() const;
    [[nodiscard]] bool closed() const;
    [[nodiscard]] std::size_t active_launches() const;
    [[nodiscard]] std::size_t retained_launches() const;
    [[nodiscard]] std::size_t input_allocations() const;
    [[nodiscard]] std::size_t registered_sessions() const;
    [[nodiscard]] std::size_t claimed_sessions() const;
    [[nodiscard]] std::size_t feedback_subscriptions() const;

  private:
    struct selection_owner_t {
      moonlight_launch_selection_key_t key;
      seat_handle_t handle;
      std::shared_ptr<rtsp_stream::launch_session_t> launch;
      bool cancelled = false;
      std::unique_ptr<moonlight_launch_selection_t> selection;
    };

    moonlight_session_coordinator_t(
      moonlight_session_coordinator_options_t options,
      std::shared_ptr<moonlight_controller_feedback_hub_t> feedback_hub,
      std::unique_ptr<backend_t> backend
    );

    [[nodiscard]] static std::optional<moonlight_launch_selection_key_t>
    launch_key(const rtsp_stream::launch_session_t &launch);
    [[nodiscard]] moonlight_coordinator_operation_status_e
    operation_status_locked() const;

    const moonlight_session_coordinator_options_t options_;
    // Declaration order is the ownership contract: reverse destruction keeps
    // the feedback hub and backend alive beyond authority and session state.
    const std::shared_ptr<moonlight_controller_feedback_hub_t> feedback_hub_;
    const std::unique_ptr<backend_t> backend_;
    authority_t authority_;
    moonlight_session_binding_registry_t binding_registry_;
    const std::shared_ptr<moonlight_session_activation_gate_t> activation_gate_;
    std::unique_ptr<moonlight_activation_installation_t>
      activation_installation_;
    mutable std::mutex state_mutex_;
    std::mutex shutdown_mutex_;
    std::vector<selection_owner_t> selections_;
    bool shutting_down_ = false;
    bool closed_ = false;
  };

}  // namespace multiseat::input

#endif
