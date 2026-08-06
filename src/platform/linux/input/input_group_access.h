/**
 * @file src/platform/linux/input/input_group_access.h
 * @brief Whether the streaming account can still open its own isolated input devices.
 *
 * Seat isolation assigns the devices Polaris creates for a client to the
 * `seat-polaris` seat, which stops logind from granting the local desktop
 * session an automatic `uaccess` ACL for them. That ACL is also what normally
 * gives Polaris itself access, so once it is gone the bundled rules'
 * `GROUP="input"` and mode `0660` are the entire access policy. An account
 * outside `input` can no longer open the devices Polaris just created, and
 * neither can the streamed game: gamepads and the virtual keyboard and mouse
 * stop working with nothing in the failure pointing at group membership.
 *
 * Enumeration reports zero visible virtual gamepads and the game sees no
 * controller at all, which reads as a broken isolation feature rather than a
 * missing group. It is cheap to check up front, so it is checked and reported
 * once instead of being diagnosed from a support thread.
 *
 * See input_seat_isolation.h for the marker the udev rules match on.
 */
#pragma once

#include <string>
#include <string_view>

namespace platf::input_access {

  /// The group the bundled udev rules give the isolated devices to.
  constexpr std::string_view input_group = "input";

  /// What the host says about one account's `input` group membership.
  struct input_group_status_t {
    std::string user;  ///< Account the check applies to, for the message.
    bool group_exists = false;  ///< Whether the host has an `input` group at all.
    bool member = false;  ///< Whether the account is in it.
  };

  /// Which seat isolation options a configuration has turned on.
  struct seat_isolation_options_t {
    bool gamepad = false;
    bool keyboard_mouse = false;

    bool any() const {
      return gamepad || keyboard_mouse;
    }
  };

  /// Read the two seat isolation options out of the active configuration.
  seat_isolation_options_t configured_seat_isolation();

  /// Name the devices the enabled options apply to, for the message.
  std::string isolated_device_summary(const seat_isolation_options_t &options);

  /// The command that adds an account to the group.
  std::string input_group_remedy_command(std::string_view user);

  /**
   * @brief Explain a configuration that cannot work as configured.
   *
   * Pure: the caller supplies both the configuration and what was found on the
   * host, so the message is testable without a host that reproduces it.
   *
   * @return The warning, or empty when the configuration is fine.
   */
  std::string seat_isolation_access_warning(
    const seat_isolation_options_t &options,
    const input_group_status_t &status
  );

  /// Look up one account's membership by name.
  input_group_status_t input_group_status_for_user(std::string_view user);

  /// Look up the membership of the account this process runs as.
  input_group_status_t current_input_group_status();

  /**
   * @brief The account `--setup-host` should report on.
   *
   * Host setup runs under `sudo`, so the process' own membership is root's and
   * says nothing about the account that will actually stream. `SUDO_USER` names
   * the account that asked for it.
   */
  std::string setup_host_target_user();

  /**
   * @brief Advice for `--setup-host` to print, or empty when the account is ready.
   *
   * Host setup runs before the configuration is parsed, so it cannot know
   * whether seat isolation is enabled and states the requirement conditionally.
   * Preparing the host is the moment to mention it either way.
   */
  std::string setup_host_input_group_advice();

  /// Warn once at startup when seat isolation cannot work for this account.
  void warn_if_seat_isolation_lacks_input_group();

}  // namespace platf::input_access
