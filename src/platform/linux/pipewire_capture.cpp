/**
 * @file src/platform/linux/pipewire_capture.cpp
 * @brief PipeWire capture format, copy, and frame metadata policy.
 */

#include "src/platform/linux/pipewire_capture.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <utility>

#include <drm_fourcc.h>
#include <pipewire/pipewire.h>
#include <spa/debug/types.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/format.h>
#include <spa/param/video/raw.h>
#include <spa/param/video/type-info.h>
#include <spa/utils/result.h>
#include <unistd.h>

#include "src/logging.h"

using namespace std::literals;

namespace pipewire_capture {
  namespace {
    void init_pipewire_once() {
      static std::once_flag init_flag;
      std::call_once(init_flag, [] {
        pw_init(nullptr, nullptr);
      });
    }

    stream_state_e from_pw_state(pw_stream_state state) {
      switch (state) {
        case PW_STREAM_STATE_UNCONNECTED:
          return stream_state_e::unconnected;
        case PW_STREAM_STATE_CONNECTING:
          return stream_state_e::connecting;
        case PW_STREAM_STATE_PAUSED:
          return stream_state_e::paused;
        case PW_STREAM_STATE_STREAMING:
          return stream_state_e::streaming;
        case PW_STREAM_STATE_ERROR:
          return stream_state_e::error;
      }

      return stream_state_e::error;
    }

    bool format_needs_rb_swap(std::uint32_t spa_format) {
      return spa_format == SPA_VIDEO_FORMAT_RGBx || spa_format == SPA_VIDEO_FORMAT_RGBA;
    }

    bool supported_bgra_source(std::uint32_t spa_format) {
      return spa_format == SPA_VIDEO_FORMAT_BGRx ||
             spa_format == SPA_VIDEO_FORMAT_BGRA ||
             spa_format == SPA_VIDEO_FORMAT_RGBx ||
             spa_format == SPA_VIDEO_FORMAT_RGBA;
    }
  }  // namespace

  std::optional<std::uint32_t> drm_format_for_spa(std::uint32_t spa_format) {
    switch (spa_format) {
      case SPA_VIDEO_FORMAT_BGRx:
        return DRM_FORMAT_XRGB8888;
      case SPA_VIDEO_FORMAT_BGRA:
        return DRM_FORMAT_ARGB8888;
      case SPA_VIDEO_FORMAT_RGBx:
        return DRM_FORMAT_XBGR8888;
      case SPA_VIDEO_FORMAT_RGBA:
        return DRM_FORMAT_ABGR8888;
      default:
        return std::nullopt;
    }
  }

  std::size_t safe_row_copy_bytes(
    std::size_t width_bytes,
    std::int32_t source_stride,
    std::size_t destination_stride
  ) {
    if (source_stride <= 0) {
      return 0;
    }

    return std::min({width_bytes, static_cast<std::size_t>(source_stride), destination_stride});
  }

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
  ) {
    if (!source || !destination || width <= 0 || height <= 0 || source_stride <= 0) {
      return false;
    }
    if (!supported_bgra_source(spa_format)) {
      return false;
    }
    if (chunk_offset > source_maxsize || chunk_size > source_maxsize - chunk_offset) {
      return false;
    }

    const auto width_bytes = static_cast<std::size_t>(width) * 4u;
    if (destination_stride < width_bytes || static_cast<std::size_t>(source_stride) < width_bytes) {
      return false;
    }

    const auto source_required = static_cast<std::size_t>(height - 1) * static_cast<std::size_t>(source_stride) + width_bytes;
    if (source_required > chunk_size) {
      return false;
    }

    const auto *payload = source + chunk_offset;
    if (format_needs_rb_swap(spa_format)) {
      for (int y = 0; y < height; ++y) {
        const auto *src_row = payload + static_cast<std::size_t>(y) * static_cast<std::size_t>(source_stride);
        auto *dst_row = destination + static_cast<std::size_t>(y) * destination_stride;
        for (int x = 0; x < width; ++x) {
          const auto src = x * 4;
          dst_row[src + 0] = src_row[src + 2];
          dst_row[src + 1] = src_row[src + 1];
          dst_row[src + 2] = src_row[src + 0];
          dst_row[src + 3] = src_row[src + 3];
        }
      }
      return true;
    }

    for (int y = 0; y < height; ++y) {
      std::memcpy(
        destination + static_cast<std::size_t>(y) * destination_stride,
        payload + static_cast<std::size_t>(y) * static_cast<std::size_t>(source_stride),
        width_bytes);
    }
    return true;
  }

  bool is_terminal_transition(stream_state_e old_state, stream_state_e new_state) {
    if (new_state == stream_state_e::error || new_state == stream_state_e::unconnected) {
      return true;
    }

    return old_state == stream_state_e::streaming && new_state == stream_state_e::paused;
  }

  platf::frame_metadata_t cpu_frame_metadata() {
    return {
      .transport = platf::frame_transport_e::shm,
      .residency = platf::frame_residency_e::cpu,
      .format = platf::frame_format_e::bgra8,
    };
  }

  platf::frame_metadata_t dmabuf_frame_metadata(std::string render_node) {
    return {
      .transport = platf::frame_transport_e::dmabuf,
      .residency = platf::frame_residency_e::gpu,
      .format = platf::frame_format_e::bgra8,
      .device = std::move(render_node),
    };
  }

  capture_t::capture_t(capture_options_t options):
      options_(options) {
    if (options_.remote_fd >= 0) {
      remote_fd_ = dup(options_.remote_fd);
      if (remote_fd_ < 0) {
        BOOST_LOG(warning) << "portal: failed to duplicate PipeWire remote fd: "sv << std::strerror(errno);
      }
    }
  }

  capture_t::~capture_t() {
    {
      std::lock_guard lk(frame_mtx_);
      running_ = false;
      terminal_result_ = wait_result_e::reinit;
      frame_cv_.notify_all();
    }

    if (loop_) {
      pw_thread_loop_stop(loop_);
    }
    if (stream_) {
      pw_stream_destroy(stream_);
      stream_ = nullptr;
    }
    if (core_) {
      pw_core_disconnect(core_);
      core_ = nullptr;
    }
    if (context_) {
      pw_context_destroy(context_);
      context_ = nullptr;
    }
    if (loop_) {
      pw_thread_loop_destroy(loop_);
      loop_ = nullptr;
    }
    if (remote_fd_ >= 0) {
      close(remote_fd_);
      remote_fd_ = -1;
    }
  }

  bool capture_t::start() {
    if (remote_fd_ < 0 || options_.node_id == 0) {
      return false;
    }

    init_pipewire_once();

    loop_ = pw_thread_loop_new("polaris-portal-capture", nullptr);
    if (!loop_) {
      BOOST_LOG(warning) << "portal: Failed to create PipeWire loop"sv;
      return false;
    }

    context_ = pw_context_new(pw_thread_loop_get_loop(loop_), nullptr, 0);
    if (!context_) {
      BOOST_LOG(warning) << "portal: Failed to create PipeWire context"sv;
      return false;
    }

    const auto core_fd = std::exchange(remote_fd_, -1);
    core_ = pw_context_connect_fd(context_, core_fd, nullptr, 0);
    if (!core_) {
      BOOST_LOG(warning) << "portal: Failed to connect PipeWire remote fd"sv;
      return false;
    }

    auto *props = pw_properties_new(
      PW_KEY_MEDIA_TYPE, "Video",
      PW_KEY_MEDIA_CATEGORY, "Capture",
      PW_KEY_MEDIA_ROLE, "Screen",
      nullptr);

    // The portal-returned node ID belongs to this private PipeWire remote and is
    // the authoritative target. Keep the object serial for diagnostics and for a
    // future registry-based lookup, but do not make startup depend on an
    // asynchronous serial target that cannot be safely retried from callbacks.
    stream_ = pw_stream_new(core_, "polaris-portal-capture", props);
    if (!stream_) {
      BOOST_LOG(warning) << "portal: Failed to create PipeWire stream"sv;
      return false;
    }

    static const pw_stream_events events = {
      .version = PW_VERSION_STREAM_EVENTS,
      .state_changed = capture_t::on_state_changed,
      .param_changed = capture_t::on_param_changed,
      .process = capture_t::on_process,
    };
    pw_stream_add_listener(stream_, &stream_listener_, &events, this);

    uint8_t params_buffer[1024];
    struct spa_pod_builder pb = SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
    const spa_rectangle default_size {
      .width = static_cast<std::uint32_t>(std::max(options_.requested_width, 1)),
      .height = static_cast<std::uint32_t>(std::max(options_.requested_height, 1)),
    };
    const spa_rectangle min_size {.width = 1, .height = 1};
    const spa_rectangle max_size {.width = 16384, .height = 16384};

    const auto *fmt_param = reinterpret_cast<const spa_pod *>(spa_pod_builder_add_object(
      &pb,
      SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
      SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
      SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
      SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(5,
        SPA_VIDEO_FORMAT_BGRx,
        SPA_VIDEO_FORMAT_BGRx,
        SPA_VIDEO_FORMAT_BGRA,
        SPA_VIDEO_FORMAT_RGBx,
        SPA_VIDEO_FORMAT_RGBA),
      SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(
        &default_size,
        &min_size,
        &max_size)));

    const auto target_id = options_.node_id;
    if (pw_stream_connect(stream_,
          PW_DIRECTION_INPUT,
          target_id,
          static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
          &fmt_param, 1) < 0) {
      BOOST_LOG(warning) << "portal: Failed to connect PipeWire stream to node "sv << options_.node_id;
      return false;
    }

    {
      std::lock_guard lk(frame_mtx_);
      running_ = true;
      terminal_result_ = wait_result_e::timeout;
    }
    if (pw_thread_loop_start(loop_) < 0) {
      BOOST_LOG(warning) << "portal: Failed to start PipeWire loop"sv;
      set_terminal(wait_result_e::error);
      return false;
    }

    BOOST_LOG(info) << "portal: PipeWire capture started on node "sv << options_.node_id
                    << " serial="sv << options_.node_serial;
    return true;
  }

  bool capture_t::running() const {
    std::lock_guard lk(frame_mtx_);
    return running_;
  }

  bool capture_t::negotiated() const {
    std::lock_guard lk(frame_mtx_);
    return negotiated_;
  }

  frame_info_t capture_t::frame_info() const {
    std::lock_guard lk(frame_mtx_);
    return front_info_;
  }

  wait_result_e capture_t::wait_for_frame(std::chrono::milliseconds timeout) {
    std::unique_lock lk(frame_mtx_);
    if (!frame_cv_.wait_for(lk, timeout, [&] {
          return frame_available_ || terminal_result_ == wait_result_e::reinit || terminal_result_ == wait_result_e::error;
        })) {
      return wait_result_e::timeout;
    }

    if (terminal_result_ == wait_result_e::reinit || terminal_result_ == wait_result_e::error) {
      return terminal_result_;
    }

    return wait_result_e::frame;
  }

  bool capture_t::copy_frame_to(platf::img_t &image) {
    std::lock_guard lk(frame_mtx_);
    if (!frame_available_) {
      return false;
    }

    if (!image.data || image.width != front_info_.width || image.height != front_info_.height) {
      return false;
    }

    const auto width_bytes = static_cast<std::size_t>(front_info_.width) * 4u;
    if (front_info_.stride <= 0 || image.row_pitch < 0 ||
        static_cast<std::size_t>(front_info_.stride) < width_bytes ||
        static_cast<std::size_t>(image.row_pitch) < width_bytes) {
      return false;
    }

    for (int y = 0; y < front_info_.height; ++y) {
      std::memcpy(
        image.data + static_cast<std::size_t>(y) * static_cast<std::size_t>(image.row_pitch),
        front_frame_.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(front_info_.stride),
        width_bytes);
    }

    image.frame_timestamp = std::chrono::steady_clock::now();
    image.frame_metadata = cpu_frame_metadata();
    frame_available_ = false;
    return true;
  }

  void capture_t::on_process(void *userdata) {
    auto *cap = static_cast<capture_t *>(userdata);
    pw_buffer *latest = nullptr;
    std::vector<pw_buffer *> older;
    while (auto *buffer = pw_stream_dequeue_buffer(cap->stream_)) {
      if (latest) {
        older.push_back(latest);
      }
      latest = buffer;
    }

    for (auto *buffer : older) {
      pw_stream_queue_buffer(cap->stream_, buffer);
    }
    if (!latest) {
      return;
    }

    cap->process_buffer(latest);
    pw_stream_queue_buffer(cap->stream_, latest);
  }

  void capture_t::process_buffer(pw_buffer *buffer) {
    if (!buffer || !buffer->buffer || buffer->buffer->n_datas == 0) {
      return;
    }

    const auto *data = &buffer->buffer->datas[0];
    if (!data->data || !data->chunk || (data->chunk->flags & SPA_CHUNK_FLAG_CORRUPTED)) {
      return;
    }
    if (data->type != SPA_DATA_MemPtr) {
      set_terminal(wait_result_e::reinit);
      return;
    }

    frame_info_t info;
    {
      std::lock_guard lk(frame_mtx_);
      info = front_info_;
    }
    if (info.width <= 0 || info.height <= 0) {
      return;
    }

    const auto width_bytes = static_cast<std::size_t>(info.width) * 4u;
    const auto staging_stride = width_bytes;
    back_frame_.resize(static_cast<std::size_t>(info.height) * staging_stride);

    if (!copy_memptr_frame_to_bgra(
          static_cast<const std::uint8_t *>(data->data),
          data->maxsize,
          data->chunk->offset,
          data->chunk->size,
          info.width,
          info.height,
          data->chunk->stride,
          info.spa_format,
          back_frame_.data(),
          staging_stride)) {
      return;
    }

    info.stride = static_cast<int>(staging_stride);
    {
      std::lock_guard lk(frame_mtx_);
      front_frame_.swap(back_frame_);
      front_info_ = info;
      frame_available_ = true;
    }
    frame_cv_.notify_one();
  }

  void capture_t::on_param_changed(void *userdata, std::uint32_t id, const spa_pod *param) {
    auto *cap = static_cast<capture_t *>(userdata);
    if (!param || id != SPA_PARAM_Format) {
      return;
    }

    std::uint32_t media_type = 0;
    std::uint32_t media_subtype = 0;
    if (spa_format_parse(param, &media_type, &media_subtype) < 0) {
      return;
    }
    if (media_type != SPA_MEDIA_TYPE_video || media_subtype != SPA_MEDIA_SUBTYPE_raw) {
      return;
    }

    spa_video_info_raw raw_info {};
    if (spa_format_video_raw_parse(param, &raw_info) < 0) {
      return;
    }
    if (!supported_bgra_source(raw_info.format)) {
      BOOST_LOG(warning) << "portal: unsupported PipeWire format: "sv
                         << spa_debug_type_find_name(spa_type_video_format, raw_info.format);
      cap->set_terminal(wait_result_e::reinit);
      return;
    }

    {
      std::lock_guard lk(cap->frame_mtx_);
      cap->front_info_ = {
        .width = static_cast<int>(raw_info.size.width),
        .height = static_cast<int>(raw_info.size.height),
        .stride = static_cast<int>(raw_info.size.width) * 4,
        .spa_format = raw_info.format,
      };
      cap->front_frame_.clear();
      cap->back_frame_.clear();
      cap->frame_available_ = false;
      cap->negotiated_ = true;
    }
    cap->frame_cv_.notify_all();

    BOOST_LOG(info) << "portal: PipeWire format negotiated: "sv
                    << raw_info.size.width << "x"sv << raw_info.size.height
                    << " format="sv << spa_debug_type_find_name(spa_type_video_format, raw_info.format);

    uint8_t params_buffer[1024];
    spa_pod_builder pb = SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
    const auto *buf_param = reinterpret_cast<const spa_pod *>(spa_pod_builder_add_object(
      &pb,
      SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
      SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 8),
      SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(1 << SPA_DATA_MemPtr)));

    pw_stream_update_params(cap->stream_, &buf_param, 1);
  }

  void capture_t::on_state_changed(void *userdata, pw_stream_state old, pw_stream_state state, const char *errmsg) {
    auto *cap = static_cast<capture_t *>(userdata);
    BOOST_LOG(info) << "portal: PipeWire state: "sv
                    << pw_stream_state_as_string(old) << " -> "sv
                    << pw_stream_state_as_string(state);
    if (state == PW_STREAM_STATE_ERROR && errmsg) {
      BOOST_LOG(warning) << "portal: PipeWire error: "sv << errmsg;
    }

    if (is_terminal_transition(from_pw_state(old), from_pw_state(state))) {
      cap->set_terminal(state == PW_STREAM_STATE_ERROR ? wait_result_e::error : wait_result_e::reinit);
    }
  }

  void capture_t::set_terminal(wait_result_e result) {
    {
      std::lock_guard lk(frame_mtx_);
      running_ = false;
      terminal_result_ = result;
    }
    frame_cv_.notify_all();
  }

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
  ) {
    return copy_memptr_frame_to_bgra(
      source, source_maxsize, chunk_offset, chunk_size, width, height, source_stride,
      spa_format, destination, destination_stride);
  }

  bool is_terminal_transition_for_tests(stream_state_e old_state, stream_state_e new_state) {
    return is_terminal_transition(old_state, new_state);
  }
#endif
}  // namespace pipewire_capture
