/**
 * @file src/platform/linux/portal_grab.cpp
 * @brief XDG Desktop Portal ScreenCast capture backend.
 *
 * Captures a window (or monitor) via the xdg-desktop-portal ScreenCast D-Bus
 * API, receiving frames through PipeWire. This is the primary capture backend
 * for the Polaris cage-as-window architecture.
 */

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <drm_fourcc.h>
#include <spa/param/video/raw.h>
#include <unistd.h>

#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/platform/linux/graphics.h"
#include "src/video.h"
#include "src/stream_stats.h"

#include "src/platform/linux/stream_runtime.h"
#ifdef POLARIS_BUILD_WAYLAND
  #include "src/platform/linux/cage_screencopy.h"
  #include "src/platform/linux/kwingrab.h"
#endif
#include "src/platform/linux/pipewire_capture.h"
#include "src/platform/linux/portal_session.h"
#include "src/platform/linux/session_media.h"

#ifdef POLARIS_BUILD_CUDA
  #include "src/platform/linux/cuda.h"
#endif
#ifdef POLARIS_BUILD_VAAPI
  #include "src/platform/linux/vaapi.h"
#endif

using namespace std::literals;

namespace portal {


  // D-Bus CreateSession/SelectSources/Start live in portal_session.cpp (S2).
  // portal_session_t + create_portal_session() are declared in portal_session.h.
  // Cage/labwc SHM capture lives in cage_screencopy.{h,cpp} (S1 extract).

  // -----------------------------------------------------------------------
  // Session media cache (S4 ownership)
  //
  // Process-wide ScreenCast + PipeWire handle is a *session media cache*:
  //   - Lazily created by portal_display_t init/capture; reused across reconnects.
  //   - Released ONLY via portal::release_global_capture() (single public entry;
  //     idempotent — safe if session_media and streaming_will_stop both call it).
  //   - Ordered stop path remains session_media::prepare_for_stop →
  //     browser_stream teardown → release_global_capture once → bounded join →
  //     then caller kills nested compositor. Do not add parallel release owners.
  //   - One mutex (g_media_mu) protects portal session + capture together so
  //     dual-mutex lock-order / UAF cannot recur. shared_ptr capture lets release
  //     move the cache out while display threads still hold a ref.
  // -----------------------------------------------------------------------

  struct media_cache_t {
    std::unique_ptr<portal_session_t> portal;
    // Opaque lifetime guard for a Wayland-only kwingrab session. Keeping the
    // cache type independent lets portal/PipeWire builds omit Wayland helpers.
    std::shared_ptr<void> kwin;
    std::shared_ptr<pipewire_capture::capture_t> capture;
    int requested_width = 0;
    int requested_height = 0;
    platf::mem_type_e mem_type = platf::mem_type_e::system;
    std::string adapter;
    // Last EnumFormat preference: prefer_hdr (force ∧ dynamicRange>0) or
    // prefer_sdr (dynamicRange<=0). Reuse only when both match.
    bool prefer_hdr = false;
    bool prefer_sdr = false;

    void clear_meta() {
      requested_width = 0;
      requested_height = 0;
      mem_type = platf::mem_type_e::system;
      adapter.clear();
      prefer_hdr = false;
      prefer_sdr = false;
    }

    void reset_all() {
      capture.reset();
      portal.reset();
      kwin.reset();
      clear_meta();
    }

    bool empty() const {
      return !capture && !portal && !kwin;
    }
  };

  static std::mutex g_media_mu;
  static std::mutex g_capture_transition_mu;
  static media_cache_t g_media;

  struct portal_cleanup_state_t {
    std::mutex mutex;
    std::condition_variable changed;
    std::thread worker;
    bool running = false;
  };

  // The state owns the joinable cleanup thread and intentionally has process
  // lifetime. A blocked PipeWire destructor therefore cannot outlive the
  // synchronization it reports completion through during static teardown.
  static portal_cleanup_state_t &portal_cleanup_state() {
    static auto *state = new portal_cleanup_state_t;
    return *state;
  }

  static void reap_portal_cleanup() {
    auto &state = portal_cleanup_state();
    std::thread completed;
    {
      std::lock_guard lock(state.mutex);
      if (!state.running && state.worker.joinable()) {
        completed = std::move(state.worker);
      }
    }
    if (completed.joinable()) {
      completed.join();
    }
  }

  // Session prep writes $XDG_RUNTIME_DIR/polaris-gamescope-force (1 when enable_hdr /
  // gamescope --hdr-enabled). Gamescope may present PQ; capture still needs a
  // separate check that this stream will *encode* HDR.
  static bool portal_force_hdr_enabled() {
    const char *rt = std::getenv("XDG_RUNTIME_DIR");
    if (!rt || !*rt) {
      return false;
    }
    std::ifstream f(std::string(rt) + "/polaris-gamescope-force");
    std::string line;
    if (!f || !std::getline(f, line)) {
      return false;
    }
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n' || line.back() == ' ')) {
      line.pop_back();
    }
    return line == "1" || line == "true";
  }

  // Exclusive 10-bit PQ EnumFormat only when gamescope is in HDR mode *and* the
  // client stream will encode HDR (dynamicRange > 0). Host KDE ScreenCast is
  // 8-bit-only; never restrict EnumFormat to PQ outside gamescope_stream.
  static bool portal_prefer_hdr_formats(int client_dynamic_range) {
    if (client_dynamic_range <= 0 || !portal_force_hdr_enabled()) {
      return false;
    }
    const auto &mode = config::video.linux_display.stream_mode;
    return mode.empty() || mode == "gamescope_stream";
  }

  // Exclusive 8-bit EnumFormat when the stream encodes SDR. Mixed offers still
  // list 10-bit first; gamescope then delivers PQ into an SDR encoder (red wash).
  static bool portal_prefer_sdr_formats(int client_dynamic_range) {
    return client_dynamic_range <= 0;
  }

  // Encoder render node for DMA-BUF eligibility: the configured adapter_name
  // when it names a canonical render node, else the host's sole render node.
  // Single-GPU hosts get zero-copy without configuration; multi-GPU hosts stay
  // fail-closed so DMA-BUF never imports across GPUs by guess.
  static std::optional<std::string> encoder_render_node_for_dmabuf() {
    if (auto configured = pipewire_capture::canonical_render_node(config::video.adapter_name)) {
      return configured;
    }
    std::vector<std::string> nodes;
    std::error_code ec;
    for (const auto &entry : std::filesystem::directory_iterator("/dev/dri", ec)) {
      if (entry.path().filename().string().starts_with("renderD")) {
        nodes.push_back(entry.path().string());
      }
    }
    auto sole = pipewire_capture::pick_sole_render_node(nodes);
    if (sole) {
      static bool logged_auto_render_node = false;
      if (!logged_auto_render_node) {
        logged_auto_render_node = true;
        BOOST_LOG(info) << "portal: adapter_name is unset; using the host's sole render node ["sv
                        << *sole << "] for DMA-BUF eligibility"sv;
      }
    }
    return sole;
  }

  // Local-graph PW capture (gamescopegrab / kwingrab): remote_fd=-1, no portal session.
  static std::shared_ptr<pipewire_capture::capture_t> start_local_pw_capture(
    std::uint32_t node_id,
    std::uint64_t node_serial,
    int width,
    int height,
    platf::mem_type_e mem_type,
    int client_dynamic_range
  ) {
    const auto encoder_render_node = encoder_render_node_for_dmabuf();
    std::vector<pipewire_capture::dmabuf_format_modifier_t> dmabuf_formats;
    bool may_use_dmabuf = false;
    const bool prefer_hdr = portal_prefer_hdr_formats(client_dynamic_range);
    const bool prefer_sdr = portal_prefer_sdr_formats(client_dynamic_range);
    if (encoder_render_node) {
      // LINEAR always for gamescope (only allocates LINEAR on PW node).
      dmabuf_formats = pipewire_capture::task1_packed_dmabuf_formats({DRM_FORMAT_MOD_LINEAR});
      // Ensure 10-bit LINEAR is present for HDR streams even if EGL skipped it.
      if (!prefer_sdr) {
        const bool has_xb30 = std::any_of(dmabuf_formats.begin(), dmabuf_formats.end(), [](const auto &f) {
          return f.spa_format == SPA_VIDEO_FORMAT_xBGR_210LE && f.modifier == DRM_FORMAT_MOD_LINEAR;
        });
        if (!has_xb30) {
          dmabuf_formats.insert(dmabuf_formats.begin(), {
            .spa_format = SPA_VIDEO_FORMAT_xBGR_210LE,
            .drm_fourcc = DRM_FORMAT_XBGR2101010,
            .modifier = DRM_FORMAT_MOD_LINEAR,
          });
        }
      }
      if (prefer_hdr) {
        std::erase_if(dmabuf_formats, [](const auto &format) {
          return format.spa_format != SPA_VIDEO_FORMAT_xBGR_210LE &&
                 format.spa_format != SPA_VIDEO_FORMAT_xRGB_210LE;
        });
      }
      else if (prefer_sdr) {
        std::erase_if(dmabuf_formats, [](const auto &format) {
          return format.spa_format == SPA_VIDEO_FORMAT_xBGR_210LE ||
                 format.spa_format == SPA_VIDEO_FORMAT_xRGB_210LE;
        });
      }
      may_use_dmabuf = !dmabuf_formats.empty();
    }
    auto local = std::make_shared<pipewire_capture::capture_t>(pipewire_capture::capture_options_t {
      .remote_fd = -1,
      .node_id = node_id,
      .node_serial = node_serial,
      .requested_width = width,
      .requested_height = height,
      .capture_render_node = encoder_render_node,
      .dmabuf_formats = std::move(dmabuf_formats),
      .mem_type = mem_type,
      .may_use_dmabuf = may_use_dmabuf,
      .prefer_hdr_formats = prefer_hdr,
      .prefer_sdr_formats = prefer_sdr,
    });
    if (!local->start()) {
      return nullptr;
    }
    return local;
  }

  // SB-2: release PipeWire + portal Session *before* gamescope/labwc is killed.
  // Holding a live PW stream into a dying compositor SEGV's gamescope
  // (CVulkanDevice dtor) and occasionally polaris itself.
  void shutdown() {
    // Process-exit deinit (misc linux_deinit_t): tear down capture before other libs.
    release_global_capture();
  }

  // Idempotent sole public release entry (S4).
  void release_global_capture() {
    auto teardown = session_media::begin_teardown();
    std::shared_ptr<pipewire_capture::capture_t> capture;
    std::unique_ptr<portal_session_t> portal;
    std::shared_ptr<void> kwin;
    {
      std::lock_guard lock(g_media_mu);
      if (g_media.empty()) {
        return;
      }
      BOOST_LOG(info) << "portal: Releasing capture/session for stream teardown"sv;
      capture = std::move(g_media.capture);
      portal = std::move(g_media.portal);
      kwin = std::move(g_media.kwin);
      g_media.clear_meta();
    }
    // Stop the capture loop before dropping our ref so waiters wake and exit
    // instead of blocking join past force-shutdown. capture_t::stop() is the
    // public SB-2 path (running_/frame_cv_ are private).
    if (capture) {
      capture->stop();
    }
    // ~capture_t / pw_thread_loop_stop can block indefinitely with gamescope.
    // Keep the cleanup on an owned worker and preserve the short caller budget;
    // its teardown owner keeps reconnect blocked until all dtors finish.
    constexpr auto k_sync_budget = std::chrono::milliseconds(400);
    reap_portal_cleanup();
    auto &cleanup = portal_cleanup_state();
    {
      std::lock_guard lock(cleanup.mutex);
      cleanup.running = true;
      cleanup.worker = std::thread {[capture = std::move(capture),
                                    portal = std::move(portal),
                                    kwin = std::move(kwin),
                                    teardown = std::move(teardown),
                                    &cleanup]() mutable {
      BOOST_LOG(info) << "portal: async capture/session destroy begin"sv;
      if (capture) {
        capture->shutdown();
      }
      capture.reset();
      portal.reset();
      kwin.reset();
      BOOST_LOG(info) << "portal: async capture/session destroy done"sv;
      {
        std::lock_guard lock(cleanup.mutex);
        cleanup.running = false;
      }
      cleanup.changed.notify_all();
    }};
    }

    std::thread completed;
    {
      std::unique_lock lock(cleanup.mutex);
      if (cleanup.changed.wait_for(lock, k_sync_budget, [&cleanup] {
            return !cleanup.running;
          })) {
        completed = std::move(cleanup.worker);
      }
    }
    if (completed.joinable()) {
      completed.join();
    } else {
      BOOST_LOG(warning) << "portal: capture/session destroy exceeded "
                         << k_sync_budget.count()
                         << "ms; owned cleanup continues while reconnect remains gated"sv;
    }
  }

  // Caller must hold g_media_mu.
  static bool ensure_session_unlocked() {
    if (!g_media.portal || !g_media.portal->ready || g_media.portal->pw_node_id == 0) {
      g_media.portal.reset();
      auto try_create = []() {
        return create_portal_session(capture_type_for_stream_display(
          config::video.linux_display.headless_mode,
          config::video.linux_display.use_cage_compositor,
          config::video.linux_display.stream_mode));
      };
      g_media.portal = try_create();
      // Nested↔idle gamescope handoff: portal-gamescope may briefly fail Start with
      // "failed to connect to wayland socket" until the new generation owns
      // gamescope-0. Retry a few times rather than fail the whole stream.
      for (int attempt = 1;
           attempt <= 4 &&
           (!g_media.portal || g_media.portal->failed || !g_media.portal->ready ||
            g_media.portal->pw_node_id == 0);
           ++attempt) {
        if (session_media::teardown_in_progress() || session_media::pending_start_cancelled(session_media::pending_start_owner())) {
          BOOST_LOG(info) << "portal: stop requested; skipping failed-session retry backoff"sv;
          g_media.portal.reset();
          break;
        }
        BOOST_LOG(warning) << "portal: create session attempt "sv << attempt
                           << " failed; waiting for gamescope-0 before retry"sv;
        g_media.portal.reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(400 * attempt));
        g_media.portal = try_create();
      }
      if (!g_media.portal || g_media.portal->failed || !g_media.portal->ready ||
          g_media.portal->pw_node_id == 0) {
        BOOST_LOG(warning) << "portal: Failed to create global session"sv;
        g_media.portal.reset();
        return false;
      }
      BOOST_LOG(info) << "portal: Global session ready, node "sv << g_media.portal->pw_node_id
                      << " serial="sv << g_media.portal->pw_node_serial;
    }
    return true;
  }

  static bool ensure_global_session() {
    std::lock_guard transition_lock(g_capture_transition_mu);
    auto start = session_media::begin_start();
    reap_portal_cleanup();
    std::lock_guard lock(g_media_mu);
    return ensure_session_unlocked();
  }

  static std::shared_ptr<pipewire_capture::capture_t> ensure_global_capture(
    int width,
    int height,
    platf::mem_type_e mem_type,
    int client_dynamic_range
  ) {
    // Only one configuration transition may retire/publish a capture generation.
    std::lock_guard transition_lock(g_capture_transition_mu);
    auto start = session_media::begin_start();
    reap_portal_cleanup();
    // Lock contract (SB-2 + S4 single mutex):
    // 1) Under g_media_mu: ensure session + start PipeWire (no dual-mutex nesting).
    // 2) Wait for negotiation OUTSIDE the lock so release_global_capture can progress.
    const bool want_prefer_hdr = portal_prefer_hdr_formats(client_dynamic_range);
    const bool want_prefer_sdr = portal_prefer_sdr_formats(client_dynamic_range);
    std::shared_ptr<pipewire_capture::capture_t> capture;
    {
      std::unique_lock lock(g_media_mu);

      const auto requested_adapter = config::video.adapter_name;
      if (g_media.capture && g_media.capture->running()) {
        const auto compatible = g_media.requested_width == width &&
                                g_media.requested_height == height &&
                                g_media.mem_type == mem_type &&
                                g_media.adapter == requested_adapter &&
                                g_media.prefer_hdr == want_prefer_hdr &&
                                g_media.prefer_sdr == want_prefer_sdr;
        if (compatible) {
          return g_media.capture;
        }

        BOOST_LOG(info) << "portal: capture configuration changed; retiring PipeWire generation before reconnect"sv;
        auto retired_capture = std::move(g_media.capture);
        auto retired_kwin = std::move(g_media.kwin);
        g_media.clear_meta();
        lock.unlock();
        retired_capture->stop();
        retired_capture->shutdown();
        retired_capture.reset();
        retired_kwin.reset();
        lock.lock();

        // A PipeWire remote FD is a protocol connection, not a reusable device
        // handle. Request a fresh remote only after the old producer is fully
        // quiescent; the transition mutex prevents a concurrent replacement.
        if (g_media.portal && g_media.portal->conn && !g_media.portal->session_handle.empty()) {
          if (g_media.portal->pw_remote_fd >= 0) {
            close(g_media.portal->pw_remote_fd);
            g_media.portal->pw_remote_fd = -1;
          }
          g_media.portal->pw_remote_fd =
            open_pipewire_remote_fd(g_media.portal->conn, g_media.portal->session_handle);
        }
        if (!g_media.portal || g_media.portal->pw_remote_fd < 0) {
          g_media.portal.reset();
        }
      } else if (g_media.capture) {
        // A dead stream invalidates the node on this private remote. Explicitly
        // retire it outside g_media_mu before replacing portal/session state.
        auto retired_capture = std::move(g_media.capture);
        auto retired_portal = std::move(g_media.portal);
        auto retired_kwin = std::move(g_media.kwin);
        g_media.clear_meta();
        lock.unlock();
        retired_capture->shutdown();
        retired_capture.reset();
        retired_portal.reset();
        retired_kwin.reset();
        lock.lock();
      }

      // W3/W5 gamescopegrab: prefer session-graph Video/Source (media.name=gamescope)
      // without private portal ScreenCast when linux_stream_mode=gamescope_stream.
      // Falls through to portal if the node is missing (idle unit not exporting yet).
      const auto &stream_mode = config::video.linux_display.stream_mode;
      if ((!g_media.capture || !g_media.capture->running()) &&
          (stream_mode == "gamescope_stream" || stream_mode.empty()) &&
          config::video.linux_display.private_runtime == "gamescope") {
        if (auto gs = pipewire_capture::find_gamescope_video_source()) {
          if (auto local = start_local_pw_capture(
                gs->node_id, gs->object_serial, width, height, mem_type, client_dynamic_range)) {
            BOOST_LOG(info) << "portal: gamescopegrab local Video/Source node="sv << gs->node_id
                            << " name="sv << gs->node_name << " (no private ScreenCast)"sv;
            g_media.kwin.reset();
            g_media.capture = std::move(local);
            g_media.requested_width = width;
            g_media.requested_height = height;
            g_media.mem_type = mem_type;
            g_media.adapter = requested_adapter;
            g_media.prefer_hdr = want_prefer_hdr;
            g_media.prefer_sdr = want_prefer_sdr;
            capture = g_media.capture;
            // Skip portal session setup; negotiation wait continues below.
          }
          else {
            BOOST_LOG(info) << "portal: gamescopegrab start failed; falling back to portal ScreenCast"sv;
          }
        }
      }

#ifdef POLARIS_BUILD_WAYLAND
      // W4/P1 kwingrab: host KDE desktop_display / headless_dongle — try KWin
      // zkde screencast before portal picker. Fail cleanly when not on KWin.
      // capture=portal/auto/empty only; explicit wlr/kms unchanged.
      const bool portal_like_capture =
        config::video.capture.empty() ||
        config::video.capture == "auto" ||
        config::video.capture == "portal" ||
        config::video.capture == "kwin";
      if ((!g_media.capture || !g_media.capture->running()) &&
          portal_like_capture &&
          kwingrab::prefer_for_current_stream_mode()) {
        g_media.kwin.reset();
        if (auto kwin_session = kwingrab::start_output_session(
              config::video.output_name.empty() ? config::video.linux_display.streaming_output :
                                                  config::video.output_name
            )) {
          const auto &src = kwin_session->source();
          if (auto local = start_local_pw_capture(
                src.node_id,
                src.object_serial,
                width > 0 ? width : src.width,
                height > 0 ? height : src.height,
                mem_type,
                client_dynamic_range)) {
            BOOST_LOG(info) << "portal: kwingrab local PW node="sv << src.node_id
                            << " output="sv << src.output_name
                            << " (no xdg-desktop-portal picker)"sv;
            g_media.kwin = std::shared_ptr<kwingrab::session_t>(std::move(kwin_session));
            g_media.portal.reset();
            g_media.capture = std::move(local);
            g_media.requested_width = width;
            g_media.requested_height = height;
            g_media.mem_type = mem_type;
            g_media.adapter = requested_adapter;
            g_media.prefer_hdr = want_prefer_hdr;
            g_media.prefer_sdr = want_prefer_sdr;
            capture = g_media.capture;
          }
          else {
            BOOST_LOG(info) << "portal: kwingrab PipeWire start failed; falling back to portal ScreenCast"sv;
            kwin_session.reset();
          }
        }
        else {
          BOOST_LOG(info) << "portal: kwingrab unavailable; falling back to portal ScreenCast"sv;
        }
      }
#endif

      // gamescopegrab / kwingrab already filled capture — skip portal session.
      if (capture && capture->running()) {
        // fall through to negotiation wait outside lock
      }
      else if (!ensure_session_unlocked()) {
        return nullptr;
      }
      else {
        auto *session = g_media.portal.get();
        const auto encoder_render_node = encoder_render_node_for_dmabuf();
        // Headless gamescope often omits SPA capture.device / render_node. If the
        // operator set adapter_name to a render node (same GPU as NVENC), assume it.
        if (!session->capture_render_node) {
          if (auto assumed = pipewire_capture::resolve_capture_render_node(std::nullopt, encoder_render_node)) {
            BOOST_LOG(info) << "portal: stream omitted capture render node; assuming encoder adapter ["sv
                            << *assumed << "] for same-GPU DmaBuf eligibility"sv;
            session->capture_render_node = std::move(assumed);
          }
        }
        std::vector<pipewire_capture::dmabuf_format_modifier_t> dmabuf_formats;
        bool may_use_dmabuf = false;
        if (!session->capture_render_node) {
          BOOST_LOG(info) << "portal: DMA-BUF disabled because the portal stream did not provide an explicit capture render node"sv;
        } else if (!encoder_render_node) {
          BOOST_LOG(info) << "portal: DMA-BUF disabled because config::video.adapter_name is not an explicit canonical render node"sv;
        } else if (*session->capture_render_node != *encoder_render_node) {
          BOOST_LOG(info) << "portal: DMA-BUF disabled because capture render node ["sv << *session->capture_render_node
                          << "] does not match encoder adapter ["sv << *encoder_render_node << ']';
        } else if (mem_type != platf::mem_type_e::vaapi && mem_type != platf::mem_type_e::cuda) {
          BOOST_LOG(info) << "portal: DMA-BUF disabled because encoder memory type is not VAAPI or CUDA"sv;
        } else {
          if (mem_type == platf::mem_type_e::cuda) {
            // LINEAR one-plane packed RGB: 8-bit BGRx/BGRA + 10-bit xBGR_210LE (HDR).
            // Keep vulkan_cuda fast path; do not drop XB30 (regression vs prefer-10-bit).
            dmabuf_formats = pipewire_capture::task1_packed_dmabuf_formats({DRM_FORMAT_MOD_LINEAR});
            std::erase_if(dmabuf_formats, [](const auto &format) {
              return format.spa_format != SPA_VIDEO_FORMAT_BGRx &&
                     format.spa_format != SPA_VIDEO_FORMAT_BGRA &&
                     format.spa_format != SPA_VIDEO_FORMAT_xBGR_210LE &&
                     format.spa_format != SPA_VIDEO_FORMAT_xRGB_210LE;
            });
          } else {
            const auto egl_formats = pipewire_capture::query_egl_dmabuf_import_formats(*encoder_render_node);
            std::vector<std::uint64_t> importable_modifiers;
            for (const auto &format : egl_formats) {
              for (const auto modifier : format.modifiers) {
                if (std::find(format.external_only_modifiers.begin(), format.external_only_modifiers.end(), modifier) == format.external_only_modifiers.end()) {
                  importable_modifiers.push_back(modifier);
                }
              }
            }
            const auto portal_formats = pipewire_capture::task1_packed_dmabuf_formats(std::move(importable_modifiers));
            dmabuf_formats = pipewire_capture::filter_importable_dmabuf_formats(portal_formats, egl_formats);
          }
          // gamescope HDR offers xBGR_210LE LINEAR; EGL/list filters may omit it.
          // Ensure LINEAR 10-bit for HDR streams so force-HDR can negotiate spa 81.
          if (!want_prefer_sdr) {
            const bool has_xb30_linear = std::any_of(dmabuf_formats.begin(), dmabuf_formats.end(), [](const auto &f) {
              return f.spa_format == SPA_VIDEO_FORMAT_xBGR_210LE && f.modifier == DRM_FORMAT_MOD_LINEAR;
            });
            if (!has_xb30_linear) {
              dmabuf_formats.insert(dmabuf_formats.begin(), {
                .spa_format = SPA_VIDEO_FORMAT_xBGR_210LE,
                .drm_fourcc = DRM_FORMAT_XBGR2101010,
                .modifier = DRM_FORMAT_MOD_LINEAR,
              });
            }
          }
          if (want_prefer_hdr) {
            std::erase_if(dmabuf_formats, [](const auto &format) {
              return format.spa_format != SPA_VIDEO_FORMAT_xBGR_210LE &&
                     format.spa_format != SPA_VIDEO_FORMAT_xRGB_210LE;
            });
          }
          else if (want_prefer_sdr) {
            std::erase_if(dmabuf_formats, [](const auto &format) {
              return format.spa_format == SPA_VIDEO_FORMAT_xBGR_210LE ||
                     format.spa_format == SPA_VIDEO_FORMAT_xRGB_210LE;
            });
          }
          const pipewire_capture::dmabuf_eligibility_t eligibility {
            .capture_render_node = session->capture_render_node,
            .encoder_render_node = *encoder_render_node,
            .mem_type = mem_type,
            .egl_import_supported = !dmabuf_formats.empty(),
          };
          may_use_dmabuf = pipewire_capture::may_offer_dmabuf(eligibility);
          if (!may_use_dmabuf) {
            BOOST_LOG(info) << "portal: DMA-BUF disabled because no compatible packed-RGB modifier is available for encoder render node ["sv
                            << *encoder_render_node << ']';
          }
        }
        // Diag only: eligibility snapshot before capture start (Bedroom/SDR SHM debug).
        BOOST_LOG(info) << "portal: dmabuf_eligibility"
                        << " may_use="sv << (may_use_dmabuf ? "true"sv : "false"sv)
                        << " formats="sv << dmabuf_formats.size()
                        << " prefer_hdr="sv << (want_prefer_hdr ? "true"sv : "false"sv)
                        << " prefer_sdr="sv << (want_prefer_sdr ? "true"sv : "false"sv)
                        << " force_hdr="sv << (portal_force_hdr_enabled() ? "true"sv : "false"sv)
                        << " client_dynamic_range="sv << client_dynamic_range
                        << " capture_node="sv << session->capture_render_node.value_or("none")
                        << " encoder_node="sv << encoder_render_node.value_or("none")
                        << " mem_type="sv << static_cast<int>(mem_type);

        auto new_capture = std::make_shared<pipewire_capture::capture_t>(pipewire_capture::capture_options_t {
          .remote_fd = session->pw_remote_fd,
          .node_id = session->pw_node_id,
          .node_serial = session->pw_node_serial,
          .requested_width = width,
          .requested_height = height,
          .capture_render_node = session->capture_render_node,
          .dmabuf_formats = std::move(dmabuf_formats),
          .mem_type = mem_type,
          .may_use_dmabuf = may_use_dmabuf,
          .prefer_hdr_formats = want_prefer_hdr,
          .prefer_sdr_formats = want_prefer_sdr,
        });
        if (!new_capture->start()) {
          BOOST_LOG(warning) << "portal: Failed to start PipeWire capture; invalidating portal session"sv;
          new_capture.reset();
          g_media.portal.reset();
          return nullptr;
        }

        g_media.requested_width = width;
        g_media.requested_height = height;
        g_media.mem_type = mem_type;
        g_media.adapter = requested_adapter;
        g_media.prefer_hdr = want_prefer_hdr;
        g_media.prefer_sdr = want_prefer_sdr;
        g_media.capture = new_capture;
        capture = g_media.capture;
      }  // portal ScreenCast path
    }

    // The capture transport determines whether the encoder factory must use
    // RAM or GPU-resident input, so never select a factory before negotiation.
    // Wait outside g_media_mu so release_global_capture can take the lock.
    for (int i = 0; i < 100 && capture && capture->running() && !capture->negotiated(); ++i) {
      std::this_thread::sleep_for(100ms);
    }

    {
      std::lock_guard lock(g_media_mu);
      // If release_global_capture raced us, drop the orphan (caller must not use it).
      if (g_media.capture != capture) {
        return nullptr;
      }
      if (!capture || !capture->negotiated()) {
        BOOST_LOG(warning) << "portal: PipeWire format negotiation did not complete; invalidating portal session"sv;
        // Keep local `capture` so ~capture_t runs after unlock (no dtor under g_media_mu).
        g_media.reset_all();
        return nullptr;
      }
      return g_media.capture;
    }
  }

  // -----------------------------------------------------------------------
  // Display backend
  // -----------------------------------------------------------------------

  class portal_display_t: public platf::display_t {
  public:
    ~portal_display_t() override {
#ifdef POLARIS_BUILD_WAYLAND
      // Wake cage_screencopy::capture if it is the active path (no-op otherwise).
      cage_screencopy::request_stop();
#endif
    }

    int requested_width = 0;
    int requested_height = 0;
    int cfg_width = 0;
    int cfg_height = 0;
    // Client stream dynamicRange (0 = SDR encode, 1 = 10-bit / HDR candidate).
    int client_dynamic_range = 0;
    platf::mem_type_e mem_type = platf::mem_type_e::system;
    bool pipewire_dmabuf_negotiated = false;
    // SPA_VIDEO_FORMAT_* from PipeWire negotiate; 0 = unknown / not yet negotiated.
    std::uint32_t capture_spa_format = 0;

    bool probe_only = false;  // true during encoder probe (skip portal session)

    int
    init(platf::mem_type_e hwdevice_type, const std::string &display_name, const ::video::config_t &config) {
      requested_width = config.width;
      requested_height = config.height;
      cfg_width = requested_width;
      cfg_height = requested_height;
      client_dynamic_range = config.dynamicRange;
      mem_type = hwdevice_type;

      if (!probe_only) {
        BOOST_LOG(info) << "portal: Initializing capture "sv
                        << cfg_width << "x"sv << cfg_height
                        << " client_dynamic_range="sv << client_dynamic_range;

        // If cage/labwc compositor is configured (windowed or headless), skip portal entirely.
        // Direct wlr-screencopy will be used in capture() instead.
        // Check config, not runtime state — cage may not be running yet at init time.
        bool cage_configured = false;
#ifdef POLARIS_BUILD_WAYLAND
        cage_configured = config::video.linux_display.use_cage_compositor;
#endif
        if (!cage_configured) {
          if (!ensure_global_session()) {
            return -1;
          }


          auto cap = ensure_global_capture(requested_width, requested_height, mem_type, client_dynamic_range);
          if (!cap) {
            return -1;
          }


          const auto info = cap->frame_info();
          pipewire_dmabuf_negotiated = cap->negotiated_dmabuf();
          capture_spa_format = info.spa_format;
          if (cap->negotiated() && info.width > 0 && info.height > 0) {
            cfg_width = info.width;
            cfg_height = info.height;
          }
        } else {
          // Direct compositor capture bypasses the gated portal helpers. Take
          // admission here so reconnect cannot overlap a prior async teardown.
          auto start = session_media::begin_start();
          (void) start;
          reap_portal_cleanup();
          BOOST_LOG(info) << "portal: Cage/labwc active — skipping portal, will use direct screencopy"sv;
        }
      } else {
        BOOST_LOG(info) << "portal: Probe mode — using dummy "sv
                        << cfg_width << "x"sv << cfg_height;
      }

      this->env_width = cfg_width;
      this->env_height = cfg_height;
      this->width = cfg_width;
      this->height = cfg_height;

      BOOST_LOG(info) << "portal: Capture ready — "sv << cfg_width << "x"sv << cfg_height
                      << " env="sv << this->env_width << "x"sv << this->env_height;
      return 0;
    }

    // True only when client wants HDR *and* capture is (or may still become) 10-bit.
    // Negotiated BGRx/8-bit + HDR PQ signaling is the washed-red path.
    bool capture_is_10bit_hdr() const {
      return capture_spa_format == SPA_VIDEO_FORMAT_xBGR_210LE ||
             capture_spa_format == SPA_VIDEO_FORMAT_xRGB_210LE;
    }

    bool is_hdr() override {
      // Stream must request HDR encode; force/gamescope alone is not enough.
      if (client_dynamic_range <= 0) {
        return false;
      }
      // Force file + prefer_hdr EnumFormat. Host KDE portal never negotiates
      // 10-bit so is_hdr stays false after Format lands on BGRx.
      if (!portal_force_hdr_enabled()) {
        return false;
      }
      // Before negotiate, allow HDR so probe can select 10-bit codecs.
      if (capture_spa_format == 0) {
        return true;
      }
      if (!capture_is_10bit_hdr()) {
        BOOST_LOG(warning) << "portal: is_hdr=false — negotiated spa_format="sv << capture_spa_format
                           << " is not xBGR/xRGB_210LE (8-bit BGRx + PQ would wash colors)"sv;
        return false;
      }
      return true;
    }

    bool get_hdr_metadata(SS_HDR_METADATA &metadata) override {
      if (!is_hdr()) {
        return false;
      }
      std::memset(&metadata, 0, sizeof(metadata));
      // SS_HDR_METADATA: CIE xy * 50_000, RGB order (Limelight.h).
      // Rec. ITU-R BT.2020-2 primaries + D65 white.
      metadata.displayPrimaries[0] = {35400, 14600};  // R 0.708, 0.292
      metadata.displayPrimaries[1] = {8500, 39850};  // G 0.170, 0.797
      metadata.displayPrimaries[2] = {6550, 2300};  // B 0.131, 0.046
      metadata.whitePoint = {15635, 16450};  // D65 0.3127, 0.3290
      metadata.maxDisplayLuminance = 1000;
      metadata.minDisplayLuminance = 1;
      metadata.maxContentLightLevel = 1000;
      metadata.maxFrameAverageLightLevel = 400;
      return true;
    }

    platf::capture_e
    capture(const push_captured_image_cb_t &push_captured_image_cb,
      const pull_free_image_cb_t &pull_free_image_cb,
      bool *cursor) override {

      BOOST_LOG(info) << "portal: capture() called"sv;

      // If cage/labwc is configured, use direct wlr-screencopy (no portal, no picker)
#ifdef POLARIS_BUILD_WAYLAND
      if (config::video.linux_display.use_cage_compositor) {
        // Wait for labwc to be running (it may still be starting up)
        for (int wait = 0; wait < 50 && !stream_runtime::labwc::is_running(); ++wait) {
          std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
        if (stream_runtime::labwc::is_running()) {
          auto cage_socket = stream_runtime::labwc::wayland_socket();
          if (!cage_socket.empty()) {
            BOOST_LOG(info) << "portal: Using direct screencopy from cage ("sv << cage_socket << ")"sv;
            auto result = cage_screencopy::capture(
              cage_socket, cfg_width, cfg_height,
              push_captured_image_cb, pull_free_image_cb, cursor,
              cfg_width, cfg_height);
            if (cfg_width > 0 && cfg_height > 0) {
              this->env_width = cfg_width;
              this->env_height = cfg_height;
              this->width = cfg_width;
              this->height = cfg_height;
            }
            return result;
          }
        }
        BOOST_LOG(warning) << "portal: Cage configured but not running after 5s — cannot capture"sv;
        return platf::capture_e::reinit;
      }
#endif

      // Fallback: portal D-Bus capture (only when cage is NOT configured)
      if (!ensure_global_session()) {
        BOOST_LOG(warning) << "portal: No capture session available"sv;
        return platf::capture_e::reinit;
      }


      auto cap = ensure_global_capture(requested_width, requested_height, mem_type, client_dynamic_range);
      if (!cap) {
        BOOST_LOG(warning) << "portal: No capture available"sv;
        return platf::capture_e::reinit;
      }


      auto frame_info = cap->frame_info();
      pipewire_dmabuf_negotiated = cap->negotiated_dmabuf();
      capture_spa_format = frame_info.spa_format;
      if (cap->negotiated() && frame_info.width > 0 && frame_info.height > 0) {
        cfg_width = frame_info.width;
        cfg_height = frame_info.height;
        this->env_width = cfg_width;
        this->env_height = cfg_height;
        this->width = cfg_width;
        this->height = cfg_height;
      }


      bool capture_transport_logged = false;
      while (cap) {
        const auto capture_start = std::chrono::steady_clock::now();
        const auto wait_result = cap->wait_for_frame(1s);
        const auto dispatch_time = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - capture_start);

        switch (wait_result) {
          case pipewire_capture::wait_result_e::timeout: {
            std::shared_ptr<platf::img_t> dummy;
            if (!push_captured_image_cb(std::move(dummy), false)) {
              return platf::capture_e::ok;
            }
            continue;
          }
          case pipewire_capture::wait_result_e::reinit:
            return platf::capture_e::reinit;
          case pipewire_capture::wait_result_e::error:
            return platf::capture_e::error;
          case pipewire_capture::wait_result_e::frame:
            break;
        }

        std::shared_ptr<platf::img_t> img_out;
        if (!pull_free_image_cb(img_out)) {
          return platf::capture_e::interrupted;
        }

        if (!cap->fill_frame(img_out)) {
          return platf::capture_e::reinit;
        }

        // P0-3 T0: the frame is genuinely available as of fill_frame()
        // succeeding, same moment update_capture_metadata below already
        // treats as "frame is ready." This backend was the other of the two
        // display_t implementers missing frame_timestamp.
        img_out->frame_timestamp = std::chrono::steady_clock::now();

        stream_stats::update_capture_metadata(img_out->frame_metadata);
        if (!capture_transport_logged) {
          capture_transport_logged = true;
          if (pipewire_capture::frame_requires_cpu_copy(img_out->frame_metadata)) {
            BOOST_LOG(warning) << "portal: capture_transport="sv << platf::from_frame_transport(img_out->frame_metadata.transport)
                               << " frame_residency="sv << platf::from_frame_residency(img_out->frame_metadata.residency)
                               << " frame_format="sv << platf::from_frame_format(img_out->frame_metadata.format)
                               << "; capture will incur an extra CPU-side copy/conversion path"sv;
          } else {
            BOOST_LOG(info) << "portal: capture_transport="sv << platf::from_frame_transport(img_out->frame_metadata.transport)
                            << " frame_residency="sv << platf::from_frame_residency(img_out->frame_metadata.residency)
                            << " frame_format="sv << platf::from_frame_format(img_out->frame_metadata.format);
          }
        }
        if (config::video.linux_display.capture_profile) {
          const auto total_time = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now() - capture_start);
          stream_stats::update_capture_profile({
            .transport = img_out->frame_metadata.transport,
            .dispatch_time = dispatch_time,
            .ingest_time = total_time - dispatch_time,
            .total_time = total_time,
          });
        }

        if (!push_captured_image_cb(std::move(img_out), true)) {
          return platf::capture_e::ok;
        }
      }

      return platf::capture_e::error;
    }

    std::shared_ptr<platf::img_t>
    alloc_img() override {
      struct portal_img_t: egl::img_descriptor_t {
      };

      auto img = std::make_shared<portal_img_t>();

      img->width = cfg_width;
      img->height = cfg_height;
      img->pixel_pitch = 4;
      img->row_pitch = cfg_width * 4;
      img->sequence = 0;
      img->serial = std::numeric_limits<decltype(img->serial)>::max();
      img->dmabuf_buffer_key = 0;
      img->sd = {};
      std::fill_n(img->sd.fds, 4, -1);
      img->buffer.assign(static_cast<std::size_t>(cfg_height) * static_cast<std::size_t>(img->row_pitch), 0);
      img->data = pipewire_dmabuf_negotiated ? nullptr : img->buffer.data();

      BOOST_LOG(info) << "portal: alloc_img "sv << img->width << "x"sv << img->height
                      << " env="sv << this->env_width << "x"sv << this->env_height;
      return img;
    }

    int
    dummy_img(platf::img_t *img) override {
      if (!img) return -1;
      if (pipewire_dmabuf_negotiated) {
        auto *descriptor = dynamic_cast<egl::img_descriptor_t *>(img);
        if (!descriptor) return -1;
        descriptor->sequence = 0;
        return 0;
      }
      if (!img->data) {
        if (auto *cursor = dynamic_cast<egl::cursor_t *>(img); cursor && !cursor->buffer.empty()) {
          img->data = cursor->buffer.data();
        }
      }
      if (!img->data) return -1;
      std::memset(img->data, 0, img->height * img->row_pitch);
      return 0;
    }

    std::unique_ptr<platf::avcodec_encode_device_t>
    make_avcodec_encode_device(platf::pix_fmt_e pix_fmt) override {
      int w = cfg_width;
      int h = cfg_height;

#ifdef POLARIS_BUILD_VAAPI
      if (mem_type == platf::mem_type_e::vaapi) {
        if (pipewire_dmabuf_negotiated) {
          return va::make_avcodec_encode_device(w, h, 0, 0, true);
        }
        return va::make_avcodec_encode_device(w, h, false);
      }
#endif
#ifdef POLARIS_BUILD_CUDA
      if (mem_type == platf::mem_type_e::cuda) {
        if (pipewire_dmabuf_negotiated) {
          auto device = cuda::make_avcodec_dmabuf_encode_device(w, h);
          // DMA-BUF BGRx is still 8-bit; only xBGR_210LE can feed real P010 HDR.
          // Without prefer_8bit, make_encode_device keeps Rec.2020+PQ on 8-bit pixels
          // → washed reds / crushed shadows.
          if (device && !capture_is_10bit_hdr()) {
            device->prefer_8bit_encode = true;
            BOOST_LOG(info) << "portal: dmabuf capture is 8-bit (spa="sv << capture_spa_format
                            << "); prefer_8bit_encode to avoid PQ-on-BGRx washout"sv;
          }
          return device;
        }
        // Portal MemFd/SHM: RAM→CUDA NV12. prefer_8bit avoids software P010 for 10-bit clients.
        auto device = cuda::make_avcodec_encode_device(w, h, false);
        if (device) {
          device->prefer_8bit_encode = true;
        }
        return device;
      }
#endif
      return std::make_unique<platf::avcodec_encode_device_t>();
    }
  };

}  // namespace portal

namespace platf {

  std::shared_ptr<display_t>
  portal_display(mem_type_e hwdevice_type, const std::string &display_name, const video::config_t &config) {
    auto disp = std::make_shared<portal::portal_display_t>();
    // Encoder validation must remain picker-free and use the RAM factory.
    // Real capture construction negotiates before image-pool/factory selection.
    disp->probe_only = video::encoder_probe_active();
    if (disp->init(hwdevice_type, display_name, config)) {
      return nullptr;
    }
    return disp;
  }

  std::vector<std::string>
  portal_display_names() {
    std::vector<std::string> names;

    if (!portal::is_portal_available()) {
      BOOST_LOG(debug) << "Portal: ScreenCast interface not available"sv;
      return names;
    }

    BOOST_LOG(info) << "Portal: XDG Desktop Portal ScreenCast interface detected"sv;
    names.emplace_back("0");
    return names;
  }

}  // namespace platf
