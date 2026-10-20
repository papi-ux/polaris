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
#include <memory>
#include <optional>
#include <system_error>
#include <string>
#include <vector>

namespace safe {
  template<class T>
  class event_t;
  template<class T>
  class post_t;
}  // namespace safe

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
   * @brief Start continuous recording to a file (host-admin action; no client permission).
   *
   * Opens a new private file at once, in any mode, and never replaces or follows an existing
   * name: `polaris_<stamp><ext>`, then `polaris_<stamp>-N<ext>` on a collision. The directory
   * and the extension's codec are fixed for this file. When exactly one eligible session is
   * selected and its codec is known, the file is bound to that session now; otherwise the
   * extension falls back to the last known stream codec (or the configured allow-lists) and
   * the file is bound later, at a keyframe, to a selected eligible session of that codec.
   * One file holds at most one session's video, in one codec, starting at a keyframe: raw
   * elementary video only (no container, audio or timestamps). An overlap suspends all input
   * and only the same owner may resume, at a keyframe; once its owner ends or violates the
   * codec, the file takes nothing more. The file stays open until stop_recording().
   *
   * Storage is asynchronous: an admitted packet is copied into the file's fixed, preallocated
   * queue (k_continuous_queue_bytes, k_continuous_queue_slots, counting the packet being
   * written) and one writer thread per file writes it; a packet push never waits for, or makes,
   * a filesystem call. A packet that does not fit is dropped and the file waits for a keyframe
   * that fits. Admitted packets are written even if the owner later ends or overlaps. There is
   * no fsync. Starting, which creates the directory and opens the file, blocks its caller but
   * never the packet path or status. Repeating a start changes nothing, and so does a start
   * after shutdown().
   */
  void start_recording();

  /**
   * @brief Stop continuous recording: refuse new packets, write every admitted packet, then
   * close the file and forget its owner. Repeating it changes nothing.
   *
   * It waits for the writer without a time limit: on a stalled filesystem it can wait
   * indefinitely. While it waits, the status still reports the file, as `stopping`.
   */
  void stop_recording();

  /**
   * @brief Process exit: stop continuous recording as stop_recording() does, and refuse every
   * later start. Like stop_recording(), it can wait indefinitely on a stalled filesystem.
   */
  void shutdown();

  /// The bytes one continuous file may hold queued or being written; allocated once per file.
  inline constexpr std::size_t k_continuous_queue_bytes = std::size_t {16} * 1024 * 1024;
  /// The packets one continuous file may hold queued or being written.
  inline constexpr std::size_t k_continuous_queue_slots = 256;

  /// Packets larger than this are refused before they are read, whatever the recorder mode.
  inline constexpr std::size_t k_continuous_max_packet_bytes = std::size_t {128} * 1024 * 1024;

  /// What the continuous file is doing. Internal diagnostics; no endpoint shape changes.
  enum class continuous_state_e {
    idle,               ///< No file is open
    awaiting_owner,     ///< Open; no selected eligible session of the file's codec has sent a keyframe
    awaiting_keyframe,  ///< Bound; the owner's next keyframe starts (or resumes) the file
    writing,            ///< Bound; the owner's packets are written
    suspended,          ///< Open; nothing is written for the reason in `cause`
    failed,             ///< Open; a write failed, and nothing more is written until stop
    stopping,           ///< Open; stop refuses new packets and writes the admitted ones
  };

  /// The most recent reason the continuous file left `writing` (or could not start it).
  enum class continuous_cause_e {
    none,
    selection,      ///< The owner is not the sole eligible session (overlap or ineligible)
    owner_ended,    ///< The owner ended; the file never takes another session
    codec,          ///< A codec differs from the file's; permanent once the file is bound
    discontinuity,  ///< An invalid, oversized or clock-regressed owner packet was dropped
    storage,        ///< A write failed or made no progress
    backpressure,   ///< An owner packet did not fit the file's queue and was dropped
  };

  enum class continuous_error_e {
    none,
    write,        ///< write() failed; see error_number
    no_progress,  ///< write() returned zero or more than was asked
  };

  /**
   * @brief The continuous file's ownership and accounting, taken under the recorder lock.
   * Defaults describe no open file. Packets of unregistered, zero, retired or other sessions
   * are not counted here: they never change the file or this state.
   *
   * Writer-side fields (written, discarded, queued, error) come from the file's writer in the
   * same snapshot, and a storage failure is reported as `failed`/`storage` whatever happened to
   * the session since. Every admitted packet is exactly one of written, discarded or queued:
   * packets_admitted == packets_written + discarded_packets + queued_packets. bytes_written grows
   * with every byte write() accepts, while the packet being written stays whole in queued_bytes
   * until it finishes; so bytes_admitted == bytes_written + discarded_bytes + queued_bytes holds
   * whenever no packet is being written (for example after the queue drains).
   */
  struct continuous_status_t {
    continuous_state_e state = continuous_state_e::idle;
    continuous_cause_e cause = continuous_cause_e::none;
    std::uint64_t owner_generation = 0;  ///< The bound session, or 0 while unbound
    int file_video_format = -1;          ///< The codec the file's extension names
    std::uint64_t packets_written = 0;   ///< Owner packets written whole
    std::uint64_t bytes_written = 0;     ///< Bytes write() accepted, including a failed packet's prefix
    std::uint64_t refused_invalid = 0;   ///< Owner (or candidate) packets with no data, no size, an unknown codec or above the cap
    std::uint64_t refused_codec = 0;     ///< Packets of a codec other than the file's
    std::uint64_t refused_awaiting_keyframe = 0;  ///< Dependent frames with no keyframe to follow
    std::uint64_t discontinuities = 0;   ///< Owner clock regressions and dropped owner frames
    continuous_error_e error = continuous_error_e::none;
    int error_number = 0;                ///< errno of a failed write
    std::uint64_t packets_admitted = 0;  ///< Owner packets copied into the queue
    std::uint64_t bytes_admitted = 0;    ///< Their bytes
    std::size_t queued_packets = 0;      ///< Admitted packets not yet finished, the one being written included
    std::size_t queued_bytes = 0;        ///< Their bytes
    std::size_t max_queue_packets = 0;   ///< The file's queue capacity in packets
    std::size_t max_queue_bytes = 0;     ///< The file's queue capacity in bytes
    std::uint64_t dropped_backpressure = 0;  ///< Owner packets that did not fit the queue
    std::uint64_t discarded_packets = 0;  ///< Admitted packets not written whole because the file had failed
    std::uint64_t discarded_bytes = 0;    ///< Their bytes not written (a failed packet's written prefix excluded)
  };

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
   *
   * The continuous file uses the same selection. `video_format` is the session's codec
   * (0=H.264, 1=HEVC, 2=AV1, 3=PyroWave); any other value is unknown and never replaces a
   * codec already known for the session. A codec change alone does not reset the replay run.
   * @param session_generation The session's process-unique generation (nonzero).
   * @param replay_eligible True when the host selects this session as replay-eligible.
   * @param video_format The session's codec, or -1 when not known.
   */
  void session_started(std::uint64_t session_generation, bool replay_eligible, int video_format = -1);

  /// What one keyframe request did. Only `attempted` reached an event, and even that is not an
  /// acknowledgment: the event's raise() returns nothing, and only an admitted owner keyframe is
  /// recovery.
  enum class keyframe_outcome_e {
    attempted,  ///< The session's event was alive and running when checked, and was raised
    expired,    ///< Every owner of the session's event (and so its mail) had released it
    stopped,    ///< The session's event was stopped
    retired,    ///< The session ended; its channel no longer requests
    fenced,     ///< A stop, overlap, owner end, new file or keyframe withdrew the request first
  };

  /**
   * @brief A session's keyframe request channel: a weak reference to that session's IDR event.
   *
   * It never keeps the event, or the session mail that owns it, alive, and never takes a
   * recorder lock. Requests are host recorder policy, not client permission.
   */
  class keyframe_channel_t {
  public:
    using event_post_t = safe::post_t<safe::event_t<bool>>;  ///< safe::mail_raw_t::event_t<bool>::element_type

    explicit keyframe_channel_t(std::weak_ptr<event_post_t> event) noexcept;
    keyframe_channel_t(const keyframe_channel_t &) = delete;
    keyframe_channel_t &operator=(const keyframe_channel_t &) = delete;

    /// Raise the session's event once if it is still alive and running.
    keyframe_outcome_e request() noexcept;
    /// After this returns, request() never raises again.
    void retire() noexcept;

  private:
    friend struct keyframe_channel_access_t;
    keyframe_outcome_e request(const std::function<void()> *after_raise) noexcept;

    std::mutex lock_;
    bool retired_ = false;
    std::weak_ptr<event_post_t> event_;
  };

  /**
   * @brief Register an admitted session together with its keyframe request channel.
   *
   * The same as the three-argument form, plus the channel (null is allowed and changes nothing).
   * A known generation keeps the channel it already has: a repeated or metadata-only call never
   * replaces, clears or retires it, and a different channel for a generation that already has
   * one is rejected as a whole (no eligibility or codec change). Production registration stays
   * the three-argument form, so no production session has a channel and no request is made.
   */
  void session_started(std::uint64_t session_generation, bool replay_eligible, int video_format,
                       std::shared_ptr<keyframe_channel_t> channel);

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
   * @brief Passive provenance of one encoded video packet, kept as evidence only (version 1, #359).
   *
   * Nothing here is a presentation timestamp. The recorder copies it beside the replay entry and
   * the save snapshot, marks what is missing, duplicated, regressed or gapped, and acts on none of
   * it: retention, refusals, resets, caps, trimming, keyframe alignment, continuous writes and saved
   * bytes are exactly those of push_packet(). See docs/recorder_media.md.
   */
  struct packet_provenance_t {
    static constexpr std::uint16_t k_version = 1;
    /// The encoder's frame index as the tap read it; empty when the caller supplied none. An
    /// index of 0 is a real index, never "unknown".
    std::optional<std::int64_t> frame_index;
    /// The packet's frame_timestamp as it was at the tap (capture or pacing time), if it had one.
    /// Evidence only: it is not a presentation time and is never replaced by the arrival time.
    std::optional<std::chrono::steady_clock::time_point> frame_timestamp;
    /// Copied from the packet: a timestamp-coordinate fact, never authority, proof or VRR.
    bool source_clock_timestamp = false;
    /// Copied from the packet: never authority, proof or VRR.
    bool source_time_paced = false;
  };

  /**
   * @brief push_packet() for one video packet, with its provenance kept as evidence.
   *
   * The same admission as push_packet(data, size, true, is_keyframe, index, session_generation,
   * video_format), where index is provenance.frame_index or 0, plus the provenance record.
   */
  void push_video(const uint8_t *data, size_t size, bool is_keyframe, std::uint64_t session_generation,
                  int video_format, const packet_provenance_t &provenance);

  namespace detail {
    /// Evidence marks of one replay entry, computed once at admission against the adjacent retained
    /// entry (the buffer's back after this admission's expiry, trimming and alignment). An empty
    /// buffer means no comparison; a coordinate unknown on either side is not compared. A mark
    /// never refuses, trims, resets or cuts anything.
    struct provenance_marks_t {
      bool index_unsupplied = false;  ///< No frame index was supplied (legacy push_packet())
      bool index_duplicate = false;   ///< Equal to the adjacent retained entry's supplied index
      bool index_regressed = false;   ///< Lower than the adjacent retained entry's supplied index
      bool index_gap = false;         ///< Higher than the adjacent retained entry's index by more than one
      bool time_missing = false;      ///< No frame_timestamp at the tap (untimed)
      bool time_duplicate = false;    ///< Equal to the adjacent retained entry's frame_timestamp
      bool time_regressed = false;    ///< Earlier than the adjacent retained entry's frame_timestamp
      bool operator==(const provenance_marks_t &) const = default;
    };

    /// What the recorder keeps beside each replay payload, live and in a held save snapshot.
    struct provenance_record_t {
      std::uint16_t version = packet_provenance_t::k_version;
      packet_provenance_t supplied;  ///< As supplied; all empty for push_packet()
      provenance_marks_t marks;
    };
  }  // namespace detail

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
    std::uint64_t saves_failed = 0;           ///< Admitted saves released without publication (abandoned, unsubmitted or failed)
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
    /**
     * Diagnostic entry: the same admission as push_packet() at an explicit steady-clock time,
     * and then, unlike push_packet(), a bounded wait (10 s) until the continuous writer has
     * finished the one packet this call admitted (written, or discarded after a failure), if
     * any. It keeps exact-byte checks possible right after a push; it says nothing about the
     * production path, which never waits.
     */
    void push_packet_at(const uint8_t *data, size_t size, bool is_video, bool is_keyframe, int64_t pts,
                        std::uint64_t session_generation, int video_format,
                        std::chrono::steady_clock::time_point now);
    /// push_video() at an explicit steady-clock time, with push_packet_at()'s bounded writer wait.
    void push_video_at(const uint8_t *data, size_t size, bool is_keyframe, std::uint64_t session_generation,
                       int video_format, const packet_provenance_t &provenance,
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

    struct save_admission_t;
#ifdef POLARIS_TESTS
    struct snapshot_provenance_t;
#endif

    /**
     * Move-only owner of one admitted replay save: its captured snapshot (payload references
     * and save accounting) and the one-save-at-a-time reservation. Any thread may own, move,
     * complete or drop it. Dropping or completing it releases the snapshot and its accounting
     * first, then the reservation, exactly once; a moved-from handle owns nothing.
     */
    class admitted_save_t {
    public:
      admitted_save_t() noexcept;
      admitted_save_t(admitted_save_t &&other) noexcept;
      admitted_save_t &operator=(admitted_save_t &&other) noexcept;
      admitted_save_t(const admitted_save_t &) = delete;
      admitted_save_t &operator=(const admitted_save_t &) = delete;
      ~admitted_save_t();

      /// True while it owns an admitted save.
      explicit operator bool() const noexcept;

    private:
      struct state_t;
      explicit admitted_save_t(std::unique_ptr<state_t> state) noexcept;
      std::unique_ptr<state_t> state_;

      friend save_admission_t admit_replay_save(std::chrono::steady_clock::time_point now);
      friend save_result_t complete_replay_save(admitted_save_t save);
#ifdef POLARIS_TESTS
      friend snapshot_provenance_t snapshot_provenance_for_tests(const admitted_save_t &save);
#endif
    };

    /// The outcome of admission. `save` owns the captured run only when `result.admitted`.
    struct save_admission_t {
      /// busy, disabled, empty or storage_error (admission failed) with admitted=false; or, when
      /// admitted, the captured fields (packets, bytes, keyframes, span, codec, generation) with
      /// admitted=true and no storage outcome yet.
      save_result_t result;
      admitted_save_t save;
    };

    /**
     * Admit a save without storing it: refuse at once when another save holds the reservation,
     * then, under the recorder lock, refuse a disabled or empty buffer and otherwise capture the
     * run (payload references, accounting). Fast; no filesystem work.
     */
    save_admission_t admit_replay_save(std::chrono::steady_clock::time_point now);

    /**
     * Store an admitted save (any thread, no recorder lock), then release its snapshot and
     * accounting and then its reservation. The result keeps the captured fields; once the file
     * is published, later diagnostics cannot change it. An empty handle is a storage_error with
     * admitted=false and touches nothing.
     */
    save_result_t complete_replay_save(admitted_save_t save);

#ifdef POLARIS_TESTS
    /// Replace the hard caps (tests only); clears the run.
    void set_replay_limits_for_tests(std::size_t max_bytes, std::size_t max_packets);
    /// Restore every replay buffer state to its process start: caps, sessions, run, counters and save hooks.
    void reset_replay_for_tests();

    /// One live or held replay entry: its payload (copied for inspection) and its provenance.
    struct provenance_entry_t {
      std::uint64_t sequence = 0;  ///< The live entry's sequence; 0 for a snapshot entry
      std::vector<std::uint8_t> payload;
      provenance_record_t record;
    };

    /// The live run's entries in order (tests only; no route, status or JSON exposes them).
    std::vector<provenance_entry_t> replay_provenance_for_tests();

    /// A held save's captured entries, paired by position, and its capacities (tests only).
    struct snapshot_provenance_t {
      std::vector<provenance_entry_t> entries;  ///< Paired entries, up to the shorter array
      std::size_t payload_count = 0;        ///< Payload references held
      std::size_t provenance_count = 0;     ///< Provenance records held (equals payload_count)
      std::size_t payload_capacity = 0;     ///< The snapshot's payload-reference array capacity
      std::size_t provenance_capacity = 0;  ///< The snapshot's provenance array capacity
    };

    /// Inspect a held admitted save without changing it; empty for an empty handle.
    snapshot_provenance_t snapshot_provenance_for_tests(const admitted_save_t &save);

    /// Compiled sizes of the replay bookkeeping (bounded accounting, not allocator or RSS cost).
    struct provenance_sizes_t {
      std::size_t packet_provenance = 0;   ///< sizeof(packet_provenance_t)
      std::size_t provenance_record = 0;   ///< sizeof(provenance_record_t), per live entry and per snapshot entry
      std::size_t live_entry = 0;          ///< sizeof the live replay entry, provenance included
      /// sizeof a mirror of the pre-#359 live entry (same members, same order, same build): a
      /// same-compiler baseline for the per-entry delta, not the removed type itself.
      std::size_t pre_provenance_entry_mirror = 0;
      std::size_t snapshot_payload_ref = 0;  ///< sizeof one snapshot payload reference
    };
    provenance_sizes_t provenance_sizes_for_tests();

    /**
     * Storage seams for tests. Each one left empty uses the real operation. A seam that
     * replaces an operation should normally call the matching native_* function, so the
     * production file flow still runs; the stamp replaces the wall-clock part of the names.
     */
    struct save_hooks_t {
      std::function<std::string()> name_stamp;
      std::function<void()> after_snapshot_payloads;  ///< Admission fault boundary, under the lock
      /// Before the capture state of an eligible run is allocated, under the recorder lock (call
      /// no recorder function from it). Never reached for busy, disabled or empty refusals.
      std::function<void()> before_admission_allocation;
      /// After the snapshot's payload array was reserved and before its provenance array is
      /// reserved (#359), under the recorder lock (call no recorder function from it). May throw.
      std::function<void()> before_snapshot_provenance_allocation;
      /// Just before the save reservation is released, with no recorder lock held; must not throw.
      std::function<void()> before_save_reservation_release;
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

    /**
     * Continuous-file seams for tests, separate from the save seams. Each one left empty uses
     * the real operation or does nothing. A start copies them into the file's writer, so later
     * changes apply to the next file only. None may call a recorder function, except `close`,
     * `stop_entered` and `before_stop_join`, which run with no recorder lock held.
     * Where each runs:
     * - name_stamp, before_block_allocation, before_open, open, after_open, before_launch,
     *   after_launch: the starting thread, no recorder lock (a throw rolls the start back);
     * - rollback_close: the starting thread, no lock, while a start rolls back its file;
     * - before_publish: the pushing thread, under the recorder lock, after the packet is copied
     *   into the queue and before it is published;
     * - write, worker_started, worker_exited, after_worker_failure: the writer thread, no lock;
     * - stop_entered: on entry to stop_recording(); before_stop_join: once `stopping` is
     *   published, before the writer is joined; close: after idle is published.
     */
    struct continuous_hooks_t {
      std::function<std::string()> name_stamp;  ///< Replaces the wall-clock part of the name
      /// Before each candidate is opened; may throw (nothing has been created yet).
      std::function<void(const std::filesystem::path &candidate)> before_open;
      /// Replaces the exclusive open; should normally call native_open_exclusive().
      std::function<int(const std::filesystem::path &candidate)> open;
      /// After a successful open, before the file is committed; a throw must leave no file behind.
      std::function<void(const std::filesystem::path &path)> after_open;
      std::function<long long(int fd, const std::uint8_t *data, std::size_t size)> write;
      std::function<int(int fd)> close;
      std::function<void()> before_block_allocation;  ///< May throw: nothing exists yet
      std::function<void()> before_launch;  ///< May throw: the file exists, no writer thread yet
      std::function<void()> after_launch;   ///< May throw: the writer runs, nothing is committed
      std::function<void()> before_publish;
      std::function<void()> worker_started;
      std::function<void()> worker_exited;
      std::function<void()> after_worker_failure;  ///< After the failure is published
      std::function<void()> stop_entered;
      std::function<void()> before_stop_join;
      /// Replaces the native close when a start rolls back a file it created (the file is
      /// removed afterwards either way). It must close `fd` exactly once and must not throw.
      std::function<void(int fd)> rollback_close;
    };
    /// Install continuous seams; set them while no file is open. reset_replay_for_tests() clears them.
    void set_continuous_hooks_for_tests(continuous_hooks_t hooks);
    /// The real exclusive, no-follow, private (0600 on POSIX) creation.
    int native_open_exclusive(const std::filesystem::path &path);
    /// Queue capacity for files started from now on (both nonzero); reset_replay_for_tests() restores the defaults.
    void set_continuous_limits_for_tests(std::size_t bytes, std::size_t slots);
    /// Wait, at most `timeout`, until the open file's writer has finished every admitted packet.
    /// An explicit test drain: production paths never wait like this.
    bool wait_continuous_drained_for_tests(std::chrono::milliseconds timeout);
    /**
     * Run the same owner mechanism that ends a writer still running at process exit: take the
     * writer thread from the recorder, tell it to stop, wake it and join it. The file stays open
     * and recorded until stop_recording(). True when a writer thread was joined.
     */
    bool finish_continuous_writer_for_tests();

    /// The identity of one keyframe request: the file it serves, its owner and its sequence.
    struct keyframe_token_t {
      std::uint64_t incarnation = 0;
      std::uint64_t generation = 0;
      std::uint64_t sequence = 0;
      bool operator==(const keyframe_token_t &) const = default;
    };

    /**
     * Diagnostic-only keyframe request state (tests only; no route or JSON exposes it).
     * Per-file fields reset at each start_recording commit and stay readable after stop until
     * the next start. Process totals are zeroed only by reset_replay_for_tests(). The two
     * monotonic counters never decrease, not even across reset_replay_for_tests().
     */
    struct keyframe_stats_t {
      std::uint64_t file_incarnation = 0;  ///< 0 while no file is open, or when incarnations ran out
      bool awaiting = false;               ///< The bound owner's file waits for a keyframe
      bool keyframe_exceeds_queue = false;  ///< An owner keyframe was larger than the whole queue
      std::optional<keyframe_token_t> in_flight;   ///< The one reservation not yet completed
      std::optional<keyframe_token_t> authorized;  ///< The one dispatch still allowed to raise
      std::optional<std::chrono::steady_clock::time_point> last_request_at;  ///< Rate anchor
      std::size_t last_keyframe_bytes = 0;  ///< Size of this file's last admitted owner keyframe
      std::uint64_t reservations = 0;
      std::uint64_t attempted = 0;
      std::uint64_t expired = 0;
      std::uint64_t stopped = 0;
      std::uint64_t retired = 0;
      std::uint64_t fenced = 0;
      std::uint64_t refused_rate = 0;
      std::uint64_t refused_writer_unavailable = 0;
      std::uint64_t refused_no_slot = 0;
      std::uint64_t refused_queue_sequence_exhausted = 0;
      std::uint64_t refused_bytes = 0;
      std::uint64_t refused_request_sequence_exhausted = 0;
      std::uint64_t keyframe_stale_completions_total = 0;
      std::uint64_t keyframe_registration_conflicts = 0;
      std::uint64_t continuous_incarnation = 0;
      std::uint64_t keyframe_request_sequence = 0;
    };
    keyframe_stats_t keyframe_stats_for_tests();

    /**
     * Keyframe request seams for tests: they only pause or observe a request, never change it.
     * before_dispatch runs after the recorder lock is released and before the request gate;
     * after_dispatch runs after completion is counted, with the recorder lock and gate released.
     * Both hold no recorder lock: they may wait on test state and call keyframe_stats_for_tests()
     * or recorder_status(), and nothing else of the recorder.
     * after_raise runs inside the real request, right after the event was raised, while the
     * request gate and the channel's lock are held. It may only wait on test state and call
     * recorder_status() or recorder_mutex_try_lock_for_tests(): keyframe_stats_for_tests() takes
     * the request gate and would deadlock, and no lifecycle, start, stop or packet entry is safe.
     */
    struct keyframe_hooks_t {
      std::function<void(const keyframe_token_t &)> before_dispatch;
      std::function<void()> after_raise;
      std::function<void(const keyframe_token_t &, keyframe_outcome_e)> after_dispatch;
    };
    void set_keyframe_hooks_for_tests(keyframe_hooks_t hooks);
    /// Raise the monotonic counters to at least these values; never lowers them.
    void set_keyframe_counters_for_tests(std::uint64_t incarnation_floor, std::uint64_t sequence_floor);
    /// Set the open file's queue sequence (a queue counter, not ownership); false when no file is open.
    bool set_continuous_next_sequence_for_tests(std::uint64_t value);
    /// True when the recorder lock was free (taken and released at once, never waited for).
    bool recorder_mutex_try_lock_for_tests();
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
    continuous_status_t continuous;  ///< The open file's ownership; idle defaults when none
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
