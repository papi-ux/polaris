/**
 * @file src/stream_recorder.cpp
 * @brief Definitions for stream recording and replay buffer.
 *
 * This module provides two recording modes:
 * - Continuous: writes encoded video packets to a raw bitstream file as they arrive.
 * - Replay buffer: keeps the last N minutes of packets in memory and saves on demand.
 *
 * Packets are written as raw Annex-B bitstream (.h264/.hevc/.av1) files. Users can
 * remux to a container format afterwards:
 *   ffmpeg -i recording.hevc -c copy output.mkv
 *
 * This keeps the implementation simple and avoids pulling in avformat muxer complexity.
 */

// standard includes
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <climits>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <thread>
#include <utility>

// platform includes
#ifdef _WIN32
  #include <fcntl.h>
  #include <io.h>
  #include <share.h>
  #include <sys/stat.h>
#else
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

// local includes
#include "config.h"
#include "logging.h"
#include "platform/common.h"
#include "stream_recorder.h"
#include "thread_safe.h"

using namespace std::literals;
namespace fs = std::filesystem;

namespace stream_recorder {

  // ---- Internal state ----

  // Replay payloads are immutable once stored, so a save can share them with the live run
  // instead of copying their bytes.
  using payload_t = std::shared_ptr<const std::vector<uint8_t>>;

  struct buffered_packet_t {
    payload_t data;
    bool is_video;
    bool is_keyframe;
    int64_t pts;
    std::chrono::steady_clock::time_point timestamp;
    std::uint64_t sequence;  ///< Increases with every stored packet; tells a snapshot's entries apart
    detail::provenance_record_t provenance;  ///< Evidence only (#359); never read by any decision
  };

  static config_t cfg;
  static std::mutex mutex;

  // Mirrors cfg.mode for push_packet's pre-lock fast path (send thread,
  // once per encoded video packet). cfg.mode itself is mutex-only state -
  // reading it without the lock, as the fast path must to stay lock-free
  // when recording is off, was a real data race against load_config()'s
  // writer. This mirror is the only thing the fast path reads unlocked.
  static std::atomic<mode_t> mode_hint {mode_t::disabled};

#ifdef POLARIS_TESTS
  static detail::continuous_hooks_t continuous_hooks;
#endif

  /**
   * One continuous file's writer: a queue of fixed, preallocated capacity and the state its
   * writer thread shares with the recorder. The recorder (under `mutex`) and the writer thread
   * each hold it; it holds no thread, closes no descriptor and refers to no recorder global.
   *
   * Lock order: the recorder `mutex`, then `lock`. The writer takes only `lock`.
   *
   * The queue is a byte ring plus a slot ring, both bounded indexes with "used" counts, so no
   * position grows without bound. An entry stays counted until its write has finished, so the
   * capacity always includes the packet being written. Only one thread admits (it holds the
   * recorder `mutex`), and the writer only ever frees space.
   */
  struct continuous_writer_t {
    struct slot_t {
      std::size_t offset;
      std::size_t size;
      std::uint64_t sequence;
    };

    continuous_writer_t(std::size_t bytes, std::size_t slot_count):
        ring {std::make_unique_for_overwrite<std::uint8_t[]>(bytes)},
        ring_bytes {bytes},
        slots {std::make_unique_for_overwrite<slot_t[]>(slot_count)},
        slot_capacity {slot_count} {}

    continuous_writer_t(const continuous_writer_t &) = delete;
    continuous_writer_t &operator=(const continuous_writer_t &) = delete;

    const std::unique_ptr<std::uint8_t[]> ring;
    const std::size_t ring_bytes;
    const std::unique_ptr<slot_t[]> slots;
    const std::size_t slot_capacity;
    int fd = -1;  ///< The file's descriptor, owned (and closed) by the recorder, never by this

#ifdef POLARIS_TESTS
    detail::continuous_hooks_t hooks;  ///< Copied at start; valid until the writer is joined
#endif

    std::mutex lock;
    std::condition_variable changed;
    // Guarded by `lock`.
    std::size_t byte_head = 0;  ///< Ring offset of the oldest unfinished byte
    std::size_t byte_used = 0;  ///< Bytes queued or being written
    std::size_t slot_head = 0;
    std::size_t slot_used = 0;
    std::uint64_t next_sequence = 1;  ///< Admission stops before it would wrap (never in practice)
    std::uint64_t completed_sequence = 0;
    bool stop_requested = false;
    bool failed = false;
    continuous_error_e error = continuous_error_e::none;
    int error_number = 0;
    std::uint64_t packets_admitted = 0;
    std::uint64_t bytes_admitted = 0;
    std::uint64_t packets_written = 0;
    std::uint64_t bytes_written = 0;
    std::uint64_t discarded_packets = 0;
    std::uint64_t discarded_bytes = 0;
  };

  /**
   * Owns a running writer thread and its share of the writer. Ending it (destruction,
   * finish() or move-assignment over it) tells the writer to stop, wakes it and joins it, using
   * only what it owns: safe at static destruction, after the recorder's globals are gone.
   */
  class continuous_thread_t {
  public:
    continuous_thread_t() noexcept = default;

    continuous_thread_t(std::thread thread, std::shared_ptr<continuous_writer_t> writer) noexcept:
        thread_ {std::move(thread)},
        writer_ {std::move(writer)} {}

    continuous_thread_t(const continuous_thread_t &) = delete;
    continuous_thread_t &operator=(const continuous_thread_t &) = delete;

    continuous_thread_t(continuous_thread_t &&other) noexcept:
        thread_ {std::move(other.thread_)},
        writer_ {std::move(other.writer_)} {}

    continuous_thread_t &operator=(continuous_thread_t &&other) noexcept {
      if (this != &other) {
        finish();
        thread_ = std::move(other.thread_);
        writer_ = std::move(other.writer_);
      }
      return *this;
    }

    ~continuous_thread_t() {
      finish();
    }

    /// Signal stop and notify before joining, so an idle writer wakes and exits.
    bool finish() noexcept {
      if (!thread_.joinable()) {
        writer_.reset();
        return false;
      }
      {
        std::lock_guard guard(writer_->lock);
        writer_->stop_requested = true;
      }
      writer_->changed.notify_all();
      thread_.join();
      writer_.reset();
      return true;
    }

  private:
    std::thread thread_;
    std::shared_ptr<continuous_writer_t> writer_;
  };

  // Continuous recording state. `recording` is true exactly while `continuous_fd` is open and
  // `recording_file` names it; all three, and everything below, change only under `mutex`.
  static std::atomic<bool> recording {false};
  static std::string recording_file;
  static int continuous_fd = -1;
  // The open file's writer (null while no file is open); its thread is `continuous_thread`.
  static std::shared_ptr<continuous_writer_t> continuous_writer;
  // Start, stop and shutdown take this, never the packet path or status; with it held:
  static std::mutex continuous_control;
  static continuous_thread_t continuous_thread;  // the open file's writer thread
  static bool continuous_shut_down = false;      // shutdown() ran: no later start
  static std::size_t continuous_limit_bytes = k_continuous_queue_bytes;  // guarded by `mutex`
  static std::size_t continuous_limit_slots = k_continuous_queue_slots;
  // The open file's ownership. One file holds at most one owner generation in one codec.
  static continuous_status_t continuous;
  // The bound owner can never write again: it ended, or it violated the file's codec.
  static bool continuous_owner_fenced = false;
  // Arrival of the owner's last valid packet, to refuse a clock regression inside the file.
  static std::optional<std::chrono::steady_clock::time_point> continuous_last_arrival;

  // Active stream codec (0=H.264, 1=HEVC, 2=AV1). Prefer over conf hevc/av1_mode
  // flags, which only mean "allowed", not "what this session encodes".
  static std::atomic<int> active_video_format {-1};

  // Replay buffer state. Every field below is guarded by `mutex`.
  //
  // A run is the packets of one selected session in one codec, starting at a keyframe,
  // with nondecreasing arrival times. Anything that could join frames across a gap
  // (a dropped owner frame, a codec change, a clock regression, a lifecycle or config
  // transition) clears the run, and the next run begins only at a keyframe.
  static std::deque<buffered_packet_t> replay_buffer;
  static std::size_t replay_bytes = 0;
  static int replay_video_format = -1;
  static std::size_t replay_max_bytes = k_replay_max_payload_bytes;
  static std::size_t replay_max_packets = k_replay_max_packets;
  // Registered sessions: whether each is replay-eligible, and its codec when known (-1 when not).
  // Selection is derived only from this, for the replay buffer and the continuous file alike.
  struct registered_session_t {
    bool eligible = false;
    int video_format = -1;
    /// The session's keyframe request channel, if its registration supplied one. Kept for the
    /// session's lifetime: metadata updates never replace or clear it; session_ended retires it.
    std::shared_ptr<keyframe_channel_t> keyframe_channel;
  };
  static std::map<std::uint64_t, registered_session_t> replay_sessions;
  static std::uint64_t replay_selected = 0;
  static replay_stats_t replay_counters;
  static std::uint64_t replay_next_sequence = 1;

  // ---- Keyframe requests (guarded by `mutex` unless noted) ----
  //
  // Lock order: `mutex`, then a writer's `lock` (taken and released), then
  // `keyframe_request_gate`, then a channel's lock, then the session event's own lock. A request
  // is decided under `mutex` and raised only after `mutex` is released.

  struct keyframe_token_t {
    std::uint64_t incarnation = 0;
    std::uint64_t generation = 0;
    std::uint64_t sequence = 0;
    bool operator==(const keyframe_token_t &) const = default;
  };

  /// The open file's keyframe request state and counters, reset at each start commit.
  struct keyframe_file_t {
    std::uint64_t incarnation = 0;  ///< 0 while no file is open, or when incarnations ran out
    bool exceeds_queue = false;     ///< An owner keyframe larger than the whole queue was dropped
    std::optional<keyframe_token_t> in_flight;  ///< At most one reservation not yet completed
    std::optional<std::chrono::steady_clock::time_point> last_request_at;  ///< Rate anchor
    std::size_t last_keyframe_bytes = 0;  ///< Size of the last admitted owner keyframe
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
  };

  static keyframe_file_t keyframe_file;
  // Monotonic: never reset and never reused, so a token names one request of one file.
  static std::uint64_t continuous_incarnation = 0;
  static std::uint64_t keyframe_request_sequence = 0;
  // Process diagnostics: completions for a file no longer open, and refused registrations.
  static std::uint64_t keyframe_stale_completions_total = 0;
  static std::uint64_t keyframe_registration_conflicts = 0;
  // The one dispatch still allowed to raise (guarded by keyframe_request_gate).
  static std::mutex keyframe_request_gate;
  static std::optional<keyframe_token_t> keyframe_authorized;
  // Proposals, unmeasured: the shortest gap between requests, and the least free queue space
  // to ask for a keyframe when no owner keyframe size is known yet.
  constexpr auto k_keyframe_request_interval = std::chrono::seconds {1};
  constexpr std::size_t k_keyframe_request_floor_bytes = std::size_t {1024} * 1024;
#ifdef POLARIS_TESTS
  static detail::keyframe_hooks_t keyframe_hooks;
#endif

  /// A request decided under `mutex`, raised by keyframe_dispatch() after `mutex` is released.
  struct keyframe_pending_t {
    std::shared_ptr<keyframe_channel_t> channel;
    keyframe_token_t token;
#ifdef POLARIS_TESTS
    detail::keyframe_hooks_t hooks;
#endif
  };

  /// Withdraw the current request (requires `mutex`): no reservation stays held and no pending
  /// dispatch may still raise. The rate anchor is kept unless `reset_anchor`.
  static void keyframe_withdraw(bool reset_anchor) noexcept {
    keyframe_file.in_flight.reset();
    {
      std::lock_guard gate(keyframe_request_gate);
      keyframe_authorized.reset();
    }
    if (reset_anchor) {
      keyframe_file.last_request_at.reset();
    }
  }

  // The save in progress, as the recorder accounts for it (guarded by `mutex`). Its snapshot
  // holds the stored packets whose sequence lies in [first, last]; when the live run drops one
  // of those, its bytes stay alive only through the snapshot and count as snapshot-only.
  static bool snapshot_active = false;
  static std::uint64_t snapshot_first_sequence = 0;
  static std::uint64_t snapshot_last_sequence = 0;
  static std::size_t snapshot_packets = 0;
  static std::size_t snapshot_bytes = 0;
  static std::size_t snapshot_only_bytes = 0;
  // The admitted snapshot's window, fixed at admission (guarded by `mutex`).
  static std::size_t snapshot_keyframes = 0;
  static std::chrono::steady_clock::duration snapshot_span {0};

  // Window accounting for the live run (guarded by `mutex`), all kept incrementally.
  static std::size_t replay_run_keyframes = 0;
  // Arrival of the last keyframe the selected run accepted. Set only when a keyframe is stored,
  // kept by trimming and expiry, and forgotten by every reset of the run (clear_replay_run).
  static std::optional<std::chrono::steady_clock::time_point> replay_last_accepted_keyframe;
  static window_limit_e replay_last_limit = window_limit_e::none;

  // One save at a time. Taken with a compare-exchange, never waited on, so a request during a
  // save is refused at once and no packet, status or lifecycle call waits for it. Unlike a
  // mutex, the owner of an admitted save may release it from any thread.
  static std::atomic<bool> save_reserved {false};
  // Counted without any lock, so a busy refusal never waits on the recorder either.
  static std::atomic<std::uint64_t> saves_refused_busy {0};

#ifdef POLARIS_TESTS
  static detail::save_hooks_t save_hooks;
#endif

  /**
   * @brief Determine the file extension for a video codec.
   * Unknown formats fall back to conf allow-lists.
   */
  static std::string extension_for(int fmt) {
    if (fmt == 3) {
      return ".pyrowave";
    }
    if (fmt == 2) {
      return ".av1";
    }
    if (fmt == 1) {
      return ".hevc";
    }
    if (fmt == 0) {
      return ".h264";
    }
    // No active stream yet: conf modes only mean "allowed", not "in use".
    // Prefer H.264 when multiple are enabled so we do not mislabel.
    if (config::video.av1_mode > 0 && config::video.hevc_mode <= 0) {
      return ".av1";
    }
    if (config::video.hevc_mode > 0 && config::video.av1_mode <= 0) {
      return ".hevc";
    }
    return ".h264";
  }

  /**
   * @brief The codec a continuous file is named for when no selected session's codec is known:
   * the last stream codec seen, else the conf allow-lists (the same choice extension_for(-1) makes).
   */
  static int fallback_video_format() {
    const int fmt = active_video_format.load(std::memory_order_relaxed);
    if (fmt >= 0 && fmt <= 3) {
      return fmt;
    }
    if (config::video.av1_mode > 0 && config::video.hevc_mode <= 0) {
      return 2;
    }
    if (config::video.hevc_mode > 0 && config::video.av1_mode <= 0) {
      return 1;
    }
    return 0;
  }

  static bool known_video_format(int video_format) {
    return video_format >= 0 && video_format <= 3;
  }

  void set_active_video_format(int video_format) {
    if (video_format < 0 || video_format > 3) {
      return;
    }
    active_video_format.store(video_format, std::memory_order_relaxed);
  }

  // ---- Replay buffer (all helpers require `mutex`) ----

  static std::chrono::minutes replay_duration() {
    return std::chrono::minutes {std::clamp(cfg.replay_buffer_minutes, 1, 60)};
  }

  /// The live run is dropping `packet`: if a save's snapshot holds it, it now lives only there.
  static void note_live_release(const buffered_packet_t &packet) {
    if (snapshot_active && packet.sequence >= snapshot_first_sequence && packet.sequence <= snapshot_last_sequence) {
      snapshot_only_bytes += packet.data->size();
    }
  }

  /// Clear the run for `reason`. A lifecycle or config clear also forgets its codec. Every clear is
  /// a reset of the run, so it also forgets the accepted-keyframe marker.
  static void clear_replay_run(bool forget_codec, window_limit_e reason) {
    for (const auto &packet : replay_buffer) {
      note_live_release(packet);
    }
    replay_buffer.clear();
    replay_bytes = 0;
    replay_run_keyframes = 0;
    replay_last_accepted_keyframe.reset();
    replay_last_limit = reason;
    if (forget_codec) {
      replay_video_format = -1;
    }
  }

  static void pop_replay_front() {
    note_live_release(replay_buffer.front());
    replay_bytes -= replay_buffer.front().data->size();
    if (replay_buffer.front().is_keyframe) {
      --replay_run_keyframes;
    }
    replay_buffer.pop_front();
    ++replay_counters.trimmed;
  }

  /// Drop dependent frames left at the front, so the run starts at a keyframe again.
  static void align_replay_to_keyframe() {
    while (!replay_buffer.empty() && !replay_buffer.front().is_keyframe) {
      pop_replay_front();
    }
  }

  static bool replay_expired(const buffered_packet_t &packet, std::chrono::steady_clock::time_point now) {
    return now - packet.timestamp > replay_duration();
  }

  /// Drop what has expired at `now`, then align to the next keyframe.
  static void expire_replay(std::chrono::steady_clock::time_point now) {
    if (replay_buffer.empty() || !replay_expired(replay_buffer.front(), now)) {
      return;
    }
    while (!replay_buffer.empty() && replay_expired(replay_buffer.front(), now)) {
      pop_replay_front();
    }
    align_replay_to_keyframe();
    replay_last_limit = window_limit_e::duration;
  }

  /// Recompute the selected session after a lifecycle change; any change of selection clears the run.
  static void reselect_replay_session() {
    std::uint64_t selected = 0;
    if (replay_sessions.size() == 1 && replay_sessions.begin()->second.eligible) {
      selected = replay_sessions.begin()->first;
    }
    if (selected != replay_selected) {
      clear_replay_run(true, window_limit_e::selection);
      replay_selected = selected;
      // The selection is committed; a failed diagnostic cannot undo or interrupt it.
      try {
        BOOST_LOG(debug) << "stream_recorder: replay session "sv
                         << (selected ? std::to_string(selected) : std::string {"none"})
                         << " ("sv << replay_sessions.size() << " registered)"sv;
      } catch (...) {
      }
    }
  }

  // ---- Continuous file ownership (all helpers require `mutex`; none allocates) ----

  static void continuous_suspend(continuous_cause_e cause) {
    continuous.state = continuous_state_e::suspended;
    continuous.cause = cause;
    // Nothing is requested while suspended; the rate anchor is kept so flapping cannot burst.
    keyframe_withdraw(false);
  }

  /// The owner can never write to this file again.
  static void continuous_fence(continuous_cause_e cause) {
    continuous_owner_fenced = true;
    continuous_suspend(cause);
    keyframe_withdraw(true);
  }

  /// Follow a committed lifecycle change: which session (if any) may feed the open file now.
  static void continuous_follow_selection() {
    // A stopping file takes nothing more; a failed one is reported failed whatever this sets.
    if (continuous_fd < 0 || continuous.state == continuous_state_e::stopping) {
      return;
    }
    const auto owner = continuous.owner_generation;
    if (owner != 0) {
      if (continuous_owner_fenced) {
        return;
      }
      const auto it = replay_sessions.find(owner);
      if (it == replay_sessions.end()) {
        continuous_fence(continuous_cause_e::owner_ended);
        return;
      }
      if (known_video_format(it->second.video_format) && it->second.video_format != continuous.file_video_format) {
        continuous_fence(continuous_cause_e::codec);
        return;
      }
      if (replay_selected != owner) {
        // An overlap, or the owner made ineligible: nothing from anyone until it is sole again.
        continuous_suspend(continuous_cause_e::selection);
      } else if (continuous.state == continuous_state_e::suspended) {
        // The same owner is sole and eligible again; frames were missed, so restart at a keyframe.
        continuous.state = continuous_state_e::awaiting_keyframe;
      }
      return;
    }
    // Unbound: a selected eligible session of the file's codec may bind at its keyframe.
    if (replay_selected != 0) {
      const auto format = replay_sessions.find(replay_selected)->second.video_format;
      if (known_video_format(format) && format != continuous.file_video_format) {
        continuous_suspend(continuous_cause_e::codec);
        return;
      }
    }
    continuous.state = continuous_state_e::awaiting_owner;
    continuous.cause = continuous_cause_e::none;
  }

  /**
   * Decide one keyframe request for the open file's owner (requires `mutex`). Only a bound,
   * unfenced, sole eligible owner with a registered channel, while its file awaits a keyframe,
   * gets one reservation at a time, at most once per interval, and never into a writer that has
   * failed, is stopping or has no room. An unbound file (owner 0) never requests.
   */
  static keyframe_pending_t keyframe_reserve(std::chrono::steady_clock::time_point now) {
    keyframe_pending_t pending;
    if (!recording.load() || continuous_fd < 0 || !continuous_writer ||
        continuous.state == continuous_state_e::stopping || keyframe_file.incarnation == 0) {
      return pending;
    }
    const auto owner = continuous.owner_generation;
    if (owner == 0 || continuous_owner_fenced || replay_selected != owner) {
      return pending;
    }
    const auto it = replay_sessions.find(owner);  // Never inserts
    if (it == replay_sessions.end() || !it->second.keyframe_channel) {
      return pending;
    }
    auto &writer = *continuous_writer;
    {
      // A failed or stopping writer refuses every decision for the owner, before the awaiting and
      // rate checks: a failed file records no discontinuity, so its refusal must be counted here.
      std::lock_guard guard(writer.lock);
      if (writer.failed || writer.stop_requested) {
        ++keyframe_file.refused_writer_unavailable;
        return pending;
      }
    }
    if (continuous.state != continuous_state_e::awaiting_keyframe || keyframe_file.in_flight ||
        keyframe_file.exceeds_queue) {
      return pending;
    }
    if (keyframe_file.last_request_at &&
        !(now >= *keyframe_file.last_request_at && now - *keyframe_file.last_request_at >= k_keyframe_request_interval)) {
      // Too soon, or the clock went backwards: no retry, and the anchor stays where it was.
      ++keyframe_file.refused_rate;
      return pending;
    }
    {
      std::lock_guard guard(writer.lock);
      if (writer.failed || writer.stop_requested) {
        ++keyframe_file.refused_writer_unavailable;
        return pending;
      }
      if (writer.slot_used == writer.slot_capacity) {
        ++keyframe_file.refused_no_slot;
        return pending;
      }
      if (writer.next_sequence == std::numeric_limits<std::uint64_t>::max()) {
        ++keyframe_file.refused_queue_sequence_exhausted;
        return pending;
      }
      const auto ring = writer.ring_bytes;
      const auto last = keyframe_file.last_keyframe_bytes;
      const std::size_t want = last == 0 ? std::min(ring, k_keyframe_request_floor_bytes) :
                                           (last <= ring / 2 ? last * 2 : ring);
      if (ring - writer.byte_used < want) {
        ++keyframe_file.refused_bytes;
        return pending;
      }
    }
    if (keyframe_request_sequence == std::numeric_limits<std::uint64_t>::max()) {
      ++keyframe_file.refused_request_sequence_exhausted;
      return pending;
    }
    const keyframe_token_t token {keyframe_file.incarnation, owner, ++keyframe_request_sequence};
    keyframe_file.in_flight = token;
    keyframe_file.last_request_at = now;
    ++keyframe_file.reservations;
    {
      std::lock_guard gate(keyframe_request_gate);
      keyframe_authorized = token;
    }
    pending.channel = it->second.keyframe_channel;
    pending.token = token;
#ifdef POLARIS_TESTS
    try {
      pending.hooks = keyframe_hooks;
    } catch (...) {
      // Seams are optional; the request itself does not depend on them.
    }
#endif
    return pending;
  }

  /// Count a finished dispatch (requires `mutex`). Only the exact token releases its reservation;
  /// a completion for a file that is no longer open counts only as stale.
  static void keyframe_complete(const keyframe_token_t &token, keyframe_outcome_e outcome) {
    if (token.incarnation == 0 || token.incarnation != keyframe_file.incarnation) {
      ++keyframe_stale_completions_total;
      return;
    }
    switch (outcome) {
      case keyframe_outcome_e::attempted:
        ++keyframe_file.attempted;
        break;
      case keyframe_outcome_e::expired:
        ++keyframe_file.expired;
        break;
      case keyframe_outcome_e::stopped:
        ++keyframe_file.stopped;
        break;
      case keyframe_outcome_e::retired:
        ++keyframe_file.retired;
        break;
      case keyframe_outcome_e::fenced:
        ++keyframe_file.fenced;
        break;
    }
    if (keyframe_file.in_flight == token) {
      keyframe_file.in_flight.reset();
    }
  }

  /// A frame of the selected session was dropped: later frames would depend on it, so restart at a keyframe.
  static void replay_discontinuity(window_limit_e reason = window_limit_e::discontinuity) {
    clear_replay_run(false, reason);
    ++replay_counters.discontinuities;
  }

  /**
   * @brief The provenance record for an entry about to be stored (requires `mutex`).
   *
   * Called after this admission's expiry, trimming and keyframe alignment, just before the entry
   * is stored. Marks compare it, once, with the adjacent retained entry (the buffer's back); an
   * empty buffer means no comparison, and a coordinate unknown on either side is not compared.
   * The marks are fixed at admission. Nothing here allocates, throws or decides.
   */
  static detail::provenance_record_t replay_provenance_record(const packet_provenance_t *supplied) noexcept {
    detail::provenance_record_t record;
    if (supplied) {
      record.supplied = *supplied;
    }
    auto &marks = record.marks;
    const auto &index = record.supplied.frame_index;
    marks.index_unsupplied = !index;
    const auto *adjacent = replay_buffer.empty() ? nullptr : &replay_buffer.back().provenance.supplied;
    if (index && adjacent && adjacent->frame_index) {
      const auto previous = *adjacent->frame_index;
      marks.index_duplicate = *index == previous;
      marks.index_regressed = *index < previous;
      // The unsigned difference is exact whenever index > previous, so no signed overflow occurs
      // at either end of the int64 range.
      marks.index_gap = *index > previous &&
                        static_cast<std::uint64_t>(*index) - static_cast<std::uint64_t>(previous) > 1;
    }
    const auto &time = record.supplied.frame_timestamp;
    marks.time_missing = !time;
    if (time && adjacent && adjacent->frame_timestamp) {
      marks.time_duplicate = *time == *adjacent->frame_timestamp;
      marks.time_regressed = *time < *adjacent->frame_timestamp;
    }
    return record;
  }

  /**
   * @brief Admit one video packet to the replay buffer, or refuse it without copying.
   *
   * `provenance` (null for push_packet()) is recorded beside the entry and decides nothing.
   */
  static void append_replay_packet(const uint8_t *data, size_t size, bool is_keyframe, int64_t pts,
                                   std::uint64_t session_generation, int video_format,
                                   std::chrono::steady_clock::time_point now,
                                   const packet_provenance_t *provenance) {
    // Packets of any session but the selected one are refused before anything else, so a
    // foreign, retired, zero or malformed generation cannot change the selected run.
    if (session_generation == 0 || session_generation != replay_selected) {
      ++replay_counters.refused_unselected;
      return;
    }
    if (data == nullptr || size == 0 || video_format < 0 || video_format > 3) {
      ++replay_counters.refused_invalid;
      replay_discontinuity();
      return;
    }
    // A frame that cannot fit the caps (including a zero byte or packet budget) is never copied.
    if (size > replay_max_bytes || replay_max_packets == 0) {
      ++replay_counters.refused_oversized;
      replay_discontinuity();
      return;
    }
    if (!replay_buffer.empty() && now < replay_buffer.back().timestamp) {
      // Time must not run backwards inside a run.
      replay_discontinuity();
    }
    if (!replay_buffer.empty() && video_format != replay_video_format) {
      // A new codec cannot continue the old one's frames.
      replay_discontinuity(window_limit_e::codec);
    }
    expire_replay(now);

    // Make room under both caps. replay_bytes <= replay_max_bytes and size <= replay_max_bytes
    // hold here, so neither comparison can overflow.
    // When both caps call for room, the payload cap is the recorded cause (fixed precedence).
    const auto cap_reason = !replay_buffer.empty() && size > replay_max_bytes - replay_bytes ? window_limit_e::payload_cap :
                                                                                               window_limit_e::entry_cap;
    bool trimmed = false;
    while (!replay_buffer.empty() &&
           (size > replay_max_bytes - replay_bytes || replay_buffer.size() >= replay_max_packets)) {
      pop_replay_front();
      trimmed = true;
    }
    if (trimmed) {
      align_replay_to_keyframe();
      replay_last_limit = cap_reason;
    }

    // A run starts only at a keyframe: a dependent frame with nothing before it cannot decode.
    if (replay_buffer.empty() && !is_keyframe) {
      ++replay_counters.refused_awaiting_keyframe;
      return;
    }
    if (replay_buffer.empty()) {
      replay_video_format = video_format;
    }

    // The one copy of the packet's bytes, made immutable so a save can share it. Its provenance
    // is stored in the same entry, so the two are kept, trimmed and released together.
    const auto record = replay_provenance_record(provenance);
    replay_buffer.push_back(buffered_packet_t {
      std::make_shared<const std::vector<uint8_t>>(data, data + size),
      true,
      is_keyframe,
      pts,
      now,
      replay_next_sequence++,
      record,
    });
    replay_bytes += size;
    if (is_keyframe) {
      ++replay_run_keyframes;
      replay_last_accepted_keyframe = now;
    }
  }

  // ---- Public API ----

  void load_config() {
    std::lock_guard lock(mutex);

    const auto previous_mode = cfg.mode;
    const auto previous_minutes = cfg.replay_buffer_minutes;
    cfg.mode = config::recording.enabled ? (config::recording.replay_buffer
                                              ? mode_t::replay_buffer
                                              : mode_t::continuous)
                                         : mode_t::disabled;
    cfg.output_dir = config::recording.output_dir;
    cfg.replay_buffer_minutes = config::recording.replay_buffer_minutes;
    // Leaving replay, entering it, or changing its duration starts a new run. Registered
    // sessions stay, so replay enabled during an admitted stream applies to that stream.
    if (cfg.mode != mode_t::replay_buffer || previous_mode != mode_t::replay_buffer ||
        cfg.replay_buffer_minutes != previous_minutes) {
      clear_replay_run(true, window_limit_e::configuration);
    }
    mode_hint.store(cfg.mode, std::memory_order_relaxed);

    BOOST_LOG(info) << "stream_recorder: mode="sv
                    << (cfg.mode == mode_t::disabled    ? "disabled"sv :
                        cfg.mode == mode_t::continuous   ? "continuous"sv :
                                                           "replay_buffer"sv)
                    << " output_dir=["sv << cfg.output_dir << "]"sv
                    << " replay_minutes="sv << cfg.replay_buffer_minutes;
  }

  bool is_recording() {
    return recording.load();
  }

  // ---- Replay save: a snapshot under the lock, storage with no recorder lock ----

  namespace {
    /// Bounded attempts for unique staging and final names; only "already exists" moves on.
    constexpr int k_save_name_attempts = 100;

    /// What an admitted save captured under the lock. It owns its share of the payloads.
    struct replay_snapshot_t {
      std::vector<payload_t> payloads;
      /// Each payload's provenance, by position (#359). Reserved with the payloads at admission and
      /// freed with the lease, after its lock, like the payload array.
      std::vector<detail::provenance_record_t> provenance;
      std::size_t bytes = 0;
      int video_format = -1;
      std::uint64_t generation = 0;
      std::string output_dir;  ///< As configured when admitted; empty means the default directory
      std::chrono::system_clock::time_point wall_time;
      std::size_t keyframes = 0;  ///< Keyframes in the captured run, fixed at admission
      std::chrono::steady_clock::duration span {0};  ///< Its arrival span, fixed at admission
    };

    /**
     * Ends an admitted save on every path out of it, exceptions included: drops the snapshot's
     * payload references before taking the lock (so no payload is freed under it), then clears
     * the accounting and counts the outcome.
     */
    class snapshot_lease_t {
    public:
      snapshot_lease_t() = default;
      snapshot_lease_t(const snapshot_lease_t &) = delete;
      snapshot_lease_t &operator=(const snapshot_lease_t &) = delete;

      ~snapshot_lease_t() {
        if (!admitted) {
          return;
        }
        // Releasing references neither allocates nor throws; the pointer array itself is freed
        // when the lease is destroyed, after the lock below is released.
        snapshot.payloads.clear();
        std::lock_guard lock(mutex);
        snapshot_active = false;
        snapshot_first_sequence = 0;
        snapshot_last_sequence = 0;
        snapshot_packets = 0;
        snapshot_bytes = 0;
        snapshot_only_bytes = 0;
        snapshot_keyframes = 0;
        snapshot_span = std::chrono::steady_clock::duration::zero();
        ++(published ? replay_counters.saves_completed : replay_counters.saves_failed);
      }

      replay_snapshot_t snapshot;
      bool admitted = false;
      bool published = false;
    };

    long long native_write_impl(int fd, const std::uint8_t *data, std::size_t size) {
#ifdef _WIN32
      return _write(fd, data, static_cast<unsigned>(std::min<std::size_t>(size, INT_MAX)));
#else
      return ::write(fd, data, size);
#endif
    }

    int native_sync_impl(int fd) {
#ifdef _WIN32
      return _commit(fd);
#else
      return ::fsync(fd);
#endif
    }

    int native_close_impl(int fd) {
#ifdef _WIN32
      return _close(fd);
#else
      return ::close(fd);
#endif
    }

    // Native removal has no allocating path conversion, including in noexcept cleanup.
    int native_remove_impl(const fs::path &path) noexcept {
#ifdef _WIN32
      return _wunlink(path.c_str());
#else
      return ::unlink(path.c_str());
#endif
    }

    /// A hard link fails when the final name exists, so publication never replaces a file.
    /// A filesystem without hard links fails here, closed, rather than falling back to a rename.
    std::error_code native_publish_impl(const fs::path &staging, const fs::path &final_path) {
#ifdef _WIN32
      std::error_code ec;
      fs::create_hard_link(staging, final_path, ec);
      return ec;
#else
      // All allocations precede the no-replace side effect. A completed link cannot be
      // followed by a filesystem wrapper allocation turning it into a reported failure.
      if (::link(staging.c_str(), final_path.c_str()) == 0) {
        return {};
      }
      return {errno, std::generic_category()};
#endif
    }

    /// Create `path` only if absent. POSIX uses 0600; Windows inherits directory ACLs.
    int open_exclusive(const fs::path &path) {
#ifdef _WIN32
      int fd = -1;
      const errno_t error = _wsopen_s(&fd, path.c_str(), _O_WRONLY | _O_CREAT | _O_EXCL | _O_BINARY | _O_NOINHERIT,
                                      _SH_DENYRW, _S_IREAD | _S_IWRITE);
      if (error) {
        errno = error;
        return -1;
      }
      return fd;
#else
      int fd;
      do {
        fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, S_IRUSR | S_IWUSR);
      } while (fd < 0 && errno == EINTR);
      return fd;
#endif
    }

    long long save_write(int fd, const std::uint8_t *data, std::size_t size) {
#ifdef POLARIS_TESTS
      if (save_hooks.write) {
        return save_hooks.write(fd, data, size);
      }
#endif
      return native_write_impl(fd, data, size);
    }

    int save_sync(int fd) {
#ifdef POLARIS_TESTS
      if (save_hooks.sync) {
        return save_hooks.sync(fd);
      }
#endif
      return native_sync_impl(fd);
    }

    int save_close(int fd) {
#ifdef POLARIS_TESTS
      if (save_hooks.close) {
        return save_hooks.close(fd);
      }
#endif
      return native_close_impl(fd);
    }

    std::error_code save_publish(const fs::path &staging, const fs::path &final_path) {
#ifdef POLARIS_TESTS
      if (save_hooks.publish) {
        return save_hooks.publish(staging, final_path);
      }
#endif
      return native_publish_impl(staging, final_path);
    }

    std::string errno_message(int error) {
      return std::error_code(error, std::generic_category()).message();
    }

    std::string local_stamp(std::chrono::system_clock::time_point when) {
      const auto time = std::chrono::system_clock::to_time_t(when);
      std::tm local {};
#ifdef _WIN32
      localtime_s(&local, &time);
#else
      localtime_r(&time, &local);
#endif
      std::ostringstream stamp;
      stamp << std::put_time(&local, "%Y%m%d_%H%M%S");
      return stamp.str();
    }

    std::string wall_stamp(std::chrono::system_clock::time_point when) {
#ifdef POLARIS_TESTS
      if (save_hooks.name_stamp) {
        return save_hooks.name_stamp();
      }
#endif
      return local_stamp(when);
    }

    /// The staging file this save created. Closes it and removes it unless it was published.
    class staging_file_t {
    public:
      staging_file_t() = default;
      staging_file_t(const staging_file_t &) = delete;
      staging_file_t &operator=(const staging_file_t &) = delete;

      ~staging_file_t() {
        if (fd >= 0) {
          native_close_impl(fd);
        }
        if (!path.empty()) {
          native_remove_impl(path);
        }
      }

      fs::path path;  ///< Set only once this save created it exclusively
      int fd = -1;
    };

    /**
     * Write the snapshot to a private staging file in its captured directory, check every
     * write, the sync and the close, then publish it under a final name that did not exist.
     * Runs with no recorder lock and reads no configuration.
     */
    bool store_snapshot(const replay_snapshot_t &snapshot, detail::save_result_t &result, bool &staging_left) {
      const auto fail = [&result](std::string what) {
        result.status = detail::save_status_e::storage_error;
        result.error = std::move(what);
        return false;
      };

      const fs::path dir = snapshot.output_dir.empty() ? platf::appdata() / "recordings" : fs::path {snapshot.output_dir};
      std::error_code ec;
      fs::create_directories(dir, ec);
      if (ec) {
        return fail("output directory [" + dir.string() + "]: " + ec.message());
      }
      if (!fs::is_directory(dir, ec)) {
        return fail("output directory [" + dir.string() + "] is not a directory");
      }

      const std::string stem = "polaris_" + wall_stamp(snapshot.wall_time) + "_replay";
      const std::string ext = extension_for(snapshot.video_format);

      staging_file_t staging;
      for (int attempt = 0; attempt < k_save_name_attempts; ++attempt) {
        auto candidate = dir / ("." + stem + ext + ".staging-" + std::to_string(attempt));
        const int fd = open_exclusive(candidate);
        if (fd >= 0) {
          staging.fd = fd;
          // Moving a preallocated path cannot orphan the exclusive file through a copy failure.
          staging.path = std::move(candidate);
          break;
        }
        const int error = errno;
        if (error != EEXIST) {
          return fail("staging [" + candidate.string() + "]: " + errno_message(error));
        }
      }
      if (staging.fd < 0) {
        return fail("staging: every candidate name already exists");
      }

#ifdef POLARIS_TESTS
      if (save_hooks.before_write) {
        save_hooks.before_write(staging.path);
      }
#endif

      for (const auto &payload : snapshot.payloads) {
        const std::uint8_t *data = payload->data();
        std::size_t left = payload->size();
        while (left > 0) {
          const auto written = save_write(staging.fd, data, left);
          if (written < 0) {
            const int error = errno;
            if (error == EINTR) {
              continue;
            }
            return fail("write: " + errno_message(error));
          }
          if (written == 0 || static_cast<unsigned long long>(written) > left) {
            return fail("write made no valid progress");
          }
          data += written;
          left -= static_cast<std::size_t>(written);
        }
      }

      int synced;
      do {
        synced = save_sync(staging.fd);
      } while (synced != 0 && errno == EINTR);
      if (synced != 0) {
        return fail("sync: " + errno_message(errno));
      }

      // Whatever close returns, the descriptor is released and is not closed again.
      const int fd = std::exchange(staging.fd, -1);
      if (save_close(fd) != 0) {
        return fail("close: " + errno_message(errno));
      }

      const auto first_final = dir / (stem + ext);
#ifdef POLARIS_TESTS
      if (save_hooks.before_publish) {
        save_hooks.before_publish(staging.path, first_final);
      }
#endif

      for (int attempt = 0; attempt < k_save_name_attempts; ++attempt) {
        const auto final_path = attempt == 0 ? first_final : dir / (stem + "-" + std::to_string(attempt) + ext);
        // Everything the success path needs is allocated before publishing, so nothing after a
        // successful publication can throw and turn a published file into a reported failure.
        std::string final_string = final_path.string();
        const auto error = save_publish(staging.path, final_path);
        if (!error) {
          // Published; the staging name is only a second link to the same complete file now.
          staging_left = native_remove_impl(staging.path) != 0;
          staging.path.clear();
          result.status = detail::save_status_e::saved;
          result.path = std::move(final_string);
          return true;
        }
        if (error != std::errc::file_exists) {
          return fail("publish [" + final_path.string() + "]: " + error.message());
        }
      }
      return fail("publish: every candidate name already exists");
    }
  }  // namespace

  // ---- Keyframe request channel ----

  keyframe_channel_t::keyframe_channel_t(std::weak_ptr<event_post_t> event) noexcept:
      event_ {std::move(event)} {}

  keyframe_outcome_e keyframe_channel_t::request() noexcept {
    return request(nullptr);
  }

  keyframe_outcome_e keyframe_channel_t::request(const std::function<void()> *after_raise) noexcept {
    std::lock_guard guard(lock_);
    if (retired_) {
      return keyframe_outcome_e::retired;
    }
    // A strong reference only for this call: post_t inherits event_t, so the arrow reaches
    // running() and raise() exactly as production's idr_events->raise(true) does.
    const auto event = event_.lock();
    if (!event) {
      return keyframe_outcome_e::expired;
    }
    if (!event->running()) {
      return keyframe_outcome_e::stopped;
    }
    event->raise(true);  // Returns nothing; a stop() just before it is silently ignored
    if (after_raise && *after_raise) {
      (*after_raise)();
    }
    return keyframe_outcome_e::attempted;
  }

  void keyframe_channel_t::retire() noexcept {
    std::lock_guard guard(lock_);
    retired_ = true;
    event_.reset();
  }

  /// Lets the recorder pass its test-only raise observer without making it public API.
  struct keyframe_channel_access_t {
    static keyframe_outcome_e request(keyframe_channel_t &channel, const std::function<void()> *after_raise) noexcept {
      return channel.request(after_raise);
    }
  };

  // ---- Continuous file: exclusive creation, a bounded queue, one writer thread per file ----

  namespace {
    std::string continuous_stamp([[maybe_unused]] const continuous_writer_t &writer) {
#ifdef POLARIS_TESTS
      if (writer.hooks.name_stamp) {
        return writer.hooks.name_stamp();
      }
#endif
      return local_stamp(std::chrono::system_clock::now());
    }

    int continuous_open([[maybe_unused]] const continuous_writer_t &writer, const fs::path &path) {
#ifdef POLARIS_TESTS
      if (writer.hooks.open) {
        return writer.hooks.open(path);
      }
#endif
      return open_exclusive(path);
    }

    long long continuous_write_call(const continuous_writer_t &writer, const std::uint8_t *data, std::size_t size) {
#ifdef POLARIS_TESTS
      if (writer.hooks.write) {
        return writer.hooks.write(writer.fd, data, size);
      }
#endif
      return native_write_impl(writer.fd, data, size);
    }

    int continuous_close_call([[maybe_unused]] const continuous_writer_t &writer, int fd) {
#ifdef POLARIS_TESTS
      if (writer.hooks.close) {
        return writer.hooks.close(fd);
      }
#endif
      return native_close_impl(fd);
    }

    /// Run a test seam if set (POLARIS_TESTS builds only).
#ifdef POLARIS_TESTS
  #define CONTINUOUS_SEAM(hooks, name) \
    do { \
      if ((hooks).name) { \
        (hooks).name(); \
      } \
    } while (false)
#else
  #define CONTINUOUS_SEAM(hooks, name) \
    do { \
    } while (false)
#endif

    /**
     * A file this start created exclusively: closed once and removed unless the recording takes
     * it. It holds the start's writer only to reach its copied test seams; the writer itself
     * never owns or closes the descriptor.
     */
    class created_file_t {
    public:
      created_file_t(int fd, const fs::path &path, std::shared_ptr<continuous_writer_t> writer) noexcept:
          fd_ {fd}, path_ {&path}, writer_ {std::move(writer)} {}

      created_file_t(const created_file_t &) = delete;
      created_file_t &operator=(const created_file_t &) = delete;

      ~created_file_t() {
        if (fd_ >= 0) {
#ifdef POLARIS_TESTS
          if (writer_ && writer_->hooks.rollback_close) {
            writer_->hooks.rollback_close(fd_);  // Documented not to throw
          } else {
            native_close_impl(fd_);
          }
#else
          native_close_impl(fd_);
#endif
          native_remove_impl(*path_);
        }
      }

      /// The recording owns the descriptor and the name from here on.
      int release() noexcept {
        return std::exchange(fd_, -1);
      }

    private:
      int fd_;
      const fs::path *path_;
      std::shared_ptr<continuous_writer_t> writer_;
    };

    /// Forget the file's ownership and accounting (requires `mutex`; does not allocate).
    void continuous_reset_locked() noexcept {
      continuous = continuous_status_t {};
      continuous_owner_fenced = false;
      continuous_last_arrival.reset();
    }

    /**
     * Write one segment, retrying interrupted and partial writes. Every byte write() accepts is
     * published to `bytes_written` at once, under the writer's lock, and also counted in `written`
     * (this packet's accepted prefix). On the first error it returns false with `error` and
     * `error_number` set. Writer thread only.
     */
    bool continuous_write_segment(continuous_writer_t &writer, const std::uint8_t *data, std::size_t size,
                                  std::uint64_t &written, continuous_error_e &error, int &error_number) {
      std::size_t left = size;
      while (left > 0) {
        const auto result = continuous_write_call(writer, data, left);
        if (result < 0) {
          const int write_error = errno;
          if (write_error == EINTR) {
            continue;
          }
          error = continuous_error_e::write;
          error_number = write_error;
          return false;
        }
        if (result == 0 || static_cast<unsigned long long>(result) > left) {
          error = continuous_error_e::no_progress;
          error_number = 0;
          return false;
        }
        data += result;
        left -= static_cast<std::size_t>(result);
        written += static_cast<std::uint64_t>(result);
        std::lock_guard guard(writer.lock);
        writer.bytes_written += static_cast<std::uint64_t>(result);
      }
      return true;
    }

    /**
     * The writer thread: writes the queue in order, then exits once stop is requested and the
     * queue is empty. It touches only its writer (never a recorder global, lock or log). After
     * the first failure it discards what remains, so every admitted packet still completes.
     */
    void continuous_writer_main(std::shared_ptr<continuous_writer_t> owned) noexcept {
      auto &writer = *owned;
      try {
        CONTINUOUS_SEAM(writer.hooks, worker_started);
      } catch (...) {
      }
      std::unique_lock guard(writer.lock);
      for (;;) {
        writer.changed.wait(guard, [&writer] {
          return writer.slot_used > 0 || writer.stop_requested;
        });
        if (writer.slot_used == 0) {
          break;  // Stop requested, and every admitted packet is finished.
        }
        const auto slot = writer.slots[writer.slot_head];
        const bool discard = writer.failed;
        guard.unlock();

        std::uint64_t written = 0;
        bool ok = true;
        auto error = continuous_error_e::none;
        int error_number = 0;
        if (!discard) {
          // The packet may wrap: its tail continues at the start of the ring.
          const auto first = std::min(slot.size, writer.ring_bytes - slot.offset);
          try {
            ok = continuous_write_segment(writer, writer.ring.get() + slot.offset, first, written, error, error_number) &&
                 continuous_write_segment(writer, writer.ring.get(), slot.size - first, written, error, error_number);
          } catch (...) {
            ok = false;
            error = continuous_error_e::write;
            error_number = 0;
          }
        }

        guard.lock();
        // One critical section publishes the outcome, the failure and the freed space together.
        const bool newly_failed = !discard && !ok;
        if (ok && !discard) {
          ++writer.packets_written;
        } else {
          ++writer.discarded_packets;
          writer.discarded_bytes += slot.size - written;
        }
        if (newly_failed) {
          writer.failed = true;
          writer.error = error;
          writer.error_number = error_number;
        }
        writer.byte_head += slot.size;
        if (writer.byte_head >= writer.ring_bytes) {
          writer.byte_head -= writer.ring_bytes;
        }
        writer.byte_used -= slot.size;
        writer.slot_head = writer.slot_head + 1 == writer.slot_capacity ? 0 : writer.slot_head + 1;
        --writer.slot_used;
        writer.completed_sequence = slot.sequence;
        guard.unlock();
        writer.changed.notify_all();
        if (newly_failed) {
          try {
            CONTINUOUS_SEAM(writer.hooks, after_worker_failure);
          } catch (...) {
          }
        }
        guard.lock();
      }
      guard.unlock();
      try {
        CONTINUOUS_SEAM(writer.hooks, worker_exited);
      } catch (...) {
      }
    }

    /// The owner's frame was dropped: later frames would depend on it, so restart at a keyframe.
    void continuous_discontinuity(continuous_cause_e cause = continuous_cause_e::discontinuity) {
      ++continuous.discontinuities;
      continuous.state = continuous_state_e::awaiting_keyframe;
      continuous.cause = cause;
    }

    /**
     * Offer one video packet to the open file (requires `mutex`; never waits for storage and
     * makes no filesystem call). Only the sole eligible session may feed it: packets of a zero,
     * unregistered, retired or other generation return before anything is read or counted. Size
     * and codec are checked before the bytes are read. An admitted packet is copied into the
     * file's preallocated queue; one that does not fit is dropped, and the file waits for a
     * keyframe that fits. Returns the admitted packet's sequence, or 0 when nothing was admitted.
     */
    std::uint64_t continuous_offer(const uint8_t *data, size_t size, bool is_keyframe, std::uint64_t generation,
                                   int video_format, std::chrono::steady_clock::time_point now) {
      if (continuous_fd < 0 || !continuous_writer || generation == 0 || generation != replay_selected ||
          continuous.state == continuous_state_e::stopping) {
        return 0;
      }
      auto &writer = *continuous_writer;
      {
        // A failed or stopping file takes nothing more.
        std::lock_guard guard(writer.lock);
        if (writer.failed || writer.stop_requested) {
          return 0;
        }
      }
      const bool invalid = data == nullptr || size == 0 || size > k_continuous_max_packet_bytes ||
                           !known_video_format(video_format);
      if (continuous.owner_generation == 0) {
        // Unbound: the sole eligible session binds at a keyframe of the file's codec.
        if (invalid) {
          ++continuous.refused_invalid;
          return 0;
        }
        if (video_format != continuous.file_video_format) {
          ++continuous.refused_codec;
          continuous_suspend(continuous_cause_e::codec);
          return 0;
        }
        if (!is_keyframe) {
          ++continuous.refused_awaiting_keyframe;
          return 0;
        }
        continuous.owner_generation = generation;
        continuous.state = continuous_state_e::awaiting_keyframe;
      } else {
        if (generation != continuous.owner_generation || continuous_owner_fenced) {
          return 0;
        }
        if (invalid) {
          ++continuous.refused_invalid;
          continuous_discontinuity();
          return 0;
        }
        if (video_format != continuous.file_video_format) {
          // One file is one codec: the owner can never continue it.
          ++continuous.refused_codec;
          continuous_fence(continuous_cause_e::codec);
          return 0;
        }
        if (continuous_last_arrival && now < *continuous_last_arrival) {
          // Time must not run backwards inside the file: drop this packet unread, take its time
          // as the new reference, and restart at the next keyframe.
          continuous_last_arrival = now;
          continuous_discontinuity();
          return 0;
        }
      }
      continuous_last_arrival = now;
      if (continuous.state != continuous_state_e::writing && !is_keyframe) {
        ++continuous.refused_awaiting_keyframe;
        return 0;
      }

      // Place it: free space is checked by subtraction from fixed capacities, with the packet
      // being written still counted. The writer only frees space, so the place stays free.
      std::size_t offset = 0;
      std::size_t slot = 0;
      bool fits;
      {
        std::lock_guard guard(writer.lock);
        fits = size <= writer.ring_bytes - writer.byte_used && writer.slot_used < writer.slot_capacity &&
               writer.next_sequence != std::numeric_limits<std::uint64_t>::max();
        if (fits) {
          // byte_head < ring_bytes and byte_used <= ring_bytes, so the sum cannot overflow.
          offset = writer.byte_head + writer.byte_used;
          if (offset >= writer.ring_bytes) {
            offset -= writer.ring_bytes;
          }
          slot = writer.slot_head + writer.slot_used;
          if (slot >= writer.slot_capacity) {
            slot -= writer.slot_capacity;
          }
        }
      }
      if (!fits) {
        // Dropped whole, unread: dependent frames wait for a keyframe that fits.
        ++continuous.dropped_backpressure;
        continuous_discontinuity(continuous_cause_e::backpressure);
        if (is_keyframe && size > writer.ring_bytes) {
          // No keyframe of this size can ever fit: asking for another would only repeat this.
          keyframe_file.exceeds_queue = true;
        }
        return 0;
      }

      // Copy outside the writer's lock: the writer cannot see this place until it is published.
      const auto first = std::min(size, writer.ring_bytes - offset);
      std::memcpy(writer.ring.get() + offset, data, first);
      std::memcpy(writer.ring.get(), data + first, size - first);
      CONTINUOUS_SEAM(writer.hooks, before_publish);

      std::uint64_t sequence;
      {
        std::lock_guard guard(writer.lock);
        if (writer.failed || writer.stop_requested) {
          // The file failed (or began stopping) while this was copied: leave it unpublished.
          return 0;
        }
        sequence = writer.next_sequence++;
        writer.slots[slot] = continuous_writer_t::slot_t {offset, size, sequence};
        writer.byte_used += size;
        ++writer.slot_used;
        ++writer.packets_admitted;
        writer.bytes_admitted += size;
      }
      writer.changed.notify_all();
      continuous.state = continuous_state_e::writing;
      if (is_keyframe) {
        // An admitted owner keyframe is the only recovery: any request is withdrawn and the
        // rate anchor cleared. Its size sizes the room asked for before the next request.
        keyframe_file.last_keyframe_bytes = size;
        keyframe_withdraw(true);
      }
      return sequence;
    }

    /**
     * Raise a decided request (no recorder lock held), then count how it ended. Every caller
     * holds `mutex` only in a scope that has ended before this call: push_packet_core()'s
     * lock_guard (destroyed at its return), the braced lock_guard blocks of session_started()
     * and session_ended(), and start_recording()'s explicit unlock.
     */
    void keyframe_dispatch(keyframe_pending_t pending) {
      if (!pending.channel) {
        return;
      }
      const std::function<void()> *after_raise = nullptr;
#ifdef POLARIS_TESTS
      if (pending.hooks.before_dispatch) {
        pending.hooks.before_dispatch(detail::keyframe_token_t {pending.token.incarnation, pending.token.generation, pending.token.sequence});
      }
      if (pending.hooks.after_raise) {
        after_raise = &pending.hooks.after_raise;
      }
#endif
      keyframe_outcome_e outcome;
      {
        std::lock_guard gate(keyframe_request_gate);
        if (keyframe_authorized != pending.token) {
          outcome = keyframe_outcome_e::fenced;
        } else {
          keyframe_authorized.reset();  // One raise per reservation
          outcome = keyframe_channel_access_t::request(*pending.channel, after_raise);
        }
      }
      {
        std::lock_guard lock(mutex);
        keyframe_complete(pending.token, outcome);
      }
#ifdef POLARIS_TESTS
      // Observers see completed bookkeeping, with both the recorder lock and request gate released.
      if (pending.hooks.after_dispatch) {
        pending.hooks.after_dispatch(detail::keyframe_token_t {pending.token.incarnation, pending.token.generation, pending.token.sequence}, outcome);
      }
#endif
    }

    /**
     * Stop the open file (requires `continuous_control`): refuse new packets, let the writer
     * finish every admitted packet, join it with no lock held, publish idle, then close once.
     * The join has no time limit.
     */
    void stop_recording_locked() {
      std::shared_ptr<continuous_writer_t> writer;
      {
        std::lock_guard guard(mutex);
        if (recording.load()) {
          // Still reported as recording, with its file, until the writer has finished.
          continuous.state = continuous_state_e::stopping;
          writer = continuous_writer;
          keyframe_withdraw(true);
        }
      }
      if (!writer) {
        try {
          BOOST_LOG(warning) << "stream_recorder: not recording"sv;
        } catch (...) {
        }
        return;
      }
      {
        std::lock_guard guard(writer->lock);
        writer->stop_requested = true;
      }
      writer->changed.notify_all();
      CONTINUOUS_SEAM(writer->hooks, before_stop_join);
      continuous_thread.finish();

      int fd;
      std::string file;
      {
        std::lock_guard guard(mutex);
        // Release the descriptor, the name and the ownership before anything can fail.
        fd = std::exchange(continuous_fd, -1);
        file = std::move(recording_file);
        recording_file.clear();
        continuous_writer.reset();
        continuous_reset_locked();
        recording.store(false);
        // The file is closed: a later completion for it counts only as stale. Its counters stay
        // readable until the next start.
        keyframe_withdraw(true);
        keyframe_file.incarnation = 0;
      }
      // The descriptor is closed exactly once, after its only writer was joined, whatever close
      // reports, with no recorder lock held.
      const bool closed = fd < 0 || continuous_close_call(*writer, fd) == 0;
      try {
        if (!closed) {
          BOOST_LOG(warning) << "stream_recorder: stopped recording ["sv << file << "]; close reported an error"sv;
        } else {
          BOOST_LOG(info) << "stream_recorder: stopped recording ["sv << file << "]"sv;
        }
      } catch (...) {
      }
    }
  }  // namespace

  void start_recording() {
    std::lock_guard control(continuous_control);

    if (continuous_shut_down || recording.load()) {
      try {
        BOOST_LOG(warning) << (continuous_shut_down ? "stream_recorder: shutting down; recording not started"sv :
                                                      "stream_recorder: already recording"sv);
      } catch (...) {
      }
      return;
    }

    try {
      // Decide under the recorder lock; allocate, create and open without it.
      std::uint64_t owner = 0;
      int format = -1;
      std::string dir;
      std::size_t limit_bytes;
      std::size_t limit_slots;
#ifdef POLARIS_TESTS
      detail::continuous_hooks_t seams;
#endif
      {
        std::lock_guard guard(mutex);
        if (replay_selected != 0) {
          const int selected_format = replay_sessions.find(replay_selected)->second.video_format;
          if (known_video_format(selected_format)) {
            owner = replay_selected;
            format = selected_format;
          }
        }
        dir = cfg.output_dir.empty() ? (platf::appdata() / "recordings").string() : cfg.output_dir;
        limit_bytes = continuous_limit_bytes;
        limit_slots = continuous_limit_slots;
#ifdef POLARIS_TESTS
        seams = continuous_hooks;
#endif
      }
      if (format < 0) {
        format = fallback_video_format();
      }

      CONTINUOUS_SEAM(seams, before_block_allocation);
      auto writer = std::make_shared<continuous_writer_t>(limit_bytes, limit_slots);
#ifdef POLARIS_TESTS
      writer->hooks = std::move(seams);
#endif

      std::error_code ec;
      fs::create_directories(dir, ec);
      if (ec) {
        BOOST_LOG(warning) << "stream_recorder: failed to create output directory ["sv << dir << "]: "sv << ec.message();
      }
      const std::string stem = dir + "/polaris_" + continuous_stamp(*writer);
      const std::string ext = extension_for(format);

      for (int attempt = 0; attempt < k_save_name_attempts; ++attempt) {
        std::string name = attempt == 0 ? stem + ext : stem + "-" + std::to_string(attempt) + ext;
        const fs::path path {name};
#ifdef POLARIS_TESTS
        if (writer->hooks.before_open) {
          writer->hooks.before_open(path);
        }
#endif
        const int fd = continuous_open(*writer, path);
        if (fd < 0) {
          const int open_error = errno;
          if (open_error == EEXIST) {
            continue;
          }
          BOOST_LOG(error) << "stream_recorder: failed to open file ["sv << name << "]: "sv << errno_message(open_error);
          return;
        }
        // Until the commit below, this guard alone owns the descriptor and the file.
        created_file_t created {fd, path, writer};
#ifdef POLARIS_TESTS
        if (writer->hooks.after_open) {
          writer->hooks.after_open(path);
        }
#endif
        writer->fd = fd;  // The value only: the writer never closes it.

        CONTINUOUS_SEAM(writer->hooks, before_launch);
        // Declared after `created`, so on any exception from here the writer is stopped, woken
        // and joined first (nothing was published to it), and only then is the file closed and
        // removed, once.
        continuous_thread_t launched {std::thread {continuous_writer_main, writer}, writer};
        CONTINUOUS_SEAM(writer->hooks, after_launch);

        std::unique_lock guard(mutex);
        // Commit. Nothing from here allocates or throws, so neither the file nor the thread can be lost.
        continuous_thread = std::move(launched);
        continuous_writer = writer;
        continuous_fd = created.release();
        recording_file = std::move(name);
        continuous_reset_locked();
        // A new file: a never-reused incarnation (0 once exhausted: no requests) and fresh counters.
        keyframe_file = keyframe_file_t {};
        if (continuous_incarnation < std::numeric_limits<std::uint64_t>::max()) {
          keyframe_file.incarnation = ++continuous_incarnation;
        }
        keyframe_withdraw(true);
        continuous.file_video_format = format;
        continuous.owner_generation = owner;
        continuous.state = owner != 0 ? continuous_state_e::awaiting_keyframe : continuous_state_e::awaiting_owner;
        // The selection may have changed since it was read: follow it, as for any later change.
        continuous_follow_selection();
        recording.store(true);
        // A file bound at start asks its owner for a first keyframe; an unbound one never asks.
        keyframe_pending_t pending;
        try {
          pending = keyframe_reserve(std::chrono::steady_clock::now());
        } catch (...) {
        }
        guard.unlock();
        try {
          BOOST_LOG(info) << "stream_recorder: started recording to ["sv << current_file() << "]"sv;
        } catch (...) {
        }
        keyframe_dispatch(std::move(pending));
        return;
      }
      BOOST_LOG(error) << "stream_recorder: failed to open a file: every candidate name already exists"sv;
    } catch (...) {
      // Nothing was committed: a launched writer was joined, then a created file was closed and
      // removed, by their guards.
      try {
        BOOST_LOG(error) << "stream_recorder: could not start recording"sv;
      } catch (...) {
      }
    }
  }

  void stop_recording() {
#ifdef POLARIS_TESTS
    detail::continuous_hooks_t seams;
    {
      std::lock_guard guard(mutex);
      seams = continuous_hooks;
    }
    CONTINUOUS_SEAM(seams, stop_entered);
#endif
    std::lock_guard control(continuous_control);
    stop_recording_locked();
  }

  void shutdown() {
    std::lock_guard control(continuous_control);
    continuous_shut_down = true;
    if (recording.load()) {
      stop_recording_locked();
    }
  }

  std::string save_replay() {
    return detail::save_replay_at(std::chrono::steady_clock::now());
  }

  detail::save_result_t save_replay_result() {
    return detail::save_replay_result_at(std::chrono::steady_clock::now());
  }

  std::string detail::save_replay_at(std::chrono::steady_clock::time_point now) {
    auto result = save_replay_result_at(now);
    return result.status == save_status_e::saved ? std::move(result.path) : std::string {};
  }

  namespace {
    /// Holds the save reservation taken by admission and releases it once, from any thread.
    class save_reservation_t {
    public:
      save_reservation_t() noexcept = default;

      static save_reservation_t taken() noexcept {
        save_reservation_t reservation;
        reservation.held_ = true;
        return reservation;
      }

      save_reservation_t(save_reservation_t &&other) noexcept:
          held_ {std::exchange(other.held_, false)} {}

      save_reservation_t &operator=(save_reservation_t &&other) noexcept {
        if (this != &other) {
          release();
          held_ = std::exchange(other.held_, false);
        }
        return *this;
      }

      save_reservation_t(const save_reservation_t &) = delete;
      save_reservation_t &operator=(const save_reservation_t &) = delete;

      ~save_reservation_t() {
        release();
      }

    private:
      void release() noexcept {
        if (!std::exchange(held_, false)) {
          return;
        }
#ifdef POLARIS_TESTS
        if (save_hooks.before_save_reservation_release) {
          save_hooks.before_save_reservation_release();
        }
#endif
        save_reserved.store(false, std::memory_order_release);
      }

      bool held_ = false;
    };
  }  // namespace

  /// What an admitted save owns. Members are destroyed in reverse order: the lease (payload
  /// references, then accounting under the recorder lock) before the reservation.
  struct detail::admitted_save_t::state_t {
    save_reservation_t reservation;
    snapshot_lease_t lease;
  };

  detail::admitted_save_t::admitted_save_t() noexcept = default;
  detail::admitted_save_t::admitted_save_t(admitted_save_t &&other) noexcept = default;
  detail::admitted_save_t &detail::admitted_save_t::operator=(admitted_save_t &&other) noexcept = default;
  detail::admitted_save_t::~admitted_save_t() = default;

  detail::admitted_save_t::admitted_save_t(std::unique_ptr<state_t> state) noexcept:
      state_ {std::move(state)} {}

  detail::admitted_save_t::operator bool() const noexcept {
    return state_ != nullptr;
  }

  detail::save_admission_t detail::admit_replay_save(std::chrono::steady_clock::time_point now) {
    save_admission_t admission;
    auto &result = admission.result;
    bool expected = false;
    if (!save_reserved.compare_exchange_strong(expected, true, std::memory_order_acq_rel)) {
      // Refused at once: no recorder lock, no waiting on the running save.
      saves_refused_busy.fetch_add(1, std::memory_order_relaxed);
      result.status = save_status_e::busy;
      BOOST_LOG(warning) << "stream_recorder: a replay save is already running"sv;
      return admission;
    }
    // Owns the reservation on every path out of admission until an admitted state takes it.
    auto reservation = save_reservation_t::taken();

    // Declared after the reservation and outside the locked scope: on any refusal or failure
    // the state is destroyed after the recorder lock is released, and before the reservation.
    std::unique_ptr<admitted_save_t::state_t> state;
    try {
      std::lock_guard lock(mutex);
      if (cfg.mode != mode_t::replay_buffer) {
        result.status = save_status_e::disabled;
      } else {
        // Nothing older than the duration is saved, even when no packet arrived since.
        expire_replay(now);
        if (replay_buffer.empty() || !replay_buffer.front().is_keyframe || replay_video_format < 0 ||
            replay_video_format > 3) {
          result.status = save_status_e::empty;
        } else {
          // An eligible run: allocate its capture state, then capture, before any accounting.
#ifdef POLARIS_TESTS
          if (save_hooks.before_admission_allocation) {
            save_hooks.before_admission_allocation();
          }
#endif
          state = std::make_unique<admitted_save_t::state_t>();
          // Pointers and metadata only: the payload bytes are shared, not copied.
          auto &snapshot = state->lease.snapshot;
          snapshot.payloads.reserve(replay_buffer.size());
#ifdef POLARIS_TESTS
          if (save_hooks.before_snapshot_provenance_allocation) {
            save_hooks.before_snapshot_provenance_allocation();
          }
#endif
          snapshot.provenance.reserve(replay_buffer.size());
          for (const auto &packet : replay_buffer) {
            snapshot.payloads.push_back(packet.data);
            snapshot.provenance.push_back(packet.provenance);
          }
#ifdef POLARIS_TESTS
          if (save_hooks.after_snapshot_payloads) {
            save_hooks.after_snapshot_payloads();
          }
#endif
          snapshot.output_dir = cfg.output_dir;
          snapshot.bytes = replay_bytes;
          snapshot.video_format = replay_video_format;
          snapshot.generation = replay_selected;
          snapshot.wall_time = std::chrono::system_clock::now();
          // From here nothing throws: the accounting becomes active with its owner in place.
          snapshot_active = true;
          snapshot_first_sequence = replay_buffer.front().sequence;
          snapshot_last_sequence = replay_buffer.back().sequence;
          snapshot_packets = replay_buffer.size();
          snapshot_bytes = replay_bytes;
          snapshot_only_bytes = 0;
          snapshot_keyframes = replay_run_keyframes;
          snapshot_span = replay_buffer.back().timestamp - replay_buffer.front().timestamp;
          snapshot.keyframes = snapshot_keyframes;
          snapshot.span = snapshot_span;
          state->lease.admitted = true;
          result.admitted = true;
          result.packets = snapshot.payloads.size();
          result.bytes = snapshot.bytes;
          result.video_format = snapshot.video_format;
          result.generation = snapshot.generation;
          result.keyframes = snapshot.keyframes;
          result.span = snapshot.span;
        }
      }
    } catch (...) {
      // Every throwing step precedes the accounting, so nothing was admitted. The lock is
      // released by unwinding; the unadmitted state, then the reservation, release here.
      result = save_result_t {};
      result.status = save_status_e::storage_error;
      return admission;
    }
    if (!state) {
      BOOST_LOG(warning) << (result.status == save_status_e::disabled ? "stream_recorder: replay buffer is not enabled"sv :
                                                                         "stream_recorder: replay buffer is empty"sv);
      return admission;
    }
    state->reservation = std::move(reservation);
    admission.save = admitted_save_t {std::move(state)};
    return admission;
  }

  detail::save_result_t detail::complete_replay_save(admitted_save_t save) {
    save_result_t result;
    if (!save) {
      result.status = save_status_e::storage_error;
      return result;
    }
    auto &lease = save.state_->lease;
    result.admitted = true;
    result.packets = lease.snapshot.payloads.size();
    result.bytes = lease.snapshot.bytes;
    result.video_format = lease.snapshot.video_format;
    result.generation = lease.snapshot.generation;
    result.keyframes = lease.snapshot.keyframes;
    result.span = lease.snapshot.span;
    bool staging_left = false;
    try {
      lease.published = store_snapshot(lease.snapshot, result, staging_left);
    } catch (...) {
      // All throwing storage work precedes publication; owned staging cleanup already ran.
      // Do not allocate another diagnostic string while handling an allocation failure.
      result.status = save_status_e::storage_error;
      result.path.clear();
    }
    const bool published = lease.published;
    // Release the snapshot and accounting, then the reservation, before reporting.
    save = admitted_save_t {};
    // Reporting must not change the outcome: a published file stays a reported success.
    try {
      if (published) {
        if (staging_left) {
          BOOST_LOG(warning) << "stream_recorder: published replay, but could not remove its staging link"sv;
        }
        BOOST_LOG(info) << "stream_recorder: saved replay ("sv << result.packets << " packets, "sv
                        << (result.bytes / 1024 / 1024) << " MiB) to ["sv << result.path << "]"sv;
      } else {
        BOOST_LOG(error) << "stream_recorder: replay save failed, nothing published: "sv << result.error;
      }
    } catch (...) {
    }
    return result;
  }

  detail::save_result_t detail::save_replay_result_at(std::chrono::steady_clock::time_point now) {
    auto admission = admit_replay_save(now);
    if (!admission.save) {
      return std::move(admission.result);
    }
    return complete_replay_save(std::move(admission.save));
  }

#ifdef POLARIS_TESTS
  long long detail::native_write(int fd, const std::uint8_t *data, std::size_t size) {
    return native_write_impl(fd, data, size);
  }

  int detail::native_sync(int fd) {
    return native_sync_impl(fd);
  }

  int detail::native_close(int fd) {
    return native_close_impl(fd);
  }

  std::error_code detail::native_publish(const fs::path &staging, const fs::path &final_path) {
    return native_publish_impl(staging, final_path);
  }
#endif

  namespace {
    /// The continuous packet a push admitted, if any: its writer and sequence.
    struct continuous_admitted_t {
      std::shared_ptr<continuous_writer_t> writer;
      std::uint64_t sequence = 0;
      keyframe_pending_t keyframe;  ///< A keyframe request decided for the owner, raised after unlock
    };

    /**
     * The one admission path of every packet entry. It never waits for storage: a continuous
     * packet is only copied into its file's queue.
     */
    continuous_admitted_t push_packet_core(const uint8_t *data, size_t size, bool is_video, bool is_keyframe, int64_t pts,
                                           std::uint64_t session_generation, int video_format,
                                           std::chrono::steady_clock::time_point now,
                                           const packet_provenance_t *provenance = nullptr) {
      // Fast path: nothing to do if disabled and not recording. Reads
      // mode_hint, not cfg.mode, so this stays lock-free and race-free on
      // the send thread in the default (disabled) configuration.
      if (mode_hint.load(std::memory_order_relaxed) == mode_t::disabled && !recording.load()) {
        return {};
      }

      // Only record video packets (audio is not written to raw bitstream files)
      if (!is_video) {
        return {};
      }

      if (video_format >= 0 && video_format <= 3) {
        active_video_format.store(video_format, std::memory_order_relaxed);
      }

      continuous_admitted_t admitted;
      std::lock_guard lock(mutex);

      // Continuous recording: only the open file's owner (or a candidate at a keyframe) is queued.
      if (recording.load()) {
        admitted.sequence = continuous_offer(data, size, is_keyframe, session_generation, video_format, now);
        if (admitted.sequence != 0) {
          admitted.writer = continuous_writer;
        }
        // Only the owner's own packets decide its keyframe requests.
        if (session_generation != 0 && session_generation == continuous.owner_generation) {
          admitted.keyframe = keyframe_reserve(now);
        }
      }

      // Replay buffer: store packet in memory
      if (cfg.mode == mode_t::replay_buffer) {
        append_replay_packet(data, size, is_keyframe, pts, session_generation, video_format, now, provenance);
      }
      return admitted;
    }
  }  // namespace

  void push_packet(const uint8_t *data, size_t size, bool is_video, bool is_keyframe, int64_t pts,
                   std::uint64_t session_generation, int video_format) {
    // Production: admission only, never a wait for the writer. A request (none in production,
    // where no session has a channel) is raised after the recorder lock was released: that lock
    // is push_packet_core()'s scoped lock_guard, destroyed when push_packet_core() returns.
    auto admitted = push_packet_core(data, size, is_video, is_keyframe, pts, session_generation, video_format,
                                     std::chrono::steady_clock::now());
    keyframe_dispatch(std::move(admitted.keyframe));
  }

  void push_video(const uint8_t *data, size_t size, bool is_keyframe, std::uint64_t session_generation,
                  int video_format, const packet_provenance_t &provenance) {
    // push_packet() with the same arguments the tap gave it, plus the evidence record.
    auto admitted = push_packet_core(data, size, true, is_keyframe, provenance.frame_index.value_or(0),
                                     session_generation, video_format, std::chrono::steady_clock::now(), &provenance);
    keyframe_dispatch(std::move(admitted.keyframe));
  }

  namespace {
    /// Diagnostic entries only: wait (bounded) for the writer to finish the one packet admitted.
    void wait_admitted_packet(const continuous_admitted_t &admitted) {
      if (admitted.sequence == 0) {
        return;
      }
      std::unique_lock guard(admitted.writer->lock);
      admitted.writer->changed.wait_for(guard, std::chrono::seconds {10}, [&admitted] {
        return admitted.writer->completed_sequence >= admitted.sequence;
      });
    }
  }  // namespace

  void detail::push_packet_at(const uint8_t *data, size_t size, bool is_video, bool is_keyframe, int64_t pts,
                              std::uint64_t session_generation, int video_format,
                              std::chrono::steady_clock::time_point now) {
    auto admitted = push_packet_core(data, size, is_video, is_keyframe, pts, session_generation, video_format, now);
    keyframe_dispatch(std::move(admitted.keyframe));  // push_packet_core()'s lock is gone, as above
    wait_admitted_packet(admitted);
  }

  void detail::push_video_at(const uint8_t *data, size_t size, bool is_keyframe, std::uint64_t session_generation,
                             int video_format, const packet_provenance_t &provenance,
                             std::chrono::steady_clock::time_point now) {
    auto admitted = push_packet_core(data, size, true, is_keyframe, provenance.frame_index.value_or(0),
                                     session_generation, video_format, now, &provenance);
    keyframe_dispatch(std::move(admitted.keyframe));  // push_packet_core()'s lock is gone, as above
    wait_admitted_packet(admitted);
  }

  namespace {
    /**
     * Register or update a session (requires `mutex`). `channel` (may be null) is attached only
     * to a new registration or one without a channel; a different channel for a session that
     * has one rejects the whole call. Returns any keyframe request the change makes due.
     */
    keyframe_pending_t session_started_locked(std::uint64_t session_generation, bool replay_eligible, int video_format,
                                              std::shared_ptr<keyframe_channel_t> channel) {
      if (session_generation == 0) {
        ++replay_counters.refused_lifecycle;
        BOOST_LOG(warning) << "stream_recorder: refused a session with no generation"sv;
        return {};
      }
      const auto existing = replay_sessions.find(session_generation);
      if (channel && existing != replay_sessions.end() && existing->second.keyframe_channel &&
          existing->second.keyframe_channel != channel) {
        // Ambiguous: two channels for one session. Nothing changes, metadata included.
        ++keyframe_registration_conflicts;
        return {};
      }
      const int known = known_video_format(video_format) ? video_format : -1;
      // The only allocation (a new registration) precedes every state change.
      auto [it, inserted] = replay_sessions.try_emplace(session_generation, registered_session_t {replay_eligible, known, {}});
      bool attached = false;
      if (channel && !it->second.keyframe_channel) {
        it->second.keyframe_channel = std::move(channel);
        attached = !inserted;
      }
      if (!inserted) {
        auto &session = it->second;
        // An unknown codec never erases a known one.
        const int format = known >= 0 ? known : session.video_format;
        if (session.eligible == replay_eligible && session.video_format == format) {
          // Metadata unchanged; a newly attached channel may still be owed a request.
          return attached ? keyframe_reserve(std::chrono::steady_clock::now()) : keyframe_pending_t {};
        }
        const bool eligibility_changed = session.eligible != replay_eligible;
        session.eligible = replay_eligible;
        session.video_format = format;
        if (eligibility_changed) {
          // The same session changing eligibility is a transition: its earlier packets go.
          // A codec learned for a session does not reset the replay run: packets carry theirs.
          clear_replay_run(true, window_limit_e::selection);
        }
      }
      reselect_replay_session();
      continuous_follow_selection();
      // The owner may be sole again after an overlap: it may be owed a request.
      return keyframe_reserve(std::chrono::steady_clock::now());
    }
  }  // namespace

  void session_started(std::uint64_t session_generation, bool replay_eligible, int video_format) {
    keyframe_pending_t pending;
    {
      std::lock_guard lock(mutex);
      pending = session_started_locked(session_generation, replay_eligible, video_format, nullptr);
    }
    keyframe_dispatch(std::move(pending));
  }

  void session_started(std::uint64_t session_generation, bool replay_eligible, int video_format,
                       std::shared_ptr<keyframe_channel_t> channel) {
    keyframe_pending_t pending;
    {
      std::lock_guard lock(mutex);
      pending = session_started_locked(session_generation, replay_eligible, video_format, std::move(channel));
    }
    keyframe_dispatch(std::move(pending));
  }

  void session_ended(std::uint64_t session_generation) {
    keyframe_pending_t pending;
    {
      std::lock_guard lock(mutex);
      const auto it = replay_sessions.find(session_generation);  // Unknown generations insert nothing
      if (it == replay_sessions.end()) {
        return;
      }
      // Withdraw anything still owed to this session, then retire its channel: after this no
      // request raises its event, even one already decided.
      const bool owed = (keyframe_file.in_flight && keyframe_file.in_flight->generation == session_generation);
      bool authorized_for_it;
      {
        std::lock_guard gate(keyframe_request_gate);
        authorized_for_it = keyframe_authorized && keyframe_authorized->generation == session_generation;
      }
      if (owed || authorized_for_it) {
        keyframe_withdraw(false);
      }
      if (it->second.keyframe_channel) {
        it->second.keyframe_channel->retire();
      }
      replay_sessions.erase(it);
      // The ended session's packets go even if it was not the selected one.
      clear_replay_run(true, window_limit_e::selection);
      reselect_replay_session();
      continuous_follow_selection();
      // Another owner may now be sole again after an overlap.
      pending = keyframe_reserve(std::chrono::steady_clock::now());
    }
    keyframe_dispatch(std::move(pending));
  }

  replay_stats_t replay_stats() {
    return detail::replay_stats_at(std::chrono::steady_clock::now());
  }

  /// The replay statistics; the caller holds `mutex`. Shared by replay_stats_at() and
  /// recorder_status_at() so each takes the (non-recursive) lock exactly once.
  static replay_stats_t replay_stats_locked(std::chrono::steady_clock::time_point now) {
    // A status read is a freshness boundary too: expired packets are released here, so an
    // idle buffer frees its memory and stale packets cannot reappear on a later read.
    expire_replay(now);
    replay_stats_t stats = replay_counters;
    stats.packets = replay_buffer.size();
    stats.bytes = replay_bytes;
    stats.selected_generation = replay_selected;
    stats.registered_sessions = replay_sessions.size();
    stats.run_video_format = replay_video_format;
    stats.duration = replay_duration();
    stats.max_bytes = replay_max_bytes;
    stats.max_packets = replay_max_packets;
    stats.saves_refused_busy = saves_refused_busy.load(std::memory_order_relaxed);
    stats.save_in_progress = snapshot_active;
    stats.snapshot_packets = snapshot_packets;
    stats.snapshot_bytes = snapshot_bytes;
    stats.snapshot_only_bytes = snapshot_only_bytes;
    stats.unique_payload_bytes = replay_bytes + snapshot_only_bytes;
    stats.metadata_entries = replay_buffer.size() + snapshot_packets;
    // A snapshot is a copy of one admitted live run, which the live caps already bound.
    stats.max_snapshot_bytes = replay_max_bytes;
    stats.max_snapshot_packets = replay_max_packets;
    stats.max_unique_payload_bytes = replay_max_bytes + stats.max_snapshot_bytes;
    stats.max_metadata_entries = replay_max_packets + stats.max_snapshot_packets;
    // A read stamped before a stored arrival reports an age of zero, never a negative one; the
    // expiry above already ran, so an earlier read cannot bring released packets back.
    const auto age = [now](std::chrono::steady_clock::time_point at) {
      return now > at ? now - at : std::chrono::steady_clock::duration::zero();
    };
    stats.keyframe_start_run = !replay_buffer.empty();
    stats.run_keyframes = replay_run_keyframes;
    if (!replay_buffer.empty()) {
      stats.run_span = replay_buffer.back().timestamp - replay_buffer.front().timestamp;
      stats.run_front_age = age(replay_buffer.front().timestamp);
    }
    if (replay_last_accepted_keyframe) {
      stats.last_accepted_keyframe_age = age(*replay_last_accepted_keyframe);
    }
    stats.awaiting_keyframe = cfg.mode == mode_t::replay_buffer && replay_selected != 0 && replay_buffer.empty();
    stats.last_window_limit = replay_last_limit;
    stats.snapshot_keyframes = snapshot_keyframes;
    stats.snapshot_span = snapshot_span;
    return stats;
  }

  replay_stats_t detail::replay_stats_at(std::chrono::steady_clock::time_point now) {
    std::lock_guard lock(mutex);
    return replay_stats_locked(now);
  }

  recorder_status_t recorder_status() {
    return detail::recorder_status_at(std::chrono::steady_clock::now());
  }

  recorder_status_t detail::recorder_status_at(std::chrono::steady_clock::time_point now) {
    recorder_status_t status;
    std::lock_guard lock(mutex);
    // One critical section: start/stop_recording() and load_config() change these under the
    // same lock, so the combination reported is one that existed. No filesystem access here.
    status.mode = cfg.mode;
    status.recording = recording.load();
    status.file = recording_file;
    status.replay = replay_stats_locked(now);
    status.continuous = continuous;
    if (continuous_writer) {
      // The writer's side, in the same snapshot (lock order: `mutex`, then the writer's lock).
      auto &writer = *continuous_writer;
      std::lock_guard guard(writer.lock);
      auto &c = status.continuous;
      c.packets_written = writer.packets_written;
      c.bytes_written = writer.bytes_written;
      c.packets_admitted = writer.packets_admitted;
      c.bytes_admitted = writer.bytes_admitted;
      c.queued_packets = writer.slot_used;
      c.queued_bytes = writer.byte_used;
      c.max_queue_packets = writer.slot_capacity;
      c.max_queue_bytes = writer.ring_bytes;
      c.discarded_packets = writer.discarded_packets;
      c.discarded_bytes = writer.discarded_bytes;
      if (writer.failed) {
        // A storage failure outranks whatever the session did after it.
        c.state = continuous_state_e::failed;
        c.cause = continuous_cause_e::storage;
        c.error = writer.error;
        c.error_number = writer.error_number;
      }
    }
    return status;
  }

#ifdef POLARIS_TESTS
  void detail::set_replay_limits_for_tests(std::size_t max_bytes, std::size_t max_packets) {
    std::lock_guard lock(mutex);
    replay_max_bytes = max_bytes;
    replay_max_packets = max_packets;
    clear_replay_run(true, window_limit_e::configuration);
  }

  std::vector<detail::provenance_entry_t> detail::replay_provenance_for_tests() {
    std::vector<provenance_entry_t> entries;
    std::lock_guard lock(mutex);
    entries.reserve(replay_buffer.size());
    for (const auto &packet : replay_buffer) {
      entries.push_back(provenance_entry_t {packet.sequence, *packet.data, packet.provenance});
    }
    return entries;
  }

  detail::snapshot_provenance_t detail::snapshot_provenance_for_tests(const admitted_save_t &save) {
    snapshot_provenance_t out;
    if (!save) {
      return out;
    }
    // The snapshot is immutable while held, and only its owner may complete or drop it.
    const auto &snapshot = save.state_->lease.snapshot;
    out.payload_count = snapshot.payloads.size();
    out.provenance_count = snapshot.provenance.size();
    out.payload_capacity = snapshot.payloads.capacity();
    out.provenance_capacity = snapshot.provenance.capacity();
    const auto count = std::min(snapshot.payloads.size(), snapshot.provenance.size());
    out.entries.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
      out.entries.push_back(provenance_entry_t {0, *snapshot.payloads[i], snapshot.provenance[i]});
    }
    return out;
  }

  detail::provenance_sizes_t detail::provenance_sizes_for_tests() {
    // The live entry's members before #359, in the same order: a same-build baseline only.
    struct pre_provenance_entry_mirror_t {
      payload_t data;
      bool is_video;
      bool is_keyframe;
      int64_t pts;
      std::chrono::steady_clock::time_point timestamp;
      std::uint64_t sequence;
    };
    provenance_sizes_t sizes;
    sizes.packet_provenance = sizeof(packet_provenance_t);
    sizes.provenance_record = sizeof(provenance_record_t);
    sizes.live_entry = sizeof(buffered_packet_t);
    sizes.pre_provenance_entry_mirror = sizeof(pre_provenance_entry_mirror_t);
    sizes.snapshot_payload_ref = sizeof(payload_t);
    return sizes;
  }

  void detail::reset_replay_for_tests() {
    // Join any writer first, so no copied seam outlives the fixture that installed it.
    std::lock_guard control(continuous_control);
    continuous_thread.finish();
    int fd = -1;
    {
      std::lock_guard lock(mutex);
      clear_replay_run(true, window_limit_e::none);
      replay_max_bytes = k_replay_max_payload_bytes;
      replay_max_packets = k_replay_max_packets;
      for (auto &[generation, session] : replay_sessions) {
        if (session.keyframe_channel) {
          session.keyframe_channel->retire();
        }
      }
      replay_sessions.clear();
      replay_selected = 0;
      // Per-file and process keyframe diagnostics restart; the monotonic counters never do.
      keyframe_withdraw(true);
      keyframe_file = keyframe_file_t {};
      keyframe_stale_completions_total = 0;
      keyframe_registration_conflicts = 0;
      keyframe_hooks = {};
      replay_counters = {};
      replay_next_sequence = 1;
      saves_refused_busy.store(0, std::memory_order_relaxed);
      active_video_format.store(-1, std::memory_order_relaxed);
      save_hooks = {};
      // An open continuous file is released like stop_recording() releases it.
      fd = std::exchange(continuous_fd, -1);
      recording_file.clear();
      continuous_writer.reset();
      continuous_reset_locked();
      recording.store(false);
      continuous_hooks = {};
      continuous_limit_bytes = k_continuous_queue_bytes;
      continuous_limit_slots = k_continuous_queue_slots;
    }
    continuous_shut_down = false;  // Tests only: a later test may start again.
    if (fd >= 0) {
      native_close_impl(fd);
    }
  }

  detail::keyframe_stats_t detail::keyframe_stats_for_tests() {
    keyframe_stats_t stats;
    std::lock_guard lock(mutex);
    const auto token = [](const ::stream_recorder::keyframe_token_t &t) {
      return detail::keyframe_token_t {t.incarnation, t.generation, t.sequence};
    };
    stats.file_incarnation = keyframe_file.incarnation;
    stats.awaiting = continuous_fd >= 0 && continuous.owner_generation != 0 && !continuous_owner_fenced &&
                     continuous.state == continuous_state_e::awaiting_keyframe;
    stats.keyframe_exceeds_queue = keyframe_file.exceeds_queue;
    if (keyframe_file.in_flight) {
      stats.in_flight = token(*keyframe_file.in_flight);
    }
    {
      std::lock_guard gate(keyframe_request_gate);
      if (keyframe_authorized) {
        stats.authorized = token(*keyframe_authorized);
      }
    }
    stats.last_request_at = keyframe_file.last_request_at;
    stats.last_keyframe_bytes = keyframe_file.last_keyframe_bytes;
    stats.reservations = keyframe_file.reservations;
    stats.attempted = keyframe_file.attempted;
    stats.expired = keyframe_file.expired;
    stats.stopped = keyframe_file.stopped;
    stats.retired = keyframe_file.retired;
    stats.fenced = keyframe_file.fenced;
    stats.refused_rate = keyframe_file.refused_rate;
    stats.refused_writer_unavailable = keyframe_file.refused_writer_unavailable;
    stats.refused_no_slot = keyframe_file.refused_no_slot;
    stats.refused_queue_sequence_exhausted = keyframe_file.refused_queue_sequence_exhausted;
    stats.refused_bytes = keyframe_file.refused_bytes;
    stats.refused_request_sequence_exhausted = keyframe_file.refused_request_sequence_exhausted;
    stats.keyframe_stale_completions_total = keyframe_stale_completions_total;
    stats.keyframe_registration_conflicts = keyframe_registration_conflicts;
    stats.continuous_incarnation = continuous_incarnation;
    stats.keyframe_request_sequence = keyframe_request_sequence;
    return stats;
  }

  void detail::set_keyframe_hooks_for_tests(keyframe_hooks_t hooks) {
    std::lock_guard lock(mutex);
    keyframe_hooks = std::move(hooks);
  }

  void detail::set_keyframe_counters_for_tests(std::uint64_t incarnation_floor, std::uint64_t sequence_floor) {
    std::lock_guard lock(mutex);
    // Raise only: a lower value would let a token name two requests.
    continuous_incarnation = std::max(continuous_incarnation, incarnation_floor);
    keyframe_request_sequence = std::max(keyframe_request_sequence, sequence_floor);
  }

  bool detail::set_continuous_next_sequence_for_tests(std::uint64_t value) {
    std::lock_guard lock(mutex);
    if (!continuous_writer) {
      return false;
    }
    std::lock_guard guard(continuous_writer->lock);
    continuous_writer->next_sequence = value;
    return true;
  }

  bool detail::recorder_mutex_try_lock_for_tests() {
    if (!mutex.try_lock()) {
      return false;
    }
    mutex.unlock();
    return true;
  }

  void detail::set_continuous_limits_for_tests(std::size_t bytes, std::size_t slots) {
    std::lock_guard lock(mutex);
    // Ring and slot arithmetic need head + used to fit in size_t.
    continuous_limit_bytes = std::clamp<std::size_t>(bytes, 1, std::numeric_limits<std::size_t>::max() / 2);
    continuous_limit_slots = std::clamp<std::size_t>(slots, 1, std::numeric_limits<std::size_t>::max() / 2);
  }

  bool detail::wait_continuous_drained_for_tests(std::chrono::milliseconds timeout) {
    std::shared_ptr<continuous_writer_t> writer;
    {
      std::lock_guard lock(mutex);
      writer = continuous_writer;
    }
    if (!writer) {
      return true;
    }
    std::unique_lock guard(writer->lock);
    return writer->changed.wait_for(guard, timeout, [&writer] {
      return writer->slot_used == 0;
    });
  }

  bool detail::finish_continuous_writer_for_tests() {
    std::lock_guard control(continuous_control);
    // The same owner that ends a writer at process exit, run now on a moved-out handle.
    continuous_thread_t owner {std::move(continuous_thread)};
    return owner.finish();
  }

  void detail::set_continuous_hooks_for_tests(continuous_hooks_t hooks) {
    std::lock_guard lock(mutex);
    continuous_hooks = std::move(hooks);
  }

  int detail::native_open_exclusive(const fs::path &path) {
    return open_exclusive(path);
  }

  void detail::set_save_hooks_for_tests(save_hooks_t hooks) {
    save_hooks = std::move(hooks);
  }
#endif

  mode_t current_mode() {
    // The mode load_config() last published. cfg.mode is mutex-only state; this is
    // called per encoded packet on the broadcast thread, so it reads the atomic
    // hint (as push_packet's fast path does) instead of taking the lock.
    return mode_hint.load(std::memory_order_relaxed);
  }

  std::string current_file() {
    std::lock_guard lock(mutex);
    return recording_file;
  }

}  // namespace stream_recorder
