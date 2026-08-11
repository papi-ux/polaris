/**
 * @file src/platform/linux/session_media.h
 * @brief Single owner for Linux stream media teardown and post-HTTP stop work.
 *
 * All stop paths (Browser Stream stop, WebUI disconnect, Moonlight cancel →
 * terminate_impl, streaming_will_stop) must funnel media teardown through here
 * so portal/PipeWire release and capture joins cannot race or double-run.
 */
#pragma once

#ifdef __linux__

  #include "session_media_gate.h"

  #include <functional>

namespace session_media {

  class pending_start_cancel_owner_t {
  public:
    pending_start_cancel_owner_t() = default;
    pending_start_cancel_owner_t(const pending_start_cancel_owner_t &) = delete;
    pending_start_cancel_owner_t &operator=(const pending_start_cancel_owner_t &) = delete;
    pending_start_cancel_owner_t(pending_start_cancel_owner_t &&other) noexcept;
    pending_start_cancel_owner_t &operator=(pending_start_cancel_owner_t &&other) noexcept;
    ~pending_start_cancel_owner_t();

  private:
    friend pending_start_cancel_owner_t cancel_pending_starts(const void *owner_tag);
    explicit pending_start_cancel_owner_t(const void *owner_tag): owner_tag_(owner_tag) {}
    void reset() noexcept;
    const void *owner_tag_ = nullptr;
  };

  class pending_start_owner_scope_t {
  public:
    explicit pending_start_owner_scope_t(const void *owner_tag);
    pending_start_owner_scope_t(const pending_start_owner_scope_t &) = delete;
    pending_start_owner_scope_t &operator=(const pending_start_owner_scope_t &) = delete;
    ~pending_start_owner_scope_t();

  private:
    const void *previous_ = nullptr;
  };

  pending_start_cancel_owner_t cancel_pending_starts(const void *owner_tag);
  bool pending_start_cancelled(const void *owner_tag);
  const void *pending_start_owner();

  /**
   * @brief Admit a capture/runtime start after all prior teardown owners finish.
   *
   * Hold the returned owner until the new media resource is fully published.
   */
  start_owner_t begin_start();

  /**
   * @brief Mark teardown active and wait for already-admitted starts to finish.
   *
   * Every asynchronous cleanup path keeps an owner until its resource release or
   * capture join reaches a terminal state. New starts remain blocked until the
   * final owner is destroyed.
   */
  teardown_owner_t begin_teardown();

  /**
   * @brief Whether teardown has closed admission for new media starts.
   */
  bool teardown_in_progress();

  /**
   * @brief Ordered media teardown for an ending stream session.
   *
   * 1) signal Browser Stream capture shutdown (if any)
   * 2) portal::release_global_capture() once
   * 3) use short synchronous budgets for HTTP responsiveness
   * 4) wait for every owned asynchronous cleanup to reach terminal state
   * 5) return the root teardown fence so admission remains closed through
   *    compositor/process termination
   *
   * Idempotent and safe when no media is active. Nested compositor kill stays
   * outside this function and is fenced until the final teardown owner exits.
   */
  teardown_owner_t prepare_for_stop();

  /**
   * @brief Run work on the coalescing teardown worker (after HTTPS responds).
   *
   * SimpleWeb flushes the response body when the handler returns; portal
   * destroy can hang in pw_thread_loop_stop. Use this instead of bare
   * std::thread{}.detach() so concurrent stop/disconnect coalesce and we keep
   * a single worker identity in logs.
   */
  void schedule(std::function<void()> work);

}  // namespace session_media

#endif  // __linux__
