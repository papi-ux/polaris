/**
 * @file tests/unit/test_launch_response_status.cpp
 * @brief The /launch and /resume handlers must never ship a response with no
 *        status_code, even when the handler throws mid-flight after partially
 *        filling the response tree. This is the host-side companion to nova
 *        #225, which stopped the client crashing (NPE) on such a body.
 */

#include <src/config.h>
#include <src/launch_failure.h>
#include <src/nvhttp.h>
#include <src/platform/common.h>
#include <src/rtsp.h>
#include <src/video.h>

#include <boost/property_tree/ptree.hpp>
#include <atomic>
#include <optional>
#include <fstream>
#include <sstream>
#include <gtest/gtest.h>

namespace pt = boost::property_tree;

TEST(LaunchResponseStatus, FillsMissingStatusCodeOnAPartiallyBuiltTree) {
  // The shape a thrown launch leaves behind: launchPolicy already in the tree,
  // no status_code yet — exactly the malformed <root> nova #225 had to survive.
  pt::ptree tree;
  tree.put("root.launchPolicy.mode", "headless_stream");

  nvhttp::ensure_response_status_code_for_tests(tree, 500, "The launch failed unexpectedly");

  EXPECT_EQ(tree.get<int>("root.<xmlattr>.status_code"), 500);
  EXPECT_EQ(tree.get<std::string>("root.<xmlattr>.status_message"), "The launch failed unexpectedly");
  // Content already in the tree is left untouched.
  EXPECT_EQ(tree.get<std::string>("root.launchPolicy.mode"), "headless_stream");
}

TEST(LaunchResponseStatus, FillsMissingStatusCodeOnAnEmptyTree) {
  pt::ptree tree;

  nvhttp::ensure_response_status_code_for_tests(tree, 500, "boom");

  EXPECT_EQ(tree.get<int>("root.<xmlattr>.status_code"), 500);
  EXPECT_EQ(tree.get<std::string>("root.<xmlattr>.status_message"), "boom");
}

TEST(LaunchResponseStatus, PreservesAnExplicitSuccessStatus) {
  pt::ptree tree;
  tree.put("root.<xmlattr>.status_code", 200);
  tree.put("root.<xmlattr>.status_message", "OK");

  nvhttp::ensure_response_status_code_for_tests(tree, 500, "should not overwrite");

  EXPECT_EQ(tree.get<int>("root.<xmlattr>.status_code"), 200);
  EXPECT_EQ(tree.get<std::string>("root.<xmlattr>.status_message"), "OK");
}

TEST(LaunchResponseStatus, PreservesAnExplicitErrorStatus) {
  // A handler that already decided on a specific error (e.g. 403 permission
  // denied) must keep it, not be flattened to the generic 500 fallback.
  pt::ptree tree;
  tree.put("root.<xmlattr>.status_code", 403);
  tree.put("root.<xmlattr>.status_message", "Permission denied");

  nvhttp::ensure_response_status_code_for_tests(tree, 500, "should not overwrite");

  EXPECT_EQ(tree.get<int>("root.<xmlattr>.status_code"), 403);
  EXPECT_EQ(tree.get<std::string>("root.<xmlattr>.status_message"), "Permission denied");
}

// The refusal a launch records travels to the client as the status message and
// as attributes Nova reads; without a record nothing gets vaguer than before.
TEST(LaunchRefusal, ReachesTheResponseAsMessageActionAndAttributes) {
  launch_failure::clear();
  launch_failure::refuse(503, "encoder_probe_failed", "No video encoder could start on this host.", "Check the host Doctor.");

  pt::ptree tree;
  nvhttp::put_launch_refusal_for_tests(tree, 503, "Video capture or encoding could not start.");

  EXPECT_EQ(tree.get<int>("root.<xmlattr>.status_code"), 503);
  EXPECT_EQ(tree.get<std::string>("root.<xmlattr>.status_message"), "No video encoder could start on this host. Check the host Doctor.");
  EXPECT_EQ(tree.get<std::string>("root.<xmlattr>.error_code"), "encoder_probe_failed");
  EXPECT_EQ(tree.get<std::string>("root.<xmlattr>.error_action"), "Check the host Doctor.");
}

TEST(LaunchRefusal, FallsBackToTheGenericTextWhenNothingWasRecorded) {
  launch_failure::clear();

  pt::ptree tree;
  nvhttp::put_launch_refusal_for_tests(tree, 503, "Video capture or encoding could not start.");

  EXPECT_EQ(tree.get<int>("root.<xmlattr>.status_code"), 503);
  EXPECT_EQ(tree.get<std::string>("root.<xmlattr>.status_message"), "Video capture or encoding could not start.");
  EXPECT_FALSE(tree.get_optional<std::string>("root.<xmlattr>.error_code"));
  EXPECT_FALSE(tree.get_optional<std::string>("root.<xmlattr>.error_action"));
}

TEST(LaunchRefusal, ARecordIsConsumedByTheResponseThatCarriesIt) {
  launch_failure::clear();
  launch_failure::refuse(503, "session_stopping", "The previous session is still stopping.", "Wait and launch again.");

  pt::ptree first;
  nvhttp::put_launch_refusal_for_tests(first, 503, "fallback");
  EXPECT_EQ(first.get<std::string>("root.<xmlattr>.error_code"), "session_stopping");

  pt::ptree second;
  nvhttp::put_launch_refusal_for_tests(second, 503, "fallback");
  EXPECT_EQ(second.get<std::string>("root.<xmlattr>.status_message"), "fallback");
  EXPECT_FALSE(second.get_optional<std::string>("root.<xmlattr>.error_code"));
}

TEST(LaunchRefusal, TheInnerReasonOutranksTheOuterOne) {
  // "No encoder could be probed against the compositor" is recorded deep in the
  // start sequence; the wrapper that then reports "the compositor did not start"
  // must not replace it.
  launch_failure::clear();
  launch_failure::refuse(503, "encoder_probe_failed", "inner", "");
  launch_failure::refuse_if_unexplained(503, "private_runtime_start_failed", "outer", "");
  auto taken = launch_failure::take();
  ASSERT_TRUE(taken.has_value());
  EXPECT_EQ(taken->code, "encoder_probe_failed");

  launch_failure::refuse_if_unexplained(503, "private_runtime_start_failed", "outer", "");
  taken = launch_failure::take();
  ASSERT_TRUE(taken.has_value());
  EXPECT_EQ(taken->code, "private_runtime_start_failed");

  launch_failure::refuse(503, "a", "first", "");
  launch_failure::refuse(503, "b", "second", "");
  taken = launch_failure::take();
  ASSERT_TRUE(taken.has_value());
  EXPECT_EQ(taken->code, "b");
  EXPECT_FALSE(launch_failure::take().has_value());
}

TEST(LaunchRefusal, ActionIsOptionalInTheStatusMessage) {
  EXPECT_EQ(launch_failure::status_message({503, "x", "Only a message.", ""}), "Only a message.");
  EXPECT_EQ(launch_failure::status_message({503, "x", "A message.", "An action."}), "A message. An action.");
}

#ifdef __linux__
TEST(LaunchRefusal, AFailedProbeNamesTheCaptureCauseBeforeTheEncoder) {
  launch_failure::clear();

  platf::set_kms_capture_refused_for_tests(true);
  video::note_launch_refused_by_probe(false);
  auto taken = launch_failure::take();
  ASSERT_TRUE(taken.has_value());
  EXPECT_EQ(taken->code, "kms_capture_needs_capability");
  EXPECT_NE(taken->action.find("--setup-host --enable-kms"), std::string::npos);
  platf::set_kms_capture_refused_for_tests(false);

  platf::set_capture_sources_missing_for_tests(true);
  video::note_launch_refused_by_probe(false);
  taken = launch_failure::take();
  ASSERT_TRUE(taken.has_value());
  EXPECT_EQ(taken->code, "no_capture_backend");
  platf::set_capture_sources_missing_for_tests(false);

  video::note_launch_refused_by_probe(true);
  taken = launch_failure::take();
  ASSERT_TRUE(taken.has_value());
  EXPECT_EQ(taken->code, "encoder_probe_failed");
  EXPECT_NE(taken->message.find("private stream compositor"), std::string::npos);
  EXPECT_NE(taken->action.find("Private Stream (GPU-native)"), std::string::npos);
  // Moonlight shows the action verbatim, so it names the launch mode, not a Nova screen.
  EXPECT_NE(taken->action.find("launch mode"), std::string::npos);
  EXPECT_EQ(taken->action.find("Play Setup"), std::string::npos);

  video::note_launch_refused_by_probe(false);
  taken = launch_failure::take();
  ASSERT_TRUE(taken.has_value());
  EXPECT_EQ(taken->code, "encoder_probe_failed");
  EXPECT_EQ(taken->message.find("private stream compositor"), std::string::npos);
}

TEST(LaunchRefusal, ACaptureRequestNothingCanServeIsRefusedWithItsReason) {
  // #739: Mirror Desktop with capture = wlr on KDE. The launch used to succeed, the video thread
  // found no backend, and Nova showed the dropped connection as "Error code: -1".
  launch_failure::clear();
  struct Restore {
    ~Restore() {
      platf::set_capture_request_satisfiable_for_tests(std::nullopt);
      platf::set_capture_sources_missing_for_tests(false);
      launch_failure::clear();
    }
  } restore;

  capture_generation::identity_t generation {};
  generation.stream_mode = "desktop_display";
  generation.capture_backend = "wlr";

  platf::set_capture_sources_missing_for_tests(false);
  platf::set_kms_capture_refused_for_tests(false);
  platf::set_capture_request_satisfiable_for_tests(false);
  ASSERT_TRUE(video::refuse_launch_if_capture_unavailable(generation));
  auto taken = launch_failure::take();
  ASSERT_TRUE(taken.has_value());
  EXPECT_EQ(taken->status, 503);
  EXPECT_EQ(taken->code, "capture_backend_unavailable");
  EXPECT_NE(taken->message.find("wlr"), std::string::npos);
  EXPECT_NE(taken->message.find("Mirror Desktop"), std::string::npos) << taken->message;
  EXPECT_NE(taken->message.find("KDE and GNOME"), std::string::npos);
  EXPECT_NE(taken->action.find("Autodetect"), std::string::npos);

  generation.capture_backend.clear();
  ASSERT_TRUE(video::refuse_launch_if_capture_unavailable(generation));
  taken = launch_failure::take();
  ASSERT_TRUE(taken.has_value());
  EXPECT_NE(taken->message.find("asks for auto capture"), std::string::npos) << taken->message;
  EXPECT_EQ(taken->message.find("KDE and GNOME"), std::string::npos);

  // An evaluation that found nothing at all keeps its own, more specific code.
  platf::set_capture_sources_missing_for_tests(true);
  ASSERT_TRUE(video::refuse_launch_if_capture_unavailable(generation));
  taken = launch_failure::take();
  ASSERT_TRUE(taken.has_value());
  EXPECT_EQ(taken->code, "no_capture_backend");
  platf::set_capture_sources_missing_for_tests(false);

  platf::set_capture_request_satisfiable_for_tests(true);
  EXPECT_FALSE(video::refuse_launch_if_capture_unavailable(generation));
  EXPECT_FALSE(launch_failure::take().has_value());
}

TEST(LaunchRefusal, TheSatisfiabilityCheckReadsTheLastEvaluation) {
  struct Restore {
    ~Restore() {
      platf::set_capture_sources_missing_for_tests(false);
    }
  } restore;

  // Before any evaluation there is nothing to refuse on.
  platf::set_capture_sources_missing_for_tests(false);
  EXPECT_TRUE(platf::capture_request_satisfiable("wlr", false));
  EXPECT_TRUE(platf::capture_request_satisfiable("", false));

  // An evaluation that found nothing serves no request, auto included.
  platf::set_capture_sources_missing_for_tests(true);
  EXPECT_FALSE(platf::capture_request_satisfiable("wlr", false));
  EXPECT_FALSE(platf::capture_request_satisfiable("kms", false));
  EXPECT_FALSE(platf::capture_request_satisfiable("", false));
  EXPECT_FALSE(platf::capture_request_satisfiable("auto", true));
}

namespace {
  /// A generation that captures the host desktop, as Mirror Desktop with capture = kms does.
  capture_generation::identity_t desktop_kms_generation() {
    capture_generation::identity_t generation {};
    generation.stream_mode = "desktop_display";
    generation.capture_backend = "kms";
    return generation;
  }

  struct RestoreCaptureRouteFacts {
    ~RestoreCaptureRouteFacts() {
      platf::set_capture_route_facts_for_tests(std::nullopt);
      launch_failure::clear();
    }
  };
}  // namespace

#ifdef POLARIS_BUILD_PYROWAVE
TEST(LaunchRefusal, APyroWaveLaunchOnAnHdrDesktopIsRefusedByNameBeforeTheStream) {
  // KWin scans an HDR desktop out as ABGR16161616F. The launch used to succeed, the client
  // built a decoder, and the stream ended at its first frame having carried nothing.
  RestoreCaptureRouteFacts restore;
  platf::set_capture_route_facts_for_tests(platf::capture_route_facts_for_tests_t {"kms", "kms", 1211384385u});
  const auto generation = desktop_kms_generation();

  EXPECT_EQ(video::pyrowave_capture_route(generation), pyrowave_availability::route_e::fp16_scanout);
  const auto refusal = video::pyrowave_capture_refusal(generation);
  ASSERT_TRUE(refusal.has_value());
  EXPECT_EQ(refusal->code, "pyrowave_capture_unreadable");

  // It reaches the client the way every launch refusal does.
  launch_failure::refuse(refusal->status, refusal->code, refusal->message, refusal->action);
  pt::ptree tree;
  nvhttp::put_launch_refusal_for_tests(tree, 503, "fallback");
  EXPECT_EQ(tree.get<int>("root.<xmlattr>.status_code"), 503);
  EXPECT_EQ(tree.get<std::string>("root.<xmlattr>.error_code"), "pyrowave_capture_unreadable");
  EXPECT_EQ(tree.get<std::string>("root.<xmlattr>.status_message"), launch_failure::status_message(*refusal));
  EXPECT_EQ(tree.get<std::string>("root.<xmlattr>.error_action"), refusal->action);
}

TEST(LaunchRefusal, APyroWaveLaunchOnARouteItCanReadGoesAhead) {
  RestoreCaptureRouteFacts restore;
  const auto generation = desktop_kms_generation();

  // An eight bit scanout, and a packed ten bit HDR one, are both formats PyroWave reads.
  for (const auto fourcc : {0x34325258u /* XR24 */, 0x30334241u /* AB30 */}) {
    platf::set_capture_route_facts_for_tests(platf::capture_route_facts_for_tests_t {"kms", "kms", fourcc});
    EXPECT_EQ(video::pyrowave_capture_route(generation), pyrowave_availability::route_e::readable) << fourcc;
    EXPECT_FALSE(video::pyrowave_capture_refusal(generation).has_value()) << fourcc;
  }

  // A private compositor is never a KMS scanout, whatever the desktop is doing.
  platf::set_capture_route_facts_for_tests(platf::capture_route_facts_for_tests_t {"wlr", "wlr", 1211384385u});
  EXPECT_FALSE(video::pyrowave_capture_refusal(generation).has_value());

  // A scanout that could not be read is no reason to refuse.
  platf::set_capture_route_facts_for_tests(platf::capture_route_facts_for_tests_t {"kms", "kms", std::nullopt});
  EXPECT_EQ(video::pyrowave_capture_route(generation), pyrowave_availability::route_e::unknown);
  EXPECT_FALSE(video::pyrowave_capture_refusal(generation).has_value());
}

TEST(LaunchRefusal, ACaptureSettingOnlyNvencCanUseRefusesPyroWaveAndNamesIt) {
  RestoreCaptureRouteFacts restore;
  auto generation = desktop_kms_generation();
  generation.capture_backend = "nvfbc";
  platf::set_capture_route_facts_for_tests(platf::capture_route_facts_for_tests_t {"none", "nvfbc", std::nullopt});

  EXPECT_EQ(video::pyrowave_capture_route(generation), pyrowave_availability::route_e::unsupported_backend);
  const auto refusal = video::pyrowave_capture_refusal(generation);
  ASSERT_TRUE(refusal.has_value());
  EXPECT_EQ(refusal->code, "pyrowave_capture_unreadable");
  EXPECT_NE(refusal->message.find("nvfbc"), std::string::npos) << refusal->message;
}
#endif

TEST(PyroWaveOffer, CapabilitiesSaysWhyPyroWaveIsMissingInTheContractShape) {
  // The field names and reason ids are what Nova builds against: capture.pyrowave_unavailable is
  // {reason, message} exactly when pyrowave is not in capture.codecs.
  pyrowave_availability::offer_facts_t facts;
  facts.built = true;
  facts.device = true;
  facts.host_route = pyrowave_availability::route_e::fp16_scanout;
  const auto hidden = nvhttp::capture_codecs_for_tests(2, 2, pyrowave_availability::unavailable(facts));
  EXPECT_EQ(hidden["codecs"], nlohmann::json::array({"h264", "hevc", "av1"})) << hidden.dump();
  ASSERT_TRUE(hidden.contains("pyrowave_unavailable")) << hidden.dump();
  const auto &why = hidden["pyrowave_unavailable"];
  ASSERT_TRUE(why.is_object());
  EXPECT_EQ(why.size(), 2u) << why.dump();
  EXPECT_EQ(why["reason"], "fp16_capture");
  ASSERT_TRUE(why["message"].is_string());
  EXPECT_NE(why["message"].get<std::string>().find("HDR"), std::string::npos) << why.dump();

  facts = {};
  EXPECT_EQ(nvhttp::capture_codecs_for_tests(1, 1, pyrowave_availability::unavailable(facts))["pyrowave_unavailable"]["reason"],
            "not_built");

  // Offered: in the list, and no reason beside it.
  const auto offered = nvhttp::capture_codecs_for_tests(2, 1, std::nullopt);
  EXPECT_EQ(offered["codecs"], nlohmann::json::array({"h264", "hevc", "pyrowave"})) << offered.dump();
  EXPECT_FALSE(offered.contains("pyrowave_unavailable")) << offered.dump();
}

TEST(LaunchRefusal, AStreamCannotJoinACaptureAnotherCodecsStreamHolds) {
  // One capture thread serves every stream and opens its display for the first one's memory
  // type, so a PyroWave stream beside an H.264 one, either way round, got no picture.
  rtsp_stream::set_cleanup_session_probe_for_tests([]() {});
  struct Restore {
    ~Restore() {
      rtsp_stream::terminate_sessions();
      rtsp_stream::set_cleanup_session_probe_for_tests({});
    }
  } restore;
  rtsp_stream::terminate_sessions();

  EXPECT_FALSE(rtsp_stream::capture_in_use_refusal(true).has_value());
  EXPECT_FALSE(rtsp_stream::capture_in_use_refusal(false).has_value());

  rtsp_stream::launch_session_t h264 {};
  h264.id = 7801;
  h264.unique_id = "h264-owner";
  rtsp_stream::add_session_for_tests(h264, false, 0);
  EXPECT_FALSE(rtsp_stream::capture_in_use_refusal(false).has_value());
  const auto refused = rtsp_stream::capture_in_use_refusal(true);
  ASSERT_TRUE(refused.has_value());
  EXPECT_EQ(refused->code, "capture_in_use_by_other_codec");
  EXPECT_EQ(refused->status, 503);

  // A stream that is stopping can still hold the capture, so it counts too.
  rtsp_stream::terminate_sessions();
  rtsp_stream::launch_session_t pyrowave {};
  pyrowave.id = 7802;
  pyrowave.unique_id = "pyrowave-owner";
  rtsp_stream::add_session_for_tests(pyrowave, true, video::VIDEO_FORMAT_PYROWAVE);
  EXPECT_FALSE(rtsp_stream::capture_in_use_refusal(true).has_value());
  ASSERT_TRUE(rtsp_stream::capture_in_use_refusal(false).has_value());
}
#endif


namespace {
  struct RestorePairedCodecModes {
    int configured_hevc = config::video.hevc_mode;
    int configured_av1 = config::video.av1_mode;
    int resolved_hevc = video::active_hevc_mode;
    int resolved_av1 = video::active_av1_mode;
#ifdef __linux__
    bool cage = config::video.linux_display.use_cage_compositor;
#endif

    RestorePairedCodecModes() {
      config::video.hevc_mode = config::video.av1_mode = 0;
      video::active_hevc_mode = video::active_av1_mode = 0;
#ifdef __linux__
      // No cache files, display creation or real GPU probing in this fixture.
      config::video.linux_display.use_cage_compositor = false;
#endif
    }

    ~RestorePairedCodecModes() {
      config::video.hevc_mode = configured_hevc;
      config::video.av1_mode = configured_av1;
      video::active_hevc_mode = resolved_hevc;
      video::active_av1_mode = resolved_av1;
#ifdef __linux__
      config::video.linux_display.use_cage_compositor = cage;
#endif
    }
  };
}

TEST(PairedCodecCapabilities, ConfiguredAutoOffersResolvedHevcMain) {
  RestorePairedCodecModes restore;
  video::active_hevc_mode = 2;
  const auto capture = nvhttp::paired_capture_codecs_for_tests(std::nullopt);
  EXPECT_EQ(capture["codecs"], nlohmann::json::array({"h264", "hevc", "pyrowave"}));
}

TEST(PairedCodecCapabilities, ConfiguredAutoOffersResolvedHevcMain10AsHevc) {
  RestorePairedCodecModes restore;
  video::active_hevc_mode = 3;
  EXPECT_EQ(nvhttp::paired_capture_codecs_for_tests(std::nullopt)["codecs"],
            nlohmann::json::array({"h264", "hevc", "pyrowave"}));
}

TEST(PairedCodecCapabilities, UnresolvedAutoDoesNotInventHevcOrAv1) {
  RestorePairedCodecModes restore;
  EXPECT_EQ(nvhttp::paired_capture_codecs_for_tests(std::nullopt)["codecs"],
            nlohmann::json::array({"h264", "pyrowave"}));
}

TEST(PairedCodecCapabilities, DetectedUnavailableAutoDoesNotInventHevcOrAv1) {
  RestorePairedCodecModes restore;
  video::active_hevc_mode = video::active_av1_mode = 1;
  EXPECT_EQ(nvhttp::paired_capture_codecs_for_tests(std::nullopt)["codecs"],
            nlohmann::json::array({"h264", "pyrowave"}));
}

TEST(PairedCodecCapabilities, ExplicitOffKeepsHevcAndAv1Unavailable) {
  RestorePairedCodecModes restore;
  config::video.hevc_mode = config::video.av1_mode = 1;
  video::active_hevc_mode = video::active_av1_mode = 1;
  EXPECT_EQ(nvhttp::paired_capture_codecs_for_tests(std::nullopt)["codecs"],
            nlohmann::json::array({"h264", "pyrowave"}));
}

TEST(PairedCodecCapabilities, ExplicitHevcMainOfferIsPreserved) {
  RestorePairedCodecModes restore;
  config::video.hevc_mode = video::active_hevc_mode = 2;
  EXPECT_EQ(nvhttp::paired_capture_codecs_for_tests(std::nullopt)["codecs"],
            nlohmann::json::array({"h264", "hevc", "pyrowave"}));
}

TEST(PairedCodecCapabilities, ExplicitHevcMain10OfferIsPreserved) {
  RestorePairedCodecModes restore;
  config::video.hevc_mode = video::active_hevc_mode = 3;
  EXPECT_EQ(nvhttp::paired_capture_codecs_for_tests(std::nullopt)["codecs"],
            nlohmann::json::array({"h264", "hevc", "pyrowave"}));
}

TEST(PairedCodecCapabilities, ConfiguredAutoOffersResolvedAv1Main8) {
  RestorePairedCodecModes restore;
  video::active_av1_mode = 2;
  EXPECT_EQ(nvhttp::paired_capture_codecs_for_tests(std::nullopt)["codecs"],
            nlohmann::json::array({"h264", "av1", "pyrowave"}));
}

TEST(PairedCodecCapabilities, ConfiguredAutoOffersResolvedAv1Main10AsAv1) {
  RestorePairedCodecModes restore;
  video::active_av1_mode = 3;
  EXPECT_EQ(nvhttp::paired_capture_codecs_for_tests(std::nullopt)["codecs"],
            nlohmann::json::array({"h264", "av1", "pyrowave"}));
}

TEST(PairedCodecCapabilities, ExplicitAv1OfferIsPreserved) {
  RestorePairedCodecModes restore;
  config::video.av1_mode = video::active_av1_mode = 2;
  EXPECT_EQ(nvhttp::paired_capture_codecs_for_tests(std::nullopt)["codecs"],
            nlohmann::json::array({"h264", "av1", "pyrowave"}));
}

TEST(PairedCodecCapabilities, PyroWaveRefusalRemainsIndependentOfResolvedClassicCodecs) {
  RestorePairedCodecModes restore;
  video::active_hevc_mode = video::active_av1_mode = 2;
  pyrowave_availability::offer_facts_t facts;
  const auto capture = nvhttp::paired_capture_codecs_for_tests(pyrowave_availability::unavailable(facts));
  EXPECT_EQ(capture["codecs"], nlohmann::json::array({"h264", "hevc", "av1"}));
  ASSERT_TRUE(capture.contains("pyrowave_unavailable"));
  EXPECT_EQ(capture["pyrowave_unavailable"]["reason"], "not_built");
}

TEST(PairedCodecCapabilities, ProductionCapabilitiesRouteUsesTheExercisedOfferBuilder) {
  std::ifstream input(std::string(POLARIS_SOURCE_DIR) + "/src/nvhttp.cpp");
  ASSERT_TRUE(input.is_open());
  std::ostringstream bytes;
  bytes << input.rdbuf();
  const auto source = bytes.str();
  const auto start = source.find("auto polarisCapabilities = ");
  ASSERT_NE(start, std::string::npos);
  const auto end = source.find("// PyroWave's bitrate advice", start);
  ASSERT_NE(end, std::string::npos);
  const auto route = source.substr(start, end - start);
  EXPECT_NE(route.find("put_paired_capture_codecs(capture, video::pyrowave_unavailable(), true);"), std::string::npos);
  EXPECT_EQ(route.find("put_capture_codecs(capture, config::video.hevc_mode"), std::string::npos);
}
