/**
 * @file tests/unit/platform/test_capture_backend_fallback_source.cpp
 * @brief Source guard: a configured capture backend that can capture nothing must fall back
 *        rather than leave Polaris with no capture at all.
 *
 * Capture backends are not interchangeable across compositors. wlr capture needs
 * zwlr_export_dmabuf_manager_v1, which only wlroots compositors have, and the stream mode decides
 * which compositor gets enumerated: a cage mode enumerates Polaris' own labwc and always works,
 * while every non-cage mode has to enumerate the host desktop. On KDE or GNOME that finds nothing,
 * and before this fallback existed the result was zero capture sources, no encoder probe, a
 * "Fatal: Unable to find display or encoder" line, and a host that carried on serving H.264 as
 * the only advertised codec (issue #677).
 *
 * Reproducing that needs a specific compositor, so the invariant is guarded at the source level,
 * the same way test_preallocated_gamepad_source.cpp guards its own.
 */
#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>

namespace {

  std::string read_misc_source() {
    const auto path = std::filesystem::path {POLARIS_SOURCE_DIR} / "src/platform/linux/misc.cpp";
    std::ifstream file {path};
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
  }

}  // namespace

TEST(CaptureBackendFallbackSource, AConfiguredBackendThatFindsNothingFallsBack) {
  const auto source = read_misc_source();
  ASSERT_FALSE(source.empty()) << "could not read misc.cpp via POLARIS_SOURCE_DIR";

  const auto entry = source.find("void reevaluate_capture_sources()");
  ASSERT_NE(entry, std::string::npos);
  const auto body = source.substr(entry);

  // The retry is the whole fix: evaluate again as if no backend were configured, which is what
  // auto-selection would have done and what would have worked on this host all along.
  EXPECT_NE(body.find("capture_backend_override = std::string {}"), std::string::npos)
    << "reevaluate_capture_sources no longer retries with auto-selection";
  EXPECT_NE(body.find("verified_action::confirm"), std::string::npos)
    << "a substituted capture backend is no longer recorded as a silent substitution";
}

TEST(CaptureBackendFallbackSource, TheEvaluationConsultsTheOverrideRatherThanTheConfig) {
  const auto source = read_misc_source();
  ASSERT_FALSE(source.empty());

  const auto entry = source.find("void evaluate_capture_sources()");
  ASSERT_NE(entry, std::string::npos);
  const auto end = source.find("void reevaluate_capture_sources()", entry);
  ASSERT_NE(end, std::string::npos);
  const auto body = source.substr(entry, end - entry);

  // Reading config::video.capture directly here would make the retry a no-op, silently.
  EXPECT_EQ(body.find("config::video.capture"), std::string::npos)
    << "evaluate_capture_sources reads the configuration directly again, so the retry cannot work";
  EXPECT_NE(body.find("requested_capture()"), std::string::npos);
}

namespace {

  std::string read_source(const char *relative) {
    const auto path = std::filesystem::path {POLARIS_SOURCE_DIR} / relative;
    std::ifstream file {path};
    std::ostringstream buffer;
    buffer << file.rdbuf();
    return buffer.str();
  }

  std::string between(const std::string &source, std::string_view begin, std::string_view end) {
    const auto start = source.find(begin);
    if (start == std::string::npos) {
      return {};
    }
    const auto stop = source.find(end, start);
    return source.substr(start, stop == std::string::npos ? std::string::npos : stop - start);
  }

}  // namespace

TEST(CaptureBackendFallbackSource, StreamsAskForTheBackendTheEvaluationSubstituted) {
  // #739: the substitution above only ever reached the encoder probe, which asks for auto. Every
  // capture generation still asked for the configured backend by name, found nothing, and the
  // stream died while the Doctor said the substitute was in use.
  const auto process = read_source("src/process.cpp");
  const auto video = read_source("src/video.cpp");
  ASSERT_FALSE(process.empty());
  ASSERT_FALSE(video.empty());

  EXPECT_EQ(process.find(".capture_backend = config::video.capture"), std::string::npos)
    << "the launch builds its capture generation from the saved preference again";
  EXPECT_NE(process.find(".capture_backend = stream_display_policy::capture_for_current_mode("), std::string::npos);

  const auto identity = between(video, "capture_generation::identity_t current_capture_generation_identity()", "std::optional<int> find_display_index(");
  ASSERT_FALSE(identity.empty());
  EXPECT_NE(identity.find("stream_display_policy::capture_for_current_mode()"), std::string::npos)
    << "a generation built without a launch asks for the saved preference again";
}

TEST(CaptureBackendFallbackSource, AHostModeChangeReevaluatesCaptureAndRetiresProbeReuse) {
  const auto nvhttp = read_source("src/nvhttp.cpp");
  ASSERT_FALSE(nvhttp.empty());

  const auto recheck = between(nvhttp, "void recheck_capture_for_host_mode_change(", "stream_display_mode_apply_result_e apply_stream_display_mode_selection(");
  ASSERT_FALSE(recheck.empty());
  EXPECT_NE(recheck.find("platf::reevaluate_capture_sources()"), std::string::npos);
  EXPECT_NE(recheck.find("video::invalidate_encoder_probe_reuse()"), std::string::npos);
  EXPECT_EQ(recheck.find("video::reset_encoder_probe_state()"), std::string::npos)
    << "dropping the chosen encoder here advertises H.264 alone until the next launch probes";
  EXPECT_NE(recheck.find("BOOST_LOG(info)"), std::string::npos)
    << "a host default changed from a client leaves no trace in the log again";

  const auto apply = between(nvhttp, "stream_display_mode_apply_result_e apply_stream_display_mode_selection(", "nlohmann::json build_client_settings_sync_status(");
  ASSERT_FALSE(apply.empty());
  EXPECT_NE(apply.find("const host_mode_changed_fn_t &host_mode_changed = recheck_capture_for_host_mode_change"), std::string::npos);
  EXPECT_NE(apply.find("host_mode_changed(previous_linux_display.stream_mode"), std::string::npos);
}

TEST(CaptureBackendFallbackSource, ALaunchReevaluatesAStaleListAndRefusesAnUnservableRequest) {
  const auto process = read_source("src/process.cpp");
  ASSERT_FALSE(process.empty());
  const auto launch = between(process, "int proc_t::execute_impl(", "void proc_t::terminate_impl(");
  ASSERT_FALSE(launch.empty());

  const auto stale = launch.find("platf::reevaluate_capture_sources_if_stale()");
  const auto generation = launch.find(".capture_backend = stream_display_policy::capture_for_current_mode(");
  const auto refusal = launch.find("video::refuse_launch_if_capture_unavailable(capture_generation)", generation);
  const auto probe = launch.find("video::probe_encoders(strict_session_encoder)");
  ASSERT_NE(stale, std::string::npos);
  ASSERT_NE(generation, std::string::npos);
  ASSERT_NE(refusal, std::string::npos);
  ASSERT_NE(probe, std::string::npos);
  EXPECT_LT(stale, generation) << "the generation must be built from a list evaluated for this mode";
  EXPECT_LT(refusal, probe) << "an unservable request must be refused before anything is probed";

  const auto cage = between(launch, "auto start_cage_session = [&]", "auto start_cage_with_runtime_fallback");
  ASSERT_FALSE(cage.empty());
  const auto reprobe = cage.find("reprobe_encoders_for_cage(strict_configured_encoder, save_successful_cache)");
  const auto cage_refusal = cage.find("video::refuse_launch_if_capture_unavailable(capture_generation)");
  ASSERT_NE(reprobe, std::string::npos);
  ASSERT_NE(cage_refusal, std::string::npos);
  EXPECT_LT(reprobe, cage_refusal);

  const auto misc = read_misc_source();
  const auto reevaluate = between(misc, "void reevaluate_capture_sources() {", "std::string capture_backend_substitution_note()");
  ASSERT_FALSE(reevaluate.empty());
  EXPECT_NE(reevaluate.find("capture_sources_evaluated_for = capture_sources_evaluation_key()"), std::string::npos)
    << "an evaluation no longer records the configuration it was built for";
}

TEST(CaptureBackendFallbackSource, TheEvaluationReadsAliasesTheWayDispatchDoes) {
  // Dispatch accepts kwin for portal and drm for kms. The evaluation compared only the literals, so
  // an alias enumerated nothing, got a substitute nobody needed, and a silent failure on the record.
  const auto misc = read_misc_source();
  const auto evaluate = between(misc, "void evaluate_capture_sources() {", "void reevaluate_capture_sources()");
  ASSERT_FALSE(evaluate.empty());
  EXPECT_NE(evaluate.find("canonical_capture_backend(requested_capture())"), std::string::npos);
  EXPECT_EQ(evaluate.find("requested_capture() == \""), std::string::npos)
    << "a literal comparison skips the aliases dispatch accepts";

  const auto refused = between(misc, "void note_kms_capture_refused_for_capability() {", "#ifdef POLARIS_TESTS");
  ASSERT_FALSE(refused.empty());
  EXPECT_NE(refused.find("canonical_capture_backend(config::video.capture) == \"kms\""), std::string::npos)
    << "a host set to drm asked for KMS as much as one set to kms";

  const auto reevaluate = between(misc, "void reevaluate_capture_sources() {", "std::string capture_backend_substitution_note()");
  ASSERT_FALSE(reevaluate.empty());
  EXPECT_NE(
    reevaluate.find("if (stream_display_policy::canonical_capture_backend(config::video.capture).empty()) {"),
    std::string::npos
  ) << "an auto host that found nothing is treated as a configured backend to substitute again";
  // The Doctor tells a kms host whose substitute came from a refused capability to run one command,
  // by the prefix of this note. Built from the raw value, a drm host got the advice to give up KMS.
  EXPECT_NE(
    reevaluate.find("capture_backend_substitution = stream_display_policy::canonical_capture_backend(requested) + \" -> \""),
    std::string::npos
  );
}

TEST(CaptureBackendFallbackSource, EachEvaluationSaysWhenTheModeSetsTheCaptureSettingAside) {
  // A private compositor mode streams through wlr whatever capture says, and the only trace was the
  // rewritten value in the per-open line. Every evaluation says it once, at warning.
  const auto misc = read_misc_source();
  const auto logger = between(misc, "void log_capture_mode_override() {", "void reevaluate_capture_sources() {");
  ASSERT_FALSE(logger.empty());
  EXPECT_NE(logger.find("capture_mode_override_for_current_mode("), std::string::npos);
  EXPECT_NE(logger.find("BOOST_LOG(warning)"), std::string::npos);

  const auto reevaluate = between(misc, "void reevaluate_capture_sources() {", "std::string capture_backend_substitution_note()");
  ASSERT_FALSE(reevaluate.empty());
  EXPECT_NE(reevaluate.find("log_capture_mode_override"), std::string::npos);
  // The substitution warning is the only line that speaks for a substituted override, so it names
  // the mode that could not capture anything.
  EXPECT_NE(reevaluate.find("\" cannot capture anything in stream mode [\"sv"), std::string::npos)
    << "the substitution warning says only the current stream mode again";

  const auto choice = between(misc, "void log_display_backend_choice(display_backend_e backend", "std::string describe_selected_sources()");
  ASSERT_FALSE(choice.empty());
  EXPECT_NE(choice.find("configured=["), std::string::npos) << "the per-open line names only the rewritten request";
  EXPECT_NE(choice.find("mode=["), std::string::npos);
  // By the time a display opens, Host Virtual Display, a session transition or Game Mode may have
  // rewritten the live setting. configured is polaris.conf as loaded, and live sits beside it.
  EXPECT_NE(choice.find("stream_display_policy::loaded_capture_setting()"), std::string::npos)
    << "configured names the live, rewritten setting again, which hides the rewrite it is there to show";
  EXPECT_NE(choice.find("live=["), std::string::npos);
}

TEST(CaptureBackendFallbackSource, EachCaptureRewriteLineReachesTheLogWhenItHolds) {
  // polaris.conf is parsed before logging starts, so a line the load logs reaches stdout only. The
  // load keeps it, and main() says it after logging::init and after the config lines it explains.
  const auto main_source = read_source("src/main.cpp");
  ASSERT_FALSE(main_source.empty());
  const auto init = main_source.find("logging::init(config::sunshine.min_log_level");
  const auto settings = main_source.find("config::modified_config_settings.clear();", init);
  const auto notes = main_source.find("stream_display_policy::log_config_load_notes();");
  ASSERT_NE(init, std::string::npos);
  ASSERT_NE(settings, std::string::npos);
  ASSERT_NE(notes, std::string::npos) << "what the load does to capture never reaches polaris.log";
  EXPECT_LT(init, notes);
  EXPECT_LT(settings, notes);

  // A client's mode switch puts capture back itself, and the launch that enters the mode speaks.
  const auto nvhttp = read_source("src/nvhttp.cpp");
  const auto apply = between(nvhttp, "stream_display_mode_apply_result_e apply_stream_display_mode_selection(", "nlohmann::json build_client_settings_sync_status(");
  ASSERT_FALSE(apply.empty());
  EXPECT_NE(apply.find("stream_display_policy::capture_rewrite_scope_e::preview"), std::string::npos)
    << "a mode switch warns that capture holds a value the switch puts back a few lines later";

  // The actuator corrects the launch's own rewrite when the display came up on another backend.
  const auto process = read_source("src/process.cpp");
  const auto actuator = between(process, "auto vdisplay = virtual_display::create(", "this->initial_linux_display_saved = true;");
  ASSERT_FALSE(actuator.empty());
  EXPECT_NE(actuator.find("stream_display_policy::capture_rewrite_scope_e::backend_change"), std::string::npos)
    << "the actuator names the launch's replacement as the operator's configured backend again";
}

TEST(CaptureBackendFallbackSource, ASessionThatDiscardsAnExplicitCaptureSaysSoAsAWarning) {
  // A Gamescope or dongle session forces portal over an explicit kms, and Mirror Desktop clears an
  // explicit wlr. Both were logged at info, beside ordinary auto fills. The launch hands the rewrite
  // to the policy, whose test holds the warning and what it names as configured.
  const auto process = read_source("src/process.cpp");
  const auto transition = between(
    process,
    "if (stream_display_policy::apply_selection(session_mode, mode_error)) {",
    "this->initial_linux_display_saved = true;"
  );
  ASSERT_FALSE(transition.empty());
  EXPECT_NE(
    transition.find("stream_display_policy::apply_capture_for_session_transition(configured_session_mode, session_mode);"),
    std::string::npos
  ) << "the launch no longer rewrites capture for the session it enters";
  EXPECT_EQ(transition.find("config::video.capture ="), std::string::npos)
    << "the launch rewrites capture itself again, measured against the live value";
}

TEST(CaptureBackendFallbackSource, TheAliasReadersReadCaptureTheWayDispatchDoes) {
  // kwin opens the portal and drm opens KMS. These readers compared the setting with a literal, so
  // a kwin dongle kept HDR on while it captured SDR through the portal and blanked the desk the
  // portal picker needs, a kwin Gamescope Stream host
  // let the portal's dummy probe take Main10 away, and a kwin host fell through the runtime label.
  // Each rule is tested where it lives; this holds the call sites to it.
  const auto topology = read_source("src/platform/linux/display_topology.cpp");
  const auto prepare = between(topology, "void prepare_for_stream(int width, int height, int refresh_hz) {", "void restore_after_stream() {");
  ASSERT_FALSE(prepare.empty());
  EXPECT_NE(prepare.find("const bool portal_capture = captures_through_host_portal(config::video.capture);"), std::string::npos);
  EXPECT_EQ(prepare.find("config::video.capture =="), std::string::npos);

  const auto video = read_source("src/video.cpp");
  ASSERT_FALSE(video.empty());
  EXPECT_NE(video.find("const bool main10_probe_is_authoritative = video::main10_probe_is_authoritative("), std::string::npos);
  EXPECT_EQ(video.find("config::video.capture == \"portal\""), std::string::npos);

  const auto browser = read_source("src/browser_stream.cpp");
  const auto runtime = between(browser, "std::string private_runtime_backend() {", "nlohmann::json fill_isolation_json(");
  ASSERT_FALSE(runtime.empty());
  EXPECT_NE(runtime.find("stream_display_policy::canonical_capture_backend(config::video.capture) == \"portal\""), std::string::npos);
  EXPECT_EQ(runtime.find("config::video.capture == \""), std::string::npos);
}
