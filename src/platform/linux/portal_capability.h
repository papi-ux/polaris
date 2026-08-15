/**
 * @file src/platform/linux/portal_capability.h
 * @brief Process capability policy for XDG Desktop Portal capture.
 */
#pragma once

#ifdef __linux__

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
   * default.
   */
  bool requires_unprivileged_process(
    std::string_view configured_capture,
    std::string_view stream_mode
  );

  /**
   * Drop inherited executable capabilities before worker threads are created.
   *
   * A file-capability-enabled process is non-dumpable. xdg-desktop-portal
   * consequently cannot inspect /proc/<pid>/root to authorize ScreenCast. A
   * portal capture path does not need Polaris' KMS capability, so make the
   * process equivalent to an ordinary unprivileged Polaris invocation.
   */
  prepare_result_e prepare_process_for_capture(
    std::string_view configured_capture,
    std::string_view stream_mode
  );

}  // namespace portal_capability

#endif  // __linux__
