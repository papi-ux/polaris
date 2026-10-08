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
#include <chrono>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>

// local includes
#include "config.h"
#include "logging.h"
#include "platform/common.h"
#include "stream_recorder.h"

using namespace std::literals;
namespace fs = std::filesystem;

namespace stream_recorder {

  // ---- Internal state ----

  struct buffered_packet_t {
    std::vector<uint8_t> data;
    bool is_video;
    bool is_keyframe;
    int64_t pts;
    std::chrono::steady_clock::time_point timestamp;
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

  /**
   * @brief Clear the run. A lifecycle or config clear also forgets its codec.
   */
  static void clear_replay_run(bool forget_codec) {
    replay_buffer.clear();
    replay_bytes = 0;
    if (forget_codec) {
      replay_video_format = -1;
    }
  }

  static void pop_replay_front() {
    replay_bytes -= replay_buffer.front().data.size();
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
  }

  /// Recompute the selected session after a lifecycle change; any change of selection clears the run.
  static void reselect_replay_session() {
    std::uint64_t selected = 0;
    if (replay_sessions.size() == 1 && replay_sessions.begin()->second) {
      selected = replay_sessions.begin()->first;
    }
    if (selected != replay_selected) {
      clear_replay_run(true);
      replay_selected = selected;
      BOOST_LOG(debug) << "stream_recorder: replay session "sv
                       << (selected ? std::to_string(selected) : std::string {"none"})
                       << " ("sv << replay_sessions.size() << " registered)"sv;
    }
  }

  /// A frame of the selected session was dropped: later frames would depend on it, so restart at a keyframe.
  static void replay_discontinuity() {
    clear_replay_run(false);
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
      replay_discontinuity();
    }
    expire_replay(now);

    // Make room under both caps. replay_bytes <= replay_max_bytes and size <= replay_max_bytes
    // hold here, so neither comparison can overflow.
    bool trimmed = false;
    while (!replay_buffer.empty() &&
           (size > replay_max_bytes - replay_bytes || replay_buffer.size() >= replay_max_packets)) {
      pop_replay_front();
      trimmed = true;
    }
    if (trimmed) {
      align_replay_to_keyframe();
    }

    // A run starts only at a keyframe: a dependent frame with nothing before it cannot decode.
    if (replay_buffer.empty() && !is_keyframe) {
      ++replay_counters.refused_awaiting_keyframe;
      return;
    }
    if (replay_buffer.empty()) {
      replay_video_format = video_format;
    }

    replay_buffer.push_back(buffered_packet_t {
      std::vector<uint8_t>(data, data + size),
      true,
      is_keyframe,
      pts,
      now,
    });
    replay_bytes += size;
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
      clear_replay_run(true);
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

  std::string save_replay() {
    return detail::save_replay_at(std::chrono::steady_clock::now());
  }

  std::string detail::save_replay_at(std::chrono::steady_clock::time_point now) {
    std::lock_guard lock(mutex);

    if (cfg.mode != mode_t::replay_buffer) {
      BOOST_LOG(warning) << "stream_recorder: replay buffer is not enabled"sv;
      return {};
    }

    // Nothing older than the duration is saved, even when no packet arrived since.
    expire_replay(now);
    if (replay_buffer.empty()) {
      BOOST_LOG(warning) << "stream_recorder: replay buffer is empty"sv;
      return {};
    }

    // Find the first keyframe in the buffer to start from
    auto it = replay_buffer.begin();
    for (; it != replay_buffer.end(); ++it) {
      if (it->is_video && it->is_keyframe) {
        break;
      }
    }

    if (it == replay_buffer.end()) {
      BOOST_LOG(warning) << "stream_recorder: no keyframe found in replay buffer"sv;
      return {};
    }

    // The run's own codec, not whichever session last set the global one.
    auto ext = extension_for(replay_video_format);
    auto filename = generate_filename("_replay" + ext);

    std::ofstream out(filename, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
      BOOST_LOG(error) << "stream_recorder: failed to open replay file ["sv << filename << "]"sv;
      return {};
    }

    size_t total_bytes = 0;
    size_t packet_count = 0;
    for (; it != replay_buffer.end(); ++it) {
      if (it->is_video) {
        out.write(reinterpret_cast<const char *>(it->data.data()), it->data.size());
        total_bytes += it->data.size();
        packet_count++;
      }
    }

    out.close();

    BOOST_LOG(info) << "stream_recorder: saved replay ("sv << packet_count << " packets, "sv
                    << (total_bytes / 1024 / 1024) << " MiB) to ["sv << filename << "]"sv;

    return filename;
  }

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
      clear_replay_run(true);
    }
    reselect_replay_session();
  }

  void session_ended(std::uint64_t session_generation) {
    std::lock_guard lock(mutex);
    if (replay_sessions.erase(session_generation) == 0) {
      return;
    }
    // The ended session's packets go even if it was not the selected one.
    clear_replay_run(true);
    reselect_replay_session();
  }

  replay_stats_t replay_stats() {
    return detail::replay_stats_at(std::chrono::steady_clock::now());
  }

  replay_stats_t detail::replay_stats_at(std::chrono::steady_clock::time_point now) {
    std::lock_guard lock(mutex);
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
    return stats;
  }

#ifdef POLARIS_TESTS
  void detail::set_replay_limits_for_tests(std::size_t max_bytes, std::size_t max_packets) {
    std::lock_guard lock(mutex);
    replay_max_bytes = max_bytes;
    replay_max_packets = max_packets;
    clear_replay_run(true);
  }

  void detail::reset_replay_for_tests() {
    std::lock_guard lock(mutex);
    clear_replay_run(true);
    replay_max_bytes = k_replay_max_payload_bytes;
    replay_max_packets = k_replay_max_packets;
    replay_sessions.clear();
    replay_selected = 0;
    replay_counters = {};
    active_video_format.store(-1, std::memory_order_relaxed);
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
