/**
 * @file tests/unit/platform/test_portal_grab_policy.cpp
 * @brief Test XDG Desktop Portal and PipeWire capture policy.
 */

#include "../../tests_common.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <vector>

#include <unistd.h>

#include <drm_fourcc.h>
#include <spa/param/video/raw.h>

#include "src/config.h"
#include "src/platform/common.h"
#include "src/platform/linux/pipewire_capture.h"
#include "src/platform/linux/portal_session.h"
#include "src/platform/linux/session_media.h"

#ifdef POLARIS_BUILD_WAYLAND
  #include "src/platform/linux/kwingrab.h"
#endif

namespace portal {
  std::uint32_t portal_pick_cursor_mode_for_tests(std::uint32_t available);
  bool portal_cancel_pending_request_for_tests();
  bool portal_cancel_request_owner_for_tests();
  bool portal_cancel_source_wakes_wait_for_tests();
}

TEST(PortalGrabPolicyTests, DesktopDisplayRequestsMonitorSource) {
  EXPECT_EQ(portal::capture_type_for_stream_display(false, false), 1u);
  EXPECT_EQ(portal::capture_type_for_stream_display(false, false, "desktop_display"), 1u);
}

TEST(PortalGrabPolicyTests, DongleRequestsMonitorSourceDespiteHeadlessFlag) {
  // headless_dongle uses headless_mode for topology privacy, not window capture.
  EXPECT_EQ(portal::capture_type_for_stream_display(true, false, "headless_dongle"), 1u);
}

TEST(PortalGrabPolicyTests, HostVirtualDisplayRequestsMonitorSourceDespiteHeadlessFlag) {
  // host_virtual_display streams the created EVDI virtual monitor. Its legacy
  // booleans set headless_mode=true, which previously dropped it into the
  // window-source branch — the wrong source type for a monitor mode, and the
  // exact window+restore_token combination the dongle comment above warns
  // hangs KDE ScreenCast.
  EXPECT_EQ(portal::capture_type_for_stream_display(true, false, "host_virtual_display"), 1u);
}

#ifdef POLARIS_BUILD_WAYLAND
TEST(PortalGrabPolicyTests, KwingrabPreferredForHostKdeModesIncludingVirtualDisplay) {
  struct config_guard_t {
    config::video_t::linux_display_t linux_display = config::video.linux_display;

    ~config_guard_t() {
      config::video.linux_display = linux_display;
    }
  } guard;

  auto &ld = config::video.linux_display;

  // Host KDE paths pin capture through kwingrab. host_virtual_display must be
  // one of them: its EVDI output is composited by KWin, and kwingrab is the
  // only path that can select that output by name — the portal picker cannot.
  for (const char *mode : {"desktop_display", "headless_dongle", "host_virtual_display"}) {
    ld.stream_mode = mode;
    EXPECT_TRUE(kwingrab::prefer_for_current_stream_mode()) << mode;
  }

  // Private compositor runtimes stay on gamescopegrab / portal / wlroots.
  for (const char *mode : {"windowed_stream", "headless_stream", "gamescope_stream"}) {
    ld.stream_mode = mode;
    EXPECT_FALSE(kwingrab::prefer_for_current_stream_mode()) << mode;
  }
}
#endif

namespace platf {
  bool host_virtual_display_needs_portal();
}

TEST(PortalGrabPolicyTests, HostVirtualDisplayIsKeptOffTheAutoWlrSource) {
  // Companion to PR #351's kwingrab routing: even with the right kwingrab
  // preference, host_virtual_display only reaches portal_grab (hence kwingrab)
  // if reevaluate_capture_sources does NOT auto-select the direct WAYLAND/wlr
  // source first — which binds the desktop wayland-0 (no wlr-export-dmabuf) and
  // hard-fails every encoder. This guards the source-selection half of that fix.
  struct config_guard_t {
    config::video_t::linux_display_t linux_display = config::video.linux_display;
    std::string capture = config::video.capture;

    ~config_guard_t() {
      config::video.linux_display = linux_display;
      config::video.capture = capture;
    }
  } guard;

  auto &ld = config::video.linux_display;
  config::video.capture.clear();  // the auto path is where the bug lived

  // host_virtual_display composites through KWin (use_cage stays false): must
  // be kept off the auto wlr source so PORTAL (→ kwingrab) is chosen.
  ld.stream_mode = "host_virtual_display";
  ld.use_cage_compositor = false;
  EXPECT_TRUE(platf::host_virtual_display_needs_portal());

  // A cage-compositor session (the booleans HVD never carries) streams through
  // labwc's own wlr socket, not portal — must NOT be diverted.
  ld.use_cage_compositor = true;
  EXPECT_FALSE(platf::host_virtual_display_needs_portal());

  // Every other mode keeps its existing source selection untouched.
  ld.use_cage_compositor = false;
  for (const char *mode :
       {"windowed_stream", "headless_stream", "gamescope_stream", "desktop_display", "headless_dongle", ""}) {
    ld.stream_mode = mode;
    EXPECT_FALSE(platf::host_virtual_display_needs_portal()) << mode;
  }
}

TEST(PortalGrabPolicyTests, PrivateAndWindowedCagePathsRequestWindowSource) {
  EXPECT_EQ(portal::capture_type_for_stream_display(true, true), 2u);
  EXPECT_EQ(portal::capture_type_for_stream_display(false, true), 2u);
  EXPECT_EQ(portal::capture_type_for_stream_display(true, false, "gamescope_stream"), 2u);
}

// XDG ScreenCast AvailableCursorModes bits: 1=Hidden, 2=Embedded, 4=Metadata.
TEST(PortalGrabPolicyTests, CursorModePrefersEmbeddedThenMetadataThenHidden) {
  EXPECT_EQ(portal::portal_pick_cursor_mode_for_tests(0), 0u);
  EXPECT_EQ(portal::portal_pick_cursor_mode_for_tests(1), 1u);  // Hidden only
  EXPECT_EQ(portal::portal_pick_cursor_mode_for_tests(2), 2u);  // Embedded
  EXPECT_EQ(portal::portal_pick_cursor_mode_for_tests(4), 4u);  // Metadata
  EXPECT_EQ(portal::portal_pick_cursor_mode_for_tests(7), 2u);  // all → Embedded
  EXPECT_EQ(portal::portal_pick_cursor_mode_for_tests(5), 4u);  // Hidden|Metadata → Metadata
  EXPECT_EQ(portal::portal_pick_cursor_mode_for_tests(3), 2u);  // Hidden|Embedded → Embedded
}

TEST(PortalGrabPolicyTests, CancelPendingRequestsCancelsRegisteredRequest) {
  EXPECT_TRUE(portal::portal_cancel_pending_request_for_tests());
}

TEST(PortalGrabPolicyTests, CancelPendingRequestsMatchesOwner) {
  EXPECT_TRUE(portal::portal_cancel_request_owner_for_tests());
}

TEST(PortalGrabPolicyTests, CancellableSourceWakesWaitLoop) {
  EXPECT_TRUE(portal::portal_cancel_source_wakes_wait_for_tests());
}

TEST(PortalGrabPolicyTests, PendingStartCancellationScopeIsOwnerBounded) {
  int owner_a = 0;
  int owner_b = 0;
  EXPECT_FALSE(session_media::pending_start_cancelled(&owner_a));
  {
    auto owner = session_media::cancel_pending_starts(&owner_a);
    EXPECT_TRUE(session_media::pending_start_cancelled(&owner_a));
    EXPECT_FALSE(session_media::pending_start_cancelled(&owner_b));
  }
  EXPECT_FALSE(session_media::pending_start_cancelled(&owner_a));
}

TEST(PortalGrabPolicyTests, TeardownCancelsPortalWaitBeforeWaitingForStartFence) {
  const auto portal_path =
    std::filesystem::path(POLARIS_SOURCE_DIR) / "src/platform/linux/portal_session.cpp";
  const auto media_path =
    std::filesystem::path(POLARIS_SOURCE_DIR) / "src/platform/linux/session_media.cpp";
  std::ifstream portal_in(portal_path);
  std::ifstream media_in(media_path);
  ASSERT_TRUE(portal_in.good());
  ASSERT_TRUE(media_in.good());
  std::ostringstream portal_out;
  std::ostringstream media_out;
  portal_out << portal_in.rdbuf();
  media_out << media_in.rdbuf();
  const auto portal_source = portal_out.str();
  const auto media_source = media_out.str();

  const auto begin = media_source.find("teardown_owner_t begin_teardown()");
  const auto begin_end = media_source.find("teardown_owner_t prepare_for_stop()", begin);
  ASSERT_NE(begin, std::string::npos);
  ASSERT_NE(begin_end, std::string::npos);
  const auto begin_body = media_source.substr(begin, begin_end - begin);
  const auto gate_wait = begin_body.find("media_gate().begin_teardown([]");
  const auto cancel = begin_body.find("portal::cancel_pending_requests()", gate_wait);
  ASSERT_NE(gate_wait, std::string::npos)
    << "teardown must announce cancellation atomically with closing start admission";
  ASSERT_NE(cancel, std::string::npos)
    << "the gate announcement must cancel in-flight portal calls before waiting for starts";

  const auto helper = portal_source.find("static portal_request_result_t portal_call_and_wait_for_response");
  const auto helper_end = portal_source.find("static std::string make_request_path", helper);
  ASSERT_NE(helper, std::string::npos);
  ASSERT_NE(helper_end, std::string::npos);
  const auto helper_body = portal_source.substr(helper, helper_end - helper);
  EXPECT_NE(helper_body.find("g_cancellable_source_new(cancellable)"), std::string::npos)
    << "cancellation must also wake a Response wait after the method call returns";
  EXPECT_NE(
    helper_body.find("portal_call_sync(conn, method, params, call_timeout_ms, cancellable)"),
    std::string::npos
  ) << "the synchronous D-Bus call must receive the same cancellable";

  const auto create = portal_source.find("std::unique_ptr<portal_session_t> create_portal_session");
  ASSERT_NE(create, std::string::npos);
  const auto create_body = portal_source.substr(create);
  EXPECT_NE(create_body.find("pending_request_registration_t registration"), std::string::npos)
    << "the cancellable must cover cursor queries and every portal request";
  EXPECT_NE(
    create_body.find("portal_wait_cursor_modes(session->conn, cancellable)"),
    std::string::npos
  ) << "cursor-mode property calls must be teardown-cancellable";
  EXPECT_NE(create_body.find("session_media::teardown_in_progress()"), std::string::npos)
    << "a session registered after teardown announcement must cancel itself";
}

TEST(PortalGrabPolicyTests, TeardownCancellationPreservesRestoreToken) {
  const auto path =
    std::filesystem::path(POLARIS_SOURCE_DIR) / "src/platform/linux/portal_session.cpp";
  std::ifstream input(path);
  ASSERT_TRUE(input.good());
  std::ostringstream output;
  output << input.rdbuf();
  const auto source = output.str();

  EXPECT_NE(source.find("struct portal_request_result_t"), std::string::npos)
    << "portal cancellation must be distinguishable from timeout/failure";

  const auto select_begin = source.find("// Step 2: SelectSources");
  const auto select_end = source.find("// Step 3: Start", select_begin);
  ASSERT_NE(select_begin, std::string::npos);
  ASSERT_NE(select_end, std::string::npos);
  const auto select_body = source.substr(select_begin, select_end - select_begin);
  const auto select_cancelled = select_body.find("if (result.cancelled)");
  const auto select_clear = select_body.find("clear_restore_token()");
  ASSERT_NE(select_cancelled, std::string::npos);
  ASSERT_NE(select_clear, std::string::npos);
  EXPECT_LT(select_cancelled, select_clear)
    << "SelectSources cancellation must exit before stale-token invalidation";

  const auto start_begin = select_end;
  const auto start_end = source.find("session->ready = true", start_begin);
  ASSERT_NE(start_end, std::string::npos);
  const auto start_body = source.substr(start_begin, start_end - start_begin);
  const auto preserve_guard = start_body.find("if (!result.cancelled)");
  const auto start_clear = start_body.find("clear_restore_token()", preserve_guard);
  ASSERT_NE(preserve_guard, std::string::npos);
  ASSERT_NE(start_clear, std::string::npos)
    << "Start may clear a failed token only inside the non-cancelled path";
}

TEST(PortalGrabPolicyTests, TeardownCancellationCoversRemoteReopenAndRetryLoop) {
  const auto session_path =
    std::filesystem::path(POLARIS_SOURCE_DIR) / "src/platform/linux/portal_session.cpp";
  const auto grab_path =
    std::filesystem::path(POLARIS_SOURCE_DIR) / "src/platform/linux/portal_grab.cpp";
  std::ifstream session_in(session_path);
  std::ifstream grab_in(grab_path);
  ASSERT_TRUE(session_in.good());
  ASSERT_TRUE(grab_in.good());
  std::ostringstream session_out;
  std::ostringstream grab_out;
  session_out << session_in.rdbuf();
  grab_out << grab_in.rdbuf();
  const auto session_source = session_out.str();
  const auto grab_source = grab_out.str();

  const auto open_begin = session_source.find("int open_pipewire_remote_fd(");
  const auto open_end = session_source.find("struct portal_request_result_t", open_begin);
  ASSERT_NE(open_begin, std::string::npos);
  ASSERT_NE(open_end, std::string::npos);
  const auto open_body = session_source.substr(open_begin, open_end - open_begin);
  EXPECT_NE(open_body.find("pending_request_registration_t registration"), std::string::npos)
    << "an existing session remote reopen must register its own cancellable";
  EXPECT_NE(open_body.find("session_media::teardown_in_progress()"), std::string::npos)
    << "a reopen racing a previously announced teardown must self-cancel";
  EXPECT_NE(open_body.find("&out_fd_list,\n      cancellable,"), std::string::npos)
    << "OpenPipeWireRemote must receive the registered cancellable";

  const auto ensure_begin = grab_source.find("static bool ensure_session_unlocked()");
  const auto ensure_end = grab_source.find("static bool ensure_global_session()", ensure_begin);
  ASSERT_NE(ensure_begin, std::string::npos);
  ASSERT_NE(ensure_end, std::string::npos);
  const auto ensure_body = grab_source.substr(ensure_begin, ensure_end - ensure_begin);
  const auto shutdown_guard = ensure_body.find(
    "if (session_media::teardown_in_progress() || session_media::pending_start_cancelled(session_media::pending_start_owner()))"
  );
  const auto retry_sleep = ensure_body.find("std::this_thread::sleep_for");
  ASSERT_NE(shutdown_guard, std::string::npos)
    << "a cancelled portal session must not enter retry backoff during stop";
  ASSERT_NE(retry_sleep, std::string::npos);
  EXPECT_LT(shutdown_guard, retry_sleep);
}

TEST(PortalGrabPolicyTests, SelectSourcesInvalidatesRestoreTokenOnFailure) {
  // Source-level contract (S2: D-Bus lives in portal_session.cpp): failed
  // SelectSources must clear portal_restore_token and retry once without
  // restore_token (never permanently disable tokens).
  const auto path = std::filesystem::path(POLARIS_SOURCE_DIR) / "src/platform/linux/portal_session.cpp";
  std::ifstream in(path);
  ASSERT_TRUE(in.good());
  std::ostringstream out;
  out << in.rdbuf();
  const auto body = out.str();
  EXPECT_NE(body.find("clear_restore_token()"), std::string::npos);
  EXPECT_NE(body.find("retry once without restore_token"), std::string::npos);
  EXPECT_NE(body.find("save_restore_token("), std::string::npos);
  EXPECT_NE(body.find("portal_wait_cursor_modes("), std::string::npos);
  // Do not permanently disable restore tokens as a "fix".
  EXPECT_EQ(body.find("restore_token_disabled"), std::string::npos);
}

TEST(PortalGrabPolicyTests, LegacyRestoreTokenMigrationIsOneShot) {
  const auto path = std::filesystem::path(POLARIS_SOURCE_DIR) / "src/platform/linux/portal_session.cpp";
  std::ifstream in(path);
  ASSERT_TRUE(in.good());
  std::ostringstream out;
  out << in.rdbuf();
  const auto body = out.str();

  const auto migration_start =
    body.find("const std::string legacy = base + \"/portal_restore_token.txt\";");
  ASSERT_NE(migration_start, std::string::npos);
  const auto migration_end = body.find("return host;", migration_start);
  ASSERT_NE(migration_end, std::string::npos);
  const auto migration = body.substr(migration_start, migration_end - migration_start);

  EXPECT_NE(migration.find("std::filesystem::rename(legacy, host, ec)"), std::string::npos)
    << "legacy token migration must consume the source so a cleared stale host token is not resurrected";
  EXPECT_EQ(migration.find("copy_file(legacy, host"), std::string::npos)
    << "copying leaves the legacy token behind and reimports it after clear_restore_token()";
}

TEST(PortalGrabPolicyTests, ResponseSignalSubscriptionPrecedesPortalMethodCall) {
  const auto path = std::filesystem::path(POLARIS_SOURCE_DIR) / "src/platform/linux/portal_session.cpp";
  std::ifstream in(path);
  ASSERT_TRUE(in.good());
  std::ostringstream out;
  out << in.rdbuf();
  const auto body = out.str();

  const auto helper = body.find("portal_call_and_wait_for_response");
  ASSERT_NE(helper, std::string::npos)
    << "portal requests need one helper that subscribes before issuing the synchronous method call";
  const auto subscribe = body.find("g_dbus_connection_signal_subscribe", helper);
  const auto call = body.find("portal_call_sync(conn, method", helper);
  ASSERT_NE(subscribe, std::string::npos);
  ASSERT_NE(call, std::string::npos);
  EXPECT_LT(subscribe, call)
    << "a fast Request::Response signal is lost when subscription starts after the portal method returns";
  EXPECT_EQ(body.find("wait_for_response(session->conn"), std::string::npos)
    << "CreateSession, SelectSources, and Start must all use the subscribe-before-call helper";
}

TEST(PortalGrabPolicyTests, EnsureGlobalCaptureLockContractAndUniqueTokens) {
  // S4: single media_cache_t + g_media_mu (no dual-mutex). Negotiation waits
  // outside the lock so release_global_capture can progress. Session/token
  // hygiene lives in portal_session.cpp (S2).
  const auto grab_path = std::filesystem::path(POLARIS_SOURCE_DIR) / "src/platform/linux/portal_grab.cpp";
  std::ifstream grab_in(grab_path);
  ASSERT_TRUE(grab_in.good());
  std::ostringstream grab_out;
  grab_out << grab_in.rdbuf();
  const auto grab = grab_out.str();

  EXPECT_NE(grab.find("struct media_cache_t"), std::string::npos);
  EXPECT_NE(grab.find("g_media_mu"), std::string::npos);
  EXPECT_EQ(grab.find("g_portal_mu"), std::string::npos);
  EXPECT_EQ(grab.find("g_capture_mtx"), std::string::npos);
  EXPECT_NE(grab.find("ensure_session_unlocked()"), std::string::npos);
  EXPECT_NE(grab.find("Wait outside g_media_mu"), std::string::npos);
  EXPECT_NE(grab.find("pipewire_capture::capture_t"), std::string::npos);
  EXPECT_NE(grab.find("polaris-gamescope-force"), std::string::npos);
  // Hybrid guards: exclusive PQ when force ∧ dynamicRange>0; exclusive 8-bit when SDR.
  EXPECT_NE(grab.find("portal_prefer_hdr_formats"), std::string::npos);
  EXPECT_NE(grab.find("portal_prefer_sdr_formats"), std::string::npos);
  EXPECT_NE(grab.find("client_dynamic_range"), std::string::npos);
  EXPECT_NE(grab.find("portal_force_hdr_enabled()"), std::string::npos);
  EXPECT_NE(grab.find("gamescope_stream"), std::string::npos);
  EXPECT_NE(grab.find("prefer_sdr_formats"), std::string::npos);

  // Self-deadlock guard: ensure_global_capture must not call locking
  // ensure_global_session() under g_media_mu (only ensure_session_unlocked).
  const auto fn_start = grab.find("static std::shared_ptr<pipewire_capture::capture_t> ensure_global_capture(");
  ASSERT_NE(fn_start, std::string::npos);
  const auto fn_end = grab.find("class portal_display_t", fn_start);
  ASSERT_NE(fn_end, std::string::npos);
  const auto fn = grab.substr(fn_start, fn_end - fn_start);
  EXPECT_NE(fn.find("ensure_session_unlocked()"), std::string::npos);
  EXPECT_NE(fn.find("Wait outside g_media_mu"), std::string::npos);
  auto stripped = fn;
  for (;;) {
    const auto p = stripped.find("ensure_session_unlocked");
    if (p == std::string::npos) {
      break;
    }
    stripped.replace(p, sizeof("ensure_session_unlocked") - 1, "UNLOCKED_OK");
  }
  EXPECT_EQ(stripped.find("ensure_global_session("), std::string::npos)
    << "ensure_global_capture must not call locking ensure_global_session() under g_media_mu";

  const auto session_path = std::filesystem::path(POLARIS_SOURCE_DIR) / "src/platform/linux/portal_session.cpp";
  std::ifstream session_in(session_path);
  ASSERT_TRUE(session_in.good());
  std::ostringstream session_out;
  session_out << session_in.rdbuf();
  const auto session = session_out.str();
  EXPECT_NE(session.find("next_handle_token("), std::string::npos);
  EXPECT_NE(session.find("Start timeout/failure"), std::string::npos);
  EXPECT_NE(session.find("no Start retry"), std::string::npos);
  EXPECT_NE(session.find("portal_restore_token_host.txt"), std::string::npos);
  EXPECT_NE(session.find("portal_restore_token_private.txt"), std::string::npos);
  EXPECT_NE(session.find("POLARIS_PORTAL_DBUS_ADDRESS"), std::string::npos);
}

TEST(PortalGrabPolicyTests, HeadlessDongleNormalizeForcesKmsCapture) {
  const auto path = std::filesystem::path(POLARIS_SOURCE_DIR) / "src/platform/linux/stream_display_policy.cpp";
  std::ifstream in(path);
  ASSERT_TRUE(in.good());
  std::ostringstream out;
  out << in.rdbuf();
  const auto body = out.str();
  EXPECT_NE(body.find("normalize_config_from_load"), std::string::npos);
  EXPECT_NE(body.find("k_headless_dongle"), std::string::npos);
  // Dongle defaults to portal (host ScreenCast); explicit kms still allowed.
  EXPECT_NE(body.find("capture = \"portal\""), std::string::npos);
}

TEST(PortalGrabPolicyTests, DonglePrivacyBootstrapKeepsDeskWithoutHostToken) {
  // Source contract: atomic enable+disable is forbidden; privacy blank only when
  // portal_restore_token_host.txt exists (or capture is non-portal).
  const auto path = std::filesystem::path(POLARIS_SOURCE_DIR) / "src/platform/linux/display_topology.cpp";
  std::ifstream in(path);
  ASSERT_TRUE(in.good());
  std::ostringstream out;
  out << in.rdbuf();
  const auto body = out.str();
  EXPECT_NE(body.find("host_portal_restore_token_present"), std::string::npos);
  EXPECT_NE(body.find("bootstrap"), std::string::npos);
  EXPECT_NE(body.find("portal_restore_token_host.txt"), std::string::npos);
  EXPECT_NE(body.find("QT_QPA_PLATFORM=wayland"), std::string::npos);
  // Staged enable before disable — not a single atomic enable+disable command.
  EXPECT_NE(body.find("enable streaming output"), std::string::npos);
}

TEST(PipeWireCapturePolicyTests, MapsSupportedSpaFormatsToDrmFormats) {
  EXPECT_EQ(pipewire_capture::drm_format_for_spa(SPA_VIDEO_FORMAT_BGRx), DRM_FORMAT_XRGB8888);
  EXPECT_EQ(pipewire_capture::drm_format_for_spa(SPA_VIDEO_FORMAT_BGRA), DRM_FORMAT_ARGB8888);
  EXPECT_EQ(pipewire_capture::drm_format_for_spa(SPA_VIDEO_FORMAT_RGBx), DRM_FORMAT_XBGR8888);
  EXPECT_EQ(pipewire_capture::drm_format_for_spa(SPA_VIDEO_FORMAT_RGBA), DRM_FORMAT_ABGR8888);
}

TEST(PipeWireCapturePolicyTests, RejectsUnsupportedSpaFormats) {
  EXPECT_EQ(pipewire_capture::drm_format_for_spa(SPA_VIDEO_FORMAT_NV12), std::nullopt);
}

TEST(PipeWireCapturePolicyTests, CopiesPaddedRowsAndLeavesDestinationPaddingUntouched) {
  std::array<std::uint8_t, 20> source {
    10, 20, 30, 40,
    50, 60, 70, 80,
    0xAA, 0xAA, 0xAA, 0xAA,
    90, 100, 110, 120,
    130, 140, 150, 160,
  };
  std::vector<std::uint8_t> destination(24, 0xEE);

  const auto result = pipewire_capture::copy_memptr_frame_to_bgra(
    source.data(), source.size(), 0, source.size(), 2, 2, 12, SPA_VIDEO_FORMAT_BGRx, destination.data(), 12);

  ASSERT_TRUE(result);
  EXPECT_EQ(destination, (std::vector<std::uint8_t> {
    10, 20, 30, 40,
    50, 60, 70, 80,
    0xEE, 0xEE, 0xEE, 0xEE,
    90, 100, 110, 120,
    130, 140, 150, 160,
    0xEE, 0xEE, 0xEE, 0xEE,
  }));
}

TEST(PipeWireCapturePolicyTests, RejectsInsufficientSourcePayload) {
  std::array<std::uint8_t, 15> source {};
  std::array<std::uint8_t, 16> destination {};

  EXPECT_FALSE(pipewire_capture::copy_memptr_frame_to_bgra(
    source.data(), source.size(), 0, source.size(), 2, 2, 8, SPA_VIDEO_FORMAT_BGRx, destination.data(), 8));
}

TEST(PipeWireCapturePolicyTests, AppliesChunkOffsetBeforeCopying) {
  std::array<std::uint8_t, 12> source {
    0xCC, 0xCC, 0xCC, 0xCC,
    1, 2, 3, 4,
    5, 6, 7, 8,
  };
  std::array<std::uint8_t, 8> destination {};

  const auto result = pipewire_capture::copy_memptr_frame_to_bgra(
    source.data(), source.size(), 4, 8, 2, 1, 8, SPA_VIDEO_FORMAT_BGRx, destination.data(), 8);

  ASSERT_TRUE(result);
  EXPECT_EQ(destination, (std::array<std::uint8_t, 8> {1, 2, 3, 4, 5, 6, 7, 8}));
}

TEST(PipeWireCapturePolicyTests, BgrFormatsPreserveByteOrder) {
  const std::array<std::uint8_t, 8> source {1, 2, 3, 4, 5, 6, 7, 8};
  for (const auto format : {SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA}) {
    std::array<std::uint8_t, 8> destination {};
    ASSERT_TRUE(pipewire_capture::copy_memptr_frame_to_bgra(
      source.data(), source.size(), 0, source.size(), 2, 1, 8, format, destination.data(), 8));
    EXPECT_EQ(destination, source);
  }
}

TEST(PipeWireCapturePolicyTests, RgbFormatsSwapRedAndBlueToBgraByteOrder) {
  std::array<std::uint8_t, 8> source {
    10, 20, 30, 40,
    50, 60, 70, 80,
  };
  for (const auto format : {SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA}) {
    std::array<std::uint8_t, 8> destination {};
    ASSERT_TRUE(pipewire_capture::copy_memptr_frame_to_bgra(
      source.data(), source.size(), 0, source.size(), 2, 1, 8, format, destination.data(), 8));
    EXPECT_EQ(destination, (std::array<std::uint8_t, 8> {
      30, 20, 10, 40,
      70, 60, 50, 80,
    }));
  }
}

TEST(PipeWireCapturePolicyTests, RejectsNonPositiveStride) {
  std::array<std::uint8_t, 8> source {};
  std::array<std::uint8_t, 8> destination {};

  EXPECT_FALSE(pipewire_capture::copy_memptr_frame_to_bgra(
    source.data(), source.size(), 0, source.size(), 2, 1, 0, SPA_VIDEO_FORMAT_BGRx, destination.data(), 8));
  EXPECT_FALSE(pipewire_capture::copy_memptr_frame_to_bgra(
    source.data(), source.size(), 0, source.size(), 2, 1, -8, SPA_VIDEO_FORMAT_BGRx, destination.data(), 8));
}

TEST(PipeWireCapturePolicyTests, CpuFramesReportSharedMemoryMetadata) {
  const auto metadata = pipewire_capture::cpu_frame_metadata();

  EXPECT_EQ(metadata.transport, platf::frame_transport_e::shm);
  EXPECT_EQ(metadata.residency, platf::frame_residency_e::cpu);
  EXPECT_EQ(metadata.format, platf::frame_format_e::bgra8);
  EXPECT_TRUE(metadata.device.empty());
}

TEST(PipeWireCapturePolicyTests, DmaBufFramesReportGpuAndRenderNodeMetadata) {
  const auto metadata = pipewire_capture::dmabuf_frame_metadata("/dev/dri/renderD128");

  EXPECT_EQ(metadata.transport, platf::frame_transport_e::dmabuf);
  EXPECT_EQ(metadata.residency, platf::frame_residency_e::gpu);
  EXPECT_EQ(metadata.format, platf::frame_format_e::bgra8);
  EXPECT_EQ(metadata.device, "/dev/dri/renderD128");
}

TEST(PipeWireCapturePolicyTests, CpuCopyWarningMatchesActualFrameResidency) {
  EXPECT_TRUE(pipewire_capture::frame_requires_cpu_copy(pipewire_capture::cpu_frame_metadata()));
  EXPECT_FALSE(pipewire_capture::frame_requires_cpu_copy(
    pipewire_capture::dmabuf_frame_metadata("/dev/dri/renderD128")));

  auto inconsistent = pipewire_capture::dmabuf_frame_metadata("/dev/dri/renderD128");
  inconsistent.residency = platf::frame_residency_e::cpu;
  EXPECT_TRUE(pipewire_capture::frame_requires_cpu_copy(inconsistent));
}

TEST(PipeWireCapturePolicyTests, RenderNodeValidationAcceptsOnlyCanonicalRenderNodes) {
  EXPECT_EQ(pipewire_capture::canonical_render_node("/dev/dri/renderD128"), "/dev/dri/renderD128");
  EXPECT_EQ(pipewire_capture::canonical_render_node("/dev/dri/renderD0"), "/dev/dri/renderD0");

  EXPECT_EQ(pipewire_capture::canonical_render_node(" /dev/dri/renderD128"), std::nullopt);
  EXPECT_EQ(pipewire_capture::canonical_render_node("/dev/dri/card0"), std::nullopt);
  EXPECT_EQ(pipewire_capture::canonical_render_node("/dev/dri/renderD128/../renderD129"), std::nullopt);
  EXPECT_EQ(pipewire_capture::canonical_render_node("/dev/dri/renderD"), std::nullopt);
  EXPECT_EQ(pipewire_capture::canonical_render_node("/dev/dri/renderDabc"), std::nullopt);
}

TEST(PipeWireCapturePolicyTests, SameRenderNodeEligibilityRequiresExplicitMatchingGpuPathAndEglSupport) {
  const pipewire_capture::dmabuf_eligibility_t eligible {
    .capture_render_node = "/dev/dri/renderD128",
    .encoder_render_node = "/dev/dri/renderD128",
    .mem_type = platf::mem_type_e::vaapi,
    .egl_import_supported = true,
  };

  EXPECT_TRUE(pipewire_capture::may_offer_dmabuf(eligible));

  auto missing_capture = eligible;
  missing_capture.capture_render_node.reset();
  EXPECT_FALSE(pipewire_capture::may_offer_dmabuf(missing_capture));

  auto mismatched = eligible;
  mismatched.encoder_render_node = "/dev/dri/renderD129";
  EXPECT_FALSE(pipewire_capture::may_offer_dmabuf(mismatched));

  auto system_memory = eligible;
  system_memory.mem_type = platf::mem_type_e::system;
  EXPECT_FALSE(pipewire_capture::may_offer_dmabuf(system_memory));

  auto no_egl = eligible;
  no_egl.egl_import_supported = false;
  EXPECT_FALSE(pipewire_capture::may_offer_dmabuf(no_egl));

  auto noncanonical = eligible;
  noncanonical.capture_render_node = "renderD128";
  noncanonical.encoder_render_node = "renderD128";
  EXPECT_FALSE(pipewire_capture::may_offer_dmabuf(noncanonical));
}

TEST(PipeWireCapturePolicyTests, DmaBufCapabilityFilteringKeepsOnlyPackedRgbImportableNonExternalFormats) {
  const std::vector<pipewire_capture::dmabuf_format_modifier_t> portal_formats {
    {.spa_format = SPA_VIDEO_FORMAT_BGRx, .drm_fourcc = DRM_FORMAT_XRGB8888, .modifier = DRM_FORMAT_MOD_LINEAR},
    {.spa_format = SPA_VIDEO_FORMAT_BGRA, .drm_fourcc = DRM_FORMAT_ARGB8888, .modifier = 0x0100000000000002ULL},
    {.spa_format = SPA_VIDEO_FORMAT_NV12, .drm_fourcc = DRM_FORMAT_NV12, .modifier = DRM_FORMAT_MOD_LINEAR},
    {.spa_format = SPA_VIDEO_FORMAT_RGBx, .drm_fourcc = DRM_FORMAT_XBGR8888, .modifier = 0x0100000000000003ULL},
  };
  const std::vector<pipewire_capture::egl_dmabuf_format_t> egl_formats {
    {.drm_fourcc = DRM_FORMAT_XRGB8888, .modifiers = {DRM_FORMAT_MOD_LINEAR}},
    {.drm_fourcc = DRM_FORMAT_ARGB8888, .modifiers = {0x0100000000000002ULL}, .external_only_modifiers = {0x0100000000000002ULL}},
    {.drm_fourcc = DRM_FORMAT_NV12, .modifiers = {DRM_FORMAT_MOD_LINEAR}},
  };

  const auto filtered = pipewire_capture::filter_importable_dmabuf_formats(portal_formats, egl_formats);

  ASSERT_EQ(filtered.size(), 1u);
  EXPECT_EQ(filtered[0].spa_format, SPA_VIDEO_FORMAT_BGRx);
  EXPECT_EQ(filtered[0].drm_fourcc, DRM_FORMAT_XRGB8888);
  EXPECT_EQ(filtered[0].modifier, DRM_FORMAT_MOD_LINEAR);
}

TEST(PipeWireCapturePolicyTests, DmaBufPlaneDescriptorValidationRequiresOneCompletePackedRgbPlane) {
  pipewire_capture::dmabuf_frame_t frame {
    .width = 640,
    .height = 480,
    .spa_format = SPA_VIDEO_FORMAT_BGRx,
    .drm_fourcc = DRM_FORMAT_XRGB8888,
    .modifier = DRM_FORMAT_MOD_LINEAR,
    .planes = {{
      {.fd = 3, .chunk_offset = 0, .chunk_size = 640u * 480u * 4u, .stride = 640 * 4, .maxsize = 640u * 480u * 4u},
    }},
    .plane_count = 1,
  };

  EXPECT_TRUE(pipewire_capture::valid_dmabuf_frame(frame));

  auto no_planes = frame;
  no_planes.plane_count = 0;
  EXPECT_FALSE(pipewire_capture::valid_dmabuf_frame(no_planes));

  auto multiple_planes = frame;
  multiple_planes.plane_count = 2;
  multiple_planes.planes[1] = multiple_planes.planes[0];
  EXPECT_FALSE(pipewire_capture::valid_dmabuf_frame(multiple_planes));

  auto bad_fd = frame;
  bad_fd.planes[0].fd = -1;
  EXPECT_FALSE(pipewire_capture::valid_dmabuf_frame(bad_fd));

  auto bad_stride = frame;
  bad_stride.planes[0].stride = 0;
  EXPECT_FALSE(pipewire_capture::valid_dmabuf_frame(bad_stride));

  auto short_payload = frame;
  short_payload.planes[0].chunk_size = 64;
  EXPECT_FALSE(pipewire_capture::valid_dmabuf_frame(short_payload));

  auto wrapped_chunk = frame;
  wrapped_chunk.planes[0].chunk_offset = wrapped_chunk.planes[0].maxsize;
  EXPECT_TRUE(pipewire_capture::valid_dmabuf_frame(wrapped_chunk));

  auto bad_chunk = frame;
  bad_chunk.planes[0].chunk_size = bad_chunk.planes[0].maxsize + 1;
  EXPECT_FALSE(pipewire_capture::valid_dmabuf_frame(bad_chunk));

  auto zero_maxsize = frame;
  zero_maxsize.planes[0].maxsize = 0;
  EXPECT_FALSE(pipewire_capture::valid_dmabuf_frame(zero_maxsize));
}

TEST(PipeWireCapturePolicyTests, DmaBufDescriptorDuplicatesFdAndNormalizesChunkOffset) {
  int pipe_fds[2] {-1, -1};
  ASSERT_EQ(pipe(pipe_fds), 0);

  pipewire_capture::dmabuf_frame_t frame {
    .width = 4,
    .height = 2,
    .spa_format = SPA_VIDEO_FORMAT_BGRx,
    .drm_fourcc = DRM_FORMAT_XRGB8888,
    .modifier = DRM_FORMAT_MOD_LINEAR,
    .planes = {{
      {.fd = pipe_fds[0], .chunk_offset = 128, .chunk_size = 32, .stride = 16, .maxsize = 128},
    }},
    .plane_count = 1,
  };
  egl::img_descriptor_t descriptor;
  std::fill_n(descriptor.sd.fds, 4, -1);

  ASSERT_TRUE(pipewire_capture::fill_dmabuf_descriptor(frame, descriptor));
  EXPECT_GE(descriptor.sd.fds[0], 0);
  EXPECT_NE(descriptor.sd.fds[0], pipe_fds[0]);
  EXPECT_EQ(descriptor.sd.offsets[0], 0u);
  EXPECT_EQ(descriptor.sd.pitches[0], 16u);
  EXPECT_EQ(descriptor.sd.fourcc, DRM_FORMAT_XRGB8888);

  close(pipe_fds[0]);
  close(pipe_fds[1]);
}

TEST(PipeWireCapturePolicyTests, BufferDataTypePolicyMatchesNegotiatedEncoderTransport) {
  EXPECT_EQ(pipewire_capture::offered_buffer_data_types(true),
            (std::vector<std::uint32_t> {SPA_DATA_DmaBuf}));
  EXPECT_EQ(pipewire_capture::offered_buffer_data_types(false),
            (std::vector<std::uint32_t> {SPA_DATA_MemFd, SPA_DATA_MemPtr}));
}

TEST(PipeWireCapturePolicyTests, ResolveCaptureRenderNodePrefersPortalValue) {
  const auto resolved = pipewire_capture::resolve_capture_render_node(
    std::optional<std::string> {"/dev/dri/renderD128"},
    std::optional<std::string> {"/dev/dri/renderD129"});
  ASSERT_TRUE(resolved);
  EXPECT_EQ(*resolved, "/dev/dri/renderD128");
}

TEST(PipeWireCapturePolicyTests, ResolveCaptureRenderNodeFallsBackToEncoderAdapter) {
  const auto resolved = pipewire_capture::resolve_capture_render_node(
    std::nullopt, std::optional<std::string> {"/dev/dri/renderD128"});
  ASSERT_TRUE(resolved);
  EXPECT_EQ(*resolved, "/dev/dri/renderD128");
}

TEST(PipeWireCapturePolicyTests, ResolveCaptureRenderNodeRejectsNonRenderPaths) {
  EXPECT_FALSE(pipewire_capture::resolve_capture_render_node(
    std::optional<std::string> {"/dev/dri/card1"},
    std::nullopt));
  const auto resolved = pipewire_capture::resolve_capture_render_node(
    std::optional<std::string> {"/dev/dri/card1"},
    std::optional<std::string> {"/dev/dri/renderD128"});
  ASSERT_TRUE(resolved);
  EXPECT_EQ(*resolved, "/dev/dri/renderD128");
}

TEST(PipeWireCapturePolicyTests, PickSoleRenderNodeResolvesOnlySingleGpuHosts) {
  // Exactly one canonical candidate — a single-GPU host — resolves.
  EXPECT_EQ(
    pipewire_capture::pick_sole_render_node({"/dev/dri/renderD128"}),
    (std::optional<std::string> {"/dev/dri/renderD128"}));
  // A sole candidate that is not a canonical render node stays fail-closed.
  EXPECT_EQ(pipewire_capture::pick_sole_render_node({"/dev/dri/card0"}), std::nullopt);
  // Multi-GPU and empty lists stay fail-closed: DMA-BUF never crosses GPUs by guess.
  EXPECT_EQ(
    pipewire_capture::pick_sole_render_node({"/dev/dri/renderD128", "/dev/dri/renderD129"}),
    std::nullopt);
  EXPECT_EQ(pipewire_capture::pick_sole_render_node({}), std::nullopt);
}
