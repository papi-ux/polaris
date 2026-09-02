/**
 * @file src/platform/linux/wlgrab.cpp
 * @brief Definitions for wlgrab capture.
 */
// standard includes
#include <cstring>
#include <string>
#include <thread>
#include <vector>

// local includes
#include "stream_runtime.h"
#include "cuda.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/stream_stats.h"
#include "src/video.h"
#include "vaapi.h"
#ifdef POLARIS_BUILD_VULKAN
  #include "vulkan_encode.h"
#endif
#include "wayland.h"
#include "wlgrab_capture_policy.h"
#include "wlgrab_frame_source.h"
#include "wlgrab_pacing.h"
#include "wlgrab_pixel_copy.h"

using namespace std::literals;

namespace wl {
  static int env_width;
  static int env_height;

  bool supports_gpu_native_capture(platf::mem_type_e hwdevice_type) {
    switch (hwdevice_type) {
#ifdef POLARIS_BUILD_VAAPI
      case platf::mem_type_e::vaapi:
        return true;
#endif
#ifdef POLARIS_BUILD_CUDA
      case platf::mem_type_e::cuda:
        return true;
#endif
#ifdef POLARIS_BUILD_VULKAN
      case platf::mem_type_e::vulkan:
        return true;
#endif
      default:
        return false;
    }
  }

  struct img_t: public platf::img_t {
    ~img_t() override {
      delete[] data;
      data = nullptr;
    }
  };

  class wlr_t: public platf::display_t {
  public:
    bool capture_profile_enabled() const {
      return config::video.linux_display.capture_profile;
    }

    void log_capture_metadata(const platf::frame_metadata_t &metadata) {
      if (capture_transport_logged) {
        return;
      }

      capture_transport_logged = true;
      if (metadata.transport == platf::frame_transport_e::shm) {
        BOOST_LOG(warning) << "wlr: capture_transport="sv << platf::from_frame_transport(metadata.transport)
                           << " frame_residency="sv << platf::from_frame_residency(metadata.residency)
                           << " frame_format="sv << platf::from_frame_format(metadata.format)
                           << "; capture will incur an extra CPU-side copy/conversion path"sv;
      } else {
        BOOST_LOG(info) << "wlr: capture_transport="sv << platf::from_frame_transport(metadata.transport)
                        << " frame_residency="sv << platf::from_frame_residency(metadata.residency)
                        << " frame_format="sv << platf::from_frame_format(metadata.format);
      }
    }

    int init(platf::mem_type_e hwdevice_type, const std::string &display_name, const ::video::config_t &config) {
      delay = std::chrono::nanoseconds {1s} / config.framerate;
      mem_type = hwdevice_type;
      const auto generation_policy = wlgrab_capture_policy::resolve_generation_policy(
        config.capture_generation,
        config::video.linux_display.use_cage_compositor,
        config::video.adapter_name
      );
      const bool generation_owned = generation_policy.owned;
      const bool use_private_compositor = generation_policy.use_private_compositor;

      // If cage compositor is running, connect to cage's Wayland socket
      // for direct wlr-screencopy capture (no portal, no picker).
      const char *wayland_target = nullptr;
      std::string wayland_socket;
#ifdef __linux__
      if (use_private_compositor) {
        if (generation_owned) {
          const auto live_socket = stream_runtime::labwc::wayland_socket();
          const auto live_instance = stream_runtime::labwc::session_instance_id();
          if (!stream_runtime::labwc::is_running() ||
              !wlgrab_capture_policy::private_runtime_matches_generation(
                generation_policy,
                live_socket,
                live_instance
              )) {
            BOOST_LOG(error) << "wlr: Immutable private capture generation does not match the live labwc instance; refusing host or replacement-compositor fallback"sv;
            return -1;
          }
          wayland_socket = generation_policy.private_wayland_socket;
        } else if (stream_runtime::labwc::is_running()) {
          wayland_socket = stream_runtime::labwc::wayland_socket();
        }
        if (!wayland_socket.empty()) {
          wayland_target = wayland_socket.c_str();
          private_compositor_capture = true;
          BOOST_LOG(info) << "wlr: Targeting cage compositor on "sv << wayland_socket;
        }
      }
      if (generation_owned && use_private_compositor && wayland_target == nullptr) {
        BOOST_LOG(error) << "wlr: Immutable private capture generation has no exact labwc target; refusing host compositor fallback"sv;
        return -1;
      }
#endif

      stream_stats::update_wayland_main_device({});
      if (display.init(wayland_target)) {
        return -1;
      }

      interface.listen(display.registry());

      display.roundtrip();

      if (!interface[wl::interface_t::XDG_OUTPUT]) {
        if (!use_private_compositor) {
          BOOST_LOG(error) << "Missing Wayland wire for xdg_output"sv;
          return -1;
        }
        BOOST_LOG(info) << "wlr: xdg_output not available (cage mode — using direct output)"sv;
      }

      if (!interface[wl::interface_t::WLR_EXPORT_DMABUF]) {
        // wlr-export-dmabuf is optional when using cage — screencopy handles frame capture.
        // Only fail if we're NOT targeting cage (i.e., capturing the desktop directly).
        if (!use_private_compositor) {
          BOOST_LOG(error) << "Missing Wayland wire for wlr-export-dmabuf"sv;
          return -1;
        }
        BOOST_LOG(info) << "wlr: wlr-export-dmabuf not available (cage mode — using screencopy only)"sv;
      }

      if (interface.monitors.empty()) {
        BOOST_LOG(error) << "Wayland compositor advertised no capturable outputs"sv;
        return -1;
      }

      for (const auto &candidate : interface.monitors) {
        candidate->listen(interface.output_manager);
      }
      display.roundtrip();

      std::vector<std::string> monitor_names;
      monitor_names.reserve(interface.monitors.size());
      for (const auto &candidate : interface.monitors) {
        monitor_names.emplace_back(candidate->name);
      }

      const auto monitor_index = wlgrab_capture_policy::select_monitor_index(
        display_name,
        monitor_names
      );
      if (!monitor_index) {
        BOOST_LOG(error) << "Requested Wayland output ["sv << display_name
                         << "] was not found; refusing to capture another output"sv;
        return -1;
      }

      auto monitor = interface.monitors[*monitor_index].get();
      reported_wayland_main_device = interface.dmabuf_feedback.main_device_path;
      stream_stats::update_wayland_main_device(reported_wayland_main_device);
      interface.consume_output_topology_dirty();
      output = monitor->output;
      offset_x = monitor->viewport.offset_x;
      offset_y = monitor->viewport.offset_y;
      width = monitor->viewport.width;
      height = monitor->viewport.height;

      this->env_width = ::wl::env_width;
      this->env_height = ::wl::env_height;

      BOOST_LOG(info) << "Selected monitor ["sv << monitor->description << "] for streaming"sv;
      BOOST_LOG(debug) << "Offset: "sv << offset_x << 'x' << offset_y;
      BOOST_LOG(debug) << "Resolution: "sv << width << 'x' << height;
      BOOST_LOG(debug) << "Desktop Resolution: "sv << env_width << 'x' << env_height;

      return 0;
    }

    int dummy_img(platf::img_t *img) override {
      return 0;
    }

    inline platf::capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor) {
      auto dispatch_start = std::chrono::steady_clock::now();
      auto to = std::chrono::steady_clock::now() + timeout;

      // Dispatch events until we get a new frame or the timeout expires
      dmabuf.prefer_shm = prefer_shm_screencopy;
      dmabuf.set_feedback(&interface.dmabuf_feedback);
      dmabuf.listen(interface.screencopy_manager, interface.dmabuf_interface, output, cursor, interface.shm);
      do {
        auto remaining_time_ms = std::chrono::duration_cast<std::chrono::milliseconds>(to - std::chrono::steady_clock::now());
        if (remaining_time_ms.count() < 0 || !display.dispatch(remaining_time_ms)) {
          dmabuf.cancel();
          return platf::capture_e::timeout;
        }
        if (interface.consume_output_topology_dirty()) {
          BOOST_LOG(warning) << "wlr: Wayland output topology changed during capture; reinitializing display capture"sv;
          dmabuf.cancel();
          return platf::capture_e::reinit;
        }
      } while (dmabuf.status == dmabuf_t::WAITING);

      if (reported_wayland_main_device != interface.dmabuf_feedback.main_device_path) {
        reported_wayland_main_device = interface.dmabuf_feedback.main_device_path;
        stream_stats::update_wayland_main_device(reported_wayland_main_device);
      }

      auto current_frame = dmabuf.current_frame;
      const bool shm_frame_ready = dmabuf.shm_frame_ready && dmabuf.shm_data;
      const auto captured_width = shm_frame_ready ? static_cast<int>(dmabuf.shm_info.width) : static_cast<int>(current_frame->sd.width);
      const auto captured_height = shm_frame_ready ? static_cast<int>(dmabuf.shm_info.height) : static_cast<int>(current_frame->sd.height);

      if (
        dmabuf.status == dmabuf_t::REINIT ||
        captured_width != width ||
        captured_height != height
      ) {
        return platf::capture_e::reinit;
      }
      last_dispatch_time = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - dispatch_start
      );
      return platf::capture_e::ok;
    }

    /**
     * @brief Shared capture pacing loop for RAM / VRAM / extcopy backends.
     * @param pacing_policy Chooses timer pacing or compositor-driven pacing.
     * @param snapshot_fn Called each tick: (pull_cb, img_out, cursor) → capture_e
     */
    template <class SnapshotFn>
    platf::capture_e capture_loop(
      const push_captured_image_cb_t &push_captured_image_cb,
      const pull_free_image_cb_t &pull_free_image_cb,
      bool *cursor,
      capture_pacing_policy_e pacing_policy,
      SnapshotFn &&snapshot_fn
    ) {
      capture_pacer_t pacer {pacing_policy, delay, std::chrono::steady_clock::now()};
      capture_source_rate_tracker_t source_rate;
      stream_stats::update_capture_pacing(capture_pacing_policy_name(pacing_policy));
      stream_stats::update_capture_source_fps(0.0);
      sleep_overshoot_logger.reset();

      while (true) {
        auto now = std::chrono::steady_clock::now();
        auto wait_duration = pacer.wait_duration(now);

        if (wait_duration > std::chrono::steady_clock::duration::zero()) {
          auto wake_time = now + wait_duration;
          std::this_thread::sleep_for(wait_duration);
          sleep_overshoot_logger.first_point(wake_time);
          sleep_overshoot_logger.second_point_now_and_log();
        }

        pacer.advance(now);

        std::shared_ptr<platf::img_t> img_out;
        auto status = snapshot_fn(pull_free_image_cb, img_out, cursor);
        switch (status) {
          case platf::capture_e::reinit:
          case platf::capture_e::error:
          case platf::capture_e::interrupted:
            return status;
          case platf::capture_e::timeout:
            if (!push_captured_image_cb(std::move(img_out), false)) {
              return platf::capture_e::ok;
            }
            break;
          case platf::capture_e::ok:
            if (const auto source_fps = source_rate.note_frame(std::chrono::steady_clock::now())) {
              stream_stats::update_capture_source_fps(*source_fps);
            }
            if (!push_captured_image_cb(std::move(img_out), true)) {
              return platf::capture_e::ok;
            }
            break;
          default:
            BOOST_LOG(error) << "Unrecognized capture status ["sv << (int) status << ']';
            return status;
        }
      }

      return platf::capture_e::ok;
    }

    platf::mem_type_e mem_type;
    bool capture_transport_logged = false;
    bool prefer_shm_screencopy = false;
    bool private_compositor_capture = false;
    std::string reported_wayland_main_device;
    std::chrono::microseconds last_dispatch_time {};

    std::chrono::nanoseconds delay;

    wl::display_t display;
    interface_t interface;
    dmabuf_t dmabuf;

    wl_output *output;
  };

  class wlr_ram_t: public wlr_t {
  public:
    platf::capture_e capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) override {
      const auto pacing = screencopy_pacing_policy(private_compositor_capture);
      BOOST_LOG(info) << "wlr: screencopy capture pacing="sv << capture_pacing_policy_name(pacing)
                      << " private_compositor="sv << private_compositor_capture;
      return capture_loop(push_captured_image_cb, pull_free_image_cb, cursor,
        pacing,
        [this](const pull_free_image_cb_t &pull, std::shared_ptr<platf::img_t> &img, bool *c) {
          return snapshot(pull, img, 1000ms, c && *c);
        });
    }

    platf::capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor) {
      auto capture_start = std::chrono::steady_clock::now();
      auto status = wlr_t::snapshot(pull_free_image_cb, img_out, timeout, cursor);
      if (status != platf::capture_e::ok) {
        return status;
      }
      auto frame_timestamp = std::chrono::steady_clock::now();

      // SHM frame path — copy raw pixels directly
      if (dmabuf.shm_frame_ready && dmabuf.shm_data) {
        if (!pull_free_image_cb(img_out)) {
          return platf::capture_e::interrupted;
        }

        auto *src = static_cast<uint8_t *>(dmabuf.shm_data);
        auto *dst = img_out->data;
        int copy_h = std::min((int) dmabuf.shm_info.height, img_out->height);
        int copy_w = std::min((int) dmabuf.shm_info.width, img_out->width);
        int src_stride = dmabuf.shm_info.stride;
        int dst_stride = img_out->row_pitch;
        const bool is_3bpp = (src_stride == dmabuf.shm_info.width * 3);

        if (is_3bpp) {
          static bool logged_bgr888_conversion = false;
          if (!logged_bgr888_conversion) {
            BOOST_LOG(info)
              << "wlr: SHM screencopy is 3bpp BGR888; expanding to 4bpp BGRA for software encoding"sv;
            logged_bgr888_conversion = true;
          }

          // Match the portal screencopy path: headless wlroots SHM frames
          // report BGR888 but the byte order is effectively RGB.
          copy_shm_3bpp_rgb_to_bgra(src, src_stride, dst, dst_stride, copy_w, copy_h);
        } else {
          copy_shm_4bpp_to_bgra(
            src,
            src_stride,
            dst,
            dst_stride,
            copy_w,
            copy_h,
            dmabuf.shm_info.format
          );
        }

        img_out->frame_timestamp = frame_timestamp;
        img_out->frame_metadata = {
          .transport = platf::frame_transport_e::shm,
          .residency = platf::frame_residency_e::cpu,
          .format = platf::frame_format_e::bgra8,
        };
        stream_stats::update_capture_metadata(img_out->frame_metadata);
        log_capture_metadata(img_out->frame_metadata);
        if (capture_profile_enabled()) {
          const auto &timing = dmabuf.last_timing_sample();
          const auto handoff_end = std::chrono::steady_clock::now();
          auto total_time = std::chrono::duration_cast<std::chrono::microseconds>(
            handoff_end - capture_start
          );
          auto ingest_time = total_time - last_dispatch_time;
          stream_stats::update_capture_profile({
            .transport = img_out->frame_metadata.transport,
            .dispatch_time = last_dispatch_time,
            .ingest_time = ingest_time,
            .total_time = total_time,
            .source_interval = timing.presentation_interval,
            .ready_to_handoff = timing.ready_at ?
              std::optional {std::chrono::duration_cast<std::chrono::microseconds>(handoff_end - *timing.ready_at)} :
              std::nullopt,
          });
        }
        dmabuf.shm_frame_ready = false;
        return platf::capture_e::ok;
      }

      // DMA-BUF frame path — import via EGL
      auto current_frame = dmabuf.current_frame;

      auto rgb_opt = egl::import_source(egl_display.get(), current_frame->sd);

      if (!rgb_opt) {
        return platf::capture_e::reinit;
      }

      if (!pull_free_image_cb(img_out)) {
        return platf::capture_e::interrupted;
      }

      gl::ctx.BindTexture(GL_TEXTURE_2D, (*rgb_opt)->tex[0]);

      // Don't remove these lines, see https://github.com/LizardByte/Sunshine/issues/453
      int w, h;
      gl::ctx.GetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
      gl::ctx.GetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
      BOOST_LOG(debug) << "width and height: w "sv << w << " h "sv << h;

      gl::ctx.GetTextureSubImage((*rgb_opt)->tex[0], 0, 0, 0, 0, width, height, 1, GL_BGRA, GL_UNSIGNED_BYTE, img_out->height * img_out->row_pitch, img_out->data);
      gl::ctx.BindTexture(GL_TEXTURE_2D, 0);
      img_out->frame_timestamp = frame_timestamp;
      img_out->frame_metadata = {
        .transport = platf::frame_transport_e::dmabuf,
        .residency = platf::frame_residency_e::cpu,
        .format = platf::frame_format_e::bgra8,
      };
      stream_stats::update_capture_metadata(img_out->frame_metadata);
      log_capture_metadata(img_out->frame_metadata);
      if (capture_profile_enabled()) {
        const auto &timing = dmabuf.last_timing_sample();
        const auto handoff_end = std::chrono::steady_clock::now();
        auto total_time = std::chrono::duration_cast<std::chrono::microseconds>(
          handoff_end - capture_start
        );
        auto ingest_time = total_time - last_dispatch_time;
        stream_stats::update_capture_profile({
          .transport = img_out->frame_metadata.transport,
          .dispatch_time = last_dispatch_time,
          .ingest_time = ingest_time,
          .total_time = total_time,
          .source_interval = timing.presentation_interval,
          .ready_to_handoff = timing.ready_at ?
            std::optional {std::chrono::duration_cast<std::chrono::microseconds>(handoff_end - *timing.ready_at)} :
            std::nullopt,
        });
      }

      return platf::capture_e::ok;
    }

    int init(platf::mem_type_e hwdevice_type, const std::string &display_name, const ::video::config_t &config) {
      if (wlr_t::init(hwdevice_type, display_name, config)) {
        return -1;
      }

      egl_display = egl::make_display(display.get());
      if (!egl_display) {
        return -1;
      }

      auto ctx_opt = egl::make_ctx(egl_display.get());
      if (!ctx_opt) {
        return -1;
      }

      ctx = std::move(*ctx_opt);

      return 0;
    }

    std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device(platf::pix_fmt_e pix_fmt) override {
#ifdef POLARIS_BUILD_VAAPI
      if (mem_type == platf::mem_type_e::vaapi) {
        return va::make_avcodec_encode_device(width, height, false);
      }
#endif

#ifdef POLARIS_BUILD_CUDA
      if (mem_type == platf::mem_type_e::cuda) {
        if (pix_fmt == platf::pix_fmt_e::nv12) {
          return cuda::make_avcodec_encode_device(width, height, false);
        }

        return std::make_unique<platf::avcodec_encode_device_t>();
      }
#endif

#ifdef POLARIS_BUILD_VULKAN
      if (mem_type == platf::mem_type_e::vulkan) {
        return vk::make_avcodec_encode_device_ram(width, height, reported_wayland_main_device);
      }
#endif

      return std::make_unique<platf::avcodec_encode_device_t>();
    }

    std::shared_ptr<platf::img_t> alloc_img() override {
      auto img = std::make_shared<img_t>();
      img->width = width;
      img->height = height;
      img->pixel_pitch = 4;
      img->row_pitch = img->pixel_pitch * width;
      img->data = new std::uint8_t[height * img->row_pitch];

      return img;
    }

    int dummy_img(platf::img_t *img) override {
      if (!img || !img->data || img->height <= 0 || img->row_pitch <= 0) {
        return -1;
      }
      std::memset(
        img->data,
        0,
        static_cast<std::size_t>(img->height) * static_cast<std::size_t>(img->row_pitch)
      );
      return 0;
    }

    egl::display_t egl_display;
    egl::ctx_t ctx;
  };

  class wlr_vram_t: public wlr_t {
  public:
    platf::capture_e capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) override {
      const auto pacing = screencopy_pacing_policy(private_compositor_capture);
      BOOST_LOG(info) << "wlr: screencopy capture pacing="sv << capture_pacing_policy_name(pacing)
                      << " private_compositor="sv << private_compositor_capture;
      return capture_loop(push_captured_image_cb, pull_free_image_cb, cursor,
        pacing,
        [this](const pull_free_image_cb_t &pull, std::shared_ptr<platf::img_t> &img, bool *c) {
          return snapshot(pull, img, 1000ms, c && *c);
        });
    }

    platf::capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, std::chrono::milliseconds timeout, bool cursor) {
      auto capture_start = std::chrono::steady_clock::now();
      auto status = wlr_t::snapshot(pull_free_image_cb, img_out, timeout, cursor);
      if (status != platf::capture_e::ok) {
        return status;
      }
      auto frame_timestamp = std::chrono::steady_clock::now();

      if (!pull_free_image_cb(img_out)) {
        return platf::capture_e::interrupted;
      }
      auto img = (egl::img_descriptor_t *) img_out.get();
      img->reset();

      auto current_frame = dmabuf.current_frame;

      ++sequence;
      img->sequence = sequence;

      img->sd = current_frame->sd;
      img->dmabuf_buffer_key = current_frame->buffer_key;
      img->frame_timestamp = frame_timestamp;
      img->frame_metadata = {
        .transport = platf::frame_transport_e::dmabuf,
        .residency = platf::frame_residency_e::gpu,
        .format = platf::frame_format_e::bgra8,
      };
      stream_stats::update_capture_metadata(img->frame_metadata);
      log_capture_metadata(img->frame_metadata);
      if (capture_profile_enabled()) {
        const auto &timing = dmabuf.last_timing_sample();
        const auto handoff_end = std::chrono::steady_clock::now();
        auto total_time = std::chrono::duration_cast<std::chrono::microseconds>(
          handoff_end - capture_start
        );
        auto ingest_time = total_time - last_dispatch_time;
        stream_stats::update_capture_profile({
          .transport = img->frame_metadata.transport,
          .dispatch_time = last_dispatch_time,
          .ingest_time = ingest_time,
          .total_time = total_time,
          .source_interval = timing.presentation_interval,
          .ready_to_handoff = timing.ready_at ?
            std::optional {std::chrono::duration_cast<std::chrono::microseconds>(handoff_end - *timing.ready_at)} :
            std::nullopt,
        });
      }

      // Prevent dmabuf from closing the file descriptors.
      std::fill_n(current_frame->sd.fds, 4, -1);

      return platf::capture_e::ok;
    }

    std::shared_ptr<platf::img_t> alloc_img() override {
      auto img = std::make_shared<egl::img_descriptor_t>();

      img->width = width;
      img->height = height;
      img->sequence = 0;
      img->serial = std::numeric_limits<decltype(img->serial)>::max();
      img->dmabuf_buffer_key = 0;
      img->data = nullptr;

      // File descriptors aren't open
      std::fill_n(img->sd.fds, 4, -1);

      return img;
    }

    std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device(platf::pix_fmt_e pix_fmt) override {
#ifdef POLARIS_BUILD_VAAPI
      if (mem_type == platf::mem_type_e::vaapi) {
        return va::make_avcodec_encode_device(width, height, 0, 0, true);
      }
#endif

#ifdef POLARIS_BUILD_CUDA
      if (mem_type == platf::mem_type_e::cuda) {
        return cuda::make_avcodec_gl_encode_device(width, height, 0, 0);
      }
#endif

#ifdef POLARIS_BUILD_VULKAN
      if (mem_type == platf::mem_type_e::vulkan) {
        return vk::make_avcodec_encode_device_vram(
          width,
          height,
          0,
          0,
          reported_wayland_main_device
        );
      }
#endif

      return std::make_unique<platf::avcodec_encode_device_t>();
    }

    int dummy_img(platf::img_t *img) override {
      // Empty images are recognized as dummies by the zero sequence number
      return 0;
    }

    std::uint64_t sequence {};
  };

  class wlr_extcopy_vram_t: public wlr_t {
  public:
    platf::capture_e capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) override {
      BOOST_LOG(info) << "wlr: ext-image-copy capture pacing=source_driven"sv;
      return capture_loop(push_captured_image_cb, pull_free_image_cb, cursor,
        capture_pacing_policy_e::source_driven,
        [this](const pull_free_image_cb_t &pull, std::shared_ptr<platf::img_t> &img, bool *c) {
          return snapshot(pull, img, c);
        });
    }

    platf::capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, bool *cursor) {
      auto capture_start = std::chrono::steady_clock::now();
      auto cursor_enabled = cursor ? *cursor : false;
      const auto frame_source = select_extcopy_frame_source(
        capture_ready,
        cursor_enabled != blend_cursor,
        prefetched_frame_pending
      );

      if (frame_source == extcopy_frame_source_e::initialize) {
        blend_cursor = cursor_enabled;
        if (extcopy.init(display, interface.copy_capture_manager, interface.output_capture_source_manager, interface.dmabuf_interface, output, blend_cursor)) {
          return platf::capture_e::reinit;
        }
        capture_ready = true;
      } else {
        if (frame_source == extcopy_frame_source_e::prefetched) {
          // extcopy.init() captured this frame while validating the negotiated
          // DMA-BUF. Hand it to the caller before requesting more damage.
          BOOST_LOG(debug) << "wlr: Consuming ext-image-copy frame captured during initialization"sv;
        } else {
          extcopy.capture(display);
        }

        if (interface.consume_output_topology_dirty()) {
          BOOST_LOG(warning) << "wlr: Wayland output topology changed during ext-image-copy capture; reinitializing display capture"sv;
          return platf::capture_e::reinit;
        }
        if (extcopy.status == wl::extcopy_t::WAITING) {
          return platf::capture_e::timeout;
        }
        if (extcopy.status != wl::extcopy_t::READY) {
          return platf::capture_e::reinit;
        }
      }

      if (!pull_free_image_cb(img_out)) {
        return platf::capture_e::interrupted;
      }

      auto *img = static_cast<egl::img_descriptor_t *>(img_out.get());
      img->reset();

      auto current_frame = extcopy.current_frame;

      ++sequence;
      img->sequence = sequence;
      img->sd = current_frame->sd;
      img->dmabuf_buffer_key = current_frame->buffer_key;
      img->frame_timestamp = std::chrono::steady_clock::now();
      img->frame_metadata = {
        .transport = platf::frame_transport_e::dmabuf,
        .residency = platf::frame_residency_e::gpu,
        .format = platf::frame_format_e::bgra8,
        .device = extcopy.capture_render_node(),
      };
      stream_stats::update_capture_metadata(img->frame_metadata);
      log_capture_metadata(img->frame_metadata);
      if (capture_profile_enabled()) {
        const auto handoff_end = std::chrono::steady_clock::now();
        const auto &timing = extcopy.last_timing_sample();
        const auto snapshot_time = std::chrono::duration_cast<std::chrono::microseconds>(
          handoff_end - capture_start
        );
        const auto dispatch_time = timing.request_to_ready.value_or(snapshot_time);
        const auto ready_to_handoff = timing.ready_at ?
          std::optional {std::chrono::duration_cast<std::chrono::microseconds>(handoff_end - *timing.ready_at)} :
          std::nullopt;
        const auto handoff_time = ready_to_handoff.value_or(0us);
        stream_stats::update_capture_profile({
          .transport = img->frame_metadata.transport,
          .dispatch_time = dispatch_time,
          .ingest_time = handoff_time,
          .total_time = dispatch_time + handoff_time,
          .source_interval = timing.presentation_interval,
          .ready_to_handoff = ready_to_handoff,
        });
      }

      std::fill_n(current_frame->sd.fds, 4, -1);
      return platf::capture_e::ok;
    }

    std::shared_ptr<platf::img_t> alloc_img() override {
      auto img = std::make_shared<egl::img_descriptor_t>();

      img->width = width;
      img->height = height;
      img->sequence = 0;
      img->serial = std::numeric_limits<decltype(img->serial)>::max();
      img->dmabuf_buffer_key = 0;
      img->data = nullptr;
      std::fill_n(img->sd.fds, 4, -1);

      return img;
    }

    std::unique_ptr<platf::avcodec_encode_device_t> make_avcodec_encode_device(platf::pix_fmt_e pix_fmt) override {
#ifdef POLARIS_BUILD_VAAPI
      if (mem_type == platf::mem_type_e::vaapi) {
        return va::make_avcodec_encode_device(width, height, 0, 0, true);
      }
#endif

#ifdef POLARIS_BUILD_CUDA
      if (mem_type == platf::mem_type_e::cuda) {
        if (pix_fmt == platf::pix_fmt_e::nv12 || pix_fmt == platf::pix_fmt_e::p010) {
          return cuda::make_avcodec_gl_encode_device(width, height, 0, 0);
        }

        return std::make_unique<platf::avcodec_encode_device_t>();
      }
#endif

#ifdef POLARIS_BUILD_VULKAN
      if (mem_type == platf::mem_type_e::vulkan) {
        return vk::make_avcodec_encode_device_vram(
          width,
          height,
          0,
          0,
          extcopy.capture_render_node()
        );
      }
#endif

      return std::make_unique<platf::avcodec_encode_device_t>();
    }

    int init(platf::mem_type_e hwdevice_type, const std::string &display_name, const ::video::config_t &config) {
      if (wlr_t::init(hwdevice_type, display_name, config)) {
        return -1;
      }

      BOOST_LOG(info) << "wlr: Attempting headless GPU-native DMA-BUF capture via ext-image-copy-capture"sv;
      blend_cursor = false;
      if (extcopy.init(display, interface.copy_capture_manager, interface.output_capture_source_manager, interface.dmabuf_interface, output, blend_cursor)) {
        BOOST_LOG(info) << "wlr: ext-image-copy-capture DMA-BUF initialization failed on this headless runtime"sv;
        return -1;
      }

      capture_ready = true;
      prefetched_frame_pending = true;
      return 0;
    }

    wl::extcopy_t extcopy;
    bool capture_ready {false};
    bool prefetched_frame_pending {false};
    bool blend_cursor {false};
    std::uint64_t sequence {};
  };

}  // namespace wl

namespace platf {
  std::shared_ptr<display_t> wl_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    if (hwdevice_type != platf::mem_type_e::system &&
        hwdevice_type != platf::mem_type_e::vaapi &&
        hwdevice_type != platf::mem_type_e::cuda &&
        hwdevice_type != platf::mem_type_e::vulkan) {
      BOOST_LOG(error) << "Could not initialize display with the given hw device type."sv;
      return nullptr;
    }

    const auto generation_policy = wlgrab_capture_policy::resolve_generation_policy(
      config.capture_generation,
      config::video.linux_display.use_cage_compositor,
      config::video.adapter_name
    );
    const bool use_private_compositor = generation_policy.use_private_compositor;
    const auto &adapter_name = generation_policy.adapter_name;

    bool prefer_ram_capture = (hwdevice_type == platf::mem_type_e::system);
    const bool gpu_native_capture_supported = wl::supports_gpu_native_capture(hwdevice_type);
    const auto direct_capture_path = wlgrab_capture_policy::select_direct_capture_path(
      hwdevice_type,
      gpu_native_capture_supported
    );
    bool prefer_linear_dmabuf = false;
    bool using_headless_ram_capture = false;
    bool try_headless_extcopy_dmabuf = false;
#ifdef __linux__
    if (!prefer_ram_capture &&
        use_private_compositor &&
        stream_runtime::labwc::is_running()) {
      auto runtime_state = stream_runtime::labwc::runtime_state();
      if (stream_runtime::labwc::should_attempt_gpu_native_cage_capture(runtime_state, hwdevice_type)) {
        static bool logged_windowed_gpu_native_attempt = false;
        if (!logged_windowed_gpu_native_attempt) {
          BOOST_LOG(info)
            << "wlr: Attempting GPU-native DMA-BUF capture on windowed labwc override"sv;
          logged_windowed_gpu_native_attempt = true;
        }
        prefer_linear_dmabuf = true;
      } else if (runtime_state.gpu_native_override_active) {
        // The override is active but this encoder's GPU-native path is not one
        // Polaris will import from. Falling back to RAM capture here is the
        // difference between a slower stream and a SIGSEGV at stream start.
        prefer_ram_capture = true;
        stream_stats::update_gpu_native_probe_attempt("windowed", "ineligible", "policy", "vaapi_gpu_native_dmabuf_disabled_for_stability");
        stream_stats::update_gpu_native_probe_selection("windowed_shm", "windowed_shm");
        static bool logged_windowed_gpu_native_refusal = false;
        if (!logged_windowed_gpu_native_refusal) {
          BOOST_LOG(warning)
            << "wlr: Using RAM capture path for the windowed labwc override because GPU-native DMA-BUF is disabled for VAAPI stability"sv;
          logged_windowed_gpu_native_refusal = true;
        }
      } else if (stream_runtime::labwc::should_report_headless_ram_capture_fallback(runtime_state)) {
        prefer_ram_capture = true;
        using_headless_ram_capture = true;
        try_headless_extcopy_dmabuf =
          stream_runtime::labwc::should_attempt_headless_extcopy_dmabuf(runtime_state, hwdevice_type);
        if (!try_headless_extcopy_dmabuf && hwdevice_type == platf::mem_type_e::vaapi) {
          const auto cached_result =
            stream_runtime::labwc::cached_headless_extcopy_dmabuf_probe_result();
          if (cached_result == std::optional<bool> {false}) {
            stream_stats::update_gpu_native_probe_attempt(
              "headless_extcopy",
              "ineligible",
              "cache",
              "cached_unsupported",
              true
            );
          } else {
            stream_stats::update_gpu_native_probe_attempt(
              "headless_extcopy",
              "ineligible",
              "policy",
              "vaapi_headless_dmabuf_disabled_for_stability"
            );
          }
          stream_stats::update_gpu_native_probe_selection("headless_shm", "headless_shm");
          if (stream_runtime::labwc::should_log_headless_ram_capture_warning()) {
            BOOST_LOG(info)
              << "wlr: Using RAM capture path for headless labwc because true-headless ext-image-copy-capture DMA-BUF is disabled for VAAPI stability"sv;
          }
        }
      } else {
        prefer_ram_capture = true;

        if (stream_runtime::labwc::should_report_windowed_ram_capture_fallback(runtime_state) &&
            stream_runtime::labwc::should_log_windowed_ram_capture_warning()) {
          BOOST_LOG(warning)
            << "wlr: Using RAM capture path for windowed labwc because nested DMA-BUF screencopy is not reliable on this stack"sv;
        }
      }
    }
#endif

    if (!prefer_ram_capture &&
        gpu_native_capture_supported &&
        (!use_private_compositor || !stream_runtime::labwc::is_running()) &&
        direct_capture_path == wlgrab_capture_policy::direct_capture_path_e::ram) {
      prefer_ram_capture = true;
      BOOST_LOG(warning)
        << "wlr: Using RAM capture path for the direct Wayland desktop because GPU-native DMA-BUF is disabled for VAAPI stability"sv;
    }

    if (!prefer_ram_capture && !gpu_native_capture_supported) {
      BOOST_LOG(info)
        << "wlr: Using RAM capture path because this build does not include a GPU-native uploader for the selected encoder"sv;
      prefer_ram_capture = true;
      prefer_linear_dmabuf = false;
    }

    if (!prefer_ram_capture && gpu_native_capture_supported) {
      auto wlr = std::make_shared<wl::wlr_vram_t>();
      wlr->dmabuf.prefer_linear_dmabuf = prefer_linear_dmabuf;
      if (wlr->init(hwdevice_type, display_name, config)) {
        return nullptr;
      }

      return wlr;
    }

    if (try_headless_extcopy_dmabuf && gpu_native_capture_supported) {
      bool device_pairing_failed = false;
      bool modifier_policy_failed = false;
      auto wlr = std::make_shared<wl::wlr_extcopy_vram_t>();
      if (!wlr->init(hwdevice_type, display_name, config)) {
        const auto capture_modifier = wlr->extcopy.capture_modifier();
        if (!stream_runtime::labwc::gpu_native_dmabuf_is_safe(
              hwdevice_type,
              wlgrab_capture_policy::gpu_native_capture_route_e::headless_extcopy,
              capture_modifier
            )) {
          modifier_policy_failed = true;
          const auto failure_reason = capture_modifier.has_value() ?
                                        "vaapi_headless_modifier_not_linear" :
                                        "vaapi_headless_modifier_unavailable";
          BOOST_LOG(warning)
            << "wlr: Disabling VAAPI headless ext-image-copy-capture DMA-BUF because the captured buffer modifier is "sv
            << (capture_modifier.has_value() ? std::to_string(*capture_modifier) : "unavailable")
            << "; only an actual linear modifier is accepted; using SHM fallback"sv;
#ifdef __linux__
          stream_runtime::labwc::update_headless_extcopy_dmabuf_probe_result(false);
#endif
          stream_stats::update_gpu_native_probe_attempt(
            "headless_extcopy",
            "failed",
            "modifier_policy",
            failure_reason
          );
        } else {
          const auto capture_render_node = wlr->extcopy.capture_render_node();
          const auto adapter_pairing = stream_stats::device_nodes_match(capture_render_node, adapter_name);
          if (!adapter_name.empty() &&
              !capture_render_node.empty() &&
              (!adapter_pairing.has_value() || !*adapter_pairing)) {
            device_pairing_failed = true;
            const bool identity_resolved = adapter_pairing.has_value();
            BOOST_LOG(warning)
              << "wlr: Disabling true-headless ext-image-copy-capture DMA-BUF because capture render_node=["sv
              << capture_render_node
              << (identity_resolved ? "] differs from encoder adapter=["sv : "] could not be identity-matched to encoder adapter=["sv)
              << adapter_name
              << "]; using SHM/system-memory fallback to avoid unsafe cross-GPU import"sv;
#ifdef __linux__
            stream_runtime::labwc::update_headless_extcopy_dmabuf_probe_result(false);
#endif
            stream_stats::update_gpu_native_probe_attempt(
              "headless_extcopy",
              "failed",
              "device_pairing",
              identity_resolved ? "cross_gpu_adapter_mismatch" : "device_identity_unresolved"
            );
          } else {
#ifdef __linux__
            stream_runtime::labwc::update_headless_extcopy_dmabuf_probe_result(true);
#endif
            stream_stats::update_gpu_native_probe_attempt("headless_extcopy", "succeeded");
            stream_stats::update_gpu_native_probe_selection("headless_extcopy_dmabuf");
            BOOST_LOG(info) << "wlr: Using ext-image-copy-capture DMA-BUF for headless labwc"sv;
            return wlr;
          }
        }
      }

#ifdef __linux__
      stream_runtime::labwc::update_headless_extcopy_dmabuf_probe_result(false);
#endif
      if (!device_pairing_failed && !modifier_policy_failed) {
        stream_stats::update_gpu_native_probe_attempt("headless_extcopy", "failed", "capture_init", "dmabuf_capture_not_initialized");
      }
      stream_stats::update_gpu_native_probe_selection("headless_shm", "headless_shm");
      BOOST_LOG(info) << "wlr: Headless ext-image-copy-capture DMA-BUF unavailable or unsafe; using SHM fallback"sv;
    }

    auto wlr = std::make_shared<wl::wlr_ram_t>();
    wlr->prefer_shm_screencopy = prefer_ram_capture;
    if (using_headless_ram_capture && stream_runtime::labwc::should_log_headless_ram_capture_warning()) {
      BOOST_LOG(info)
        << "wlr: Using RAM capture path for headless labwc because GPU-native override is not active"sv;
    }
    if (wlr->init(hwdevice_type, display_name, config)) {
      return nullptr;
    }

    return wlr;
  }

  static std::vector<std::string> wl_display_names_for_generation(
    const capture_generation::identity_t *generation
  ) {
    std::vector<std::string> display_names;
    const auto generation_policy = wlgrab_capture_policy::resolve_generation_policy(
      generation == nullptr ? capture_generation::identity_t {} : *generation,
      config::video.linux_display.use_cage_compositor,
      config::video.adapter_name
    );
    const bool generation_owned = generation_policy.owned;
    const bool use_private_compositor = generation_policy.use_private_compositor;

    // If cage is running, enumerate from cage. Otherwise use default display.
    const char *wayland_target = nullptr;
    std::string wayland_socket;
#ifdef __linux__
    if (use_private_compositor) {
      if (generation_owned) {
        const auto live_socket = stream_runtime::labwc::wayland_socket();
        const auto live_instance = stream_runtime::labwc::session_instance_id();
        if (!stream_runtime::labwc::is_running() ||
            !wlgrab_capture_policy::private_runtime_matches_generation(
              generation_policy,
              live_socket,
              live_instance
            )) {
          BOOST_LOG(error) << "wlr: Immutable private capture generation cannot enumerate a replacement labwc instance"sv;
          return {};
        }
        wayland_socket = generation_policy.private_wayland_socket;
      } else if (stream_runtime::labwc::is_running()) {
        wayland_socket = stream_runtime::labwc::wayland_socket();
      }
      if (!wayland_socket.empty()) {
        wayland_target = wayland_socket.c_str();
        BOOST_LOG(info) << "wlr: Enumerating displays from cage on "sv << wayland_socket;
      }
    }
    if (generation_owned && use_private_compositor && wayland_target == nullptr) {
      BOOST_LOG(error) << "wlr: Immutable private capture generation cannot enumerate without its exact labwc target"sv;
      return {};
    }
#endif

    wl::display_t display;
    if (display.init(wayland_target)) {
      return {};
    }

    wl::interface_t interface;
    interface.listen(display.registry());

    display.roundtrip();

    if (!interface[wl::interface_t::XDG_OUTPUT]) {
      BOOST_LOG(warning) << "Missing Wayland wire for xdg_output"sv;
      return {};
    }

    if (!interface[wl::interface_t::WLR_EXPORT_DMABUF]) {
      BOOST_LOG(warning) << "Missing Wayland wire for wlr-export-dmabuf"sv;
      return {};
    }

    wl::env_width = 0;
    wl::env_height = 0;

    for (auto &monitor : interface.monitors) {
      monitor->listen(interface.output_manager);
    }

    display.roundtrip();

    BOOST_LOG(info) << "-------- Start of Wayland monitor list --------"sv;

    for (int x = 0; x < interface.monitors.size(); ++x) {
      auto monitor = interface.monitors[x].get();

      wl::env_width = std::max(wl::env_width, (int) (monitor->viewport.offset_x + monitor->viewport.width));
      wl::env_height = std::max(wl::env_height, (int) (monitor->viewport.offset_y + monitor->viewport.height));

      BOOST_LOG(info) << "Monitor " << x << " is "sv << monitor->name << ": "sv << monitor->description;

      display_names.emplace_back(wlgrab_capture_policy::enumerated_monitor_identity(
        static_cast<std::size_t>(x),
        monitor->name
      ));
    }

    BOOST_LOG(info) << "--------- End of Wayland monitor list ---------"sv;

    return display_names;
  }

  std::vector<std::string> wl_display_names() {
    return wl_display_names_for_generation(nullptr);
  }

  std::vector<std::string> wl_display_names(
    const capture_generation::identity_t &generation
  ) {
    return wl_display_names_for_generation(&generation);
  }

}  // namespace platf
