/**
 * Producer binding on a real, private, in-process PipeWire graph (tests/fixtures/pipewire-pool.conf).
 *
 * The fixture's producer stream and the capture stream share one in-process core, so the producer
 * client IS the capture client here and its credentials are this process's. That proves the actual
 * mechanics (own registry on the capture core, bounded core sync, bound client id, server-set
 * client.id/link/sec.* keys, link removal, cancellation) without a user daemon. It is not a
 * cross-process producer, display, compositor or physical acceptance.
 */
#include "../../tests_common.h"

#if defined(__linux__) && defined(POLARIS_BUILD_PORTAL)
  #include <chrono>
  #include <drm_fourcc.h>
  #include <unistd.h>

  #include "../../fixtures/pipewire_producer_graph.h"

namespace pipewire_capture {
  struct capture_producer_test_access {
    // The fixture builds the graph with tracking off; turning it on afterwards also proves that a
    // late registry still receives every existing global.
    static bool track(capture_t &capture) {
      pw_thread_loop_lock(capture.loop_);
      capture.options_.track_producer = true;
      const bool started = capture.start_producer_tracking();
      pw_thread_loop_unlock(capture.loop_);
      return started;
    }
  };
}  // namespace pipewire_capture

namespace {
  using namespace std::chrono_literals;
  using access = pipewire_capture::capture_graph_test_access;
  using tracking = pipewire_capture::capture_producer_test_access;
}  // namespace

TEST(PipeWireProducerGraphTests, DisabledTrackingCostsNothingAndSaysSo) {
  test_utils::pipewire_producer_graph_t graph {false, 64, 64};
  ASSERT_TRUE(graph.start());
  const auto binding = graph.capture->producer_binding(1s);
  EXPECT_FALSE(binding.verified);
  EXPECT_EQ(binding.reason, "producer_tracking_disabled");
}

TEST(PipeWireProducerGraphTests, BindsTheLinkedSourceThroughTheCaptureCoresOwnRegistry) {
  test_utils::pipewire_producer_graph_t graph {false, 64, 64};
  ASSERT_TRUE(graph.start());
  ASSERT_TRUE(tracking::track(*graph.capture));

  const auto binding = graph.capture->producer_binding(2s);
  ASSERT_TRUE(binding.verified) << binding.reason;
  EXPECT_EQ(binding.reason, "verified");
  EXPECT_FALSE(binding.server_version.empty());
  pw_thread_loop_lock(access::loop(*graph.capture));
  const auto source_node = pw_stream_get_node_id(graph.source);
  const auto capture_node = access::node(*graph.capture);
  pw_thread_loop_unlock(access::loop(*graph.capture));
  EXPECT_EQ(binding.source_node.id, source_node);
  EXPECT_EQ(binding.capture_node.id, capture_node);
  EXPECT_NE(binding.source_node.serial, 0u);
  EXPECT_NE(binding.capture_node.serial, 0u);
  ASSERT_FALSE(binding.links.empty());
  for (const auto &link : binding.links) {
    EXPECT_EQ(link.source, source_node);
    EXPECT_EQ(link.sink, capture_node);
  }
  // One in-process core owns both streams, so the producer is this process's own client.
  EXPECT_EQ(binding.producer_client, binding.capture_client);
  EXPECT_EQ(binding.producer_pid, static_cast<std::int64_t>(getpid()));
  EXPECT_EQ(binding.producer_uid, static_cast<std::uint64_t>(getuid()));

  // A second read of an unchanged graph is the same observation.
  EXPECT_EQ(graph.capture->producer_binding(2s), binding);
}

TEST(PipeWireProducerGraphTests, RemovingTheLinkUnbindsTheSource) {
  test_utils::pipewire_producer_graph_t graph {false, 64, 64};
  ASSERT_TRUE(graph.start());
  ASSERT_TRUE(tracking::track(*graph.capture));
  ASSERT_TRUE(graph.capture->producer_binding(2s).verified);

  pw_thread_loop_lock(access::loop(*graph.capture));
  pw_proxy_destroy(graph.link);
  graph.link = nullptr;
  pw_thread_loop_unlock(access::loop(*graph.capture));
  // The read's own core sync orders it after the server processed the destroy.
  const auto binding = graph.capture->producer_binding(2s);
  EXPECT_FALSE(binding.verified);
  EXPECT_EQ(binding.producer_pid, 0);
  // An unlinked consumer pauses; should this PipeWire end the stream instead, the capture is no
  // longer running. Either way nothing is bound, and the reason says which.
  EXPECT_TRUE(binding.reason == "source_not_linked" || binding.reason == "capture_not_running") << binding.reason;
}

TEST(PipeWireProducerGraphTests, StopAndShutdownEndReadsWithoutWaitingForTheTimeout) {
  test_utils::pipewire_producer_graph_t graph {false, 64, 64};
  ASSERT_TRUE(graph.start());
  ASSERT_TRUE(tracking::track(*graph.capture));

  graph.capture->stop();
  const auto started = std::chrono::steady_clock::now();
  const auto stopped = graph.capture->producer_binding(10s);
  EXPECT_LT(std::chrono::steady_clock::now() - started, 1s);
  EXPECT_FALSE(stopped.verified);
  EXPECT_EQ(stopped.reason, "read_cancelled");

  graph.shutdown_capture();
  const auto after = graph.capture->producer_binding(10s);
  EXPECT_FALSE(after.verified);
  EXPECT_EQ(after.reason, "read_cancelled");
}

TEST(PipeWireProducerGraphTests, AReadFromThePipeWireThreadIsRefusedNotDeadlocked) {
  test_utils::pipewire_producer_graph_t graph {false, 64, 64};
  ASSERT_TRUE(graph.start());
  ASSERT_TRUE(tracking::track(*graph.capture));

  struct call_t {
    pipewire_capture::capture_t *capture;
    capture_producer::binding_t result;
  } call {graph.capture.get(), {}};
  auto *loop = access::loop(*graph.capture);
  pw_thread_loop_lock(loop);
  const int invoked = pw_loop_invoke(pw_thread_loop_get_loop(loop),
    [](spa_loop *, bool, std::uint32_t, const void *, std::size_t, void *opaque) {
      auto *c = static_cast<call_t *>(opaque);
      c->result = c->capture->producer_binding(std::chrono::seconds(10));
      return 0;
    }, 0, nullptr, 0, true, &call);
  pw_thread_loop_unlock(loop);
  ASSERT_GE(invoked, 0);
  EXPECT_FALSE(call.result.verified);
  EXPECT_EQ(call.result.reason, "binding_on_pipewire_thread");
}
#endif
