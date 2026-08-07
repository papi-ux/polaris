/**
 * @file src/platform/linux/pipewire_capture.cpp
 * @brief PipeWire capture format, copy, and frame metadata policy.
 */

#include "src/platform/linux/pipewire_capture.h"
#include "src/platform/linux/pipewire_transport_policy.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <cinttypes>
#include <cstdlib>
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
#include <spa/param/video/color.h>
#include <spa/param/video/format-utils.h>
#include <spa/param/video/format.h>
#include <spa/param/video/raw.h>
#include <spa/param/video/type-info.h>
#include <spa/utils/defs.h>
#include <spa/utils/result.h>
#include <unistd.h>

#ifndef SPA_ID_INVALID
  #define SPA_ID_INVALID ((uint32_t) 0xffffffff)
#endif

// Older spa headers (Ubuntu CI / pre-PipeWire PQ enum) lack ST 2084.
// Numeric value is stable in spa/param/video/color.h (after BT2020_10).
#ifndef SPA_VIDEO_TRANSFER_SMPTE2084
  #define SPA_VIDEO_TRANSFER_SMPTE2084 14
#endif

#include "src/logging.h"

using namespace std::literals;

namespace pipewire_capture {
  namespace {
    std::atomic<std::uint64_t> next_buffer_allocation_key {1};

    std::uint64_t allocate_buffer_key() {
      auto key = next_buffer_allocation_key.fetch_add(1, std::memory_order_relaxed);
      if (key == 0) {
        key = next_buffer_allocation_key.fetch_add(1, std::memory_order_relaxed);
      }
      return key;
    }

    void init_pipewire_once() {
      static std::once_flag init_flag;
      std::call_once(init_flag, [] {
        pw_init(nullptr, nullptr);
      });
    }

    bool format_needs_rb_swap(std::uint32_t spa_format) {
      return spa_format == SPA_VIDEO_FORMAT_RGBx || spa_format == SPA_VIDEO_FORMAT_RGBA;
    }

    bool supported_bgra_source(std::uint32_t spa_format) {
      return spa_format == SPA_VIDEO_FORMAT_xBGR_210LE ||
             spa_format == SPA_VIDEO_FORMAT_xRGB_210LE ||
             spa_format == SPA_VIDEO_FORMAT_BGRx ||
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

    // 10-bit first when both are offered. For true HDR capture (prefer_hdr_formats)
    // only the 10-bit entries are advertised — see connect path.
    constexpr std::array<std::uint32_t, 6> packed_spa_formats {
      SPA_VIDEO_FORMAT_xBGR_210LE,
      SPA_VIDEO_FORMAT_xRGB_210LE,
      SPA_VIDEO_FORMAT_BGRx,
      SPA_VIDEO_FORMAT_BGRA,
      SPA_VIDEO_FORMAT_RGBx,
      SPA_VIDEO_FORMAT_RGBA,
    };

    bool spa_format_is_hdr_rgb10(std::uint32_t spa_format) {
      return spa_format == SPA_VIDEO_FORMAT_xBGR_210LE ||
             spa_format == SPA_VIDEO_FORMAT_xRGB_210LE;
    }

    using egl_query_formats_fn = EGLBoolean (*)(EGLDisplay, EGLint, EGLint *, EGLint *);
    using egl_query_modifiers_fn = EGLBoolean (*)(EGLDisplay, EGLint, EGLint, EGLuint64KHR *, EGLBoolean *, EGLint *);
  }  // namespace

  std::optional<std::uint32_t> drm_format_for_spa(std::uint32_t spa_format) {
    switch (spa_format) {
      case SPA_VIDEO_FORMAT_xBGR_210LE:
        return DRM_FORMAT_XBGR2101010;
      case SPA_VIDEO_FORMAT_xRGB_210LE:
        return DRM_FORMAT_XRGB2101010;
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

  std::optional<std::string> resolve_capture_render_node(
    const std::optional<std::string> &portal_capture_render_node,
    const std::optional<std::string> &encoder_render_node
  ) {
    if (portal_capture_render_node) {
      if (auto node = canonical_render_node(*portal_capture_render_node)) {
        return node;
      }
    }
    return encoder_render_node;
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

  std::vector<std::uint32_t> offered_buffer_data_types(bool may_use_dmabuf) {
    std::vector<std::uint32_t> data_types;
    for (const auto transport : pipewire_transport::offered_buffer_transports(may_use_dmabuf)) {
      switch (transport) {
        case pipewire_transport::buffer_transport_e::dmabuf:
          data_types.push_back(SPA_DATA_DmaBuf);
          break;
        case pipewire_transport::buffer_transport_e::memfd:
          data_types.push_back(SPA_DATA_MemFd);
          break;
        case pipewire_transport::buffer_transport_e::memptr:
          data_types.push_back(SPA_DATA_MemPtr);
          break;
      }
    }
    return data_types;
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

  platf::frame_format_e spa_to_frame_format(std::uint32_t spa_format) {
    // p010 = 10-bit class source for stats (no dedicated rgb10 enum).
    if (spa_format == SPA_VIDEO_FORMAT_xBGR_210LE ||
        spa_format == SPA_VIDEO_FORMAT_xRGB_210LE) {
      return platf::frame_format_e::p010;
    }
    return platf::frame_format_e::bgra8;
  }

  platf::frame_metadata_t cpu_frame_metadata(std::uint32_t spa_format) {
    return {
      .transport = platf::frame_transport_e::shm,
      .residency = platf::frame_residency_e::cpu,
      .format = spa_to_frame_format(spa_format),
    };
  }

  platf::frame_metadata_t dmabuf_frame_metadata(std::string render_node, std::uint32_t spa_format) {
    return {
      .transport = platf::frame_transport_e::dmabuf,
      .residency = platf::frame_residency_e::gpu,
      .format = spa_to_frame_format(spa_format),
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
    shutdown();
  }

  void capture_t::shutdown() {
    std::lock_guard shutdown_lk(shutdown_mtx_);
    if (shutdown_complete_) {
      return;
    }
    shutdown_complete_ = true;
    {
      std::unique_lock lk(frame_mtx_);
      running_ = false;
      terminal_result_ = wait_result_e::reinit;
      front_dmabuf_buffer_ = nullptr;
      front_dmabuf_buffer_key_ = 0;
      front_dmabuf_frame_.reset();
      frame_available_ = false;
      frame_cv_.notify_all();
      frame_cv_.wait(lk, [this] { return active_dmabuf_leases_ == 0; });
    }

    // Sunshine-style teardown: hold the loop lock while disconnecting the stream
    // and core, then stop/destroy the loop. Avoids racing callbacks and matches
    // LizardByte/Sunshine pipewire.cpp (W1 micro-adopt).
    if (loop_) {
      pw_thread_loop_lock(loop_);
    }
    if (stream_) {
      pw_stream_set_active(stream_, false);
      pw_stream_disconnect(stream_);
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
    if (remote_fd_ >= 0) {
      close(remote_fd_);
      remote_fd_ = -1;
    }
    if (loop_) {
      pw_thread_loop_unlock(loop_);
      pw_thread_loop_stop(loop_);
      pw_thread_loop_destroy(loop_);
      loop_ = nullptr;
    }
    buffer_keys_.clear();
  }

  bool capture_t::start() {
    // Portal remote: remote_fd >= 0 + node_id. Local graph (gamescopegrab / kwin):
    // remote_fd < 0 + node_id and/or valid object serial.
    const bool has_node = options_.node_id != 0;
    const bool has_serial = options_.node_serial != 0 &&
                            (options_.node_serial & SPA_ID_INVALID) != SPA_ID_INVALID;
    if (!has_node && !has_serial) {
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

    if (remote_fd_ >= 0) {
      const auto core_fd = std::exchange(remote_fd_, -1);
      core_ = pw_context_connect_fd(context_, core_fd, nullptr, 0);
      if (!core_) {
        BOOST_LOG(warning) << "portal: Failed to connect PipeWire remote fd"sv;
        return false;
      }
    }
    else {
      // Default user session graph (direct node capture without portal remote).
      core_ = pw_context_connect(context_, nullptr, 0);
      if (!core_) {
        BOOST_LOG(warning) << "portal: Failed to connect default PipeWire core"sv;
        return false;
      }
    }

#ifndef PW_KEY_TARGET_OBJECT
  #define PW_KEY_TARGET_OBJECT "target.object"
#endif

    // Build format params once (used for serial-first and node-id connect).
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

    // gamescope HDR pods carry MANDATORY BT.2020 + SMPTE ST.2084.
    // Without matching props the intersection skips 10-bit and falls to BGRx
    // (gamescope lists 8-bit first among producer formats).
    auto push_format_pod = [&](std::uint32_t spa_format, std::optional<std::uint64_t> modifier) {
      params_storage.emplace_back(1024);
      spa_pod_builder pb = SPA_POD_BUILDER_INIT(
        params_storage.back().data(),
        static_cast<std::uint32_t>(params_storage.back().size())
      );
      spa_pod_frame frame {};
      spa_pod_builder_push_object(&pb, &frame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
      spa_pod_builder_add(&pb,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_Id(spa_format),
        SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&default_size, &min_size, &max_size),
        0);
      if (modifier) {
        spa_pod_builder_prop(&pb, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY);
        spa_pod_builder_long(&pb, static_cast<std::int64_t>(*modifier));
      }
      if (spa_format_is_hdr_rgb10(spa_format)) {
        spa_pod_builder_prop(&pb, SPA_FORMAT_VIDEO_colorPrimaries, SPA_POD_PROP_FLAG_MANDATORY);
        spa_pod_builder_id(&pb, SPA_VIDEO_COLOR_PRIMARIES_BT2020);
        spa_pod_builder_prop(&pb, SPA_FORMAT_VIDEO_transferFunction, SPA_POD_PROP_FLAG_MANDATORY);
        spa_pod_builder_id(&pb, SPA_VIDEO_TRANSFER_SMPTE2084);
      }
      params.push_back(reinterpret_cast<const spa_pod *>(spa_pod_builder_pop(&pb, &frame)));
    };

    if (options_.may_use_dmabuf) {
      for (const auto &format : options_.dmabuf_formats) {
        if (options_.prefer_hdr_formats && !spa_format_is_hdr_rgb10(format.spa_format)) {
          continue;
        }
        if (options_.prefer_sdr_formats && spa_format_is_hdr_rgb10(format.spa_format)) {
          continue;
        }
        push_format_pod(format.spa_format, format.modifier);
      }
    }

    // MemPtr/MemFd fallback pods. Prefer exclusive sets so gamescope cannot fixate
    // the wrong depth: HDR stream → 10-bit only; SDR stream → 8-bit only.
    if (options_.prefer_hdr_formats) {
      push_format_pod(SPA_VIDEO_FORMAT_xBGR_210LE, std::nullopt);
      push_format_pod(SPA_VIDEO_FORMAT_xRGB_210LE, std::nullopt);
      BOOST_LOG(info) << "portal: PipeWire EnumFormat prefer_hdr — only 10-bit PQ/BT.2020 pods (no BGRx)"sv;
    }
    else if (options_.prefer_sdr_formats) {
      push_format_pod(SPA_VIDEO_FORMAT_BGRx, std::nullopt);
      push_format_pod(SPA_VIDEO_FORMAT_BGRA, std::nullopt);
      push_format_pod(SPA_VIDEO_FORMAT_RGBx, std::nullopt);
      push_format_pod(SPA_VIDEO_FORMAT_RGBA, std::nullopt);
      BOOST_LOG(info) << "portal: PipeWire EnumFormat prefer_sdr — only 8-bit pods (no xBGR_210LE)"sv;
    }
    else {
      params_storage.emplace_back(1024);
      spa_pod_builder mem_pb = SPA_POD_BUILDER_INIT(
        params_storage.back().data(),
        static_cast<std::uint32_t>(params_storage.back().size())
      );
      const auto *memptr_fmt_param = reinterpret_cast<const spa_pod *>(spa_pod_builder_add_object(
        &mem_pb,
        SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
        SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
        SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
        SPA_FORMAT_VIDEO_format, SPA_POD_CHOICE_ENUM_Id(6,
          SPA_VIDEO_FORMAT_xBGR_210LE,
          SPA_VIDEO_FORMAT_xBGR_210LE,
          SPA_VIDEO_FORMAT_BGRx,
          SPA_VIDEO_FORMAT_BGRA,
          SPA_VIDEO_FORMAT_RGBx,
          SPA_VIDEO_FORMAT_RGBA),
        SPA_FORMAT_VIDEO_size, SPA_POD_CHOICE_RANGE_Rectangle(&default_size, &min_size, &max_size)));
      params.push_back(memptr_fmt_param);
    }

    static const pw_stream_events events = {
      .version = PW_VERSION_STREAM_EVENTS,
      .state_changed = capture_t::on_state_changed,
      .param_changed = capture_t::on_param_changed,
      .add_buffer = capture_t::on_add_buffer,
      .remove_buffer = capture_t::on_remove_buffer,
      .process = capture_t::on_process,
    };

    // Sunshine connect strategy: prefer object serial (target.object) then node id.
    // pw_stream_get_properties is const — set target on mutable props before pw_stream_new.
    auto make_props = [&](bool with_serial) -> pw_properties * {
      auto *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Video",
        PW_KEY_MEDIA_CATEGORY, "Capture",
        PW_KEY_MEDIA_ROLE, "Screen",
        nullptr
      );
      if (with_serial && has_serial) {
        pw_properties_setf(props, PW_KEY_TARGET_OBJECT, "%" PRIu64, options_.node_serial);
      }
      return props;
    };

    int connect_rc = -1;
    if (has_serial) {
      stream_ = pw_stream_new(core_, "polaris-portal-capture", make_props(true));
      if (!stream_) {
        BOOST_LOG(warning) << "portal: Failed to create PipeWire stream"sv;
        return false;
      }
      pw_stream_add_listener(stream_, &stream_listener_, &events, this);
      connect_rc = pw_stream_connect(
        stream_,
        PW_DIRECTION_INPUT,
        PW_ID_ANY,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
        params.data(),
        params.size()
      );
      if (connect_rc < 0) {
        BOOST_LOG(debug) << "portal: serial connect failed; falling back to node id"sv;
        pw_stream_destroy(stream_);
        stream_ = nullptr;
        spa_zero(stream_listener_);
      }
      else {
        BOOST_LOG(info) << "portal: PipeWire connect via object serial="sv << options_.node_serial;
      }
    }
    if (connect_rc < 0) {
      if (!has_node) {
        BOOST_LOG(warning) << "portal: Failed to connect PipeWire stream (no node id after serial fail)"sv;
        return false;
      }
      stream_ = pw_stream_new(core_, "polaris-portal-capture", make_props(false));
      if (!stream_) {
        BOOST_LOG(warning) << "portal: Failed to create PipeWire stream"sv;
        return false;
      }
      pw_stream_add_listener(stream_, &stream_listener_, &events, this);
      connect_rc = pw_stream_connect(
        stream_,
        PW_DIRECTION_INPUT,
        options_.node_id,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS),
        params.data(),
        params.size()
      );
      if (connect_rc < 0) {
        BOOST_LOG(warning) << "portal: Failed to connect PipeWire stream to node "sv << options_.node_id;
        return false;
      }
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

  void capture_t::stop() {
    std::lock_guard lk(frame_mtx_);
    running_ = false;
    terminal_result_ = wait_result_e::reinit;
    frame_cv_.notify_all();
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
      descriptor->dmabuf_buffer_key = front_dmabuf_buffer_key_;
      descriptor->frame_timestamp = front_dmabuf_timestamp_;
      descriptor->frame_metadata = dmabuf_frame_metadata(
        options_.capture_render_node.value_or(std::string {}),
        front_dmabuf_frame_->spa_format);

      auto retained_buffer = front_dmabuf_buffer_;
      auto pooled = std::move(image);
      auto *pooled_image = pooled.get();
      front_dmabuf_buffer_ = nullptr;
      front_dmabuf_buffer_key_ = 0;
      front_dmabuf_frame_.reset();
      frame_available_ = false;
      ++active_dmabuf_leases_;
      auto self = shared_from_this();
      image = std::shared_ptr<platf::img_t>(pooled_image, [pooled = std::move(pooled), self = std::move(self), retained_buffer](platf::img_t *) mutable {
        pooled.reset();
        self->release_buffer_lease(retained_buffer);
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
    image->frame_metadata = cpu_frame_metadata(front_info_.spa_format);
    frame_available_ = false;
    return true;
  }

  void capture_t::on_process(void *userdata) noexcept {
    auto *cap = static_cast<capture_t *>(userdata);
    if (!cap || !cap->running()) {
      return;
    }
    try {
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
    } catch (...) {
      // PipeWire C callbacks must not throw across the ABI.
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
    const auto accepted_data_types = offered_buffer_data_types(negotiated_dmabuf_);
    if (std::find(accepted_data_types.begin(), accepted_data_types.end(), data->type) == accepted_data_types.end()) {
      BOOST_LOG(warning) << "portal: unsupported PipeWire buffer data type="sv << data->type
                         << " data="sv << (data->data ? "mapped" : "null");
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

      auto frame_modifier = info.modifier;
      if (frame_modifier == DRM_FORMAT_MOD_INVALID) {
        frame_modifier = DRM_FORMAT_MOD_LINEAR;
      }
      dmabuf_frame_t frame {
        .width = info.width,
        .height = info.height,
        .spa_format = info.spa_format,
        .drm_fourcc = *drm_format,
        .modifier = frame_modifier,
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

      const auto [key_entry, inserted] = buffer_keys_.try_emplace(buffer, 0);
      if (inserted || key_entry->second == 0) {
        key_entry->second = allocate_buffer_key();
      }
      const auto buffer_key = key_entry->second;
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
        front_dmabuf_buffer_key_ = buffer_key;
        front_dmabuf_buffer_ = buffer;
        frame_available_ = true;
      }
      if (replaced_buffer) {
        // process_buffer() runs only from PipeWire's process callback, which
        // already holds the thread-loop lock. Queue directly here: taking
        // shutdown_mtx_ from the callback would invert shutdown's
        // shutdown_mtx_ -> thread-loop-lock order and deadlock teardown.
        pw_stream_queue_buffer(stream_, replaced_buffer);
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

  void capture_t::on_param_changed(void *userdata, std::uint32_t id, const spa_pod *param) noexcept {
    auto *cap = static_cast<capture_t *>(userdata);
    // SB-2: late format renegotiation after running_=false (compositor dying)
    // used to queue_buffer into a half-torn-down stream → polaris SEGV.
    if (!cap || !cap->running() || !cap->stream_) {
      return;
    }
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
    const char *dmabuf_env = std::getenv("POLARIS_PORTAL_DMABUF");
    const bool force_cpu = dmabuf_env && dmabuf_env[0] == '0' && dmabuf_env[1] == '\0';
    const bool want_dmabuf = cap->options_.may_use_dmabuf && !force_cpu;
    const auto has_modifier = (raw_info.flags & SPA_VIDEO_FLAG_MODIFIER) &&
                              raw_info.modifier != DRM_FORMAT_MOD_INVALID;
    const auto effective_modifier = has_modifier ? raw_info.modifier : DRM_FORMAT_MOD_LINEAR;
    const auto offered_dmabuf = want_dmabuf && std::any_of(
      cap->options_.dmabuf_formats.begin(), cap->options_.dmabuf_formats.end(),
      [&](const auto &format) {
        return format.spa_format == raw_info.format && format.modifier == effective_modifier;
      });
    if (has_modifier && (!want_dmabuf || !offered_dmabuf)) {
      BOOST_LOG(warning) << "portal: rejecting unoffered PipeWire DMA-BUF format/modifier pair"sv;
      cap->set_terminal(wait_result_e::reinit);
      return;
    }
    // Host KDE ScreenCast often advertises BGRx without SPA_VIDEO_FLAG_MODIFIER and
    // then delivers MemFd/SHM buffers. Claiming DMA-BUF in that case selects the
    // GPU encode factory while frames stay on the CPU → black video. Only negotiate
    // DMA-BUF when the producer explicitly set a modifier (gamescope portal does).
    const auto dmabuf_negotiated = want_dmabuf && has_modifier && offered_dmabuf;
    BOOST_LOG(info) << "portal: dmabuf_negotiate"
                    << " may_use="sv << (cap->options_.may_use_dmabuf ? "true"sv : "false"sv)
                    << " force_cpu="sv << (force_cpu ? "true"sv : "false"sv)
                    << " has_modifier="sv << (has_modifier ? "true"sv : "false"sv)
                    << " modifier="sv << effective_modifier
                    << " spa_format="sv << raw_info.format
                    << " formats="sv << cap->options_.dmabuf_formats.size()
                    << " offered="sv << (offered_dmabuf ? "true"sv : "false"sv)
                    << " negotiated="sv << (dmabuf_negotiated ? "true"sv : "false"sv)
                    << " flags="sv << raw_info.flags;
    if (dmabuf_negotiated) {
      BOOST_LOG(info) << "portal: PipeWire DMA-BUF path enabled (modifier="sv
                      << effective_modifier << ")"sv;
    }
    else if (want_dmabuf && !has_modifier) {
      BOOST_LOG(info) << "portal: PipeWire CPU/MemFd path (producer omitted modifier; host KDE typical)"sv;
    }

    pw_buffer *replaced_buffer = nullptr;
    bool requires_reinit = false;
    const auto negotiated_modifier = dmabuf_negotiated ? effective_modifier :
      (raw_info.flags & SPA_VIDEO_FLAG_MODIFIER ? raw_info.modifier : DRM_FORMAT_MOD_INVALID);
    {
      std::lock_guard lk(cap->frame_mtx_);
      const auto width = static_cast<int>(raw_info.size.width);
      const auto height = static_cast<int>(raw_info.size.height);
      requires_reinit = cap->negotiated_ &&
                        (cap->negotiated_dmabuf_ != dmabuf_negotiated ||
                         cap->front_info_.width != width ||
                         cap->front_info_.height != height ||
                         cap->front_info_.spa_format != raw_info.format ||
                         cap->front_info_.modifier != negotiated_modifier);
      replaced_buffer = cap->front_dmabuf_buffer_;
      cap->front_info_ = {
        .width = width,
        .height = height,
        .stride = width * 4,
        .spa_format = raw_info.format,
        .modifier = negotiated_modifier,
      };
      cap->front_frame_.clear();
      cap->back_frame_.clear();
      cap->front_dmabuf_frame_.reset();
      cap->front_dmabuf_buffer_ = nullptr;
      cap->front_dmabuf_buffer_key_ = 0;
      cap->frame_available_ = false;
      cap->negotiated_ = true;
      cap->negotiated_dmabuf_ = dmabuf_negotiated;
    }
    if (replaced_buffer) {
      // Format callbacks run with PipeWire's thread-loop lock held. Queue
      // directly so shutdown_mtx_ is never acquired in loop callback context.
      pw_stream_queue_buffer(cap->stream_, replaced_buffer);
    }
    cap->frame_cv_.notify_all();

    BOOST_LOG(info) << "portal: PipeWire format negotiated: "sv
                    << raw_info.size.width << "x"sv << raw_info.size.height
                    << " format="sv << spa_debug_type_find_name(spa_type_video_format, raw_info.format);

    if (requires_reinit) {
      cap->buffer_keys_.clear();
      BOOST_LOG(info) << "portal: PipeWire transport/format/layout changed; reinitializing encoder"sv;
      cap->set_terminal(wait_result_e::reinit);
      return;
    }

    uint8_t params_buffer[1024];
    spa_pod_builder pb = SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
    std::uint32_t data_type_mask = 0;
    for (const auto data_type : offered_buffer_data_types(dmabuf_negotiated)) {
      data_type_mask |= 1u << data_type;
    }
    const auto *buf_param = reinterpret_cast<const spa_pod *>(spa_pod_builder_add_object(
      &pb,
      SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
      SPA_PARAM_BUFFERS_buffers, SPA_POD_CHOICE_RANGE_Int(4, 2, 8),
      SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(static_cast<int>(data_type_mask))));

    pw_stream_update_params(cap->stream_, &buf_param, 1);
  }

  void capture_t::on_add_buffer(void *userdata, pw_buffer *buffer) noexcept {
    auto *cap = static_cast<capture_t *>(userdata);
    if (!cap || !buffer) {
      return;
    }
    cap->buffer_keys_[buffer] = allocate_buffer_key();
  }

  void capture_t::on_remove_buffer(void *userdata, pw_buffer *buffer) noexcept {
    auto *cap = static_cast<capture_t *>(userdata);
    if (!cap || !buffer) {
      return;
    }
    cap->buffer_keys_.erase(buffer);
    std::lock_guard lk(cap->frame_mtx_);
    if (cap->front_dmabuf_buffer_ == buffer) {
      cap->front_dmabuf_buffer_ = nullptr;
      cap->front_dmabuf_buffer_key_ = 0;
      cap->front_dmabuf_frame_.reset();
      cap->frame_available_ = false;
    }
  }

  void capture_t::on_state_changed(void *userdata, pw_stream_state old, pw_stream_state state, const char *errmsg) noexcept {
    auto *cap = static_cast<capture_t *>(userdata);
    if (!cap || !cap->running()) {
      return;
    }
    try {
      BOOST_LOG(info) << "portal: PipeWire state: "sv
                      << pw_stream_state_as_string(old) << " -> "sv
                      << pw_stream_state_as_string(state);
      if (state == PW_STREAM_STATE_ERROR && errmsg) {
        BOOST_LOG(warning) << "portal: PipeWire error: "sv << errmsg;
      }
    } catch (...) {
      // PipeWire C callbacks must not throw logging failures across the ABI.
    }

    if (state == PW_STREAM_STATE_ERROR || state == PW_STREAM_STATE_UNCONNECTED) {
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
    std::lock_guard shutdown_lk(shutdown_mtx_);
    if (shutdown_complete_ || !buffer || !stream_) {
      return;
    }
    // Drop buffers once teardown cleared running_; never touch PW after stop.
    if (!running()) {
      return;
    }
    if (loop_ && !pw_thread_loop_in_thread(loop_)) {
      pw_thread_loop_lock(loop_);
      if (stream_ && running()) {
        pw_stream_queue_buffer(stream_, buffer);
      }
      pw_thread_loop_unlock(loop_);
      return;
    }
    pw_stream_queue_buffer(stream_, buffer);
  }

  void capture_t::release_buffer_lease(pw_buffer *buffer) {
    bool should_queue = false;
    {
      std::lock_guard lk(frame_mtx_);
      if (active_dmabuf_leases_ > 0) {
        --active_dmabuf_leases_;
      }
      should_queue = running_;
    }
    frame_cv_.notify_all();
    if (should_queue) {
      queue_buffer(buffer);
    }
  }

  namespace {
    struct gamescope_find_t {
      pw_main_loop *loop = nullptr;
      std::optional<video_source_t> found;
    };

    void on_registry_global(void *data, uint32_t id, uint32_t /*permissions*/, const char *type, uint32_t /*version*/, const struct spa_dict *props) {
      auto *ctx = static_cast<gamescope_find_t *>(data);
      if (!ctx || !type || !props || std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0) {
        return;
      }
      if (ctx->found) {
        return;
      }
      const char *media_class = spa_dict_lookup(props, "media.class");
      const char *media_name = spa_dict_lookup(props, "media.name");
      const char *node_name = spa_dict_lookup(props, "node.name");
      const char *app_name = spa_dict_lookup(props, "application.name");
      const bool is_video_source = media_class && std::strcmp(media_class, "Video/Source") == 0;
      auto has_gamescope = [](const char *s) {
        return s && std::strstr(s, "gamescope") != nullptr;
      };
      // Prefer Video/Source named gamescope; allow any Video/Source with gamescope in name.
      if (!is_video_source || (!has_gamescope(media_name) && !has_gamescope(node_name) && !has_gamescope(app_name))) {
        return;
      }
      video_source_t src;
      src.node_id = id;
      if (const char *serial = spa_dict_lookup(props, "object.serial")) {
        src.object_serial = std::strtoull(serial, nullptr, 10);
      }
      src.node_name = node_name ? node_name : (media_name ? media_name : "gamescope");
      ctx->found = src;
      if (ctx->loop) {
        pw_main_loop_quit(ctx->loop);
      }
    }

    const pw_registry_events registry_events = {
      .version = PW_VERSION_REGISTRY_EVENTS,
      .global = on_registry_global,
    };
  }  // namespace

  std::optional<video_source_t> find_gamescope_video_source() {
    // Operator override for smoke without registry walk.
    if (const char *env = std::getenv("POLARIS_GAMESCOPE_PW_NODE"); env && *env) {
      char *end = nullptr;
      const auto id = std::strtoul(env, &end, 10);
      if (end != env && id > 0 && id < 0xffffffffu) {
        video_source_t src;
        src.node_id = static_cast<std::uint32_t>(id);
        src.node_name = "env:POLARIS_GAMESCOPE_PW_NODE";
        return src;
      }
    }

    init_pipewire_once();
    gamescope_find_t ctx;
    ctx.loop = pw_main_loop_new(nullptr);
    if (!ctx.loop) {
      return std::nullopt;
    }
    auto *context = pw_context_new(pw_main_loop_get_loop(ctx.loop), nullptr, 0);
    if (!context) {
      pw_main_loop_destroy(ctx.loop);
      return std::nullopt;
    }
    auto *core = pw_context_connect(context, nullptr, 0);
    if (!core) {
      pw_context_destroy(context);
      pw_main_loop_destroy(ctx.loop);
      return std::nullopt;
    }
    auto *registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
    spa_hook registry_listener {};
    pw_registry_add_listener(registry, &registry_listener, &registry_events, &ctx);

    auto *loop = pw_main_loop_get_loop(ctx.loop);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(400);
    while (!ctx.found && std::chrono::steady_clock::now() < deadline) {
      pw_loop_iterate(loop, 50);
    }

    spa_hook_remove(&registry_listener);
    pw_proxy_destroy(reinterpret_cast<pw_proxy *>(registry));
    pw_core_disconnect(core);
    pw_context_destroy(context);
    pw_main_loop_destroy(ctx.loop);

    if (ctx.found) {
      BOOST_LOG(info) << "pipewire: found gamescope Video/Source node="sv << ctx.found->node_id
                      << " serial="sv << ctx.found->object_serial
                      << " name="sv << ctx.found->node_name;
    }
    return ctx.found;
  }
}  // namespace pipewire_capture
