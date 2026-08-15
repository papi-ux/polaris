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
    never_attached,  ///< Grace elapsed and no window ever appeared. Reported, never enforced.
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
   * reports. Managed Xwayland windows are included, so this covers native Wayland
   * clients and the ordinary X11 windows Proton and Wine produce.
   *
   * It does not see X11 override-redirect windows. wlroots treats those as
   * unmanaged surfaces, so labwc builds no view and no toplevel handle for them.
   * Measured against labwc 0.9.6: adding an override-redirect window to a session
   * already holding one managed window leaves the count at 1. Use
   * probe_private_session() rather than this alone, so that class is covered.
   */
  probe_result_t probe_toplevels(const std::string &wayland_socket);

  /**
   * @brief Count mapped client windows on the private session's Xwayland.
   *
   * Covers what the Wayland toplevel list cannot: an override-redirect window is
   * an ordinary child of the X root even though the compositor never manages it.
   *
   * Only viewable children count. Measured against labwc 0.9.6, an idle private
   * Xwayland carries four root children of its own (sized 10x10 and 8192x8192)
   * and every one of them is IsUnmapped, so a viewable count of zero is a real
   * absence rather than internals being miscounted. A managed window reads 1, and
   * adding an override-redirect window reads 2.
   *
   * Uses xcb rather than Xlib deliberately. Xlib's default I/O error handler
   * calls exit() when a display connection drops, and this probe runs against a
   * compositor that is expected to exit at session end, so Xlib would eventually
   * take Polaris down with it. xcb reports a lost connection as a return value.
   */
  probe_result_t probe_x11_windows(const std::string &x11_display);

  /**
   * @brief Fold two probes into one verdict. Attached when either saw a window.
   *
   * Pure, so the combination rules are testable without a compositor. A signal
   * that could not measure anything never suppresses one that did: a working
   * Wayland probe still decides the outcome when there is no Xwayland to ask.
   *
   * The resulting count is a lower bound rather than a total. The two signals
   * overlap, because a managed X11 window is both a Wayland toplevel and a
   * viewable child of the X root, so this takes the larger of the two rather
   * than reporting one window twice.
   */
  probe_result_t combine(const probe_result_t &wayland, const probe_result_t &x11);

  /// Run both signals against one private session and combine them.
  probe_result_t probe_private_session(
    const std::string &wayland_socket,
    const std::string &x11_display
  );

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
