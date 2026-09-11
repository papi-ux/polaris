/**
 * @file src/platform/linux/multiseat_worker_launch_connection.h
 * @brief One authenticated launch's reservation of its original worker connection.
 */
#pragma once

#ifdef __linux__

  #include "multiseat_worker_launch_authority.h"

  #include <atomic>
  #include <memory>
  #include <string_view>

namespace rtsp_stream {
  struct launch_session_t;
}

namespace multiseat::input {
  class moonlight_worker_launch_adapter_t;
  class moonlight_session_activation_gate_t;

  /**
   * Minted only inside serialized worker authorization for an authenticated
   * launch. Copies retain the original connection, never a client lookup.
   * One stream allocation may claim this reservation. Retirement is permanent
   * and does not close the coordinator-owned worker transport.
   *
   * This reserves ownership only: it does not attach the data plane or grant
   * network delivery. Future consumers must also acquire the stream's packet
   * destination and obey negotiated media configuration.
   */
  class worker_launch_connection_t final {
  public:
    [[nodiscard]] bool matches(
      std::uint32_t launch_id, std::uint64_t lifecycle_generation,
      std::string_view client_key, const seat_handle_t &handle
    ) const;
    [[nodiscard]] bool try_claim(std::uint64_t stream_generation);
    [[nodiscard]] bool bound_to(std::uint64_t stream_generation) const;
    [[nodiscard]] bool matches_launch(const rtsp_stream::launch_session_t &launch) const;
    [[nodiscard]] bool matches_selection(std::uint32_t launch_id,
      std::uint64_t lifecycle_generation, const seat_handle_t &handle) const;
    [[nodiscard]] bool matches_stream(std::uint32_t launch_id, std::uint64_t lifecycle_generation,
      std::string_view client_key, const seat_handle_t &handle,
      const std::shared_ptr<const std::atomic_bool> &requirement) const;
    void retire() noexcept;
    /**
     * The reserved connection, for the one consumer that carries this stream's
     * media. Empty unless this reservation is still live and already claimed by
     * exactly that stream generation, so no other stream and no retired
     * reservation can reach the worker.
     */
    [[nodiscard]] worker_ipc::controller_connection_t stream_connection(
      std::uint64_t stream_generation
    ) const;

#ifdef POLARIS_TESTS
    [[nodiscard]] const worker_ipc::controller_connection_t &connection_for_tests() const {
      return connection_;
    }
#endif

  private:
    friend class moonlight_worker_launch_adapter_t;
    friend class moonlight_session_activation_gate_t;
    [[nodiscard]] bool try_register();
    worker_launch_connection_t(
      std::shared_ptr<rtsp_stream::launch_session_t> launch,
      authenticated_worker_seat_t seat,
      worker_ipc::controller_connection_t connection
    );

    const std::shared_ptr<rtsp_stream::launch_session_t> launch_;
    const std::uint32_t launch_id_;
    const std::uint64_t lifecycle_generation_;
    const authenticated_worker_seat_t seat_;
    const worker_ipc::controller_connection_t connection_;
    std::atomic_bool registered_ {false};
    std::atomic<std::uint64_t> stream_generation_ {0};
    std::atomic_bool retired_ {false};
  };

  /** Required mode is independent of pointer presence: missing means reject. */
  struct worker_connection_selection_t {
    bool required = false;
    std::shared_ptr<worker_launch_connection_t> connection;
  };
}

#endif
