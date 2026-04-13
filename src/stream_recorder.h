/**
 * @file src/stream_recorder.h
 * @brief Declarations for stream recording and replay buffer.
 */
#pragma once

// standard includes
#include <atomic>
#include <chrono>
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

  /**
   * @brief Feed an encoded packet into the recorder.
   * Called from the stream pipeline after encoding but before network send.
   * @param data Pointer to the encoded packet data.
   * @param size Size of the packet data in bytes.
   * @param is_video True if this is a video packet, false for audio.
   * @param is_keyframe True if this is an IDR/keyframe.
   * @param pts Presentation timestamp (frame index).
   */
  void push_packet(const uint8_t *data, size_t size, bool is_video, bool is_keyframe, int64_t pts);

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
