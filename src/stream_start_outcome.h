/**
 * @file src/stream_start_outcome.h
 * @brief How a stream's start ended, and the words the log and Doctor use for it.
 *
 * A client that negotiates a stream and then cannot build its decoder connects the control stream,
 * leaves a moment later, and never sends the video or audio ping the host waits for. The host used to
 * wait out its ping timeout and log "Initial Ping Timeout" once for each socket, which reads as a
 * network fault, and the support report then said only that no stream was active. The pieces here
 * tell that start apart from a real ping timeout, where the client stays and its UDP never arrives.
 */
#pragma once

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>
#include <string_view>

namespace stream_start {
  /// A ping arrived on the video or the audio socket, so the client got past its own setup.
  inline constexpr std::string_view k_started = "started";
  /// The client connected its control stream and left before any video or audio ping arrived.
  inline constexpr std::string_view k_client_left_during_setup = "client_left_during_setup";
  /// The wait for a ping ran out any other way: the client stayed, or never connected.
  inline constexpr std::string_view k_no_ping = "no_ping";

  /// What the control stream did while the host waited for a session's first ping.
  struct control_timeline_t {
    bool connected = false;
    bool disconnected = false;
    /// From the control stream connecting to it disconnecting. Zero unless both happened.
    std::chrono::milliseconds connected_for {0};
    /// A video or an audio ping arrived before the wait ended.
    bool any_ping = false;
  };

  /**
   * @brief The longest a client can stay connected and still read as leaving during its own setup.
   *
   * A client that cannot create its decoder gives up at once: the Android TV client that could not
   * run PyroWave left 113 ms after connecting. A client whose pings never reach the host gives up
   * too, but only after waiting for video, and moonlight-common-c waits ten seconds. The host waits
   * ping_timeout, ten seconds by default and longer when someone raises it, so it can see that client
   * leave first. A client that stayed several seconds was waiting on the UDP path, not its decoder.
   */
  inline constexpr std::chrono::milliseconds k_setup_leave_limit {5000};

  /**
   * @brief Classify a wait for a first ping that ran out.
   *
   * A client that connected and left soon after, before any ping, stopped during its own setup,
   * usually because it could not create a decoder for what it negotiated. Everything else is a real
   * ping timeout, a client that left only after waiting for video that never came included.
   */
  inline std::string_view classify_ping_timeout(const control_timeline_t &timeline) {
    return timeline.connected && timeline.disconnected && !timeline.any_ping &&
               timeline.connected_for < k_setup_leave_limit ?
             k_client_left_during_setup :
             k_no_ping;
  }

  /// The codec a stream negotiated, as a person names it. Empty for an id this build does not know.
  inline std::string_view codec_label(std::string_view codec) {
    if (codec == "h264") return "H.264";
    if (codec == "hevc") return "HEVC";
    if (codec == "av1") return "AV1";
    if (codec == "pyrowave") return "PyroWave";
    return {};
  }

  /// A frame rate without trailing zeros: 60, 59.94, 119.88.
  inline std::string format_fps(double fps) {
    if (!std::isfinite(fps) || fps <= 0.0) {
      return "unknown";
    }
    char buffer[32];
    std::snprintf(buffer, sizeof(buffer), "%.2f", fps);
    std::string out {buffer};
    while (!out.empty() && out.back() == '0') {
      out.pop_back();
    }
    if (!out.empty() && out.back() == '.') {
      out.pop_back();
    }
    return out;
  }

  /// What a session negotiated, as the ANNOUNCE the client sent set it.
  struct negotiated_stream_t {
    std::string_view client;
    std::string_view codec;  ///< h264, hevc, av1 or pyrowave
    int dynamic_range = 0;  ///< 0 asks for 8-bit, 1 for 10-bit
    int chroma_sampling = 0;  ///< 0 is 4:2:0, 1 is 4:4:4
    int width = 0;
    int height = 0;
    double fps = 0.0;
  };

  /// The one line a session logs when it starts, so every later line can be read against it.
  inline std::string describe_negotiated_stream(const negotiated_stream_t &stream) {
    const auto label = codec_label(stream.codec);
    return "Stream negotiated for [" + std::string {stream.client} + "]: " +
           (label.empty() ? std::string {stream.codec} : std::string {label}) + ", " +
           (stream.dynamic_range > 0 ? "10-bit" : "8-bit") + ", " +
           (stream.chroma_sampling == 1 ? "4:4:4" : "4:2:0") + ", " +
           std::to_string(stream.width) + "x" + std::to_string(stream.height) + " at " +
           format_fps(stream.fps) + " fps";
  }

  /// The one line that replaces a ping timeout for each socket when the client left during setup.
  inline std::string client_left_during_setup_message(std::string_view client, std::string_view codec,
                                                      std::chrono::milliseconds connected_for) {
    const auto label = codec_label(codec);
    return "Stream failed to start for [" + std::string {client} + "]: the client left during video setup " +
           std::to_string(connected_for.count()) + " ms after connecting, before any video or audio arrived (codec " +
           (label.empty() ? std::string {codec} : std::string {label}) + ")";
  }

  /**
   * @brief The error a socket logs when its first ping never arrived and the client did not leave during setup.
   * @param socket "video" or "audio".
   * @param port The UDP port the ping was expected on.
   */
  inline std::string ping_timeout_message(std::string_view socket, std::uint16_t port,
                                          std::chrono::milliseconds timeout, const control_timeline_t &timeline) {
    std::string out = "Initial Ping Timeout: no " + std::string {socket} + " ping arrived on UDP " +
                      std::to_string(port) + " within " + std::to_string(timeout.count()) + " ms";
    if (!timeline.connected) {
      out += ", and the client never connected its control stream";
    } else if (!timeline.disconnected) {
      out += " while the client's control stream stayed connected";
    } else {
      out += ", and the client disconnected " + std::to_string(timeline.connected_for.count()) + " ms after connecting";
      if (timeline.any_ping) {
        // The other socket heard from it, so the client got past its setup before it left.
        return out;
      }
      // No ping at all, and it stayed too long for its own setup to have failed: it left after waiting
      // for video that never came, which is how pings the host never received look from the client.
    }
    return out + ". A firewall or a UDP path problem between the client and this host usually does that";
  }

  /// What to do about a client that left during video setup, for the codec it negotiated.
  inline std::string failed_start_next_step(std::string_view codec) {
    if (codec == "pyrowave") {
      return "The client could not start its PyroWave decoder. Choose HEVC or H.264 for that device, or update the client.";
    }
    return "The client stopped during video setup; its own error message names the cause.";
  }
}  // namespace stream_start
