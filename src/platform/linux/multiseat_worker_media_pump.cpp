/**
 * @file src/platform/linux/multiseat_worker_media_pump.cpp
 * @brief Implementation of the worker media pump.
 */
#ifdef __linux__

  #include "multiseat_worker_media_pump.h"

  #include <atomic>
  #include <chrono>
  #include <limits>
  #include <string>
  #include <span>
  #include <thread>

namespace multiseat::media {

  using namespace std::literals;
  using worker_ipc::message_e;
  using worker_ipc::transport_status_e;

  namespace {
    /** How long the request side waits before looking at the host again. */
    constexpr auto request_tick = 20ms;

    /**
     * Moonlight's video format index for H.264. The contract can describe only
     * H.264 today, so any other negotiated format has no worker to serve it.
     */
    constexpr int h264_format_index = 0;

    bool serves(const worker_ipc::media_config_t &contract, const expected_media_t &expected) {
      if (expected.video_format != h264_format_index) {
        return false;
      }
      if (contract.width != expected.width || contract.height != expected.height) {
        return false;
      }
      // The contract carries an exact rate; the client asked for a whole one.
      if (expected.fps == 0 || contract.fps_denominator == 0 ||
          contract.fps_numerator != expected.fps * contract.fps_denominator) {
        return false;
      }
      return contract.audio_channels == expected.audio_channels;
    }

    /**
     * Sends the host's asks on control while the caller reads media, and closes
     * the transport once the session ends so the reader cannot outlive it.
     */
    class request_side_t {
    public:
      request_side_t(
        const worker_ipc::controller_connection_t &connection,
        const host_requests_t &requests
      ):
          connection_(connection),
          requests_(requests),
          thread_([this] { run(); }) {
      }

      ~request_side_t() {
        join();
      }

      request_side_t(const request_side_t &) = delete;
      request_side_t &operator=(const request_side_t &) = delete;

      /** Join, then fold what was sent into the report. One writer from here on. */
      void finish(pump_report_t &report) {
        join();
        report.idr_requests += idr_requests_;
        report.invalidations += invalidations_;
      }

      /** True once the host asked the stream to end. */
      [[nodiscard]] bool stopped() const {
        return stopped_.load();
      }

    private:
      void join() {
        finished_.store(true);
        if (thread_.joinable()) {
          thread_.join();
        }
      }

      void run() {
        while (!finished_.load()) {
          if (requests_.stop_requested && requests_.stop_requested()) {
            stopped_.store(true);
            // The reader is parked on the media channel with no deadline of its
            // own until a frame or a fault arrives. Closing this lease is what
            // returns it, so a stopping session never waits for the worker.
            connection_.close();
            return;
          }
          if (requests_.take_invalidation) {
            if (const auto span = requests_.take_invalidation()) {
              if (span->first >= 0 && span->second >= span->first &&
                  connection_.invalidate_ref_frames({
                    .first = static_cast<std::uint64_t>(span->first),
                    .last = static_cast<std::uint64_t>(span->second),
                  }) == transport_status_e::applied) {
                ++invalidations_;
              }
            }
          }
          if (requests_.take_idr_request && requests_.take_idr_request()) {
            if (connection_.request_idr() == transport_status_e::applied) {
              ++idr_requests_;
            }
          }
          std::this_thread::sleep_for(request_tick);
        }
      }

      const worker_ipc::controller_connection_t &connection_;
      const host_requests_t &requests_;
      std::uint64_t idr_requests_ = 0;
      std::uint64_t invalidations_ = 0;
      std::atomic<bool> finished_ {false};
      std::atomic<bool> stopped_ {false};
      std::thread thread_;
    };

    /** Split one delivered payload, rejecting anything the contract forbids. */
    bool accept_frame(
      const worker_ipc::encoded_media_packet_t &packet,
      worker_ipc::media_frame_t &frame,
      std::vector<std::uint8_t> &bytes
    ) {
      std::span<const std::uint8_t> encoded;
      const auto parsed = worker_ipc::parse_media_frame(packet.payload, encoded);
      if (!parsed) {
        return false;
      }
      if (parsed->frame_index > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return false;
      }
      frame = *parsed;
      bytes.assign(encoded.begin(), encoded.end());
      return true;
    }
  }  // namespace

  std::string_view describe(const pump_status_e status) {
    switch (status) {
      case pump_status_e::ended_on_shutdown:
        return "the session ended the stream";
      case pump_status_e::ended_on_end_of_stream:
        return "the worker ended its media";
      case pump_status_e::no_connection:
        return "no worker connection was bound to this stream";
      case pump_status_e::attach_refused:
        return "the worker refused the data plane attachment";
      case pump_status_e::announced_nothing:
        return "the worker never announced a media contract";
      case pump_status_e::frame_before_contract:
        return "the worker sent media before announcing its contract";
      case pump_status_e::unrepresentable_contract:
        return "the worker announced a contract this host cannot represent";
      case pump_status_e::mismatched_contract:
        return "the worker announced a contract the client did not negotiate";
      case pump_status_e::acknowledgement_refused:
        return "the worker refused the contract acknowledgement";
      case pump_status_e::malformed_frame:
        return "the worker sent a frame its contract does not allow";
      case pump_status_e::transport_lost:
        return "the worker transport ended";
    }
    return "unknown";
  }

  bool ended_cleanly(const pump_status_e status) {
    return status == pump_status_e::ended_on_shutdown ||
           status == pump_status_e::ended_on_end_of_stream;
  }

  pump_report_t run(
    const worker_ipc::controller_connection_t &connection,
    const expected_media_t &expected,
    const delivery_sinks_t &sinks,
    const host_requests_t &requests
  ) {
    pump_report_t report;
    if (!connection.connected() || !sinks.video || !sinks.audio) {
      report.status = pump_status_e::no_connection;
      return report;
    }
    if (!connection.data_plane_attached() &&
        connection.attach_data_plane() != transport_status_e::applied) {
      report.status = pump_status_e::attach_refused;
      return report;
    }

    request_side_t request_side {connection, requests};
    const auto finish = [&](const pump_status_e status) {
      report.status = status;
      request_side.finish(report);
      return report;
    };

    worker_ipc::encoded_media_packet_t packet;
    if (connection.receive_media(packet) != transport_status_e::applied) {
      return finish(request_side.stopped() ? pump_status_e::ended_on_shutdown :
                                             pump_status_e::announced_nothing);
    }
    if (packet.message != message_e::media_config) {
      report.detail = "first media message was " + std::to_string(static_cast<int>(packet.message));
      return finish(pump_status_e::frame_before_contract);
    }
    const auto contract = worker_ipc::parse_media_config(packet.payload);
    if (!contract) {
      return finish(pump_status_e::unrepresentable_contract);
    }
    report.contract = *contract;
    if (!serves(*contract, expected)) {
      report.detail = std::to_string(contract->width) + "x" + std::to_string(contract->height) +
                      " at " + std::to_string(contract->fps_numerator) + "/" +
                      std::to_string(contract->fps_denominator) + " with " +
                      std::to_string(contract->audio_channels) + " audio channels";
      return finish(pump_status_e::mismatched_contract);
    }
    if (connection.acknowledge_media_config() != transport_status_e::applied) {
      return finish(pump_status_e::acknowledgement_refused);
    }

    std::vector<std::uint8_t> bytes;
    while (true) {
      const auto received = connection.receive_media(packet);
      if (received != transport_status_e::applied) {
        return finish(request_side.stopped() ? pump_status_e::ended_on_shutdown :
                                               pump_status_e::transport_lost);
      }
      switch (packet.message) {
        case message_e::video:
        case message_e::audio: {
          worker_ipc::media_frame_t frame;
          if (!accept_frame(packet, frame, bytes)) {
            return finish(pump_status_e::malformed_frame);
          }
          report.last_frame_index = frame.frame_index;
          if (packet.message == message_e::video) {
            ++report.video_frames;
            sinks.video(std::move(bytes), static_cast<std::int64_t>(frame.frame_index), frame.idr);
          } else {
            ++report.audio_frames;
            sinks.audio(std::move(bytes));
          }
          bytes.clear();
          break;
        }
        case message_e::discontinuity:
          // The worker lost frames of its own. Nothing downstream can repair a
          // gap in a reference chain, so the next frame has to stand alone.
          ++report.discontinuities;
          if (connection.request_idr() == transport_status_e::applied) {
            ++report.idr_requests;
          }
          break;
        case message_e::end_of_stream:
          return finish(pump_status_e::ended_on_end_of_stream);
        case message_e::media_config:
          // The contract is announced once. A second one would mean the worker
          // changed what it produces underneath a client already decoding it.
          report.detail = "the worker announced a second contract";
          return finish(pump_status_e::malformed_frame);
        default:
          report.detail = "unexpected media message " + std::to_string(static_cast<int>(packet.message));
          return finish(pump_status_e::malformed_frame);
      }
    }
  }

}  // namespace multiseat::media

#endif
