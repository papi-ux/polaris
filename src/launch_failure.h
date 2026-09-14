/**
 * @file src/launch_failure.h
 * @brief Why a launch was refused, carried from the code that knows to the response the client shows.
 *
 * A launch that cannot start returns 503 from about twenty places, each of
 * which logs the real reason and then hands the HTTP layer a bare number. The
 * client shows "error 503" and the person on the couch reads it as "Polaris is
 * broken"; the support thread starts by asking for the journal. The refusing
 * code records its reason here, on the thread that is handling the launch,
 * and the launch and resume handlers put it on the response as the
 * status_message plus error_code / error_action root attributes, which
 * Moonlight ignores and Nova reads.
 */
#pragma once

#include <optional>
#include <string>
#include <utility>

namespace launch_failure {
  struct record_t {
    int status = 0;
    std::string code;  ///< a stable snake_case identifier, greppable in docs and support threads
    std::string message;  ///< what happened, in one or two plain sentences
    std::string action;  ///< the one change that fixes it, when there is one
  };

  /// The refusal recorded for the launch attempt this thread is handling.
  inline thread_local std::optional<record_t> pending;

  /**
   * @brief Record why the launch on this thread is being refused; returns the status to propagate.
   * @details Overwrites an earlier record: the code that knows most is the one closest to the fault.
   */
  inline int refuse(int status, std::string code, std::string message, std::string action = {}) {
    pending = record_t {status, std::move(code), std::move(message), std::move(action)};
    return status;
  }

  /**
   * @brief Record a reason only when nothing more specific was recorded first.
   * @details For the outer failure sites that wrap a whole start sequence: "the compositor did not
   *          start" must not replace "no encoder could be probed against it".
   */
  inline int refuse_if_unexplained(int status, std::string code, std::string message, std::string action = {}) {
    if (!pending) {
      pending = record_t {status, std::move(code), std::move(message), std::move(action)};
    }
    return status;
  }

  /// Consume the record for this thread's current attempt; empty when the refusal did not say why.
  inline std::optional<record_t> take() {
    auto taken = std::move(pending);
    pending.reset();
    return taken;
  }

  /// Forget a record left by an earlier attempt on this thread.
  inline void clear() {
    pending.reset();
  }

  /// The one string a Moonlight client shows: the message, then the action.
  inline std::string status_message(const record_t &record) {
    return record.action.empty() ? record.message : record.message + " " + record.action;
  }
}  // namespace launch_failure
