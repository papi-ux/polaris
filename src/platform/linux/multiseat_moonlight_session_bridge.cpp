/**
 * @file src/platform/linux/multiseat_moonlight_session_bridge.cpp
 * @brief Offline authenticated-session ownership for multiseat Moonlight I/O.
 */
#include "multiseat_moonlight_session_bridge.h"

#ifdef __linux__

  #include <condition_variable>
  #include <utility>

namespace multiseat::input {
  namespace {
    bool binding_matches_allocation(
      const authenticated_moonlight_session_t &binding,
      const allocation_t &allocation
    ) {
      if (!binding.key.valid() || !binding.handle.valid() ||
          binding.handle != allocation.handle) {
        return false;
      }
      if (binding.input_permissions.touch && !allocation.plan.touch) {
        return false;
      }
      if (binding.input_permissions.pen && !allocation.plan.pen) {
        return false;
      }
      if (binding.input_permissions.controller &&
          allocation.plan.gamepad_slots == 0) {
        return false;
      }
      return !binding.controller_feedback ||
             (binding.input_permissions.controller &&
              allocation.plan.gamepad_slots > 0);
    }
  }  // namespace

  bool moonlight_control_session_key_t::valid() const {
    return launch_session_id != 0 && session_generation != 0;
  }

  struct moonlight_session_bridge_t::shared_state_t final:
      std::enable_shared_from_this<shared_state_t> {
    enum class phase_e {
      open,
      closing,
      closed,
    };

    class operation_t {
    public:
      operation_t() = default;

      explicit operation_t(std::shared_ptr<shared_state_t> state):
          state_(std::move(state)) {
      }

      operation_t(const operation_t &) = delete;
      operation_t &operator=(const operation_t &) = delete;
      operation_t(operation_t &&) noexcept = default;
      operation_t &operator=(operation_t &&) = delete;

      ~operation_t() {
        if (state_) {
          state_->leave();
        }
      }

      explicit operator bool() const {
        return static_cast<bool>(state_);
      }

    private:
      std::shared_ptr<shared_state_t> state_;
    };

    struct drain_outcome_t {
      moonlight_session_drain_result_e status =
        moonlight_session_drain_result_e::sender_failed;
      bool close_after = false;
    };

    shared_state_t(
      authority_t &authority,
      authenticated_moonlight_session_t binding,
      std::uint32_t maximum_feedback_gamepad_slots
    ):
        binding_(std::move(binding)),
        input_(authority, binding_.handle, binding_.input_permissions),
        feedback_(binding_.handle),
        maximum_feedback_gamepad_slots_(maximum_feedback_gamepad_slots) {
    }

    operation_t enter() {
      auto self = shared_from_this();
      std::scoped_lock lock {lifecycle_mutex_};
      if (phase_ != phase_e::open) {
        return {};
      }
      ++active_operations_;
      return operation_t {std::move(self)};
    }

    void leave() noexcept {
      std::scoped_lock lock {lifecycle_mutex_};
      --active_operations_;
      if (active_operations_ == 0) {
        lifecycle_changed_.notify_all();
      }
    }

    moonlight_route_result_t route_input(
      std::span<const std::uint8_t> packet
    ) {
      auto operation = enter();
      if (!operation) {
        return {.status = moonlight_route_status_e::session_closed};
      }
      return input_.route(packet);
    }

    void publish_feedback(const controller_feedback_t &feedback) noexcept {
      try {
        auto operation = enter();
        if (operation &&
            feedback.event.gamepad_slot < maximum_feedback_gamepad_slots_) {
          (void) feedback_.push(feedback);
        }
      } catch (...) {
      }
    }

    drain_outcome_t drain_feedback(
      moonlight_session_feedback_sender_t *sender
    ) {
      std::scoped_lock drain_lock {drain_mutex_};
      auto operation = enter();
      if (!operation) {
        return {
          .status = moonlight_session_drain_result_e::session_closed,
        };
      }
      const auto pending = feedback_.peek();
      if (!pending) {
        return {.status = moonlight_session_drain_result_e::empty};
      }
      if (!sender) {
        return {
          .status = moonlight_session_drain_result_e::sender_failed,
          .close_after = true,
        };
      }

      moonlight_feedback_send_result_e sent;
      try {
        sent = sender->send(binding_, *pending);
      } catch (...) {
        return {
          .status = moonlight_session_drain_result_e::sender_failed,
          .close_after = true,
        };
      }
      switch (sent) {
        case moonlight_feedback_send_result_e::sent:
          {
            const auto acknowledged = feedback_.acknowledge(
              pending->source_sequence
            );
            if (acknowledged ==
                  controller_feedback_ack_result_e::acknowledged ||
                acknowledged == controller_feedback_ack_result_e::not_pending) {
              return {.status = moonlight_session_drain_result_e::sent};
            }
            return {
              .status = moonlight_session_drain_result_e::sender_failed,
              .close_after = true,
            };
          }
        case moonlight_feedback_send_result_e::retry_later:
          return {.status = moonlight_session_drain_result_e::retry_later};
        case moonlight_feedback_send_result_e::closed:
          return {
            .status = moonlight_session_drain_result_e::sender_closed,
            .close_after = true,
          };
      }
      return {
        .status = moonlight_session_drain_result_e::sender_failed,
        .close_after = true,
      };
    }

    void begin_close() noexcept {
      std::scoped_lock lock {lifecycle_mutex_};
      if (phase_ == phase_e::open) {
        phase_ = phase_e::closing;
      }
    }

    void finish_close() noexcept {
      std::unique_lock lock {lifecycle_mutex_};
      if (phase_ == phase_e::closed) {
        return;
      }
      phase_ = phase_e::closing;
      lifecycle_changed_.wait(lock, [this]() {
        return active_operations_ == 0;
      });
      feedback_.close();
      phase_ = phase_e::closed;
      lock.unlock();
      lifecycle_changed_.notify_all();
    }

    const authenticated_moonlight_session_t &binding() const {
      return binding_;
    }

    bool accepting() const {
      std::scoped_lock lock {lifecycle_mutex_};
      return phase_ == phase_e::open;
    }

    bool closed() const {
      std::scoped_lock lock {lifecycle_mutex_};
      return phase_ == phase_e::closed;
    }

    std::size_t pending_feedback() const {
      return feedback_.pending();
    }

    std::uint64_t last_feedback_sequence() const {
      return feedback_.last_sequence();
    }

  private:
    const authenticated_moonlight_session_t binding_;
    moonlight_input_adapter_t input_;
    moonlight_controller_feedback_queue_t feedback_;
    const std::uint32_t maximum_feedback_gamepad_slots_;
    mutable std::mutex lifecycle_mutex_;
    std::condition_variable lifecycle_changed_;
    phase_e phase_ = phase_e::open;
    std::size_t active_operations_ = 0;
    std::mutex drain_mutex_;
  };

  moonlight_session_bridge_t::moonlight_session_bridge_t(
    std::shared_ptr<shared_state_t> state,
    std::unique_ptr<moonlight_session_binding_lease_t> binding_lease,
    std::shared_ptr<moonlight_controller_feedback_source_t> feedback_source,
    std::shared_ptr<moonlight_session_feedback_sender_t> feedback_sender
  ):
      state_(std::move(state)),
      binding_lease_(std::move(binding_lease)),
      feedback_source_(std::move(feedback_source)),
      feedback_sender_(std::move(feedback_sender)) {
  }

  moonlight_session_bridge_t::~moonlight_session_bridge_t() {
    close();
  }

  moonlight_route_result_t moonlight_session_bridge_t::route_input(
    std::span<const std::uint8_t> packet
  ) {
    return state_->route_input(packet);
  }

  moonlight_session_drain_result_e
    moonlight_session_bridge_t::drain_feedback() {
    const auto outcome = state_->drain_feedback(feedback_sender_.get());
    if (outcome.close_after) {
      close();
    }
    return outcome.status;
  }

  void moonlight_session_bridge_t::close() noexcept {
    std::scoped_lock close_lock {close_mutex_};
    if (state_->closed()) {
      return;
    }
    state_->begin_close();
    if (feedback_subscription_) {
      feedback_subscription_->detach();
      feedback_subscription_.reset();
    }
    state_->finish_close();
    if (binding_lease_) {
      binding_lease_->detach();
      binding_lease_.reset();
    }
  }

  const authenticated_moonlight_session_t &
    moonlight_session_bridge_t::binding() const {
    return state_->binding();
  }

  bool moonlight_session_bridge_t::accepting() const {
    return state_->accepting();
  }

  bool moonlight_session_bridge_t::closed() const {
    return state_->closed();
  }

  std::size_t moonlight_session_bridge_t::pending_feedback() const {
    return state_->pending_feedback();
  }

  std::uint64_t moonlight_session_bridge_t::last_feedback_sequence() const {
    return state_->last_feedback_sequence();
  }

  moonlight_session_open_result_t open_moonlight_session_bridge(
    authority_t &authority,
    moonlight_session_binding_source_t &binding_source,
    const moonlight_control_session_key_t &key,
    std::shared_ptr<moonlight_controller_feedback_source_t> feedback_source,
    std::shared_ptr<moonlight_session_feedback_sender_t> feedback_sender
  ) {
    if (!key.valid()) {
      return {.status = moonlight_session_open_status_e::invalid_key};
    }

    moonlight_session_authentication_result_t authenticated;
    try {
      authenticated = binding_source.attach(key);
    } catch (...) {
      return {
        .status =
          moonlight_session_open_status_e::authentication_indeterminate,
      };
    }
    const auto reject_lease = [&authenticated](
                                moonlight_session_open_status_e status
                              ) {
      if (authenticated.lease) {
        authenticated.lease->detach();
        authenticated.lease.reset();
      }
      return moonlight_session_open_result_t {.status = status};
    };
    switch (authenticated.status) {
      case moonlight_session_authentication_status_e::rejected:
        return reject_lease(
          authenticated.lease ?
            moonlight_session_open_status_e::invalid_binding :
            moonlight_session_open_status_e::authentication_rejected
        );
      case moonlight_session_authentication_status_e::indeterminate:
        return reject_lease(
          authenticated.lease ?
            moonlight_session_open_status_e::invalid_binding :
            moonlight_session_open_status_e::authentication_indeterminate
        );
      case moonlight_session_authentication_status_e::authenticated:
        break;
      default:
        return reject_lease(
          moonlight_session_open_status_e::invalid_binding
        );
    }
    if (!authenticated.lease) {
      return {.status = moonlight_session_open_status_e::invalid_binding};
    }
    const auto binding = authenticated.lease->binding();
    if (binding.key != key || !binding.handle.valid()) {
      return reject_lease(moonlight_session_open_status_e::invalid_binding);
    }
    if (!authority.admission_ready()) {
      return reject_lease(
        moonlight_session_open_status_e::authority_unavailable
      );
    }
    const auto allocation = authority.allocation(binding.handle);
    if (!allocation) {
      return reject_lease(
        moonlight_session_open_status_e::authority_unavailable
      );
    }
    if (!binding_matches_allocation(binding, *allocation)) {
      return reject_lease(moonlight_session_open_status_e::invalid_binding);
    }
    if (binding.controller_feedback &&
        (!feedback_source || !feedback_sender)) {
      return reject_lease(
        moonlight_session_open_status_e::missing_feedback_dependency
      );
    }

    auto state = std::make_shared<moonlight_session_bridge_t::shared_state_t>(
      authority,
      binding,
      allocation->plan.gamepad_slots
    );
    auto bridge = std::shared_ptr<moonlight_session_bridge_t>(
      new moonlight_session_bridge_t(
        state,
        std::move(authenticated.lease),
        std::move(feedback_source),
        std::move(feedback_sender)
      )
    );
    if (binding.controller_feedback) {
      try {
        bridge->feedback_subscription_ = bridge->feedback_source_->attach(
          binding.handle,
          [weak_state = std::weak_ptr {state}](
            const controller_feedback_t &feedback
          ) noexcept {
            if (const auto active = weak_state.lock()) {
              active->publish_feedback(feedback);
            }
          }
        );
      } catch (...) {
        bridge->close();
        return {
          .status = moonlight_session_open_status_e::feedback_attach_failed,
        };
      }
      if (!bridge->feedback_subscription_) {
        bridge->close();
        return {
          .status = moonlight_session_open_status_e::feedback_attach_failed,
        };
      }
    }
    return {
      .status = moonlight_session_open_status_e::opened,
      .bridge = std::move(bridge),
    };
  }

}  // namespace multiseat::input

#endif
