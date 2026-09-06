/**
 * @file src/platform/linux/multiseat_moonlight_runtime.cpp
 * @brief Explicitly configured owner for Moonlight multiseat input.
 */
#include "multiseat_moonlight_runtime.h"

#ifdef __linux__

  #include "input/inputtino_multiseat_backend.h"
  #include "src/rtsp.h"

  #include <algorithm>
  #include <condition_variable>
  #include <mutex>
  #include <utility>
  #include <vector>

namespace multiseat::input {
  namespace {
    struct installed_runtime_state_t {
      std::mutex mutex;
      std::condition_variable changed;
      moonlight_session_runtime_t *runtime = nullptr;
      std::size_t calls_in_flight = 0;
    };

    installed_runtime_state_t &installed_runtime_state() {
      static installed_runtime_state_t state;
      return state;
    }

    class runtime_call_t final {
    public:
      runtime_call_t() {
        auto &state = installed_runtime_state();
        std::scoped_lock lock {state.mutex};
        if (state.runtime) {
          runtime_ = state.runtime;
          ++state.calls_in_flight;
        }
      }

      ~runtime_call_t() {
        if (!runtime_) {
          return;
        }
        auto &state = installed_runtime_state();
        std::scoped_lock lock {state.mutex};
        --state.calls_in_flight;
        state.changed.notify_all();
      }

      runtime_call_t(const runtime_call_t &) = delete;
      runtime_call_t &operator=(const runtime_call_t &) = delete;

      [[nodiscard]] moonlight_session_runtime_t *get() const {
        return runtime_;
      }

    private:
      moonlight_session_runtime_t *runtime_ = nullptr;
    };

    bool install_runtime(moonlight_session_runtime_t &runtime) {
      auto &state = installed_runtime_state();
      std::scoped_lock lock {state.mutex};
      if (state.runtime) {
        return false;
      }
      state.runtime = &runtime;
      return true;
    }

    void uninstall_runtime(moonlight_session_runtime_t &runtime) {
      auto &state = installed_runtime_state();
      std::unique_lock lock {state.mutex};
      if (state.runtime != &runtime) {
        return;
      }
      state.runtime = nullptr;
      state.changed.wait(lock, [&state]() {
        return state.calls_in_flight == 0;
      });
    }

    std::optional<moonlight_launch_selection_key_t> launch_key(
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

    moonlight_runtime_create_status_e map_create_status(
      moonlight_coordinator_create_status_e status
    ) {
      switch (status) {
        case moonlight_coordinator_create_status_e::invalid_factory:
          return moonlight_runtime_create_status_e::invalid_factory;
        case moonlight_coordinator_create_status_e::backend_unavailable:
          return moonlight_runtime_create_status_e::backend_unavailable;
        case moonlight_coordinator_create_status_e::activation_install_rejected:
          return moonlight_runtime_create_status_e::activation_install_rejected;
        case moonlight_coordinator_create_status_e::ready_disabled:
        case moonlight_coordinator_create_status_e::ready_enabled:
          break;
      }
      return moonlight_runtime_create_status_e::backend_unavailable;
    }

    struct production_backend_dependencies_t {
      inputtino_device_factory_t device_factory;
      posix_kernel_node_io_t kernel_io;
      linux_kernel_node_probe_t kernel_probe {kernel_io};
    };
  }  // namespace

  struct moonlight_session_runtime_t::impl_t {
    struct tracked_launch_t {
      moonlight_launch_selection_key_t key;
      std::shared_ptr<rtsp_stream::launch_session_t> launch;
      bool cancelled = false;
    };

    impl_t(
      std::shared_ptr<void> dependencies,
      std::unique_ptr<moonlight_session_coordinator_t> coordinator_value
    ):
        backend_dependencies(std::move(dependencies)),
        coordinator(std::move(coordinator_value)) {
      launches.reserve(maximum_input_allocations);
    }

    std::shared_ptr<void> backend_dependencies;
    std::unique_ptr<moonlight_session_coordinator_t> coordinator;
    mutable std::mutex state_mutex;
    std::mutex shutdown_mutex;
    std::vector<tracked_launch_t> launches;
    bool installed = false;
    bool shutting_down = false;
    bool closed = false;
  };

  namespace {
    template<class Impl>
    moonlight_runtime_lifecycle_status_e sweep_cancelled_launches(
      Impl &impl,
      const std::shared_ptr<rtsp_stream::launch_session_t> &target
    ) {
      if (impl.coordinator->claimed_sessions() != 0) {
        return moonlight_runtime_lifecycle_status_e::retained_until_streams_close;
      }

      bool target_retired = false;
      for (auto entry = impl.launches.begin(); entry != impl.launches.end();) {
        if (!entry->cancelled) {
          ++entry;
          continue;
        }
        const auto retired =
          impl.coordinator->retire_cancelled_launch(entry->launch);
        if (retired == moonlight_coordinator_retire_status_e::retired ||
            retired == moonlight_coordinator_retire_status_e::launch_not_found) {
          target_retired = target_retired || entry->launch == target;
          entry = impl.launches.erase(entry);
        } else {
          ++entry;
        }
      }
      return target_retired ?
               moonlight_runtime_lifecycle_status_e::retired :
               moonlight_runtime_lifecycle_status_e::retained_until_streams_close;
    }

    template<class Impl>
    moonlight_runtime_lifecycle_status_e cancel_tracked_launch(
      Impl &impl,
      const std::shared_ptr<rtsp_stream::launch_session_t> &launch
    ) {
      if (!launch) {
        return moonlight_runtime_lifecycle_status_e::not_selected;
      }
      const auto found = std::find_if(
        impl.launches.begin(),
        impl.launches.end(),
        [&launch](const auto &entry) {
          return entry.launch == launch;
        }
      );
      if (found == impl.launches.end()) {
        return moonlight_runtime_lifecycle_status_e::not_selected;
      }

      launch->cancel();
      if (!found->cancelled) {
        const auto cancelled = impl.coordinator->cancel_launch(launch);
        if (cancelled != moonlight_coordinator_cancel_status_e::cancelled &&
            cancelled !=
              moonlight_coordinator_cancel_status_e::already_cancelled) {
          return moonlight_runtime_lifecycle_status_e::not_selected;
        }
        found->cancelled = true;
      }
      return sweep_cancelled_launches(impl, launch);
    }
  }  // namespace

  moonlight_session_runtime_t::moonlight_session_runtime_t(
    std::unique_ptr<impl_t> impl
  ):
      impl_(std::move(impl)) {
  }

  moonlight_session_runtime_t::~moonlight_session_runtime_t() {
    (void) shutdown();
  }

  moonlight_runtime_create_result_t moonlight_session_runtime_t::create(
    moonlight_session_runtime_options_t options,
    moonlight_input_backend_factory_t backend_factory,
    std::shared_ptr<void> backend_dependencies
  ) {
    if (!options.enabled) {
      return {
        .status = moonlight_runtime_create_status_e::ready_disabled,
      };
    }
    if (!backend_factory) {
      return {
        .status = moonlight_runtime_create_status_e::invalid_factory,
      };
    }

    auto coordinated = moonlight_session_coordinator_t::create(
      {.enabled = true},
      std::move(backend_factory)
    );
    if (coordinated.status !=
          moonlight_coordinator_create_status_e::ready_enabled ||
        !coordinated.coordinator) {
      return {
        .status = map_create_status(coordinated.status),
      };
    }

    std::unique_ptr<moonlight_session_runtime_t> runtime;
    try {
      runtime = std::unique_ptr<moonlight_session_runtime_t> {
        new moonlight_session_runtime_t(std::make_unique<impl_t>(
          std::move(backend_dependencies),
          std::move(coordinated.coordinator)
        ))
      };
    } catch (...) {
      return {
        .status = moonlight_runtime_create_status_e::backend_unavailable,
      };
    }
    if (!install_runtime(*runtime)) {
      return {
        .status = moonlight_runtime_create_status_e::runtime_install_rejected,
      };
    }
    {
      std::scoped_lock lock {runtime->impl_->state_mutex};
      runtime->impl_->installed = true;
    }
    return {
      .status = moonlight_runtime_create_status_e::ready_enabled,
      .runtime = std::move(runtime),
    };
  }

  moonlight_coordinator_reconcile_result_t
  moonlight_session_runtime_t::reconcile_inputs(
    const std::vector<expectation_t> &expected
  ) {
    std::scoped_lock lock {impl_->state_mutex};
    if (impl_->shutting_down || impl_->closed) {
      return {
        .status = moonlight_coordinator_operation_status_e::shutting_down,
      };
    }
    return impl_->coordinator->reconcile_inputs(expected);
  }

  moonlight_coordinator_prepare_result_t
  moonlight_session_runtime_t::prepare_input(
    const expectation_t &expectation
  ) {
    std::scoped_lock lock {impl_->state_mutex};
    if (impl_->shutting_down || impl_->closed) {
      return {
        .status = moonlight_coordinator_operation_status_e::shutting_down,
      };
    }
    return impl_->coordinator->prepare_input(expectation);
  }

  moonlight_coordinator_release_result_t
  moonlight_session_runtime_t::release_input(const seat_handle_t &handle) {
    std::scoped_lock lock {impl_->state_mutex};
    if (impl_->shutting_down || impl_->closed) {
      return {
        .status = moonlight_coordinator_operation_status_e::shutting_down,
      };
    }
    return impl_->coordinator->release_input(handle);
  }

  moonlight_launch_selection_status_e
  moonlight_session_runtime_t::select_authenticated_launch(
    const std::shared_ptr<rtsp_stream::launch_session_t> &launch,
    seat_handle_t handle,
    bool controller_feedback
  ) {
    std::scoped_lock lock {impl_->state_mutex};
    if (impl_->shutting_down || impl_->closed) {
      return moonlight_launch_selection_status_e::gate_closed;
    }
    const auto key = launch ? launch_key(*launch) : std::nullopt;
    if (!key) {
      return moonlight_launch_selection_status_e::invalid_selection;
    }
    const auto selected = impl_->coordinator->select_launch(
      launch,
      std::move(handle),
      controller_feedback
    );
    if (selected == moonlight_launch_selection_status_e::registered) {
      impl_->launches.push_back({
        .key = *key,
        .launch = launch,
      });
    }
    return selected;
  }

  moonlight_runtime_lifecycle_status_e
  moonlight_session_runtime_t::cancel_launch(
    const std::shared_ptr<rtsp_stream::launch_session_t> &launch
  ) {
    std::scoped_lock lock {impl_->state_mutex};
    if (impl_->shutting_down || impl_->closed) {
      return moonlight_runtime_lifecycle_status_e::runtime_shutting_down;
    }
    return cancel_tracked_launch(*impl_, launch);
  }

  moonlight_runtime_lifecycle_status_e
  moonlight_session_runtime_t::finish_stream(
    moonlight_launch_selection_key_t key
  ) {
    std::scoped_lock lock {impl_->state_mutex};
    if (impl_->shutting_down || impl_->closed) {
      return moonlight_runtime_lifecycle_status_e::runtime_shutting_down;
    }
    const auto found = std::find_if(
      impl_->launches.begin(),
      impl_->launches.end(),
      [&key](const auto &entry) {
        return entry.key == key;
      }
    );
    if (found == impl_->launches.end()) {
      return moonlight_runtime_lifecycle_status_e::not_selected;
    }
    return cancel_tracked_launch(*impl_, found->launch);
  }

  moonlight_coordinator_shutdown_report_t
  moonlight_session_runtime_t::shutdown() noexcept {
    std::scoped_lock shutdown_lock {impl_->shutdown_mutex};
    {
      std::scoped_lock state_lock {impl_->state_mutex};
      if (impl_->closed) {
        return {
          .status = moonlight_coordinator_shutdown_status_e::already_closed,
        };
      }
      impl_->shutting_down = true;
      for (auto &entry : impl_->launches) {
        entry.launch->cancel();
        if (!entry.cancelled) {
          (void) impl_->coordinator->cancel_launch(entry.launch);
          entry.cancelled = true;
        }
      }
    }

    uninstall_runtime(*this);
    {
      std::scoped_lock state_lock {impl_->state_mutex};
      impl_->installed = false;
    }

    const auto report = impl_->coordinator->shutdown();
    if (report.status == moonlight_coordinator_shutdown_status_e::closed ||
        report.status ==
          moonlight_coordinator_shutdown_status_e::already_closed) {
      std::scoped_lock state_lock {impl_->state_mutex};
      impl_->launches.clear();
      impl_->closed = true;
    }
    return report;
  }

  bool moonlight_session_runtime_t::installed() const {
    std::scoped_lock lock {impl_->state_mutex};
    return impl_->installed;
  }

  bool moonlight_session_runtime_t::shutting_down() const {
    std::scoped_lock lock {impl_->state_mutex};
    return impl_->shutting_down;
  }

  bool moonlight_session_runtime_t::closed() const {
    std::scoped_lock lock {impl_->state_mutex};
    return impl_->closed;
  }

  std::size_t moonlight_session_runtime_t::tracked_launches() const {
    std::scoped_lock lock {impl_->state_mutex};
    return impl_->launches.size();
  }

  std::size_t moonlight_session_runtime_t::active_launches() const {
    std::scoped_lock lock {impl_->state_mutex};
    return impl_->coordinator->active_launches();
  }

  std::size_t moonlight_session_runtime_t::retained_launches() const {
    std::scoped_lock lock {impl_->state_mutex};
    return impl_->coordinator->retained_launches();
  }

  std::size_t moonlight_session_runtime_t::input_allocations() const {
    std::scoped_lock lock {impl_->state_mutex};
    return impl_->coordinator->input_allocations();
  }

  std::size_t moonlight_session_runtime_t::claimed_sessions() const {
    std::scoped_lock lock {impl_->state_mutex};
    return impl_->coordinator->claimed_sessions();
  }

  moonlight_runtime_create_result_t
  create_production_moonlight_session_runtime(
    moonlight_session_runtime_options_t options
  ) {
    if (!options.enabled) {
      return moonlight_session_runtime_t::create(options, {});
    }

    try {
      auto dependencies =
        std::make_shared<production_backend_dependencies_t>();
      return moonlight_session_runtime_t::create(
        options,
        [dependencies](moonlight_controller_feedback_sink_t feedback_sink) {
          return std::make_unique<inputtino_host_backend_t>(
            dependencies->device_factory,
            dependencies->kernel_probe,
            inputtino_host_backend_options_t {},
            inputtino_backend_waiter_t {},
            std::move(feedback_sink)
          );
        },
        dependencies
      );
    } catch (...) {
      return {
        .status = moonlight_runtime_create_status_e::backend_unavailable,
      };
    }
  }

  std::optional<moonlight_launch_selection_status_e>
  select_authenticated_moonlight_launch(
    const std::shared_ptr<rtsp_stream::launch_session_t> &launch,
    seat_handle_t handle,
    bool controller_feedback
  ) {
    runtime_call_t call;
    if (!call.get()) {
      return std::nullopt;
    }
    return call.get()->select_authenticated_launch(
      launch,
      std::move(handle),
      controller_feedback
    );
  }

  moonlight_runtime_lifecycle_status_e
  cancel_registered_moonlight_launch(
    const std::shared_ptr<rtsp_stream::launch_session_t> &launch
  ) {
    runtime_call_t call;
    if (!call.get()) {
      return moonlight_runtime_lifecycle_status_e::runtime_unavailable;
    }
    return call.get()->cancel_launch(launch);
  }

  moonlight_runtime_lifecycle_status_e
  finish_registered_moonlight_stream(
    moonlight_launch_selection_key_t key
  ) {
    runtime_call_t call;
    if (!call.get()) {
      return moonlight_runtime_lifecycle_status_e::runtime_unavailable;
    }
    return call.get()->finish_stream(std::move(key));
  }

  bool moonlight_session_runtime_installed() {
    auto &state = installed_runtime_state();
    std::scoped_lock lock {state.mutex};
    return state.runtime != nullptr;
  }

}  // namespace multiseat::input

#endif
