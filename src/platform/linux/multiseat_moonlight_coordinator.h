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
  #include <optional>
  #include <string_view>
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
    streams_pending,
    input_cleanup_incomplete,
  };

  enum class moonlight_coordinator_quiesce_status_e {
    quiesced,
    streams_pending,
    already_closed,
  };

  struct moonlight_coordinator_quiesce_report_t {
    moonlight_coordinator_quiesce_status_e status =
      moonlight_coordinator_quiesce_status_e::streams_pending;
    std::size_t claimed_sessions = 0;
    std::size_t activations_in_flight = 0;
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
   * quiesce() atomically rejects new registry claims and activation without
   * waiting for an activation or stream already in flight. shutdown() returns
   * streams_pending until those owners detach, then closes feedback and
   * releases input allocations. Enabled owners must observe a successful
   * shutdown report before destruction. As a last-resort safety fence, the
   * destructor uninstalls the process-global gate and deliberately retains the
   * complete dependency graph whenever a stream, activation, or input cleanup
   * is still pending.
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
      std::string_view expected_input_seat,
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

    /** Close new selection/claims and report existing owners without waiting. */
    [[nodiscard]] moonlight_coordinator_quiesce_report_t quiesce() noexcept;

    /** Release dependencies only after quiesce reports no stream owner. */
    [[nodiscard]] moonlight_coordinator_shutdown_report_t shutdown() noexcept;

    [[nodiscard]] bool enabled() const;
    [[nodiscard]] bool activation_installed() const;
    [[nodiscard]] bool shutting_down() const;
    [[nodiscard]] bool closed() const;
    [[nodiscard]] std::size_t active_launches() const;
    [[nodiscard]] std::size_t retained_launches() const;
    [[nodiscard]] std::size_t input_allocations() const;
    /**
     * Exact live allocation for a seat. Stays readable through quiesce and a
     * pending shutdown so worker inventory can prove absence; empty when
     * disabled, after release, or once shutdown has closed.
     */
    [[nodiscard]] std::optional<allocation_t> input_allocation(
      const seat_handle_t &handle
    ) const;
    [[nodiscard]] std::size_t registered_sessions() const;
    [[nodiscard]] std::size_t claimed_sessions() const;
    [[nodiscard]] std::size_t feedback_subscriptions() const;

  private:
    struct impl_t;

    moonlight_session_coordinator_t(
      moonlight_session_coordinator_options_t options,
      std::shared_ptr<moonlight_controller_feedback_hub_t> feedback_hub,
      std::unique_ptr<backend_t> backend
    );

    [[nodiscard]] static std::optional<moonlight_launch_selection_key_t>
    launch_key(const rtsp_stream::launch_session_t &launch);
    [[nodiscard]] moonlight_coordinator_operation_status_e
    operation_status_locked() const;
    [[nodiscard]] moonlight_coordinator_quiesce_report_t
    quiesce_locked() noexcept;

    std::unique_ptr<impl_t> impl_;
  };

}  // namespace multiseat::input

#endif
