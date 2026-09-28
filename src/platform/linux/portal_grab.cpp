/**
 * @file src/platform/linux/portal_grab.cpp
 * @brief XDG Desktop Portal ScreenCast capture backend.
 *
 * Captures a window (or monitor) via the xdg-desktop-portal ScreenCast D-Bus
 * API, receiving frames through PipeWire. This is the primary capture backend
 * for the Polaris cage-as-window architecture.
 */

#include <gio/gio.h>

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

#include "src/capture_generation.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/platform/linux/game_mode_host.h"
#include "src/platform/linux/game_mode_repaint.h"
#include "src/platform/linux/graphics.h"
#include "src/platform/linux/misc.h"
#include "src/video.h"
#include "src/stream_stats.h"

#include "src/platform/linux/stream_runtime.h"
#ifdef POLARIS_BUILD_WAYLAND
  #include "src/platform/linux/cage_screencopy.h"
  #include "src/platform/linux/kwingrab.h"
#endif
#include "src/platform/linux/pipewire_capture.h"
#ifdef POLARIS_BUILD_PYROWAVE
  #include "src/platform/linux/pyrowave_encode.h"
#endif
#include "src/platform/linux/portal_session.h"
#include "src/platform/linux/session_media.h"

#ifdef POLARIS_BUILD_CUDA
  #include "src/platform/linux/cuda.h"
#endif
#ifdef POLARIS_BUILD_VAAPI
  #include "src/platform/linux/vaapi.h"
#endif
#ifdef POLARIS_BUILD_VULKAN
  #include "src/platform/linux/vulkan_encode.h"
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

  bool capture_generation_matches(
    const capture_generation::identity_t &cached,
    const capture_generation::identity_t &requested
  ) {
    return cached == requested;
  }

  bool portal_capture_backend_allowed(std::string_view capture_backend) {
    return capture_backend.empty() ||
           capture_backend == "auto" ||
           capture_backend == "portal" ||
           capture_backend == "kwin";
  }

#ifdef POLARIS_TESTS
  bool portal_capture_backend_allowed_for_tests(std::string_view capture_backend) {
    return portal_capture_backend_allowed(capture_backend);
  }

  bool portal_capture_generation_matches_for_tests(
    const capture_generation::identity_t &cached,
    const capture_generation::identity_t &requested
  ) {
    return capture_generation_matches(cached, requested);
  }
#endif

  struct media_cache_t {
    std::unique_ptr<portal_session_t> portal;
    // Opaque lifetime guard for a Wayland-only kwingrab session. Keeping the
    // cache type independent lets portal/PipeWire builds omit Wayland helpers.
    std::shared_ptr<void> kwin;
    std::shared_ptr<pipewire_capture::capture_t> capture;
    // Non-null only while a specific HTTP launch owns unclaimed preparation.
    std::shared_ptr<const void> prepared_token;
    int requested_width = 0;
    int requested_height = 0;
    AVRational requested_rate {0, 1};
    platf::mem_type_e mem_type = platf::mem_type_e::system;
    capture_generation::identity_t generation;
    // Last EnumFormat preference: prefer_hdr (force ∧ dynamicRange>0) or
    // prefer_sdr (dynamicRange<=0). Reuse only when both match.
    bool prefer_hdr = false;
    bool prefer_sdr = false;
    // The route the capture took, so a display that reuses it reports the route it is on.
    platf::capture_route_t route;

    void clear_meta() {
      route = {};
      prepared_token.reset();
      requested_width = 0;
      requested_height = 0;
      requested_rate = {0, 1};
      mem_type = platf::mem_type_e::system;
      generation = {};
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
  static bool portal_prefer_hdr_formats(
    int client_dynamic_range,
    std::string_view stream_mode
  ) {
    if (client_dynamic_range <= 0 || !portal_force_hdr_enabled()) {
      return false;
    }
    return stream_mode.empty() || stream_mode == "gamescope_stream";
  }

  // Exclusive 8-bit EnumFormat when the stream encodes SDR. Mixed offers still
  // list 10-bit first; gamescope then delivers PQ into an SDR encoder (red wash).
  static bool portal_prefer_sdr_formats(int client_dynamic_range) {
    return client_dynamic_range <= 0;
  }

  static pipewire_capture::dmabuf_override_e portal_dmabuf_override() {
    return pipewire_capture::dmabuf_override_from_env(std::getenv("POLARIS_PORTAL_DMABUF"));
  }

  static bool encoder_dmabuf_import_compiled(platf::mem_type_e mem_type) {
    switch (mem_type) {
      case platf::mem_type_e::cuda:
#ifdef POLARIS_BUILD_CUDA
        return true;
#else
        return false;
#endif
      case platf::mem_type_e::vaapi:
#ifdef POLARIS_BUILD_VAAPI
        return true;
#else
        return false;
#endif
      case platf::mem_type_e::vulkan_pyrowave:
#ifdef POLARIS_BUILD_PYROWAVE
        // Asked of the device rather than of the build, because the extensions this needs are a
        // driver's to offer and a host whose GPU lacks them copies its frames instead.
        return pyrowave_encode::dmabuf_import_available();
#else
        return false;
#endif
      default:
        return false;
    }
  }

  struct dmabuf_policy_note_t {
    bool warning = false;
    std::string text;
  };

  // What the portal says about DMA-BUF for this encoder before it looks at render nodes.
  static std::optional<dmabuf_policy_note_t> dmabuf_policy_note(
    platf::mem_type_e mem_type,
    pipewire_capture::dmabuf_override_e override,
    std::string_view source
  ) {
    const std::string at = "source=" + std::string {source};
    if (override == pipewire_capture::dmabuf_override_e::force_cpu) {
      return dmabuf_policy_note_t {false, "portal: portal_dmabuf_forced_off " + at + "; offering SHM only"};
    }
    if (mem_type == platf::mem_type_e::vaapi && override == pipewire_capture::dmabuf_override_e::allow_vaapi) {
      return dmabuf_policy_note_t {
        true,
        "portal: vaapi_pipewire_dmabuf_explicitly_enabled " + at +
          "; operator opted into an unvalidated path with no automatic stall fallback"
      };
    }
    if (mem_type == platf::mem_type_e::vaapi) {
      return dmabuf_policy_note_t {
        false,
        "portal: vaapi_pipewire_dmabuf_disabled_for_stability " + at +
          "; offering SHM by default; set POLARIS_PORTAL_DMABUF=1 only on a host where this path is known to work"
      };
    }
    // Vulkan Video on the portal always gets the RAM uploader (see make_avcodec_encode_device
    // below), which reads a frame's CPU copy and has none to read from a DMA-BUF, so the offer
    // stays closed on purpose until the portal can retire a failed DMA-BUF frame to that uploader.
    // Say it is policy, and say the environment variable is VA-API's (#635).
    constexpr std::string_view vulkan_until = ", until the portal can fall back when a DMA-BUF frame fails to import";
    if (mem_type == platf::mem_type_e::vulkan && override == pipewire_capture::dmabuf_override_e::allow_vaapi) {
      return dmabuf_policy_note_t {
        true,
        "portal: vulkan_pipewire_dmabuf_opt_in_is_vaapi_only " + at +
          "; POLARIS_PORTAL_DMABUF=1 opts VA-API into DMA-BUF only, so Vulkan Video on the portal "
          "stays on shared memory by policy" + std::string {vulkan_until}
      };
    }
    if (mem_type == platf::mem_type_e::vulkan) {
      return dmabuf_policy_note_t {
        false,
        "portal: vulkan_pipewire_dmabuf_disabled_by_policy " + at +
          "; Vulkan Video on the portal stays on shared memory by policy, not for a missing build "
          "feature" + std::string {vulkan_until}
      };
    }
    return std::nullopt;
  }

  static void log_dmabuf_policy(
    platf::mem_type_e mem_type,
    pipewire_capture::dmabuf_override_e override,
    std::string_view source
  ) {
    const auto note = dmabuf_policy_note(mem_type, override, source);
    if (!note) {
      return;
    }
    if (note->warning) {
      BOOST_LOG(warning) << note->text;
    } else {
      BOOST_LOG(info) << note->text;
    }
  }

  // The line for an encoder the portal has no DMA-BUF import path for. Vulkan Video has none on
  // purpose and dmabuf_policy_note has already said so, so blaming the build would be wrong.
  static std::string_view missing_import_path_line(platf::mem_type_e mem_type) {
    if (mem_type == platf::mem_type_e::vulkan) {
      return {};
    }
    return "portal: DMA-BUF disabled because this build lacks the encoder-specific import path"sv;
  }

#ifdef POLARIS_TESTS
  std::string dmabuf_policy_log_for_tests(
    platf::mem_type_e mem_type,
    pipewire_capture::dmabuf_override_e override,
    std::string_view source
  ) {
    const auto note = dmabuf_policy_note(mem_type, override, source);
    if (!note) {
      return {};
    }
    return (note->warning ? "warning " : "info ") + note->text;
  }

  std::string missing_import_path_log_for_tests(platf::mem_type_e mem_type) {
    return std::string {missing_import_path_line(mem_type)};
  }
#endif

  // Encoder render node for DMA-BUF eligibility: the configured adapter_name
  // when it names a canonical render node, else the host's sole render node.
  // Single-GPU hosts get zero-copy without configuration; multi-GPU hosts stay
  // fail-closed so DMA-BUF never imports across GPUs by guess.
  static std::optional<std::string> encoder_render_node_for_dmabuf(std::string_view adapter_name) {
    if (auto configured = pipewire_capture::canonical_render_node(adapter_name)) {
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
  static bool game_mode_screen_generation(const capture_generation::identity_t &generation) {
    return platf::game_mode_host::streams_session_screen(
      generation.stream_mode,
      generation.use_cage_compositor,
      !generation.private_wayland_socket.empty(),
      platf::game_mode_host::session_live()
    );
  }

  static std::shared_ptr<pipewire_capture::capture_t> start_local_pw_capture(
    std::uint32_t node_id,
    std::uint64_t node_serial,
    int width,
    int height,
    platf::mem_type_e mem_type,
    int client_dynamic_range,
    const capture_generation::identity_t &generation,
    AVRational requested_rate
  ) {
    const auto encoder_render_node = encoder_render_node_for_dmabuf(generation.adapter_name);
    std::vector<pipewire_capture::dmabuf_format_modifier_t> dmabuf_formats;
    bool may_use_dmabuf = false;
    const bool encoder_import_supported = encoder_dmabuf_import_compiled(mem_type);
    const bool prefer_hdr = portal_prefer_hdr_formats(client_dynamic_range, generation.stream_mode);
    const bool prefer_sdr = portal_prefer_sdr_formats(client_dynamic_range);
    const auto dmabuf_override = portal_dmabuf_override();
    if (encoder_render_node) {
      const pipewire_capture::dmabuf_eligibility_t eligibility {
        .capture_render_node = encoder_render_node,
        .encoder_render_node = *encoder_render_node,
        .mem_type = mem_type,
        .encoder_import_supported = encoder_import_supported,
        .egl_import_supported = true,
      };
      may_use_dmabuf = pipewire_capture::may_offer_dmabuf(eligibility, dmabuf_override);
    }
    log_dmabuf_policy(mem_type, dmabuf_override, "local_graph"sv);
    if (encoder_render_node && !encoder_import_supported) {
      if (const auto line = missing_import_path_line(mem_type); !line.empty()) {
        BOOST_LOG(info) << line;
      }
    }
    if (may_use_dmabuf) {
#ifdef POLARIS_BUILD_PYROWAVE
      if (mem_type == platf::mem_type_e::vulkan_pyrowave) {
        // The layouts this encoder's device says it can import, asked of Vulkan rather than assumed.
        //
        // Linear alone, which is what the other arm offers and what this one used to borrow, is
        // refused by KWin for a virtual output on at least one driver here: it does not allocate
        // linear, so capture fell back to shared memory with the fast path sitting there unused.
        // Linear is still in this list when the driver can import it, so a producer that only makes
        // linear buffers, which is the gamescope case the line below is about, still matches.
        //
        // Asking is what makes offering more than linear safe. A frame that arrives as a dmabuf has
        // no host copy behind it, so a layout that turns out not to import is a lost frame rather
        // than a slow one, and every modifier here was checked against the exact image the import
        // creates.
        for (const auto spa_format : {SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA,
                                      SPA_VIDEO_FORMAT_xBGR_210LE, SPA_VIDEO_FORMAT_xRGB_210LE}) {
          const auto drm_format = pipewire_capture::drm_format_for_spa(spa_format);
          if (!drm_format) {
            continue;
          }
          for (const auto modifier : pyrowave_encode::importable_dmabuf_modifiers(*drm_format)) {
            dmabuf_formats.push_back({
              .spa_format = static_cast<std::uint32_t>(spa_format),
              .drm_fourcc = *drm_format,
              .modifier = modifier,
            });
          }
        }
        BOOST_LOG(info) << "portal: offering "sv << dmabuf_formats.size()
                        << " dmabuf layouts the compute codec can import"sv;
      } else
#endif
      {
        // LINEAR always for gamescope (only allocates LINEAR on PW node).
        dmabuf_formats = pipewire_capture::task1_packed_dmabuf_formats({DRM_FORMAT_MOD_LINEAR});
      }
      // Ensure 10-bit LINEAR is present for HDR streams even if EGL skipped it.
      if (!prefer_sdr) {
        pipewire_capture::offer_hdr_linear_ten_bit(
          dmabuf_formats,
          mem_type == platf::mem_type_e::vulkan_pyrowave
        );
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
      .requested_rate = requested_rate,
      .request_fixed_rate = generation.private_runtime != "gamescope" && running_kwin_uses_fixed_rate(),
    });
    if (session_media::teardown_in_progress() || session_media::pending_start_cancelled(session_media::pending_start_owner())) return nullptr;
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
  static bool ensure_session_unlocked(const capture_generation::identity_t &generation) {
    if (!g_media.portal || !g_media.portal->ready || g_media.portal->pw_node_id == 0) {
      g_media.portal.reset();
      auto try_create = [&generation]() {
        return create_portal_session(capture_type_for_stream_display(
          generation.headless_mode,
          generation.use_cage_compositor,
          generation.stream_mode));
      };
      g_media.portal = try_create();
      // Nested↔idle gamescope handoff: portal-gamescope may briefly fail Start with
      // "failed to connect to wayland socket" until the new generation owns
      // gamescope-0. Retry a few times rather than fail the whole stream.
      for (int attempt = 1;
           generation.stream_mode == "gamescope_stream" && attempt <= 4 &&
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

  static bool capture_start_cancelled() {
    return session_media::teardown_in_progress() ||
           session_media::pending_start_cancelled(session_media::pending_start_owner());
  }

  static bool wait_for_capture_negotiation(const std::shared_ptr<pipewire_capture::capture_t> &capture) {
    if (!capture) return false;
    for (int i = 0; i < 100; ++i) {
      if (capture_start_cancelled()) {
        capture->stop();
        return false;
      }
      if (capture->negotiated()) return true;
      if (!capture->running() && !capture->retry_rate_negotiation(capture_start_cancelled)) break;
      std::this_thread::sleep_for(100ms);
    }
    if (capture_start_cancelled()) {
      capture->stop();
      return false;
    }
    return capture->negotiated();
  }

#ifdef POLARIS_TESTS
  bool wait_for_capture_negotiation_for_tests(const std::shared_ptr<pipewire_capture::capture_t> &capture) {
    return wait_for_capture_negotiation(capture);
  }
#endif

  /// How a local PipeWire node the portal backend asked for fared.
  enum class local_node_e {
    not_asked,
    missing,  ///< the compositor offered nothing to attach to
    failed,  ///< it did, and capture on it did not start
    started,
  };

  /**
   * The route a portal capture took, from how each local node it asked for fared. The gamescope
   * node is asked for first and a KWin output second, and a ScreenCast is what is left. The
   * fallback names the first node asked for and not taken, because that is the route this capture
   * wanted; a node that was never asked for is no fallback.
   */
  static platf::capture_route_t portal_capture_route(local_node_e gamescope, local_node_e kwin) {
    platf::capture_route_t route;
    route.opened = "portal";
    if (gamescope == local_node_e::started) {
      route.route = platf::k_capture_route_portal_gamescope_node;
      return route;
    }
    route.route = kwin == local_node_e::started ? platf::k_capture_route_portal_kwin_node :
                                                 platf::k_capture_route_portal_screencast;
    if (gamescope == local_node_e::missing) {
      route.fallback_reason = platf::k_capture_route_fallback_gamescope_node_missing;
    }
    else if (gamescope == local_node_e::failed) {
      route.fallback_reason = platf::k_capture_route_fallback_gamescope_node_failed;
    }
    else if (kwin == local_node_e::missing) {
      route.fallback_reason = platf::k_capture_route_fallback_kwin_node_unavailable;
    }
    else if (kwin == local_node_e::failed) {
      route.fallback_reason = platf::k_capture_route_fallback_kwin_node_failed;
    }
    return route;
  }

#ifdef POLARIS_BUILD_WAYLAND
  /**
   * How the KWin output fared when KWin opened no output session for it. A host with no Wayland
   * display, or whose compositor is not KWin, has no KWin output to ask for, so its ScreenCast is
   * the route it always had and no fallback. A KWin that withheld its screencast protocol, or
   * whose output stream did not start, is one.
   */
  static local_node_e kwin_node_without_session(kwingrab::start_failure_e failure) {
    switch (failure) {
      case kwingrab::start_failure_e::no_wayland:
      case kwingrab::start_failure_e::not_kwin:
        return local_node_e::not_asked;
      case kwingrab::start_failure_e::protocol_withheld:
      case kwingrab::start_failure_e::stream_failed:
        return local_node_e::missing;
    }
    return local_node_e::missing;
  }
#endif

#ifdef POLARIS_TESTS
  platf::capture_route_t portal_capture_route_for_tests(std::string_view gamescope, std::string_view kwin) {
    const auto outcome = [](std::string_view name) {
      if (name == "missing") return local_node_e::missing;
      if (name == "failed") return local_node_e::failed;
      if (name == "started") return local_node_e::started;
      return local_node_e::not_asked;
    };
    return portal_capture_route(outcome(gamescope), outcome(kwin));
  }

  #ifdef POLARIS_BUILD_WAYLAND
  platf::capture_route_t portal_route_without_kwin_session_for_tests(std::string_view gamescope, std::string_view failure) {
    const auto gamescope_node = gamescope == "missing" ? local_node_e::missing :
                                gamescope == "failed"  ? local_node_e::failed :
                                                         local_node_e::not_asked;
    auto why = kwingrab::start_failure_e::stream_failed;
    if (failure == "no_wayland") why = kwingrab::start_failure_e::no_wayland;
    if (failure == "not_kwin") why = kwingrab::start_failure_e::not_kwin;
    if (failure == "protocol_withheld") why = kwingrab::start_failure_e::protocol_withheld;
    return portal_capture_route(gamescope_node, kwin_node_without_session(why));
  }
  #endif
#endif

  static std::shared_ptr<pipewire_capture::capture_t> ensure_global_capture(
    int width,
    int height,
    platf::mem_type_e mem_type,
    int client_dynamic_range,
    const capture_generation::identity_t &generation,
    AVRational requested_rate,
    const std::shared_ptr<const void> &prepared_token = {},
    platf::capture_route_t *route_out = nullptr
  ) {
    if (!portal_capture_backend_allowed(generation.capture_backend)) {
      BOOST_LOG(error) << "portal: capture generation backend ["sv << generation.capture_backend
                       << "] is not portal-authoritative; refusing acquisition"sv;
      return nullptr;
    }
    // Only one configuration transition may retire/publish a capture generation.
    std::lock_guard transition_lock(g_capture_transition_mu);
    auto start = session_media::begin_start();
    reap_portal_cleanup();
    if (session_media::pending_start_cancelled(session_media::pending_start_owner())) {
      return nullptr;
    }
    // Lock contract (SB-2 + S4 single mutex):
    // 1) Under g_media_mu: ensure session + start PipeWire (no dual-mutex nesting).
    // 2) Wait for negotiation OUTSIDE the lock so release_global_capture can progress.
    const bool want_prefer_hdr = portal_prefer_hdr_formats(client_dynamic_range, generation.stream_mode);
    const bool want_prefer_sdr = portal_prefer_sdr_formats(client_dynamic_range);
    std::shared_ptr<pipewire_capture::capture_t> capture;
    {
      std::unique_lock lock(g_media_mu);

      if (g_media.capture && g_media.capture->running()) {
        const auto compatible = g_media.requested_width == width &&
                                g_media.requested_height == height &&
                                av_cmp_q(g_media.requested_rate, requested_rate) == 0 &&
                                g_media.mem_type == mem_type &&
                                g_media.generation == generation &&
                                g_media.prefer_hdr == want_prefer_hdr &&
                                g_media.prefer_sdr == want_prefer_sdr;
        if (compatible) {
          if (capture_start_cancelled()) return nullptr;
          // A normal capture call atomically adopts the preparation. A later
          // expiry of its HTTP launch must not stop an active video thread.
          g_media.prepared_token = prepared_token;
          if (route_out) *route_out = g_media.route;
          return g_media.capture;
        }

        BOOST_LOG(info) << "portal: capture configuration changed; retiring PipeWire generation before reconnect"sv;
        auto retired_capture = std::move(g_media.capture);
        // ANNOUNCE may refine size/HDR after HTTP launch. Retain the permission
        // session for the same prepared source so that renegotiating PipeWire
        // cannot reopen a picker after the client starts its video timer.
        const bool retain_prepared_portal = g_media.prepared_token &&
          g_media.generation == generation;
        auto retired_portal = retain_prepared_portal ? nullptr : std::move(g_media.portal);
        auto retired_kwin = std::move(g_media.kwin);
        g_media.clear_meta();
        lock.unlock();
        retired_capture->stop();
        retired_capture->shutdown();
        retired_capture.reset();
        retired_portal.reset();
        retired_kwin.reset();
        lock.lock();
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

      g_media.prepared_token = prepared_token;
      // Which local node this capture asked for and how each fared, which names its route.
      auto gamescope_node = local_node_e::not_asked;
      auto kwin_node = local_node_e::not_asked;

      // W3/W5 gamescopegrab: prefer session-graph Video/Source (media.name=gamescope)
      // without private portal ScreenCast when linux_stream_mode=gamescope_stream.
      // Falls through to portal if the node is missing (idle unit not exporting yet).
      //
      // The same node is what a host in Steam Game Mode shows on its one screen. There the
      // session's own gamescope exports it, Polaris owns nothing, and a mirror of "the desktop"
      // means that picture. The lookup never asked who owns the compositor, so nothing else
      // changes: the stream attaches to the node and leaves the session alone.
      if ((!g_media.capture || !g_media.capture->running()) &&
          (((generation.stream_mode == "gamescope_stream" || generation.stream_mode.empty()) &&
            generation.private_runtime == "gamescope") ||
           game_mode_screen_generation(generation))) {
        if (auto gs = pipewire_capture::find_gamescope_video_source()) {
          if (auto local = start_local_pw_capture(
                gs->node_id, gs->object_serial, width, height, mem_type, client_dynamic_range, generation, requested_rate)) {
            BOOST_LOG(info) << "portal: gamescopegrab local Video/Source node="sv << gs->node_id
                            << " name="sv << gs->node_name << " (no private ScreenCast)"sv;
            g_media.kwin.reset();
            g_media.capture = std::move(local);
            g_media.requested_width = width;
            g_media.requested_height = height;
        g_media.requested_rate = requested_rate;
            g_media.mem_type = mem_type;
            g_media.generation = generation;
            g_media.prefer_hdr = want_prefer_hdr;
            g_media.prefer_sdr = want_prefer_sdr;
            capture = g_media.capture;
            gamescope_node = local_node_e::started;
            // Skip portal session setup; negotiation wait continues below.
          }
          else {
            BOOST_LOG(info) << "portal: gamescopegrab start failed; falling back to portal ScreenCast"sv;
            gamescope_node = local_node_e::failed;
          }
        }
        else {
          // Falls through to the portal, as it always did; the route now says so.
          gamescope_node = local_node_e::missing;
        }
      }

#ifndef POLARIS_BUILD_WAYLAND
      if (generation.stream_mode == "host_virtual_display" ||
          generation.stream_mode == "desktop_takeover") {
        BOOST_LOG(error) << "portal: host virtual capture requires KWin output pinning, but Wayland support is not built"sv;
        return nullptr;
      }
#endif

#ifdef POLARIS_BUILD_WAYLAND
      // W4/P1 kwingrab: host KDE desktop_display / headless_dongle — try KWin
      // zkde screencast before portal picker. Fail cleanly when not on KWin.
      // capture=portal/auto/empty only; explicit wlr/kms unchanged.
      const bool portal_like_capture = portal_capture_backend_allowed(generation.capture_backend);
      if ((!g_media.capture || !g_media.capture->running()) &&
          portal_like_capture &&
          kwingrab::prefer_for_generation(generation)) {
        g_media.kwin.reset();
        auto kwin_failure = kwingrab::start_failure_e::stream_failed;
        if (auto kwin_session = kwingrab::start_output_session(generation.requested_output_name, &kwin_failure)) {
          const auto &src = kwin_session->source();
          if (auto local = start_local_pw_capture(
                src.node_id,
                src.object_serial,
                width > 0 ? width : src.width,
                height > 0 ? height : src.height,
                mem_type,
                client_dynamic_range,
                generation, requested_rate)) {
            BOOST_LOG(info) << "portal: kwingrab local PW node="sv << src.node_id
                            << " output="sv << src.output_name
                            << " (no xdg-desktop-portal picker)"sv;
            g_media.kwin = std::shared_ptr<kwingrab::session_t>(std::move(kwin_session));
            g_media.portal.reset();
            g_media.capture = std::move(local);
            g_media.requested_width = width;
            g_media.requested_height = height;
        g_media.requested_rate = requested_rate;
            g_media.mem_type = mem_type;
            g_media.generation = generation;
            g_media.prefer_hdr = want_prefer_hdr;
            g_media.prefer_sdr = want_prefer_sdr;
            capture = g_media.capture;
            kwin_node = local_node_e::started;
          }
          else {
            BOOST_LOG(error) << "portal: kwingrab PipeWire start failed"sv;
            kwin_node = local_node_e::failed;
            kwin_session.reset();
            if (kwingrab::require_for_generation(generation)) {
              BOOST_LOG(error) << "portal: host virtual capture requires output-pinned KWin capture; refusing generic portal fallback"sv;
              return nullptr;
            }
            BOOST_LOG(info) << "portal: falling back to portal ScreenCast"sv;
          }
        }
        else {
          BOOST_LOG(error) << "portal: kwingrab unavailable"sv;
          kwin_node = kwin_node_without_session(kwin_failure);
          if (kwingrab::require_for_generation(generation)) {
            BOOST_LOG(error) << "portal: host virtual capture requires output-pinned KWin capture; refusing generic portal fallback"sv;
            return nullptr;
          }
          BOOST_LOG(info) << "portal: falling back to portal ScreenCast"sv;
        }
      }
#endif

      // gamescopegrab / kwingrab already filled capture — skip portal session.
      if (capture && capture->running()) {
        // fall through to negotiation wait outside lock
      }
      else if (!ensure_session_unlocked(generation)) {
        return nullptr;
      }
      else {
        auto *session = g_media.portal.get();
        const auto encoder_render_node = encoder_render_node_for_dmabuf(generation.adapter_name);
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
        const auto dmabuf_override = portal_dmabuf_override();
        const bool allow_vaapi = dmabuf_override == pipewire_capture::dmabuf_override_e::allow_vaapi;
        const bool encoder_import_supported = encoder_dmabuf_import_compiled(mem_type);
        log_dmabuf_policy(mem_type, dmabuf_override, "portal_remote"sv);
        if (dmabuf_override == pipewire_capture::dmabuf_override_e::force_cpu ||
            (mem_type == platf::mem_type_e::vaapi && !allow_vaapi) ||
            mem_type == platf::mem_type_e::vulkan) {
          // The policy log above records whether this is an operator-forced or
          // default stability containment decision, or Vulkan Video's shared
          // memory policy, which no render node below could change.
        } else if (!session->capture_render_node) {
          BOOST_LOG(info) << "portal: DMA-BUF disabled because the portal stream did not provide an explicit capture render node"sv;
        } else if (!encoder_render_node) {
          BOOST_LOG(info) << "portal: DMA-BUF disabled because the capture generation adapter is not an explicit canonical render node"sv;
        } else if (*session->capture_render_node != *encoder_render_node) {
          BOOST_LOG(info) << "portal: DMA-BUF disabled because capture render node ["sv << *session->capture_render_node
                          << "] does not match encoder adapter ["sv << *encoder_render_node << ']';
        } else if (!encoder_import_supported) {
          if (const auto line = missing_import_path_line(mem_type); !line.empty()) {
            BOOST_LOG(info) << line;
          }
        } else if (mem_type != platf::mem_type_e::cuda &&
                   mem_type != platf::mem_type_e::vulkan_pyrowave &&
                   !(allow_vaapi && mem_type == platf::mem_type_e::vaapi)) {
          BOOST_LOG(info) << "portal: DMA-BUF disabled because encoder memory type is neither CUDA nor explicitly enabled VAAPI"sv;
        } else {
          if (mem_type == platf::mem_type_e::vulkan_pyrowave) {
#ifdef POLARIS_BUILD_PYROWAVE
            // The same packed RGB formats CUDA asks for, and for each of them the layouts this
            // encoder's device says it can import, asked of Vulkan rather than assumed.
            //
            // Offering linear alone, which is what the line below does and what this arm used to
            // borrow, is why the offer was refused on the host this was written on: KWin does not
            // hand out a linear buffer for a virtual output on that driver, so capture fell back to
            // shared memory with the fast path sitting there unused.
            //
            // Asking is what makes offering more than linear safe. A frame that arrives as a dmabuf
            // has no host copy behind it, so a layout that turns out not to import is a lost frame
            // rather than a slow one, and every modifier here has been checked against the exact
            // image the import creates.
            for (const auto spa_format : {SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA,
                                          SPA_VIDEO_FORMAT_xBGR_210LE, SPA_VIDEO_FORMAT_xRGB_210LE}) {
              const auto drm_format = pipewire_capture::drm_format_for_spa(spa_format);
              if (!drm_format) {
                continue;
              }
              for (const auto modifier : pyrowave_encode::importable_dmabuf_modifiers(*drm_format)) {
                dmabuf_formats.push_back({
                  .spa_format = static_cast<std::uint32_t>(spa_format),
                  .drm_fourcc = *drm_format,
                  .modifier = modifier,
                });
              }
            }
            BOOST_LOG(info) << "portal: offering "sv << dmabuf_formats.size()
                            << " dmabuf layouts the compute codec can import"sv;
#endif
          } else if (mem_type == platf::mem_type_e::cuda) {
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
            pipewire_capture::offer_hdr_linear_ten_bit(
              dmabuf_formats,
              mem_type == platf::mem_type_e::vulkan_pyrowave
            );
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
            .encoder_import_supported = encoder_import_supported,
            .egl_import_supported = !dmabuf_formats.empty(),
          };
          may_use_dmabuf = pipewire_capture::may_offer_dmabuf(eligibility, dmabuf_override);
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
                        << " vaapi_opt_in="sv << (allow_vaapi ? "true"sv : "false"sv)
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
          .requested_rate = requested_rate,
          .request_fixed_rate = generation.private_runtime != "gamescope" && running_kwin_uses_fixed_rate(),
        });
        if (session_media::teardown_in_progress() || session_media::pending_start_cancelled(session_media::pending_start_owner())) return nullptr;
        if (!new_capture->start()) {
          BOOST_LOG(warning) << "portal: Failed to start PipeWire capture; invalidating portal session"sv;
          new_capture.reset();
          g_media.portal.reset();
          return nullptr;
        }

        g_media.requested_width = width;
        g_media.requested_height = height;
        g_media.requested_rate = requested_rate;
        g_media.mem_type = mem_type;
        g_media.generation = generation;
        g_media.prefer_hdr = want_prefer_hdr;
        g_media.prefer_sdr = want_prefer_sdr;
        g_media.capture = new_capture;
        capture = g_media.capture;
      }  // portal ScreenCast path
      g_media.route = portal_capture_route(gamescope_node, kwin_node);
    }

    // The capture transport determines whether the encoder factory must use
    // RAM or GPU-resident input, so never select a factory before negotiation.
    // Wait outside g_media_mu so release_global_capture can take the lock.
    const bool negotiated = wait_for_capture_negotiation(capture);

    {
      std::lock_guard lock(g_media_mu);
      // If release_global_capture raced us, drop the orphan (caller must not use it).
      if (g_media.capture != capture || g_media.generation != generation) {
        return nullptr;
      }
      if (capture_start_cancelled()) {
        if (capture) capture->stop();
        return nullptr;
      }
      if (!negotiated) {
        BOOST_LOG(warning) << "portal: PipeWire format negotiation did not complete; invalidating portal session"sv;
        // Keep local `capture` so ~capture_t runs after unlock (no dtor under g_media_mu).
        g_media.reset_all();
        return nullptr;
      }
      if (route_out) *route_out = g_media.route;
      return g_media.capture;
    }
  }

  static void release_prepared_capture(const std::shared_ptr<const void> &token) {
    std::lock_guard transition_lock(g_capture_transition_mu);
    {
      std::lock_guard media_lock(g_media_mu);
      if (g_media.prepared_token != token) {
        return;
      }
    }
    // Reject stale leases before entering teardown: begin_teardown itself
    // cancels portal requests. The transition lock fences replacement/adoption.
    release_global_capture();
  }

  bool prepare_capture(platf::mem_type_e mem_type, const video::config_t &config,
                       std::shared_ptr<void> &preparation) {
    auto token = std::make_shared<const char>();
    auto owner = std::shared_ptr<void>(new char, [token](void *value) {
      delete static_cast<char *>(value);
      session_media::schedule_retirement([token]() { release_prepared_capture(token); });
    });
    if (!ensure_global_capture(config.width, config.height, mem_type,
                               config.dynamicRange, config.capture_generation,
                               video::framerate_to_rational(config), token)) {
      return false;
    }
    preparation = std::move(owner);
    return true;
  }

#ifdef POLARIS_TESTS
  std::shared_ptr<const void> install_prepared_cache_for_tests() {
    std::lock_guard lock(g_media_mu);
    g_media.portal = std::make_unique<portal_session_t>();
    g_media.prepared_token = std::make_shared<const char>();
    return g_media.prepared_token;
  }

  void adopt_prepared_cache_for_tests() {
    std::lock_guard lock(g_media_mu);
    g_media.prepared_token.reset();
  }

  void release_prepared_cache_for_tests(const std::shared_ptr<const void> &token) {
    release_prepared_capture(token);
  }

  bool prepared_cache_present_for_tests() {
    std::lock_guard lock(g_media_mu);
    return !g_media.empty();
  }
#endif

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
    AVRational requested_rate {0, 1};
    platf::mem_type_e mem_type = platf::mem_type_e::system;
    bool pipewire_dmabuf_negotiated = false;
    // SPA_VIDEO_FORMAT_* from PipeWire negotiate; 0 = unknown / not yet negotiated.
    std::uint32_t capture_spa_format = 0;

    bool probe_only = false;  // true during encoder probe (skip portal session)
    capture_generation::identity_t generation_;

    int
    init(platf::mem_type_e hwdevice_type, const std::string &display_name, const ::video::config_t &config) {
      generation_ = config.capture_generation;
      if (!portal_capture_backend_allowed(generation_.capture_backend)) {
        BOOST_LOG(error) << "portal: capture generation backend ["sv << generation_.capture_backend
                         << "] is not portal-authoritative; refusing initialization"sv;
        return -1;
      }
      if (!generation_.exact_display_name.empty() &&
          (display_name != generation_.exact_display_name ||
           generation_.requested_output_name != generation_.exact_display_name)) {
        BOOST_LOG(error) << "portal: exact capture generation identity mismatch; refusing initialization"sv;
        return -1;
      }
      requested_width = config.width;
      requested_height = config.height;
      cfg_width = requested_width;
      cfg_height = requested_height;
      client_dynamic_range = config.dynamicRange;
      requested_rate = video::framerate_to_rational(config);
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
        cage_configured = generation_.use_cage_compositor;
#endif
        if (!cage_configured) {
          platf::capture_route_t route;
          auto cap = ensure_global_capture(requested_width, requested_height, mem_type, client_dynamic_range, generation_, requested_rate, {}, &route);
          if (!cap) {
            return -1;
          }
          // What initialization opened. A later capture() that has to start the capture again
          // does not change it: this is the route the display came up on.
          capture_route = std::move(route);


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
          // Direct screencopy from Polaris' own labwc: the portal backend opened, and no portal ran.
          capture_route.opened = "cage";
          capture_route.route = "cage";
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

      // In Game Mode the session's gamescope can fit its own screen into the frame, bars and all,
      // and it turns every touch from our virtual touchscreen by the internal panel's orientation.
      // The screen is named so input maps inside it, and the turn so a touch is turned back first.
      this->scaled_screen_width = 0;
      this->scaled_screen_height = 0;
      this->compositor_touch_turn = 0;
      if (!probe_only && game_mode_screen_generation(generation_)) {
        if (const auto screen = platf::game_mode_host::session_screen_within(std::chrono::milliseconds {500})) {
          this->scaled_screen_width = screen->width;
          this->scaled_screen_height = screen->height;
          this->compositor_touch_turn = screen->touch_turn.degrees;
          if (screen->touch_turn.degrees != 0) {
            BOOST_LOG(info) << "portal: Game Mode screen is "sv << screen->width << 'x' << screen->height
                            << ", fitted into the "sv << cfg_width << 'x' << cfg_height << " frame; gamescope turns a touch "sv
                            << screen->touch_turn.degrees << " degrees for "sv << screen->touch_turn.source
                            << ", so each touch is turned back before it is sent"sv;
          } else {
            BOOST_LOG(info) << "portal: Game Mode screen is "sv << screen->width << 'x' << screen->height
                            << ", fitted into the "sv << cfg_width << 'x' << cfg_height << " frame; touch is not turned, for "sv
                            << screen->touch_turn.source;
          }
        }
      }

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
      if (generation_.use_cage_compositor) {
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

      // Fallback: source-owned portal/KWin capture (only when cage is NOT configured)
      auto cap = ensure_global_capture(requested_width, requested_height, mem_type, client_dynamic_range, generation_, requested_rate);
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


      // gamescope sends a frame only when the focused window commits, so a Game Mode screen that is
      // standing still gives a new capture nothing to show. Past the private compositor branch above,
      // a capture on a host in Game Mode is a capture of that screen, by either route.
      platf::game_mode_host::first_frame_t first_frame {platf::game_mode_host::session_live()};
      if (first_frame.ask()) {
        platf::game_mode_host::request_focused_window_repaint_async();
      }

      bool capture_transport_logged = false;
      while (cap) {
        const auto capture_start = std::chrono::steady_clock::now();
        const auto wait_result = cap->wait_for_frame(1s);
        const auto dispatch_time = std::chrono::duration_cast<std::chrono::microseconds>(
          std::chrono::steady_clock::now() - capture_start);

        switch (wait_result) {
          case pipewire_capture::wait_result_e::timeout: {
            if (first_frame.ask()) {
              platf::game_mode_host::request_focused_window_repaint_async();
            }
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
            first_frame.frame_arrived();
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
#ifdef POLARIS_BUILD_VULKAN
      if (mem_type == platf::mem_type_e::vulkan) {
        // Portal remains on its safe SHM/MemFd transport for Vulkan until the
        // negotiated DMA-BUF route has the same automatic live-frame
        // retirement contract as the private WLR paths. The upload and color
        // conversion remain Vulkan-backed and an explicit Vulkan request can
        // therefore work without requiring DRM/KMS permissions.
        return vk::make_avcodec_encode_device_ram(
          w,
          h,
          config::video.adapter_name.empty() ?
            platf::default_render_device() : config::video.adapter_name
        );
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

    // A Game Mode host may run no ScreenCast portal at all: SteamOS ships one for gamescope,
    // other gamescope-session distributions need not. The session's picture is still there as
    // gamescope's own PipeWire node, and this backend is the one that attaches to it.
    if (game_mode_host::session_live() && pipewire_capture::find_gamescope_video_source()) {
      BOOST_LOG(info) << "Portal: Steam Game Mode screen available as a PipeWire source"sv;
      names.emplace_back("0");
      return names;
    }

    if (!portal::is_portal_available()) {
      BOOST_LOG(debug) << "Portal: ScreenCast interface not available"sv;
      return names;
    }

    BOOST_LOG(info) << "Portal: XDG Desktop Portal ScreenCast interface detected"sv;
    names.emplace_back("0");
    return names;
  }

}  // namespace platf
