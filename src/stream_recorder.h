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
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <system_error>
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
   *
   * One save runs at a time; a request while one is running is refused at once. The save
   * snapshots the current run under the recorder lock (shared payload pointers, no byte copy)
   * and then writes, syncs and publishes it with no recorder lock held, so packets, status
   * and session changes never wait for storage. A save admitted this way finishes its captured
   * run, codec and output directory even if the session, mode or configuration changes while
   * it writes; that is host recorder policy, not a client or paired permission.
   * @return The path of the published file, or empty string on any refusal or failure.
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
   * @brief The cause of the most recent change that removed live replay packets or reset the run.
   *
   * It records what happened, not what the buffer is waiting for: whether a run is buffered and
   * whether replay is waiting for a keyframe are reported separately. It persists until another
   * such change replaces it.
   */
  enum class window_limit_e {
    none,           ///< Nothing has trimmed or reset the run since the process (or test) started
    duration,       ///< Expiry past the configured duration removed packets
    payload_cap,    ///< Making room under the payload cap removed packets
    entry_cap,      ///< Making room under the packet cap removed packets (payload_cap wins when both applied)
    configuration,  ///< A mode, duration or cap reconfiguration reset the run
    selection,      ///< A session start, end or eligibility change reset the run
    codec,          ///< The selected session's codec changed, so its earlier frames went
    discontinuity,  ///< A dropped owner frame (invalid or oversized) or a clock regression reset the run
  };

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
    // A save in progress holds an immutable snapshot of the run it admitted. Its payloads are
    // shared with the live run until the live run drops them, so the unique replay payload is
    // the live bytes plus the snapshot bytes no longer live: at most max_bytes + max_snapshot_bytes.
    bool save_in_progress = false;            ///< One save at a time
    std::size_t snapshot_packets = 0;         ///< Entries the save in progress holds
    std::size_t snapshot_bytes = 0;           ///< Payload bytes those entries reference
    std::size_t snapshot_only_bytes = 0;      ///< Of those, bytes the live run no longer holds
    std::size_t unique_payload_bytes = 0;     ///< bytes + snapshot_only_bytes
    std::size_t metadata_entries = 0;         ///< packets + snapshot_packets
    std::size_t max_snapshot_bytes = 0;       ///< A snapshot is one admitted run: never above max_bytes
    std::size_t max_snapshot_packets = 0;     ///< ... and never above max_packets
    std::size_t max_unique_payload_bytes = 0; ///< max_bytes + max_snapshot_bytes
    std::size_t max_metadata_entries = 0;     ///< max_packets + max_snapshot_packets
    std::uint64_t saves_completed = 0;        ///< Saves published
    std::uint64_t saves_failed = 0;           ///< Admitted saves that failed in storage (nothing published)
    std::uint64_t saves_refused_busy = 0;     ///< Requests refused because a save was running
    // The buffered window, by arrival time. A run is the stored packets of the selected session;
    // it is either empty or starts at a keyframe. That is stored-run evidence only: it is not save
    // eligibility (mode, busy gate, storage), not media decodability, and arrival span is not
    // presentation duration.
    bool keyframe_start_run = false;          ///< The live run is non-empty (and so starts at a keyframe)
    std::size_t run_keyframes = 0;            ///< Keyframes in the live run
    std::chrono::steady_clock::duration run_span {0};       ///< Newest minus front arrival time; 0 when empty
    std::chrono::steady_clock::duration run_front_age {0};  ///< Read time minus front arrival; never negative
    /// Read time minus the arrival of the last keyframe the selected run accepted; never negative.
    /// Unknown (empty) until one is accepted, and after any reset of the run (configuration,
    /// selection, codec or discontinuity). Trimming and expiry keep it, so it still reports how
    /// long ago the last keyframe arrived even after the run becomes empty.
    std::optional<std::chrono::steady_clock::duration> last_accepted_keyframe_age;
    /// Replay is enabled, one eligible session is selected, and no run is buffered: the next
    /// packet the run can start with is a keyframe. Independent of any refused-packet history.
    bool awaiting_keyframe = false;
    window_limit_e last_window_limit = window_limit_e::none;  ///< See window_limit_e
    std::size_t snapshot_keyframes = 0;       ///< Keyframes in the save in progress, fixed at admission
    std::chrono::steady_clock::duration snapshot_span {0};  ///< Its arrival span, fixed at admission
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

    /// Why a save published nothing, or that it published. Internal: no endpoint shape changes.
    enum class save_status_e {
      saved,          ///< Published at `path`
      busy,           ///< Another save was running; refused without waiting
      disabled,       ///< Replay mode is not enabled
      empty,          ///< No fresh run to save; nothing was created
      storage_error,  ///< Directory, staging, write, sync, close or publication failed; nothing published
    };

    struct save_result_t {
      save_status_e status = save_status_e::empty;
      std::string path;                ///< The published file, only when saved
      std::size_t packets = 0;         ///< Packets in the admitted snapshot
      std::size_t bytes = 0;           ///< Their payload bytes
      int video_format = -1;           ///< The snapshot run's codec
      std::uint64_t generation = 0;    ///< The session selected when the save was admitted
      /// A run was captured under the lock (saved, or a storage error after capture). Busy,
      /// disabled, empty and a failed admission leave it false and capture nothing.
      bool admitted = false;
      std::size_t keyframes = 0;       ///< Keyframes in the admitted snapshot
      std::chrono::steady_clock::duration span {0};  ///< Its arrival span (not presentation duration)
      std::string error;               ///< What failed, for storage errors
    };

    /// save_replay_at() with its outcome. save_replay_at() returns `path` only when saved.
    save_result_t save_replay_result_at(std::chrono::steady_clock::time_point now);

#ifdef POLARIS_TESTS
    /// Replace the hard caps (tests only); clears the run.
    void set_replay_limits_for_tests(std::size_t max_bytes, std::size_t max_packets);
    /// Restore every replay buffer state to its process start: caps, sessions, run, counters and save hooks.
    void reset_replay_for_tests();

    /**
     * Storage seams for tests. Each one left empty uses the real operation. A seam that
     * replaces an operation should normally call the matching native_* function, so the
     * production file flow still runs; the stamp replaces the wall-clock part of the names.
     */
    struct save_hooks_t {
      std::function<std::string()> name_stamp;
      std::function<void()> after_snapshot_payloads;  ///< Admission fault boundary, under the lock
      std::function<void(const std::filesystem::path &staging)> before_write;
      std::function<long long(int fd, const std::uint8_t *data, std::size_t size)> write;
      std::function<int(int fd)> sync;
      std::function<int(int fd)> close;
      std::function<void(const std::filesystem::path &staging, const std::filesystem::path &final_path)> before_publish;
      std::function<std::error_code(const std::filesystem::path &staging, const std::filesystem::path &final_path)> publish;
    };
    /// Install seams for the next saves; set them before starting a save, not during one.
    void set_save_hooks_for_tests(save_hooks_t hooks);
    long long native_write(int fd, const std::uint8_t *data, std::size_t size);
    int native_sync(int fd);
    int native_close(int fd);
    std::error_code native_publish(const std::filesystem::path &staging, const std::filesystem::path &final_path);
#endif
  }  // namespace detail

  /**
   * @brief Save the replay buffer and say how it ended (host-admin use; no new permission).
   * Same save as save_replay(); the result also says why nothing was published.
   */
  detail::save_result_t save_replay_result();

  /**
   * @brief The recorder's state as one consistent read, taken under the recorder lock.
   *
   * `mode` is the configured mode and `recording` whether a continuous writer is open: they are
   * independent facts (a continuous writer can stay open in any mode). `file` is that writer's
   * path. `replay` is replay_stats() taken in the same critical section, including a save
   * snapshot that stays in progress after the mode changes.
   */
  struct recorder_status_t {
    mode_t mode = mode_t::disabled;
    bool recording = false;
    std::string file;
    replay_stats_t replay;
  };

  recorder_status_t recorder_status();

  namespace detail {
    /// recorder_status() at an explicit steady-clock time (the replay expiry boundary).
    recorder_status_t recorder_status_at(std::chrono::steady_clock::time_point now);
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
