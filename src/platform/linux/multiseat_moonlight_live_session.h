/**
 * @file src/platform/linux/multiseat_moonlight_live_session.h
 * @brief Explicitly activated production owner for one multiseat Moonlight session.
 */
#pragma once

#ifdef __linux__

  #include "multiseat_moonlight_session_adapters.h"

  #include <functional>
  #include <memory>
  #include <mutex>
  #include <optional>
  #include <span>

namespace multiseat::input {

  /** Non-secret identity copied from an already-authenticated stream session. */
  struct moonlight_live_session_identity_t {
    moonlight_control_session_key_t key;
    moonlight_input_permissions_t input_permissions;

    [[nodiscard]] bool valid() const;
  };

  using moonlight_feedback_submitter_t = std::function<
    moonlight_feedback_mailbox_result_e(const moonlight_feedback_t &)>;

  enum class moonlight_live_session_open_status_e {
    opened,
    invalid_identity,
    invalid_handle,
    missing_feedback_dependency,
    registration_rejected,
    bridge_open_failed,
  };

  class moonlight_live_session_t;

  struct moonlight_live_session_open_result_t {
    moonlight_live_session_open_status_e status =
      moonlight_live_session_open_status_e::invalid_identity;
    std::optional<moonlight_session_registration_status_e>
      registration_status;
    std::optional<moonlight_session_open_status_e> bridge_status;
    std::shared_ptr<moonlight_live_session_t> session;
  };

  /**
   * Construct the complete owner for one explicitly selected multiseat stream.
   *
   * The caller supplies only an authenticated, non-secret session identity,
   * an already-admitted seat handle, and a typed mailbox callback. No network
   * peer, cipher, session token, or device-creation capability crosses this
   * boundary. The authority must outlive the returned session; registry state
   * and the optional feedback hub are retained by owned handles.
   */
  [[nodiscard]] moonlight_live_session_open_result_t
  open_moonlight_live_session(
    authority_t &authority,
    moonlight_session_binding_registry_t &binding_registry,
    moonlight_live_session_identity_t identity,
    seat_handle_t handle,
    std::shared_ptr<moonlight_controller_feedback_hub_t> feedback_hub,
    moonlight_feedback_submitter_t feedback_submitter,
    bool controller_feedback
  );

  /**
   * Owns registration, exclusive bridge claim, feedback endpoint, and close
   * ordering for one explicitly activated live session.
   */
  class moonlight_live_session_t final {
  public:
    ~moonlight_live_session_t();

    moonlight_live_session_t(const moonlight_live_session_t &) = delete;
    moonlight_live_session_t &operator=(
      const moonlight_live_session_t &
    ) = delete;
    moonlight_live_session_t(moonlight_live_session_t &&) = delete;
    moonlight_live_session_t &operator=(moonlight_live_session_t &&) = delete;

    [[nodiscard]] moonlight_route_result_t route_input(
      std::span<const std::uint8_t> packet
    );
    [[nodiscard]] moonlight_session_drain_result_e drain_feedback();
    void close() noexcept;

    [[nodiscard]] const authenticated_moonlight_session_t &binding() const;
    [[nodiscard]] bool accepting() const;
    [[nodiscard]] bool closed() const;

  private:
    class callback_mailbox_t;

    moonlight_live_session_t(
      std::shared_ptr<callback_mailbox_t> mailbox,
      std::shared_ptr<moonlight_session_bridge_t> bridge,
      std::unique_ptr<moonlight_session_registration_t> registration
    );

    friend moonlight_live_session_open_result_t open_moonlight_live_session(
      authority_t &,
      moonlight_session_binding_registry_t &,
      moonlight_live_session_identity_t,
      seat_handle_t,
      std::shared_ptr<moonlight_controller_feedback_hub_t>,
      moonlight_feedback_submitter_t,
      bool
    );

    const std::shared_ptr<callback_mailbox_t> mailbox_;
    const std::shared_ptr<moonlight_session_bridge_t> bridge_;
    std::unique_ptr<moonlight_session_registration_t> registration_;
    mutable std::mutex close_mutex_;
    bool closed_ = false;
  };

}  // namespace multiseat::input

#endif
