/**
 * @file src/platform/send_wait.h
 * @brief Cancellable UDP send-buffer waits without closing shared sockets.
 */
#pragma once

#include <atomic>
#include <cerrno>
#include <memory>
#ifdef _WIN32
#include <winsock2.h>
#else
#include <poll.h>
#endif

namespace platf::send_wait {
#ifdef _WIN32
  inline bool writable(SOCKET socket, const std::shared_ptr<const std::atomic_bool> &cancellation) {
    while (!cancellation || !cancellation->load(std::memory_order_acquire)) {
      fd_set write_set, error_set;
      FD_ZERO(&write_set);
      FD_ZERO(&error_set);
      FD_SET(socket, &write_set);
      FD_SET(socket, &error_set);
      timeval timeout {0, 50000};
      const auto result = select(0, nullptr, &write_set, &error_set, &timeout);
      if (result == SOCKET_ERROR && WSAGetLastError() != WSAEINTR) {
        return false;
      }
      if (result > 0) {
        return FD_ISSET(socket, &write_set) && !FD_ISSET(socket, &error_set) &&
               (!cancellation || !cancellation->load(std::memory_order_acquire));
      }
    }
    return false;
  }
#else
  inline bool writable(int socket, const std::shared_ptr<const std::atomic_bool> &cancellation) {
    // UDP send calls use MSG_DONTWAIT. Bound the following wait so retiring one
    // stream wakes its sender without changing another stream's shared socket.
    while (!cancellation || !cancellation->load(std::memory_order_acquire)) {
      pollfd descriptor {socket, POLLOUT, 0};
      const auto result = poll(&descriptor, 1, 50);
      if (result < 0 && errno != EINTR) {
        return false;
      }
      if (result > 0) {
        return (descriptor.revents & POLLOUT) &&
               !(descriptor.revents & (POLLERR | POLLHUP | POLLNVAL)) &&
               (!cancellation || !cancellation->load(std::memory_order_acquire));
      }
    }
    return false;
  }
#endif
}
