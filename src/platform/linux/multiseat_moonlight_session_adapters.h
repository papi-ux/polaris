/**
 * @file src/platform/linux/multiseat_moonlight_session_adapters.h
 * @brief Inert production ownership adapters for multiseat Moonlight sessions.
 */
#pragma once

#ifdef __linux__

  #include "multiseat_moonlight_session_bridge.h"

  #include <cstddef>
  #include <functional>
  #include <memory>

namespace multiseat::input {

  struct moonlight_session_binding_registry_state_t;
  struct moonlight_session_binding_entry_t;

  enum class moonlight_session_registration_status_e {
    registered,
    invalid_binding,
    duplicate_session,
    duplicate_seat,
    capacity_reached,
    registry_closed,
  };

  class moonlight_session_registration_t final {
  public:
    ~moonlight_session_registration_t();

    moonlight_session_registration_t(
      const moonlight_session_registration_t &
    ) = delete;
    moonlight_session_registration_t &operator=(
      const moonlight_session_registration_t &
    ) = delete;
    moonlight_session_registration_t(
      moonlight_session_registration_t &&
    ) = delete;
    moonlight_session_registration_t &operator=(
      moonlight_session_registration_t &&
    ) = delete;

    /**
     * Retire this binding and wait for its exclusive bridge claim to detach.
     * The session owner must close its bridge before calling close() on the
     * same thread.
     */
    void close() noexcept;

    [[nodiscard]] const authenticated_moonlight_session_t &binding() const;
    [[nodiscard]] bool registered() const;

  private:
    moonlight_session_registration_t(
      std::shared_ptr<moonlight_session_binding_registry_state_t> state,
      std::shared_ptr<moonlight_session_binding_entry_t> entry
    );

    friend class moonlight_session_binding_registry_t;

    const std::shared_ptr<moonlight_session_binding_registry_state_t> state_;
    const std::shared_ptr<moonlight_session_binding_entry_t> entry_;
  };

  struct moonlight_session_registration_result_t {
    moonlight_session_registration_status_e status =
      moonlight_session_registration_status_e::invalid_binding;
    std::unique_ptr<moonlight_session_registration_t> registration;
  };

  /**
   * Process-local source of exact authenticated session bindings.
   *
   * Registration contains no raw authentication secret. A bridge receives at
   * most one exclusive lease for a registered key. quiesce() closes new
   * registration and attachment without waiting; finish_close() succeeds only
   * after every existing bridge claim has detached. close() remains the
   * synchronous barrier for owners which have already proved that condition.
   */
  class moonlight_session_binding_registry_t final:
      public moonlight_session_binding_source_t {
  public:
    moonlight_session_binding_registry_t();
    ~moonlight_session_binding_registry_t() override;

    moonlight_session_binding_registry_t(
      const moonlight_session_binding_registry_t &
    ) = delete;
    moonlight_session_binding_registry_t &operator=(
      const moonlight_session_binding_registry_t &
    ) = delete;
    moonlight_session_binding_registry_t(
      moonlight_session_binding_registry_t &&
    ) = delete;
    moonlight_session_binding_registry_t &operator=(
      moonlight_session_binding_registry_t &&
    ) = delete;

    [[nodiscard]] moonlight_session_registration_result_t register_session(
      authenticated_moonlight_session_t binding
    );
    moonlight_session_authentication_result_t attach(
      const moonlight_control_session_key_t &key
    ) override;

    [[nodiscard]] std::size_t quiesce() noexcept;
    [[nodiscard]] bool finish_close() noexcept;
    void close() noexcept;
    [[nodiscard]] std::size_t registered_sessions() const;
    [[nodiscard]] std::size_t claimed_sessions() const;
    [[nodiscard]] bool closed() const;

  private:
    const std::shared_ptr<moonlight_session_binding_registry_state_t> state_;
  };

  struct moonlight_controller_feedback_hub_state_t;

  enum class moonlight_feedback_publish_status_e {
    delivered,
    invalid_feedback,
    not_attached,
    callback_failed,
    hub_closed,
  };

  using moonlight_controller_feedback_sink_t =
    std::function<void(const controller_feedback_t &)>;

  /**
   * Exact-seat fanout from the trusted input backend to one bridge callback.
   * Subscription detach and hub close wait for callbacks already in flight.
   * A callback must not synchronously detach itself or close this hub.
   */
  class moonlight_controller_feedback_hub_t final:
      public moonlight_controller_feedback_source_t {
  public:
    moonlight_controller_feedback_hub_t();
    ~moonlight_controller_feedback_hub_t() override;

    moonlight_controller_feedback_hub_t(
      const moonlight_controller_feedback_hub_t &
    ) = delete;
    moonlight_controller_feedback_hub_t &operator=(
      const moonlight_controller_feedback_hub_t &
    ) = delete;
    moonlight_controller_feedback_hub_t(
      moonlight_controller_feedback_hub_t &&
    ) = delete;
    moonlight_controller_feedback_hub_t &operator=(
      moonlight_controller_feedback_hub_t &&
    ) = delete;

    std::unique_ptr<moonlight_feedback_subscription_t> attach(
      const seat_handle_t &handle,
      moonlight_controller_feedback_callback_t callback
    ) override;

    [[nodiscard]] moonlight_feedback_publish_status_e publish(
      const controller_feedback_t &feedback
    ) noexcept;
    /** Safe to retain in the input backend after this hub is destroyed. */
    [[nodiscard]] moonlight_controller_feedback_sink_t sink() const;

    void close() noexcept;
    [[nodiscard]] std::size_t subscriptions() const;
    [[nodiscard]] bool closed() const;

  private:
    const std::shared_ptr<moonlight_controller_feedback_hub_state_t> state_;
  };

  enum class moonlight_feedback_mailbox_result_e {
    queued,
    retry_later,
    closed,
  };

  /** Typed, session-owned edge to the existing control-thread mailbox. */
  class moonlight_session_feedback_mailbox_t {
  public:
    virtual ~moonlight_session_feedback_mailbox_t() = default;

    /** Immutable for this mailbox's complete lifetime. */
    [[nodiscard]] virtual const authenticated_moonlight_session_t &binding() const noexcept = 0;
    virtual moonlight_feedback_mailbox_result_e submit(
      const moonlight_feedback_t &feedback
    ) = 0;
  };

  /**
   * Concrete sender which validates the full authenticated binding before
   * forwarding one bounded rumble state to a session-owned mailbox.
   */
  class moonlight_session_mailbox_feedback_sender_t final:
      public moonlight_session_feedback_sender_t {
  public:
    explicit moonlight_session_mailbox_feedback_sender_t(
      std::shared_ptr<moonlight_session_feedback_mailbox_t> mailbox
    );

    moonlight_feedback_send_result_e send(
      const authenticated_moonlight_session_t &session,
      const moonlight_feedback_t &feedback
    ) override;

  private:
    const std::shared_ptr<moonlight_session_feedback_mailbox_t> mailbox_;
  };

}  // namespace multiseat::input

#endif
