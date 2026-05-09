/**
 * @file src/platform/linux/wlgrab.cpp
 * @brief Definitions for wlgrab capture.
 */
// standard includes
#include <thread>

// local includes
#include "cage_display_router.h"
#include "cuda.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/stream_stats.h"
#include "src/video.h"
#include "vaapi.h"
#include "wayland.h"
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

      // If cage compositor is running, connect to cage's Wayland socket
      // for direct wlr-screencopy capture (no portal, no picker).
      const char *wayland_target = nullptr;
#ifdef __linux__
      if (config::video.linux_display.use_cage_compositor && cage_display_router::is_running()) {
        static std::string cached_socket;
        cached_socket = cage_display_router::get_wayland_socket();
        if (!cached_socket.empty()) {
          wayland_target = cached_socket.c_str();
          BOOST_LOG(info) << "wlr: Targeting cage compositor on "sv << cached_socket;
        }
      }
#endif

      if (display.init(wayland_target)) {
        return -1;
      }

      interface.listen(display.registry());

      display.roundtrip();

      if (!interface[wl::interface_t::XDG_OUTPUT]) {
        if (!config::video.linux_display.use_cage_compositor) {
          BOOST_LOG(error) << "Missing Wayland wire for xdg_output"sv;
          return -1;
        }
        BOOST_LOG(info) << "wlr: xdg_output not available (cage mode — using direct output)"sv;
      }

      if (!interface[wl::interface_t::WLR_EXPORT_DMABUF]) {
        // wlr-export-dmabuf is optional when using cage — screencopy handles frame capture.
        // Only fail if we're NOT targeting cage (i.e., capturing the desktop directly).
        if (!config::video.linux_display.use_cage_compositor) {
          BOOST_LOG(error) << "Missing Wayland wire for wlr-export-dmabuf"sv;
          return -1;
        }
        BOOST_LOG(info) << "wlr: wlr-export-dmabuf not available (cage mode — using screencopy only)"sv;
      }

      auto monitor = interface.monitors[0].get();

      if (!display_name.empty()) {
        auto streamedMonitor = util::from_view(display_name);

        if (streamedMonitor >= 0 && streamedMonitor < interface.monitors.size()) {
          monitor = interface.monitors[streamedMonitor].get();
        }
      }

      monitor->listen(interface.output_manager);

      display.roundtrip();

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
          return platf::capture_e::timeout;
        }
      } while (dmabuf.status == dmabuf_t::WAITING);

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

    platf::mem_type_e mem_type;
    bool capture_transport_logged = false;
    bool prefer_shm_screencopy = false;
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
      auto next_frame = std::chrono::steady_clock::now();

      sleep_overshoot_logger.reset();

      while (true) {
        auto now = std::chrono::steady_clock::now();

        if (next_frame > now) {
          std::this_thread::sleep_for(next_frame - now);
          sleep_overshoot_logger.first_point(next_frame);
          sleep_overshoot_logger.second_point_now_and_log();
        }

        next_frame += delay;
        if (next_frame < now) {  // some major slowdown happened; we couldn't keep up
          next_frame = now + delay;
        }

        std::shared_ptr<platf::img_t> img_out;
        auto status = snapshot(pull_free_image_cb, img_out, 1000ms, *cursor);
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
          auto total_time = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - capture_start
          );
          auto ingest_time = total_time - last_dispatch_time;
          stream_stats::update_capture_profile({
            .transport = img_out->frame_metadata.transport,
            .dispatch_time = last_dispatch_time,
            .ingest_time = ingest_time,
            .total_time = total_time,
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
        auto total_time = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - capture_start
        );
        auto ingest_time = total_time - last_dispatch_time;
        stream_stats::update_capture_profile({
          .transport = img_out->frame_metadata.transport,
          .dispatch_time = last_dispatch_time,
          .ingest_time = ingest_time,
          .total_time = total_time,
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

    egl::display_t egl_display;
    egl::ctx_t ctx;
  };

  class wlr_vram_t: public wlr_t {
  public:
    platf::capture_e capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) override {
      auto next_frame = std::chrono::steady_clock::now();

      sleep_overshoot_logger.reset();

      while (true) {
        auto now = std::chrono::steady_clock::now();

        if (next_frame > now) {
          std::this_thread::sleep_for(next_frame - now);
          sleep_overshoot_logger.first_point(next_frame);
          sleep_overshoot_logger.second_point_now_and_log();
        }

        next_frame += delay;
        if (next_frame < now) {  // some major slowdown happened; we couldn't keep up
          next_frame = now + delay;
        }

        std::shared_ptr<platf::img_t> img_out;
        auto status = snapshot(pull_free_image_cb, img_out, 1000ms, *cursor);
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
        auto total_time = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - capture_start
        );
        auto ingest_time = total_time - last_dispatch_time;
        stream_stats::update_capture_profile({
          .transport = img->frame_metadata.transport,
          .dispatch_time = last_dispatch_time,
          .ingest_time = ingest_time,
          .total_time = total_time,
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
      auto next_frame = std::chrono::steady_clock::now();

      sleep_overshoot_logger.reset();

      while (true) {
        auto now = std::chrono::steady_clock::now();

        if (next_frame > now) {
          std::this_thread::sleep_for(next_frame - now);
          sleep_overshoot_logger.first_point(next_frame);
          sleep_overshoot_logger.second_point_now_and_log();
        }

        next_frame += delay;
        if (next_frame < now) {
          next_frame = now + delay;
        }

        std::shared_ptr<platf::img_t> img_out;
        auto status = snapshot(pull_free_image_cb, img_out, cursor);
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

    platf::capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<platf::img_t> &img_out, bool *cursor) {
      auto capture_start = std::chrono::steady_clock::now();
      auto cursor_enabled = cursor ? *cursor : false;

      if (!capture_ready || cursor_enabled != blend_cursor) {
        blend_cursor = cursor_enabled;
        if (extcopy.init(display, interface.copy_capture_manager, interface.output_capture_source_manager, interface.dmabuf_interface, output, blend_cursor)) {
          return platf::capture_e::reinit;
        }
        capture_ready = true;
      } else {
        extcopy.capture(display);
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
      };
      stream_stats::update_capture_metadata(img->frame_metadata);
      log_capture_metadata(img->frame_metadata);
      if (capture_profile_enabled()) {
        auto total_time = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - capture_start
        );
        stream_stats::update_capture_profile({
          .transport = img->frame_metadata.transport,
          .dispatch_time = total_time,
          .ingest_time = 0us,
          .total_time = total_time,
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

      return std::make_unique<platf::avcodec_encode_device_t>();
    }

    int init(platf::mem_type_e hwdevice_type, const std::string &display_name, const ::video::config_t &config) {
      if (wlr_t::init(hwdevice_type, display_name, config)) {
        return -1;
      }

      BOOST_LOG(info) << "wlr: Attempting experimental headless DMA-BUF capture via ext-image-copy-capture"sv;
      blend_cursor = false;
      if (extcopy.init(display, interface.copy_capture_manager, interface.output_capture_source_manager, interface.dmabuf_interface, output, blend_cursor)) {
        BOOST_LOG(info) << "wlr: ext-image-copy-capture DMA-BUF initialization failed on this headless runtime"sv;
        return -1;
      }

      capture_ready = true;
      return 0;
    }

    wl::extcopy_t extcopy;
    bool capture_ready {false};
    bool blend_cursor {false};
    std::uint64_t sequence {};
  };

}  // namespace wl

namespace platf {
  std::shared_ptr<display_t> wl_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    if (hwdevice_type != platf::mem_type_e::system && hwdevice_type != platf::mem_type_e::vaapi && hwdevice_type != platf::mem_type_e::cuda) {
      BOOST_LOG(error) << "Could not initialize display with the given hw device type."sv;
      return nullptr;
    }

    bool prefer_ram_capture = (hwdevice_type == platf::mem_type_e::system);
    const bool gpu_native_capture_supported = wl::supports_gpu_native_capture(hwdevice_type);
    bool prefer_linear_dmabuf = false;
    bool using_headless_ram_capture = false;
    bool try_headless_extcopy_dmabuf = false;
#ifdef __linux__
    if (!prefer_ram_capture &&
        config::video.linux_display.use_cage_compositor &&
        cage_display_router::is_running()) {
      auto runtime_state = cage_display_router::runtime_state();
      if (cage_display_router::should_attempt_gpu_native_cage_capture(runtime_state)) {
        static bool logged_windowed_gpu_native_attempt = false;
        if (!logged_windowed_gpu_native_attempt) {
          BOOST_LOG(info)
            << "wlr: Attempting GPU-native DMA-BUF capture on windowed labwc override"sv;
          logged_windowed_gpu_native_attempt = true;
        }
        prefer_linear_dmabuf = true;
      } else if (cage_display_router::should_report_headless_ram_capture_fallback(runtime_state)) {
        prefer_ram_capture = true;
        using_headless_ram_capture = true;
        try_headless_extcopy_dmabuf =
          cage_display_router::should_attempt_headless_extcopy_dmabuf(runtime_state);

        // Even when GPU-native override is not active, opportunistically probe
        // ext-image-copy DMA-BUF in headless cage mode. If unsupported, we
        // fall back to the existing SHM path and keep current behavior.
        if (!try_headless_extcopy_dmabuf && gpu_native_capture_supported) {
          try_headless_extcopy_dmabuf = true;
          static bool logged_headless_extcopy_probe = false;
          if (!logged_headless_extcopy_probe) {
            BOOST_LOG(info)
              << "wlr: Probing ext-image-copy-capture DMA-BUF in headless labwc before falling back to SHM"sv;
            logged_headless_extcopy_probe = true;
          }
        }
      } else {
        prefer_ram_capture = true;

        if (cage_display_router::should_report_windowed_ram_capture_fallback(runtime_state) &&
            cage_display_router::should_log_windowed_ram_capture_warning()) {
          BOOST_LOG(warning)
            << "wlr: Using RAM capture path for windowed labwc because nested DMA-BUF screencopy is not reliable on this stack"sv;
        }
      }
    }
#endif

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
      auto wlr = std::make_shared<wl::wlr_extcopy_vram_t>();
      if (!wlr->init(hwdevice_type, display_name, config)) {
#ifdef __linux__
        cage_display_router::update_headless_extcopy_dmabuf_probe_result(true);
#endif
        BOOST_LOG(info) << "wlr: Using ext-image-copy-capture DMA-BUF for headless labwc"sv;
        return wlr;
      }

#ifdef __linux__
      cage_display_router::update_headless_extcopy_dmabuf_probe_result(false);
#endif
      BOOST_LOG(info) << "wlr: Headless ext-image-copy-capture DMA-BUF unavailable; using SHM fallback"sv;
    }

    auto wlr = std::make_shared<wl::wlr_ram_t>();
    wlr->prefer_shm_screencopy = prefer_ram_capture;
    if (using_headless_ram_capture && cage_display_router::should_log_headless_ram_capture_warning()) {
      BOOST_LOG(info)
        << "wlr: Using RAM capture path for headless labwc because GPU-native override is not active"sv;
    }
    if (wlr->init(hwdevice_type, display_name, config)) {
      return nullptr;
    }

    return wlr;
  }

  std::vector<std::string> wl_display_names() {
    std::vector<std::string> display_names;

    // If cage is running, enumerate from cage. Otherwise use default display.
    const char *wayland_target = nullptr;
#ifdef __linux__
    if (config::video.linux_display.use_cage_compositor && cage_display_router::is_running()) {
      static std::string cached_socket;
      cached_socket = cage_display_router::get_wayland_socket();
      if (!cached_socket.empty()) {
        wayland_target = cached_socket.c_str();
        BOOST_LOG(info) << "wlr: Enumerating displays from cage on "sv << cached_socket;
      }
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

      display_names.emplace_back(std::to_string(x));
    }

    BOOST_LOG(info) << "--------- End of Wayland monitor list ---------"sv;

    return display_names;
  }

}  // namespace platf
