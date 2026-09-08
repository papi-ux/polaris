/**
 * @file src/platform/linux/multiseat_worker_coordinator.cpp
 * @brief Fail-closed authority and transport coordinator for multiseat workers.
 */
#include "multiseat_worker_coordinator.h"

#ifdef __linux__

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace multiseat {
  namespace {
    class native_control_session_t final : public worker_control_session_t {
    public:
      worker_ipc::transport_status_e connect(
        const worker_ipc::authority_handle_t &authority,
        worker_ipc::controller_client_options_t options
      ) override {
        return client_.connect(authority, options);
      }

      worker_ipc::transport_status_e heartbeat(
        worker_ipc::channel_e channel
      ) override {
        return client_.heartbeat(channel);
      }

      worker_ipc::transport_status_e shutdown() override {
        return client_.shutdown();
      }

      void close() noexcept override {
        client_.close();
      }

      bool connected() const noexcept override {
        return client_.connected();
      }

      worker_ipc::controller_connection_t lease_connection() const override {
        return client_.lease_connection();
      }

    private:
      worker_ipc::controller_client_t client_;
    };
  }  // namespace

  worker_coordinator_t::worker_coordinator_t(
    registry_t &registry,
    worker_backend_t &backend,
    worker_ipc::authority_store_t &authority_store,
    worker_coordinator_options_t options,
    worker_broker_t::now_fn_t now,
    worker_control_session_factory_t session_factory
  ) :
      registry_(registry),
      authority_store_(authority_store),
      options_(options),
      session_factory_(std::move(session_factory)),
      broker_(
        registry,
        backend,
        options.broker,
        std::move(now),
        [this](const worker_identity_t &identity) {
          return ready_endpoint_locked(identity);
        },
        [this](const worker_identity_t &identity) {
          shutdown_endpoint_locked(identity);
        }
      ) {
    if (!session_factory_) {
      session_factory_ = []() {
        return std::make_unique<native_control_session_t>();
      };
    }
  }

  worker_coordinator_t::~worker_coordinator_t() {
    std::scoped_lock lock {mutex_};
    for (auto &worker : managed_) {
      if (worker.session) {
        worker.session->close();
      }
    }
  }

  worker_identity_t worker_coordinator_t::worker_identity_for(
    const seat_snapshot_t &seat
  ) {
    return {
      .seat = seat.handle,
      .worker_name = seat.resources.worker_name,
    };
  }

  worker_ipc::endpoint_identity_t worker_coordinator_t::endpoint_identity_for(
    const worker_identity_t &identity
  ) {
    return {
      .controller_epoch = identity.seat.controller_epoch,
      .logical_gpu_id = identity.seat.logical_gpu_id,
      .slot = identity.seat.slot,
      .generation = identity.seat.generation,
      .worker_name = identity.worker_name,
    };
  }

  worker_coordinator_t::managed_worker_t *worker_coordinator_t::find_managed_locked(
    const worker_identity_t &identity
  ) {
    const auto worker = std::find_if(
      managed_.begin(),
      managed_.end(),
      [&identity](const auto &candidate) {
        return candidate.identity == identity;
      }
    );
    return worker == managed_.end() ? nullptr : &*worker;
  }

  const worker_coordinator_t::managed_worker_t *
  worker_coordinator_t::find_managed_locked(
    const worker_identity_t &identity
  ) const {
    const auto worker = std::find_if(
      managed_.begin(),
      managed_.end(),
      [&identity](const auto &candidate) {
        return candidate.identity == identity;
      }
    );
    return worker == managed_.end() ? nullptr : &*worker;
  }

  bool worker_coordinator_t::ready_endpoint_locked(
    const worker_identity_t &identity
  ) {
    auto *worker = find_managed_locked(identity);
    if (!worker ||
        authority_store_.validate(worker->authority) !=
          worker_ipc::authority_status_e::applied) {
      ++callback_failures_;
      authority_blocked_ = true;
      return false;
    }
    if (worker->session && worker->session->connected()) {
      return true;
    }

    std::unique_ptr<worker_control_session_t> session;
    try {
      session = session_factory_();
    } catch (...) {
      ++callback_failures_;
      return false;
    }
    if (!session) {
      ++callback_failures_;
      return false;
    }
    worker_ipc::transport_status_e status;
    try {
      status = session->connect(worker->authority, options_.client);
    } catch (...) {
      status = worker_ipc::transport_status_e::io_error;
    }
    if (status != worker_ipc::transport_status_e::applied ||
        !session->connected()) {
      session->close();
      ++callback_failures_;
      return false;
    }
    worker->session = std::move(session);
    ++callback_connections_;
    return true;
  }

  void worker_coordinator_t::shutdown_endpoint_locked(
    const worker_identity_t &identity
  ) {
    auto *worker = find_managed_locked(identity);
    if (!worker || !worker->session || !worker->session->connected()) {
      return;
    }
    worker_ipc::transport_status_e status;
    try {
      status = worker->session->shutdown();
    } catch (...) {
      status = worker_ipc::transport_status_e::io_error;
    }
    callback_shutdown_status_ = status;
    ++callback_shutdowns_;
    if (status != worker_ipc::transport_status_e::applied) {
      ++callback_shutdown_failures_;
    }
    worker->session->close();
    worker->session.reset();
  }

  worker_ipc::authority_status_e worker_coordinator_t::remove_managed_locked(
    const worker_identity_t &identity
  ) {
    const auto worker = std::find_if(
      managed_.begin(),
      managed_.end(),
      [&identity](const auto &candidate) {
        return candidate.identity == identity;
      }
    );
    if (worker == managed_.end()) {
      return worker_ipc::authority_status_e::inactive;
    }
    if (worker->session) {
      worker->session->close();
      worker->session.reset();
    }
    const auto status = authority_store_.remove(worker->authority);
    if (status == worker_ipc::authority_status_e::applied) {
      managed_.erase(worker);
    } else {
      authority_blocked_ = true;
    }
    return status;
  }

  coordinator_start_result_e worker_coordinator_t::start_seat(
    const seat_handle_t &handle
  ) {
    std::scoped_lock lock {mutex_};
    if (!admission_ready_locked()) {
      return coordinator_start_result_e::reconciliation_required;
    }
    const auto seat = registry_.snapshot(handle);
    if (!seat) {
      return coordinator_start_result_e::seat_not_found;
    }
    if (seat->state != seat_state_e::reserved) {
      return coordinator_start_result_e::invalid_seat_state;
    }
    const auto identity = worker_identity_for(*seat);
    if (find_managed_locked(identity)) {
      return coordinator_start_result_e::invalid_seat_state;
    }

    auto created = authority_store_.create(
      endpoint_identity_for(identity),
      seat->resources.runtime_namespace,
      {
        .compositor = seat->selected_compositor,
        .workload = seat->workload,
      }
    );
    if (!created.created()) {
      startup_recovery_complete_ = false;
      authority_blocked_ = true;
      if (registry_.begin_stop(handle) == mutation_result_e::applied) {
        (void) registry_.release(handle);
      }
      return coordinator_start_result_e::authority_rejected;
    }
    managed_.push_back({
      .identity = identity,
      .authority = std::move(*created.authority),
      .session = {},
    });

    const auto started = broker_.start_seat(handle);
    switch (started) {
      case broker_start_result_e::started:
        return coordinator_start_result_e::started;
      case broker_start_result_e::backend_indeterminate:
        return coordinator_start_result_e::backend_indeterminate;
      case broker_start_result_e::backend_rejected: {
        const auto cleanup = remove_managed_locked(identity);
        return cleanup == worker_ipc::authority_status_e::applied ?
                 coordinator_start_result_e::backend_rejected :
                 coordinator_start_result_e::cleanup_blocked;
      }
      case broker_start_result_e::reconciliation_required: {
        const auto cleanup = remove_managed_locked(identity);
        return cleanup == worker_ipc::authority_status_e::applied ?
                 coordinator_start_result_e::reconciliation_required :
                 coordinator_start_result_e::cleanup_blocked;
      }
      case broker_start_result_e::seat_not_found: {
        const auto cleanup = remove_managed_locked(identity);
        return cleanup == worker_ipc::authority_status_e::applied ?
                 coordinator_start_result_e::seat_not_found :
                 coordinator_start_result_e::cleanup_blocked;
      }
      case broker_start_result_e::invalid_seat_state: {
        const auto cleanup = remove_managed_locked(identity);
        return cleanup == worker_ipc::authority_status_e::applied ?
                 coordinator_start_result_e::invalid_seat_state :
                 coordinator_start_result_e::cleanup_blocked;
      }
    }
    authority_blocked_ = true;
    return coordinator_start_result_e::cleanup_blocked;
  }

  coordinator_stop_result_t worker_coordinator_t::stop_seat(
    const seat_handle_t &handle
  ) {
    std::scoped_lock lock {mutex_};
    const auto seat = registry_.snapshot(handle);
    const auto identity = seat ?
                            std::optional {worker_identity_for(*seat)} :
                            std::nullopt;
    callback_shutdown_status_.reset();
    const auto result = broker_.stop_seat(handle);
    std::optional<worker_ipc::authority_status_e> cleanup;
    if (result == broker_stop_result_e::released && identity) {
      cleanup = remove_managed_locked(*identity);
    }
    return {
      .broker = result,
      .transport = callback_shutdown_status_,
      .cleanup = cleanup,
    };
  }

  void worker_coordinator_t::recover_startup_locked(
    const reconciliation_report_t &broker_report,
    coordinator_reconciliation_report_t &report
  ) {
    if (!broker_report.inventory_authoritative) {
      return;
    }
    std::vector<worker_ipc::endpoint_identity_t> active;
    active.reserve(broker_report.active_workers.size());
    for (const auto &identity : broker_report.active_workers) {
      active.push_back(endpoint_identity_for(identity));
    }
    auto recovered = authority_store_.recover_inactive(active);
    startup_recovery_complete_ = false;
    report.authority_status = recovered.status;
    report.authority_entries = recovered.observed;
    if (!recovered.inspected()) {
      authority_blocked_ = true;
      ++report.authority_failures;
      return;
    }
    const auto managed_endpoint = [this](const auto &endpoint) {
      return std::any_of(
        managed_.begin(),
        managed_.end(),
        [&endpoint](const auto &worker) {
          return endpoint_identity_for(worker.identity) == endpoint;
        }
      );
    };
    report.active_orphan_authorities = static_cast<std::size_t>(std::count_if(
      recovered.active_identities.begin(),
      recovered.active_identities.end(),
      [&managed_endpoint](const auto &identity) {
        return !managed_endpoint(identity);
      }
    ));
    for (auto &authority : recovered.inactive) {
      ++report.recovered_authorities;
      if (authority_store_.remove(authority) ==
          worker_ipc::authority_status_e::applied) {
        ++report.removed_authorities;
      } else {
        authority_blocked_ = true;
        ++report.authority_failures;
      }
    }
    if (report.active_orphan_authorities != 0 ||
        report.authority_failures != 0) {
      authority_blocked_ = true;
      return;
    }

    const auto confirmed = authority_store_.recover_inactive(active);
    report.authority_status = confirmed.status;
    const auto confirmed_orphans = static_cast<std::size_t>(std::count_if(
      confirmed.active_identities.begin(),
      confirmed.active_identities.end(),
      [&managed_endpoint](const auto &identity) {
        return !managed_endpoint(identity);
      }
    ));
    if (!confirmed.inspected() ||
        !confirmed.inactive.empty() ||
        confirmed_orphans != 0 ||
        confirmed.active_identities.size() != managed_.size()) {
      authority_blocked_ = true;
      ++report.authority_failures;
      return;
    }
    startup_recovery_complete_ = true;
  }

  void worker_coordinator_t::inspect_managed_locked(
    coordinator_reconciliation_report_t &report
  ) {
    for (std::size_t index = 0; index < managed_.size();) {
      const auto identity = managed_[index].identity;
      const auto seat = registry_.snapshot(identity.seat);
      if (!seat || seat->resources.worker_name != identity.worker_name) {
        if (remove_managed_locked(identity) ==
            worker_ipc::authority_status_e::applied) {
          ++report.removed_authorities;
          continue;
        }
        ++report.authority_failures;
        ++index;
        continue;
      }
      if (authority_store_.validate(managed_[index].authority) !=
          worker_ipc::authority_status_e::applied) {
        authority_blocked_ = true;
        ++report.authority_failures;
        (void) broker_.stop_seat(identity.seat);
        ++index;
        continue;
      }
      if (seat->state != seat_state_e::running) {
        ++index;
        continue;
      }
      auto &session = managed_[index].session;
      if (!session || !session->connected()) {
        ++callback_failures_;
        (void) broker_.stop_seat(identity.seat);
        ++index;
        continue;
      }
      bool healthy = true;
      for (const auto channel : {
             worker_ipc::channel_e::control,
             worker_ipc::channel_e::media,
           }) {
        worker_ipc::transport_status_e status;
        try {
          status = session->heartbeat(channel);
        } catch (...) {
          status = worker_ipc::transport_status_e::io_error;
        }
        if (status != worker_ipc::transport_status_e::applied) {
          healthy = false;
          ++callback_failures_;
          break;
        }
        ++report.endpoint_heartbeats;
      }
      if (!healthy) {
        session->close();
        session.reset();
        (void) broker_.stop_seat(identity.seat);
      }
      ++index;
    }
  }

  coordinator_reconciliation_report_t worker_coordinator_t::reconcile() {
    std::scoped_lock lock {mutex_};
    coordinator_reconciliation_report_t report;
    authority_blocked_ = false;
    callback_connections_ = 0;
    callback_failures_ = 0;
    callback_shutdowns_ = 0;
    callback_shutdown_failures_ = 0;
    callback_shutdown_status_.reset();

    report.broker = broker_.reconcile();
    report.authority_status = authority_store_.status();
    inspect_managed_locked(report);
    recover_startup_locked(report.broker, report);

    report.endpoint_connections = callback_connections_;
    report.endpoint_failures = callback_failures_;
    report.endpoint_shutdowns = callback_shutdowns_;
    report.endpoint_shutdown_failures = callback_shutdown_failures_;
    report.startup_recovery_complete = startup_recovery_complete_;
    report.authority_blocked = authority_blocked_;
    report.admission_ready = admission_ready_locked();
    return report;
  }

  worker_ipc::transport_status_e worker_coordinator_t::heartbeat(
    const seat_handle_t &handle,
    worker_ipc::channel_e channel
  ) {
    std::scoped_lock lock {mutex_};
    const auto seat = registry_.snapshot(handle);
    if (!seat) {
      return worker_ipc::transport_status_e::closed;
    }
    auto *worker = find_managed_locked(worker_identity_for(*seat));
    if (!worker || !worker->session || !worker->session->connected()) {
      return worker_ipc::transport_status_e::closed;
    }
    worker_ipc::transport_status_e status;
    try {
      status = worker->session->heartbeat(channel);
    } catch (...) {
      status = worker_ipc::transport_status_e::io_error;
    }
    if (status != worker_ipc::transport_status_e::applied) {
      worker->session->close();
      worker->session.reset();
      const auto stopped = broker_.stop_seat(handle);
      if (stopped == broker_stop_result_e::released) {
        (void) remove_managed_locked(worker_identity_for(*seat));
      }
    }
    return status;
  }

  bool worker_coordinator_t::admission_ready_locked() const {
    return startup_recovery_complete_ &&
           !authority_blocked_ &&
           broker_.admission_ready();
  }

  bool worker_coordinator_t::admission_ready() const {
    std::scoped_lock lock {mutex_};
    return admission_ready_locked();
  }

  std::vector<worker_identity_t> worker_coordinator_t::managed_workers() const {
    std::scoped_lock lock {mutex_};
    std::vector<worker_identity_t> identities;
    identities.reserve(managed_.size());
    for (const auto &worker : managed_) {
      identities.push_back(worker.identity);
    }
    return identities;
  }

  worker_seat_authorization_status_e
  worker_coordinator_t::with_authenticated_worker_seat(
    const seat_handle_t &handle,
    const authenticated_worker_seat_action_t &action
  ) {
    if (!action) {
      return worker_seat_authorization_status_e::invalid_request;
    }
    return with_authenticated_session(handle, [&](const auto &seat, auto &) {
      action(seat);
      return worker_seat_authorization_status_e::applied;
    });
  }

  worker_seat_authorization_status_e
  worker_coordinator_t::with_authenticated_worker_connection(
    const seat_handle_t &handle,
    const authenticated_worker_connection_action_t &action
  ) {
    if (!action) {
      return worker_seat_authorization_status_e::invalid_request;
    }
    return with_authenticated_session(handle, [&](const auto &seat, auto &session) {
      const auto connection = session.lease_connection();
      const auto expected = endpoint_identity_for({seat.handle, seat.worker_name});
      if (!connection.connected() || connection.identity() != expected) {
        return worker_seat_authorization_status_e::endpoint_not_authenticated;
      }
      action(seat, connection);
      return worker_seat_authorization_status_e::applied;
    });
  }

  worker_seat_authorization_status_e
  worker_coordinator_t::with_authenticated_session(
    const seat_handle_t &handle,
    const authenticated_session_action_t &action
  ) {
    if (!handle.valid() || !action) {
      return worker_seat_authorization_status_e::invalid_request;
    }

    std::scoped_lock lock {mutex_};
    if (!admission_ready_locked()) {
      return worker_seat_authorization_status_e::reconciliation_required;
    }
    const auto seat = registry_.snapshot(handle);
    if (!seat) {
      return worker_seat_authorization_status_e::seat_not_found;
    }
    if (seat->state != seat_state_e::running) {
      return worker_seat_authorization_status_e::seat_not_running;
    }

    const auto identity = worker_identity_for(*seat);
    auto *worker = find_managed_locked(identity);
    if (!worker) {
      return worker_seat_authorization_status_e::worker_not_managed;
    }
    if (authority_store_.validate(worker->authority) !=
        worker_ipc::authority_status_e::applied) {
      authority_blocked_ = true;
      return worker_seat_authorization_status_e::authority_rejected;
    }
    if (!worker->session || !worker->session->connected()) {
      return worker_seat_authorization_status_e::endpoint_not_authenticated;
    }

    const authenticated_worker_seat_t authenticated {
      .handle = seat->handle,
      .worker_name = seat->resources.worker_name,
      .input_seat = seat->resources.input_seat,
      .client_key = seat->client_key,
    };
    try {
      return action(authenticated, *worker->session);
    } catch (...) {
      return worker_seat_authorization_status_e::action_failed;
    }
  }

}  // namespace multiseat

#endif
