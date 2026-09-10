/**
 * @file src/platform/linux/multiseat_moonlight_activation.cpp
 * @brief Default-off launch selection for production multiseat Moonlight input.
 */
#include "multiseat_moonlight_activation.h"

#ifdef __linux__

  #include "src/rtsp.h"
  #include "src/stream.h"

  #include <algorithm>
  #include <condition_variable>
  #include <mutex>
  #include <utility>
  #include <vector>

namespace multiseat::input {
  namespace {
    enum class selection_phase_e {
      pending,
      activating,
      bound,
      failed,
      cancelled,
    };

    bool same_seat_slot(
      const seat_handle_t &lhs,
      const seat_handle_t &rhs
    ) {
      return lhs.logical_gpu_id == rhs.logical_gpu_id && lhs.slot == rhs.slot;
    }
  }  // namespace

  struct moonlight_launch_selection_entry_t {
    moonlight_launch_selection_entry_t(
      moonlight_launch_selection_key_t key_value,
      seat_handle_t handle_value,
      bool controller_feedback_value,
      worker_connection_selection_t worker_connection_value
    ):
        key(std::move(key_value)),
        handle(std::move(handle_value)),
        controller_feedback(controller_feedback_value),
        worker_connection(std::move(worker_connection_value)) {
    }

    const moonlight_launch_selection_key_t key;
    const seat_handle_t handle;
    const bool controller_feedback;
    const worker_connection_selection_t worker_connection;
    std::uint64_t bound_stream_generation = 0;
    selection_phase_e phase = selection_phase_e::pending;

    void retire_connection() const noexcept {
      if (worker_connection.connection) worker_connection.connection->retire();
    }
  };

  struct moonlight_session_activation_gate_state_t {
    moonlight_session_activation_gate_state_t(
      bool enabled_value,
      authority_t &authority_value,
      moonlight_session_binding_registry_t &binding_registry_value,
      std::shared_ptr<moonlight_controller_feedback_hub_t> feedback_hub_value
    ):
        enabled(enabled_value),
        authority(authority_value),
        binding_registry(binding_registry_value),
        feedback_hub(std::move(feedback_hub_value)) {
    }

    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::shared_ptr<moonlight_launch_selection_entry_t>> entries;
    const bool enabled;
    bool closed = false;
    authority_t &authority;
    moonlight_session_binding_registry_t &binding_registry;
    const std::shared_ptr<moonlight_controller_feedback_hub_t> feedback_hub;
  };

  namespace {
    struct installed_gate_state_t {
      std::mutex mutex;
      std::shared_ptr<moonlight_session_activation_gate_t> gate;
    };

    installed_gate_state_t &installed_gate_state() {
      static installed_gate_state_t state;
      return state;
    }

#ifdef POLARIS_TESTS
    struct activation_test_hook_state_t {
      std::mutex mutex;
      moonlight_activation_before_bind_hook_t before_bind;
    };

    activation_test_hook_state_t &activation_test_hook_state() {
      static activation_test_hook_state_t state;
      return state;
    }

    void run_activation_before_bind_hook_for_tests() {
      moonlight_activation_before_bind_hook_t hook;
      {
        auto &state = activation_test_hook_state();
        std::scoped_lock lock {state.mutex};
        hook = state.before_bind;
      }
      if (hook) {
        hook();
      }
    }
#endif

    void erase_selection(
      moonlight_session_activation_gate_state_t &state,
      const std::shared_ptr<moonlight_launch_selection_entry_t> &entry
    ) {
      std::erase(state.entries, entry);
      state.changed.notify_all();
    }
  }  // namespace

  bool moonlight_launch_selection_key_t::valid() const {
    return launch_session_id != 0 && lifecycle_generation != 0;
  }

  moonlight_launch_selection_t::moonlight_launch_selection_t(
    std::shared_ptr<moonlight_session_activation_gate_state_t> state,
    std::shared_ptr<moonlight_launch_selection_entry_t> entry
  ):
      state_(std::move(state)),
      entry_(std::move(entry)) {
  }

  moonlight_launch_selection_t::~moonlight_launch_selection_t() {
    close();
  }

  void moonlight_launch_selection_t::cancel() noexcept {
    std::unique_lock lock {state_->mutex};
    state_->changed.wait(lock, [this]() {
      return entry_->phase != selection_phase_e::activating;
    });
    if (entry_->phase == selection_phase_e::cancelled) {
      return;
    }
    entry_->phase = selection_phase_e::cancelled;
    entry_->retire_connection();
    state_->changed.notify_all();
  }

  void moonlight_launch_selection_t::close() noexcept {
    std::unique_lock lock {state_->mutex};
    state_->changed.wait(lock, [this]() {
      return entry_->phase != selection_phase_e::activating;
    });
    entry_->phase = selection_phase_e::cancelled;
    entry_->retire_connection();
    erase_selection(*state_, entry_);
  }

  bool moonlight_launch_selection_t::active() const {
    std::scoped_lock lock {state_->mutex};
    return entry_->phase != selection_phase_e::cancelled &&
           std::find(state_->entries.begin(), state_->entries.end(), entry_) !=
             state_->entries.end();
  }

  const moonlight_launch_selection_key_t &
    moonlight_launch_selection_t::key() const {
    return entry_->key;
  }

  moonlight_session_activation_gate_t::moonlight_session_activation_gate_t(
    bool enabled,
    authority_t &authority,
    moonlight_session_binding_registry_t &binding_registry,
    std::shared_ptr<moonlight_controller_feedback_hub_t> feedback_hub
  ):
      state_(std::make_shared<moonlight_session_activation_gate_state_t>(
        enabled,
        authority,
        binding_registry,
        std::move(feedback_hub)
      )) {
  }

  moonlight_session_activation_gate_t::~moonlight_session_activation_gate_t() {
    close();
  }

  moonlight_launch_selection_result_t
    moonlight_session_activation_gate_t::register_selection(
      moonlight_launch_selection_key_t key,
      seat_handle_t handle,
      std::string_view expected_input_seat,
      bool controller_feedback,
      worker_connection_selection_t worker_connection
    ) {
    if (!state_->enabled) {
      return {
        .status = moonlight_launch_selection_status_e::gate_disabled,
      };
    }
    if (!key.valid() || !handle.valid() || expected_input_seat.empty()) {
      return {
        .status = moonlight_launch_selection_status_e::invalid_selection,
      };
    }
    if (worker_connection.required != static_cast<bool>(worker_connection.connection) ||
        (worker_connection.required && !worker_connection.connection->matches_selection(
          key.launch_session_id, key.lifecycle_generation, handle))) {
      return {.status = moonlight_launch_selection_status_e::invalid_selection};
    }
    if (controller_feedback && !state_->feedback_hub) {
      return {
        .status =
          moonlight_launch_selection_status_e::missing_feedback_dependency,
      };
    }

    {
      std::scoped_lock lock {state_->mutex};
      if (state_->closed) {
        return {
          .status = moonlight_launch_selection_status_e::gate_closed,
        };
      }
    }

    const auto authority_ready = state_->authority.admission_ready();
    const auto allocation = state_->authority.allocation(handle);
    if (!authority_ready || !allocation ||
        allocation->input_seat != expected_input_seat ||
        (controller_feedback && allocation->plan.gamepad_slots == 0)) {
      return {
        .status = moonlight_launch_selection_status_e::seat_not_admitted,
      };
    }

    auto entry = std::make_shared<moonlight_launch_selection_entry_t>(
      std::move(key),
      std::move(handle),
      controller_feedback,
      std::move(worker_connection)
    );
    std::unique_lock lock {state_->mutex};
    if (state_->closed) {
      return {
        .status = moonlight_launch_selection_status_e::gate_closed,
      };
    }
    if (std::any_of(
          state_->entries.begin(),
          state_->entries.end(),
          [&entry](const auto &candidate) {
            return candidate->key == entry->key;
          }
        )) {
      return {
        .status = moonlight_launch_selection_status_e::duplicate_session,
      };
    }
    if (std::any_of(
          state_->entries.begin(),
          state_->entries.end(),
          [&entry](const auto &candidate) {
            return candidate->phase != selection_phase_e::cancelled &&
                   same_seat_slot(candidate->handle, entry->handle);
          }
        )) {
      return {
        .status = moonlight_launch_selection_status_e::duplicate_seat,
      };
    }
    if (state_->entries.size() >= maximum_input_allocations) {
      return {
        .status = moonlight_launch_selection_status_e::capacity_reached,
      };
    }

    if (entry->worker_connection.required && !entry->worker_connection.connection->try_register()) {
      return {.status = moonlight_launch_selection_status_e::invalid_selection};
    }
    try {
      state_->entries.push_back(entry);
      return {
        .status = moonlight_launch_selection_status_e::registered,
        .selection = std::unique_ptr<moonlight_launch_selection_t> {
          new moonlight_launch_selection_t(state_, entry)
        },
      };
    } catch (...) {
      entry->retire_connection();
      erase_selection(*state_, entry);
      throw;
    }
  }

  moonlight_session_activation_status_e
    moonlight_session_activation_gate_t::activate(
      stream::session_t &session
    ) {
    if (!state_->enabled) {
      return moonlight_session_activation_status_e::gate_disabled;
    }

    const moonlight_launch_selection_key_t key {
      .launch_session_id = stream::session::launch_session_id(session),
      .lifecycle_generation =
        stream::session::launch_lifecycle_generation(session),
    };
    if (!key.valid()) {
      return moonlight_session_activation_status_e::invalid_session;
    }

    std::shared_ptr<moonlight_launch_selection_entry_t> entry;
    {
      std::unique_lock lock {state_->mutex};
      if (state_->closed) {
        return moonlight_session_activation_status_e::gate_closed;
      }
      const auto found = std::find_if(
        state_->entries.begin(),
        state_->entries.end(),
        [&key](const auto &candidate) {
          return candidate->key == key;
        }
      );
      if (found == state_->entries.end()) {
        return moonlight_session_activation_status_e::unselected;
      }
      entry = *found;
      switch (entry->phase) {
        case selection_phase_e::pending:
          entry->phase = selection_phase_e::activating;
          break;
        case selection_phase_e::activating:
          return moonlight_session_activation_status_e::selection_in_progress;
        case selection_phase_e::bound:
          lock.unlock();
          return stream::session::multiseat_input_bound_to(session, entry->bound_stream_generation) ?
                   moonlight_session_activation_status_e::bound :
                   moonlight_session_activation_status_e::selected_binding_failed;
        case selection_phase_e::failed:
          return moonlight_session_activation_status_e::selected_binding_failed;
        case selection_phase_e::cancelled:
          return moonlight_session_activation_status_e::selection_cancelled;
      }
    }

    auto bound = false;
    try {
#ifdef POLARIS_TESTS
      run_activation_before_bind_hook_for_tests();
#endif
      bound = stream::session::bind_multiseat_input(
                session,
                state_->authority,
                state_->binding_registry,
                entry->handle,
                state_->feedback_hub,
                entry->controller_feedback,
                entry->worker_connection
              ) == stream::session::multiseat_input_bind_status_e::bound;
    } catch (...) {
      bound = false;
    }

    {
      std::scoped_lock lock {state_->mutex};
      if (bound) entry->bound_stream_generation = stream::session::generation(session);
      entry->phase = bound ? selection_phase_e::bound : selection_phase_e::failed;
      if (!bound || state_->closed) entry->retire_connection();
      state_->changed.notify_all();
    }
    return bound ? moonlight_session_activation_status_e::bound :
                   moonlight_session_activation_status_e::selected_binding_failed;
  }

  std::size_t moonlight_session_activation_gate_t::quiesce() noexcept {
    std::scoped_lock lock {state_->mutex};
    state_->closed = true;
    std::size_t activations_in_flight = 0;
    for (const auto &entry : state_->entries) {
      if (entry->phase == selection_phase_e::activating) {
        ++activations_in_flight;
      } else {
        entry->phase = selection_phase_e::cancelled;
        entry->retire_connection();
      }
    }
    state_->changed.notify_all();
    return activations_in_flight;
  }

  bool moonlight_session_activation_gate_t::finish_close() noexcept {
    std::scoped_lock lock {state_->mutex};
    state_->closed = true;
    if (std::any_of(
          state_->entries.begin(),
          state_->entries.end(),
          [](const auto &entry) {
            return entry->phase == selection_phase_e::activating;
          }
        )) {
      return false;
    }
    for (const auto &entry : state_->entries) {
      entry->phase = selection_phase_e::cancelled;
      entry->retire_connection();
    }
    state_->entries.clear();
    state_->changed.notify_all();
    return true;
  }

  void moonlight_session_activation_gate_t::close() noexcept {
    (void) quiesce();
    std::unique_lock lock {state_->mutex};
    state_->changed.wait(lock, [this]() {
      return std::none_of(
        state_->entries.begin(),
        state_->entries.end(),
        [](const auto &entry) {
          return entry->phase == selection_phase_e::activating;
        }
      );
    });
    for (const auto &entry : state_->entries) {
      entry->phase = selection_phase_e::cancelled;
      entry->retire_connection();
    }
    state_->entries.clear();
    state_->changed.notify_all();
  }

  bool moonlight_session_activation_gate_t::enabled() const {
    return state_->enabled;
  }

  bool moonlight_session_activation_gate_t::closed() const {
    std::scoped_lock lock {state_->mutex};
    return state_->closed;
  }

  std::size_t moonlight_session_activation_gate_t::active_selections() const {
    std::scoped_lock lock {state_->mutex};
    return std::count_if(
      state_->entries.begin(),
      state_->entries.end(),
      [](const auto &entry) {
        return entry->phase != selection_phase_e::cancelled;
      }
    );
  }

  moonlight_activation_installation_t::moonlight_activation_installation_t(
    std::shared_ptr<moonlight_session_activation_gate_t> gate
  ):
      gate_(std::move(gate)) {
  }

  moonlight_activation_installation_t::~moonlight_activation_installation_t() {
    close();
  }

  void moonlight_activation_installation_t::close() noexcept {
    auto &installed = installed_gate_state();
    std::scoped_lock lock {installed.mutex};
    if (!gate_) {
      return;
    }
    if (installed.gate == gate_) {
      installed.gate.reset();
    }
    gate_.reset();
  }

  bool moonlight_activation_installation_t::installed() const {
    auto &installed = installed_gate_state();
    std::scoped_lock lock {installed.mutex};
    return gate_ && installed.gate == gate_;
  }

  moonlight_activation_install_result_t
  install_moonlight_session_activation_gate(
    std::shared_ptr<moonlight_session_activation_gate_t> gate
  ) {
    if (!gate) {
      return {
        .status = moonlight_activation_install_status_e::invalid_gate,
      };
    }
    if (!gate->enabled()) {
      return {
        .status = moonlight_activation_install_status_e::gate_disabled,
      };
    }
    if (gate->closed()) {
      return {
        .status = moonlight_activation_install_status_e::gate_closed,
      };
    }

    auto &installed = installed_gate_state();
    std::unique_lock lock {installed.mutex};
    if (installed.gate) {
      return {
        .status = moonlight_activation_install_status_e::already_installed,
      };
    }
    installed.gate = gate;
    try {
      return {
        .status = moonlight_activation_install_status_e::installed,
        .installation =
          std::unique_ptr<moonlight_activation_installation_t> {
            new moonlight_activation_installation_t(std::move(gate))
          },
      };
    } catch (...) {
      installed.gate.reset();
      throw;
    }
  }

  moonlight_session_activation_status_e
  activate_registered_moonlight_session(stream::session_t &session) {
    std::shared_ptr<moonlight_session_activation_gate_t> gate;
    {
      auto &installed = installed_gate_state();
      std::scoped_lock lock {installed.mutex};
      gate = installed.gate;
    }
    return gate ? gate->activate(session) :
                  moonlight_session_activation_status_e::not_installed;
  }

  bool moonlight_session_activation_gate_installed() {
    auto &installed = installed_gate_state();
    std::scoped_lock lock {installed.mutex};
    return static_cast<bool>(installed.gate);
  }

#ifdef POLARIS_TESTS
  void set_moonlight_activation_before_bind_hook_for_tests(
    moonlight_activation_before_bind_hook_t hook
  ) {
    auto &state = activation_test_hook_state();
    std::scoped_lock lock {state.mutex};
    state.before_bind = std::move(hook);
  }
#endif

}  // namespace multiseat::input

#endif
