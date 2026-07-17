/**
 * @file src/platform/linux/evdigrab.cpp
 * @brief Native EVDI frame capture backend.
 *
 * When the active virtual display is EVDI-backed, frames must be consumed
 * through libevdi by the process that opened the device (Polaris) — the same
 * pattern the DisplayLink daemon uses. Reading the EVDI card's scanout via
 * KMS returns black: the compositor renders and page-flips, but the pixels
 * are delivered to the device owner, not parked in a scanout buffer.
 *
 * Frames arrive as BGRA in system memory, so this backend feeds the same
 * CPU-side encode path as SHM screencopy capture.
 */

// standard includes
#include <chrono>
#include <cstring>
#include <thread>

// local includes
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/stream_stats.h"
#include "src/video.h"
#include "virtual_display.h"

using namespace std::literals;

namespace platf {

  namespace {

    struct evdi_img_t: public img_t {
      ~evdi_img_t() override {
        delete[] data;
        data = nullptr;
      }
    };

    class evdi_display_t: public display_t {
    public:
      int init(const ::video::config_t &config) {
        delay = std::chrono::nanoseconds {1s} / config.framerate;

        int w = 0, h = 0;
        if (!virtual_display::evdi_capture::dimensions(w, h) || w <= 0 || h <= 0) {
          BOOST_LOG(error) << "evdigrab: no active EVDI session to capture"sv;
          return -1;
        }

        width = w;
        height = h;
        env_width = w;
        env_height = h;
        offset_x = 0;
        offset_y = 0;

        BOOST_LOG(info) << "evdigrab: capturing EVDI display ["sv
                        << virtual_display::evdi_capture::output_name()
                        << "] "sv << width << 'x' << height;
        return 0;
      }

      capture_e capture(const push_captured_image_cb_t &push_captured_image_cb, const pull_free_image_cb_t &pull_free_image_cb, bool *cursor) override {
        auto next_frame = std::chrono::steady_clock::now();

        while (true) {
          auto now = std::chrono::steady_clock::now();
          if (next_frame > now) {
            std::this_thread::sleep_for(next_frame - now);
          }
          next_frame += delay;
          if (next_frame < now) {  // major slowdown; reset pacing
            next_frame = now + delay;
          }

          std::shared_ptr<img_t> img_out;
          auto status = snapshot(pull_free_image_cb, img_out, 1000ms);
          switch (status) {
            case capture_e::reinit:
            case capture_e::error:
            case capture_e::interrupted:
              return status;
            case capture_e::timeout:
              if (!push_captured_image_cb(std::move(img_out), false)) {
                return capture_e::ok;
              }
              break;
            case capture_e::ok:
              if (!push_captured_image_cb(std::move(img_out), true)) {
                return capture_e::ok;
              }
              break;
            default:
              BOOST_LOG(error) << "evdigrab: unrecognized capture status ["sv << (int) status << ']';
              return status;
          }
        }
      }

      capture_e snapshot(const pull_free_image_cb_t &pull_free_image_cb, std::shared_ptr<img_t> &img_out, std::chrono::milliseconds timeout) {
        virtual_display::evdi_capture::frame_view_t frame;
        const int rc = virtual_display::evdi_capture::grab_frame(frame, timeout);
        if (rc < 0) {
          // Session torn down under us (stream ending / display destroyed).
          BOOST_LOG(info) << "evdigrab: EVDI session gone; reinitializing capture"sv;
          return capture_e::reinit;
        }
        if (rc == 0) {
          return capture_e::timeout;  // no damage this frame window
        }

        // Mode changed mid-stream — geometry no longer matches what the
        // encoder was initialized with; force a capture reinit.
        if (frame.width != width || frame.height != height) {
          BOOST_LOG(info) << "evdigrab: EVDI mode changed ("sv << width << 'x' << height
                          << " -> "sv << frame.width << 'x' << frame.height << "); reinitializing"sv;
          return capture_e::reinit;
        }

        if (!pull_free_image_cb(img_out)) {
          return capture_e::interrupted;
        }

        const auto frame_timestamp = std::chrono::steady_clock::now();
        const int copy_h = std::min(frame.height, (int) img_out->height);
        const int copy_bytes = std::min(frame.stride, (int) img_out->row_pitch);
        for (int y = 0; y < copy_h; ++y) {
          std::memcpy(img_out->data + (std::size_t) y * img_out->row_pitch,
                      frame.data + (std::size_t) y * frame.stride,
                      copy_bytes);
        }

        img_out->frame_timestamp = frame_timestamp;
        img_out->frame_metadata = {
          .transport = frame_transport_e::shm,
          .residency = frame_residency_e::cpu,
          .format = frame_format_e::bgra8,
        };
        stream_stats::update_capture_metadata(img_out->frame_metadata);
        if (!capture_metadata_logged) {
          capture_metadata_logged = true;
          BOOST_LOG(info) << "evdigrab: native EVDI frame grab active (cpu/bgra8)"sv;
        }
        return capture_e::ok;
      }

      std::shared_ptr<img_t> alloc_img() override {
        auto img = std::make_shared<evdi_img_t>();
        img->width = width;
        img->height = height;
        img->pixel_pitch = 4;
        img->row_pitch = img->pixel_pitch * width;
        img->data = new std::uint8_t[(std::size_t) height * img->row_pitch];
        return img;
      }

      int dummy_img(img_t *img) override {
        if (img && img->data) {
          std::memset(img->data, 0, (std::size_t) img->height * img->row_pitch);
        }
        return 0;
      }

    private:
      std::chrono::nanoseconds delay {};
      bool capture_metadata_logged = false;
    };

  }  // namespace

  std::shared_ptr<display_t> evdi_display(mem_type_e hwdevice_type, const std::string &display_name, const ::video::config_t &config) {
    // Frames land in system memory; CUDA's zero-copy path can't consume them.
    if (hwdevice_type == mem_type_e::cuda) {
      return nullptr;
    }

    auto disp = std::make_shared<evdi_display_t>();
    if (disp->init(config)) {
      return nullptr;
    }
    return disp;
  }

}  // namespace platf
