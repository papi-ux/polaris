/**
 * @file src/multiseat_worker_broker.h
 * @brief Backend-neutral worker lifecycle and reconciliation contract.
 */
#pragma once

#include "multiseat_runtime.h"

#include <chrono>
#include <cstddef>
#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace multiseat {

  /**
   * The worker drains seven components serially with a five-second bound for
   * each. The controller allows that complete 35-second reverse teardown plus
   * the default five-second authenticated-shutdown I/O budget and five-second
   * backend command budget before it force-removes the exact generation.
   */
  inline constexpr std::size_t worker_runtime_component_count = 7;
  inline constexpr std::chrono::milliseconds worker_runtime_component_stop_timeout {5000};
  inline constexpr std::chrono::milliseconds worker_runtime_graceful_stop_margin {10000};
  inline constexpr std::chrono::milliseconds worker_runtime_graceful_stop_timeout {
    static_cast<std::chrono::milliseconds::rep>(worker_runtime_component_count) *
      worker_runtime_component_stop_timeout.count() +
      worker_runtime_graceful_stop_margin.count()
  };

  enum class worker_observed_state_e {
    starting,
    ready,
    stopping,
    stopped,
    failed,
  };

  enum class worker_stop_mode_e {
    graceful,
    force,
  };

  /**
   * Result of one exact backend command.
   *
   * `not_found` is an authoritative absence result. `indeterminate` means the
   * command may have taken effect and therefore requires reconciliation.
   */
  enum class worker_command_result_e {
    applied,
    already_applied,
    not_found,
    rejected,
    indeterminate,
  };

  enum class broker_start_result_e {
    started,
    reconciliation_required,
    seat_not_found,
    invalid_seat_state,
    backend_rejected,
    backend_indeterminate,
  };

  enum class broker_stop_result_e {
    stop_requested,
    already_stopping,
    released,
    seat_not_found,
    backend_pending,
  };

  /** Exact authority for one backend worker. */
  struct worker_identity_t {
    seat_handle_t seat;
    std::string worker_name;

    bool operator==(const worker_identity_t &) const = default;
  };

  /**
   * Narrow launch payload passed to a future container/process backend.
   *
   * Client identity is intentionally absent. Profile and workload keys are
   * opaque control-plane references, not paths or credentials.
   */
  struct worker_launch_spec_t {
    worker_identity_t identity;
    seat_resources_t resources;
    std::string profile_key;
    std::string workload_key;
    std::string render_node;
    compositor_e compositor = compositor_e::automatic;
    std::uint32_t encoder_sessions = 1;

    bool operator==(const worker_launch_spec_t &) const = default;
  };

  struct worker_observation_t {
    worker_identity_t identity;
    worker_observed_state_e state = worker_observed_state_e::starting;

    bool operator==(const worker_observation_t &) const = default;
  };

  /**
   * Backend contract used by the broker.
   *
   * `inventory()` must return an authoritative, complete snapshot of workers
   * owned by this Polaris deployment at the instant it returns. Stop commands
   * must target the complete identity and must never fall back to a name,
   * process group, profile, or controller-wide wildcard.
   */
  class worker_backend_t {
  public:
    virtual ~worker_backend_t() = default;

    virtual worker_command_result_e launch(const worker_launch_spec_t &spec) = 0;
    virtual worker_command_result_e stop(
      const worker_identity_t &identity,
      worker_stop_mode_e mode
    ) = 0;
    virtual std::vector<worker_observation_t> inventory() = 0;
  };

  struct worker_broker_options_t {
    std::chrono::milliseconds graceful_stop_timeout {worker_runtime_graceful_stop_timeout};
    std::chrono::milliseconds force_stop_timeout {5000};
  };

  struct reconciliation_report_t {
    std::size_t observations = 0;
    std::size_t current_workers = 0;
    std::size_t orphan_workers = 0;
    std::size_t ready_transitions = 0;
    std::size_t missing_workers = 0;
    std::size_t released_seats = 0;
    std::size_t graceful_stop_requests = 0;
    std::size_t force_stop_requests = 0;
    std::size_t stuck_workers = 0;
    std::size_t protocol_errors = 0;
    std::size_t readiness_rejections = 0;
    std::vector<worker_identity_t> active_workers;
    bool backend_observation_failed = false;
    bool inventory_authoritative = false;
    bool admission_ready = false;
  };

  /**
   * Serializes worker commands and reconciles registry intent with backend
   * reality. Admission remains closed until one clean inventory pass proves
   * that no worker from an older controller epoch is still active.
   */
  class worker_broker_t {
  public:
    using monotonic_clock_t = std::chrono::steady_clock;
    using time_point_t = monotonic_clock_t::time_point;
    using now_fn_t = std::function<time_point_t()>;
    /** Hooks run inside broker serialization and must not re-enter the broker. */
    using ready_fn_t = std::function<bool(const worker_identity_t &)>;
    using shutdown_fn_t = std::function<void(const worker_identity_t &)>;

    worker_broker_t(
      registry_t &registry,
      worker_backend_t &backend,
      worker_broker_options_t options = {},
      now_fn_t now = {},
      ready_fn_t ready = {},
      shutdown_fn_t shutdown = {}
    );

    broker_start_result_e start_seat(const seat_handle_t &handle);
    broker_stop_result_e stop_seat(const seat_handle_t &handle);
    reconciliation_report_t reconcile();

    [[nodiscard]] bool admission_ready() const;

  private:
    struct pending_stop_t {
      worker_identity_t identity;
      time_point_t deadline;
      bool orphan = false;
      bool force_requested = false;
    };

    [[nodiscard]] pending_stop_t *find_pending_locked(const worker_identity_t &identity);
    void forget_pending_locked(const worker_identity_t &identity);
    worker_command_result_e request_graceful_stop_locked(
      const worker_identity_t &identity,
      bool orphan,
      time_point_t now,
      reconciliation_report_t *report
    );
    worker_command_result_e backend_stop_locked(
      const worker_identity_t &identity,
      worker_stop_mode_e mode
    );
    bool release_current_locked(
      const worker_identity_t &identity,
      reconciliation_report_t *report
    );

    registry_t &registry_;
    worker_backend_t &backend_;
    const worker_broker_options_t options_;
    now_fn_t now_;
    ready_fn_t ready_;
    shutdown_fn_t shutdown_;
    mutable std::mutex mutex_;
    std::vector<pending_stop_t> pending_stops_;
    bool admission_ready_ = false;
  };

}  // namespace multiseat
