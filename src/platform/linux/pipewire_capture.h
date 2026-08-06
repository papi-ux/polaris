/**
 * @file src/platform/linux/pipewire_capture.h
 * @brief PipeWire capture format, copy, and frame metadata policy.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <array>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <pipewire/pipewire.h>
#include <spa/param/video/raw.h>
#include <spa/utils/hook.h>

#include "src/platform/common.h"
#include "src/platform/linux/graphics.h"

namespace pipewire_capture {
  enum class wait_result_e {
    frame,
    timeout,
    reinit,
    error,
  };

  struct frame_info_t {
    int width = 0;
    int height = 0;
    int stride = 0;
    std::uint32_t spa_format = 0;
    std::uint64_t modifier = 0;
  };

  struct dmabuf_format_modifier_t {
    std::uint32_t spa_format = 0;
    std::uint32_t drm_fourcc = 0;
    std::uint64_t modifier = 0;
  };

  struct capture_options_t {
    int remote_fd = -1;
    std::uint32_t node_id = 0;
    std::uint64_t node_serial = 0;
    int requested_width = 0;
    int requested_height = 0;
    std::optional<std::string> capture_render_node;
    std::vector<dmabuf_format_modifier_t> dmabuf_formats;
    platf::mem_type_e mem_type = platf::mem_type_e::system;
    bool may_use_dmabuf = false;
  };

  struct egl_dmabuf_format_t {
    std::uint32_t drm_fourcc = 0;
    std::vector<std::uint64_t> modifiers;
    std::vector<std::uint64_t> external_only_modifiers;
  };

  struct dmabuf_eligibility_t {
    std::optional<std::string> capture_render_node;
    std::string encoder_render_node;
    platf::mem_type_e mem_type = platf::mem_type_e::system;
    bool egl_import_supported = false;
  };

  struct dmabuf_plane_t {
    int fd = -1;
    std::uint32_t chunk_offset = 0;
    std::uint32_t chunk_size = 0;
    std::int32_t stride = 0;
    std::uint32_t maxsize = 0;
  };

  struct dmabuf_frame_t {
    int width = 0;
    int height = 0;
    std::uint32_t spa_format = 0;
    std::uint32_t drm_fourcc = 0;
    std::uint64_t modifier = 0;
    std::array<dmabuf_plane_t, 4> planes {};
    std::uint32_t plane_count = 0;
  };

  std::vector<egl_dmabuf_format_t> query_egl_dmabuf_import_formats(const std::string &render_node);

  std::optional<std::uint32_t> drm_format_for_spa(std::uint32_t spa_format);
  std::optional<std::string> canonical_render_node(std::string_view path);
  // When the portal stream omits capture render node, assume the operator-pinned
  // encoder adapter (same-GPU headless/gamescope). Enables DmaBuf offer; MemFd stays fallback.
  std::optional<std::string> resolve_capture_render_node(
    const std::optional<std::string> &portal_capture_render_node,
    const std::optional<std::string> &encoder_render_node
  );
  bool may_offer_dmabuf(const dmabuf_eligibility_t &eligibility);
  std::vector<dmabuf_format_modifier_t> task1_packed_dmabuf_formats(std::vector<std::uint64_t> modifiers);
  std::vector<dmabuf_format_modifier_t> filter_importable_dmabuf_formats(
    const std::vector<dmabuf_format_modifier_t> &portal_formats,
    const std::vector<egl_dmabuf_format_t> &egl_formats
  );
  bool valid_dmabuf_frame(const dmabuf_frame_t &frame);
  std::vector<std::uint32_t> offered_buffer_data_types(bool may_use_dmabuf);
  bool fill_dmabuf_descriptor(const dmabuf_frame_t &frame, egl::img_descriptor_t &image);

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

  platf::frame_metadata_t cpu_frame_metadata(std::uint32_t spa_format = SPA_VIDEO_FORMAT_BGRx);
  platf::frame_metadata_t dmabuf_frame_metadata(std::string render_node, std::uint32_t spa_format = SPA_VIDEO_FORMAT_BGRx);
  bool frame_requires_cpu_copy(const platf::frame_metadata_t &metadata);

  /**
   * @brief Locate gamescope's exported Video/Source on the default PipeWire graph.
   *
   * gamescope publishes media.name=gamescope (node.name often .gamescope-wrapped).
   * Used for gamescopegrab (direct capture without private portal ScreenCast).
   */
  struct video_source_t {
    std::uint32_t node_id = 0;
    std::uint64_t object_serial = 0;
    std::string node_name;
  };
  std::optional<video_source_t> find_gamescope_video_source();

  class capture_t: public std::enable_shared_from_this<capture_t> {
  public:
    explicit capture_t(capture_options_t options);
    ~capture_t();

    capture_t(const capture_t &) = delete;
    capture_t &operator=(const capture_t &) = delete;

    bool start();
    // Wake waiters and mark capture terminal without destroying (SB-2 teardown).
    void stop();
    // Idempotently disconnect and destroy PipeWire resources even while other
    // shared_ptr aliases still retain this capture generation.
    void shutdown();
    bool running() const;
    bool negotiated() const;
    bool negotiated_dmabuf() const;
    frame_info_t frame_info() const;
    wait_result_e wait_for_frame(std::chrono::milliseconds timeout);
    bool fill_frame(std::shared_ptr<platf::img_t> &image);

  private:
    static void on_process(void *userdata) noexcept;
    static void on_add_buffer(void *userdata, struct pw_buffer *buffer) noexcept;
    static void on_remove_buffer(void *userdata, struct pw_buffer *buffer) noexcept;
    static void on_param_changed(void *userdata, std::uint32_t id, const struct spa_pod *param) noexcept;
    static void on_state_changed(void *userdata, enum pw_stream_state old, enum pw_stream_state state, const char *errmsg) noexcept;

    bool process_buffer(struct pw_buffer *buffer);
    void set_terminal(wait_result_e result);
    void queue_buffer(struct pw_buffer *buffer);
    void release_buffer_lease(struct pw_buffer *buffer);

    capture_options_t options_;
    int remote_fd_ = -1;
    pw_thread_loop *loop_ = nullptr;
    pw_context *context_ = nullptr;
    pw_core *core_ = nullptr;
    pw_stream *stream_ = nullptr;
    spa_hook stream_listener_ {};
    std::unordered_map<struct pw_buffer *, std::uint64_t> buffer_keys_;

    std::mutex shutdown_mtx_;
    bool shutdown_complete_ = false;

    mutable std::mutex frame_mtx_;
    std::condition_variable frame_cv_;
    std::vector<std::uint8_t> front_frame_;
    std::vector<std::uint8_t> back_frame_;
    std::optional<dmabuf_frame_t> front_dmabuf_frame_;
    std::chrono::steady_clock::time_point front_dmabuf_timestamp_ {};
    std::uint64_t front_dmabuf_sequence_ = 0;
    std::uint64_t front_dmabuf_buffer_key_ = 0;
    struct pw_buffer *front_dmabuf_buffer_ = nullptr;
    std::size_t active_dmabuf_leases_ = 0;
    frame_info_t front_info_;
    bool frame_available_ = false;
    bool negotiated_ = false;
    bool negotiated_dmabuf_ = false;
    bool running_ = false;
    wait_result_e terminal_result_ = wait_result_e::timeout;
    std::uint64_t sequence_ = 0;
  };
}  // namespace pipewire_capture
