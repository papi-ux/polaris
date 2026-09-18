/**
 * @file src/platform/linux/portal_capability.h
 * @brief Process capability policy for XDG Desktop Portal capture.
 */
#pragma once

#ifdef __linux__

  #include <string>
  #include <string_view>

namespace portal_capability {

  enum class prepare_result_e {
    not_needed,
    unchanged,
    dropped,
    failed,
  };

  /**
   * Return whether the configured capture path may need the desktop portal.
   * Explicit non-portal capture selections always win over the stream-mode
   * default. A host set to KWin screens also needs it: KWin identifies the
   * client asking for a screen by /proc/<pid>/exe, which a process holding
   * file capabilities hides.
   */
  bool requires_unprivileged_process(
    std::string_view configured_capture,
    std::string_view stream_mode,
    std::string_view virtual_display_backend = {}
  );

  /**
   * Drop inherited executable capabilities before any other thread exists.
   *
   * A file-capability-enabled process is non-dumpable. xdg-desktop-portal
   * consequently cannot inspect /proc/<pid>/root to authorize ScreenCast, and
   * KWin cannot read /proc/<pid>/exe. A portal capture path does not need
   * Polaris' KMS capability, so make the process equivalent to an ordinary
   * unprivileged Polaris invocation.
   *
   * capset() covers only the calling thread, while dumpability covers the
   * whole process, so this refuses to make a process dumpable while another
   * thread could still hold the capability. Call it before logging starts its
   * thread; it does not log, and `outcome` says what happened for the caller
   * to log once logging is up.
   */
  prepare_result_e prepare_process_for_capture(
    std::string_view configured_capture,
    std::string_view stream_mode,
    std::string_view virtual_display_backend = {},
    std::string *outcome = nullptr
  );

}  // namespace portal_capability

#endif  // __linux__
