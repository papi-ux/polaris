/**
 * @file src/platform/linux/multiseat_moonlight_live_session.cpp
 * @brief Explicitly activated production owner for one multiseat Moonlight session.
 */
#include "multiseat_moonlight_live_session.h"

#ifdef __linux__

  #include <utility>

namespace multiseat::input {
  namespace {
    bool any_input_permission(
      const moonlight_input_permissions_t &permissions
    ) {
      return permissions.keyboard || permissions.mouse || permissions.touch ||
             permissions.pen || permissions.controller;
    }
  }  // namespace

  bool moonlight_live_session_identity_t::valid() const {
    return key.valid() && any_input_permission(input_permissions);
  }

  class moonlight_live_session_t::callback_mailbox_t final:
      public moonlight_session_feedback_mailbox_t {
  public:
    callback_mailbox_t(
      authenticated_moonlight_session_t binding,
      moonlight_feedback_submitter_t submitter
    ):
        binding_(std::move(binding)),
        submitter_(std::move(submitter)) {
    }

    const authenticated_moonlight_session_t &binding() const noexcept override {
      return binding_;
    }

    moonlight_feedback_mailbox_result_e submit(
      const moonlight_feedback_t &feedback
    ) override {
      std::scoped_lock lock {mutex_};
      if (!accepting_ || !submitter_) {
        return moonlight_feedback_mailbox_result_e::closed;
      }
      return submitter_(feedback);
    }

    void close() noexcept {
      std::scoped_lock lock {mutex_};
      accepting_ = false;
      submitter_ = {};
    }

  private:
    const authenticated_moonlight_session_t binding_;
    moonlight_feedback_submitter_t submitter_;
    std::mutex mutex_;
    bool accepting_ = true;
  };

  moonlight_live_session_t::moonlight_live_session_t(
    std::shared_ptr<callback_mailbox_t> mailbox,
    std::shared_ptr<moonlight_session_bridge_t> bridge,
    std::unique_ptr<moonlight_session_registration_t> registration
  ):
      mailbox_(std::move(mailbox)),
      bridge_(std::move(bridge)),
      registration_(std::move(registration)) {
  }

  moonlight_live_session_t::~moonlight_live_session_t() {
    close();
  }

  moonlight_route_result_t moonlight_live_session_t::route_input(
    std::span<const std::uint8_t> packet
  ) {
    return bridge_->route_input(packet);
  }

  moonlight_session_drain_result_e
    moonlight_live_session_t::drain_feedback() {
    return bridge_->drain_feedback();
  }

  void moonlight_live_session_t::close() noexcept {
    std::scoped_lock lock {close_mutex_};
    if (closed_) {
      return;
    }

    // Stop mailbox entry first, detach and quiesce the bridge second, then
    // retire the discoverable registration only after its claim is gone.
    mailbox_->close();
    bridge_->close();
    if (registration_) {
      registration_->close();
      registration_.reset();
    }
    closed_ = true;
  }

  const authenticated_moonlight_session_t &
    moonlight_live_session_t::binding() const {
    return bridge_->binding();
  }

  bool moonlight_live_session_t::accepting() const {
    std::scoped_lock lock {close_mutex_};
    return !closed_ && bridge_->accepting();
  }

  bool moonlight_live_session_t::closed() const {
    std::scoped_lock lock {close_mutex_};
    return closed_;
  }

  moonlight_live_session_open_result_t open_moonlight_live_session(
    authority_t &authority,
    moonlight_session_binding_registry_t &binding_registry,
    moonlight_live_session_identity_t identity,
    seat_handle_t handle,
    std::shared_ptr<moonlight_controller_feedback_hub_t> feedback_hub,
    moonlight_feedback_submitter_t feedback_submitter,
    bool controller_feedback
  ) {
    if (!identity.valid() ||
        (controller_feedback && !identity.input_permissions.controller)) {
      return {
        .status = moonlight_live_session_open_status_e::invalid_identity,
      };
    }
    if (!handle.valid()) {
      return {
        .status = moonlight_live_session_open_status_e::invalid_handle,
      };
    }
    if (controller_feedback && (!feedback_hub || !feedback_submitter)) {
      return {
        .status =
          moonlight_live_session_open_status_e::missing_feedback_dependency,
      };
    }

    const auto binding = authenticated_moonlight_session_t {
      .key = identity.key,
      .handle = std::move(handle),
      .input_permissions = identity.input_permissions,
      .controller_feedback = controller_feedback,
    };
    auto registered = binding_registry.register_session(binding);
    if (registered.status !=
          moonlight_session_registration_status_e::registered ||
        !registered.registration) {
      return {
        .status =
          moonlight_live_session_open_status_e::registration_rejected,
        .registration_status = registered.status,
      };
    }

    auto mailbox = std::make_shared<moonlight_live_session_t::callback_mailbox_t>(
      binding,
      std::move(feedback_submitter)
    );
    auto sender = std::make_shared<moonlight_session_mailbox_feedback_sender_t>(
      mailbox
    );
    auto opened = open_moonlight_session_bridge(
      authority,
      binding_registry,
      binding.key,
      controller_feedback ? std::move(feedback_hub) : nullptr,
      controller_feedback ? std::move(sender) : nullptr
    );
    if (opened.status != moonlight_session_open_status_e::opened ||
        !opened.bridge) {
      mailbox->close();
      registered.registration->close();
      return {
        .status = moonlight_live_session_open_status_e::bridge_open_failed,
        .registration_status = registered.status,
        .bridge_status = opened.status,
      };
    }

    auto live = std::shared_ptr<moonlight_live_session_t>(
      new moonlight_live_session_t(
        std::move(mailbox),
        std::move(opened.bridge),
        std::move(registered.registration)
      )
    );
    return {
      .status = moonlight_live_session_open_status_e::opened,
      .registration_status = registered.status,
      .bridge_status = opened.status,
      .session = std::move(live),
    };
  }

}  // namespace multiseat::input

#endif
