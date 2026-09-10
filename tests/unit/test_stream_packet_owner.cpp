/**
 * @file tests/unit/test_stream_packet_owner.cpp
 * @brief Queued media must not outlive or follow a replaced stream session.
 */
#include "src/stream_packet_owner.h"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <optional>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

TEST(StreamPacketOwnerTests, EmptyAndNullDestinationsCannotEnterHostBroadcasters) {
  stream_packets::destination_t empty;
  stream_packets::destination_t probe {nullptr};
  stream_packets::owner_t null_owner {nullptr};
  EXPECT_FALSE(empty.acquire());
  EXPECT_FALSE(probe.acquire());
  EXPECT_FALSE(null_owner.destination().acquire());
  EXPECT_EQ(empty.capture_owner_tag(), nullptr);
}

TEST(StreamPacketOwnerTests, StoppingRejectsQueuedAndLateProducerPackets) {
  int session = 42;
  stream_packets::owner_t owner {&session};
  std::vector<stream_packets::destination_t> queued(64, owner.destination());
  auto producer = owner.destination();
  EXPECT_EQ(producer.capture_owner_tag(), &session);
  EXPECT_EQ(queued.front().acquire().get(), &session);

  owner.close();
  queued.push_back(producer);
  for (const auto &packet : queued) {
    EXPECT_FALSE(packet.acquire());
  }
  owner.close_and_wait();
  owner.close_and_wait();
}

TEST(StreamPacketOwnerTests, ReconnectAtSameAddressNeverReopensOldPackets) {
  int reused_storage = 1;
  stream_packets::destination_t old_packet;
  {
    stream_packets::owner_t old_owner {&reused_storage};
    old_packet = old_owner.destination();
    EXPECT_EQ(old_packet.acquire().get(), &reused_storage);
  }

  reused_storage = 2;
  stream_packets::owner_t new_owner {&reused_storage};
  auto new_packet = new_owner.destination();
  EXPECT_FALSE(old_packet.acquire());
  EXPECT_EQ(new_packet.acquire().get(), &reused_storage);
  new_owner.close_and_wait();
  EXPECT_FALSE(new_packet.acquire());
}

TEST(StreamPacketOwnerTests, TeardownWaitsForBothActiveSendsButNotQueuedPackets) {
  int session = 42;
  stream_packets::owner_t owner {&session};
  auto queued_packet = owner.destination();
  auto video = queued_packet.acquire();
  auto audio = queued_packet.acquire();
  owner.close();
  EXPECT_FALSE(queued_packet.acquire());

  std::promise<void> entered;
  auto teardown = std::async(std::launch::async, [&] {
    entered.set_value();
    owner.close_and_wait();
  });
  entered.get_future().wait();
  EXPECT_EQ(teardown.wait_for(20ms), std::future_status::timeout);
  video.reset();
  EXPECT_EQ(teardown.wait_for(20ms), std::future_status::timeout);
  // An admitted send retains access after close, through its final use.
  EXPECT_EQ(*static_cast<int *>(audio.get()), 42);
  audio.reset();
  EXPECT_EQ(teardown.wait_for(1s), std::future_status::ready);
  teardown.get();
  EXPECT_FALSE(queued_packet.acquire());
}

TEST(StreamPacketOwnerTests, AbortedStartDestructionWaitsBeforeFreeingSession) {
  auto session = std::make_unique<int>(42);
  auto owner = std::make_unique<stream_packets::owner_t>(session.get());
  auto queued_packet = owner->destination();
  auto active = queued_packet.acquire();
  std::promise<void> entered;
  auto destruction = std::async(std::launch::async, [&] {
    entered.set_value();
    owner.reset();
    session.reset();
  });
  entered.get_future().wait();
  EXPECT_EQ(destruction.wait_for(20ms), std::future_status::timeout);
  EXPECT_EQ(*static_cast<int *>(active.get()), 42);
  active.reset();
  EXPECT_EQ(destruction.wait_for(1s), std::future_status::ready);
  destruction.get();
  EXPECT_FALSE(queued_packet.acquire());
}

TEST(StreamPacketOwnerTests, MovingDeliveryReleasesExactlyItsPreviousOwner) {
  int first = 1, second = 2;
  stream_packets::owner_t first_owner {&first}, second_owner {&second};
  auto first_send = first_owner.destination().acquire();
  auto second_send = second_owner.destination().acquire();
  stream_packets::delivery_t moved {std::move(first_send)};
  EXPECT_FALSE(first_send);
  EXPECT_EQ(moved.get(), &first);
  moved = std::move(second_send);
  EXPECT_FALSE(second_send);
  EXPECT_EQ(moved.get(), &second);
  first_owner.close_and_wait();
  moved.reset();
  moved.reset();
  second_owner.close_and_wait();
}

TEST(StreamPacketOwnerTests, StoppingOneSessionLeavesItsPeerDelivering) {
  int first = 1, second = 2;
  stream_packets::owner_t first_owner {&first}, second_owner {&second};
  auto stale = first_owner.destination();
  auto surviving = second_owner.destination();
  auto active = surviving.acquire();
  first_owner.close_and_wait();
  EXPECT_FALSE(stale.acquire());
  EXPECT_EQ(active.get(), &second);
  EXPECT_EQ(surviving.acquire().get(), &second);
}

TEST(StreamPacketOwnerTests, ConcurrentSendersCannotAccessSessionAfterTeardown) {
  // Keep queued destinations alive past destruction; run repeatedly so ASan
  // and TSan exercise close against acquire and the last release on both lanes.
  for (int reconnect = 0; reconnect < 100; ++reconnect) {
    auto session = std::make_unique<std::atomic<int>>(0);
    stream_packets::owner_t owner {session.get()};
    const auto packet = owner.destination();
    std::atomic<int> started = 0;
    auto sender = [&] {
      auto initial = packet.acquire();
      EXPECT_TRUE(initial);
      if (initial) {
        ++*static_cast<std::atomic<int> *>(initial.get());
      }
      ++started;
      initial.reset();
      while (auto delivery = packet.acquire()) {
        ++*static_cast<std::atomic<int> *>(delivery.get());
        std::this_thread::yield();
      }
    };
    std::thread video {sender}, audio {sender};
    while (started.load() != 2) {
      std::this_thread::yield();
    }
    owner.close_and_wait();
    EXPECT_GE(session->load(), 2);
    session.reset();
    video.join();
    audio.join();
    EXPECT_FALSE(packet.acquire());
  }
}
