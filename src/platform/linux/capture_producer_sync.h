#pragma once

#include <chrono>
#include <poll.h>
#include <span>
#include <string_view>

namespace capture_producer_sync {
  enum class status_e {ready, timeout, unsupported, failed};

  struct result_t {
    status_e status;
    int error;
  };

  inline std::string_view status_name(status_e status) {
    switch (status) {
      case status_e::ready: return "ready";
      case status_e::timeout: return "timeout";
      case status_e::unsupported: return "unsupported";
      case status_e::failed: return "failed";
    }
    return "failed";
  }

  // The caller retains the producer lease throughout this wait and subsequent
  // GPU use. Only exported fence descriptors are owned here, never capture FDs.
  result_t wait(std::span<const int> capture_fds, std::chrono::milliseconds budget);

  namespace detail {
    struct syscalls_t {
      void *context;
      int (*export_read_fence)(void *, int, int &);
      int (*poll_fences)(void *, pollfd *, nfds_t, int);
      void (*close_fence)(void *, int);
      std::chrono::steady_clock::time_point (*now)(void *);
    };
  }

#ifdef POLARIS_TESTS
  result_t wait_for_tests(std::span<const int> capture_fds, std::chrono::milliseconds budget, const detail::syscalls_t &calls);
#endif
}
