/**
 * @file src/platform/linux/pipewire_capture.h
 * @brief PipeWire capture format, copy, and frame metadata policy.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <pipewire/pipewire.h>
#include <spa/utils/hook.h>

#include "src/platform/common.h"

namespace pipewire_capture {
  enum class wait_result_e {
    frame,
    timeout,
    reinit,
    error,
  };

  enum class stream_state_e {
    unconnected,
    connecting,
    paused,
    streaming,
    error,
  };

  struct frame_info_t {
    int width = 0;
    int height = 0;
    int stride = 0;
    std::uint32_t spa_format = 0;
  };

  struct capture_options_t {
    int remote_fd = -1;
    std::uint32_t node_id = 0;
    std::uint64_t node_serial = 0;
    int requested_width = 0;
    int requested_height = 0;
  };

  std::optional<std::uint32_t> drm_format_for_spa(std::uint32_t spa_format);

  std::size_t safe_row_copy_bytes(
    std::size_t width_bytes,
    std::int32_t source_stride,
    std::size_t destination_stride
  );

  bool copy_memptr_frame_to_bgra(
    const std::uint8_t *source,
    std::size_t source_maxsize,
    std::uint32_t chunk_offset,
    std::uint32_t chunk_size,
    int width,
    int height,
    std::int32_t source_stride,
    std::uint32_t spa_format,
    std::uint8_t *destination,
    std::size_t destination_stride
  );

  bool is_terminal_transition(stream_state_e old_state, stream_state_e new_state);

  platf::frame_metadata_t cpu_frame_metadata();
  platf::frame_metadata_t dmabuf_frame_metadata(std::string render_node);

  class capture_t {
  public:
    explicit capture_t(capture_options_t options);
    ~capture_t();

    capture_t(const capture_t &) = delete;
    capture_t &operator=(const capture_t &) = delete;

    bool start();
    bool running() const;
    bool negotiated() const;
    frame_info_t frame_info() const;

    wait_result_e wait_for_frame(std::chrono::milliseconds timeout);
    bool copy_frame_to(platf::img_t &image);

  private:
    static void on_process(void *userdata);
    static void on_param_changed(void *userdata, std::uint32_t id, const struct spa_pod *param);
    static void on_state_changed(void *userdata, enum pw_stream_state old, enum pw_stream_state state, const char *errmsg);

    void process_buffer(struct pw_buffer *buffer);
    void set_terminal(wait_result_e result);

    capture_options_t options_;
    int remote_fd_ = -1;
    pw_thread_loop *loop_ = nullptr;
    pw_context *context_ = nullptr;
    pw_core *core_ = nullptr;
    pw_stream *stream_ = nullptr;
    spa_hook stream_listener_ {};

    mutable std::mutex frame_mtx_;
    std::condition_variable frame_cv_;
    std::vector<std::uint8_t> front_frame_;
    std::vector<std::uint8_t> back_frame_;
    frame_info_t front_info_;
    bool frame_available_ = false;
    bool negotiated_ = false;
    bool running_ = false;
    wait_result_e terminal_result_ = wait_result_e::timeout;
  };

#if defined(POLARIS_TESTS)
  bool copy_memptr_frame_for_tests(
    const std::uint8_t *source,
    std::size_t source_maxsize,
    std::uint32_t chunk_offset,
    std::uint32_t chunk_size,
    int width,
    int height,
    std::int32_t source_stride,
    std::uint32_t spa_format,
    std::uint8_t *destination,
    std::size_t destination_stride
  );

  bool is_terminal_transition_for_tests(stream_state_e old_state, stream_state_e new_state);
#endif
}  // namespace pipewire_capture
