/**
 * @file src/codec_support_readout.h
 * @brief What the console's codec readout says about 4:4:4 and PyroWave, built from facts the caller
 *        gathers.
 *
 * The console's codec panel read the advertised H.264, HEVC and AV1 support and nothing else, so a
 * host could not see which encoder gives a client 4:4:4, or whether it can serve PyroWave and why
 * not. Both answers exist on the host already: the probe records 4:4:4 per codec, the encoder tables
 * say which encoders can carry it at all, and capabilities says why PyroWave is left out. These turn
 * them into the two objects GET /api/config serves inside encoder_codec_support, and take plain
 * values so the shape can be tested without a GPU.
 */
#pragma once

// standard includes
#include <array>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "src/launch_failure.h"
#include "src/pyrowave_availability.h"

namespace codec_support_readout {

  /// An encoder in this build by name, and whether its table lets H.264, HEVC and AV1 carry 4:4:4.
  using encoder_yuv444_t = std::pair<std::string_view, std::array<bool, 3>>;

  namespace detail {
    inline nlohmann::json codec_names(const std::array<bool, 3> &codecs) {
      static constexpr std::array<std::string_view, 3> names {"h264", "hevc", "av1"};
      auto list = nlohmann::json::array();
      for (std::size_t index = 0; index < names.size(); ++index) {
        if (codecs[index]) {
          list.emplace_back(std::string {names[index]});
        }
      }
      return list;
    }
  }  // namespace detail

  /**
   * @brief encoder_codec_support.yuv444: what the probed encoder gives clients in 4:4:4, and which
   *        encoders in this build could.
   *
   * advertised follows the rule serverinfo uses for the 4:4:4 bits in ServerCodecModeSupport, so the
   * console never names a 4:4:4 codec a client is not offered: H.264 whenever the probe passed it in
   * 4:4:4, HEVC and AV1 only while the codec itself is advertised too.
   *
   * @param hevc_mode The advertised HEVC mode: 2 or 3 when HEVC is offered.
   * @param av1_mode The advertised AV1 mode, the same way.
   * @param yuv444_for_codec What the probe found for 4:4:4, for H.264, HEVC and AV1.
   * @param encoders Every encoder the probe could choose in this build, in its search order.
   */
  inline nlohmann::json yuv444_json(int hevc_mode, int av1_mode, const std::array<bool, 3> &yuv444_for_codec,
                                    const std::vector<encoder_yuv444_t> &encoders) {
    const std::array<bool, 3> advertised {
      yuv444_for_codec[0],
      hevc_mode >= 2 && yuv444_for_codec[1],
      av1_mode >= 2 && yuv444_for_codec[2],
    };
    auto listed = nlohmann::json::array();
    for (const auto &[name, codecs] : encoders) {
      listed.push_back({{"encoder", std::string {name}}, {"codecs", detail::codec_names(codecs)}});
    }
    return {
      {"h264", advertised[0]},
      {"hevc", advertised[1]},
      {"av1", advertised[2]},
      {"encoders", std::move(listed)},
    };
  }

  /**
   * @brief encoder_codec_support.pyrowave: whether this host offers PyroWave, and why not when it
   *        does not.
   *
   * available is exactly what capabilities says, because it is the same answer: reason and message
   * are capture.pyrowave_unavailable, word for word, and only present when PyroWave is not offered.
   *
   * Capabilities offers PyroWave when any launch a client can make would stream it, so a host whose
   * own stream mode captures a scanout PyroWave cannot read still offers it for a stream mode with its
   * own compositor. host_mode_refusal is the refusal a launch into the host's own mode gets then, so
   * the console does not call the codec available without saying which launches it is available to.
   *
   * @param unavailable Why capabilities leaves PyroWave out, or nothing when it offers it.
   * @param hdr Whether the host can carry PyroWave in HDR10, which needs the GPU input path.
   * @param host_mode_refusal The refusal for a PyroWave launch into the host's own stream mode.
   */
  inline nlohmann::json pyrowave_json(const std::optional<pyrowave_availability::unavailable_t> &unavailable,
                                      bool hdr,
                                      const std::optional<launch_failure::record_t> &host_mode_refusal) {
    nlohmann::json value {
      {"available", !unavailable.has_value()},
      {"hdr", !unavailable.has_value() && hdr},
      {"reason", nullptr},
      {"message", nullptr},
      {"host_mode_refusal", nullptr},
    };
    if (unavailable) {
      value["reason"] = std::string {pyrowave_availability::reason_id(unavailable->reason)};
      value["message"] = unavailable->message;
    } else if (host_mode_refusal) {
      value["host_mode_refusal"] = {
        {"code", host_mode_refusal->code},
        {"message", host_mode_refusal->message},
        {"action", host_mode_refusal->action},
      };
    }
    return value;
  }

}  // namespace codec_support_readout
