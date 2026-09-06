/**
 * @file src/platform/linux/multiseat_controller_runtime.cpp
 * @brief Default-off owner for trusted multiseat launch composition.
 */
#include "multiseat_controller_runtime.h"

#ifdef __linux__

  #include <algorithm>
  #include <mutex>
  #include <utility>

namespace multiseat {
  namespace {
    bool valid_expectation_syntax(
      const std::vector<input::expectation_t> &expectations
    ) {
      for (std::size_t index = 0; index < expectations.size(); ++index) {
        const auto &candidate = expectations[index];
        if (!candidate.handle.valid() || candidate.input_seat.empty() ||
            !input::valid_plan(candidate.plan)) {
          return false;
        }
        for (std::size_t prior = 0; prior < index; ++prior) {
          if (expectations[prior].handle == candidate.handle ||
              expectations[prior].input_seat == candidate.input_seat) {
            return false;
          }
        }
      }
      return true;
    }

    bool input_release_complete(
      const input::moonlight_coordinator_release_result_t &released
    ) {
      if (released.status !=
          input::moonlight_coordinator_operation_status_e::applied) {
        return false;
      }
      return released.input_status == input::status_e::applied ||
             released.input_status == input::status_e::not_found;
    }

    bool worker_start_indeterminate(coordinator_start_result_e status) {
      return status == coordinator_start_result_e::backend_indeterminate ||
             status == coordinator_start_result_e::cleanup_blocked;
    }
  }  // namespace

  struct controller_runtime_t::impl_t {
    explicit impl_t(controller_runtime_dependencies_t dependencies) :
        registry(std::move(dependencies.registry)),
        worker_authority_store(
          std::move(dependencies.worker_authority_store)
        ),
        moonlight_runtime(std::move(dependencies.moonlight_runtime)),
        worker_backend_dependencies(
          std::move(dependencies.worker_backend_dependencies)
        ),
        worker_backend(std::move(dependencies.worker_backend)),
        input_expectations(
          std::move(dependencies.recovered_input_expectations)
        ) {
      workers = std::make_unique<worker_coordinator_t>(
        *registry,
        *worker_backend,
        *worker_authority_store,
        dependencies.worker_options,
        std::move(dependencies.now),
        std::move(dependencies.session_factory)
      );
      launch_adapter =
        std::make_unique<input::moonlight_worker_launch_adapter_t>(*workers);
    }

    // Reverse destruction is deliberate: adapter, coordinator, worker
    // backend, its opaque dependencies, Moonlight input, authority store,
    // then the shared registry.
    std::unique_ptr<registry_t> registry;
    std::unique_ptr<worker_ipc::authority_store_t> worker_authority_store;
    std::unique_ptr<input::moonlight_session_runtime_t> moonlight_runtime;
    std::shared_ptr<void> worker_backend_dependencies;
    std::unique_ptr<worker_backend_t> worker_backend;
    std::unique_ptr<worker_coordinator_t> workers;
    std::unique_ptr<input::moonlight_worker_launch_adapter_t> launch_adapter;
    mutable std::mutex state_mutex;
    std::mutex shutdown_mutex;
    std::vector<input::expectation_t> input_expectations;
    bool admission_ready = false;
    bool shutting_down = false;
    bool closed = false;
  };

  controller_runtime_t::controller_runtime_t(std::unique_ptr<impl_t> impl) :
      impl_(std::move(impl)) {
  }

  controller_runtime_t::~controller_runtime_t() {
    if (!impl_) {
      return;
    }
    const auto report = shutdown();
    if (!report.closed()) {
      // Exact worker or input ownership is still live or indeterminate. The
      // process-global Moonlight callbacks still point into this graph, so
      // fail-closed retention is safer than destroying authority underneath
      // them. Callers must normally retry shutdown instead of reaching here.
      (void) impl_.release();
    }
  }

  controller_runtime_create_result_t controller_runtime_t::create(
    controller_runtime_options_t options,
    controller_runtime_dependencies_factory_t dependencies_factory
  ) {
    if (!options.enabled) {
      return {
        .status = controller_runtime_create_status_e::ready_disabled,
      };
    }
    if (!dependencies_factory) {
      return {
        .status = controller_runtime_create_status_e::invalid_factory,
      };
    }

    std::optional<controller_runtime_dependencies_t> dependencies;
    try {
      dependencies = dependencies_factory();
    } catch (...) {
      return {
        .status = controller_runtime_create_status_e::dependencies_unavailable,
      };
    }
    if (!dependencies) {
      return {
        .status = controller_runtime_create_status_e::dependencies_unavailable,
      };
    }
    if (!dependencies->registry || !dependencies->worker_authority_store ||
        !dependencies->moonlight_runtime || !dependencies->worker_backend ||
        dependencies->worker_authority_store->status() !=
          worker_ipc::authority_status_e::applied ||
        !dependencies->moonlight_runtime->installed() ||
        dependencies->moonlight_runtime->shutting_down() ||
        dependencies->moonlight_runtime->closed() ||
        !valid_expectation_syntax(
          dependencies->recovered_input_expectations
        )) {
      return {
        .status = controller_runtime_create_status_e::invalid_dependencies,
      };
    }

    try {
      return {
        .status = controller_runtime_create_status_e::ready_enabled,
        .runtime = std::unique_ptr<controller_runtime_t> {
          new controller_runtime_t(
            std::make_unique<impl_t>(std::move(*dependencies))
          )
        },
      };
    } catch (...) {
      return {
        .status = controller_runtime_create_status_e::construction_failed,
      };
    }
  }

  controller_reconcile_result_t controller_runtime_t::reconcile() {
    std::scoped_lock lock {impl_->state_mutex};
    controller_reconcile_result_t result;
    if (impl_->shutting_down || impl_->closed) {
      result.status = controller_reconcile_status_e::controller_shutting_down;
      return result;
    }
    impl_->admission_ready = false;
    if (!valid_expectation_syntax(impl_->input_expectations)) {
      result.status =
        controller_reconcile_status_e::invalid_input_expectations;
      return result;
    }

    try {
      result.worker = impl_->workers->reconcile();
    } catch (...) {
      result.status =
        controller_reconcile_status_e::worker_reconciliation_required;
      return result;
    }
    if (!result.worker.admission_ready) {
      result.status =
        controller_reconcile_status_e::worker_reconciliation_required;
      return result;
    }

    try {
      for (std::size_t index = 0;
           index < impl_->input_expectations.size();) {
        const auto &expectation = impl_->input_expectations[index];
        const auto seat = impl_->registry->snapshot(expectation.handle);
        if (seat) {
          if (seat->resources.input_seat != expectation.input_seat) {
            result.status =
              controller_reconcile_status_e::invalid_input_expectations;
            return result;
          }
          ++index;
          continue;
        }

        const auto released =
          impl_->moonlight_runtime->release_input(expectation.handle);
        if (!input_release_complete(released)) {
          result.status =
            controller_reconcile_status_e::input_reconciliation_required;
          return result;
        }
        impl_->input_expectations.erase(
          impl_->input_expectations.begin() +
            static_cast<std::ptrdiff_t>(index)
        );
      }

      for (const auto &seat : impl_->registry->seats()) {
        if (seat.state == seat_state_e::reserved) {
          continue;
        }
        const auto expected = std::find_if(
          impl_->input_expectations.begin(),
          impl_->input_expectations.end(),
          [&seat](const auto &candidate) {
            return candidate.handle == seat.handle &&
                   candidate.input_seat == seat.resources.input_seat;
          }
        );
        if (expected == impl_->input_expectations.end()) {
          result.status =
            controller_reconcile_status_e::invalid_input_expectations;
          return result;
        }
      }

      result.input = impl_->moonlight_runtime->reconcile_inputs(
        impl_->input_expectations
      );
    } catch (...) {
      result.status =
        controller_reconcile_status_e::input_reconciliation_required;
      return result;
    }
    if (result.input->status !=
          input::moonlight_coordinator_operation_status_e::applied ||
        !result.input->report.admission_ready) {
      result.status =
        controller_reconcile_status_e::input_reconciliation_required;
      return result;
    }

    impl_->admission_ready = true;
    result.status = controller_reconcile_status_e::ready;
    return result;
  }

  admission_result_t controller_runtime_t::admit(
    const seat_request_t &request
  ) {
    std::scoped_lock lock {impl_->state_mutex};
    if (!impl_->admission_ready || impl_->shutting_down || impl_->closed) {
      return {.rejection = admission_rejection_e::invalid_request};
    }
    try {
      return impl_->registry->admit(request);
    } catch (...) {
      impl_->admission_ready = false;
      return {.rejection = admission_rejection_e::invalid_request};
    }
  }

  mutation_result_e controller_runtime_t::bind_runtime(
    const seat_handle_t &handle,
    compositor_e selected,
    std::string selection_reason
  ) {
    std::scoped_lock lock {impl_->state_mutex};
    if (!impl_->admission_ready || impl_->shutting_down || impl_->closed) {
      return mutation_result_e::invalid_state;
    }
    try {
      return impl_->registry->bind_runtime(
        handle,
        selected,
        std::move(selection_reason)
      );
    } catch (...) {
      impl_->admission_ready = false;
      return mutation_result_e::invalid_state;
    }
  }

  controller_start_result_t controller_runtime_t::start_seat(
    const seat_handle_t &handle,
    input::plan_t input_plan
  ) {
    std::scoped_lock lock {impl_->state_mutex};
    controller_start_result_t result;
    if (!impl_->admission_ready || impl_->shutting_down || impl_->closed) {
      result.status = controller_start_status_e::controller_not_ready;
      return result;
    }
    if (!handle.valid() || !input::valid_plan(input_plan)) {
      result.status = controller_start_status_e::invalid_request;
      return result;
    }
    const auto seat = impl_->registry->snapshot(handle);
    if (!seat || seat->state != seat_state_e::reserved ||
        seat->resources.input_seat.empty()) {
      result.status = controller_start_status_e::invalid_request;
      return result;
    }

    input::expectation_t expectation {
      .handle = handle,
      .input_seat = seat->resources.input_seat,
      .plan = input_plan,
    };
    const auto existing = std::find_if(
      impl_->input_expectations.begin(),
      impl_->input_expectations.end(),
      [&handle](const auto &candidate) {
        return candidate.handle == handle;
      }
    );
    if (existing != impl_->input_expectations.end() &&
        *existing != expectation) {
      result.status = controller_start_status_e::invalid_request;
      return result;
    }

    try {
      result.input = impl_->moonlight_runtime->prepare_input(expectation);
    } catch (...) {
      impl_->admission_ready = false;
      result.status = controller_start_status_e::input_rejected;
      return result;
    }
    if (result.input.status !=
          input::moonlight_coordinator_operation_status_e::applied ||
        !result.input.input.prepared()) {
      impl_->admission_ready = false;
      result.status = controller_start_status_e::input_rejected;
      return result;
    }
    if (existing == impl_->input_expectations.end()) {
      try {
        impl_->input_expectations.push_back(expectation);
      } catch (...) {
        impl_->admission_ready = false;
        try {
          result.rollback = impl_->moonlight_runtime->release_input(handle);
        } catch (...) {
        }
        if (result.rollback && input_release_complete(*result.rollback)) {
          result.status = controller_start_status_e::input_rejected;
        } else {
          result.status = controller_start_status_e::input_cleanup_incomplete;
        }
        return result;
      }
    }

    try {
      result.worker = impl_->workers->start_seat(handle);
    } catch (...) {
      result.worker = coordinator_start_result_e::backend_indeterminate;
    }
    if (result.worker == coordinator_start_result_e::started) {
      result.status = controller_start_status_e::started;
      return result;
    }

    impl_->admission_ready = false;
    if (worker_start_indeterminate(*result.worker)) {
      result.status = controller_start_status_e::worker_indeterminate;
      return result;
    }

    try {
      result.rollback = impl_->moonlight_runtime->release_input(handle);
    } catch (...) {
      result.status = controller_start_status_e::input_cleanup_incomplete;
      return result;
    }
    if (!input_release_complete(*result.rollback)) {
      result.status = controller_start_status_e::input_cleanup_incomplete;
      return result;
    }
    impl_->input_expectations.erase(
      std::remove_if(
        impl_->input_expectations.begin(),
        impl_->input_expectations.end(),
        [&handle](const auto &candidate) {
          return candidate.handle == handle;
        }
      ),
      impl_->input_expectations.end()
    );
    result.status = controller_start_status_e::worker_rejected;
    return result;
  }

  input::moonlight_worker_selection_result_t
  controller_runtime_t::select_authenticated_launch(
    const std::shared_ptr<rtsp_stream::launch_session_t> &launch,
    const seat_handle_t &handle
  ) {
    std::scoped_lock lock {impl_->state_mutex};
    input::moonlight_worker_selection_result_t rejected;
    if (!impl_->admission_ready || impl_->shutting_down || impl_->closed) {
      rejected.status =
        input::moonlight_worker_selection_status_e::worker_not_authorized;
      rejected.authority_status =
        worker_seat_authorization_status_e::reconciliation_required;
      return rejected;
    }
    const auto expected = std::find_if(
      impl_->input_expectations.begin(),
      impl_->input_expectations.end(),
      [&handle](const auto &candidate) {
        return candidate.handle == handle;
      }
    );
    if (expected == impl_->input_expectations.end()) {
      rejected.status =
        input::moonlight_worker_selection_status_e::authority_mismatch;
      return rejected;
    }
    try {
      return impl_->launch_adapter->select(launch, handle);
    } catch (...) {
      rejected.status =
        input::moonlight_worker_selection_status_e::worker_not_authorized;
      rejected.authority_status =
        worker_seat_authorization_status_e::action_failed;
      return rejected;
    }
  }

  controller_stop_result_t controller_runtime_t::stop_seat(
    const seat_handle_t &handle
  ) {
    std::scoped_lock lock {impl_->state_mutex};
    controller_stop_result_t result;
    if (impl_->shutting_down || impl_->closed) {
      result.status = controller_stop_status_e::controller_shutting_down;
      return result;
    }
    if (!handle.valid()) {
      return result;
    }

    impl_->admission_ready = false;
    try {
      result.worker = impl_->workers->stop_seat(handle);
    } catch (...) {
      result.status = controller_stop_status_e::stopping;
      return result;
    }
    std::vector<worker_identity_t> managed;
    try {
      managed = impl_->workers->managed_workers();
    } catch (...) {
      result.status = controller_stop_status_e::stopping;
      return result;
    }
    const auto worker_retained = std::any_of(
      managed.begin(),
      managed.end(),
      [&handle](const auto &identity) {
        return identity.seat == handle;
      }
    );
    const auto worker_absent =
      !impl_->registry->snapshot(handle) && !worker_retained;
    if (!worker_absent) {
      result.status = controller_stop_status_e::stopping;
      return result;
    }

    const auto expected = std::find_if(
      impl_->input_expectations.begin(),
      impl_->input_expectations.end(),
      [&handle](const auto &candidate) {
        return candidate.handle == handle;
      }
    );
    if (expected == impl_->input_expectations.end()) {
      result.status = controller_stop_status_e::released;
      return result;
    }
    try {
      result.input = impl_->moonlight_runtime->release_input(handle);
    } catch (...) {
      result.status = controller_stop_status_e::cleanup_pending;
      return result;
    }
    if (!input_release_complete(*result.input)) {
      result.status = controller_stop_status_e::cleanup_pending;
      return result;
    }
    impl_->input_expectations.erase(expected);
    result.status = controller_stop_status_e::released;
    return result;
  }

  controller_shutdown_report_t controller_runtime_t::shutdown() noexcept {
    controller_shutdown_report_t result;
    try {
      std::scoped_lock shutdown_lock {impl_->shutdown_mutex};
      std::scoped_lock state_lock {impl_->state_mutex};
      if (impl_->closed) {
        result.status = controller_shutdown_status_e::already_closed;
        return result;
      }
      impl_->shutting_down = true;
      impl_->admission_ready = false;

      for (const auto &seat : impl_->registry->seats()) {
        const auto stopped = impl_->workers->stop_seat(seat.handle);
        if (stopped.broker != broker_stop_result_e::seat_not_found) {
          ++result.stop_requests;
        }
      }
      result.worker = impl_->workers->reconcile();
      if (!result.worker->admission_ready ||
          !impl_->registry->seats().empty() ||
          !impl_->workers->managed_workers().empty()) {
        result.status = controller_shutdown_status_e::workers_pending;
        return result;
      }
      if (impl_->moonlight_runtime->claimed_sessions() != 0) {
        result.status = controller_shutdown_status_e::streams_pending;
        return result;
      }

      result.status = controller_shutdown_status_e::input_cleanup_incomplete;
      result.input = impl_->moonlight_runtime->shutdown();
      if (result.input->status !=
            input::moonlight_coordinator_shutdown_status_e::closed &&
          result.input->status !=
            input::moonlight_coordinator_shutdown_status_e::already_closed) {
        return result;
      }
      impl_->input_expectations.clear();
      impl_->closed = true;
      result.status = controller_shutdown_status_e::closed;
      return result;
    } catch (...) {
      return result;
    }
  }

  bool controller_runtime_t::admission_ready() const {
    std::scoped_lock lock {impl_->state_mutex};
    return impl_->admission_ready && !impl_->shutting_down && !impl_->closed;
  }

  bool controller_runtime_t::shutting_down() const {
    std::scoped_lock lock {impl_->state_mutex};
    return impl_->shutting_down;
  }

  bool controller_runtime_t::closed() const {
    std::scoped_lock lock {impl_->state_mutex};
    return impl_->closed;
  }

  std::size_t controller_runtime_t::seats() const {
    std::scoped_lock lock {impl_->state_mutex};
    return impl_->registry->seats().size();
  }

  std::size_t controller_runtime_t::managed_workers() const {
    std::scoped_lock lock {impl_->state_mutex};
    return impl_->workers->managed_workers().size();
  }

  std::size_t controller_runtime_t::input_allocations() const {
    std::scoped_lock lock {impl_->state_mutex};
    return impl_->moonlight_runtime->input_allocations();
  }

  std::size_t controller_runtime_t::tracked_launches() const {
    std::scoped_lock lock {impl_->state_mutex};
    return impl_->moonlight_runtime->tracked_launches();
  }

}  // namespace multiseat

#endif
