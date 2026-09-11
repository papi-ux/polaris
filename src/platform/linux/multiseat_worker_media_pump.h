/**
 * @file src/platform/linux/multiseat_worker_media_pump.h
 * @brief Carries one worker's encoded media to one stream, under its contract.
 */
#pragma once

#ifdef __linux__

  #include "multiseat_worker_client.h"

  #include <cstdint>
  #include <functional>
  #include <optional>
  #include <string_view>
  #include <utility>
  #include <vector>

namespace multiseat::media {

  /**
   * What the client negotiated with the host. The worker's announced contract
   * has to serve exactly this, or the stream never starts: a client decodes
   * what it asked for, and nothing downstream re-encodes.
   */
  struct expected_media_t {
    std::uint16_t width = 0;
    std::uint16_t height = 0;
    /** Whole frames per second, as the client asked for them. */
    std::uint32_t fps = 0;
    /** Moonlight's video format index. Only 0, H.264, can be served today. */
    int video_format = 0;
    std::uint8_t audio_channels = 0;
  };

  enum class pump_status_e {
    /** The session asked the stream to end. */
    ended_on_shutdown,
    /** The worker said it had no more media. */
    ended_on_end_of_stream,
    no_connection,
    attach_refused,
    /** The connection ended before the worker announced anything. */
    announced_nothing,
    /** A frame arrived where the contract should have been. */
    frame_before_contract,
    /** The announced contract is not representable. */
    unrepresentable_contract,
    /** The contract is representable but is not what the client negotiated. */
    mismatched_contract,
    acknowledgement_refused,
    /** A frame whose prefix, index or payload the contract does not allow. */
    malformed_frame,
    transport_lost,
  };

  [[nodiscard]] std::string_view describe(pump_status_e status);

  /** True for the two ways a stream ends without a fault. */
  [[nodiscard]] bool ended_cleanly(pump_status_e status);

  /**
   * Where delivered media goes. The pump owns no packet type of its own, so
   * the caller decides what a frame becomes; production builds the host's own
   * video and audio packets and stamps them with the stream's destination.
   */
  struct delivery_sinks_t {
    std::function<void(std::vector<std::uint8_t> &&bytes, std::int64_t frame_index, bool idr)> video;
    std::function<void(std::vector<std::uint8_t> &&bytes)> audio;
  };

  /**
   * What the host asks of the worker while the stream runs. Each is polled,
   * never blocking longer than the pump's own tick, so a stop is prompt.
   */
  struct host_requests_t {
    /** True once the session is ending; the pump then closes its transport. */
    std::function<bool()> stop_requested;
    /** True when the host wants the next frame to be an IDR; consumes it. */
    std::function<bool()> take_idr_request;
    /** An inclusive frame span the client can no longer reference; consumes it. */
    std::function<std::optional<std::pair<std::int64_t, std::int64_t>>()> take_invalidation;
  };

  struct pump_report_t {
    pump_status_e status = pump_status_e::no_connection;
    worker_ipc::media_config_t contract {};
    std::uint64_t video_frames = 0;
    std::uint64_t audio_frames = 0;
    std::uint64_t discontinuities = 0;
    std::uint64_t idr_requests = 0;
    std::uint64_t invalidations = 0;
    std::uint64_t last_frame_index = 0;
    /** Set when a fault carries a detail worth logging once. */
    std::string detail;
  };

  /**
   * Attach the data plane, take the worker's announced contract, hold it
   * against what the client negotiated, acknowledge it, then deliver every
   * frame to the sinks until the session ends. Blocks for the whole stream.
   *
   * The worker must keep producing: an idle media channel retires the
   * transport at the client's I/O deadline, which ends the stream as a
   * transport loss rather than a pause. That is the contract's own meaning of
   * continuous, and it is why the pump has no keepalive of its own.
   *
   * Nothing here falls back to host capture. Every fault returns a status the
   * caller must treat as the end of the stream.
   */
  [[nodiscard]] pump_report_t run(
    const worker_ipc::controller_connection_t &connection,
    const expected_media_t &expected,
    const delivery_sinks_t &sinks,
    const host_requests_t &requests
  );

}  // namespace multiseat::media

#endif
