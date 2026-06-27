/**
 * @file tests/unit/test_session_event_queue.cpp
 * @brief Tests for Polaris session SSE event backlog filtering.
 */

#include <src/session_event_queue.h>

#include <gtest/gtest.h>
#include <string>
#include <vector>

TEST(SessionEventQueueTests, SubscriberOnlyReceivesEventsNewerThanItsCursor) {
  session_event_queue::event_queue queue;

  queue.push("stale-stream-ended");
  const auto cursor = queue.cursor();
  queue.push("current-stream-active");

  const auto events = queue.events_after(cursor);

  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events.front(), "current-stream-active");
}

TEST(SessionEventQueueTests, OldEventsArePrunedWithoutReplayingToNewSubscribers) {
  session_event_queue::event_queue queue {3};

  queue.push("old-1");
  queue.push("old-2");
  queue.push("old-3");
  queue.push("old-4");

  const auto cursor = queue.cursor();
  queue.push("new-1");

  const auto events = queue.events_after(cursor);

  ASSERT_EQ(events.size(), 1u);
  EXPECT_EQ(events.front(), "new-1");
}
