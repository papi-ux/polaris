/**
 * @file src/platform/linux/cage_screencopy.cpp
 * @brief Direct wlr-screencopy capture from cage/labwc (no portal, no picker).
 *
 * Extracted from portal_grab (stream-runtime structure S1). Do not grow
 * portal_grab with more Wayland client code — extend this TU / wlgrab instead.
 */

#include "src/platform/linux/cage_screencopy.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <limits>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <errno.h>
#include <poll.h>
#include <sys/mman.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wlr-screencopy-unstable-v1.h>

#include "src/logging.h"

using namespace std::literals;

namespace cage_screencopy {

  namespace {

  struct screencopy_state_t {
    // Wayland globals
    wl_display *display = nullptr;
    wl_registry *registry = nullptr;
    wl_shm *shm = nullptr;
    wl_output *output = nullptr;
    zwlr_screencopy_manager_v1 *screencopy_mgr = nullptr;

    // SHM buffer
    int shm_fd = -1;
    void *shm_data = nullptr;
    size_t shm_size = 0;
    wl_shm_pool *shm_pool = nullptr;
    wl_buffer *shm_buffer = nullptr;

    // Frame info from buffer event
    uint32_t fmt = 0, width = 0, height = 0, stride = 0;
    bool buffer_info_received = false;
    bool frame_ready = false;
    bool frame_failed = false;

    void cleanup_buffer() {
      if (shm_buffer) { wl_buffer_destroy(shm_buffer); shm_buffer = nullptr; }
      if (shm_pool) { wl_shm_pool_destroy(shm_pool); shm_pool = nullptr; }
      if (shm_data) { munmap(shm_data, shm_size); shm_data = nullptr; }
      if (shm_fd >= 0) { ::close(shm_fd); shm_fd = -1; }
      shm_size = 0;
    }

    ~screencopy_state_t() {
      cleanup_buffer();
      if (screencopy_mgr) zwlr_screencopy_manager_v1_destroy(screencopy_mgr);
      if (shm) wl_shm_destroy(shm);
      if (output) wl_output_destroy(output);
      if (registry) wl_registry_destroy(registry);
      if (display) { wl_display_flush(display); wl_display_disconnect(display); }
    }
  };

  // Registry listener
  void sc_registry_global(void *data, wl_registry *reg, uint32_t id,
    const char *iface, uint32_t version) {
    auto *st = static_cast<screencopy_state_t *>(data);
    if (!std::strcmp(iface, wl_shm_interface.name)) {
      st->shm = static_cast<wl_shm *>(wl_registry_bind(reg, id, &wl_shm_interface, 1));
    } else if (!std::strcmp(iface, wl_output_interface.name) && !st->output) {
      st->output = static_cast<wl_output *>(wl_registry_bind(reg, id, &wl_output_interface, 1));
    } else if (!std::strcmp(iface, zwlr_screencopy_manager_v1_interface.name)) {
      st->screencopy_mgr = static_cast<zwlr_screencopy_manager_v1 *>(
        wl_registry_bind(reg, id, &zwlr_screencopy_manager_v1_interface, std::min(version, 3u)));
    }
  }
  void sc_registry_global_remove(void *, wl_registry *, uint32_t) {}
  const wl_registry_listener sc_registry_listener = {
    sc_registry_global, sc_registry_global_remove
  };

  // Frame listener callbacks
  void sc_frame_buffer(void *data, zwlr_screencopy_frame_v1 *,
    uint32_t format, uint32_t w, uint32_t h, uint32_t stride) {
    auto *st = static_cast<screencopy_state_t *>(data);
    st->fmt = format; st->width = w; st->height = h; st->stride = stride;
    st->buffer_info_received = true;
    BOOST_LOG(info) << "screencopy: frame format=" << format
                    << " (" << (format == 0 ? "ARGB8888" : format == 1 ? "XRGB8888" : "other") << ")"
                    << " " << w << "x" << h << " stride=" << stride;
  }
  void sc_frame_flags(void *, zwlr_screencopy_frame_v1 *, uint32_t) {}
  void sc_frame_ready(void *data, zwlr_screencopy_frame_v1 *,
    uint32_t, uint32_t, uint32_t) {
    auto *st = static_cast<screencopy_state_t *>(data);
    st->frame_ready = true;
  }
  void sc_frame_failed(void *data, zwlr_screencopy_frame_v1 *) {
    auto *st = static_cast<screencopy_state_t *>(data);
    st->frame_failed = true;
  }
  void sc_frame_damage(void *, zwlr_screencopy_frame_v1 *,
    uint32_t, uint32_t, uint32_t, uint32_t) {}
  void sc_frame_linux_dmabuf(void *, zwlr_screencopy_frame_v1 *,
    uint32_t, uint32_t, uint32_t) {}
  void sc_frame_buffer_done(void *data, zwlr_screencopy_frame_v1 *frame) {
    auto *st = static_cast<screencopy_state_t *>(data);
    if (!st->buffer_info_received || !st->shm) {
      st->frame_failed = true;
      return;
    }

    // Create SHM buffer in the compositor's native format
    st->cleanup_buffer();
    if (st->height == 0 || st->stride == 0 ||
        static_cast<std::size_t>(st->stride) > std::numeric_limits<std::size_t>::max() / st->height ||
        static_cast<std::size_t>(st->stride) * st->height > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
      BOOST_LOG(error) << "screencopy: Invalid SHM frame size"sv;
      st->frame_failed = true;
      return;
    }
    st->shm_size = static_cast<std::size_t>(st->stride) * st->height;
    st->shm_fd = memfd_create("polaris-sc", MFD_CLOEXEC);
    if (st->shm_fd < 0 || ftruncate(st->shm_fd, st->shm_size) < 0) {
      st->frame_failed = true;
      return;
    }
    st->shm_data = mmap(nullptr, st->shm_size, PROT_READ | PROT_WRITE, MAP_SHARED, st->shm_fd, 0);
    if (st->shm_data == MAP_FAILED) { st->shm_data = nullptr; st->frame_failed = true; return; }

    st->shm_pool = wl_shm_create_pool(st->shm, st->shm_fd, st->shm_size);
    st->shm_buffer = wl_shm_pool_create_buffer(st->shm_pool, 0,
      st->width, st->height, st->stride, st->fmt);

    // Request copy
    zwlr_screencopy_frame_v1_copy(frame, st->shm_buffer);
  }

  const zwlr_screencopy_frame_v1_listener sc_frame_listener = {
    .buffer = sc_frame_buffer,
    .flags = sc_frame_flags,
    .ready = sc_frame_ready,
    .failed = sc_frame_failed,
    .damage = sc_frame_damage,
    .linux_dmabuf = sc_frame_linux_dmabuf,
    .buffer_done = sc_frame_buffer_done,
  };

  // Stop flag for screencopy — set when session ends / display dtor
  std::atomic<bool> g_screencopy_stop{false};

  }  // namespace

  void request_stop() {
    g_screencopy_stop = true;
  }

  platf::capture_e capture(
    const std::string &cage_socket,
    int req_width, int req_height,
    const platf::display_t::push_captured_image_cb_t &push_cb,
    const platf::display_t::pull_free_image_cb_t &pull_cb,
    bool *cursor,
    int &out_width, int &out_height) {

    (void) req_width;
    (void) req_height;

    g_screencopy_stop = false;

    screencopy_state_t st;
    st.display = wl_display_connect(cage_socket.c_str());
    if (!st.display) {
      BOOST_LOG(warning) << "screencopy: Cannot connect to "sv << cage_socket;
      return platf::capture_e::reinit;
    }

    st.registry = wl_display_get_registry(st.display);
    wl_registry_add_listener(st.registry, &sc_registry_listener, &st);
    wl_display_roundtrip(st.display);

    if (!st.screencopy_mgr || !st.output || !st.shm) {
      BOOST_LOG(warning) << "screencopy: Missing required protocols from cage"sv;
      return platf::capture_e::reinit;
    }

    BOOST_LOG(info) << "screencopy: Connected to cage on "sv << cage_socket;

    // Shared frame buffer between screencopy thread and capture() caller
    std::mutex frame_mtx;
    std::condition_variable frame_cv;
    struct published_frame_t {
      std::vector<uint8_t> bytes;
      std::uint32_t width {0};
      std::uint32_t height {0};
      std::uint32_t stride {0};
    } published_frame;
    bool has_frame = false;
    std::atomic<bool> sc_running{true};
    std::atomic<bool> sc_error{false};

    // Run screencopy in a background thread so capture() can be interrupted
    auto sc_thread = std::thread([&]() {
      while (sc_running && !g_screencopy_stop) {
        st.buffer_info_received = false;
        st.frame_ready = false;
        st.frame_failed = false;

        // Check connection health before requesting a frame
        if (wl_display_get_error(st.display) != 0) {
          BOOST_LOG(warning) << "screencopy: Wayland connection error, stopping"sv;
          sc_error = true;
          return;
        }

        auto *frame = zwlr_screencopy_manager_v1_capture_output(
          st.screencopy_mgr, *cursor ? 1 : 0, st.output);
        zwlr_screencopy_frame_v1_add_listener(frame, &sc_frame_listener, &st);

        while (!st.frame_ready && !st.frame_failed && sc_running && !g_screencopy_stop) {
          // Flush before dispatch to detect broken connections
          const int flush_result = wl_display_flush(st.display);
          if (flush_result < 0 && errno != EAGAIN) {
            BOOST_LOG(warning) << "screencopy: Wayland flush failed (compositor died?), stopping"sv;
            sc_error = true;
            zwlr_screencopy_frame_v1_destroy(frame);
            return;
          }
          pollfd display_poll {
            .fd = wl_display_get_fd(st.display),
            .events = static_cast<short>(POLLIN | (flush_result < 0 ? POLLOUT : 0)),
            .revents = 0,
          };
          const int poll_result = ::poll(&display_poll, 1, 100);
          if (poll_result < 0 && errno == EINTR) {
            continue;
          }
          if (poll_result < 0 || (display_poll.revents & (POLLERR | POLLHUP | POLLNVAL))) {
            BOOST_LOG(warning) << "screencopy: Wayland connection poll failed, stopping"sv;
            sc_error = true;
            zwlr_screencopy_frame_v1_destroy(frame);
            return;
          }
          if (poll_result == 0) {
            continue;
          }
          if (!(display_poll.revents & POLLIN)) {
            continue;
          }
          if (wl_display_dispatch(st.display) < 0) {
            BOOST_LOG(warning) << "screencopy: Wayland dispatch failed, stopping"sv;
            sc_error = true;
            zwlr_screencopy_frame_v1_destroy(frame);
            return;
          }
        }
        zwlr_screencopy_frame_v1_destroy(frame);

        if (!sc_running || g_screencopy_stop || st.frame_failed) {
          if (st.frame_failed) sc_error = true;
          return;
        }

        // Copy frame to shared buffer
        if (st.shm_data && st.shm_size > 0) {
          std::lock_guard lk(frame_mtx);
          published_frame.bytes.resize(st.shm_size);
          std::memcpy(published_frame.bytes.data(), st.shm_data, st.shm_size);
          published_frame.width = st.width;
          published_frame.height = st.height;
          published_frame.stride = st.stride;
          has_frame = true;
          frame_cv.notify_one();
        }
      }
    });

    // Consume frames from the screencopy thread
    while (!g_screencopy_stop) {
      std::unique_lock lk(frame_mtx);
      if (!has_frame) {
        frame_cv.wait_for(lk, 100ms, [&] { return has_frame || g_screencopy_stop || sc_error.load(); });
      }

      if (g_screencopy_stop || sc_error) {
        lk.unlock();
        break;
      }

      if (!has_frame) {
        lk.unlock();
        // Timeout — check if encoder wants us to stop
        std::shared_ptr<platf::img_t> timeout_img;
        if (!push_cb(std::move(timeout_img), false)) {
          break;
        }
        continue;
      }

      auto frame = std::move(published_frame);
      has_frame = false;
      lk.unlock();

      const auto required_bytes = static_cast<std::size_t>(frame.stride) * frame.height;
      if (frame.width == 0 || frame.height == 0 || frame.stride == 0 ||
          frame.height > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
          frame.width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
          frame.stride > frame.bytes.size() || required_bytes > frame.bytes.size()) {
        BOOST_LOG(error) << "screencopy: Published frame metadata exceeds its buffer"sv;
        sc_error = true;
        break;
      }

      out_width = static_cast<int>(frame.width);
      out_height = static_cast<int>(frame.height);

      std::shared_ptr<platf::img_t> img_out;
      if (!pull_cb(img_out)) {
        break;
      }

      int copy_h = std::min(static_cast<int>(frame.height), img_out->height);
      int copy_w = std::min(static_cast<int>(frame.width), img_out->width);

      bool is_3bpp = static_cast<std::size_t>(frame.stride) == static_cast<std::size_t>(frame.width) * 3;

      if (is_3bpp) {
        if (static_cast<std::size_t>(copy_w) * 4 > static_cast<std::size_t>(img_out->row_pitch)) {
          BOOST_LOG(error) << "screencopy: Destination row is too small for converted frame"sv;
          sc_error = true;
          break;
        }
        // BGR888 (3 bytes/pixel) → BGRA (4 bytes/pixel) conversion
        // wlroots BGR888 = memory order [B, G, R] per pixel
        // CUDA kernel reads texture as BGRA = memory order [B, G, R, A]
        // But the actual byte order from headless may be [R, G, B] — swap R↔B
        for (int y = 0; y < copy_h; ++y) {
          const uint8_t *src = frame.bytes.data() + static_cast<std::size_t>(y) * frame.stride;
          uint8_t *dst = img_out->data + y * img_out->row_pitch;
          for (int x = 0; x < copy_w; ++x) {
            dst[x * 4 + 0] = src[x * 3 + 2];  // B ← src R (swap)
            dst[x * 4 + 1] = src[x * 3 + 1];  // G
            dst[x * 4 + 2] = src[x * 3 + 0];  // R ← src B (swap)
            dst[x * 4 + 3] = 255;              // A (opaque)
          }
        }
      } else {
        // 4bpp format (XRGB8888/ARGB8888) — direct copy
        int copy_bytes = std::min(static_cast<int>(frame.stride), img_out->row_pitch);
        for (int y = 0; y < copy_h; ++y) {
          std::memcpy(img_out->data + y * img_out->row_pitch,
            frame.bytes.data() + static_cast<std::size_t>(y) * frame.stride, copy_bytes);
        }
      }
      img_out->frame_timestamp = std::chrono::steady_clock::now();

      if (!push_cb(std::move(img_out), true)) {
        break;
      }
    }

    // Signal thread to stop
    sc_running = false;
    g_screencopy_stop = true;

    // The capture loop polls the Wayland fd with a bounded timeout, so it can
    // always observe the stop flag without detaching from stack-owned state.
    if (sc_thread.joinable()) {
      sc_thread.join();
    }

    BOOST_LOG(info) << "screencopy: Capture stopped"sv;
    return sc_error ? platf::capture_e::reinit : platf::capture_e::ok;
  }

}  // namespace cage_screencopy
