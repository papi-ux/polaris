/**
 * @file src/platform/linux/multiseat_worker_client.h
 * @brief Authenticated controller-side Unix transport for multiseat workers.
 */
#pragma once

#ifdef __linux__

#include "multiseat_worker_authority.h"

#include <chrono>
#include <memory>
#include <span>
#include <vector>

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

  struct encoded_media_packet_t {
    message_e message = message_e::video;
    std::vector<std::uint8_t> payload;

    bool operator==(const encoded_media_packet_t &) const = default;
  };

  /**
   * Owns authenticated control and media connections to one exact worker.
   *
   * `connect()` is all-or-nothing: both same-UID sockets must complete mutual
   * authentication for the exact authority generation. This checkpoint only
   * exposes heartbeat and graceful shutdown immediately. Data-plane attachment
   * is separate so health probes cannot consume media. Once both channels are
   * attached, input travels only controller-to-worker while feedback and
   * already encoded media travel only worker-to-controller.
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
    [[nodiscard]] transport_status_e attach_data_plane();
    [[nodiscard]] transport_status_e send_input(std::span<const std::uint8_t> payload);
    [[nodiscard]] transport_status_e receive_feedback(std::vector<std::uint8_t> &payload);
    [[nodiscard]] transport_status_e receive_media(encoded_media_packet_t &packet);
    [[nodiscard]] transport_status_e heartbeat(channel_e channel);
    [[nodiscard]] transport_status_e shutdown();
    void close() noexcept;

    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] bool data_plane_attached() const noexcept;

  private:
    struct implementation_t;
    std::unique_ptr<implementation_t> implementation_;
  };

}  // namespace multiseat::worker_ipc

#endif
