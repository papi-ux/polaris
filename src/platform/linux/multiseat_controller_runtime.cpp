/**
 * @file src/platform/linux/multiseat_controller_runtime.cpp
 * @brief Default-off owner for trusted multiseat launch composition.
 */
#include "multiseat_controller_runtime.h"
#include "src/rtsp.h"

#ifdef __linux__

  #include <algorithm>
  #include <mutex>
  #include <string_view>
  #include <unordered_set>
  #include <utility>

namespace multiseat {
  namespace {
    bool routing_key(std::string_view key) {
      return !key.empty() && key.size() <= 128 &&
             key.find_first_not_of(
               "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_."
             ) == std::string_view::npos;
    }

    bool valid_profile_routes(const controller_runtime_options_t &options) {
      if (options.profile_routes.empty()) {
        return true;
      }
      if (!options.worker_media_enabled || options.profile_routes.size() > 4096) {
        return false;
      }
      if (options.profile_launch_timeout <= std::chrono::milliseconds::zero() ||
          options.profile_launch_timeout > std::chrono::minutes {5}) {
        return false;
      }
      std::unordered_set<std::string> profiles;
      std::unordered_set<std::string> clients;
      std::size_t grant_count = 0;
      for (const auto &route : options.profile_routes) {
        if (!routing_key(route.profile_key) ||
            !profiles.insert(route.profile_key).second ||
            !valid_workload_plan(route.workload) ||
            !workload_matches_runtime_profile(route.workload, route.runtime_profile) ||
            route.client_keys.size() > 4096 || route.access_clients.size() > 4096 || route.logical_gpu_ids.empty() ||
            route.logical_gpu_ids.size() > 64) {
          return false;
        }
        for (const auto &client : route.client_keys) {
          if (!routing_key(client) || !clients.insert(client).second || clients.size() > 65536) {
            return false;
          }
        }
        std::unordered_set<std::string> access;
        for (const auto &client : route.access_clients)
          if (!routing_key(client) || !access.insert(client).second || ++grant_count > 65536) return false;
        std::unordered_set<std::string> gpus;
        for (const auto &gpu : route.logical_gpu_ids) {
          if (!routing_key(gpu) || !gpus.insert(gpu).second) {
            return false;
          }
        }
      }
      return true;
    }

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
    struct owned_profile_launch_t {
      std::weak_ptr<rtsp_stream::launch_session_t> launch;
      input::moonlight_launch_selection_key_t key;
      std::string client_key;
      std::string profile_key;
      seat_handle_t handle;
      worker_broker_t::time_point_t deadline;
      bool cleanup_requested = false;
      bool selection_registered = false;
    };

    explicit impl_t(controller_runtime_dependencies_t dependencies, controller_runtime_options_t options) :
        profile_catalog_lease(std::move(dependencies.profile_catalog_lease)),
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
        ), worker_media_enabled(options.worker_media_enabled),
        profile_routes(std::move(options.profile_routes)),
        profile_catalog(std::move(options.profile_catalog)),
        profile_launch_timeout(options.profile_launch_timeout),
        now(dependencies.now) {
      if (!now) now = [] { return worker_broker_t::monotonic_clock_t::now(); };
      profile_launches.reserve(input::maximum_input_allocations);
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
    std::shared_ptr<void> profile_catalog_lease;
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
    const bool worker_media_enabled;
    const std::vector<controller_profile_route_t> profile_routes;
    const std::vector<profile_summary_t> profile_catalog;
    const std::chrono::milliseconds profile_launch_timeout;
    worker_broker_t::now_fn_t now;
    std::vector<owned_profile_launch_t> profile_launches;
    bool admission_ready = false;
    bool shutting_down = false;
    bool closed = false;

    auto profile_launch(const seat_handle_t &handle) {
      return std::find_if(profile_launches.begin(), profile_launches.end(),
        [&handle](const auto &entry) { return entry.handle == handle; });
    }

    bool cleanup_needed(owned_profile_launch_t &entry) {
      const auto launch = entry.launch.lock();
      entry.cleanup_requested = entry.cleanup_requested || !launch ||
        launch->is_cancelled() || launch->id != entry.key.launch_session_id ||
        launch->lifecycle_generation != entry.key.lifecycle_generation ||
        launch->unique_id != entry.client_key ||
        ((!entry.selection_registered ||
           launch->setup_state.load() != rtsp_stream::launch_session_t::setup_state_e::started) &&
          now() >= entry.deadline);
      return entry.cleanup_requested;
    }

    bool profile_cancelled(const seat_handle_t &handle) {
      const auto entry = profile_launch(handle);
      return entry != profile_launches.end() && cleanup_needed(*entry);
    }
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
      // Exact worker or input ownership is still live or indeterminate. Apply
      // the same fail-closed policy as the Moonlight runtime and coordinator:
      // detach every process-global entry point this graph installed, then
      // retain the graph rather than destroying authority underneath a bound
      // stream. Callers must normally retry shutdown instead of reaching here.
      impl_->moonlight_runtime->detach_process_globals();
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
    if (!valid_profile_routes(options)) {
      return {.status = controller_runtime_create_status_e::invalid_dependencies};
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
            std::make_unique<impl_t>(std::move(*dependencies), std::move(options))
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
      // Network cancellation only marks the retained launch. Worker commands
      // run here under the controller owner, never on an HTTP/RTSP timer thread.
      for (auto &entry : impl_->profile_launches) {
        if (!impl_->cleanup_needed(entry)) continue;
        if (auto launch = entry.launch.lock()) launch->cancel();
        const auto quiesced = impl_->moonlight_runtime->quiesce_seat(entry.handle);
        if (quiesced == input::moonlight_runtime_lifecycle_status_e::retained_until_streams_close) {
          continue;
        }
        if (quiesced != input::moonlight_runtime_lifecycle_status_e::retired &&
            quiesced != input::moonlight_runtime_lifecycle_status_e::not_selected) {
          result.status = controller_reconcile_status_e::input_reconciliation_required;
          return result;
        }
        (void) impl_->workers->stop_seat(entry.handle);
      }
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

    std::erase_if(impl_->profile_launches, [&](const auto &entry) {
      return !impl_->registry->snapshot(entry.handle) &&
        std::none_of(impl_->input_expectations.begin(), impl_->input_expectations.end(),
          [&](const auto &expectation) { return expectation.handle == entry.handle; });
    });
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

  controller_profile_admission_result_t
  controller_runtime_t::admit_authenticated_profile_launch(
    const std::shared_ptr<rtsp_stream::launch_session_t> &launch,
    seat_display_mode_t display_mode
  ) {
    using status_e = controller_profile_admission_status_e;
    std::scoped_lock lock {impl_->state_mutex};
    if (impl_->profile_routes.empty()) {
      return {.status = status_e::unselected};
    }
    if (!launch || launch->unique_id.empty()) {
      return {.status = status_e::invalid_launch};
    }
    const auto selected = launch->worker_profile_key.empty() ? profile_for_client(launch->unique_id) :
      std::optional<std::string>{launch->worker_profile_key};
    const auto route = std::find_if(impl_->profile_routes.begin(), impl_->profile_routes.end(),
      [&](const auto &candidate) {
        return selected == candidate.profile_key &&
          (std::find(candidate.client_keys.begin(), candidate.client_keys.end(), launch->unique_id) != candidate.client_keys.end() ||
           std::find(candidate.access_clients.begin(), candidate.access_clients.end(), launch->unique_id) != candidate.access_clients.end());
      });
    if (route == impl_->profile_routes.end()) {
      if (!launch->worker_profile_key.empty() || routes_client(launch->unique_id)) {
        launch->require_worker_connection();
        return {.status = status_e::invalid_launch};
      }
      return {.status = status_e::unselected};
    }
    // Routing is sticky even if admission or a later worker step fails. Stream
    // startup must never silently capture the host for a selected profile.
    launch->require_worker_connection();
    if (launch->id == 0 || !launch->lifecycle_generation ||
        *launch->lifecycle_generation == 0 || !launch->is_pending() ||
        launch->watch_only || launch->input_only || launch->temporary_authorization ||
        !(launch->perm & crypto::PERM::launch)) {
      return {.status = status_e::invalid_launch};
    }
    if (!impl_->admission_ready || impl_->shutting_down || impl_->closed) {
      return {.status = status_e::controller_not_ready};
    }
    const seat_request_t request {
      .client_key = launch->unique_id,
      .profile_key = route->profile_key,
      .workload = route->workload,
      .logical_gpu_id = {},
      .runtime_profile = route->runtime_profile,
      .data_plane = {
        .display_topology = display_topology_e::capture_host_with_nested_compositor,
        .media_pipeline = media_pipeline_e::worker_local_capture_encode,
      },
      .display_mode = display_mode,
      .requested_compositor = compositor_e::gamescope,
      .encoder_sessions = 1,
    };
    try {
      if (impl_->profile_launches.size() >= input::maximum_input_allocations) {
        return {.status = status_e::controller_not_ready};
      }
      impl_t::owned_profile_launch_t owned {
        .launch = launch,
        .key = {launch->id, *launch->lifecycle_generation},
        .client_key = launch->unique_id,
        .profile_key = route->profile_key,
        .deadline = impl_->now() + impl_->profile_launch_timeout,
      };
      auto admission = impl_->registry->admit_first_available(request, route->logical_gpu_ids);
      if (admission.accepted()) {
        try {
          owned.handle = admission.seat->handle;
          impl_->profile_launches.push_back(std::move(owned));
        } catch (...) {
          // No worker/input exists yet. A bookkeeping allocation failure must
          // not strand an unowned reservation in the registry.
          (void) impl_->workers->stop_seat(admission.seat->handle);
          throw;
        }
      }
      const auto status = admission.accepted() ? status_e::admitted : status_e::rejected;
      return {.status = status, .admission = std::move(admission)};
    } catch (...) {
      impl_->admission_ready = false;
      return {.status = status_e::controller_not_ready};
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
    const auto prior = impl_->profile_launch(handle);
    if (prior != impl_->profile_launches.end() && !impl_->cleanup_needed(*prior)) {
      const auto seat = impl_->registry->snapshot(handle);
      if (seat && seat->state != seat_state_e::reserved) {
        // A duplicate start is not a failure of the already running launch.
        return result;
      }
    }
    try {
      result = start_seat_locked(handle, input_plan);
    } catch (...) {
      impl_->admission_ready = false;
      result.status = controller_start_status_e::worker_indeterminate;
    }
    const auto entry = impl_->profile_launch(handle);
    if (entry != impl_->profile_launches.end()) {
      if (impl_->cleanup_needed(*entry)) result.status = controller_start_status_e::launch_cancelled;
      if (!result.started()) entry->cleanup_requested = true;
    }
    return result;
  }

  controller_start_result_t controller_runtime_t::start_seat_locked(
    const seat_handle_t &handle, input::plan_t input_plan
  ) {
    controller_start_result_t result;
    if (impl_->profile_cancelled(handle)) {
      result.status = controller_start_status_e::launch_cancelled;
      return result;
    }
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

    if (impl_->profile_cancelled(handle)) {
      result.status = controller_start_status_e::launch_cancelled;
      return result;
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
    const auto owned = impl_->profile_launch(handle);
    if (owned != impl_->profile_launches.end()) {
      // The original retained launch owns this profile reservation. A second
      // object, even with identical fields, cannot replace or cancel it.
      if (owned->launch.lock() != launch) return rejected;
      if (impl_->cleanup_needed(*owned)) return rejected;
      if (owned->selection_registered) return rejected;
    }
    // A failed selected-media launch must retain its requirement so a caller
    // cannot accidentally start ordinary host capture after selection failed.
    if (impl_->worker_media_enabled && launch) launch->require_worker_connection();
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
      auto selected = impl_->worker_media_enabled ?
        impl_->launch_adapter->select_with_connection(launch, handle) :
        impl_->launch_adapter->select(launch, handle);
      if (owned != impl_->profile_launches.end()) {
        owned->selection_registered = selected.selected();
        owned->cleanup_requested = !selected.selected();
      }
      return selected;
    } catch (...) {
      rejected.status =
        input::moonlight_worker_selection_status_e::worker_not_authorized;
      rejected.authority_status =
        worker_seat_authorization_status_e::action_failed;
      if (owned != impl_->profile_launches.end()) owned->cleanup_requested = true;
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

    const auto owned = impl_->profile_launch(handle);
    if (owned != impl_->profile_launches.end()) {
      owned->cleanup_requested = true;
      if (auto launch = owned->launch.lock()) launch->cancel();
    }
    try {
      const auto quiesced = impl_->moonlight_runtime->quiesce_seat(handle);
      if (quiesced == input::moonlight_runtime_lifecycle_status_e::retained_until_streams_close) {
        result.status = controller_stop_status_e::streams_pending;
        return result;
      }
      if (quiesced != input::moonlight_runtime_lifecycle_status_e::retired &&
          quiesced != input::moonlight_runtime_lifecycle_status_e::not_selected) {
        result.status = controller_stop_status_e::cleanup_pending;
        return result;
      }
      impl_->admission_ready = false;
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

      const auto quiesced = impl_->moonlight_runtime->quiesce();
      if (quiesced.status ==
          input::moonlight_coordinator_quiesce_status_e::streams_pending) {
        result.status = controller_shutdown_status_e::streams_pending;
        return result;
      }

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
      result.status = controller_shutdown_status_e::input_cleanup_incomplete;
      result.input = impl_->moonlight_runtime->shutdown();
      if (result.input->status ==
          input::moonlight_coordinator_shutdown_status_e::streams_pending) {
        result.status = controller_shutdown_status_e::streams_pending;
        return result;
      }
      if (result.input->status !=
            input::moonlight_coordinator_shutdown_status_e::closed &&
          result.input->status !=
            input::moonlight_coordinator_shutdown_status_e::already_closed) {
        return result;
      }
      impl_->input_expectations.clear();
      impl_->profile_launches.clear();
      impl_->closed = true;
      impl_->profile_catalog_lease.reset();
      result.status = controller_shutdown_status_e::closed;
      return result;
    } catch (...) {
      return result;
    }
  }

  bool controller_runtime_t::routes_client(std::string_view client_key) const {
    // Routes are immutable for this owner's entire lifetime, including drain.
    return std::any_of(impl_->profile_routes.begin(), impl_->profile_routes.end(), [&](const auto &route) {
      return std::find(route.client_keys.begin(), route.client_keys.end(), client_key) != route.client_keys.end() ||
        std::find(route.access_clients.begin(), route.access_clients.end(), client_key) != route.access_clients.end();
    });
  }

  std::optional<std::string> controller_runtime_t::profile_for_client(std::string_view client_key) const {
    for (const auto &route : impl_->profile_routes)
      if (std::find(route.client_keys.begin(), route.client_keys.end(), client_key) != route.client_keys.end())
        return route.profile_key;
    for (const auto &route : impl_->profile_routes)
      if (std::find(route.access_clients.begin(), route.access_clients.end(), client_key) != route.access_clients.end())
        return route.profile_key;
    return std::nullopt;
  }

  std::vector<profile_summary_t> controller_runtime_t::profile_catalog() const { return impl_->profile_catalog; }

  std::vector<profile_activity_t> controller_runtime_t::profile_activity() const {
    std::scoped_lock lock {impl_->state_mutex};
    std::vector<profile_activity_t> result;
    for (const auto &entry : impl_->profile_launches) {
      const auto launch = entry.launch.lock();
      const auto seat = impl_->registry->snapshot(entry.handle);
      // Keep cleanup visible until reconciliation releases the owned record.
      const auto state = entry.cleanup_requested || !launch || launch->is_cancelled() ||
        (seat && seat->state == seat_state_e::stopping) ? "stopping" :
        launch->setup_state.load() == rtsp_stream::launch_session_t::setup_state_e::started ? "running" : "starting";
      result.push_back({entry.profile_key, entry.client_key, state});
    }
    return result;
  }

  std::optional<seat_state_e> controller_runtime_t::seat_state(const seat_handle_t &handle) const {
    std::scoped_lock lock {impl_->state_mutex};
    const auto seat = impl_->registry->snapshot(handle);
    return seat ? std::optional {seat->state} : std::nullopt;
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

  std::size_t controller_runtime_t::owned_profile_launches() const {
    std::scoped_lock lock {impl_->state_mutex};
    return impl_->profile_launches.size();
  }

}  // namespace multiseat

#endif
