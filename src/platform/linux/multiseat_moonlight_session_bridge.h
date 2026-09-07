/**
 * @file src/platform/linux/multiseat_moonlight_session_bridge.h
 * @brief Offline authenticated-session ownership for multiseat Moonlight I/O.
 */
#pragma once

#ifdef __linux__

  #include "multiseat_moonlight_input_adapter.h"

  #include <cstddef>
  #include <cstdint>
  #include <functional>
  #include <memory>
  #include <mutex>
  #include <span>

namespace multiseat::input {

  /**
   * Non-secret identity assigned by the existing authenticated launch/session
   * lifecycle. Neither value is accepted from a Moonlight input packet.
   */
  struct moonlight_control_session_key_t {
    std::uint32_t launch_session_id = 0;
    std::uint64_t session_generation = 0;

    [[nodiscard]] bool valid() const;

    bool operator==(const moonlight_control_session_key_t &) const = default;
  };

  /** Trusted result of resolving one already-authenticated control session. */
  struct authenticated_moonlight_session_t {
    moonlight_control_session_key_t key;
    seat_handle_t handle;
    moonlight_input_permissions_t input_permissions;
    bool controller_feedback = false;

    bool operator==(const authenticated_moonlight_session_t &) const = default;
  };

  enum class moonlight_session_authentication_status_e {
    authenticated,
    rejected,
    indeterminate,
  };

  /**
   * Exclusive ownership of one authenticated control-session lifetime.
   * Destruction must provide the same release guarantee as detach().
   */
  class moonlight_session_binding_lease_t {
  public:
    virtual ~moonlight_session_binding_lease_t() = default;

    /** The returned binding is immutable for the complete lease lifetime. */
    [[nodiscard]] virtual const authenticated_moonlight_session_t &binding() const noexcept = 0;
    /** Release the claim synchronously. Must be idempotent. */
    virtual void detach() noexcept = 0;
  };

  struct moonlight_session_authentication_result_t {
    moonlight_session_authentication_status_e status =
      moonlight_session_authentication_status_e::indeterminate;
    std::unique_ptr<moonlight_session_binding_lease_t> lease;
  };

  /**
   * Trusted adapter over the existing launch/control authentication state.
   * attach() atomically claims an unclaimed exact session or rejects it.
   * Implementations must not accept a raw session token through this API.
   */
  class moonlight_session_binding_source_t {
  public:
    virtual ~moonlight_session_binding_source_t() = default;
    virtual moonlight_session_authentication_result_t attach(
      const moonlight_control_session_key_t &key
    ) = 0;
  };

  using moonlight_controller_feedback_callback_t =
    std::function<void(const controller_feedback_t &)>;

  /** Synchronous, idempotent callback ownership returned by a trusted source. */
  class moonlight_feedback_subscription_t {
  public:
    virtual ~moonlight_feedback_subscription_t() = default;

    /** Prevent future callback entry and wait for callbacks already invoked. */
    virtual void detach() noexcept = 0;
  };

  /** Injectable bridge to the host backend's typed feedback publication edge. */
  class moonlight_controller_feedback_source_t {
  public:
    virtual ~moonlight_controller_feedback_source_t() = default;

    /**
     * On success, callback ownership belongs to the returned subscription.
     * Returning null or throwing must leave no callback installed.
     */
    virtual std::unique_ptr<moonlight_feedback_subscription_t> attach(
      const seat_handle_t &handle,
      moonlight_controller_feedback_callback_t callback
    ) = 0;
  };

  enum class moonlight_feedback_send_result_e {
    sent,
    retry_later,
    closed,
  };

  /**
   * Immediate, injected egress for one authenticated control session.
   * send() must return promptly and must not call close() or drain_feedback()
   * on its bridge.
   */
  class moonlight_session_feedback_sender_t {
  public:
    virtual ~moonlight_session_feedback_sender_t() = default;
    virtual moonlight_feedback_send_result_e send(
      const authenticated_moonlight_session_t &session,
      const moonlight_feedback_t &feedback
    ) = 0;
  };

  enum class moonlight_session_open_status_e {
    opened,
    invalid_key,
    authentication_rejected,
    authentication_indeterminate,
    invalid_binding,
    authority_unavailable,
    missing_feedback_dependency,
    feedback_attach_failed,
  };

  enum class moonlight_session_drain_result_e {
    sent,
    empty,
    retry_later,
    sender_closed,
    sender_failed,
    session_closed,
  };

  class moonlight_session_bridge_t;

  struct moonlight_session_open_result_t {
    moonlight_session_open_status_e status =
      moonlight_session_open_status_e::invalid_key;
    std::shared_ptr<moonlight_session_bridge_t> bridge;
  };

  [[nodiscard]] moonlight_session_open_result_t open_moonlight_session_bridge(
    authority_t &authority,
    moonlight_session_binding_source_t &binding_source,
    const moonlight_control_session_key_t &key,
    std::shared_ptr<moonlight_controller_feedback_source_t> feedback_source = {},
    std::shared_ptr<moonlight_session_feedback_sender_t> feedback_sender = {}
  );

  /**
   * Owns Moonlight input and feedback for one authenticated session lifetime.
   *
   * close() first stops new operations, then synchronously detaches the typed
   * feedback callback, waits for accepted operations, and finally clears the
   * bounded feedback queue before releasing the authenticated lease. It must
   * be called by the session owner, not re-entered from an authority route or
   * feedback sender. The referenced authority must outlive the bridge.
   */
  class moonlight_session_bridge_t final {
  public:
    ~moonlight_session_bridge_t();

    moonlight_session_bridge_t(const moonlight_session_bridge_t &) = delete;
    moonlight_session_bridge_t &operator=(
      const moonlight_session_bridge_t &
    ) = delete;
    moonlight_session_bridge_t(moonlight_session_bridge_t &&) = delete;
    moonlight_session_bridge_t &operator=(moonlight_session_bridge_t &&) = delete;

    [[nodiscard]] moonlight_route_result_t route_input(
      std::span<const std::uint8_t> packet
    );
    [[nodiscard]] moonlight_session_drain_result_e drain_feedback();
    void close() noexcept;

    [[nodiscard]] const authenticated_moonlight_session_t &binding() const;
    [[nodiscard]] bool accepting() const;
    [[nodiscard]] bool closed() const;
    [[nodiscard]] std::size_t pending_feedback() const;
    [[nodiscard]] std::uint64_t last_feedback_sequence() const;

  private:
    struct shared_state_t;

    moonlight_session_bridge_t(
      std::shared_ptr<shared_state_t> state,
      std::unique_ptr<moonlight_session_binding_lease_t> binding_lease,
      std::shared_ptr<moonlight_controller_feedback_source_t> feedback_source,
      std::shared_ptr<moonlight_session_feedback_sender_t> feedback_sender
    );

    friend moonlight_session_open_result_t open_moonlight_session_bridge(
      authority_t &,
      moonlight_session_binding_source_t &,
      const moonlight_control_session_key_t &,
      std::shared_ptr<moonlight_controller_feedback_source_t>,
      std::shared_ptr<moonlight_session_feedback_sender_t>
    );

    const std::shared_ptr<shared_state_t> state_;
    // Non-const so close() can release the claim before object destruction.
    std::unique_ptr<moonlight_session_binding_lease_t> binding_lease_;
    const std::shared_ptr<moonlight_controller_feedback_source_t>
      feedback_source_;
    const std::shared_ptr<moonlight_session_feedback_sender_t> feedback_sender_;
    std::unique_ptr<moonlight_feedback_subscription_t> feedback_subscription_;
    std::mutex close_mutex_;
  };

}  // namespace multiseat::input

#endif
