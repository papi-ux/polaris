/**
 * @file src/confighttp_benchmark_auth.cpp
 * @brief Request-rate limiting for the P0-5 benchmark control surface.
 */
#include "confighttp_benchmark_auth.h"

namespace confighttp::benchmark_auth {
  rate_limiter_t::rate_limiter_t(int max_requests, std::chrono::steady_clock::duration window):
      max_requests_(max_requests),
      window_(window) {
  }

  bool rate_limiter_t::is_rate_limited(const std::string &ip, std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto it = requests_by_ip_.find(ip);
    if (it == requests_by_ip_.end()) {
      return false;
    }

    auto &timestamps = it->second;
    while (!timestamps.empty() && now - timestamps.front() > window_) {
      timestamps.pop_front();
    }
    return static_cast<int>(timestamps.size()) >= max_requests_;
  }

  void rate_limiter_t::record_request(const std::string &ip, std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    auto &timestamps = requests_by_ip_[ip];
    while (!timestamps.empty() && now - timestamps.front() > window_) {
      timestamps.pop_front();
    }
    timestamps.push_back(now);
  }
}  // namespace confighttp::benchmark_auth
