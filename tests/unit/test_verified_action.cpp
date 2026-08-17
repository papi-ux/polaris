/**
 * @file tests/unit/test_verified_action.cpp
 * @brief Test read-back confirmation of otherwise silent failures.
 */
#include <gtest/gtest.h>
#include <src/verified_action.h>
#include <string>

using namespace std::literals;

namespace {
  class VerifiedAction: public ::testing::Test {
  protected:
    void SetUp() override {
      verified_action::clear();
    }

    void TearDown() override {
      verified_action::clear();
    }
  };
}  // namespace

TEST_F(VerifiedAction, RecordsNothingWhenTheReadBackMatchesTheRequest) {
  EXPECT_TRUE(verified_action::confirm("display.mode", "Apply the requested display mode", "2560x1440@120", "2560x1440@120"));

  EXPECT_TRUE(verified_action::silent_failures().empty());
}

TEST_F(VerifiedAction, RecordsBothValuesWhenTheReadBackDisagrees) {
  // The whole point of the read-back: the caller asked for 120 Hz, the system
  // says 60, and nothing in the return code of the request would have said so.
  EXPECT_FALSE(verified_action::confirm("display.mode", "Apply the requested display mode", "2560x1440@120", "2560x1440@60"));

  const auto recorded = verified_action::silent_failures();
  ASSERT_EQ(recorded.size(), 1);
  EXPECT_EQ(recorded.front().id, "display.mode");
  EXPECT_EQ(recorded.front().requested, "2560x1440@120");
  EXPECT_EQ(recorded.front().actual, "2560x1440@60");
  EXPECT_EQ(recorded.front().description, "Apply the requested display mode");
  EXPECT_FALSE(recorded.front().observed_at.empty());
}

TEST_F(VerifiedAction, ReturnsTheVerdictSoCallersCanBranchOnIt) {
  // A caller that wants to fall back needs the answer, not just the record.
  EXPECT_FALSE(verified_action::confirm("step.ran", "Run the topology step", false));
  EXPECT_TRUE(verified_action::confirm("step.ran", "Run the topology step", true));
}

TEST_F(VerifiedAction, RecordsAStepThatNeverRan) {
  EXPECT_FALSE(verified_action::confirm("topology.step", "Run the headless topology step", false));

  const auto recorded = verified_action::silent_failures();
  ASSERT_EQ(recorded.size(), 1);
  EXPECT_EQ(recorded.front().id, "topology.step");
  EXPECT_EQ(recorded.front().requested, "applied");
  EXPECT_EQ(recorded.front().actual, "not applied");
}

TEST_F(VerifiedAction, KeepsTheNewestMismatchesWithinItsBound) {
  // A failure repeating every frame must not be able to grow without limit, and
  // the entries worth keeping are the most recent ones.
  for (std::size_t index = 0; index < verified_action::max_retained_records + 10; ++index) {
    verified_action::confirm("capture.path", "Use the requested capture path", "dmabuf", "shm" + std::to_string(index));
  }

  const auto recorded = verified_action::silent_failures();
  ASSERT_EQ(recorded.size(), verified_action::max_retained_records);
  EXPECT_EQ(recorded.back().actual, "shm" + std::to_string(verified_action::max_retained_records + 9));
  EXPECT_EQ(recorded.front().actual, "shm10");
}

TEST_F(VerifiedAction, RendersRecordsForTheSupportBundle) {
  verified_action::confirm("capture.path", "Use the requested capture path", "dmabuf", "shm");

  const auto document = verified_action::to_json();
  ASSERT_TRUE(document.is_array());
  ASSERT_EQ(document.size(), 1);
  EXPECT_EQ(document[0]["id"], "capture.path");
  EXPECT_EQ(document[0]["requested"], "dmabuf");
  EXPECT_EQ(document[0]["actual"], "shm");
  EXPECT_FALSE(document[0]["observed_at"].get<std::string>().empty());
}

TEST_F(VerifiedAction, RendersAnEmptyArrayWhenNothingFailedSilently) {
  const auto document = verified_action::to_json();

  ASSERT_TRUE(document.is_array());
  EXPECT_TRUE(document.empty());
}
