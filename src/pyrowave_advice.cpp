/**
 * @file src/pyrowave_advice.cpp
 * @brief The bitrate PyroWave needs for a picture, from the model its author published.
 */
#include "pyrowave_advice.h"

// standard includes
#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "stream_bitrate.h"

#ifdef POLARIS_BUILD_PYROWAVE
  // Upstream's generated header, reached through the pyrowave-regression-results target. It is C, and
  // it defines its table and its one function static, so this is the only file that includes it.
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wsign-compare"
  #pragma GCC diagnostic ignored "-Wunused-function"
  #include <pyrowave_regression_results.h>
  #pragma GCC diagnostic pop
#endif

namespace pyrowave_advice {
  namespace {
    // The two edges of the sizes the model was fitted on, both 16:9, so the model answers for each
    // without extrapolating.
    constexpr int k_smallest_width = 1280;
    constexpr int k_smallest_height = 720;
    constexpr int k_largest_width = 3840;
    constexpr int k_largest_height = 2160;
    constexpr std::int64_t k_min_pixels = static_cast<std::int64_t>(k_smallest_width) * k_smallest_height;
    constexpr std::int64_t k_max_pixels = static_cast<std::int64_t>(k_largest_width) * k_largest_height;
    constexpr int k_height_factors = 16;

    // The flat figure Nova advises without a model, measured by eye. Kept only as the last
    // fallback, for a question the model and its edges cannot answer.
    constexpr double k_fallback_bits_per_pixel = 0.73;

    int kbps_of(double bits_per_second) {
      const double kbps = bits_per_second / 1000.0;
      if (!(kbps > 0.0)) {
        return 0;
      }
      return kbps >= static_cast<double>(std::numeric_limits<int>::max()) ?
               std::numeric_limits<int>::max() :
               static_cast<int>(kbps);
    }

    int clamp_to_int(std::int64_t value) {
      return static_cast<int>(std::clamp<std::int64_t>(value, 0, std::numeric_limits<int>::max()));
    }

    /// A decimal integer from min to max, digits only, or nullopt.
    std::optional<int> query_integer(std::string_view text, int min, int max) {
      if (text.empty() || text.size() > 6) {
        return std::nullopt;
      }
      int value = 0;
      for (const char c : text) {
        if (c < '0' || c > '9') {
          return std::nullopt;
        }
        value = value * 10 + (c - '0');
      }
      if (value < min || value > max) {
        return std::nullopt;
      }
      return value;
    }

    /// Upstream's own function, asked only what it answers without asserting. nullopt otherwise.
    std::optional<double> model_mbps(int width, int height, int fps, bool chroma444, int height_factor) {
#ifdef POLARIS_BUILD_PYROWAVE
      const auto pixels = static_cast<std::int64_t>(width) * height;
      if (pixels < k_min_pixels || pixels > k_max_pixels || height_factor < 0 ||
          height_factor >= k_height_factors || fps <= 0) {
        return std::nullopt;
      }
      static_assert(k_target_db >= PYROWAVE_REGRESSION_MIN_PSNR_HVS_M_H &&
                    k_target_db <= PYROWAVE_REGRESSION_MAX_PSNR_HVS_M_H);
      static_assert(k_min_pixels == PYROWAVE_REGRESSION_MIN_PIXELS && k_max_pixels == PYROWAVE_REGRESSION_MAX_PIXELS);
      const double mbps = pyrowave_psnr_hvs_m_h_estimate_mbits(
        k_target_db, width, height, static_cast<enum pyrowave_height_factor>(height_factor),
        chroma444 ? 1 : 0, static_cast<double>(fps));
      if (!(mbps > 0.0) || std::isinf(mbps)) {
        return std::nullopt;
      }
      return mbps;
#else
      (void) width;
      (void) height;
      (void) fps;
      (void) chroma444;
      (void) height_factor;
      return std::nullopt;
#endif
    }
  }  // namespace

  std::string_view rule_name(rule_e rule) {
    switch (rule) {
      case rule_e::model:
        return "model";
      case rule_e::model_not_16_9:
        return "model_not_16_9";
      case rule_e::below_model_edge:
        return "below_model_edge";
      case rule_e::above_model_edge:
        return "above_model_edge";
      case rule_e::flat_fallback:
        return "flat_fallback";
      case rule_e::none:
        break;
    }
    return "none";
  }

  bool model_available() {
#ifdef POLARIS_BUILD_PYROWAVE
    return true;
#else
    return false;
#endif
  }

  estimate_t estimate(int width, int height, int fps, bool chroma444, int height_factor) {
    if (width <= 0 || height <= 0 || fps <= 0) {
      return {};
    }
    const double pixels_per_second = static_cast<double>(width) * static_cast<double>(height) * fps;
    const auto pixels = static_cast<std::int64_t>(width) * height;

    if (const auto mbps = model_mbps(width, height, fps, chroma444, height_factor)) {
      const bool sixteen_nine = static_cast<std::int64_t>(width) * 9 == static_cast<std::int64_t>(height) * 16;
      return {*mbps, kbps_of(*mbps * 1'000'000.0), sixteen_nine ? rule_e::model : rule_e::model_not_16_9};
    }

    // Past an edge, the bits per pixel the model gives at that edge, at the same distance and chroma.
    // The model's bits per pixel were still rising toward 720p and still falling toward 4K, so holding
    // the edge claims no more than was measured.
    if (pixels < k_min_pixels || pixels > k_max_pixels) {
      const bool below = pixels < k_min_pixels;
      const int edge_width = below ? k_smallest_width : k_largest_width;
      const int edge_height = below ? k_smallest_height : k_largest_height;
      if (const auto edge_mbps = model_mbps(edge_width, edge_height, fps, chroma444, height_factor)) {
        const double bits_per_pixel =
          *edge_mbps * 1'000'000.0 / (static_cast<double>(edge_width) * edge_height * fps);
        const double bits_per_second = bits_per_pixel * pixels_per_second;
        return {bits_per_second / 1'000'000.0, kbps_of(bits_per_second),
                below ? rule_e::below_model_edge : rule_e::above_model_edge};
      }
    }

    if (!model_available()) {
      return {};
    }
    const double bits_per_second = k_fallback_bits_per_pixel * pixels_per_second;
    return {bits_per_second / 1'000'000.0, kbps_of(bits_per_second), rule_e::flat_fallback};
  }

  advice_t advise(int width, int height, int fps, bool chroma444, const link_t &link, int max_bitrate_kbps) {
    advice_t advice;
    advice.width = width;
    advice.height = height;
    advice.fps = fps;
    advice.chroma444 = chroma444;

    const auto far = estimate(width, height, fps, chroma444, k_height_factor_far);
    const auto near = estimate(width, height, fps, chroma444, k_height_factor_near);
    if (far.kbps <= 0 || near.kbps <= 0) {
      return advice;
    }
    advice.valid = true;
    advice.rule = far.rule;
    advice.far_encoder_kbps = far.kbps;
    advice.near_encoder_kbps = near.kbps;
    advice.advice_far_kbps = clamp_to_int(
      stream_bitrate::wire_kbps_for_encoder(far.kbps, link.fec_percentage, link.audio_kbps));
    advice.advice_near_kbps = clamp_to_int(
      stream_bitrate::wire_kbps_for_encoder(near.kbps, link.fec_percentage, link.audio_kbps));

    advice.raise_goal_kbps = advice.advice_far_kbps;
    advice.raise_goal_limited_by = "advice";
    if (advice.cap_kbps < advice.raise_goal_kbps) {
      advice.raise_goal_kbps = advice.cap_kbps;
      advice.raise_goal_limited_by = "cap";
    }
    if (max_bitrate_kbps > 0 && max_bitrate_kbps < advice.raise_goal_kbps) {
      advice.raise_goal_kbps = max_bitrate_kbps;
      advice.raise_goal_limited_by = "max_bitrate";
    }
    advice.raise_goal_encoder_kbps = clamp_to_int(
      stream_bitrate::encoder_kbps_for_wire(advice.raise_goal_kbps, link.fec_percentage, link.audio_kbps));
    advice.floor_encoder_kbps = far.kbps / 2;
    return advice;
  }

  nlohmann::json advice_json(const advice_t &advice) {
    return {
      {"version", 1},
      {"model", k_model},
      {"target_db", k_target_db},
      {"width", advice.width},
      {"height", advice.height},
      {"fps", advice.fps},
      {"chroma", advice.chroma444 ? "444" : "420"},
      {"advice_far_kbps", advice.advice_far_kbps},
      {"advice_near_kbps", advice.advice_near_kbps},
      {"raise_goal_kbps", advice.raise_goal_kbps},
      {"cap_kbps", advice.cap_kbps},
      {"rule", rule_name(advice.rule)},
      {"raise_goal_limited_by", advice.raise_goal_limited_by},
    };
  }

  nlohmann::json advice_reply(std::string_view width, std::string_view height, std::string_view fps,
                              std::string_view chroma, const route_host_t &host, int &http_status) {
    const auto parsed_width = query_integer(width, 1, 16384);
    const auto parsed_height = query_integer(height, 1, 16384);
    const auto parsed_fps = query_integer(fps, 1, 1000);
    const bool chroma_known = chroma == "420" || chroma == "444";
    if (!parsed_width || !parsed_height || !parsed_fps || !chroma_known) {
      http_status = 400;
      return {
        {"code", "invalid_query"},
        {"error", "width and height must be 1 to 16384, fps 1 to 1000, and chroma 420 or 444"}
      };
    }
    http_status = 200;
    if (!host.built || !model_available()) {
      return {
        {"version", 1},
        {"available", false},
        {"reason", "not_built"},
        {"message", "This Polaris was built without PyroWave, so it has no PyroWave advice to give."}
      };
    }
    const auto advice = advise(*parsed_width, *parsed_height, *parsed_fps, chroma == "444",
                               {host.fec_percentage, k_default_audio_kbps}, host.max_bitrate_kbps);
    auto reply = advice_json(advice);
    reply["available"] = host.device_available;
    if (!host.device_available) {
      reply["reason"] = "no_vulkan_device";
      reply["message"] = "No GPU on this PC can run PyroWave, so it cannot stream it. The figures are what the "
                         "codec would need.";
    }
    reply["assumes"] = {{"fec_percentage", host.fec_percentage}, {"audio_kbps", k_default_audio_kbps}};
    return reply;
  }

}  // namespace pyrowave_advice
