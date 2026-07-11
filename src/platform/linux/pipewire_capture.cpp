/**
 * @file src/platform/linux/pipewire_capture.cpp
 * @brief PipeWire capture format, copy, and frame metadata policy.
 */

#include "src/platform/linux/pipewire_capture.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cctype>
#include <cstring>
#include <limits>
#include <utility>

#include <drm_fourcc.h>
#include <fcntl.h>
#include <gbm.h>
#include <spa/buffer/meta.h>
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

    bool gpu_encoder_mem_type(platf::mem_type_e mem_type) {
      return mem_type == platf::mem_type_e::vaapi || mem_type == platf::mem_type_e::cuda;
    }

    bool contains_modifier(const std::vector<std::uint64_t> &modifiers, std::uint64_t modifier) {
      return std::find(modifiers.begin(), modifiers.end(), modifier) != modifiers.end();
    }

    constexpr std::array<std::uint32_t, 4> packed_spa_formats {
      SPA_VIDEO_FORMAT_BGRx,
      SPA_VIDEO_FORMAT_BGRA,
      SPA_VIDEO_FORMAT_RGBx,
      SPA_VIDEO_FORMAT_RGBA,
    };

    using egl_query_formats_fn = EGLBoolean (*)(EGLDisplay, EGLint, EGLint *, EGLint *);
    using egl_query_modifiers_fn = EGLBoolean (*)(EGLDisplay, EGLint, EGLint, EGLuint64KHR *, EGLBoolean *, EGLint *);
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

  std::optional<std::string> canonical_render_node(std::string_view path) {
    constexpr std::string_view prefix = "/dev/dri/renderD";
    if (!path.starts_with(prefix) || path.size() == prefix.size()) {
      return std::nullopt;
    }

    for (auto i = prefix.size(); i < path.size(); ++i) {
      if (!std::isdigit(static_cast<unsigned char>(path[i]))) {
        return std::nullopt;
      }
    }

    return std::string(path);
  }

  bool may_offer_dmabuf(const dmabuf_eligibility_t &eligibility) {
    if (!eligibility.capture_render_node) {
      return false;
    }
    const auto capture_node = canonical_render_node(*eligibility.capture_render_node);
    const auto encoder_node = canonical_render_node(eligibility.encoder_render_node);
    return capture_node && encoder_node &&
           *capture_node == *encoder_node &&
           gpu_encoder_mem_type(eligibility.mem_type) &&
           eligibility.egl_import_supported;
  }

  std::vector<dmabuf_format_modifier_t> task1_packed_dmabuf_formats(std::vector<std::uint64_t> modifiers) {
    std::vector<dmabuf_format_modifier_t> result;
    for (const auto spa_format : packed_spa_formats) {
      const auto drm_format = drm_format_for_spa(spa_format);
      if (!drm_format) {
        continue;
      }
      for (const auto modifier : modifiers) {
        result.push_back({
          .spa_format = spa_format,
          .drm_fourcc = *drm_format,
          .modifier = modifier,
        });
      }
    }
    return result;
  }

  std::vector<dmabuf_format_modifier_t> filter_importable_dmabuf_formats(
    const std::vector<dmabuf_format_modifier_t> &portal_formats,
    const std::vector<egl_dmabuf_format_t> &egl_formats
  ) {
    std::vector<dmabuf_format_modifier_t> result;
    for (const auto &candidate : portal_formats) {
      const auto drm_format = drm_format_for_spa(candidate.spa_format);
      if (!drm_format || *drm_format != candidate.drm_fourcc) {
        continue;
      }

      auto egl_format = std::find_if(egl_formats.begin(), egl_formats.end(), [&](const auto &format) {
        return format.drm_fourcc == candidate.drm_fourcc;
      });
      if (egl_format == egl_formats.end()) {
        continue;
      }
      if (!contains_modifier(egl_format->modifiers, candidate.modifier)) {
        continue;
      }
      if (contains_modifier(egl_format->external_only_modifiers, candidate.modifier)) {
        continue;
      }

      result.push_back(candidate);
    }
    return result;
  }

  bool valid_dmabuf_frame(const dmabuf_frame_t &frame) {
    // Task 3 is deliberately limited to packed 32-bit RGB DRM formats, which
    // are represented by exactly one plane.
    if (frame.width <= 0 || frame.height <= 0 || frame.plane_count != 1) {
      return false;
    }
    const auto expected_fourcc = drm_format_for_spa(frame.spa_format);
    if (!expected_fourcc || *expected_fourcc != frame.drm_fourcc) {
      return false;
    }

    const auto &plane = frame.planes[0];
    if (plane.fd < 0 || plane.stride <= 0 || plane.maxsize == 0 || plane.chunk_size == 0) {
      return false;
    }
    const auto width_bytes = static_cast<std::uint64_t>(frame.width) * 4u;
    if (static_cast<std::uint64_t>(plane.stride) < width_bytes) {
      return false;
    }
    const auto required_bytes = static_cast<std::uint64_t>(frame.height - 1) *
                                  static_cast<std::uint64_t>(plane.stride) + width_bytes;
    const auto offset = plane.chunk_offset % plane.maxsize;
    if (plane.chunk_size > plane.maxsize ||
        offset > plane.maxsize - plane.chunk_size ||
        required_bytes > plane.chunk_size) {
      return false;
    }
    return true;
  }

  std::uint32_t negotiated_buffer_data_type(bool dmabuf_negotiated) {
    return dmabuf_negotiated ? SPA_DATA_DmaBuf : SPA_DATA_MemPtr;
  }

  std::vector<std::uint32_t> offered_buffer_data_types(bool may_use_dmabuf) {
    if (may_use_dmabuf) {
      return {SPA_DATA_DmaBuf, SPA_DATA_MemPtr};
    }
    return {SPA_DATA_MemPtr};
  }

  bool fill_dmabuf_descriptor(const dmabuf_frame_t &frame, egl::img_descriptor_t &image) {
    image.reset();
    if (!valid_dmabuf_frame(frame)) {
      return false;
    }

    image.sd = {
      .width = frame.width,
      .height = frame.height,
      .fds = {-1, -1, -1, -1},
      .fourcc = frame.drm_fourcc,
      .modifier = frame.modifier,
      .pitches = {},
      .offsets = {},
    };

    for (std::uint32_t i = 0; i < frame.plane_count; ++i) {
      const auto &plane = frame.planes[i];
      const auto fd = dup(plane.fd);
      if (fd < 0) {
        BOOST_LOG(warning) << "portal: failed to duplicate DMA-BUF plane fd: "sv << std::strerror(errno);
        image.reset();
        return false;
      }

      image.sd.fds[i] = fd;
      image.sd.pitches[i] = static_cast<std::uint32_t>(plane.stride);
      // spa_data::mapoffset is the page-aligned mmap offset. EGL's plane
      // offset is the normalized spa_chunk offset inside the DMA-BUF.
      image.sd.offsets[i] = plane.chunk_offset % plane.maxsize;
    }

    return true;
  }

  std::vector<egl_dmabuf_format_t> query_egl_dmabuf_import_formats(const std::string &render_node) {
    std::vector<egl_dmabuf_format_t> result;
    const auto canonical = canonical_render_node(render_node);
    if (!canonical) {
      return result;
    }

    if (gbm::init()) {
      BOOST_LOG(warning) << "portal: GBM initialization failed while checking DMA-BUF import support"sv;
      return result;
    }

    file_t render_fd(open(canonical->c_str(), O_RDWR | O_CLOEXEC));
    if (render_fd.el < 0) {
      BOOST_LOG(warning) << "portal: cannot open encoder render node ["sv << *canonical
                         << "] for DMA-BUF import query: "sv << std::strerror(errno);
      return result;
    }

    gbm::gbm_t gbm_device(gbm::create_device(render_fd.el));
    if (!gbm_device) {
      BOOST_LOG(warning) << "portal: cannot create GBM device for encoder render node ["sv << *canonical << ']';
      return result;
    }

    auto egl_display = egl::make_display(gbm_device.get());
    if (!egl_display) {
      BOOST_LOG(warning) << "portal: cannot create EGL display for encoder render node ["sv << *canonical << ']';
      return result;
    }

    auto *query_formats = reinterpret_cast<egl_query_formats_fn>(eglGetProcAddress("eglQueryDmaBufFormatsEXT"));
    auto *query_modifiers = reinterpret_cast<egl_query_modifiers_fn>(eglGetProcAddress("eglQueryDmaBufModifiersEXT"));
    if (!query_formats || !query_modifiers) {
      BOOST_LOG(warning) << "portal: EGL DMA-BUF query functions are unavailable for encoder render node ["sv << *canonical << ']';
      return result;
    }

    EGLint format_count = 0;
    if (!query_formats(egl_display.get(), 0, nullptr, &format_count) || format_count <= 0) {
      return result;
    }

    std::vector<EGLint> formats(static_cast<std::size_t>(format_count));
    if (!query_formats(egl_display.get(), format_count, formats.data(), &format_count)) {
      return result;
    }

    for (EGLint i = 0; i < format_count; ++i) {
      EGLint modifier_count = 0;
      if (!query_modifiers(egl_display.get(), formats[i], 0, nullptr, nullptr, &modifier_count) || modifier_count <= 0) {
        continue;
      }

      std::vector<EGLuint64KHR> modifiers(static_cast<std::size_t>(modifier_count));
      std::vector<EGLBoolean> external_only(static_cast<std::size_t>(modifier_count));
      if (!query_modifiers(egl_display.get(), formats[i], modifier_count, modifiers.data(), external_only.data(), &modifier_count)) {
        continue;
      }

      egl_dmabuf_format_t format {
        .drm_fourcc = static_cast<std::uint32_t>(formats[i]),
      };
      for (EGLint j = 0; j < modifier_count; ++j) {
        const auto modifier = static_cast<std::uint64_t>(modifiers[j]);
        format.modifiers.push_back(modifier);
        if (external_only[j]) {
          format.external_only_modifiers.push_back(modifier);
        }
      }
      result.push_back(std::move(format));
    }

    return result;
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

  bool frame_requires_cpu_copy(const platf::frame_metadata_t &metadata) {
    return metadata.transport != platf::frame_transport_e::dmabuf ||
           metadata.residency != platf::frame_residency_e::gpu;
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

    const spa_rectangle default_size {
      .width = static_cast<std::uint32_t>(std::max(options_.requested_width, 1)),
      .height = static_cast<std::uint32_t>(std::max(options_.requested_height, 1)),
    };
    const spa_rectangle min_size {.width = 1, .height = 1};
    const spa_rectangle max_size {.width = 16384, .height = 16384};
    std::vector<std::vector<std::uint8_t>> params_storage;
    std::vector<const spa_pod *> params;
    params_storage.reserve(options_.dmabuf_formats.size() + 1);
    params.reserve(options_.dmabuf_formats.size() + 1);

    if (options_.may_use_dmabuf) {
      for (const auto &format : options_.dmabuf_formats) {
        params_storage.emplace_back(1024);
        spa_pod_builder pb = SPA_POD_BUILDER_INIT(params_storage.back().data(), static_cast<std::uint32_t>(params_storage.back().size()));
        spa_pod_frame frame {};
        spa_pod_builder_push_object(&pb, &frame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
        spa_pod_builder_add(&pb,
          SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
          SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
          SPA_FORMAT_VIDEO_format, SPA_POD_Id(format.spa_format),
          SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&default_size, &min_size, &max_size),
          0);
        spa_pod_builder_prop(&pb, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY);
        spa_pod_builder_long(&pb, static_cast<std::int64_t>(format.modifier));
        params.push_back(reinterpret_cast<const spa_pod *>(spa_pod_builder_pop(&pb, &frame)));
      }
    }

    params_storage.emplace_back(1024);
    spa_pod_builder mem_pb = SPA_POD_BUILDER_INIT(params_storage.back().data(), static_cast<std::uint32_t>(params_storage.back().size()));
    const auto *memptr_fmt_param = reinterpret_cast<const spa_pod *>(spa_pod_builder_add_object(
      &mem_pb,
      SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
      SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
      SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
      SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(5,
        SPA_VIDEO_FORMAT_BGRx,
        SPA_VIDEO_FORMAT_BGRx,
        SPA_VIDEO_FORMAT_BGRA,
        SPA_VIDEO_FORMAT_RGBx,
        SPA_VIDEO_FORMAT_RGBA),
      SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&default_size, &min_size, &max_size)));
    params.push_back(memptr_fmt_param);

    const auto target_id = options_.node_id;
    if (pw_stream_connect(stream_,
          PW_DIRECTION_INPUT,
          target_id,
          static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
          params.data(), params.size()) < 0) {
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

  bool capture_t::negotiated_dmabuf() const {
    std::lock_guard lk(frame_mtx_);
    return negotiated_dmabuf_;
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

  bool capture_t::fill_frame(std::shared_ptr<platf::img_t> &image) {
    std::lock_guard lk(frame_mtx_);
    if (!frame_available_) {
      return false;
    }

    if (front_dmabuf_frame_) {
      auto *descriptor = dynamic_cast<egl::img_descriptor_t *>(image.get());
      if (!descriptor) {
        return false;
      }
      if (!fill_dmabuf_descriptor(*front_dmabuf_frame_, *descriptor)) {
        return false;
      }

      descriptor->width = front_dmabuf_frame_->width;
      descriptor->height = front_dmabuf_frame_->height;
      descriptor->pixel_pitch = 4;
      descriptor->row_pitch = front_dmabuf_frame_->width * 4;
      descriptor->data = nullptr;
      descriptor->sequence = front_dmabuf_sequence_;
      descriptor->dmabuf_buffer_key = reinterpret_cast<std::uintptr_t>(front_dmabuf_buffer_->buffer);
      descriptor->frame_timestamp = front_dmabuf_timestamp_;
      descriptor->frame_metadata = dmabuf_frame_metadata(options_.capture_render_node.value_or(std::string {}));

      auto retained_buffer = front_dmabuf_buffer_;
      auto pooled = std::move(image);
      auto *pooled_image = pooled.get();
      front_dmabuf_buffer_ = nullptr;
      front_dmabuf_frame_.reset();
      frame_available_ = false;
      std::weak_ptr<capture_t> weak_self = weak_from_this();
      image = std::shared_ptr<platf::img_t>(pooled_image, [pooled = std::move(pooled), weak_self, retained_buffer](platf::img_t *) mutable {
        if (auto self = weak_self.lock()) {
          self->queue_buffer(retained_buffer);
        }
        pooled.reset();
      });
      return true;
    }

    if (image && !image->data) {
      if (auto *cursor = dynamic_cast<egl::cursor_t *>(image.get()); cursor && !cursor->buffer.empty()) {
        image->data = cursor->buffer.data();
      }
    }
    if (!image || !image->data || image->width != front_info_.width || image->height != front_info_.height) {
      return false;
    }

    const auto width_bytes = static_cast<std::size_t>(front_info_.width) * 4u;
    if (front_info_.stride <= 0 || image->row_pitch < 0 ||
        static_cast<std::size_t>(front_info_.stride) < width_bytes ||
        static_cast<std::size_t>(image->row_pitch) < width_bytes) {
      return false;
    }

    for (int y = 0; y < front_info_.height; ++y) {
      std::memcpy(
        image->data + static_cast<std::size_t>(y) * static_cast<std::size_t>(image->row_pitch),
        front_frame_.data() + static_cast<std::size_t>(y) * static_cast<std::size_t>(front_info_.stride),
        width_bytes);
    }

    image->frame_timestamp = std::chrono::steady_clock::now();
    image->frame_metadata = cpu_frame_metadata();
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

    if (!cap->process_buffer(latest)) {
      pw_stream_queue_buffer(cap->stream_, latest);
    }
  }

  bool capture_t::process_buffer(pw_buffer *buffer) {
    if (!buffer || !buffer->buffer || buffer->buffer->n_datas == 0) {
      return false;
    }

    const auto *data = &buffer->buffer->datas[0];
    if (!data->chunk || (data->chunk->flags & SPA_CHUNK_FLAG_CORRUPTED)) {
      return false;
    }
    if (data->type != SPA_DATA_MemPtr && data->type != SPA_DATA_DmaBuf) {
      set_terminal(wait_result_e::reinit);
      return false;
    }

    frame_info_t info;
    {
      std::lock_guard lk(frame_mtx_);
      info = front_info_;
    }
    if (info.width <= 0 || info.height <= 0) {
      return false;
    }

    if (data->type == SPA_DATA_DmaBuf) {
      auto drm_format = drm_format_for_spa(info.spa_format);
      if (!drm_format || !negotiated_dmabuf_) {
        set_terminal(wait_result_e::reinit);
        return false;
      }

      dmabuf_frame_t frame {
        .width = info.width,
        .height = info.height,
        .spa_format = info.spa_format,
        .drm_fourcc = *drm_format,
        .modifier = info.modifier,
        .plane_count = buffer->buffer->n_datas,
      };
      if (frame.plane_count > frame.planes.size()) {
        BOOST_LOG(warning) << "portal: rejecting DMA-BUF frame with invalid plane count "sv << frame.plane_count;
        return false;
      }

      for (std::uint32_t i = 0; i < frame.plane_count; ++i) {
        const auto &plane_data = buffer->buffer->datas[i];
        if (!plane_data.chunk || (plane_data.chunk->flags & SPA_CHUNK_FLAG_CORRUPTED)) {
          return false;
        }
        if (plane_data.type != SPA_DATA_DmaBuf) {
          return false;
        }
        frame.planes[i] = {
          .fd = static_cast<int>(plane_data.fd),
          .chunk_offset = plane_data.chunk->offset,
          .chunk_size = plane_data.chunk->size,
          .stride = plane_data.chunk->stride,
          .maxsize = plane_data.maxsize,
        };
      }

      auto frame_timestamp = std::chrono::steady_clock::now();
      // Polaris reserves sequence 0 for dummy images and the encoder cache
      // expects a strictly increasing sequence for every published frame.
      const auto frame_sequence = ++sequence_;

      pw_buffer *replaced_buffer = nullptr;
      {
        std::lock_guard lk(frame_mtx_);
        replaced_buffer = front_dmabuf_buffer_;
        front_dmabuf_buffer_ = nullptr;
        front_dmabuf_frame_ = frame;
        front_dmabuf_timestamp_ = frame_timestamp;
        front_dmabuf_sequence_ = frame_sequence;
        front_dmabuf_buffer_ = buffer;
        frame_available_ = true;
      }
      if (replaced_buffer) {
        queue_buffer(replaced_buffer);
      }
      frame_cv_.notify_one();
      return true;
    }

    if (!data->data) {
      return false;
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
      return false;
    }

    info.stride = static_cast<int>(staging_stride);
    {
      std::lock_guard lk(frame_mtx_);
      front_frame_.swap(back_frame_);
      front_info_ = info;
      front_dmabuf_frame_.reset();
      frame_available_ = true;
    }
    frame_cv_.notify_one();
    return false;
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
    const auto has_modifier = (raw_info.flags & SPA_VIDEO_FLAG_MODIFIER) &&
                              raw_info.modifier != DRM_FORMAT_MOD_INVALID;
    const auto offered_dmabuf = has_modifier && std::any_of(
      cap->options_.dmabuf_formats.begin(), cap->options_.dmabuf_formats.end(),
      [&](const auto &format) {
        return format.spa_format == raw_info.format && format.modifier == raw_info.modifier;
      });
    if (has_modifier && (!cap->options_.may_use_dmabuf || !offered_dmabuf)) {
      BOOST_LOG(warning) << "portal: rejecting unoffered PipeWire DMA-BUF format/modifier pair"sv;
      cap->set_terminal(wait_result_e::reinit);
      return;
    }
    const auto dmabuf_negotiated = cap->options_.may_use_dmabuf && offered_dmabuf;
    if (dmabuf_negotiated) {
      BOOST_LOG(info) << "portal: PipeWire DMA-BUF format negotiated with modifier="sv
                      << raw_info.modifier;
    }

    pw_buffer *replaced_buffer = nullptr;
    bool requires_reinit = false;
    {
      std::lock_guard lk(cap->frame_mtx_);
      const auto width = static_cast<int>(raw_info.size.width);
      const auto height = static_cast<int>(raw_info.size.height);
      requires_reinit = cap->negotiated_ &&
                        (cap->negotiated_dmabuf_ != dmabuf_negotiated ||
                         cap->front_info_.width != width ||
                         cap->front_info_.height != height);
      replaced_buffer = cap->front_dmabuf_buffer_;
      cap->front_info_ = {
        .width = width,
        .height = height,
        .stride = width * 4,
        .spa_format = raw_info.format,
        .modifier = raw_info.flags & SPA_VIDEO_FLAG_MODIFIER ? raw_info.modifier : DRM_FORMAT_MOD_INVALID,
      };
      cap->front_frame_.clear();
      cap->back_frame_.clear();
      cap->front_dmabuf_frame_.reset();
      cap->front_dmabuf_buffer_ = nullptr;
      cap->frame_available_ = false;
      cap->negotiated_ = true;
      cap->negotiated_dmabuf_ = dmabuf_negotiated;
    }
    if (replaced_buffer) {
      cap->queue_buffer(replaced_buffer);
    }
    cap->frame_cv_.notify_all();

    BOOST_LOG(info) << "portal: PipeWire format negotiated: "sv
                    << raw_info.size.width << "x"sv << raw_info.size.height
                    << " format="sv << spa_debug_type_find_name(spa_type_video_format, raw_info.format);

    if (requires_reinit) {
      BOOST_LOG(info) << "portal: PipeWire capture transport or dimensions changed; reinitializing encoder"sv;
      cap->set_terminal(wait_result_e::reinit);
      return;
    }

    uint8_t params_buffer[1024];
    spa_pod_builder pb = SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
    const auto data_type = negotiated_buffer_data_type(dmabuf_negotiated);
    const auto *buf_param = reinterpret_cast<const spa_pod *>(spa_pod_builder_add_object(
      &pb,
      SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
      SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 8),
      SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(1 << data_type)));

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

  void capture_t::queue_buffer(pw_buffer *buffer) {
    if (!buffer || !stream_) {
      return;
    }
    if (loop_ && !pw_thread_loop_in_thread(loop_)) {
      pw_thread_loop_lock(loop_);
      pw_stream_queue_buffer(stream_, buffer);
      pw_thread_loop_unlock(loop_);
      return;
    }
    pw_stream_queue_buffer(stream_, buffer);
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
