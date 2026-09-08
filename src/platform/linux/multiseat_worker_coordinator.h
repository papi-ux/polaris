/**
 * @file src/platform/linux/multiseat_worker_coordinator.h
 * @brief Fail-closed authority and transport coordinator for multiseat workers.
 */
#pragma once

#ifdef __linux__

#include "multiseat_worker_launch_authority.h"
#include "multiseat_worker_authority.h"
#include "multiseat_worker_client.h"
#include "src/multiseat_worker_broker.h"

#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace multiseat {

  /** Injectable controller transport used by deterministic coordinator tests. */
  class worker_control_session_t {
  public:
    virtual ~worker_control_session_t() = default;

    /** `applied` means both control and media channels are authenticated. */
    virtual worker_ipc::transport_status_e connect(
      const worker_ipc::authority_handle_t &authority,
      worker_ipc::controller_client_options_t options
    ) = 0;
    virtual worker_ipc::transport_status_e heartbeat(worker_ipc::channel_e channel) = 0;
    virtual worker_ipc::transport_status_e shutdown() = 0;
    virtual void close() noexcept = 0;
    [[nodiscard]] virtual bool connected() const noexcept = 0;
    /** Only a real mutually authenticated session can provide a live lease. */
    [[nodiscard]] virtual worker_ipc::controller_connection_t lease_connection() const {
      return {};
    }
  };

  using worker_control_session_factory_t =
    std::function<std::unique_ptr<worker_control_session_t>()>;

  struct worker_coordinator_options_t {
    worker_broker_options_t broker;
    worker_ipc::controller_client_options_t client;
  };

  enum class coordinator_start_result_e {
    started,
    reconciliation_required,
    seat_not_found,
    invalid_seat_state,
    authority_rejected,
    backend_rejected,
    backend_indeterminate,
    cleanup_blocked,
  };

  struct coordinator_stop_result_t {
    broker_stop_result_e broker = broker_stop_result_e::seat_not_found;
    std::optional<worker_ipc::transport_status_e> transport;
    std::optional<worker_ipc::authority_status_e> cleanup;
  };

  struct coordinator_reconciliation_report_t {
    reconciliation_report_t broker;
    worker_ipc::authority_status_e authority_status =
      worker_ipc::authority_status_e::inactive;
    std::size_t authority_entries = 0;
    std::size_t active_orphan_authorities = 0;
    std::size_t recovered_authorities = 0;
    std::size_t removed_authorities = 0;
    std::size_t authority_failures = 0;
    std::size_t endpoint_connections = 0;
    std::size_t endpoint_failures = 0;
    std::size_t endpoint_heartbeats = 0;
    std::size_t endpoint_shutdowns = 0;
    std::size_t endpoint_shutdown_failures = 0;
    bool startup_recovery_complete = false;
    bool authority_blocked = false;
    bool admission_ready = false;
  };

  /**
   * Owns each exact authority handle and authenticated controller session from
   * pre-launch preparation through authoritative backend absence.
   *
   * The class accepts any `worker_backend_t`; it has no Podman dependency and
   * is deliberately not wired into Polaris' singleton runtime. Backend Ready
   * cannot become registry Running until both local channels authenticate.
   * Graceful backend stop is preceded by an authenticated shutdown when a
   * session exists. Crash leftovers are removed only after a clean complete
   * backend inventory proves their signed identity absent.
   */
  class worker_coordinator_t final : public authenticated_worker_seat_authority_t {
  public:
    worker_coordinator_t(
      registry_t &registry,
      worker_backend_t &backend,
      worker_ipc::authority_store_t &authority_store,
      worker_coordinator_options_t options = {},
      worker_broker_t::now_fn_t now = {},
      worker_control_session_factory_t session_factory = {}
    );
    ~worker_coordinator_t();

    worker_coordinator_t(const worker_coordinator_t &) = delete;
    worker_coordinator_t &operator=(const worker_coordinator_t &) = delete;
    worker_coordinator_t(worker_coordinator_t &&) = delete;
    worker_coordinator_t &operator=(worker_coordinator_t &&) = delete;

    [[nodiscard]] coordinator_start_result_e start_seat(const seat_handle_t &handle);
    [[nodiscard]] coordinator_stop_result_t stop_seat(const seat_handle_t &handle);
    [[nodiscard]] coordinator_reconciliation_report_t reconcile();
    [[nodiscard]] worker_ipc::transport_status_e heartbeat(
      const seat_handle_t &handle,
      worker_ipc::channel_e channel
    );

    [[nodiscard]] bool admission_ready() const;
    [[nodiscard]] std::vector<worker_identity_t> managed_workers() const;
    [[nodiscard]] worker_seat_authorization_status_e
    with_authenticated_worker_seat(
      const seat_handle_t &handle,
      const authenticated_worker_seat_action_t &action
    ) override;
    [[nodiscard]] worker_seat_authorization_status_e
    with_authenticated_worker_connection(
      const seat_handle_t &handle,
      const authenticated_worker_connection_action_t &action
    ) override;

  private:
    using authenticated_session_action_t = std::function<worker_seat_authorization_status_e(
      const authenticated_worker_seat_t &,
      worker_control_session_t &
    )>;
    [[nodiscard]] worker_seat_authorization_status_e with_authenticated_session(
      const seat_handle_t &handle,
      const authenticated_session_action_t &action
    );

    struct managed_worker_t {
      worker_identity_t identity;
      worker_ipc::authority_handle_t authority;
      std::unique_ptr<worker_control_session_t> session;
    };

    [[nodiscard]] static worker_identity_t worker_identity_for(
      const seat_snapshot_t &seat
    );
    [[nodiscard]] static worker_ipc::endpoint_identity_t endpoint_identity_for(
      const worker_identity_t &identity
    );
    [[nodiscard]] managed_worker_t *find_managed_locked(
      const worker_identity_t &identity
    );
    [[nodiscard]] const managed_worker_t *find_managed_locked(
      const worker_identity_t &identity
    ) const;
    [[nodiscard]] bool ready_endpoint_locked(const worker_identity_t &identity);
    void shutdown_endpoint_locked(const worker_identity_t &identity);
    [[nodiscard]] worker_ipc::authority_status_e remove_managed_locked(
      const worker_identity_t &identity
    );
    void recover_startup_locked(
      const reconciliation_report_t &broker_report,
      coordinator_reconciliation_report_t &report
    );
    void inspect_managed_locked(coordinator_reconciliation_report_t &report);
    [[nodiscard]] bool admission_ready_locked() const;

    registry_t &registry_;
    worker_ipc::authority_store_t &authority_store_;
    const worker_coordinator_options_t options_;
    worker_control_session_factory_t session_factory_;
    mutable std::mutex mutex_;
    std::vector<managed_worker_t> managed_;
    bool startup_recovery_complete_ = false;
    bool authority_blocked_ = false;
    std::size_t callback_connections_ = 0;
    std::size_t callback_failures_ = 0;
    std::size_t callback_shutdowns_ = 0;
    std::size_t callback_shutdown_failures_ = 0;
    std::optional<worker_ipc::transport_status_e> callback_shutdown_status_;
    worker_broker_t broker_;
  };

}  // namespace multiseat

#endif
