/**
 * @file src/platform/linux/private_session_attach.h
 * @brief Prove that a launched app actually attached to the private session.
 */
#pragma once

#ifdef __linux__

  #include <chrono>
  #include <string>

namespace private_session_attach {

  /// Outcome of one enumeration of the private compositor's toplevel list.
  enum class probe_status_e {
    ok,  ///< Connected and enumerated; toplevel_count is meaningful.
    unsupported,  ///< Connected, but no toplevel-list global is advertised.
    unavailable,  ///< Could not reach the private compositor at all.
  };

  struct probe_result_t {
    probe_status_e status = probe_status_e::unavailable;
    int toplevel_count = 0;
  };

  /// What the watcher concludes from one probe at one point in time.
  enum class verdict_e {
    attached,  ///< A window exists in the private session.
    waiting,  ///< Nothing yet, and the grace period is still running.
    never_attached,  ///< Grace elapsed and no window ever appeared. Report only.
    skipped,  ///< The guard could not measure this session; stay silent.
  };

  /// Grace period a launch gets before a windowless session is called a failure.
  inline constexpr std::chrono::milliseconds k_default_attach_grace {120000};

  /// Interval between probes while waiting for a window to appear.
  inline constexpr std::chrono::milliseconds k_default_attach_poll {2000};

  /**
   * @brief Enumerate toplevel windows on @p wayland_socket.
   *
   * Binds ext-foreign-toplevel-list-v1 and counts the toplevels the compositor
   * reports. Managed Xwayland windows are included, so one probe covers native
   * Wayland clients and the ordinary X11 windows Proton and Wine produce.
   *
   * It does not see X11 override-redirect windows. wlroots treats those as
   * unmanaged surfaces, so labwc builds no view and no toplevel handle for them.
   * Measured against labwc 0.9.6: adding an override-redirect window to a session
   * already holding one managed window leaves the count at 1. Splash screens and
   * some fullscreen paths map exactly that kind of window, so a zero count is
   * evidence of a problem rather than proof of one.
   *
   * That blind spot is why the verdict below is reported and never enforced.
   * Tearing a session down on this signal would sometimes kill a stream whose
   * app attached correctly and simply mapped a window Polaris cannot enumerate.
   */
  probe_result_t probe_toplevels(const std::string &wayland_socket);

  /**
   * @brief Decide what a probe means. Pure, so it is testable without a compositor.
   *
   * A probe that could not measure anything never produces an accusation: an
   * unreachable compositor resolves to waiting inside the grace period and to
   * skipped past it, never to never_attached.
   */
  verdict_e evaluate(
    const probe_result_t &probe,
    std::chrono::milliseconds elapsed,
    std::chrono::milliseconds grace
  );

  /**
   * @brief True when a command can lose the private DISPLAY at a Flatpak portal hop.
   *
   * Measured on Fedora/KDE 2026-08-14 against issue #234: the Flatpak portal
   * builds each sandbox from the portal service's own environment, so it stamps
   * the login session's DISPLAY over whatever the caller exported and binds only
   * that one X socket into the container. An explicit --env=DISPLAY override does
   * not survive it either. Anything that reaches the portal therefore routes X11
   * clients back to the host session no matter what Polaris sets.
   *
   * pressure-vessel and a direct `flatpak run` both honor the caller correctly,
   * so this is specifically about commands that can spawn back out through the
   * portal, not about Flatpak or Proton as such. See docs/troubleshooting.md.
   */
  bool may_lose_display_to_flatpak_portal(const std::string &cmd);

}  // namespace private_session_attach

#endif
