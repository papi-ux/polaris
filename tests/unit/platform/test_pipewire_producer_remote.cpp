/**
 * Producer binding across processes on a real, private PipeWire server.
 *
 * The test process hosts the server (tests/fixtures/pipewire-remote-producer.conf) on a socket in a
 * directory it owns. A separate helper process owns the Video/Source. The capture under test
 * connects over its own socket through capture_t::start()'s production remote_fd path with
 * track_producer on, so its registry, core sync and credentials are the real ones. The control
 * client links source to capture as a session manager would.
 *
 * Proves: the producer is the helper's own client and PID (not the test's), the exact link, source
 * and capture ids and serials, invalidation on link removal and helper exit, a bounded timeout
 * against a stalled server, and bounded cancellation. Not compositor, portal, Deck or physical
 * acceptance.
 */
#include "../../tests_common.h"

#if defined(__linux__) && defined(POLARIS_BUILD_PORTAL)
  #if defined(POLARIS_PIPEWIRE_REMOTE_PRODUCER_PATH)
    #include <chrono>
    #include <memory>
    #include <unistd.h>

    #include "../../fixtures/pipewire_remote_producer.h"
    #include "src/platform/linux/pipewire_capture.h"

namespace {
  using namespace std::chrono_literals;

  struct remote_graph_t {
    test_utils::pipewire_remote_server_t server;
    test_utils::pipewire_remote_producer_t helper;
    std::shared_ptr<pipewire_capture::capture_t> capture;
    pw_proxy *link = nullptr;
    std::uint32_t helper_client = SPA_ID_INVALID;
    std::uint32_t capture_client = SPA_ID_INVALID;
    std::uint32_t capture_node = SPA_ID_INVALID;
    std::uint32_t link_id = SPA_ID_INVALID;

    // Every step reports where it failed, so a first failure names its cause.
    std::string start() {
      if (!server.start()) return "private server did not start or did not listen";
      if (!helper.spawn(server)) return "producer helper did not spawn or report a node";

      const int fd = server.connect_socket();
      if (fd < 0) return "capture socket did not connect";
      pipewire_capture::capture_options_t options;
      options.remote_fd = fd;
      options.node_id = helper.node;
      options.requested_width = 64;
      options.requested_height = 64;
      options.requested_rate = {60, 1};
      options.track_producer = true;
      capture = std::make_shared<pipewire_capture::capture_t>(options);
      close(fd);  // the capture keeps its own duplicate
      bool started = false;
      {
        // The capture's context loads the in-tree client config, never the host's client.conf.
        test_utils::scoped_env_t env {
          {"PIPEWIRE_CONFIG_NAME", std::string {POLARIS_SOURCE_DIR "/tests/fixtures/pipewire-pool.conf"}},
          {"PIPEWIRE_DAEMON", std::nullopt},
        };
        started = capture->start();
      }
      if (!started) return "capture did not start on the private socket";

      const auto self = getpid();
      if (!server.wait([&] {
            const auto helpers = server.clients_with_pid(helper.pid);
            auto ours = server.clients_with_pid(self);
            std::erase(ours, server.control_client());
            if (helpers.size() != 1 || ours.size() != 1) return false;
            const auto nodes = server.nodes_of_client(ours[0]);
            if (nodes.size() != 1) return false;
            helper_client = helpers[0];
            capture_client = ours[0];
            capture_node = nodes[0];
            return true;
          })) {
        return "server never showed exactly one helper client and one capture client with one node";
      }
      if (server.nodes_of_client(helper_client) != std::vector<std::uint32_t> {helper.node}) {
        return "the helper's reported node is not its client's only node";
      }

      pw_thread_loop_lock(server.loop);
      link = server.link(helper.node, capture_node);
      pw_thread_loop_unlock(server.loop);
      if (!link) return "link-factory refused the link";
      if (!server.wait([&] {
            link_id = pw_proxy_get_bound_id(link);
            return link_id != SPA_ID_INVALID && server.globals.count(link_id) == 1;
          })) {
        return "link was never published";
      }
      const auto end = std::chrono::steady_clock::now() + 5s;
      while (!capture->negotiated()) {
        if (std::chrono::steady_clock::now() >= end) return "capture never negotiated with the helper";
        std::this_thread::sleep_for(2ms);
      }
      return {};
    }

    void unlink() {
      pw_thread_loop_lock(server.loop);
      if (link) pw_proxy_destroy(link);
      link = nullptr;
      pw_thread_loop_unlock(server.loop);
    }

    ~remote_graph_t() {
      // The capture is a client of this server: it goes first, then the helper, then the server.
      if (capture) capture->shutdown();
      if (server.loop) unlink();
      helper.finish();
      server.stop();
    }
  };

  bool unbound(const capture_producer::binding_t &binding) {
    return !binding.verified && binding.producer_pid == 0 &&
           (binding.reason == "source_not_linked" || binding.reason == "capture_not_running");
  }
}  // namespace

TEST(PipeWireProducerRemoteTests, BindsTheHelpersOwnClientAndPidNotTheTests) {
  remote_graph_t graph;
  const auto failure = graph.start();
  ASSERT_TRUE(failure.empty()) << failure;

  const auto binding = graph.capture->producer_binding(2s);
  ASSERT_TRUE(binding.verified) << binding.reason;
  EXPECT_FALSE(binding.server_version.empty());

  pw_thread_loop_lock(graph.server.loop);
  const capture_producer::object_t source {graph.helper.node, graph.server.serial(graph.helper.node)};
  const capture_producer::object_t capture_node {graph.capture_node, graph.server.serial(graph.capture_node)};
  const capture_producer::object_t producer {graph.helper_client, graph.server.serial(graph.helper_client)};
  const capture_producer::object_t capture_client {graph.capture_client, graph.server.serial(graph.capture_client)};
  const capture_producer::object_t link {graph.link_id, graph.server.serial(graph.link_id)};
  pw_thread_loop_unlock(graph.server.loop);

  EXPECT_EQ(binding.source_node, source);
  EXPECT_EQ(binding.capture_node, capture_node);
  EXPECT_EQ(binding.producer_client, producer);
  EXPECT_EQ(binding.capture_client, capture_client);
  EXPECT_NE(binding.producer_client.id, binding.capture_client.id);
  EXPECT_EQ(binding.producer_pid, static_cast<std::int64_t>(graph.helper.pid));
  EXPECT_NE(binding.producer_pid, static_cast<std::int64_t>(getpid()));
  EXPECT_EQ(binding.producer_uid, static_cast<std::uint64_t>(getuid()));
  ASSERT_EQ(binding.links.size(), 1u);
  EXPECT_EQ(binding.links[0], (capture_producer::link_t {link, graph.helper.node, graph.capture_node}));
  for (const auto &object : {source, capture_node, producer, capture_client, link}) {
    EXPECT_NE(object.serial, 0u);
  }

  // An unchanged graph reads the same.
  EXPECT_EQ(graph.capture->producer_binding(2s), binding);
}

TEST(PipeWireProducerRemoteTests, RemovingTheLinkUnbindsTheProducer) {
  remote_graph_t graph;
  const auto failure = graph.start();
  ASSERT_TRUE(failure.empty()) << failure;
  ASSERT_TRUE(graph.capture->producer_binding(2s).verified);

  const auto link_id = graph.link_id;
  graph.unlink();
  ASSERT_TRUE(graph.server.wait([&] { return graph.server.globals.count(link_id) == 0; }));
  const auto binding = graph.capture->producer_binding(2s);
  EXPECT_TRUE(unbound(binding)) << binding.reason;
}

TEST(PipeWireProducerRemoteTests, TheHelperExitingUnbindsTheProducer) {
  remote_graph_t graph;
  const auto failure = graph.start();
  ASSERT_TRUE(failure.empty()) << failure;
  const auto before = graph.capture->producer_binding(2s);
  ASSERT_TRUE(before.verified) << before.reason;

  const auto status = graph.helper.finish();
  EXPECT_TRUE(WIFEXITED(status) && WEXITSTATUS(status) == 0) << "helper did not exit cleanly: " << status;
  const auto client = graph.helper_client;
  ASSERT_TRUE(graph.server.wait([&] { return graph.server.globals.count(client) == 0; }));
  const auto binding = graph.capture->producer_binding(2s);
  EXPECT_TRUE(unbound(binding)) << binding.reason;
  EXPECT_NE(binding, before);
}

TEST(PipeWireProducerRemoteTests, AStalledServerEndsTheReadAtItsBudget) {
  remote_graph_t graph;
  const auto failure = graph.start();
  ASSERT_TRUE(failure.empty()) << failure;
  ASSERT_TRUE(graph.capture->producer_binding(2s).verified);

  // Holding the server's loop lock stops it answering the capture's core sync.
  pw_thread_loop_lock(graph.server.loop);
  const auto started = std::chrono::steady_clock::now();
  const auto binding = graph.capture->producer_binding(200ms);
  const auto elapsed = std::chrono::steady_clock::now() - started;
  pw_thread_loop_unlock(graph.server.loop);

  EXPECT_FALSE(binding.verified);
  EXPECT_EQ(binding.reason, "read_timeout");
  EXPECT_EQ(binding.producer_pid, 0);
  EXPECT_GE(elapsed, 150ms);
  EXPECT_LT(elapsed, 600ms) << "the read outlived its budget";

  // The late done for that sync must not satisfy a later read; a fresh read binds again.
  const auto later = graph.capture->producer_binding(2s);
  EXPECT_TRUE(later.verified) << later.reason;
}

TEST(PipeWireProducerRemoteTests, StopAndShutdownEndReadsAtOnce) {
  remote_graph_t graph;
  const auto failure = graph.start();
  ASSERT_TRUE(failure.empty()) << failure;

  graph.capture->stop();
  const auto started = std::chrono::steady_clock::now();
  const auto stopped = graph.capture->producer_binding(10s);
  EXPECT_LT(std::chrono::steady_clock::now() - started, 1s);
  EXPECT_FALSE(stopped.verified);
  EXPECT_EQ(stopped.reason, "read_cancelled");

  graph.capture->shutdown();
  const auto after = graph.capture->producer_binding(10s);
  EXPECT_FALSE(after.verified);
  EXPECT_EQ(after.reason, "read_cancelled");
}
  #else
TEST(PipeWireProducerRemoteTests, HelperProgramIsBuilt) {
  FAIL() << "POLARIS_PIPEWIRE_REMOTE_PRODUCER_PATH is not defined: build tests/fixtures/pipewire_remote_producer.cpp "
            "and pass its path, or this cross-process test silently proves nothing.";
}
  #endif
#endif
