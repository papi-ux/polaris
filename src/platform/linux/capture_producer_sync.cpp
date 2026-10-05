#include "capture_producer_sync.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <sys/ioctl.h>
#include <unistd.h>
#ifdef __linux__
  #include <linux/dma-buf.h>
#endif

namespace capture_producer_sync {
  namespace {
    using clock_t = std::chrono::steady_clock;

    struct fence_owner_t {
      const detail::syscalls_t &calls;
      std::array<int, 4> fds {-1, -1, -1, -1};

      ~fence_owner_t() {
        for (const auto fd : fds) {
          if (fd >= 0) {
            calls.close_fence(calls.context, fd);
          }
        }
      }
    };

    result_t wait_impl(std::span<const int> capture_fds, std::chrono::milliseconds budget, const detail::syscalls_t &calls) {
      if (capture_fds.empty() || capture_fds.size() > 4 || capture_fds.front() < 0 || budget.count() < 0) {
        return {status_e::failed, EINVAL};
      }
      const auto deadline = calls.now(calls.context) + budget;
      fence_owner_t owned {calls};
      std::array<pollfd, 4> pending {};
      std::size_t count = 0;
      for (const auto capture_fd : capture_fds) {
        if (capture_fd < 0) {
          continue;
        }
        int fence_fd = -1;
        while (calls.export_read_fence(calls.context, capture_fd, fence_fd) < 0) {
          const auto error = errno;
          if (error != EINTR) {
            const bool unsupported = error == ENOTTY || error == ENOSYS || error == EOPNOTSUPP;
            return {unsupported ? status_e::unsupported : status_e::failed, error};
          }
          if (calls.now(calls.context) >= deadline) {
            return {status_e::timeout, ETIMEDOUT};
          }
        }
        if (fence_fd < 0) {
          return {status_e::failed, EIO};
        }
        owned.fds[count] = fence_fd;
        pending[count++] = {fence_fd, POLLIN, 0};
      }

      auto remaining = count;
      while (remaining) {
        const auto time_left = std::max(deadline - calls.now(calls.context), clock_t::duration::zero());
        const auto timeout = std::min<std::int64_t>(std::chrono::ceil<std::chrono::milliseconds>(time_left).count(), INT_MAX);
        const auto polled = calls.poll_fences(calls.context, pending.data(), count, static_cast<int>(timeout));
        if (polled < 0) {
          const auto error = errno;
          if (error != EINTR) {
            return {status_e::failed, error};
          }
          if (calls.now(calls.context) >= deadline) {
            return {status_e::timeout, ETIMEDOUT};
          }
          continue;
        }
        if (polled == 0) {
          return {status_e::timeout, ETIMEDOUT};
        }
        bool made_progress = false;
        for (std::size_t i = 0; i < count; ++i) {
          auto &fence = pending[i];
          if (fence.fd < 0) {
            continue;
          }
          if (fence.revents & (POLLERR | POLLNVAL | POLLHUP)) {
            return {status_e::failed, fence.revents & POLLNVAL ? EBADF : EIO};
          }
          if (fence.revents & POLLIN) {
            fence.fd = -1;
            --remaining;
            made_progress = true;
          }
        }
        if (!made_progress) {
          return {status_e::failed, EIO};
        }
      }
      return {status_e::ready, 0};
    }

    const detail::syscalls_t system_calls {
      nullptr,
      [](void *, int capture_fd, int &fence_fd) {
#if defined(__linux__) && defined(DMA_BUF_IOCTL_EXPORT_SYNC_FILE)
        dma_buf_export_sync_file request {DMA_BUF_SYNC_READ, -1};
        const auto result = ioctl(capture_fd, DMA_BUF_IOCTL_EXPORT_SYNC_FILE, &request);
        if (result == 0) {
          fence_fd = request.fd;
        }
        return result;
#else
        errno = ENOSYS;
        return -1;
#endif
      },
      [](void *, pollfd *fds, nfds_t count, int timeout) {return poll(fds, count, timeout);},
      [](void *, int fd) {(void) close(fd);},
      [](void *) {return clock_t::now();},
    };
  }

  result_t wait(std::span<const int> capture_fds, std::chrono::milliseconds budget) {
    return wait_impl(capture_fds, budget, system_calls);
  }

#ifdef POLARIS_TESTS
  result_t wait_for_tests(std::span<const int> capture_fds, std::chrono::milliseconds budget, const detail::syscalls_t &calls) {
    return wait_impl(capture_fds, budget, calls);
  }
#endif
}
