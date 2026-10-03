/**
 * @file src/pyrowave_advice.h
 * @brief The bitrate PyroWave needs for a picture, from the model its author published.
 *
 * PyroWave's author ran four lossless game clips through the codec at every 16:9 size from 1280x720
 * to 3840x2160, scored each result with PSNR-HVS-M-H (PSNR-HVS-M weighted for how far away the picture
 * is watched), and fitted one polynomial per quality, viewing distance and chroma to the bitrate each
 * needed. The fit ships as eval-results/pyrowave_regression_results.h in the PyroWave tree Polaris
 * builds, and this evaluates it at two qualities. A television or monitor gets 35 dB, the level the
 * author calls the default good quality. A device's own screen gets 31 dB, calibrated by eye: on a
 * Retroid Pocket 6, 200 Mbps was right for Control at 1920x1080, 120 fps and 4:4:4, where the model
 * asks about 359 Mbps at the encoder at 35 dB, about 400 as a request, and about 192 at 31 dB, about
 * 215 as a request. The television figure keeps 35 dB until it is checked on a big screen.
 *
 * Nova's estimator is a port of the same header, so the host and the client quote the same number for
 * the same picture only while both evaluate it at the same two qualities. Nova's merged estimator
 * (nova#107) still reads the far figure at 35 dB. It mirrors k_far_target_db from nova#130, and until
 * that lands Nova's own estimate for a device's own screen is higher than the host's. A picture outside
 * the sizes the model was fitted on gets the bits per pixel of the nearest edge, and
 * tests/fixtures/pyrowave-rate-model.json pins the host's figures at both qualities.
 *
 * The model's limits travel with every number it gives: an objective metric on four clips of about ten
 * frames each, luma only, sampled at 16:9, measured on SDR. The 31 dB target is one check by eye on
 * one device, and Doctor raises no further than the far figure and never past k_cap_kbps.
 */
#pragma once

// standard includes
#include <cstdint>
#include <optional>
#include <string_view>

// lib includes
#include <nlohmann/json_fwd.hpp>

// local includes
#include "stream_bitrate.h"

namespace pyrowave_advice {

  /// The quality the near figure aims for, in dB of PSNR-HVS-M-H: 35, the author's default good
  /// quality, for a television, a monitor or an external display. Not yet checked on one.
  inline constexpr int k_target_db = 35;

  /// The handheld target: the quality the far figure aims for on a device's own screen, in dB of
  /// PSNR-HVS-M-H. 31, calibrated to the Retroid Pocket 6 check above rather than the author's default.
  /// Nova's estimator mirrors it from nova#130, and from then on it changes on both ends together,
  /// with the fixture.
  inline constexpr int k_far_target_db = 31;

  /// The model's name in the contract clients read.
  inline constexpr std::string_view k_model = "psnr-hvs-m";

  /// The PyroWave revision whose regression header this evaluates, and which the fixture is keyed by.
  inline constexpr std::string_view k_model_revision = "186f0393b77f7755953b5ecde994bb1cec2e4155";

  /// The most Doctor raises a PyroWave stream to, as a request, and the most any figure the host
  /// recommends on its own. A player may set more by hand, up to stream_bitrate::k_max_request_kbps.
  inline constexpr int k_cap_kbps = 300000;

  /// H 2.0, a television, a monitor or an external display. The model's higher, "near" figure.
  inline constexpr int k_height_factor_near = 8;

  /// H 2.875, a device's own screen. The farthest distance the model reaches, and its lower figure.
  inline constexpr int k_height_factor_far = 15;

  /// A stream is starved below this share of Doctor's raise goal, both as requests. The far figure is
  /// one judgment by eye, so a stream within a tenth of it is healthy: 200 Mbps on the Retroid Pocket 6
  /// at 1920x1080, 120 fps and 4:4:4 is 93% of the 215 Mbps the far figure asks there.
  inline constexpr double k_starved_below_share = 0.9;

  /// A stream the cap or max_bitrate holds below the far figure wants more than Doctor raises it to once
  /// more than this share of its recent frames fill PyroWave's byte budget. The share decides nothing
  /// else: where nothing holds the goal below the far figure, a full budget says only that the codec
  /// would use more bits, and that figure is calibrated to where the Retroid Pocket 6 looked right.
  inline constexpr double k_ceiling_bound_share = 0.8;

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
   * @brief What the encoder needs for a picture at target_db, watched from height_factor.
   *
   * Upstream asserts on a picture outside 1280x720 to 3840x2160 pixels, so such a picture is never
   * handed to it: it gets the edge's bits per pixel instead. A height factor outside 0 to 15, or a
   * target outside the model's 30 to 50 dB, gets the flat fallback, and a size or frame rate that is
   * not positive gets nothing.
   * @param height_factor An index into upstream's distances, H = 1 + index / 8.
   * @param target_db The quality to reach, in dB of PSNR-HVS-M-H: k_far_target_db or k_target_db. It has
   * no default, so a caller names the target of the figure it evaluates, and a far figure cannot fall
   * back to the television's 35 dB unseen.
   */
  estimate_t estimate(int width, int height, int fps, bool chroma444, int height_factor, int target_db);

  /// What the stream around the video costs, which is what turns an encoder rate into a request.
  struct link_t {
    int fec_percentage = 10;
    int audio_kbps = k_default_audio_kbps;
  };

  /**
   * @brief What a stream's requests are grossed up for.
   *
   * A stream's own figures once its handshake is recorded: the FEC share it started with, which a
   * config reload since does not change, and its own audio. Without one, the host's FEC share now and
   * stereo in high quality, which is what the advice assumes before a launch. Session status, the
   * advice route and the Live Tuning floor all take their link from here, so they agree with each
   * other and with bitrate_units.
   * @param request The stream's recorded request, or nullptr when there is none.
   * @param host_fec_percentage The host's `fec_percentage` now.
   */
  inline link_t stream_link(const stream_bitrate::request_t *request, int host_fec_percentage) {
    if (request == nullptr) {
      return {host_fec_percentage, k_default_audio_kbps};
    }
    return {request->fec_percentage, request->audio_kbps > 0 ? request->audio_kbps : k_default_audio_kbps};
  }

  /// The advice for one stream shape.
  struct advice_t {
    bool valid = false;
    int width = 0;
    int height = 0;
    int fps = 0;
    bool chroma444 = false;
    rule_e rule = rule_e::none;
    /// What the encoder needs on a device's own screen (H 2.875, at k_far_target_db) and on a
    /// television (H 2.0, at k_target_db).
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

  /// The request that lands the encoder on encoder_kbps over link: what a client sets for that rate.
  /// 0 when encoder_kbps is not positive.
  int request_for_encoder(int encoder_kbps, const link_t &link);

  /**
   * @brief Whether a stream that runs at request_kbps is short of bits.
   *
   * True below k_starved_below_share of the raise goal, both as requests, which is the one figure
   * Doctor quotes. A stream with no advice, or at no known rate, is not starved.
   */
  bool starved(const advice_t &advice, int request_kbps);

  /**
   * @brief Whether a stream wants more than Doctor raises it to at its size and frame rate.
   *
   * True when k_cap_kbps or max_bitrate holds the raise goal below the far figure, the stream runs at
   * request_kbps no more than a tenth below that goal, so it is not starved, and below the far figure,
   * and more than k_ceiling_bound_share of its recent frames fill PyroWave's byte budget. No raise
   * Doctor offers reaches what the model asks and a cut only softens the picture, so Doctor suggests a
   * smaller or slower picture, or HEVC, and where only k_cap_kbps holds the goal, notes that a player
   * can set more by hand, up to stream_bitrate::k_max_request_kbps or max_bitrate if that is lower.
   * A stream whose goal is the far
   * figure itself never wants more, whatever its ceiling share, and neither does one that runs at the
   * far figure or above it.
   * @param ceiling_frame_share The share of recent frames at the byte budget, or nullopt while unknown.
   */
  bool needs_more_than_allowed(const advice_t &advice, int request_kbps, std::optional<double> ceiling_frame_share);

  /**
   * @brief The advice fields of the contract, without anything live.
   *
   * version, model, target_db (the near figure's quality), far_target_db (the far figure's), width,
   * height, fps, chroma, advice_far_kbps, advice_near_kbps, raise_goal_kbps and cap_kbps, plus rule and
   * raise_goal_limited_by. Every kbps figure is a request.
   */
  nlohmann::json advice_json(const advice_t &advice);

  /// What the host can say about PyroWave when a client asks for advice before a launch.
  struct route_host_t {
    /// This build carries the model and the codec.
    bool built = false;
    /// A device on this host can run the codec.
    bool device_available = false;
    /// The FEC share and audio cost the advice is grossed up for: the asking client's own stream's
    /// when it streams here, see stream_link(), and the host's FEC share with stereo in high quality
    /// when it does not.
    int fec_percentage = 10;
    int max_bitrate_kbps = 0;
    int audio_kbps = k_default_audio_kbps;
  };

  /**
   * @brief The reply to GET /polaris/v1/pyrowave/advice.
   *
   * The same advice fields session status carries while a PyroWave stream runs, for a shape a client
   * is about to ask for, grossed up for host.fec_percentage and host.audio_kbps: the asking client's
   * own stream's when it streams here, and the host's FEC share with stereo in high quality when it
   * does not. A host that cannot serve
   * PyroWave says so, with a reason code and a sentence a player can read; one that has the model but
   * no device still gives the figures, because they do not depend on the device.
   * @param width,height,fps Decimal integers, 1 to 16384 and 1 to 1000.
   * @param chroma 420 or 444.
   * @param http_status Set to 200, or to 400 when a field is missing or malformed.
   */
  nlohmann::json advice_reply(std::string_view width, std::string_view height, std::string_view fps,
                              std::string_view chroma, const route_host_t &host, int &http_status);

}  // namespace pyrowave_advice
