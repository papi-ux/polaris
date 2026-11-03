/** @file Native observation through the real stream_stats lifecycle; selected by test_polaris_http. */
#include "src/stream_stats.h"
#include "src/stream_native_reception.h"
#include <gtest/gtest.h>
#include <memory>
#include <string>
namespace {
  struct reception_scope_t {
    const std::string owner = "native-reception-fixture";
    std::uint64_t generation = 615001;
    ~reception_scope_t() { stream_stats::stop_session_timing(owner,generation); }
    void start(std::shared_ptr<stream_frame_feedback::sender_ledger_t> frames,
               std::shared_ptr<stream_native_reception::scope_t> observation) {
      stream_stats::start_session_timing(owner,generation,"fixture-app-session",frames,{},observation);
    }
  };
}
TEST(StreamStatsNativeReception, OwnCurrentTimingSnapshotAndWrongGenerationAreSeparated) {
  reception_scope_t fixture;
  auto frames=std::make_shared<stream_frame_feedback::sender_ledger_t>(fixture.generation);
  auto observation=std::make_shared<stream_native_reception::scope_t>(fixture.generation,frames,true,false);
  fixture.start(frames,observation);
  auto own=stream_stats::get_session_timing(fixture.owner);
  ASSERT_TRUE(own.native_reception);EXPECT_TRUE(own.native_reception->negotiated);
  EXPECT_FALSE(own.native_reception->media_authenticated);EXPECT_FALSE(own.native_reception->has_report);
  EXPECT_FALSE(stream_stats::get_session_timing("other-native-fixture").native_reception);
  auto wrong_frames=std::make_shared<stream_frame_feedback::sender_ledger_t>(fixture.generation+1);
  auto wrong=std::make_shared<stream_native_reception::scope_t>(fixture.generation+1,wrong_frames,true);
  fixture.start(frames,wrong);
  EXPECT_FALSE(stream_stats::get_session_timing(fixture.owner).native_reception);
  EXPECT_FALSE(observation->readback());
}
TEST(StreamStatsNativeReception, ReconnectRetiresOldObservationAndLateStopCannotRemoveNewScope) {
  reception_scope_t fixture;
  auto old_frames=std::make_shared<stream_frame_feedback::sender_ledger_t>(fixture.generation);
  auto old=std::make_shared<stream_native_reception::scope_t>(fixture.generation,old_frames,true,true);
  fixture.start(old_frames,old);
  const auto old_generation=fixture.generation++;
  auto frames=std::make_shared<stream_frame_feedback::sender_ledger_t>(fixture.generation);
  auto current=std::make_shared<stream_native_reception::scope_t>(fixture.generation,frames,true,false);
  fixture.start(frames,current);EXPECT_FALSE(old->readback());
  stream_stats::stop_session_timing(fixture.owner,old_generation);
  auto timing=stream_stats::get_session_timing(fixture.owner);
  ASSERT_TRUE(timing.session_active);EXPECT_EQ(timing.session_generation,fixture.generation);
  ASSERT_TRUE(timing.native_reception);EXPECT_FALSE(timing.native_reception->media_authenticated);
  EXPECT_FALSE(timing.native_reception->has_report);
}
