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
