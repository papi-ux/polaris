/**
 * @file tests/unit/test_launch_response_status.cpp
 * @brief The /launch and /resume handlers must never ship a response with no
 *        status_code, even when the handler throws mid-flight after partially
 *        filling the response tree. This is the host-side companion to nova
 *        #225, which stopped the client crashing (NPE) on such a body.
 */

#include <src/launch_failure.h>
#include <src/nvhttp.h>
#include <src/platform/common.h>
#include <src/video.h>

#include <boost/property_tree/ptree.hpp>
#include <optional>
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
#endif
