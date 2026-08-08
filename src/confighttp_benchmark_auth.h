/**
 * @file src/confighttp_benchmark_auth.h
 * @brief Request-rate limiting for the P0-5 benchmark control surface.
 */
#pragma once

#include <chrono>
#include <deque>
#include <mutex>
#include <string>
#include <unordered_map>

namespace confighttp::benchmark_auth {
  /**
   * @brief Per-IP sliding-window request rate limiter for the P0-5
   * benchmark control surface (measurement-spec-v1.md 6.4's "request rate
   * limits" requirement). Deliberately separate from confighttp.cpp's own
   * login_rate_limits, which guards password-guessing attempts
   * specifically and uses a different, harsher-lockout shape keyed to
   * consecutive failures - a benchmark harness polling GET
   * .../runs/{run_id} every few seconds during a 120-180s run is normal,
   * expected traffic here, not an attack.
   *
   * Time is passed in explicitly rather than read internally from
   * steady_clock::now(), so this stays unit-testable without real sleeps.
   */
  class rate_limiter_t {
  public:
    /**
     * @param max_requests Maximum requests from one IP within window.
     * @param window The trailing duration max_requests is measured over.
     */
    rate_limiter_t(int max_requests, std::chrono::steady_clock::duration window);

    /**
     * @brief Whether ip has already reached max_requests within the
     * trailing window as of now. Does not itself record a request -
     * call record_request() separately for that, regardless of what
     * this returns.
     */
    bool is_rate_limited(const std::string &ip, std::chrono::steady_clock::time_point now);

    /**
     * @brief Record one request from ip at now, for future
     * is_rate_limited() calls to weigh. Also opportunistically evicts
     * this ip's own timestamps older than window.
     */
    void record_request(const std::string &ip, std::chrono::steady_clock::time_point now);

  private:
    int max_requests_;
    std::chrono::steady_clock::duration window_;
    std::mutex mutex_;
    std::unordered_map<std::string, std::deque<std::chrono::steady_clock::time_point>> requests_by_ip_;
  };
}  // namespace confighttp::benchmark_auth
