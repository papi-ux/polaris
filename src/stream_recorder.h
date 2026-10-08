/**
 * @file src/stream_recorder.h
 * @brief Declarations for stream recording and replay buffer.
 */
#pragma once

// standard includes
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <vector>

namespace stream_recorder {

  enum class mode_t {
    disabled,
    continuous,    ///< Record everything to a file
    replay_buffer  ///< Keep last N minutes in memory, save on demand
  };

  struct config_t {
    mode_t mode = mode_t::disabled;
    std::string output_dir;            ///< Where to save recordings
    int replay_buffer_minutes = 5;     ///< How many minutes to keep in replay buffer
    std::string container = "mkv";     ///< Container format for filenames (mkv or mp4)
  };

  /**
   * @brief Start continuous recording to a file.
   */
  void start_recording();

  /**
   * @brief Stop continuous recording.
   */
  void stop_recording();

  /**
   * @brief Check if continuous recording is active.
   * @return True if recording.
   */
  bool is_recording();

  /**
   * @brief Save the replay buffer to a file.
   * @return The path of the saved file, or empty string on failure.
   */
  std::string save_replay();

  /// Hard cap on replay payload bytes held in memory, whatever the configured duration.
  inline constexpr std::size_t k_replay_max_payload_bytes = std::size_t {128} * 1024 * 1024;
  /// Hard cap on replay packets held in memory, which bounds per-packet metadata.
  inline constexpr std::size_t k_replay_max_packets = 32768;

  /**
   * @brief Register an admitted stream session with the replay buffer.
   *
   * Host policy: the replay buffer keeps packets only while exactly one session is
   * registered and that session is eligible. A second session, or a sole ineligible
   * (watcher or input-only) session, clears the buffer and refuses replay. Selection
   * comes only from these calls, never from which session's packet arrives first.
   * Registration works while recording is disabled, so enabling replay during an
   * admitted stream takes effect. Generation 0 is refused. Repeating an identical
   * call changes nothing; changing a session's eligibility is a transition.
   * Call before the session's encoders can produce packets.
   * @param session_generation The session's process-unique generation (nonzero).
   * @param replay_eligible True when the host selects this session as replay-eligible.
   */
  void session_started(std::uint64_t session_generation, bool replay_eligible);

  /**
   * @brief Unregister a session. Unknown or repeated generations change nothing.
   * Call after the session's packet owner has closed and drained its admitted sends,
   * so none of its queued packets can reach push_packet afterwards.
   * @param session_generation The generation passed to session_started().
   */
  void session_ended(std::uint64_t session_generation);

  /**
   * @brief Feed an encoded packet into the recorder.
   * Called from the stream pipeline after encoding but before network send.
   * @param data Pointer to the encoded packet data.
   * @param size Size of the packet data in bytes.
   * @param is_video True if this is a video packet, false for audio.
   * @param is_keyframe True if this is an IDR/keyframe.
   * @param pts Presentation timestamp (frame index).
   * @param session_generation The generation of the session that encoded the packet.
   *        The replay buffer accepts only the selected session's generation.
   * @param video_format The session's codec: 0=H.264, 1=HEVC, 2=AV1, 3=PyroWave.
   *        Filenames follow it; the replay buffer refuses any other value.
   */
  void push_packet(const uint8_t *data, size_t size, bool is_video, bool is_keyframe, int64_t pts,
                   std::uint64_t session_generation, int video_format);

  /**
   * @brief Status of the replay buffer, as fresh as the moment it is taken.
   * Taking it releases packets older than the configured duration (and the dependent
   * frames before the next keyframe), even when no packet has arrived since.
   */
  struct replay_stats_t {
    std::size_t packets = 0;                  ///< Fresh packets a save would write
    std::size_t bytes = 0;                    ///< Their payload bytes
    std::uint64_t selected_generation = 0;    ///< The selected session, or 0 when replay is refused
    std::size_t registered_sessions = 0;      ///< Sessions currently registered
    int run_video_format = -1;                ///< Codec of the current run, or -1 when unknown
    std::chrono::minutes duration {0};        ///< Requested duration (best effort under the caps)
    std::size_t max_bytes = 0;                ///< Payload cap in force
    std::size_t max_packets = 0;              ///< Packet cap in force
    std::uint64_t refused_unselected = 0;     ///< Packets of no, zero, unknown, retired or other sessions
    std::uint64_t refused_invalid = 0;        ///< Packets with no data, no size or an unknown codec
    std::uint64_t refused_oversized = 0;      ///< Packets that cannot fit the caps (never copied)
    std::uint64_t refused_awaiting_keyframe = 0;  ///< Dependent frames with no keyframe to follow
    std::uint64_t trimmed = 0;                ///< Packets dropped for duration, bytes, count or GOP alignment
    std::uint64_t discontinuities = 0;        ///< Runs cleared by a dropped owner frame, a clock regression or a codec change
    std::uint64_t refused_lifecycle = 0;      ///< session_started() calls refused (generation 0)
  };

  /**
   * @brief Snapshot the replay buffer's state under the lock, after releasing expired packets.
   * Status only: it changes no configuration, session or selection, and writes no file.
   */
  replay_stats_t replay_stats();

  namespace detail {
    /// push_packet() at an explicit steady-clock time; push_packet() passes now().
    void push_packet_at(const uint8_t *data, size_t size, bool is_video, bool is_keyframe, int64_t pts,
                        std::uint64_t session_generation, int video_format,
                        std::chrono::steady_clock::time_point now);
    /// replay_stats() at an explicit steady-clock time.
    replay_stats_t replay_stats_at(std::chrono::steady_clock::time_point now);
    /// save_replay() at an explicit steady-clock time.
    std::string save_replay_at(std::chrono::steady_clock::time_point now);

#ifdef POLARIS_TESTS
    /// Replace the hard caps (tests only); clears the run.
    void set_replay_limits_for_tests(std::size_t max_bytes, std::size_t max_packets);
    /// Restore every replay buffer state to its process start: caps, sessions, run and counters.
    void reset_replay_for_tests();
#endif
  }  // namespace detail

  /**
   * @brief Remember the active stream videoFormat for recording filenames.
   * @param video_format 0=H.264, 1=HEVC, 2=AV1.
   */
  void set_active_video_format(int video_format);

  /**
   * @brief Get the current recording mode from config.
   * @return The current mode.
   */
  mode_t current_mode();

  /**
   * @brief Get the path of the current recording file (if recording).
   * @return The file path, or empty string if not recording.
   */
  std::string current_file();

  /**
   * @brief Load config from the global config::recording settings.
   */
  void load_config();

}  // namespace stream_recorder
