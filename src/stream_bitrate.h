/**
 * @file src/stream_bitrate.h
 * @brief How a client's requested bitrate becomes the rate the encoder runs at, and back.
 *
 * A client asks for a bitrate for everything it receives: the video, the FEC parity that protects
 * it, the audio, and the packet overhead around all of them. The RTSP handshake takes FEC, audio and
 * overhead off that request and hands the encoder what is left. This is that arithmetic, in one place,
 * so the handshake and anything that has to say what a client should ask for use the same numbers.
 */
#pragma once

// standard includes
#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace stream_bitrate {

  /**
   * @brief What a client asked for at the handshake, and what the host did to it.
   *
   * Recorded once, when the stream starts, so that anything which later says what a stream needs can
   * also say what stood between the client and the rate it asked for.
   */
  struct request_t {
    /// The client's own request, before the host did anything to it: the total it configured, or its
    /// maximum when it configured none. 0 when it sent neither.
    std::int64_t client_kbps = 0;
    /// A cap that cut the request, and the setting or launch decision it came from. 0 when none did.
    int cap_kbps = 0;
    std::string cap_source;
    /// A launch cap the stream's codec did not apply, and where it came from. PyroWave sets them aside.
    int set_aside_kbps = 0;
    std::string set_aside_source;
    /// What the stream's audio costs as the handshake counts it, see audio_kbps(). Recorded for every
    /// stream, whether or not it had a request to take it off.
    int audio_kbps = 0;
    /// The FEC share the handshake split with, `fec_percentage` when the stream started.
    int fec_percentage = 0;
    /// The total the handshake split: client_kbps times warp_factor, cut to cap_kbps when a cap did, so
    /// that encoder_kbps_for_wire(split_kbps, fec_percentage, audio_kbps) is encoder_kbps. 0 when the
    /// encoder rate is no split of this stream's own request: a client that sent no bitrate at all, or
    /// a watcher, which encodes at its owner's rate. The formula says nothing about such a stream.
    std::int64_t split_kbps = 0;
    /// Where the handshake left the encoder, config.monitor.bitrate when the stream started.
    int encoder_kbps = 0;
    /// How many times the handshake multiplied the request because `limit_framerate` renders faster than
    /// the client streams. 1 when it did not. Last, so a positional initializer of the fields above
    /// still means what it did.
    int warp_factor = 1;

    bool operator==(const request_t &) const = default;
  };

  /// The name session status gives this arithmetic in bitrate_units.formula: a request becomes an
  /// encoder rate by encoder_kbps_for_wire(), and an encoder rate a request by wire_kbps_for_encoder().
  inline constexpr std::string_view k_formula = "stream_bitrate_v1";

  /// The FEC share above which the handshake stops taking FEC off a request.
  inline constexpr int k_max_adjusted_fec_percentage = 80;

  /// What a stream's audio costs, as the handshake counts it: 256 kbps a channel in high quality, 96 otherwise.
  inline int audio_kbps(bool high_quality, int channels) {
    return (high_quality ? 256 : 96) * channels;
  }

  /**
   * @brief The encoder rate a client's request becomes.
   *
   * FEC comes off first, as a share of the request. Then the audio, but never more than a fifth of what
   * is left. Then 500 kbps for packet overhead and control traffic, but never more than a tenth. The
   * FEC step divides in single precision, as the handshake always has, so the two agree to the kbps.
   * @param wire_kbps What the client asked for.
   * @param fec_percentage The host's FEC share, `fec_percentage`.
   * @param audio_kbps What the stream's audio costs, see audio_kbps().
   */
  inline std::int64_t encoder_kbps_for_wire(std::int64_t wire_kbps, int fec_percentage, int audio_kbps) {
    std::int64_t kbps = wire_kbps;
    if (fec_percentage <= k_max_adjusted_fec_percentage) {
      kbps /= 100.f / (100 - fec_percentage);
    }
    kbps -= std::min(static_cast<std::int64_t>(audio_kbps), kbps / 5);
    kbps -= std::min(static_cast<std::int64_t>(500), kbps / 10);
    return kbps;
  }

  /**
   * @brief Split a client's request the way the handshake does, and record the split on request.
   *
   * The audio cost and the FEC share are recorded for every stream, so a stream can always say what a
   * request is split for. The total and the encoder rate it becomes are recorded only when there is a
   * total: a zero one splits nothing, as the handshake has always had it.
   * @param request Where the split is recorded.
   * @param total_kbps What the client asked for, after any host cap.
   * @param fec_percentage The host's FEC share, `fec_percentage`.
   * @param audio_kbps What the stream's audio costs, see audio_kbps().
   * @return The encoder rate the total becomes, or nullopt when there was nothing to split.
   */
  inline std::optional<std::int64_t> split_request(request_t &request, std::int64_t total_kbps, int fec_percentage, int audio_kbps) {
    request.audio_kbps = audio_kbps;
    request.fec_percentage = fec_percentage;
    if (total_kbps == 0) {
      return std::nullopt;
    }
    const auto encoder_kbps = encoder_kbps_for_wire(total_kbps, fec_percentage, audio_kbps);
    request.split_kbps = total_kbps;
    request.encoder_kbps = static_cast<int>(encoder_kbps);
    return encoder_kbps;
  }

  /**
   * @brief Record where the handshake leaves the encoder, and return that rate.
   *
   * The handshake's last word on the encoder rate. A watcher encodes at its owner's rate, which is no
   * split of its own request, so its split is cleared and the formula says nothing about it. Every
   * other stream keeps its split and starts where the split left it.
   * @param request Where the split was recorded.
   * @param negotiated_kbps config.monitor.bitrate after the split.
   * @param owner_kbps A watcher's owner's rate, and nullopt for every other stream.
   */
  inline int settle_encoder(request_t &request, int negotiated_kbps, std::optional<int> owner_kbps) {
    if (owner_kbps) {
      request.split_kbps = 0;
      request.encoder_kbps = *owner_kbps;
    } else {
      request.encoder_kbps = negotiated_kbps;
    }
    return request.encoder_kbps;
  }

  /**
   * @brief The smallest request that lands the encoder on at least encoder_kbps.
   *
   * The inverse of encoder_kbps_for_wire(), found by search rather than by algebra, so that it inverts
   * the rounding the handshake does and not an idealised version of it. A client that asks for this
   * much gets an encoder at or above encoder_kbps, and one that asks for a kbps less does not.
   * @return 0 when encoder_kbps is not positive.
   */
  inline std::int64_t wire_kbps_for_encoder(std::int64_t encoder_kbps, int fec_percentage, int audio_kbps) {
    if (encoder_kbps <= 0) {
      return 0;
    }
    // Every step only takes something away, so the answer is never below the encoder rate itself.
    std::int64_t low = encoder_kbps - 1;
    std::int64_t high = encoder_kbps;
    while (encoder_kbps_for_wire(high, fec_percentage, audio_kbps) < encoder_kbps) {
      low = high;
      high *= 2;
    }
    // encoder_kbps_for_wire(low) < encoder_kbps <= encoder_kbps_for_wire(high), and the function never
    // falls as its input grows.
    while (high - low > 1) {
      const std::int64_t middle = low + (high - low) / 2;
      if (encoder_kbps_for_wire(middle, fec_percentage, audio_kbps) >= encoder_kbps) {
        high = middle;
      } else {
        low = middle;
      }
    }
    return high;
  }

}  // namespace stream_bitrate
