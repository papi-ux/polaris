/**
 * @file src/pyrowave_advice.h
 * @brief The bitrate PyroWave needs for a picture, from the model its author published.
 *
 * PyroWave's author ran four lossless game clips through the codec at every 16:9 size from 1280x720
 * to 3840x2160, scored each result with PSNR-HVS-M-H (PSNR-HVS-M weighted for how far away the picture
 * is watched), and fitted one polynomial per quality, viewing distance and chroma to the bitrate each
 * needed. The fit ships as eval-results/pyrowave_regression_results.h in the PyroWave tree Polaris
 * builds, and this evaluates it at 35 dB, the level the author calls the default good quality.
 *
 * Nova's matching estimator, a port of the same header, is in review, so that the host and the client
 * can quote the same number for the same picture. A picture outside the sizes the model was fitted on
 * gets the bits per pixel of the nearest edge, and tests/fixtures/pyrowave-rate-model.json pins the
 * host's figures.
 *
 * The model's limits travel with every number it gives: an objective metric on four clips of about ten
 * frames each, luma only, sampled at 16:9, measured on SDR. It is not a measurement on any device.
 * On a Retroid Pocket 6, 200 Mbps was right for Control at 1080p120 where the model asks about 359,
 * which is why Doctor raises no further than the far figure and never past k_cap_kbps.
 */
#pragma once

// standard includes
#include <cstdint>
#include <string_view>

// lib includes
#include <nlohmann/json_fwd.hpp>

namespace pyrowave_advice {

  /// The quality the advice aims for, in dB of PSNR-HVS-M-H.
  inline constexpr int k_target_db = 35;

  /// The model's name in the contract clients read.
  inline constexpr std::string_view k_model = "psnr-hvs-m";

  /// The PyroWave revision whose regression header this evaluates, and which the fixture is keyed by.
  inline constexpr std::string_view k_model_revision = "186f0393b77f7755953b5ecde994bb1cec2e4155";

  /// The most Doctor raises a PyroWave stream to, on the wire. Also the most any paired endpoint takes.
  inline constexpr int k_cap_kbps = 300000;

  /// H 2.0, a television, a monitor or an external display. The model's higher, "near" figure.
  inline constexpr int k_height_factor_near = 8;

  /// H 2.875, a device's own screen. The farthest distance the model reaches, and its lower figure.
  inline constexpr int k_height_factor_far = 15;

  /// More than this share of recent frames at the byte ceiling is a starved stream.
  inline constexpr double k_starved_ceiling_share = 0.8;

  /// Stereo in high quality, which is what the pre-launch advice assumes the stream's audio costs.
  inline constexpr int k_default_audio_kbps = 512;

  /// Which rule produced a figure. rule_name() gives the name the contract carries.
  enum class rule_e {
    none,  ///< A size or frame rate that is not positive, which describes no stream.
    model,  ///< The model's own estimate, 16:9 and inside the sizes it was fitted on.
    model_not_16_9,  ///< The model's estimate for a shape it never saw, keyed on the pixel count.
    below_model_edge,  ///< Fewer pixels than 1280x720: that edge's bits per pixel, times this picture's.
    above_model_edge,  ///< More pixels than 3840x2160: that edge's bits per pixel, times this picture's.
    flat_fallback,  ///< Nothing the model or its edges could answer: 0.73 bits per pixel.
  };

  /// The rule as the contract spells it: model, model_not_16_9, below_model_edge and so on.
  std::string_view rule_name(rule_e rule);

  /// Whether this build carries the model at all. False when built without PyroWave.
  bool model_available();

  /// One estimate of what the encoder needs.
  struct estimate_t {
    double mbps = 0.0;  ///< The model's figure, unrounded, in Mbps.
    int kbps = 0;  ///< The same figure truncated, not rounded, to a whole kbps.
    rule_e rule = rule_e::none;
  };

  /**
   * @brief What the encoder needs for a picture at 35 dB, watched from height_factor.
   *
   * Upstream asserts on a picture outside 1280x720 to 3840x2160 pixels, so such a picture is never
   * handed to it: it gets the edge's bits per pixel instead. A height factor outside 0 to 15 gets the
   * flat fallback, and a size or frame rate that is not positive gets nothing.
   * @param height_factor An index into upstream's distances, H = 1 + index / 8.
   */
  estimate_t estimate(int width, int height, int fps, bool chroma444, int height_factor);

  /// What the stream around the video costs, which is what turns an encoder rate into a request.
  struct link_t {
    int fec_percentage = 10;
    int audio_kbps = k_default_audio_kbps;
  };

  /// The advice for one stream shape.
  struct advice_t {
    bool valid = false;
    int width = 0;
    int height = 0;
    int fps = 0;
    bool chroma444 = false;
    rule_e rule = rule_e::none;
    /// What the encoder needs on a device's own screen (H 2.875) and on a television (H 2.0).
    int far_encoder_kbps = 0;
    int near_encoder_kbps = 0;
    /// The same two as requests, grossed up for FEC, audio and overhead. What a client should ask for.
    int advice_far_kbps = 0;
    int advice_near_kbps = 0;
    /// What Doctor would raise the stream to, as a request: the far figure, k_cap_kbps, or max_bitrate.
    int raise_goal_kbps = 0;
    /// Where a request of raise_goal_kbps lands the encoder.
    int raise_goal_encoder_kbps = 0;
    /// Which of the three set raise_goal_kbps: advice, cap or max_bitrate.
    std::string_view raise_goal_limited_by;
    int cap_kbps = k_cap_kbps;
    /// Live Tuning's floor for this shape: half the far figure, at the encoder.
    int floor_encoder_kbps = 0;
  };

  /**
   * @brief The advice for a stream shape.
   * @param max_bitrate_kbps The host's `max_bitrate`, or 0 when unset. It caps the raise goal.
   */
  advice_t advise(int width, int height, int fps, bool chroma444, const link_t &link, int max_bitrate_kbps);

  /**
   * @brief The advice fields of the contract, without anything live.
   *
   * version, model, target_db, width, height, fps, chroma, advice_far_kbps, advice_near_kbps,
   * raise_goal_kbps and cap_kbps, plus rule and raise_goal_limited_by. Every kbps figure is a request.
   */
  nlohmann::json advice_json(const advice_t &advice);

  /// What the host can say about PyroWave when a client asks for advice before a launch.
  struct route_host_t {
    /// This build carries the model and the codec.
    bool built = false;
    /// A device on this host can run the codec.
    bool device_available = false;
    int fec_percentage = 10;
    int max_bitrate_kbps = 0;
  };

  /**
   * @brief The reply to GET /polaris/v1/pyrowave/advice.
   *
   * The same advice fields session status carries while a PyroWave stream runs, for a shape a client
   * is about to ask for, with the audio assumed to be stereo in high quality. A host that cannot serve
   * PyroWave says so, with a reason code and a sentence a player can read; one that has the model but
   * no device still gives the figures, because they do not depend on the device.
   * @param width,height,fps Decimal integers, 1 to 16384 and 1 to 1000.
   * @param chroma 420 or 444.
   * @param http_status Set to 200, or to 400 when a field is missing or malformed.
   */
  nlohmann::json advice_reply(std::string_view width, std::string_view height, std::string_view fps,
                              std::string_view chroma, const route_host_t &host, int &http_status);

}  // namespace pyrowave_advice
