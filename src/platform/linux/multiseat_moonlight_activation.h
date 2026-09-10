/**
 * @file src/platform/linux/multiseat_moonlight_activation.h
 * @brief Default-off launch selection for production multiseat Moonlight input.
 */
#pragma once

#ifdef __linux__

  #include "multiseat_moonlight_session_adapters.h"
  #include "multiseat_worker_launch_connection.h"

  #include <cstddef>
  #include <cstdint>
  #include <functional>
  #include <memory>
  #include <string_view>

namespace stream {
  struct session_t;
}

namespace multiseat::input {

  /** Exact non-secret identity admitted by the authenticated launch path. */
  struct moonlight_launch_selection_key_t {
    std::uint32_t launch_session_id = 0;
    std::uint64_t lifecycle_generation = 0;

    [[nodiscard]] bool valid() const;
    bool operator==(const moonlight_launch_selection_key_t &) const = default;
  };

  enum class moonlight_launch_selection_status_e {
    registered,
    gate_disabled,
    invalid_selection,
    seat_not_admitted,
    missing_feedback_dependency,
    duplicate_session,
    duplicate_seat,
    capacity_reached,
    gate_closed,
  };

  struct moonlight_session_activation_gate_state_t;
  struct moonlight_launch_selection_entry_t;

  /**
   * RAII ownership for one pending launch-to-seat selection.
   *
   * cancel() leaves a fail-closed tombstone for the exact launch key. close()
   * retires that tombstone and must only be used after the authenticated launch
   * lifecycle can no longer enter RTSP setup. Both wait for an activation which
   * is already in progress to reach a terminal state.
   */
  class moonlight_launch_selection_t final {
  public:
    ~moonlight_launch_selection_t();

    moonlight_launch_selection_t(const moonlight_launch_selection_t &) = delete;
    moonlight_launch_selection_t &operator=(
      const moonlight_launch_selection_t &
    ) = delete;
    moonlight_launch_selection_t(moonlight_launch_selection_t &&) = delete;
    moonlight_launch_selection_t &operator=(
      moonlight_launch_selection_t &&
    ) = delete;

    void cancel() noexcept;
    void close() noexcept;
    [[nodiscard]] bool active() const;
    [[nodiscard]] const moonlight_launch_selection_key_t &key() const;

  private:
    moonlight_launch_selection_t(
      std::shared_ptr<moonlight_session_activation_gate_state_t> state,
      std::shared_ptr<moonlight_launch_selection_entry_t> entry
    );

    friend class moonlight_session_activation_gate_t;

    const std::shared_ptr<moonlight_session_activation_gate_state_t> state_;
    const std::shared_ptr<moonlight_launch_selection_entry_t> entry_;
  };

  struct moonlight_launch_selection_result_t {
    moonlight_launch_selection_status_e status =
      moonlight_launch_selection_status_e::invalid_selection;
    std::unique_ptr<moonlight_launch_selection_t> selection;
  };

  enum class moonlight_session_activation_status_e {
    not_installed,
    gate_disabled,
    unselected,
    bound,
    selected_binding_failed,
    selection_in_progress,
    selection_cancelled,
    gate_closed,
    invalid_session,
  };

  /**
   * Bounded one-time map from authenticated launches to admitted input seats.
   *
   * Merely constructing this gate changes nothing. It must be explicitly
   * enabled, installed, and populated before stream::session::start() can
   * select multiseat input. The authority and registry must outlive the gate,
   * and selected stream sessions must close before those dependencies.
   */
  class moonlight_session_activation_gate_t final {
  public:
    moonlight_session_activation_gate_t(
      bool enabled,
      authority_t &authority,
      moonlight_session_binding_registry_t &binding_registry,
      std::shared_ptr<moonlight_controller_feedback_hub_t> feedback_hub
    );
    ~moonlight_session_activation_gate_t();

    moonlight_session_activation_gate_t(
      const moonlight_session_activation_gate_t &
    ) = delete;
    moonlight_session_activation_gate_t &operator=(
      const moonlight_session_activation_gate_t &
    ) = delete;
    moonlight_session_activation_gate_t(
      moonlight_session_activation_gate_t &&
    ) = delete;
    moonlight_session_activation_gate_t &operator=(
      moonlight_session_activation_gate_t &&
    ) = delete;

    [[nodiscard]] moonlight_launch_selection_result_t register_selection(
      moonlight_launch_selection_key_t key,
      seat_handle_t handle,
      std::string_view expected_input_seat,
      bool controller_feedback,
      worker_connection_selection_t worker_connection = {}
    );
    [[nodiscard]] moonlight_session_activation_status_e activate(
      stream::session_t &session
    );

    /** Close new activation without waiting for an activation already entered. */
    [[nodiscard]] std::size_t quiesce() noexcept;
    /** Clear retained selections only when no activation remains in flight. */
    [[nodiscard]] bool finish_close() noexcept;
    void close() noexcept;
    [[nodiscard]] bool enabled() const;
    [[nodiscard]] bool closed() const;
    [[nodiscard]] std::size_t active_selections() const;

  private:
    const std::shared_ptr<moonlight_session_activation_gate_state_t> state_;
  };

  enum class moonlight_activation_install_status_e {
    installed,
    invalid_gate,
    gate_disabled,
    gate_closed,
    already_installed,
  };

  struct moonlight_activation_install_result_t;

  /**
   * RAII removal of one exact process-global activation gate.
   *
   * The owning coordinator must retain this installation until every pending
   * selection has either activated or been cancelled. Removing it earlier
   * deliberately restores the default unselected behavior for new streams.
   */
  class moonlight_activation_installation_t final {
  public:
    ~moonlight_activation_installation_t();

    moonlight_activation_installation_t(
      const moonlight_activation_installation_t &
    ) = delete;
    moonlight_activation_installation_t &operator=(
      const moonlight_activation_installation_t &
    ) = delete;
    moonlight_activation_installation_t(
      moonlight_activation_installation_t &&
    ) = delete;
    moonlight_activation_installation_t &operator=(
      moonlight_activation_installation_t &&
    ) = delete;

    void close() noexcept;
    [[nodiscard]] bool installed() const;

  private:
    explicit moonlight_activation_installation_t(
      std::shared_ptr<moonlight_session_activation_gate_t> gate
    );

    friend moonlight_activation_install_result_t
    install_moonlight_session_activation_gate(
      std::shared_ptr<moonlight_session_activation_gate_t>
    );

    std::shared_ptr<moonlight_session_activation_gate_t> gate_;
  };

  struct moonlight_activation_install_result_t {
    moonlight_activation_install_status_e status =
      moonlight_activation_install_status_e::invalid_gate;
    std::unique_ptr<moonlight_activation_installation_t> installation;
  };

  /**
   * Install an explicitly enabled gate for the stream construction boundary.
   * There is intentionally no production installer callsite yet.
   */
  [[nodiscard]] moonlight_activation_install_result_t
  install_moonlight_session_activation_gate(
    std::shared_ptr<moonlight_session_activation_gate_t> gate
  );

  /** Called exactly once by stream::session::start() before input selection closes. */
  [[nodiscard]] moonlight_session_activation_status_e
  activate_registered_moonlight_session(stream::session_t &session);

  [[nodiscard]] bool moonlight_session_activation_gate_installed();

#ifdef POLARIS_TESTS
  using moonlight_activation_before_bind_hook_t = std::function<void()>;

  /** Deterministic concurrency seam; production builds contain no hook. */
  void set_moonlight_activation_before_bind_hook_for_tests(
    moonlight_activation_before_bind_hook_t hook
  );
#endif

}  // namespace multiseat::input

#endif
