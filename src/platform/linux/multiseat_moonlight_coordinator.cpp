/**
 * @file src/platform/linux/multiseat_moonlight_coordinator.cpp
 * @brief Default-off owner for multiseat Moonlight input dependencies.
 */
#include "multiseat_moonlight_coordinator.h"

#ifdef __linux__

  #include "src/rtsp.h"

  #include <algorithm>
  #include <mutex>
  #include <utility>
  #include <vector>

namespace multiseat::input {

  struct moonlight_session_coordinator_t::impl_t {
    struct selection_owner_t {
      moonlight_launch_selection_key_t key;
      seat_handle_t handle;
      std::shared_ptr<rtsp_stream::launch_session_t> launch;
      bool cancelled = false;
      std::unique_ptr<moonlight_launch_selection_t> selection;
    };

    impl_t(
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

    // Declaration order is the ownership contract: reverse destruction keeps
    // the feedback hub and backend alive beyond authority and session state.
    const moonlight_session_coordinator_options_t options_;
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
      coordinator->impl_->activation_gate_
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
    coordinator->impl_->activation_installation_ = std::move(installed.installation);
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
      impl_(std::make_unique<impl_t>(
        options,
        std::move(feedback_hub),
        std::move(backend)
      )) {
  }

  moonlight_session_coordinator_t::~moonlight_session_coordinator_t() {
    if (!impl_) {
      return;
    }
    const auto report = shutdown();
    if (report.status == moonlight_coordinator_shutdown_status_e::closed ||
        report.status ==
          moonlight_coordinator_shutdown_status_e::already_closed) {
      return;
    }

    // The public owner may be destroyed directly. Remove its process-global
    // entry point, then retain the complete raw-reference graph so an already
    // bound stream or activation can never outlive authority or its backend.
    uninstall_activation();
    (void) impl_.release();
  }

  void moonlight_session_coordinator_t::uninstall_activation() noexcept {
    if (!impl_) {
      return;
    }
    std::scoped_lock state_lock {impl_->state_mutex_};
    // No new selection or input mutation may target a gate nobody can reach.
    impl_->shutting_down_ = true;
    if (impl_->activation_installation_) {
      impl_->activation_installation_->close();
      impl_->activation_installation_.reset();
    }
  }

  moonlight_coordinator_operation_status_e
  moonlight_session_coordinator_t::operation_status_locked() const {
    if (impl_->shutting_down_ || impl_->closed_) {
      return moonlight_coordinator_operation_status_e::shutting_down;
    }
    return impl_->options_.enabled ?
             moonlight_coordinator_operation_status_e::applied :
             moonlight_coordinator_operation_status_e::disabled;
  }

  moonlight_coordinator_reconcile_result_t
  moonlight_session_coordinator_t::reconcile_inputs(
    const std::vector<expectation_t> &expected
  ) {
    std::scoped_lock lock {impl_->state_mutex_};
    const auto status = operation_status_locked();
    if (status != moonlight_coordinator_operation_status_e::applied) {
      return {.status = status};
    }
    if (std::any_of(
          impl_->selections_.begin(),
          impl_->selections_.end(),
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
      .report = impl_->authority_.reconcile(expected),
    };
  }

  moonlight_coordinator_prepare_result_t
  moonlight_session_coordinator_t::prepare_input(
    const expectation_t &expectation
  ) {
    std::scoped_lock lock {impl_->state_mutex_};
    const auto status = operation_status_locked();
    if (status != moonlight_coordinator_operation_status_e::applied) {
      return {.status = status};
    }
    return {
      .status = status,
      .input = impl_->authority_.prepare(expectation),
    };
  }

  moonlight_coordinator_release_result_t
  moonlight_session_coordinator_t::release_input(
    const seat_handle_t &handle
  ) {
    std::scoped_lock lock {impl_->state_mutex_};
    const auto status = operation_status_locked();
    if (status != moonlight_coordinator_operation_status_e::applied) {
      return {.status = status};
    }
    if (std::any_of(
          impl_->selections_.begin(),
          impl_->selections_.end(),
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
      .input_status = impl_->authority_.release(handle),
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
    std::scoped_lock lock {impl_->state_mutex_};
    if (impl_->shutting_down_ || impl_->closed_) {
      return moonlight_launch_selection_status_e::gate_closed;
    }
    if (!impl_->options_.enabled) {
      return moonlight_launch_selection_status_e::gate_disabled;
    }
    if (!launch) {
      return moonlight_launch_selection_status_e::invalid_selection;
    }
    const auto key = launch_key(*launch);
    if (!key || !launch->is_pending()) {
      return moonlight_launch_selection_status_e::invalid_selection;
    }

    auto selected = impl_->activation_gate_->register_selection(
      *key,
      handle,
      expected_input_seat,
      controller_feedback
    );
    if (selected.status != moonlight_launch_selection_status_e::registered ||
        !selected.selection) {
      return selected.status;
    }
    impl_->selections_.push_back({
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
    std::scoped_lock lock {impl_->state_mutex_};
    if (impl_->shutting_down_ || impl_->closed_) {
      return moonlight_coordinator_cancel_status_e::coordinator_shutting_down;
    }
    if (!impl_->options_.enabled) {
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
      impl_->selections_.begin(),
      impl_->selections_.end(),
      [&key](const auto &owner) {
        return owner.key == *key;
      }
    );
    if (found == impl_->selections_.end()) {
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
    std::scoped_lock lock {impl_->state_mutex_};
    if (impl_->shutting_down_ || impl_->closed_) {
      return moonlight_coordinator_retire_status_e::coordinator_shutting_down;
    }
    if (!impl_->options_.enabled) {
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
      impl_->selections_.begin(),
      impl_->selections_.end(),
      [&key](const auto &owner) {
        return owner.key == *key;
      }
    );
    if (found == impl_->selections_.end()) {
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
    if (impl_->binding_registry_.claimed_sessions() != 0) {
      return moonlight_coordinator_retire_status_e::stream_still_bound;
    }
    found->selection->cancel();
    found->selection->close();
    impl_->selections_.erase(found);
    return moonlight_coordinator_retire_status_e::retired;
  }

  moonlight_coordinator_shutdown_report_t
  moonlight_session_coordinator_t::shutdown() noexcept {
    moonlight_coordinator_shutdown_report_t report;
    try {
      std::scoped_lock shutdown_lock {impl_->shutdown_mutex_};
      const auto quiesced = quiesce_locked();
      if (quiesced.status ==
          moonlight_coordinator_quiesce_status_e::already_closed) {
        report.status =
          moonlight_coordinator_shutdown_status_e::already_closed;
        return report;
      }
      if (quiesced.status ==
          moonlight_coordinator_quiesce_status_e::streams_pending) {
        report.status = moonlight_coordinator_shutdown_status_e::streams_pending;
        return report;
      }
      if (!impl_->activation_gate_->finish_close() ||
          !impl_->binding_registry_.finish_close()) {
        report.status = moonlight_coordinator_shutdown_status_e::streams_pending;
        return report;
      }

      impl_->feedback_hub_->close();

      const auto cleanup = impl_->authority_.release_all();
      report.released_allocations = cleanup.released_allocations;
      report.cleanup_failures = cleanup.cleanup_failures;
      if (report.cleanup_failures != 0) {
        return report;
      }

      {
        std::scoped_lock state_lock {impl_->state_mutex_};
        impl_->selections_.clear();
        if (impl_->activation_installation_) {
          impl_->activation_installation_->close();
          impl_->activation_installation_.reset();
        }
        impl_->closed_ = true;
        report.status = moonlight_coordinator_shutdown_status_e::closed;
      }
      return report;
    } catch (...) {
      ++report.cleanup_failures;
      return report;
    }
  }

  moonlight_coordinator_quiesce_report_t
  moonlight_session_coordinator_t::quiesce() noexcept {
    std::scoped_lock shutdown_lock {impl_->shutdown_mutex_};
    return quiesce_locked();
  }

  moonlight_coordinator_quiesce_report_t
  moonlight_session_coordinator_t::quiesce_locked() noexcept {
    {
      std::scoped_lock state_lock {impl_->state_mutex_};
      if (impl_->closed_) {
        return {
          .status = moonlight_coordinator_quiesce_status_e::already_closed,
        };
      }
      impl_->shutting_down_ = true;
    }

    // Closing the registry first is the commit barrier. An activation which
    // has not already claimed a binding cannot acquire one after this point;
    // closing the gate then prevents any new activation from entering.
    const auto claimed_sessions = impl_->binding_registry_.quiesce();
    const auto activations_in_flight = impl_->activation_gate_->quiesce();
    const auto status = claimed_sessions != 0 || activations_in_flight != 0 ?
                          moonlight_coordinator_quiesce_status_e::streams_pending :
                          moonlight_coordinator_quiesce_status_e::quiesced;
    return {
      .status = status,
      .claimed_sessions = claimed_sessions,
      .activations_in_flight = activations_in_flight,
    };
  }

  bool moonlight_session_coordinator_t::enabled() const {
    return impl_->options_.enabled;
  }

  bool moonlight_session_coordinator_t::activation_installed() const {
    std::scoped_lock lock {impl_->state_mutex_};
    return impl_->activation_installation_ &&
           impl_->activation_installation_->installed();
  }

  bool moonlight_session_coordinator_t::shutting_down() const {
    std::scoped_lock lock {impl_->state_mutex_};
    return impl_->shutting_down_;
  }

  bool moonlight_session_coordinator_t::closed() const {
    std::scoped_lock lock {impl_->state_mutex_};
    return impl_->closed_;
  }

  std::size_t moonlight_session_coordinator_t::active_launches() const {
    std::scoped_lock lock {impl_->state_mutex_};
    return std::count_if(
      impl_->selections_.begin(),
      impl_->selections_.end(),
      [](const auto &owner) {
        return !owner.cancelled;
      }
    );
  }

  std::size_t moonlight_session_coordinator_t::retained_launches() const {
    std::scoped_lock lock {impl_->state_mutex_};
    return impl_->selections_.size();
  }

  std::size_t moonlight_session_coordinator_t::input_allocations() const {
    std::scoped_lock lock {impl_->state_mutex_};
    return impl_->authority_.allocations().size();
  }

  std::optional<allocation_t>
  moonlight_session_coordinator_t::input_allocation(
    const seat_handle_t &handle
  ) const {
    std::scoped_lock lock {impl_->state_mutex_};
    // Readable while shutting down: the authoritative worker inventory that
    // proves a seat's worker is gone runs after quiesce and before close.
    if (!impl_->options_.enabled || impl_->closed_) {
      return std::nullopt;
    }
    return impl_->authority_.allocation(handle);
  }

  std::size_t moonlight_session_coordinator_t::registered_sessions() const {
    return impl_->binding_registry_.registered_sessions();
  }

  std::size_t moonlight_session_coordinator_t::claimed_sessions() const {
    return impl_->binding_registry_.claimed_sessions();
  }

  std::size_t moonlight_session_coordinator_t::feedback_subscriptions() const {
    return impl_->feedback_hub_->subscriptions();
  }

}  // namespace multiseat::input

#endif
