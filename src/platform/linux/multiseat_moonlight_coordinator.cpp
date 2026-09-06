/**
 * @file src/platform/linux/multiseat_moonlight_coordinator.cpp
 * @brief Default-off owner for multiseat Moonlight input dependencies.
 */
#include "multiseat_moonlight_coordinator.h"

#ifdef __linux__

  #include "src/rtsp.h"

  #include <algorithm>
  #include <utility>

namespace multiseat::input {

  moonlight_coordinator_create_result_t
  moonlight_session_coordinator_t::create(
    moonlight_session_coordinator_options_t options,
    moonlight_input_backend_factory_t backend_factory
  ) {
    if (!backend_factory) {
      return {
        .status = moonlight_coordinator_create_status_e::invalid_factory,
      };
    }

    auto feedback_hub = std::make_shared<moonlight_controller_feedback_hub_t>();
    std::unique_ptr<backend_t> backend;
    try {
      backend = backend_factory(feedback_hub->sink());
    } catch (...) {
      return {
        .status = moonlight_coordinator_create_status_e::backend_unavailable,
      };
    }
    if (!backend) {
      return {
        .status = moonlight_coordinator_create_status_e::backend_unavailable,
      };
    }

    auto coordinator = std::unique_ptr<moonlight_session_coordinator_t> {
      new moonlight_session_coordinator_t(
        options,
        std::move(feedback_hub),
        std::move(backend)
      )
    };
    if (!options.enabled) {
      return {
        .status = moonlight_coordinator_create_status_e::ready_disabled,
        .coordinator = std::move(coordinator),
      };
    }

    auto installed = install_moonlight_session_activation_gate(
      coordinator->activation_gate_
    );
    const auto activation_status = installed.status;
    if (activation_status != moonlight_activation_install_status_e::installed ||
        !installed.installation) {
      return {
        .status =
          moonlight_coordinator_create_status_e::activation_install_rejected,
        .activation_status = activation_status,
      };
    }
    coordinator->activation_installation_ = std::move(installed.installation);
    return {
      .status = moonlight_coordinator_create_status_e::ready_enabled,
      .activation_status = activation_status,
      .coordinator = std::move(coordinator),
    };
  }

  moonlight_session_coordinator_t::moonlight_session_coordinator_t(
    moonlight_session_coordinator_options_t options,
    std::shared_ptr<moonlight_controller_feedback_hub_t> feedback_hub,
    std::unique_ptr<backend_t> backend
  ):
      options_(options),
      feedback_hub_(std::move(feedback_hub)),
      backend_(std::move(backend)),
      authority_(*backend_),
      activation_gate_(
        std::make_shared<moonlight_session_activation_gate_t>(
          options_.enabled,
          authority_,
          binding_registry_,
          feedback_hub_
        )
      ) {
  }

  moonlight_session_coordinator_t::~moonlight_session_coordinator_t() {
    (void) shutdown();
  }

  moonlight_coordinator_operation_status_e
  moonlight_session_coordinator_t::operation_status_locked() const {
    if (shutting_down_ || closed_) {
      return moonlight_coordinator_operation_status_e::shutting_down;
    }
    return options_.enabled ?
             moonlight_coordinator_operation_status_e::applied :
             moonlight_coordinator_operation_status_e::disabled;
  }

  moonlight_coordinator_reconcile_result_t
  moonlight_session_coordinator_t::reconcile_inputs(
    const std::vector<expectation_t> &expected
  ) {
    std::scoped_lock lock {state_mutex_};
    const auto status = operation_status_locked();
    if (status != moonlight_coordinator_operation_status_e::applied) {
      return {.status = status};
    }
    if (std::any_of(
          selections_.begin(),
          selections_.end(),
          [&expected](const auto &owner) {
            return std::none_of(
              expected.begin(),
              expected.end(),
              [&owner](const auto &expectation) {
                return expectation.handle == owner.handle;
              }
            );
          }
        )) {
      return {
        .status =
          moonlight_coordinator_operation_status_e::selection_retained,
      };
    }
    return {
      .status = status,
      .report = authority_.reconcile(expected),
    };
  }

  moonlight_coordinator_prepare_result_t
  moonlight_session_coordinator_t::prepare_input(
    const expectation_t &expectation
  ) {
    std::scoped_lock lock {state_mutex_};
    const auto status = operation_status_locked();
    if (status != moonlight_coordinator_operation_status_e::applied) {
      return {.status = status};
    }
    return {
      .status = status,
      .input = authority_.prepare(expectation),
    };
  }

  moonlight_coordinator_release_result_t
  moonlight_session_coordinator_t::release_input(
    const seat_handle_t &handle
  ) {
    std::scoped_lock lock {state_mutex_};
    const auto status = operation_status_locked();
    if (status != moonlight_coordinator_operation_status_e::applied) {
      return {.status = status};
    }
    if (std::any_of(
          selections_.begin(),
          selections_.end(),
          [&handle](const auto &owner) {
            return owner.handle == handle;
          }
        )) {
      return {
        .status =
          moonlight_coordinator_operation_status_e::selection_retained,
      };
    }
    return {
      .status = status,
      .input_status = authority_.release(handle),
    };
  }

  std::optional<moonlight_launch_selection_key_t>
  moonlight_session_coordinator_t::launch_key(
    const rtsp_stream::launch_session_t &launch
  ) {
    if (!launch.lifecycle_generation) {
      return std::nullopt;
    }
    moonlight_launch_selection_key_t key {
      .launch_session_id = launch.id,
      .lifecycle_generation = *launch.lifecycle_generation,
    };
    return key.valid() ? std::optional {key} : std::nullopt;
  }

  moonlight_launch_selection_status_e
  moonlight_session_coordinator_t::select_launch(
    const std::shared_ptr<rtsp_stream::launch_session_t> &launch,
    seat_handle_t handle,
    std::string_view expected_input_seat,
    bool controller_feedback
  ) {
    std::scoped_lock lock {state_mutex_};
    if (shutting_down_ || closed_) {
      return moonlight_launch_selection_status_e::gate_closed;
    }
    if (!options_.enabled) {
      return moonlight_launch_selection_status_e::gate_disabled;
    }
    if (!launch) {
      return moonlight_launch_selection_status_e::invalid_selection;
    }
    const auto key = launch_key(*launch);
    if (!key || !launch->is_pending()) {
      return moonlight_launch_selection_status_e::invalid_selection;
    }

    auto selected = activation_gate_->register_selection(
      *key,
      handle,
      expected_input_seat,
      controller_feedback
    );
    if (selected.status != moonlight_launch_selection_status_e::registered ||
        !selected.selection) {
      return selected.status;
    }
    selections_.push_back({
      .key = *key,
      .handle = std::move(handle),
      .launch = launch,
      .selection = std::move(selected.selection),
    });
    return moonlight_launch_selection_status_e::registered;
  }

  moonlight_coordinator_cancel_status_e
  moonlight_session_coordinator_t::cancel_launch(
    const std::shared_ptr<rtsp_stream::launch_session_t> &launch
  ) {
    std::scoped_lock lock {state_mutex_};
    if (shutting_down_ || closed_) {
      return moonlight_coordinator_cancel_status_e::coordinator_shutting_down;
    }
    if (!options_.enabled) {
      return moonlight_coordinator_cancel_status_e::coordinator_disabled;
    }
    if (!launch) {
      return moonlight_coordinator_cancel_status_e::invalid_launch;
    }
    const auto key = launch_key(*launch);
    if (!key) {
      return moonlight_coordinator_cancel_status_e::invalid_launch;
    }
    const auto found = std::find_if(
      selections_.begin(),
      selections_.end(),
      [&key](const auto &owner) {
        return owner.key == *key;
      }
    );
    if (found == selections_.end()) {
      return moonlight_coordinator_cancel_status_e::launch_not_found;
    }
    if (found->launch != launch) {
      return moonlight_coordinator_cancel_status_e::invalid_launch;
    }
    if (found->cancelled) {
      return moonlight_coordinator_cancel_status_e::already_cancelled;
    }
    found->selection->cancel();
    found->cancelled = true;
    return moonlight_coordinator_cancel_status_e::cancelled;
  }

  moonlight_coordinator_retire_status_e
  moonlight_session_coordinator_t::retire_cancelled_launch(
    const std::shared_ptr<rtsp_stream::launch_session_t> &launch
  ) {
    std::scoped_lock lock {state_mutex_};
    if (shutting_down_ || closed_) {
      return moonlight_coordinator_retire_status_e::coordinator_shutting_down;
    }
    if (!options_.enabled) {
      return moonlight_coordinator_retire_status_e::coordinator_disabled;
    }
    if (!launch) {
      return moonlight_coordinator_retire_status_e::invalid_launch;
    }
    const auto key = launch_key(*launch);
    if (!key) {
      return moonlight_coordinator_retire_status_e::invalid_launch;
    }
    const auto found = std::find_if(
      selections_.begin(),
      selections_.end(),
      [&key](const auto &owner) {
        return owner.key == *key;
      }
    );
    if (found == selections_.end()) {
      return moonlight_coordinator_retire_status_e::launch_not_found;
    }
    if (found->launch != launch) {
      return moonlight_coordinator_retire_status_e::invalid_launch;
    }
    if (!launch->is_cancelled()) {
      return moonlight_coordinator_retire_status_e::launch_still_admissible;
    }
    // The registry currently exposes a process-wide claim count. Refuse
    // conservatively while any selected stream can still reference authority;
    // a later seat-keyed teardown edge can narrow this without weakening it.
    if (binding_registry_.claimed_sessions() != 0) {
      return moonlight_coordinator_retire_status_e::stream_still_bound;
    }
    found->selection->cancel();
    found->selection->close();
    selections_.erase(found);
    return moonlight_coordinator_retire_status_e::retired;
  }

  moonlight_coordinator_shutdown_report_t
  moonlight_session_coordinator_t::shutdown() noexcept {
    std::scoped_lock shutdown_lock {shutdown_mutex_};
    {
      std::scoped_lock state_lock {state_mutex_};
      if (closed_) {
        return {
          .status = moonlight_coordinator_shutdown_status_e::already_closed,
        };
      }
      shutting_down_ = true;
    }

    // Keep the closed gate installed until every referenced dependency is
    // quiescent. New RTSP sessions fail closed throughout this interval.
    activation_gate_->close();
    binding_registry_.close();
    feedback_hub_->close();

    moonlight_coordinator_shutdown_report_t report;
    for (const auto &allocation : authority_.allocations()) {
      const auto released = authority_.release(allocation.handle);
      if (released == status_e::applied) {
        ++report.released_allocations;
      } else {
        ++report.cleanup_failures;
      }
    }
    if (report.cleanup_failures != 0) {
      report.status =
        moonlight_coordinator_shutdown_status_e::input_cleanup_incomplete;
      return report;
    }

    {
      std::scoped_lock state_lock {state_mutex_};
      selections_.clear();
      if (activation_installation_) {
        activation_installation_->close();
        activation_installation_.reset();
      }
      closed_ = true;
      report.status = moonlight_coordinator_shutdown_status_e::closed;
    }
    return report;
  }

  bool moonlight_session_coordinator_t::enabled() const {
    return options_.enabled;
  }

  bool moonlight_session_coordinator_t::activation_installed() const {
    std::scoped_lock lock {state_mutex_};
    return activation_installation_ && activation_installation_->installed();
  }

  bool moonlight_session_coordinator_t::shutting_down() const {
    std::scoped_lock lock {state_mutex_};
    return shutting_down_;
  }

  bool moonlight_session_coordinator_t::closed() const {
    std::scoped_lock lock {state_mutex_};
    return closed_;
  }

  std::size_t moonlight_session_coordinator_t::active_launches() const {
    std::scoped_lock lock {state_mutex_};
    return std::count_if(
      selections_.begin(),
      selections_.end(),
      [](const auto &owner) {
        return !owner.cancelled;
      }
    );
  }

  std::size_t moonlight_session_coordinator_t::retained_launches() const {
    std::scoped_lock lock {state_mutex_};
    return selections_.size();
  }

  std::size_t moonlight_session_coordinator_t::input_allocations() const {
    std::scoped_lock lock {state_mutex_};
    return authority_.allocations().size();
  }

  std::size_t moonlight_session_coordinator_t::registered_sessions() const {
    return binding_registry_.registered_sessions();
  }

  std::size_t moonlight_session_coordinator_t::claimed_sessions() const {
    return binding_registry_.claimed_sessions();
  }

  std::size_t moonlight_session_coordinator_t::feedback_subscriptions() const {
    return feedback_hub_->subscriptions();
  }

}  // namespace multiseat::input

#endif
