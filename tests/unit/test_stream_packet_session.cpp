/**
 * @file tests/unit/test_stream_packet_session.cpp
 * @brief Packet destinations follow real host session stop and destruction.
 */
#include "src/rtsp.h"
#include "src/stream.h"

#include <gtest/gtest.h>

#include <chrono>
#include <future>

using namespace std::chrono_literals;

namespace {
  std::shared_ptr<stream::session_t> make_session() {
    stream::config_t config {};
    rtsp_stream::launch_session_t launch {};
    launch.id = 1;
    launch.gcm_key.resize(16);
    launch.iv.resize(16);
    launch.unique_id = "packet-owner-client";
    return stream::session::alloc(config, launch);
  }
}

TEST(StreamPacketSessionTests, StopRejectsQueuedAudioAndVideoBeforeSessionDestruction) {
  for (const auto graceful : {false, true}) {
    auto session = make_session();
    const auto destination = stream::session::packet_destination_for_tests(*session);
    video::packet_raw_generic video {{1, 2, 3}, 1, true};
    video.channel_data = destination;
    audio::packet_t audio {destination, audio::buffer_t {3}};
    auto active = video.channel_data.acquire();
    EXPECT_EQ(active.get(), session.get());
    EXPECT_EQ(audio.first.acquire().get(), session.get());

    stream::session::set_state_for_tests(*session, stream::session::state_e::RUNNING);
    if (graceful) {
      stream::session::graceful_stop(*session);
    } else {
      stream::session::stop(*session);
    }
    EXPECT_EQ(stream::session::state(*session), stream::session::state_e::STOPPING);
    EXPECT_FALSE(video.channel_data.acquire());
    EXPECT_FALSE(audio.first.acquire());
    // stop cannot block waiting for this already admitted broadcaster.
    EXPECT_EQ(stream::session::uuid(*static_cast<stream::session_t *>(active.get())),
              "packet-owner-client");
    active.reset();
    session.reset();
    EXPECT_FALSE(video.channel_data.acquire());
    EXPECT_FALSE(audio.first.acquire());
  }
}

TEST(StreamPacketSessionTests, AbortedAllocationDrainsSendsAndRejectsRetainedPackets) {
  auto session = make_session();
  const auto queued = stream::session::packet_destination_for_tests(*session);
  auto active = queued.acquire();
  std::promise<void> entered;
  auto destruction = std::async(std::launch::async,
    [retiring = std::move(session), &entered]() mutable {
      entered.set_value();
      retiring.reset();
    });
  entered.get_future().wait();
  EXPECT_EQ(destruction.wait_for(20ms), std::future_status::timeout);
  active.reset();
  EXPECT_EQ(destruction.wait_for(1s), std::future_status::ready);
  destruction.get();
  EXPECT_FALSE(queued.acquire());

  auto reconnected = make_session();
  auto current = stream::session::packet_destination_for_tests(*reconnected);
  EXPECT_FALSE(queued.acquire());
  EXPECT_EQ(current.acquire().get(), reconnected.get());
}
