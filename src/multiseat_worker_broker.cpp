/**
 * @file src/multiseat_worker_broker.cpp
 * @brief Backend-neutral worker lifecycle and reconciliation contract.
 */
#include "multiseat_worker_broker.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace multiseat {
  namespace {
    worker_identity_t identity_for(const seat_snapshot_t &seat) {
      return {
        .seat = seat.handle,
        .worker_name = seat.resources.worker_name,
      };
    }

    worker_launch_spec_t launch_spec_for(const seat_snapshot_t &seat) {
      return {
        .identity = identity_for(seat),
        .resources = seat.resources,
        .profile_key = seat.profile_key,
        .workload_key = seat.workload_key,
        .render_node = seat.render_node,
        .runtime_profile = seat.runtime_profile,
        .display_mode = seat.display_mode,
        .compositor = seat.selected_compositor,
        .encoder_sessions = seat.encoder_sessions,
      };
    }

    bool active(worker_observed_state_e state) {
      return state != worker_observed_state_e::stopped;
    }

    bool valid_observed_state(worker_observed_state_e state) {
      switch (state) {
        case worker_observed_state_e::starting:
        case worker_observed_state_e::ready:
        case worker_observed_state_e::stopping:
        case worker_observed_state_e::stopped:
        case worker_observed_state_e::failed:
          return true;
      }
      return false;
    }

    bool valid_identity(const worker_identity_t &identity) {
      return identity.seat.valid() && !identity.worker_name.empty();
    }

    bool contains_identity(
      const std::vector<worker_identity_t> &identities,
      const worker_identity_t &identity
    ) {
      return std::find(identities.begin(), identities.end(), identity) != identities.end();
    }

    bool contains_active_observation(
      const std::vector<worker_observation_t> &observations,
      const worker_identity_t &identity
    ) {
      return std::any_of(
        observations.begin(),
        observations.end(),
        [&identity](const auto &observation) {
          return observation.identity == identity && active(observation.state);
        }
      );
    }
  }  // namespace

  worker_broker_t::worker_broker_t(
    registry_t &registry,
    worker_backend_t &backend,
    worker_broker_options_t options,
    now_fn_t now,
    ready_fn_t ready,
    shutdown_fn_t shutdown
  ) :
      registry_(registry),
      backend_(backend),
      options_(options),
      now_(std::move(now)),
      ready_(std::move(ready)),
      shutdown_(std::move(shutdown)) {
    if (options_.graceful_stop_timeout <= std::chrono::milliseconds::zero() ||
        options_.force_stop_timeout <= std::chrono::milliseconds::zero()) {
      throw std::invalid_argument {"multiseat worker stop timeouts must be positive"};
    }
    if (!now_) {
      now_ = []() {
        return monotonic_clock_t::now();
      };
    }
  }

  broker_start_result_e worker_broker_t::start_seat(const seat_handle_t &handle) {
    std::scoped_lock lock {mutex_};
    if (!admission_ready_) {
      return broker_start_result_e::reconciliation_required;
    }

    const auto seat = registry_.snapshot(handle);
    if (!seat) {
      return broker_start_result_e::seat_not_found;
    }
    if (seat->state != seat_state_e::reserved) {
      return broker_start_result_e::invalid_seat_state;
    }

    const auto starting = registry_.mark_starting(handle);
    if (starting != mutation_result_e::applied) {
      if (starting == mutation_result_e::not_found ||
          starting == mutation_result_e::stale_controller ||
          starting == mutation_result_e::stale_generation) {
        return broker_start_result_e::seat_not_found;
      }
      return broker_start_result_e::invalid_seat_state;
    }

    const auto spec = launch_spec_for(*seat);
    worker_command_result_e launch_result;
    try {
      launch_result = backend_.launch(spec);
    } catch (...) {
      launch_result = worker_command_result_e::indeterminate;
    }

    switch (launch_result) {
      case worker_command_result_e::applied:
      case worker_command_result_e::already_applied:
        return broker_start_result_e::started;
      case worker_command_result_e::not_found:
      case worker_command_result_e::rejected:
        release_current_locked(spec.identity, nullptr);
        return broker_start_result_e::backend_rejected;
      case worker_command_result_e::indeterminate:
        registry_.begin_stop(handle);
        request_graceful_stop_locked(spec.identity, false, now_(), nullptr);
        return broker_start_result_e::backend_indeterminate;
    }

    release_current_locked(spec.identity, nullptr);
    return broker_start_result_e::backend_indeterminate;
  }

  broker_stop_result_e worker_broker_t::stop_seat(const seat_handle_t &handle) {
    std::scoped_lock lock {mutex_};
    const auto seat = registry_.snapshot(handle);
    if (!seat) {
      return broker_stop_result_e::seat_not_found;
    }

    const bool already_stopping = seat->state == seat_state_e::stopping;
    const auto stopping = registry_.begin_stop(handle);
    if (stopping != mutation_result_e::applied) {
      return broker_stop_result_e::seat_not_found;
    }

    const auto identity = identity_for(*seat);
    const auto result = request_graceful_stop_locked(
      identity,
      false,
      now_(),
      nullptr
    );
    if (result == worker_command_result_e::not_found) {
      const bool released = release_current_locked(identity, nullptr);
      forget_pending_locked(identity);
      return released ? broker_stop_result_e::released : broker_stop_result_e::seat_not_found;
    }
    if (already_stopping) {
      return broker_stop_result_e::already_stopping;
    }
    if (result == worker_command_result_e::applied ||
        result == worker_command_result_e::already_applied) {
      return broker_stop_result_e::stop_requested;
    }
    return broker_stop_result_e::backend_pending;
  }

  reconciliation_report_t worker_broker_t::reconcile() {
    std::scoped_lock lock {mutex_};
    reconciliation_report_t report;
    std::vector<worker_observation_t> inventory;
    try {
      inventory = backend_.inventory();
    } catch (...) {
      admission_ready_ = false;
      report.backend_observation_failed = true;
      return report;
    }

    report.observations = inventory.size();
    const auto now = now_();
    std::vector<worker_identity_t> observed_identities;
    observed_identities.reserve(inventory.size());
    for (const auto &observation : inventory) {
      if (!valid_identity(observation.identity) ||
          !valid_observed_state(observation.state) ||
          contains_identity(observed_identities, observation.identity)) {
        ++report.protocol_errors;
        continue;
      }
      observed_identities.push_back(observation.identity);
    }
    if (report.protocol_errors != 0) {
      admission_ready_ = false;
      return report;
    }
    report.inventory_authoritative = true;
    for (const auto &observation : inventory) {
      if (active(observation.state)) {
        report.active_workers.push_back(observation.identity);
      }
    }

    bool orphan_seen = false;
    std::vector<worker_identity_t> current_active_workers;
    for (const auto &observation : inventory) {
      const auto seat = registry_.snapshot(observation.identity.seat);
      const bool exact_current = seat &&
                                 seat->resources.worker_name == observation.identity.worker_name;

      if (observation.state == worker_observed_state_e::stopped) {
        if (exact_current && release_current_locked(observation.identity, &report)) {
          forget_pending_locked(observation.identity);
        } else if (!exact_current) {
          forget_pending_locked(observation.identity);
        }
        continue;
      }

      if (!exact_current) {
        orphan_seen = true;
        ++report.orphan_workers;
        if (seat) {
          ++report.protocol_errors;
        }
        request_graceful_stop_locked(observation.identity, true, now, &report);
        continue;
      }

      ++report.current_workers;
      current_active_workers.push_back(observation.identity);
      switch (observation.state) {
        case worker_observed_state_e::starting:
          if (seat->state == seat_state_e::reserved) {
            ++report.protocol_errors;
            registry_.begin_stop(seat->handle);
            request_graceful_stop_locked(observation.identity, false, now, &report);
          } else if (seat->state == seat_state_e::running) {
            ++report.protocol_errors;
          } else if (seat->state == seat_state_e::stopping) {
            request_graceful_stop_locked(observation.identity, false, now, &report);
          }
          break;
        case worker_observed_state_e::ready:
          if (seat->state == seat_state_e::starting) {
            bool endpoint_ready = true;
            if (ready_) {
              try {
                endpoint_ready = ready_(observation.identity);
              } catch (...) {
                endpoint_ready = false;
              }
            }
            if (!endpoint_ready) {
              ++report.readiness_rejections;
              registry_.begin_stop(seat->handle);
              request_graceful_stop_locked(observation.identity, false, now, &report);
            } else if (registry_.mark_running(seat->handle) == mutation_result_e::applied) {
              ++report.ready_transitions;
            } else {
              ++report.protocol_errors;
            }
          } else if (seat->state == seat_state_e::reserved) {
            ++report.protocol_errors;
            registry_.begin_stop(seat->handle);
            request_graceful_stop_locked(observation.identity, false, now, &report);
          } else if (seat->state == seat_state_e::stopping) {
            request_graceful_stop_locked(observation.identity, false, now, &report);
          }
          break;
        case worker_observed_state_e::stopping:
        case worker_observed_state_e::failed:
          registry_.begin_stop(seat->handle);
          request_graceful_stop_locked(observation.identity, false, now, &report);
          break;
        case worker_observed_state_e::stopped:
          break;
      }
    }

    for (const auto &seat : registry_.seats()) {
      if (seat.state == seat_state_e::reserved) {
        continue;
      }
      const auto identity = identity_for(seat);
      if (contains_identity(current_active_workers, identity)) {
        continue;
      }

      ++report.missing_workers;
      if (release_current_locked(identity, &report)) {
        forget_pending_locked(identity);
      }
    }

    for (std::size_t index = 0; index < pending_stops_.size();) {
      auto &pending = pending_stops_[index];
      if (!contains_active_observation(inventory, pending.identity)) {
        release_current_locked(pending.identity, &report);
        pending_stops_.erase(pending_stops_.begin() + static_cast<std::ptrdiff_t>(index));
        continue;
      }

      if (pending.force_requested) {
        if (now >= pending.deadline) {
          ++report.stuck_workers;
        }
        ++index;
        continue;
      }
      if (now < pending.deadline) {
        ++index;
        continue;
      }

      const auto force_result = backend_stop_locked(
        pending.identity,
        worker_stop_mode_e::force
      );
      ++report.force_stop_requests;
      if (force_result == worker_command_result_e::not_found) {
        release_current_locked(pending.identity, &report);
        pending_stops_.erase(pending_stops_.begin() + static_cast<std::ptrdiff_t>(index));
        continue;
      }
      pending.force_requested = true;
      pending.deadline = now + options_.force_stop_timeout;
      ++index;
    }

    const bool orphan_pending = std::any_of(
      pending_stops_.begin(),
      pending_stops_.end(),
      [](const auto &pending) {
        return pending.orphan;
      }
    );
    admission_ready_ = !orphan_seen &&
                       !orphan_pending &&
                       report.protocol_errors == 0;
    report.admission_ready = admission_ready_;
    return report;
  }

  bool worker_broker_t::admission_ready() const {
    std::scoped_lock lock {mutex_};
    return admission_ready_;
  }

  worker_broker_t::pending_stop_t *worker_broker_t::find_pending_locked(
    const worker_identity_t &identity
  ) {
    const auto pending = std::find_if(
      pending_stops_.begin(),
      pending_stops_.end(),
      [&identity](const auto &candidate) {
        return candidate.identity == identity;
      }
    );
    return pending == pending_stops_.end() ? nullptr : &*pending;
  }

  void worker_broker_t::forget_pending_locked(const worker_identity_t &identity) {
    pending_stops_.erase(
      std::remove_if(
        pending_stops_.begin(),
        pending_stops_.end(),
        [&identity](const auto &pending) {
          return pending.identity == identity;
        }
      ),
      pending_stops_.end()
    );
  }

  worker_command_result_e worker_broker_t::request_graceful_stop_locked(
    const worker_identity_t &identity,
    bool orphan,
    time_point_t now,
    reconciliation_report_t *report
  ) {
    if (auto *pending = find_pending_locked(identity)) {
      pending->orphan = pending->orphan || orphan;
      return worker_command_result_e::already_applied;
    }

    if (shutdown_) {
      try {
        shutdown_(identity);
      } catch (...) {
      }
    }
    const auto result = backend_stop_locked(identity, worker_stop_mode_e::graceful);
    if (report) {
      ++report->graceful_stop_requests;
    }
    if (result != worker_command_result_e::not_found) {
      pending_stops_.push_back({
        .identity = identity,
        .deadline = now + options_.graceful_stop_timeout,
        .orphan = orphan,
        .force_requested = false,
      });
    }
    return result;
  }

  worker_command_result_e worker_broker_t::backend_stop_locked(
    const worker_identity_t &identity,
    worker_stop_mode_e mode
  ) {
    try {
      return backend_.stop(identity, mode);
    } catch (...) {
      return worker_command_result_e::indeterminate;
    }
  }

  bool worker_broker_t::release_current_locked(
    const worker_identity_t &identity,
    reconciliation_report_t *report
  ) {
    const auto seat = registry_.snapshot(identity.seat);
    if (!seat || seat->resources.worker_name != identity.worker_name) {
      return false;
    }
    if (registry_.begin_stop(identity.seat) != mutation_result_e::applied) {
      return false;
    }
    if (registry_.release(identity.seat) != mutation_result_e::applied) {
      return false;
    }
    if (report) {
      ++report->released_seats;
    }
    return true;
  }

}  // namespace multiseat
