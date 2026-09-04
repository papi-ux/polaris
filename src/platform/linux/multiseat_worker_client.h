/**
 * @file src/platform/linux/multiseat_worker_client.h
 * @brief Authenticated controller-side Unix transport for multiseat workers.
 */
#pragma once

#ifdef __linux__

#include "multiseat_worker_authority.h"

#include <chrono>
#include <memory>

namespace multiseat::worker_ipc {

  enum class transport_status_e {
    applied,
    invalid_argument,
    unavailable,
    timeout,
    peer_rejected,
    protocol_rejected,
    authentication_rejected,
    io_error,
    closed,
  };

  struct controller_client_options_t {
    std::chrono::milliseconds connect_timeout {2000};
    std::chrono::milliseconds handshake_timeout {2000};
    std::chrono::milliseconds io_timeout {5000};
  };

  /**
   * Owns authenticated control and media connections to one exact worker.
   *
   * `connect()` is all-or-nothing: both same-UID sockets must complete mutual
   * authentication for the exact authority generation. This checkpoint only
   * exposes heartbeat and graceful shutdown; gameplay input and media routing
   * remain deliberately unwired.
   */
  class controller_client_t {
  public:
    controller_client_t();
    ~controller_client_t();

    controller_client_t(const controller_client_t &) = delete;
    controller_client_t &operator=(const controller_client_t &) = delete;
    controller_client_t(controller_client_t &&) = delete;
    controller_client_t &operator=(controller_client_t &&) = delete;

    [[nodiscard]] transport_status_e connect(
      const authority_handle_t &authority,
      controller_client_options_t options = {}
    );
    [[nodiscard]] transport_status_e heartbeat(channel_e channel);
    [[nodiscard]] transport_status_e shutdown();
    void close() noexcept;

    [[nodiscard]] bool connected() const;

  private:
    struct implementation_t;
    std::unique_ptr<implementation_t> implementation_;
  };

}  // namespace multiseat::worker_ipc

#endif
