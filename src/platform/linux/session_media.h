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

  #include <functional>

namespace session_media {

  /**
   * @brief Ordered media teardown for an ending stream session.
   *
   * 1) signal Browser Stream capture shutdown (if any)
   * 2) portal::release_global_capture() once
   * 3) bounded capture thread join (≤3s)
   * 4) brief settle only when media was live
   *
   * Idempotent and safe when no media is active. Nested compositor kill stays
   * outside this function (caller runs proc::terminate / runtime stop after).
   */
  void prepare_for_stop();

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
