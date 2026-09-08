/**
 * @file src/platform/linux/multiseat_worker_client.h
 * @brief Authenticated controller-side Unix transport for multiseat workers.
 */
#pragma once

#ifdef __linux__

  #include "multiseat_worker_authority.h"

  #include <chrono>
  #include <memory>
  #include <optional>
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
   * Copyable lease on one mutually authenticated connection, never a client
   * lookup. Reconnect and client destruction retire every old lease; operations
   * through an old lease cannot attach, read from, or close the replacement.
   * Dropping a lease does not close the client's connection. Explicit close()
   * retires this connection and all its copies without waiting for consumers.
   *
   * Identity and equality describe the original connection even after close;
   * equal endpoint identities do not imply equal connections. They do not grant
   * a launch or stream authorization. Returned bytes still require downstream
   * stream lifetime fencing before delivery to a network client.
   */
  class controller_connection_t {
  public:
    controller_connection_t() = default;

    [[nodiscard]] std::optional<endpoint_identity_t> identity() const;
    [[nodiscard]] bool operator==(const controller_connection_t &) const = default;
    [[nodiscard]] transport_status_e attach_data_plane() const;
    [[nodiscard]] transport_status_e send_input(std::span<const std::uint8_t> payload) const;
    [[nodiscard]] transport_status_e receive_feedback(std::vector<std::uint8_t> &payload) const;
    [[nodiscard]] transport_status_e receive_media(encoded_media_packet_t &packet) const;
    [[nodiscard]] transport_status_e heartbeat(channel_e channel) const;
    [[nodiscard]] transport_status_e shutdown() const;
    void close() const noexcept;
    [[nodiscard]] bool connected() const noexcept;
    [[nodiscard]] bool data_plane_attached() const noexcept;

  private:
    friend class controller_client_t;
    struct implementation_t;
    explicit controller_connection_t(std::shared_ptr<implementation_t> connection);
    std::shared_ptr<implementation_t> connection_;
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
   *
   * Concurrent control and media operations use independent readers. Each
   * channel admits one request awaiting its ACK; async packets share a bounded
   * 64-frame / 32 MiB queue. Idle channels have no deadline, while an incomplete
   * frame, request, or empty consumer wait retains the configured I/O deadline.
   * Any timeout or protocol failure retires both channels. close() cancels
   * readers, writers, authentication, and pending request admission without
   * waiting for their deadlines. Operations retain their original connection
   * through completion; reconnect never redirects an old operation.
   *
   * The caller must retain this client during its member calls and the
   * connect() authority through connection establishment. Connection leases
   * can outlive this client but retire when it closes. Callers synchronize
   * access to supplied buffers and retain coordinator authority separately.
   * Returned packet values belong to the connection that delivered them;
   * downstream stream ownership must prevent their reuse after retirement.
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
    /** Empty until both channels authenticate, and after shutdown or close. */
    [[nodiscard]] controller_connection_t lease_connection() const;
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
