/**
 * @file tests/unit/test_launch_response_status.cpp
 * @brief The /launch and /resume handlers must never ship a response with no
 *        status_code, even when the handler throws mid-flight after partially
 *        filling the response tree. This is the host-side companion to nova
 *        #225, which stopped the client crashing (NPE) on such a body.
 */

#include <src/nvhttp.h>

#include <boost/property_tree/ptree.hpp>
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
