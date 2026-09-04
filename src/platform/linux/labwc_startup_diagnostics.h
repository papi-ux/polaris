/**
 * @file src/platform/linux/labwc_startup_diagnostics.h
 * @brief Bounded, generation-scoped evidence from a labwc startup client.
 */
#pragma once

#ifdef __linux__

#include <cstddef>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace labwc_startup_diagnostics {

  inline constexpr std::size_t max_retained_stderr_bytes = 8U * 1024U;
  inline constexpr std::size_t max_log_summary_bytes = 1024U;

  struct redacted_text_t {
    std::string text;
    bool truncated = false;
  };

  struct snapshot_t {
    std::optional<int> shell_exit_status;
    std::size_t stderr_bytes_seen = 0;
    bool stderr_truncated = false;
    std::string stderr_summary;
  };

  /**
   * @brief Thread-safe in-memory state for one immutable runtime generation.
   *
   * The collector retains only the tail of stderr. A caller must present the
   * exact generation id before receiving a snapshot, preventing a detached
   * attach watcher from reading evidence from a later private session.
   */
  class collector_t {
  public:
    explicit collector_t(std::string session_instance_id);

    void append_stderr(std::string_view bytes);
    void record_shell_exit_status(int status);
    std::optional<snapshot_t> snapshot(std::string_view expected_session_instance_id) const;

  private:
    std::string session_instance_id_;
    mutable std::mutex mutex_;
    std::string stderr_tail_;
    std::size_t stderr_bytes_seen_ = 0;
    bool stderr_truncated_ = false;
    std::optional<int> shell_exit_status_;
  };

  /** Flatten log injection and redact common credentials, identities, and endpoints. */
  redacted_text_t redact_stderr_for_log(std::string_view raw);

  /** Render a stable one-line field set containing only already-redacted evidence. */
  std::string describe_for_log(const snapshot_t &snapshot);

  /**
   * @brief Wrap a shell program so its stderr and eventual shell status are
   *        written to inherited descriptors while preserving its exit status.
   */
  std::string instrument_shell_command(
    std::string_view command,
    int stderr_fd,
    int status_fd
  );

  /**
   * @brief Encode a shell program for labwc's `-s` argument.
   *
   * labwc tokenizes this string with GLib and calls execvp() directly, so the
   * complete program must be the single argument following `bash -c`.
   */
  std::string make_labwc_startup_command(std::string_view shell_program);

  /**
   * @brief Drain the two read descriptors into @p collector, then close them.
   *
   * Intended to run on one detached reader thread for the runtime lifetime.
   */
  void drain_pipes(
    int stderr_fd,
    int status_fd,
    const std::shared_ptr<collector_t> &collector
  );

}  // namespace labwc_startup_diagnostics

#endif  // __linux__
