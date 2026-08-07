/**
 * @file src/audio.h
 * @brief Declarations for audio capture and encoding.
 */
#pragma once

// local includes
#include "platform/common.h"
#include "thread_safe.h"
#include "utility.h"

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <string>

namespace audio {
  enum stream_config_e : int {
    STEREO,  ///< Stereo
    HIGH_STEREO,  ///< High stereo
    SURROUND51,  ///< Surround 5.1
    HIGH_SURROUND51,  ///< High surround 5.1
    SURROUND71,  ///< Surround 7.1
    HIGH_SURROUND71,  ///< High surround 7.1
    MAX_STREAM_CONFIG  ///< Maximum audio stream configuration
  };

  struct opus_stream_config_t {
    std::int32_t sampleRate;
    int channelCount;
    int streams;
    int coupledStreams;
    const std::uint8_t *mapping;
    int bitrate;
  };

  struct stream_params_t {
    int channelCount;
    int streams;
    int coupledStreams;
    std::uint8_t mapping[8];
  };

  extern opus_stream_config_t stream_configs[MAX_STREAM_CONFIG];

  /**
   * Return the audio capture jitter buffer capacity in interleaved samples.
   *
   * The capture side should tolerate short host scheduling or PipeWire callback
   * bursts without silently dropping audio. Keep this helper shared so the
   * runtime and unit tests agree on the intended buffer depth.
   */
  std::size_t capture_jitter_buffer_capacity(std::uint32_t frame_size, int channels);

  /**
   * Return how many incoming interleaved samples would be dropped by appending
   * incoming_samples to a ring buffer with the given available/capacity state.
   */
  std::size_t ring_buffer_overflow_samples(std::size_t available, std::size_t capacity, std::size_t incoming_samples);

  struct config_t {
    enum flags_e : int {
      HIGH_QUALITY,  ///< High quality audio
      HOST_AUDIO,  ///< Host audio
      CUSTOM_SURROUND_PARAMS,  ///< Custom surround parameters
      MAX_FLAGS  ///< Maximum number of flags
    };

    int packetDuration;
    int channels;
    int mask;

    stream_params_t customStreamParams;

    std::bitset<MAX_FLAGS> flags;

    // Who TF knows what Sunshine did
    // putting input_only at the end of flags will always be over written to true
    uint64_t __padding;

    bool input_only;
  };

  struct audio_ctx_t {
    // We want to change the sink for the first stream only
    std::unique_ptr<std::atomic_bool> sink_flag;

    std::unique_ptr<platf::audio_control_t> control;

    bool restore_sink;
    /**
     * @brief Whether this session is the one holding a default-sink claim.
     *
     * Separate from restore_sink, which only means "something needs undoing at
     * stop". Releasing a refcounted claim this session never took would
     * decrement a claim another session still holds.
     */
    bool claimed_default;
    platf::sink_t sink;
  };

  using buffer_t = util::buffer_t<std::uint8_t>;
  using packet_t = std::pair<void *, buffer_t>;
  using packet_queue_t = safe::mail_raw_t::queue_t<packet_t>;
  using audio_ctx_ref_t = safe::shared_t<audio_ctx_t>::ptr_t;

  void capture(safe::mail_t mail, config_t config, void *channel_data, packet_queue_t packets = nullptr);

  std::string select_sink_name(const audio_ctx_t &ctx, int channels, bool host_audio);

  bool sink_is_virtual(const audio_ctx_t &ctx, const std::string &sink);

  // EasyEffects / JamesDSP style sinks pin streams via WirePlumber target.object.
  // Used for diagnostics only — stream isolation claims a virtual sink as default
  // instead of capturing the processing graph or re-pinning sink-inputs.
  bool host_sink_is_processing(const std::string &sink_name);

  // Legacy re-pin path (disabled when claim is active). Prefer should_claim_default_sink.
  bool should_route_session_sink_without_default(const audio_ctx_t &ctx, const std::string &sink, bool host_audio);

  // Claim the stream capture sink as the session default so
  // WirePlumber follows it. Disabled with POLARIS_STREAM_SINK=0.
  bool stream_sink_claim_enabled();
  bool should_claim_default_sink(const audio_ctx_t &ctx, const std::string &sink, bool host_audio);

  /**
   * @brief Whether this session may release the refcounted default-sink claim.
   *
   * The claim is refcounted on the shared audio control, so a session that never
   * took one must not release one: the decrement would come out of a claim
   * another session still holds, and on the last decrement it would restore the
   * host default underneath a stream still running.
   *
   * `restore_sink` cannot answer this. It means "something needs undoing at
   * stop", and the legacy set_sink path sets it too without ever claiming.
   */
  bool owns_default_sink_claim(const audio_ctx_t &ctx);

  /**
   * @brief Get the reference to the audio context.
   * @returns A shared pointer reference to audio context.
   * @note Aside from the configuration purposes, it can be used to extend the
   *       audio sink lifetime to capture sink earlier and restore it later.
   *
   * @examples
   * audio_ctx_ref_t audio = get_audio_ctx_ref()
   * @examples_end
   */
  audio_ctx_ref_t get_audio_ctx_ref();

  /**
   * @brief Check if the audio sink held by audio context is available.
   * @returns True if available (and can probably be restored), false otherwise.
   * @note Useful for delaying the release of audio context shared pointer (which
   *       tries to restore original sink).
   *
   * @examples
   * audio_ctx_ref_t audio = get_audio_ctx_ref()
   * if (audio.get()) {
   *     return is_audio_ctx_sink_available(*audio.get());
   * }
   * return false;
   * @examples_end
   */
  bool is_audio_ctx_sink_available(const audio_ctx_t &ctx);
}  // namespace audio
