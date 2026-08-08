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

  // Replay buffer state
  static std::deque<buffered_packet_t> replay_buffer;

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
   * @brief Determine the file extension for the current video codec.
   * Prefer the active session's videoFormat; fall back to conf allow-lists.
   */
  static std::string codec_extension() {
    const int fmt = active_video_format.load(std::memory_order_relaxed);
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

  void set_active_video_format(int video_format) {
    if (video_format < 0 || video_format > 2) {
      return;
    }
    active_video_format.store(video_format, std::memory_order_relaxed);
  }

  /**
   * @brief Trim the replay buffer to keep only the last N minutes of data.
   * Caller must hold the mutex.
   */
  static void trim_replay_buffer() {
    if (replay_buffer.empty()) {
      return;
    }

    auto cutoff = std::chrono::steady_clock::now() - std::chrono::minutes(cfg.replay_buffer_minutes);

    while (!replay_buffer.empty() && replay_buffer.front().timestamp < cutoff) {
      replay_buffer.pop_front();
    }
  }

  // ---- Public API ----

  void load_config() {
    std::lock_guard lock(mutex);

    cfg.mode = config::recording.enabled ? (config::recording.replay_buffer
                                              ? mode_t::replay_buffer
                                              : mode_t::continuous)
                                         : mode_t::disabled;
    cfg.output_dir = config::recording.output_dir;
    cfg.replay_buffer_minutes = config::recording.replay_buffer_minutes;
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
    std::lock_guard lock(mutex);

    if (cfg.mode != mode_t::replay_buffer) {
      BOOST_LOG(warning) << "stream_recorder: replay buffer is not enabled"sv;
      return {};
    }

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

    auto ext = codec_extension();
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

  void push_packet(const uint8_t *data, size_t size, bool is_video, bool is_keyframe, int64_t pts, int video_format) {
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

    if (video_format >= 0 && video_format <= 2) {
      active_video_format.store(video_format, std::memory_order_relaxed);
    }

    std::lock_guard lock(mutex);

    // Continuous recording: write directly to file
    if (recording.load() && recording_stream.is_open()) {
      recording_stream.write(reinterpret_cast<const char *>(data), size);
    }

    // Replay buffer: store packet in memory
    if (cfg.mode == mode_t::replay_buffer) {
      replay_buffer.push_back(buffered_packet_t {
        std::vector<uint8_t>(data, data + size),
        is_video,
        is_keyframe,
        pts,
        std::chrono::steady_clock::now(),
      });

      trim_replay_buffer();
    }
  }

  mode_t current_mode() {
    return cfg.mode;
  }

  std::string current_file() {
    std::lock_guard lock(mutex);
    return recording_file;
  }

}  // namespace stream_recorder
