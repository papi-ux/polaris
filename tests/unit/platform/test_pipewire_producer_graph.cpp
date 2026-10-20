/**
 * Producer binding on a real, private, in-process PipeWire graph (tests/fixtures/pipewire-pool.conf).
 *
 * The fixture's producer stream and the capture stream share one in-process core, so the producer
 * client IS the capture client here and its credentials are this process's. A late observer lacks runtime Core version evidence on this non-daemon fixture, so it must
 * remain unbound. These tests cover that refusal, opt-in cost, cancellation and wrong-thread
 * handling. The separate-process fixture covers actual credential binding and link removal. It is not a
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

TEST(PipeWireProducerGraphTests, LateInProcessObserverCannotInventAMissingRuntimeVersion) {
  test_utils::pipewire_producer_graph_t graph {false, 64, 64};
  ASSERT_TRUE(graph.start());
  ASSERT_TRUE(tracking::track(*graph.capture));
  // This non-daemon fixture already completed hello before tracking was installed. It publishes
  // no Core global version and does not replay core.info to the late listener. Never substitute
  // the linked library version for a server observation. The remote fixture tests real binding.
  const auto binding = graph.capture->producer_binding(2s);
  EXPECT_FALSE(binding.verified);
  EXPECT_TRUE(binding.server_version.empty());
  EXPECT_EQ(binding.reason, "server_version_unsupported");
  EXPECT_EQ(binding.producer_pid, 0);
}

TEST(PipeWireProducerGraphTests, RemovingAnInProcessLinkCannotManufactureProducerIdentity) {
  test_utils::pipewire_producer_graph_t graph {false, 64, 64};
  ASSERT_TRUE(graph.start());
  ASSERT_TRUE(tracking::track(*graph.capture));
  EXPECT_EQ(graph.capture->producer_binding(2s).reason, "server_version_unsupported");
  pw_thread_loop_lock(access::loop(*graph.capture));
  pw_proxy_destroy(graph.link);
  graph.link = nullptr;
  pw_thread_loop_unlock(access::loop(*graph.capture));
  const auto binding = graph.capture->producer_binding(2s);
  EXPECT_FALSE(binding.verified);
  EXPECT_EQ(binding.producer_pid, 0);
  EXPECT_TRUE(binding.reason == "server_version_unsupported" || binding.reason == "capture_not_running") << binding.reason;
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
