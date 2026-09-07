/**
 * @file src/platform/linux/multiseat_moonlight_runtime.h
 * @brief Explicitly configured owner for Moonlight multiseat input.
 */
#pragma once

#ifdef __linux__

  #include "multiseat_moonlight_coordinator.h"

  #include <cstddef>
  #include <cstdint>
  #include <memory>
  #include <optional>
  #include <string_view>
  #include <vector>

namespace multiseat::input {

  struct moonlight_session_runtime_options_t {
    bool enabled = false;
  };

  enum class moonlight_runtime_create_status_e {
    ready_disabled,
    ready_enabled,
    invalid_factory,
    backend_unavailable,
    activation_install_rejected,
    runtime_install_rejected,
  };

  enum class moonlight_runtime_lifecycle_status_e {
    retired,
    retained_until_streams_close,
    not_selected,
    runtime_unavailable,
    runtime_shutting_down,
  };

  class moonlight_session_runtime_t;

  struct moonlight_runtime_create_result_t {
    moonlight_runtime_create_status_e status =
      moonlight_runtime_create_status_e::backend_unavailable;
    std::unique_ptr<moonlight_session_runtime_t> runtime;
  };

  /**
   * Process owner for the opt-in Moonlight multiseat input boundary.
   *
   * Disabled creation returns no owner and never invokes the backend factory.
   * An enabled owner installs both the existing activation gate and one
   * lifecycle target used by RTSP timeout/abort and selected-stream teardown.
   * The opaque dependency owner, when supplied, outlives the coordinator and
   * its backend. Shutdown is retryable: its quiesce phase returns immediately
   * while an activation or claimed stream remains. The exact-generation
   * allocation view stays readable through quiesce and a pending shutdown so
   * authoritative worker inventory can still prove absence; it returns no
   * authority after release or once shutdown has closed. The destructor is a
   * last-resort fence: after an incomplete shutdown it detaches the runtime
   * lifecycle target and the coordinator's activation entry, then retains the
   * complete graph rather than destroying authority beneath a bound stream.
   */
  class moonlight_session_runtime_t final {
  public:
    static moonlight_runtime_create_result_t create(
      moonlight_session_runtime_options_t options,
      moonlight_input_backend_factory_t backend_factory,
      std::shared_ptr<void> backend_dependencies = {}
    );

    ~moonlight_session_runtime_t();

    moonlight_session_runtime_t(const moonlight_session_runtime_t &) = delete;
    moonlight_session_runtime_t &operator=(
      const moonlight_session_runtime_t &
    ) = delete;
    moonlight_session_runtime_t(moonlight_session_runtime_t &&) = delete;
    moonlight_session_runtime_t &operator=(
      moonlight_session_runtime_t &&
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

    /**
     * Stage one exact launch only after the caller has authenticated it and
     * supplied the input-seat name from that same worker authority.
     * This must run after all request validation and immediately before the
     * common RTSP raise. A rejected raise is retired by that common boundary.
     */
    [[nodiscard]] moonlight_launch_selection_status_e
    select_authenticated_launch(
      const std::shared_ptr<rtsp_stream::launch_session_t> &launch,
      seat_handle_t handle,
      std::string_view expected_input_seat,
      bool controller_feedback
    );

    [[nodiscard]] moonlight_runtime_lifecycle_status_e cancel_launch(
      const std::shared_ptr<rtsp_stream::launch_session_t> &launch
    );
    [[nodiscard]] moonlight_runtime_lifecycle_status_e finish_stream(
      moonlight_launch_selection_key_t key
    );

    /** Close new launch selection and activation without waiting for streams. */
    [[nodiscard]] moonlight_coordinator_quiesce_report_t quiesce() noexcept;
    [[nodiscard]] moonlight_coordinator_shutdown_report_t shutdown() noexcept;

    /**
     * Remove the runtime lifecycle target and the coordinator's activation
     * entry without destroying either, and refuse further launch selection.
     * A direct owner calls this after an incomplete shutdown and then retains
     * the complete graph, so bound streams keep valid authority while new
     * sessions take the ordinary path.
     */
    void detach_process_globals() noexcept;

    [[nodiscard]] bool installed() const;
    [[nodiscard]] bool shutting_down() const;
    [[nodiscard]] bool closed() const;
    [[nodiscard]] std::size_t tracked_launches() const;
    [[nodiscard]] std::size_t active_launches() const;
    [[nodiscard]] std::size_t retained_launches() const;
    [[nodiscard]] std::size_t input_allocations() const;
    /**
     * Read-only exact-generation manifest authority for a trusted worker
     * backend. Returns no allocation after shutdown begins.
     */
    [[nodiscard]] std::optional<allocation_t> input_allocation(
      const seat_handle_t &handle
    ) const;
    [[nodiscard]] std::size_t claimed_sessions() const;

  private:
    struct impl_t;

    explicit moonlight_session_runtime_t(std::unique_ptr<impl_t> impl);
    [[nodiscard]] moonlight_coordinator_quiesce_report_t
    quiesce_locked() noexcept;

    std::unique_ptr<impl_t> impl_;
  };

  /** Production inputtino owner. Disabled options remain allocation-free. */
  [[nodiscard]] moonlight_runtime_create_result_t
  create_production_moonlight_session_runtime(
    moonlight_session_runtime_options_t options
  );

  /**
   * Narrow process-global seam for a future authenticated seat authority.
   * No network request can supply a seat handle or input-seat name through
   * this API directly.
   */
  [[nodiscard]] std::optional<moonlight_launch_selection_status_e>
  select_authenticated_moonlight_launch(
    const std::shared_ptr<rtsp_stream::launch_session_t> &launch,
    seat_handle_t handle,
    std::string_view expected_input_seat,
    bool controller_feedback
  );

  /** Common RTSP rejection, timeout, and shutdown notification. */
  [[nodiscard]] moonlight_runtime_lifecycle_status_e
  cancel_registered_moonlight_launch(
    const std::shared_ptr<rtsp_stream::launch_session_t> &launch
  );

  /** Selected stream notification after its live input owner has closed. */
  [[nodiscard]] moonlight_runtime_lifecycle_status_e
  finish_registered_moonlight_stream(
    moonlight_launch_selection_key_t key
  );

  [[nodiscard]] bool moonlight_session_runtime_installed();

}  // namespace multiseat::input

#endif
