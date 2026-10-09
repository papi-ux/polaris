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
#include <cstdint>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
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
  };

  static config_t cfg;
  static std::mutex mutex;

  // Mirrors cfg.mode for push_packet's pre-lock fast path (send thread,
  // once per encoded video packet). cfg.mode itself is mutex-only state -
  // reading it without the lock, as the fast path must to stay lock-free
  // when recording is off, was a real data race against load_config()'s
  // writer. This mirror is the only thing the fast path reads unlocked.
  static std::atomic<mode_t> mode_hint {mode_t::disabled};

  // Continuous recording state
  static std::atomic<bool> recording {false};
  static std::string recording_file;
  static std::ofstream recording_stream;

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
  // Registered sessions and whether each is replay-eligible. Selection is derived only from this.
  static std::map<std::uint64_t, bool> replay_sessions;
  static std::uint64_t replay_selected = 0;
  static replay_stats_t replay_counters;
  static std::uint64_t replay_next_sequence = 1;

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
   * @brief Generate a timestamped filename for a recording.
   * @param suffix File extension (e.g. ".hevc", ".h264", ".av1").
   * @return Full file path.
   */
  static std::string generate_filename(const std::string &suffix) {
    auto now = std::chrono::system_clock::now();
    auto time_t_now = std::chrono::system_clock::to_time_t(now);
    std::tm tm_now {};
#ifdef _WIN32
    localtime_s(&tm_now, &time_t_now);
#else
    localtime_r(&time_t_now, &tm_now);
#endif

    std::ostringstream oss;
    oss << std::put_time(&tm_now, "%Y%m%d_%H%M%S");

    std::string dir = cfg.output_dir;
    if (dir.empty()) {
      dir = (platf::appdata() / "recordings").string();
    }

    // Ensure the output directory exists
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) {
      BOOST_LOG(warning) << "stream_recorder: failed to create output directory ["sv << dir << "]: "sv << ec.message();
    }

    return dir + "/polaris_" + oss.str() + suffix;
  }

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
   * @brief Determine the file extension for the current video codec.
   * Prefer the active session's videoFormat; fall back to conf allow-lists.
   */
  static std::string codec_extension() {
    return extension_for(active_video_format.load(std::memory_order_relaxed));
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
    if (replay_sessions.size() == 1 && replay_sessions.begin()->second) {
      selected = replay_sessions.begin()->first;
    }
    if (selected != replay_selected) {
      clear_replay_run(true, window_limit_e::selection);
      replay_selected = selected;
      BOOST_LOG(debug) << "stream_recorder: replay session "sv
                       << (selected ? std::to_string(selected) : std::string {"none"})
                       << " ("sv << replay_sessions.size() << " registered)"sv;
    }
  }

  /// A frame of the selected session was dropped: later frames would depend on it, so restart at a keyframe.
  static void replay_discontinuity(window_limit_e reason = window_limit_e::discontinuity) {
    clear_replay_run(false, reason);
    ++replay_counters.discontinuities;
  }

  /**
   * @brief Admit one video packet to the replay buffer, or refuse it without copying.
   */
  static void append_replay_packet(const uint8_t *data, size_t size, bool is_keyframe, int64_t pts,
                                   std::uint64_t session_generation, int video_format,
                                   std::chrono::steady_clock::time_point now) {
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

    // The one copy of the packet's bytes, made immutable so a save can share it.
    replay_buffer.push_back(buffered_packet_t {
      std::make_shared<const std::vector<uint8_t>>(data, data + size),
      true,
      is_keyframe,
      pts,
      now,
      replay_next_sequence++,
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

  void start_recording() {
    std::lock_guard lock(mutex);

    if (recording.load()) {
      BOOST_LOG(warning) << "stream_recorder: already recording"sv;
      return;
    }

    auto ext = codec_extension();
    recording_file = generate_filename(ext);

    recording_stream.open(recording_file, std::ios::binary | std::ios::trunc);
    if (!recording_stream.is_open()) {
      BOOST_LOG(error) << "stream_recorder: failed to open file ["sv << recording_file << "]"sv;
      recording_file.clear();
      return;
    }

    recording.store(true);
    BOOST_LOG(info) << "stream_recorder: started recording to ["sv << recording_file << "]"sv;
  }

  void stop_recording() {
    std::lock_guard lock(mutex);

    if (!recording.load()) {
      BOOST_LOG(warning) << "stream_recorder: not recording"sv;
      return;
    }

    recording.store(false);
    recording_stream.close();
    BOOST_LOG(info) << "stream_recorder: stopped recording ["sv << recording_file << "]"sv;
    recording_file.clear();
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

    std::string wall_stamp(std::chrono::system_clock::time_point when) {
#ifdef POLARIS_TESTS
      if (save_hooks.name_stamp) {
        return save_hooks.name_stamp();
      }
#endif
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
          for (const auto &packet : replay_buffer) {
            snapshot.payloads.push_back(packet.data);
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

  void push_packet(const uint8_t *data, size_t size, bool is_video, bool is_keyframe, int64_t pts,
                   std::uint64_t session_generation, int video_format) {
    detail::push_packet_at(data, size, is_video, is_keyframe, pts, session_generation, video_format,
                           std::chrono::steady_clock::now());
  }

  void detail::push_packet_at(const uint8_t *data, size_t size, bool is_video, bool is_keyframe, int64_t pts,
                              std::uint64_t session_generation, int video_format,
                              std::chrono::steady_clock::time_point now) {
    // Fast path: nothing to do if disabled and not recording. Reads
    // mode_hint, not cfg.mode, so this stays lock-free and race-free on
    // the send thread in the default (disabled) configuration.
    if (mode_hint.load(std::memory_order_relaxed) == mode_t::disabled && !recording.load()) {
      return;
    }

    // Only record video packets (audio is not written to raw bitstream files)
    if (!is_video) {
      return;
    }

    if (video_format >= 0 && video_format <= 3) {
      active_video_format.store(video_format, std::memory_order_relaxed);
    }

    std::lock_guard lock(mutex);

    // Continuous recording: write directly to file
    if (recording.load() && recording_stream.is_open()) {
      recording_stream.write(reinterpret_cast<const char *>(data), size);
    }

    // Replay buffer: store packet in memory
    if (cfg.mode == mode_t::replay_buffer) {
      append_replay_packet(data, size, is_keyframe, pts, session_generation, video_format, now);
    }
  }

  void session_started(std::uint64_t session_generation, bool replay_eligible) {
    std::lock_guard lock(mutex);
    if (session_generation == 0) {
      ++replay_counters.refused_lifecycle;
      BOOST_LOG(warning) << "stream_recorder: refused a session with no generation"sv;
      return;
    }
    auto [it, inserted] = replay_sessions.try_emplace(session_generation, replay_eligible);
    if (!inserted) {
      if (it->second == replay_eligible) {
        return;
      }
      // The same session changing eligibility is a transition: its earlier packets go.
      it->second = replay_eligible;
      clear_replay_run(true, window_limit_e::selection);
    }
    reselect_replay_session();
  }

  void session_ended(std::uint64_t session_generation) {
    std::lock_guard lock(mutex);
    if (replay_sessions.erase(session_generation) == 0) {
      return;
    }
    // The ended session's packets go even if it was not the selected one.
    clear_replay_run(true, window_limit_e::selection);
    reselect_replay_session();
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
    return status;
  }

#ifdef POLARIS_TESTS
  void detail::set_replay_limits_for_tests(std::size_t max_bytes, std::size_t max_packets) {
    std::lock_guard lock(mutex);
    replay_max_bytes = max_bytes;
    replay_max_packets = max_packets;
    clear_replay_run(true, window_limit_e::configuration);
  }

  void detail::reset_replay_for_tests() {
    std::lock_guard lock(mutex);
    clear_replay_run(true, window_limit_e::none);
    replay_max_bytes = k_replay_max_payload_bytes;
    replay_max_packets = k_replay_max_packets;
    replay_sessions.clear();
    replay_selected = 0;
    replay_counters = {};
    replay_next_sequence = 1;
    saves_refused_busy.store(0, std::memory_order_relaxed);
    active_video_format.store(-1, std::memory_order_relaxed);
    save_hooks = {};
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
