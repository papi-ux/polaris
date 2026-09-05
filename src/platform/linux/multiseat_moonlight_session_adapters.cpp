/**
 * @file src/platform/linux/multiseat_moonlight_session_adapters.cpp
 * @brief Inert production ownership adapters for multiseat Moonlight sessions.
 */
#include "multiseat_moonlight_session_adapters.h"

#ifdef __linux__

  #include <algorithm>
  #include <condition_variable>
  #include <limits>
  #include <mutex>
  #include <stdexcept>
  #include <utility>
  #include <vector>

namespace multiseat::input {
  namespace {
    bool valid_binding(const authenticated_moonlight_session_t &binding) {
      return binding.key.valid() && binding.handle.valid() &&
             (!binding.controller_feedback ||
              binding.input_permissions.controller);
    }

    bool same_seat_slot(
      const seat_handle_t &lhs,
      const seat_handle_t &rhs
    ) {
      return lhs.logical_gpu_id == rhs.logical_gpu_id && lhs.slot == rhs.slot;
    }
  }  // namespace

  struct moonlight_session_binding_entry_t {
    explicit moonlight_session_binding_entry_t(
      authenticated_moonlight_session_t value
    ):
        binding(std::move(value)) {
    }

    const authenticated_moonlight_session_t binding;
    std::uint64_t claim_generation = 0;
    bool registered = true;
    bool claimed = false;
  };

  struct moonlight_session_binding_registry_state_t {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::shared_ptr<moonlight_session_binding_entry_t>> entries;
    bool closed = false;
  };

  namespace {
    class registry_binding_lease_t final:
        public moonlight_session_binding_lease_t {
    public:
      registry_binding_lease_t(
        std::shared_ptr<moonlight_session_binding_registry_state_t> state,
        std::shared_ptr<moonlight_session_binding_entry_t> entry,
        std::uint64_t claim_generation
      ):
          state_(std::move(state)),
          entry_(std::move(entry)),
          claim_generation_(claim_generation) {
      }

      ~registry_binding_lease_t() override {
        detach();
      }

      const authenticated_moonlight_session_t &binding() const noexcept override {
        return entry_->binding;
      }

      void detach() noexcept override {
        std::scoped_lock lock {state_->mutex};
        if (detached_) {
          return;
        }
        detached_ = true;
        if (entry_->claimed &&
            entry_->claim_generation == claim_generation_) {
          entry_->claimed = false;
          state_->changed.notify_all();
        }
      }

    private:
      const std::shared_ptr<moonlight_session_binding_registry_state_t> state_;
      const std::shared_ptr<moonlight_session_binding_entry_t> entry_;
      const std::uint64_t claim_generation_;
      bool detached_ = false;
    };
  }  // namespace

  moonlight_session_registration_t::moonlight_session_registration_t(
    std::shared_ptr<moonlight_session_binding_registry_state_t> state,
    std::shared_ptr<moonlight_session_binding_entry_t> entry
  ):
      state_(std::move(state)),
      entry_(std::move(entry)) {
  }

  moonlight_session_registration_t::~moonlight_session_registration_t() {
    close();
  }

  void moonlight_session_registration_t::close() noexcept {
    std::unique_lock lock {state_->mutex};
    entry_->registered = false;
    state_->changed.wait(lock, [this]() {
      return !entry_->claimed;
    });
    std::erase(state_->entries, entry_);
    state_->changed.notify_all();
  }

  const authenticated_moonlight_session_t &
    moonlight_session_registration_t::binding() const {
    return entry_->binding;
  }

  bool moonlight_session_registration_t::registered() const {
    std::scoped_lock lock {state_->mutex};
    return entry_->registered && !state_->closed;
  }

  moonlight_session_binding_registry_t::moonlight_session_binding_registry_t():
      state_(std::make_shared<moonlight_session_binding_registry_state_t>()) {
  }

  moonlight_session_binding_registry_t::~moonlight_session_binding_registry_t() {
    std::scoped_lock lock {state_->mutex};
    state_->closed = true;
    for (const auto &entry : state_->entries) {
      entry->registered = false;
    }
    state_->changed.notify_all();
  }

  moonlight_session_registration_result_t
    moonlight_session_binding_registry_t::register_session(
      authenticated_moonlight_session_t binding
    ) {
    if (!valid_binding(binding)) {
      return {
        .status = moonlight_session_registration_status_e::invalid_binding,
      };
    }

    auto entry = std::make_shared<moonlight_session_binding_entry_t>(
      std::move(binding)
    );
    std::unique_lock lock {state_->mutex};
    if (state_->closed) {
      return {
        .status = moonlight_session_registration_status_e::registry_closed,
      };
    }
    if (std::any_of(
          state_->entries.begin(),
          state_->entries.end(),
          [&entry](const auto &candidate) {
            return candidate->registered &&
                   candidate->binding.key == entry->binding.key;
          }
        )) {
      return {
        .status = moonlight_session_registration_status_e::duplicate_session,
      };
    }
    if (std::any_of(
          state_->entries.begin(),
          state_->entries.end(),
          [&entry](const auto &candidate) {
            return candidate->registered && same_seat_slot(
                                              candidate->binding.handle,
                                              entry->binding.handle
                                            );
          }
        )) {
      return {
        .status = moonlight_session_registration_status_e::duplicate_seat,
      };
    }
    if (state_->entries.size() >= maximum_input_allocations) {
      return {
        .status = moonlight_session_registration_status_e::capacity_reached,
      };
    }

    state_->entries.push_back(entry);
    try {
      return {
        .status = moonlight_session_registration_status_e::registered,
        .registration = std::unique_ptr<moonlight_session_registration_t> {
          new moonlight_session_registration_t(state_, entry)
        },
      };
    } catch (...) {
      std::erase(state_->entries, entry);
      throw;
    }
  }

  moonlight_session_authentication_result_t
    moonlight_session_binding_registry_t::attach(
      const moonlight_control_session_key_t &key
    ) {
    if (!key.valid()) {
      return {
        .status = moonlight_session_authentication_status_e::rejected,
      };
    }

    std::unique_lock lock {state_->mutex};
    if (state_->closed) {
      return {
        .status = moonlight_session_authentication_status_e::indeterminate,
      };
    }
    const auto found = std::find_if(
      state_->entries.begin(),
      state_->entries.end(),
      [&key](const auto &entry) {
        return entry->registered && entry->binding.key == key;
      }
    );
    if (found == state_->entries.end() || (*found)->claimed) {
      return {
        .status = moonlight_session_authentication_status_e::rejected,
      };
    }
    auto entry = *found;
    if (entry->claim_generation ==
        std::numeric_limits<std::uint64_t>::max()) {
      return {
        .status = moonlight_session_authentication_status_e::indeterminate,
      };
    }

    entry->claimed = true;
    ++entry->claim_generation;
    try {
      return {
        .status = moonlight_session_authentication_status_e::authenticated,
        .lease = std::make_unique<registry_binding_lease_t>(
          state_,
          entry,
          entry->claim_generation
        ),
      };
    } catch (...) {
      entry->claimed = false;
      state_->changed.notify_all();
      throw;
    }
  }

  void moonlight_session_binding_registry_t::close() noexcept {
    std::unique_lock lock {state_->mutex};
    state_->closed = true;
    for (const auto &entry : state_->entries) {
      entry->registered = false;
    }
    state_->changed.wait(lock, [this]() {
      return std::none_of(
        state_->entries.begin(),
        state_->entries.end(),
        [](const auto &entry) {
          return entry->claimed;
        }
      );
    });
    state_->entries.clear();
    state_->changed.notify_all();
  }

  std::size_t moonlight_session_binding_registry_t::registered_sessions() const {
    std::scoped_lock lock {state_->mutex};
    return std::count_if(
      state_->entries.begin(),
      state_->entries.end(),
      [](const auto &entry) {
        return entry->registered;
      }
    );
  }

  std::size_t moonlight_session_binding_registry_t::claimed_sessions() const {
    std::scoped_lock lock {state_->mutex};
    return std::count_if(
      state_->entries.begin(),
      state_->entries.end(),
      [](const auto &entry) {
        return entry->claimed;
      }
    );
  }

  bool moonlight_session_binding_registry_t::closed() const {
    std::scoped_lock lock {state_->mutex};
    return state_->closed;
  }

  namespace {
    struct feedback_subscription_entry_t {
      feedback_subscription_entry_t(
        seat_handle_t value,
        moonlight_controller_feedback_callback_t callback_value
      ):
          handle(std::move(value)),
          callback(std::move(callback_value)) {
      }

      const seat_handle_t handle;
      moonlight_controller_feedback_callback_t callback;
      std::size_t callbacks_in_flight = 0;
      bool accepting = true;
    };
  }  // namespace

  struct moonlight_controller_feedback_hub_state_t {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::shared_ptr<feedback_subscription_entry_t>> entries;
    std::size_t callbacks_in_flight = 0;
    bool closed = false;
  };

  namespace {
    class hub_feedback_subscription_t final:
        public moonlight_feedback_subscription_t {
    public:
      hub_feedback_subscription_t(
        std::shared_ptr<moonlight_controller_feedback_hub_state_t> state,
        std::shared_ptr<feedback_subscription_entry_t> entry
      ):
          state_(std::move(state)),
          entry_(std::move(entry)) {
      }

      ~hub_feedback_subscription_t() override {
        detach();
      }

      void detach() noexcept override {
        std::unique_lock lock {state_->mutex};
        if (detached_) {
          return;
        }
        detached_ = true;
        entry_->accepting = false;
        std::erase(state_->entries, entry_);
        state_->changed.wait(lock, [this]() {
          return entry_->callbacks_in_flight == 0;
        });
        entry_->callback = {};
        state_->changed.notify_all();
      }

    private:
      const std::shared_ptr<moonlight_controller_feedback_hub_state_t> state_;
      const std::shared_ptr<feedback_subscription_entry_t> entry_;
      bool detached_ = false;
    };

    moonlight_feedback_publish_status_e publish_feedback(
      const std::shared_ptr<moonlight_controller_feedback_hub_state_t> &state,
      const controller_feedback_t &feedback
    ) noexcept {
      if (!convert_controller_feedback(feedback)) {
        return moonlight_feedback_publish_status_e::invalid_feedback;
      }

      std::shared_ptr<feedback_subscription_entry_t> entry;
      {
        std::scoped_lock lock {state->mutex};
        if (state->closed) {
          return moonlight_feedback_publish_status_e::hub_closed;
        }
        const auto found = std::find_if(
          state->entries.begin(),
          state->entries.end(),
          [&feedback](const auto &candidate) {
            return candidate->accepting &&
                   candidate->handle == feedback.handle;
          }
        );
        if (found == state->entries.end()) {
          return moonlight_feedback_publish_status_e::not_attached;
        }
        entry = *found;
        ++entry->callbacks_in_flight;
        ++state->callbacks_in_flight;
      }

      auto status = moonlight_feedback_publish_status_e::delivered;
      try {
        entry->callback(feedback);
      } catch (...) {
        status = moonlight_feedback_publish_status_e::callback_failed;
      }
      {
        std::scoped_lock lock {state->mutex};
        --entry->callbacks_in_flight;
        --state->callbacks_in_flight;
        state->changed.notify_all();
      }
      return status;
    }
  }  // namespace

  moonlight_controller_feedback_hub_t::moonlight_controller_feedback_hub_t():
      state_(std::make_shared<moonlight_controller_feedback_hub_state_t>()) {
  }

  moonlight_controller_feedback_hub_t::~moonlight_controller_feedback_hub_t() {
    close();
  }

  std::unique_ptr<moonlight_feedback_subscription_t>
    moonlight_controller_feedback_hub_t::attach(
      const seat_handle_t &handle,
      moonlight_controller_feedback_callback_t callback
    ) {
    if (!handle.valid() || !callback) {
      return {};
    }
    auto entry = std::make_shared<feedback_subscription_entry_t>(
      handle,
      std::move(callback)
    );
    std::unique_lock lock {state_->mutex};
    if (state_->closed || state_->entries.size() >= maximum_input_allocations ||
        std::any_of(
          state_->entries.begin(),
          state_->entries.end(),
          [&entry](const auto &candidate) {
            return candidate->accepting && same_seat_slot(
                                             candidate->handle,
                                             entry->handle
                                           );
          }
        )) {
      return {};
    }

    state_->entries.push_back(entry);
    try {
      return std::unique_ptr<moonlight_feedback_subscription_t> {
        new hub_feedback_subscription_t(state_, entry)
      };
    } catch (...) {
      std::erase(state_->entries, entry);
      throw;
    }
  }

  moonlight_feedback_publish_status_e
    moonlight_controller_feedback_hub_t::publish(
      const controller_feedback_t &feedback
    ) noexcept {
    return publish_feedback(state_, feedback);
  }

  moonlight_controller_feedback_sink_t
    moonlight_controller_feedback_hub_t::sink() const {
    return [weak_state = std::weak_ptr {state_}](
             const controller_feedback_t &feedback
           ) noexcept {
      if (const auto state = weak_state.lock()) {
        (void) publish_feedback(state, feedback);
      }
    };
  }

  void moonlight_controller_feedback_hub_t::close() noexcept {
    std::unique_lock lock {state_->mutex};
    state_->closed = true;
    for (const auto &entry : state_->entries) {
      entry->accepting = false;
    }
    state_->changed.wait(lock, [this]() {
      return state_->callbacks_in_flight == 0;
    });
    for (const auto &entry : state_->entries) {
      entry->callback = {};
    }
    state_->entries.clear();
    state_->changed.notify_all();
  }

  std::size_t moonlight_controller_feedback_hub_t::subscriptions() const {
    std::scoped_lock lock {state_->mutex};
    return state_->entries.size();
  }

  bool moonlight_controller_feedback_hub_t::closed() const {
    std::scoped_lock lock {state_->mutex};
    return state_->closed;
  }

  moonlight_session_mailbox_feedback_sender_t::
    moonlight_session_mailbox_feedback_sender_t(
      std::shared_ptr<moonlight_session_feedback_mailbox_t> mailbox
    ):
      mailbox_(std::move(mailbox)) {
    if (!mailbox_ || !valid_binding(mailbox_->binding())) {
      throw std::invalid_argument {
        "invalid multiseat Moonlight feedback mailbox"
      };
    }
  }

  moonlight_feedback_send_result_e
    moonlight_session_mailbox_feedback_sender_t::send(
      const authenticated_moonlight_session_t &session,
      const moonlight_feedback_t &feedback
    ) {
    if (session != mailbox_->binding() || !session.controller_feedback ||
        !session.input_permissions.controller ||
        feedback.source_sequence == 0 ||
        feedback.message.type != platf::gamepad_feedback_e::rumble ||
        feedback.message.id >= maximum_gamepad_slots) {
      return moonlight_feedback_send_result_e::closed;
    }

    switch (mailbox_->submit(feedback)) {
      case moonlight_feedback_mailbox_result_e::queued:
        return moonlight_feedback_send_result_e::sent;
      case moonlight_feedback_mailbox_result_e::retry_later:
        return moonlight_feedback_send_result_e::retry_later;
      case moonlight_feedback_mailbox_result_e::closed:
        return moonlight_feedback_send_result_e::closed;
    }
    return moonlight_feedback_send_result_e::closed;
  }

}  // namespace multiseat::input

#endif
