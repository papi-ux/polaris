/**
 * @file tests/unit/platform/test_portal_grab_policy.cpp
 * @brief Test XDG Desktop Portal and PipeWire capture policy.
 */

#include "../../tests_common.h"

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <optional>
#include <numeric>
#include <nlohmann/json.hpp>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include <unistd.h>

#include <fcntl.h>
#include <sys/prctl.h>
#include <sys/wait.h>

#include <drm_fourcc.h>
#include <spa/param/video/raw.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/filter.h>

#include "src/capture_generation.h"
#include "src/config.h"
#include "src/platform/common.h"
#include "src/platform/linux/pipewire_capture.h"
#include "src/platform/linux/portal_capability.h"
#include "src/platform/linux/portal_session.h"
#include "src/platform/linux/session_media.h"
#include "src/platform/linux/virtual_display.h"
#include "src/process.h"
#include "src/rtsp.h"
#include "src/video.h"

#ifdef POLARIS_BUILD_WAYLAND
  #include "src/platform/linux/kwingrab.h"
#endif

namespace portal {
  std::uint32_t portal_pick_cursor_mode_for_tests(std::uint32_t available);
  bool portal_cancel_pending_request_for_tests();
  bool portal_cancel_request_owner_for_tests();
  bool portal_cancel_source_wakes_wait_for_tests();
  bool wait_for_capture_negotiation_for_tests(const std::shared_ptr<pipewire_capture::capture_t> &capture);
  bool kwin_rate_query_with_hook_for_tests(const std::function<void(GCancellable *)> &hook);
  bool portal_capture_backend_allowed_for_tests(std::string_view capture_backend);
  bool portal_capture_generation_matches_for_tests(
    const capture_generation::identity_t &cached,
    const capture_generation::identity_t &requested
  );
}

namespace platf {
  std::string capture_backend_dispatch_for_tests(
    std::string_view capture_backend,
    bool nvfbc_available,
    bool wayland_available,
    bool portal_available,
    bool kms_available,
    bool x11_available,
    bool cuda_memory
  );

  std::string exact_capture_backend_dispatch_for_tests(
    std::string_view capture_backend,
    bool exact_output_owned,
    bool nvfbc_available,
    bool wayland_available,
    bool portal_available,
    bool kms_available,
    bool x11_available,
    bool cuda_memory
  );
}

TEST(PortalCapabilityPolicyTests, ExplicitCaptureSelectionWinsOverStreamModeDefault) {
  EXPECT_TRUE(portal_capability::requires_unprivileged_process("portal", "headless_stream"));
  EXPECT_TRUE(portal_capability::requires_unprivileged_process("PoRtAl", "headless_stream"));
  EXPECT_FALSE(portal_capability::requires_unprivileged_process("kms", "gamescope_stream"));
  EXPECT_FALSE(portal_capability::requires_unprivileged_process("wlr", "gamescope_stream"));
}

TEST(PortalCapabilityPolicyTests, PortalOrientedModesDropCapabilitiesForImplicitCapture) {
  for (const char *mode : {"desktop_display", "desktop_takeover", "gamescope_stream", "headless_dongle"}) {
    EXPECT_TRUE(portal_capability::requires_unprivileged_process("", mode)) << mode;
    EXPECT_TRUE(portal_capability::requires_unprivileged_process("auto", mode)) << mode;
  }

  EXPECT_FALSE(portal_capability::requires_unprivileged_process("", "headless_stream"));
  EXPECT_FALSE(portal_capability::requires_unprivileged_process("auto", "windowed_stream"));
}

TEST(PortalCapabilityPolicyTests, PreparationLeavesProcRootReadableToSameUserPortalPeer) {
  const auto result = portal_capability::prepare_process_for_capture("portal", "gamescope_stream");
  ASSERT_NE(result, portal_capability::prepare_result_e::failed);
  EXPECT_EQ(prctl(PR_GET_DUMPABLE, 0, 0, 0, 0), 1);

  const auto parent = getpid();
  const auto child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    const auto path = "/proc/" + std::to_string(parent) + "/root";
    const int fd = open(path.c_str(), O_PATH | O_CLOEXEC);
    if (fd >= 0) {
      close(fd);
      _exit(0);
    }
    _exit(1);
  }

  int status = 0;
  ASSERT_EQ(waitpid(child, &status, 0), child);
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 0);
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
  EXPECT_EQ(portal::capture_type_for_stream_display(true, false, "desktop_takeover"), 1u);
}

TEST(PortalGrabPolicyTests, EmptyModeSourcePolicyDoesNotReadMutableGlobalMode) {
  const auto original_mode = config::video.linux_display.stream_mode;
  config::video.linux_display.stream_mode = "host_virtual_display";
  EXPECT_EQ(portal::capture_type_for_stream_display(true, false, ""), 2u);
  config::video.linux_display.stream_mode = original_mode;
}

TEST(PortalGrabPolicyTests, ExplicitBackendDispatchNeverFallsThrough) {
  EXPECT_EQ(platf::capture_backend_dispatch_for_tests("wlr", false, false, true, false, false, false), "none");
  EXPECT_EQ(platf::capture_backend_dispatch_for_tests("wlr", false, true, true, false, false, false), "wayland");
  EXPECT_EQ(platf::capture_backend_dispatch_for_tests("portal", false, true, true, true, true, false), "portal");
  EXPECT_EQ(platf::capture_backend_dispatch_for_tests("kms", false, true, true, false, true, false), "none");
  EXPECT_EQ(platf::capture_backend_dispatch_for_tests("auto", false, true, true, true, true, false), "wayland");
  EXPECT_EQ(platf::capture_backend_dispatch_for_tests("bogus", true, true, true, true, true, true), "none");
}

TEST(PortalGrabPolicyTests, ExactOwnedBackendMustBeConcrete) {
  EXPECT_EQ(platf::exact_capture_backend_dispatch_for_tests("", true, false, false, true, false, false, false), "none");
  EXPECT_EQ(platf::exact_capture_backend_dispatch_for_tests("auto", true, false, true, true, false, false, false), "none");
  EXPECT_EQ(platf::exact_capture_backend_dispatch_for_tests("", false, false, false, true, false, false, false), "portal");
}

TEST(PortalGrabPolicyTests, PortalAcceptsOnlyPortalAuthority) {
  for (const auto backend : {"", "auto", "portal", "kwin"}) {
    EXPECT_TRUE(portal::portal_capture_backend_allowed_for_tests(backend)) << backend;
  }
  for (const auto backend : {"wlr", "kms", "drm", "x11", "nvfbc"}) {
    EXPECT_FALSE(portal::portal_capture_backend_allowed_for_tests(backend)) << backend;
  }
}

TEST(PortalGrabPolicyTests, CaptureCacheReuseRequiresTheWholeImmutableGeneration) {
  const capture_generation::identity_t generation {
    .generation_id = 42,
    .exact_display_name = "DVI-I-1",
    .requested_output_name = "DVI-I-1",
    .stream_mode = "host_virtual_display",
    .capture_backend = "portal",
    .private_runtime = "",
    .adapter_name = "/dev/dri/renderD128",
    .headless_mode = true,
    .use_cage_compositor = false,
  };
  EXPECT_TRUE(portal::portal_capture_generation_matches_for_tests(generation, generation));

  std::vector<capture_generation::identity_t> mismatches(9, generation);
  mismatches[0].generation_id = 43;
  mismatches[1].exact_display_name = "DVI-I-2";
  mismatches[2].requested_output_name = "HDMI-A-1";
  mismatches[3].stream_mode = "desktop_display";
  mismatches[4].capture_backend = "wlr";
  mismatches[5].private_runtime = "gamescope";
  mismatches[6].adapter_name = "/dev/dri/renderD129";
  mismatches[7].headless_mode = false;
  mismatches[8].use_cage_compositor = true;
  for (const auto &mismatch : mismatches) {
    EXPECT_FALSE(portal::portal_capture_generation_matches_for_tests(generation, mismatch));
  }
}

#ifdef POLARIS_BUILD_WAYLAND
TEST(PortalGrabPolicyTests, KwingrabPreferenceUsesImmutableGenerationPolicy) {
  capture_generation::identity_t generation;
  for (const char *mode : {"desktop_display", "headless_dongle", "host_virtual_display"}) {
    generation.stream_mode = mode;
    EXPECT_TRUE(kwingrab::prefer_for_generation(generation)) << mode;
  }
  for (const char *mode : {"windowed_stream", "headless_stream", "gamescope_stream"}) {
    generation.stream_mode = mode;
    EXPECT_FALSE(kwingrab::prefer_for_generation(generation)) << mode;
  }

  generation.stream_mode.clear();
  generation.use_cage_compositor = false;
  generation.private_runtime.clear();
  EXPECT_TRUE(kwingrab::prefer_for_generation(generation));
  generation.private_runtime = "gamescope";
  EXPECT_FALSE(kwingrab::prefer_for_generation(generation));
  generation.private_runtime.clear();
  generation.use_cage_compositor = true;
  EXPECT_FALSE(kwingrab::prefer_for_generation(generation));
}
TEST(PortalGrabPolicyTests, KwingrabRequirementUsesImmutableGenerationPolicy) {
  capture_generation::identity_t generation;
  generation.stream_mode = "host_virtual_display";
  EXPECT_TRUE(kwingrab::require_for_generation(generation));

  for (const char *unrequired : {"desktop_display", "headless_dongle", "windowed_stream", ""}) {
    generation.stream_mode = unrequired;
    EXPECT_FALSE(kwingrab::require_for_generation(generation)) << unrequired;
  }
}

TEST(PortalGrabPolicyTests, KwingrabNamedOutputMissCannotFallback) {
  EXPECT_TRUE(kwingrab::output_selection_can_fallback(""));
  EXPECT_FALSE(kwingrab::output_selection_can_fallback("DVI-I-1"));
}
#endif

namespace platf {
  bool host_virtual_display_needs_portal_for_backend(
    std::string_view stream_mode,
    bool use_cage_compositor,
    virtual_display::backend_e backend
  );
}

TEST(PortalGrabPolicyTests, HostVirtualDisplayCaptureRoutingDependsOnBackend) {
  using virtual_display::backend_e;

  for (const auto backend : {backend_e::EVDI, backend_e::KSCREEN_DOCTOR, backend_e::NONE}) {
    EXPECT_TRUE(platf::host_virtual_display_needs_portal_for_backend(
      "host_virtual_display", false, backend
    ));
  }

  EXPECT_FALSE(platf::host_virtual_display_needs_portal_for_backend(
    "host_virtual_display", false, backend_e::WAYLAND_WLR
  )) << "a native wlroots headless output must be captured directly by name";

  EXPECT_TRUE(platf::host_virtual_display_needs_portal_for_backend(
    "desktop_takeover", false, backend_e::EVDI
  ));
  EXPECT_FALSE(platf::host_virtual_display_needs_portal_for_backend(
    "desktop_takeover", false, backend_e::WAYLAND_WLR
  ));

  for (const auto backend : {backend_e::EVDI, backend_e::WAYLAND_WLR, backend_e::KSCREEN_DOCTOR, backend_e::NONE}) {
    EXPECT_FALSE(platf::host_virtual_display_needs_portal_for_backend(
      "host_virtual_display", true, backend
    )) << "a cage compositor owns its own capture source";

    for (const auto mode :
         {"windowed_stream", "headless_stream", "gamescope_stream", "desktop_display", "headless_dongle", ""}) {
      EXPECT_FALSE(platf::host_virtual_display_needs_portal_for_backend(mode, false, backend))
        << mode;
    }
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

TEST(PortalGrabPolicyTests, AbandonedPreparationCannotCancelReplacementOrAdoptedCapture) {
  using namespace std::chrono_literals;
  const auto old_token = portal::install_prepared_cache_for_tests();
  const auto replacement = portal::install_prepared_cache_for_tests();
  std::optional<session_media::start_owner_t> start {session_media::begin_start()};
  auto stale_cleanup = std::async(std::launch::async, [&]() {
    portal::release_prepared_cache_for_tests(old_token);
  });
  // Entering teardown before rejecting a stale token would block on this
  // admitted start and cancel its portal request, even without releasing it.
  EXPECT_EQ(stale_cleanup.wait_for(1s), std::future_status::ready);
  start.reset();
  stale_cleanup.get();
  EXPECT_TRUE(portal::prepared_cache_present_for_tests());
  portal::adopt_prepared_cache_for_tests();
  portal::release_prepared_cache_for_tests(replacement);
  EXPECT_TRUE(portal::prepared_cache_present_for_tests());
  portal::release_global_capture();
  const auto abandoned = portal::install_prepared_cache_for_tests();
  portal::release_prepared_cache_for_tests(abandoned);
  EXPECT_FALSE(portal::prepared_cache_present_for_tests());
}

TEST(PortalGrabPolicyTests, OnlyAuthorizedOwnerCanCancelBeforePortalRegistration) {
  proc::proc_t process;
  auto launch = std::make_shared<rtsp_stream::launch_session_t>();
  launch->unique_id = "portal-owner";
  launch->session_token = "portal-token";
  process.set_active_launch_for_tests(proc::ctx_t {}, launch);
  const auto owner_tag = process.capture_preparation_owner_for_tests();
  EXPECT_FALSE(process.cancel_capture_preparation_for_shutdown("other-owner", "portal-token", true, true));
  EXPECT_TRUE(process.cancel_capture_preparation_for_shutdown("PORTAL-OWNER", "stale-token", true, true));
  EXPECT_FALSE(process.cancel_capture_preparation_for_shutdown("portal-owner", "portal-token", false, true));
  EXPECT_FALSE(session_media::pending_start_cancelled(owner_tag));
  {
    auto cancel = process.cancel_capture_preparation_for_shutdown("portal-owner", "portal-token", true, true);
    ASSERT_TRUE(cancel);
    EXPECT_TRUE(session_media::pending_start_cancelled(owner_tag));
    session_media::pending_start_owner_scope_t owner {owner_tag};
    video::config_t config {};
    config.width = 1280;
    config.height = 720;
    config.capture_generation.capture_backend = "portal";
    config.capture_generation.stream_mode = "desktop_display";
    std::shared_ptr<void> preparation;
    EXPECT_FALSE(portal::prepare_capture(platf::mem_type_e::system, config, preparation));
    EXPECT_FALSE(preparation);
  }
  EXPECT_FALSE(session_media::pending_start_cancelled(owner_tag));
  // The legacy cancel protocol permits the owning certificate's stale token.
  EXPECT_TRUE(process.cancel_capture_preparation_for_shutdown("portal-owner", "stale-token", true, false));
  auto old_fence = process.cancel_capture_preparation_for_shutdown("portal-owner", "portal-token", true, true);
  const auto generation = process.capture_session_launch_generation();
  ASSERT_TRUE(generation);
  ASSERT_TRUE(process.try_begin_session_launch(*generation));
  const auto next_owner = process.capture_preparation_owner_for_tests();
  EXPECT_NE(next_owner, owner_tag);
  EXPECT_FALSE(session_media::pending_start_cancelled(next_owner));
  // The new admission has not installed its launch metadata yet. An old owner
  // cannot use that gap to cancel the replacement's pending capture.
  EXPECT_FALSE(process.cancel_capture_preparation_for_shutdown("portal-owner", "portal-token", true, true));
  process.finish_session_launch();
}

TEST(PortalGrabPolicyTests, StopQueueNeverDropsOutOfOrderPreparedRetirements) {
  using namespace std::chrono_literals;
  const auto stale_first = portal::install_prepared_cache_for_tests();
  const auto stale_second = portal::install_prepared_cache_for_tests();
  const auto current = portal::install_prepared_cache_for_tests();
  auto blocked = std::make_shared<std::promise<void>>();
  auto release = std::make_shared<std::promise<void>>();
  auto finished = std::make_shared<std::promise<void>>();
  auto resume = release->get_future().share();
  auto unblock = util::fail_guard([release]() { release->set_value(); });
  session_media::schedule_retirement([blocked, resume]() {
    blocked->set_value();
    resume.wait();
  });
  ASSERT_EQ(blocked->get_future().wait_for(5s), std::future_status::ready);
  session_media::schedule([]() {});
  session_media::schedule_retirement([stale_first]() { portal::release_prepared_cache_for_tests(stale_first); });
  session_media::schedule_retirement([current]() { portal::release_prepared_cache_for_tests(current); });
  session_media::schedule_retirement([stale_second]() { portal::release_prepared_cache_for_tests(stale_second); });
  session_media::schedule([]() {});
  session_media::schedule_retirement([finished]() { finished->set_value(); });
  release->set_value();
  unblock.disable();
  auto done = finished->get_future();
  ASSERT_EQ(done.wait_for(5s), std::future_status::ready);
  EXPECT_FALSE(portal::prepared_cache_present_for_tests());
}

TEST(PortalGrabPolicyTests, LaunchPreparationSkipsPrivateExactAndInputOnlyCapture) {
  std::shared_ptr<void> preparation;
  video::config_t config {};
  for (const auto mode : {"headless_stream", "windowed_stream", "gamescope_stream", "host_virtual_display", "desktop_takeover"}) {
    config.capture_generation.stream_mode = mode;
    EXPECT_TRUE(video::prepare_capture_for_launch(config, preparation));
    EXPECT_FALSE(preparation);
  }
  config.capture_generation.stream_mode = "desktop_display";
  config.input_only = true;
  EXPECT_TRUE(video::prepare_capture_for_launch(config, preparation));
  config.input_only = false;
  config.capture_generation.exact_display_name = "generation-owned-output";
  EXPECT_TRUE(video::prepare_capture_for_launch(config, preparation));
  EXPECT_FALSE(preparation);
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

  const auto ensure_begin = grab_source.find("static bool ensure_session_unlocked(const capture_generation::identity_t &generation)");
  const auto ensure_end = grab_source.find(
    "static std::shared_ptr<pipewire_capture::capture_t> ensure_global_capture(",
    ensure_begin
  );
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
  EXPECT_NE(grab.find("ensure_session_unlocked(generation)"), std::string::npos);
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
  EXPECT_NE(fn.find("ensure_session_unlocked(generation)"), std::string::npos);
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

TEST(PipeWireCapturePolicyTests, DmaBufEligibilityRequiresSupportedEncoderAndExplicitMatchingGpuPath) {
  const pipewire_capture::dmabuf_eligibility_t eligible {
    .capture_render_node = "/dev/dri/renderD128",
    .encoder_render_node = "/dev/dri/renderD128",
    .mem_type = platf::mem_type_e::cuda,
    .encoder_import_supported = true,
    .egl_import_supported = true,
  };

  EXPECT_TRUE(pipewire_capture::may_offer_dmabuf(eligible));
  EXPECT_FALSE(pipewire_capture::may_offer_dmabuf(eligible, pipewire_capture::dmabuf_override_e::force_cpu));

  auto vaapi = eligible;
  vaapi.mem_type = platf::mem_type_e::vaapi;
  EXPECT_FALSE(pipewire_capture::may_offer_dmabuf(vaapi));
  EXPECT_TRUE(pipewire_capture::may_offer_dmabuf(vaapi, pipewire_capture::dmabuf_override_e::allow_vaapi));
  EXPECT_FALSE(pipewire_capture::may_offer_dmabuf(vaapi, pipewire_capture::dmabuf_override_e::force_cpu));

  auto vaapi_missing_capture = vaapi;
  vaapi_missing_capture.capture_render_node.reset();
  EXPECT_FALSE(pipewire_capture::may_offer_dmabuf(vaapi_missing_capture, pipewire_capture::dmabuf_override_e::allow_vaapi));

  auto vaapi_mismatched = vaapi;
  vaapi_mismatched.encoder_render_node = "/dev/dri/renderD129";
  EXPECT_FALSE(pipewire_capture::may_offer_dmabuf(vaapi_mismatched, pipewire_capture::dmabuf_override_e::allow_vaapi));

  auto vaapi_no_egl = vaapi;
  vaapi_no_egl.egl_import_supported = false;
  EXPECT_FALSE(pipewire_capture::may_offer_dmabuf(vaapi_no_egl, pipewire_capture::dmabuf_override_e::allow_vaapi));

  auto missing_capture = eligible;
  missing_capture.capture_render_node.reset();
  EXPECT_FALSE(pipewire_capture::may_offer_dmabuf(missing_capture));

  auto mismatched = eligible;
  mismatched.encoder_render_node = "/dev/dri/renderD129";
  EXPECT_FALSE(pipewire_capture::may_offer_dmabuf(mismatched));

  auto system_memory = eligible;
  system_memory.mem_type = platf::mem_type_e::system;
  EXPECT_FALSE(pipewire_capture::may_offer_dmabuf(system_memory));
  EXPECT_FALSE(pipewire_capture::may_offer_dmabuf(system_memory, pipewire_capture::dmabuf_override_e::allow_vaapi));

  auto no_egl = eligible;
  no_egl.egl_import_supported = false;
  EXPECT_FALSE(pipewire_capture::may_offer_dmabuf(no_egl));

  auto no_encoder_import = eligible;
  no_encoder_import.encoder_import_supported = false;
  EXPECT_FALSE(pipewire_capture::may_offer_dmabuf(no_encoder_import));

  auto noncanonical = eligible;
  noncanonical.capture_render_node = "renderD128";
  noncanonical.encoder_render_node = "renderD128";
  EXPECT_FALSE(pipewire_capture::may_offer_dmabuf(noncanonical));
}

TEST(PipeWireCapturePolicyTests, DmaBufEnvironmentOverrideRequiresAnExactZeroOrOne) {
  using enum pipewire_capture::dmabuf_override_e;

  EXPECT_EQ(pipewire_capture::dmabuf_override_from_env(nullptr), default_safe);
  EXPECT_EQ(pipewire_capture::dmabuf_override_from_env(""), default_safe);
  EXPECT_EQ(pipewire_capture::dmabuf_override_from_env("0"), force_cpu);
  EXPECT_EQ(pipewire_capture::dmabuf_override_from_env("1"), allow_vaapi);
  EXPECT_EQ(pipewire_capture::dmabuf_override_from_env("00"), default_safe);
  EXPECT_EQ(pipewire_capture::dmabuf_override_from_env("01"), default_safe);
  EXPECT_EQ(pipewire_capture::dmabuf_override_from_env("true"), default_safe);
  EXPECT_EQ(pipewire_capture::dmabuf_override_from_env(" 1"), default_safe);
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

TEST(PipeWireRateTests, CompositorVersionUsesTheReleasedKwinBoundary) {
  for (const auto version : {"", "KWin version: 5.27.12", "KWin version: 6.7.0", "KWin version: invalid", "KWin version: 6.8"}) {
    EXPECT_FALSE(pipewire_capture::kwin_uses_fixed_rate(version)) << version;
  }
  for (const auto version : {"KWin version: 6.8.0", "KWin version: 6.9.1\n", "KWin version: 7.0.0"}) {
    EXPECT_TRUE(pipewire_capture::kwin_uses_fixed_rate(version)) << version;
  }
}

TEST(PipeWireRateTests, NegotiatedVariableAndFasterRatesNeedPacingWhileSlowerRatesStayEventDriven) {
  const AVRational requested {60000, 1001};
  EXPECT_TRUE(pipewire_capture::requires_host_pacing(requested, {0, 1}));
  EXPECT_TRUE(pipewire_capture::requires_host_pacing(requested, {120, 1}));
  EXPECT_FALSE(pipewire_capture::requires_host_pacing(requested, requested));
  EXPECT_FALSE(pipewire_capture::requires_host_pacing(requested, {30, 1}));
  EXPECT_FALSE(pipewire_capture::requires_host_pacing({0, 1}, {120, 1}));
  EXPECT_EQ(av_cmp_q(pipewire_capture::negotiated_capture_rate({0, 1}, {60000, 1001}), requested), 0);
  EXPECT_EQ(av_cmp_q(pipewire_capture::negotiated_capture_rate({30, 1}, {0, 0}), AVRational {30, 1}), 0);
  EXPECT_EQ(av_cmp_q(pipewire_capture::negotiated_capture_rate({30, 1}, {0, 1}), AVRational {30, 1}), 0);
  EXPECT_FALSE(video::rate::valid(pipewire_capture::negotiated_capture_rate({0, 0}, {UINT32_MAX, 1})));
}

namespace pipewire_capture {
  struct capture_test_access {
    static bool local_stream(capture_t &capture) {
      pw_init(nullptr, nullptr);
      capture.loop_ = pw_thread_loop_new("polaris-rate-retry-test", nullptr);
      if (!capture.loop_) return false;
      capture.context_ = pw_context_new(pw_thread_loop_get_loop(capture.loop_), nullptr, 0);
      if (!capture.context_) return false;
      // A real in-process core; no user daemon, portal or external producer.
      capture.core_ = pw_context_connect_self(capture.context_, nullptr, 0);
      if (!capture.core_) return false;
      capture.stream_ = pw_stream_new(capture.core_, "polaris-rate-test", pw_properties_new(nullptr, nullptr));
      capture.stream_state_ = PW_STREAM_STATE_ERROR;
      capture.terminal_result_ = wait_result_e::error;
      return capture.stream_ != nullptr;
    }
    static pw_thread_loop *loop(capture_t &capture) { return capture.loop_; }
    static pw_stream *stream(capture_t &capture) { return capture.stream_; }
    static bool retry_entered(capture_t &capture) {
      if (!capture.shutdown_mtx_.try_lock()) return true;
      capture.shutdown_mtx_.unlock();
      return false;
    }
    static void running(capture_t &capture) {
      std::lock_guard lock(capture.frame_mtx_);
      capture.running_ = true;
      capture.stream_state_ = PW_STREAM_STATE_STREAMING;
      capture.terminal_result_ = wait_result_e::timeout;
    }
    static void ready(capture_t &capture, AVRational negotiated) {
      std::lock_guard lock(capture.frame_mtx_);
      capture.front_info_ = {.width = 1, .height = 1, .stride = 4, .spa_format = SPA_VIDEO_FORMAT_BGRx};
      capture.front_frame_.assign(4, 0);
      capture.frame_available_ = true;
      capture.negotiated_ = true;
      capture.negotiated_rate_ = negotiated;
      capture.frame_cv_.notify_all();
    }
  };
}

TEST(PipeWireRateTests, ActualFrameWaitStaysEventDrivenWhenProducerMeetsTheRate) {
  auto capture = std::make_shared<pipewire_capture::capture_t>(pipewire_capture::capture_options_t {.requested_rate = {2, 1}});
  for (int frame = 0; frame < 3; ++frame) {
    pipewire_capture::capture_test_access::ready(*capture, {1, 1});
    EXPECT_EQ(capture->wait_for_frame(std::chrono::milliseconds(10)), pipewire_capture::wait_result_e::frame);
  }
}

TEST(PipeWireRateTests, StopInterruptsActualPacedFrameWait) {
  auto capture = std::make_shared<pipewire_capture::capture_t>(pipewire_capture::capture_options_t {.requested_rate = {1, 1}});
  pipewire_capture::capture_test_access::ready(*capture, {0, 1});
  ASSERT_EQ(capture->wait_for_frame(std::chrono::milliseconds(10)), pipewire_capture::wait_result_e::frame);
  auto waiting = std::async(std::launch::async, [&] { return capture->wait_for_frame(std::chrono::seconds(2)); });
  EXPECT_EQ(waiting.wait_for(std::chrono::milliseconds(50)), std::future_status::timeout);
  capture->stop();
  ASSERT_EQ(waiting.wait_for(std::chrono::seconds(1)), std::future_status::ready);
  EXPECT_EQ(waiting.get(), pipewire_capture::wait_result_e::reinit);
  EXPECT_FALSE(capture->retry_rate_negotiation());
}

TEST(PipeWireRateTests, KwinLookupCancellationUsesTheActualPendingStartOwner) {
  int owner_a, owner_b;
  std::promise<void> entered;
  auto query = std::async(std::launch::async, [&] {
    session_media::pending_start_owner_scope_t owner(&owner_a);
    return portal::kwin_rate_query_with_hook_for_tests([&](GCancellable *cancellable) {
      GPollFD fd {};
      ASSERT_TRUE(g_cancellable_make_pollfd(cancellable, &fd));
      auto release_fd = util::fail_guard([&] { g_cancellable_release_fd(cancellable); });
      entered.set_value();
      EXPECT_EQ(g_poll(&fd, 1, 1500), 1);
      EXPECT_TRUE(g_cancellable_is_cancelled(cancellable));
    });
  });
  entered.get_future().wait();
  portal::cancel_pending_requests(&owner_b);
  EXPECT_EQ(query.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);
  portal::cancel_pending_requests(&owner_a);
  ASSERT_EQ(query.wait_for(std::chrono::seconds(1)), std::future_status::ready);
  EXPECT_FALSE(query.get());
}

TEST(PipeWireRateTests, KwinLookupDeadlineAlsoCoversBusAcquisition) {
  const auto started = std::chrono::steady_clock::now();
  EXPECT_FALSE(portal::kwin_rate_query_with_hook_for_tests([&](GCancellable *cancellable) {
    GPollFD fd {};
    ASSERT_TRUE(g_cancellable_make_pollfd(cancellable, &fd));
    auto release_fd = util::fail_guard([&] { g_cancellable_release_fd(cancellable); });
    EXPECT_EQ(g_poll(&fd, 1, 1500), 1);
    EXPECT_TRUE(g_cancellable_is_cancelled(cancellable));
  }));
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(2));
}

TEST(PipeWireRateTests, RetryRechecksStopAndNegotiationAfterWaitingForTheActualLoop) {
  using access = pipewire_capture::capture_test_access;
  for (const bool stop : {true, false}) {
    auto capture = std::make_shared<pipewire_capture::capture_t>(pipewire_capture::capture_options_t {.requested_rate = {60, 1}});
    ASSERT_TRUE(access::local_stream(*capture));
    const auto original = access::stream(*capture);
    auto loop = access::loop(*capture);
    pw_thread_loop_lock(loop);
    auto retry = std::async(std::launch::async, [&] { return capture->retry_rate_negotiation(); });
    auto unlock = util::fail_guard([&] { pw_thread_loop_unlock(loop); });
    for (int i = 0; i < 1000 && !access::retry_entered(*capture); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    ASSERT_TRUE(access::retry_entered(*capture));
    EXPECT_EQ(retry.wait_for(std::chrono::milliseconds(10)), std::future_status::timeout);
    if (stop) capture->stop();
    else access::ready(*capture, {60, 1});
    pw_thread_loop_unlock(loop);
    unlock.disable();
    ASSERT_EQ(retry.wait_for(std::chrono::seconds(1)), std::future_status::ready);
    EXPECT_FALSE(retry.get());
    EXPECT_EQ(access::stream(*capture), original);
  }
}

TEST(PipeWireRateTests, OwnerCancellationPreventsRetryWhileWaitingForTheActualLoop) {
  using access = pipewire_capture::capture_test_access;
  int owner;
  auto capture = std::make_shared<pipewire_capture::capture_t>(pipewire_capture::capture_options_t {.requested_rate = {60, 1}});
  ASSERT_TRUE(access::local_stream(*capture));
  const auto original = access::stream(*capture);
  auto loop = access::loop(*capture);
  pw_thread_loop_lock(loop);
  auto waiting = std::async(std::launch::async, [&] {
    session_media::pending_start_owner_scope_t scope(&owner);
    auto start = session_media::begin_start();
    return portal::wait_for_capture_negotiation_for_tests(capture);
  });
  auto unlock = util::fail_guard([&] { pw_thread_loop_unlock(loop); });
  for (int i = 0; i < 1000 && !access::retry_entered(*capture); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(1));
  ASSERT_TRUE(access::retry_entered(*capture));
  auto cancellation = session_media::cancel_pending_starts(&owner);
  pw_thread_loop_unlock(loop);
  unlock.disable();
  ASSERT_EQ(waiting.wait_for(std::chrono::seconds(1)), std::future_status::ready);
  EXPECT_FALSE(waiting.get());
  EXPECT_FALSE(capture->running());
  EXPECT_EQ(access::stream(*capture), original);
}

TEST(PipeWireRateTests, TeardownCancelsNegotiationBeforeWaitingForTheStartOwner) {
  auto capture = std::make_shared<pipewire_capture::capture_t>(pipewire_capture::capture_options_t {.requested_rate = {60, 1}});
  pipewire_capture::capture_test_access::running(*capture);
  std::promise<void> admitted;
  auto waiting = std::async(std::launch::async, [&] {
    auto start = session_media::begin_start();
    admitted.set_value();
    return portal::wait_for_capture_negotiation_for_tests(capture);
  });
  admitted.get_future().wait();
  EXPECT_EQ(waiting.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);
  auto teardown = std::async(std::launch::async, [] { return session_media::begin_teardown(); });
  ASSERT_EQ(waiting.wait_for(std::chrono::seconds(1)), std::future_status::ready);
  EXPECT_FALSE(waiting.get());
  EXPECT_FALSE(capture->running());
  ASSERT_EQ(teardown.wait_for(std::chrono::seconds(1)), std::future_status::ready);
  auto fence = teardown.get();
}

TEST(PipeWireRateTests, ActualSpaOffersKeepVariableFixedAndLegacyNegotiationDistinct) {
  for (const bool fixed : {true, false}) {
    for (const bool use_maximum : {true, false}) {
      alignas(8) std::array<uint8_t, 1024> bytes {};
      spa_pod_builder builder = SPA_POD_BUILDER_INIT(bytes.data(), bytes.size());
      spa_pod_frame frame;
      spa_pod_builder_push_object(&builder, &frame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
      pipewire_capture::append_rate_properties(&builder, {60000, 1001}, fixed, use_maximum);
      auto pod = static_cast<spa_pod *>(spa_pod_builder_pop(&builder, &frame));
      auto fps = spa_pod_find_prop(pod, nullptr, SPA_FORMAT_VIDEO_framerate);
      ASSERT_NE(fps, nullptr);
      spa_fraction value {};
      ASSERT_EQ(spa_pod_get_fraction(&fps->value, &value), 0);
      EXPECT_EQ(value.num, fixed && !use_maximum ? 60000u : 0u);
      EXPECT_EQ(value.denom, fixed && !use_maximum ? 1001u : 1u);
      auto maximum = spa_pod_find_prop(pod, nullptr, SPA_FORMAT_VIDEO_maxFramerate);
      if (!use_maximum) { EXPECT_EQ(maximum, nullptr); continue; }
      ASSERT_NE(maximum, nullptr);
      auto choice = reinterpret_cast<const spa_pod_choice *>(&maximum->value);
      ASSERT_EQ(SPA_POD_CHOICE_TYPE(choice), SPA_CHOICE_Range);
      ASSERT_EQ(SPA_POD_CHOICE_N_VALUES(choice), 3u);
      auto values = static_cast<const spa_fraction *>(SPA_POD_CHOICE_VALUES(choice));
      EXPECT_EQ(values[0].num, fixed ? 60000u : 0u);
      EXPECT_EQ(values[0].denom, fixed ? 1001u : 1u);
      EXPECT_EQ(values[1].num, 0u);
      EXPECT_EQ(values[2].num, 1000u);
    }
  }
}

TEST(PipeWireRateTests, ActualSpaIntersectionPrefersVariableAndAcceptsFixedOnlyProducers) {
  for (const bool reverse : {false, true}) {
    for (const bool compatibility : {false, true}) {
      for (const unsigned producer_rate : {0u, 60u, 120u}) {
        for (const bool producer_has_variable : {false, true}) {
          alignas(8) std::array<uint8_t, 1024> consumer_bytes {}, producer_bytes {}, result_bytes {};
          spa_pod_builder consumer_builder = SPA_POD_BUILDER_INIT(consumer_bytes.data(), consumer_bytes.size());
          spa_pod_frame frame {};
          spa_pod_builder_push_object(&consumer_builder, &frame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
          pipewire_capture::append_rate_properties(&consumer_builder, {60, 1}, false, true, compatibility);
          auto consumer = static_cast<spa_pod *>(spa_pod_builder_pop(&consumer_builder, &frame));

          spa_pod_builder producer_builder = SPA_POD_BUILDER_INIT(producer_bytes.data(), producer_bytes.size());
          spa_pod_builder_push_object(&producer_builder, &frame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
          const spa_fraction fixed {producer_rate, 1}, variable {0, 1};
          if (producer_has_variable) {
            // The producer prefers fixed capture, but also supports variable.
            spa_pod_builder_add(&producer_builder, SPA_FORMAT_VIDEO_framerate,
              SPA_POD_CHOICE_ENUM_Fraction(3, &fixed, &fixed, &variable), 0);
          } else {
            spa_pod_builder_add(&producer_builder, SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&fixed), 0);
          }
          auto producer = static_cast<spa_pod *>(spa_pod_builder_pop(&producer_builder, &frame));
          spa_pod_builder result_builder = SPA_POD_BUILDER_INIT(result_bytes.data(), result_bytes.size());
          spa_pod *result = nullptr;
          const int filtered = spa_pod_filter(&result_builder, &result,
            reverse ? producer : consumer, reverse ? consumer : producer);
          if (!compatibility && producer_rate != 0 && !producer_has_variable) {
            EXPECT_LT(filtered, 0);
            continue;
          }
          ASSERT_EQ(filtered, 0);
          ASSERT_NE(result, nullptr);
          ASSERT_EQ(spa_pod_fixate(result), 0);
          const auto property = spa_pod_find_prop(result, nullptr, SPA_FORMAT_VIDEO_framerate);
          ASSERT_NE(property, nullptr);
          spa_fraction rate {};
          uint32_t values = 0, choice = 0;
          const auto selected = spa_pod_get_values(&property->value, &values, &choice);
          ASSERT_NE(selected, nullptr);
          ASSERT_EQ(choice, SPA_CHOICE_None);
          ASSERT_EQ(spa_pod_get_fraction(selected, &rate), 0);
          if (!compatibility) { EXPECT_EQ(rate.num, 0u); }
          if (!producer_has_variable) { EXPECT_EQ(rate.num, producer_rate); }
          EXPECT_EQ(rate.denom, 1u);
          if (compatibility) { EXPECT_EQ(spa_pod_find_prop(consumer, nullptr, SPA_FORMAT_VIDEO_maxFramerate), nullptr); }
        }
      }
    }
  }
}

TEST(PipeWireLiveProducerTests, NegotiatesAndPacesAnIsolatedSyntheticProducer) {
  const auto test_case = std::getenv("POLARIS_TEST_PIPEWIRE_CASE");
  if (!test_case) GTEST_SKIP() << "Run tools/tests/pipewire_rate_harness.py with this test binary";
  const auto parameters = nlohmann::json::parse(test_case);
  const AVRational requested {parameters.at("numerator").get<int>(), parameters.at("denominator").get<int>()};
  ASSERT_TRUE(video::rate::valid(requested));
  auto capture = std::make_shared<pipewire_capture::capture_t>(pipewire_capture::capture_options_t {
    .node_id = parameters.at("node").get<uint32_t>(),
    .requested_width = 64,
    .requested_height = 64,
    .requested_rate = requested,
  });
  ASSERT_TRUE(capture->start());
  for (int i = 0; i < 100 && !capture->negotiated(); ++i) {
    if (!capture->running() && !capture->retry_rate_negotiation()) break;
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  ASSERT_TRUE(capture->negotiated());
  ASSERT_FALSE(capture->negotiated_dmabuf());
  const auto info = capture->frame_info();
  ASSERT_EQ(info.width, 64);
  ASSERT_EQ(info.height, 64);
  std::vector<uint8_t> pixels(64 * 64 * 4);
  auto image = std::make_shared<platf::img_t>();
  image->width = image->height = 64;
  image->row_pitch = 64 * 4;
  image->pixel_pitch = 4;
  image->data = pixels.data();
  std::vector<double> intervals;
  std::optional<std::chrono::steady_clock::time_point> previous;
  for (int i = 0; i < 181; ++i) {
    ASSERT_EQ(capture->wait_for_frame(std::chrono::seconds(2)), pipewire_capture::wait_result_e::frame);
    ASSERT_TRUE(capture->fill_frame(image));
    const auto now = std::chrono::steady_clock::now();
    if (previous) intervals.push_back(std::chrono::duration<double>(now - *previous).count());
    previous = now;
  }
  double total = std::accumulate(intervals.begin(), intervals.end(), 0.0);
  const double measured = intervals.size() / total;
  const double expected = av_q2d(requested);
  EXPECT_GT(measured, expected * 0.8);
  EXPECT_LT(measured, expected * 1.05);
  std::sort(intervals.begin(), intervals.end());
  std::cout << "PIPEWIRE_RATE_RESULT " << nlohmann::json {
    {"requested_numerator", requested.num}, {"requested_denominator", requested.den},
    {"measured_fps", measured}, {"median_interval_ms", intervals[intervals.size() / 2] * 1000},
    {"p95_interval_ms", intervals[intervals.size() * 95 / 100] * 1000}, {"frames", 181}
  }.dump() << '\n';
  capture->stop();
  EXPECT_EQ(capture->wait_for_frame(std::chrono::seconds(1)), pipewire_capture::wait_result_e::reinit);
  capture->shutdown();
}
