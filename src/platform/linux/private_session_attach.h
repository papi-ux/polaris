/**
 * @file src/platform/linux/private_session_attach.h
 * @brief Prove that a launched app actually attached to the private session.
 */
#pragma once

#ifdef __linux__

  #include <chrono>
  #include <cstdint>
  #include <functional>
  #include <optional>
  #include <string>
  #include <vector>

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

  /**
   * @brief The app id a command starts with `flatpak run`, or through an exported launcher wrapper.
   *
   * The first word after `run` that is not an option and reads as a Flatpak app id, which has at
   * least three dot separated parts; a ref's branch or arch is dropped. It names the app only for
   * logging and for telling a desktop instance of the same app apart, never to signal anything.
   */
  std::optional<std::string> flatpak_run_app_id(const std::string &cmd);

  /// What one top level X11 window in the private session says about itself.
  struct x11_window_t {
    std::uint32_t id = 0;
    bool viewable = false;  ///< mapped and on screen
    bool override_redirect = false;  ///< a menu or tooltip no window manager closes
    std::optional<std::uint32_t> pid;  ///< _NET_WM_PID, when the client set it
    bool accepts_delete = false;  ///< WM_DELETE_WINDOW is among its WM_PROTOCOLS
    /// The pid of the client that made the window, as the X server knows it from the client's
    /// socket through X-Resource. It is a pid on the host, even for a client in a sandbox's pid
    /// namespace, whose _NET_WM_PID is its pid inside the sandbox.
    std::optional<std::uint32_t> client_pid;
  };

  /**
   * @brief Which processes are the app's, asked of the process each window names.
   *
   * The two questions differ because the two names do: X-Resource gives a host pid, while
   * _NET_WM_PID is whatever pid the client saw itself as, which inside a Flatpak or
   * pressure-vessel pid namespace is a small pid of that namespace.
   */
  struct window_owner_test_t {
    /// Asked of a host pid, the client pid X-Resource reports.
    std::function<bool(std::uint32_t pid)> host_pid;
    /// Asked of a _NET_WM_PID, when the server could not report the client pid. Left empty, the
    /// window's own word is taken as a host pid, as it is for a client outside any sandbox.
    std::function<bool(std::uint32_t pid)> net_wm_pid;
  };

  /**
   * @brief The windows a player closing an app would close, given which processes are the app's.
   *
   * Pure, so the choice is testable without an X server. Only viewable, managed windows that ask
   * to be told about a close are chosen, and only when their process is the app's. The client pid
   * X-Resource reports is preferred to _NET_WM_PID, and when it is known the window's own word is
   * not consulted at all. A window that names no process either way cannot be told apart from
   * Steam's own, and closing a window that never asked for WM_DELETE_WINDOW is what a window
   * manager does by killing the client, which is the opposite of asking.
   */
  std::vector<std::uint32_t> close_targets(
    const std::vector<x11_window_t> &windows,
    const window_owner_test_t &belongs_to_app
  );

  /// close_targets with one test for either name, which suits an app that runs in no sandbox.
  std::vector<std::uint32_t> close_targets(
    const std::vector<x11_window_t> &windows,
    const std::function<bool(std::uint32_t pid)> &belongs_to_app
  );

  /// Outcome of asking an app's X11 windows to close.
  struct close_request_result_t {
    probe_status_e status = probe_status_e::unavailable;
    int windows_asked = 0;  ///< close requests sent, one per chosen window
    bool timed_out = false;  ///< the display did not answer in time, so nothing is known to be asked
    bool client_pids = false;  ///< the server reported each window's client pid through X-Resource
  };

  /**
   * @brief Ask the app's windows on @p x11_display to close, the way clicking a window's close button
   *        does: an ICCCM WM_DELETE_WINDOW client message to each window close_targets chooses.
   *
   * Nothing is injected into the app's input and nothing is killed. Wine turns the message into
   * the close a Windows game gets from its title bar, so a Proton title quits the way it would on its
   * own. Native Wayland windows are out of reach: the protocols that list them name no process, so
   * they cannot be told apart from Steam's.
   */
  close_request_result_t request_x11_window_close(
    const std::string &x11_display,
    const window_owner_test_t &belongs_to_app
  );

  /// The exchange request_x11_window_close_within runs.
  using close_request_t =
    std::function<close_request_result_t(const std::string &, const window_owner_test_t &)>;

  /**
   * @brief request_x11_window_close, given at most @p timeout to finish.
   *
   * The X exchange has no timeout of its own, and the stop that asks holds the session lifecycle
   * lock that a new launch and a stop request's answer wait on. An Xwayland that stops answering,
   * behind a hung compositor or a client holding a server grab, must not hold them as well. The
   * exchange runs on a thread of its own. When it has not finished in time, the result says so and
   * asks nothing, the stop goes on as though no window could be asked, and the thread ends once the
   * display goes away with the private session.
   *
   * @param request The exchange, request_x11_window_close unless a test stands in for it.
   */
  close_request_result_t request_x11_window_close_within(
    const std::string &x11_display,
    window_owner_test_t belongs_to_app,
    std::chrono::milliseconds timeout,
    close_request_t request = request_x11_window_close
  );

}  // namespace private_session_attach

#endif
