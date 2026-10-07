#ifdef __linux__
#include <src/platform/linux/game_mode_feedback.h>
#include <gtest/gtest.h>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <spawn.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>
#ifdef POLARIS_BUILD_X11_XCB
#include <xcb/xcb.h>
#endif

using namespace std::chrono_literals;
extern char **environ;

namespace {
  capture_producer::binding_t binding(pid_t pid) {
    capture_producer::binding_t result;
    result.verified = true;
    result.reason = "verified";
    result.producer_pid = pid;
    result.producer_uid = getuid();
    result.source_node = {21, 121};
    result.capture_node = {22, 122};
    result.producer_client = {20, 120};
    result.capture_client = {19, 119};
    result.links = {{{23, 123}, 21, 22}};
    return result;
  }

  class GameModeFeedbackTests: public testing::Test {
  protected:
    std::filesystem::path scratch;
    platf::game_mode_feedback::probe_t probe;
    void SetUp() override {
      char pattern[] = "/tmp/gmfeed-XXXXXX";
      const auto *directory = mkdtemp(pattern);
      ASSERT_NE(directory, nullptr);
      scratch = directory;
      probe.socket_dir = scratch;
      probe.os_release = scratch / "os-release";
      std::ofstream(probe.os_release) << "ID=steamos\nBUILD_ID=fixture-build\nVERSION_ID=fixture-version\n";
    }
    void TearDown() override { std::error_code error; std::filesystem::remove_all(scratch, error); }
  };
}

TEST_F(GameModeFeedbackTests, UnlinkedOrAmbiguousGraphNeverProbesASessionDisplay) {
  int calls = 0;
  const auto result = platf::game_mode_feedback::collect_once([&](auto) {
    ++calls; capture_producer::binding_t value; value.reason = "source_ambiguous"; return value;
  }, probe);
  EXPECT_EQ(calls, 1);
  EXPECT_EQ(result.reason, "source_ambiguous");
  EXPECT_FALSE(result.route_bound);
}

TEST_F(GameModeFeedbackTests, UnreadableGenerationCannotSubstituteProcessOrNodeNames) {
  probe.proc_root = scratch / "unavailable-proc";
  const auto result = platf::game_mode_feedback::collect_once([](auto) { return binding(getpid()); }, probe);
  EXPECT_EQ(result.reason, "producer_generation_unreadable");
  EXPECT_FALSE(result.route_bound);
}

TEST_F(GameModeFeedbackTests, CaptureObserverIsJoinableAndRetiresItsAcceptedImageReader) {
  std::atomic<unsigned> calls = 0;
  std::shared_ptr<const game_mode_adaptive_sync::store_t> reader;
  {
    platf::game_mode_feedback::collector_t collector([&](auto) {
      ++calls; capture_producer::binding_t value; value.reason = "source_not_linked"; return value;
    }, probe);
    reader = collector.reader();
    const auto deadline = std::chrono::steady_clock::now() + 1s;
    while (calls.load() == 0 && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(1ms);
    EXPECT_GT(calls.load(), 0u);
  }
  EXPECT_TRUE(reader->read().ended);
  const auto count = calls.load(); std::this_thread::sleep_for(10ms);
  EXPECT_EQ(calls.load(), count);
}

#ifdef POLARIS_BUILD_X11_XCB
TEST_F(GameModeFeedbackTests, MoreThanEightDisplayCandidatesAreAmbiguousWithoutOpeningAny) {
  for (unsigned index = 0; index < 9; ++index) std::ofstream(scratch / ("X" + std::to_string(index))) << "fixture";
  const auto result = platf::game_mode_feedback::collect_once([](auto) { return binding(getpid()); }, probe);
  EXPECT_EQ(result.reason, "root_display_ambiguous");
  EXPECT_FALSE(result.route_bound);
}

TEST_F(GameModeFeedbackTests, StalledXSetupIsInterruptedAtTheSocketAndNeverDetachesAReader) {
  const auto socket_path = (scratch / "X777").string();
  const int listener = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
  ASSERT_GE(listener, 0);
  sockaddr_un address {}; address.sun_family = AF_UNIX;
  ASSERT_LT(socket_path.size(), sizeof(address.sun_path));
  std::memcpy(address.sun_path, socket_path.c_str(), socket_path.size() + 1);
  ASSERT_EQ(bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)), 0);
  ASSERT_EQ(listen(listener, 1), 0);
  std::atomic<bool> closed = false;
  std::thread server([&] {
    pollfd waiting {listener, POLLIN, 0};
    if (poll(&waiting, 1, 1000) <= 0) return;
    const int connection = accept(listener, nullptr, nullptr);
    if (connection >= 0) {
      char bytes[256];
      while (recv(connection, bytes, sizeof(bytes), 0) > 0) {}
      close(connection); closed = true;
    }
  });
  const auto start = std::chrono::steady_clock::now();
  const auto result = platf::game_mode_feedback::collect_once([](auto) { return binding(getpid()); }, probe);
  server.join(); close(listener);
  const auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_EQ(result.reason, "x_read_timeout");
  EXPECT_FALSE(result.route_bound);
  EXPECT_TRUE(closed.load());
  EXPECT_GE(elapsed, 200ms);
  EXPECT_LT(elapsed, 1500ms);  // Scheduling margin, not the production deadline.
}

namespace {
  class GameModeFeedbackXTests: public GameModeFeedbackTests {
  protected:
    pid_t server = -1;
    std::string display;
    xcb_connection_t *x = nullptr;
    xcb_window_t root = 0;
    xcb_atom_t id = 0, capable = 0, feedback = 0;
    xcb_atom_t key(const char *name) {
      auto *reply = xcb_intern_atom_reply(x, xcb_intern_atom(x, 0, std::strlen(name), name), nullptr);
      if (!reply) return 0;
      const auto value = reply->atom; free(reply); return value;
    }
    void write(xcb_atom_t atom, std::uint32_t value, xcb_atom_t type = XCB_ATOM_CARDINAL) {
      xcb_change_property(x, XCB_PROP_MODE_REPLACE, root, atom, type, 32, 1, &value);
      auto *reply = xcb_get_input_focus_reply(x, xcb_get_input_focus(x), nullptr);
      ASSERT_NE(reply, nullptr); free(reply);
    }
    void SetUp() override {
      GameModeFeedbackTests::SetUp();
      ASSERT_FALSE(scratch.empty());
      // No ambient DISPLAY, auth cookie or daemon is consumed: this fixture owns this server.
      const auto number = std::to_string(20000 + getpid() % 10000);
      display = ":" + number;
      const auto socket_path = std::filesystem::path("/tmp/.X11-unix") / ("X" + number);
      ASSERT_FALSE(std::filesystem::exists(socket_path)) << "refuse to borrow an existing server";
      std::vector<std::string> args {"Xvfb", display, "-screen", "0", "640x480x24", "-nolisten", "tcp", "-ac"};
      std::vector<char *> argv; for (auto &argument : args) argv.push_back(argument.data()); argv.push_back(nullptr);
      ASSERT_EQ(posix_spawnp(&server, "Xvfb", nullptr, nullptr, argv.data(), environ), 0) << "Xvfb required in disposable Linux qualification job";
      const auto deadline = std::chrono::steady_clock::now() + 2s;
      while (!std::filesystem::exists(socket_path) && std::chrono::steady_clock::now() < deadline) std::this_thread::sleep_for(5ms);
      ASSERT_TRUE(std::filesystem::exists(socket_path));
      std::filesystem::create_symlink(socket_path, scratch / ("X" + number));
      x = xcb_connect(display.c_str(), nullptr);
      ASSERT_NE(x, nullptr); ASSERT_EQ(xcb_connection_has_error(x), 0);
      root = xcb_setup_roots_iterator(xcb_get_setup(x)).data->root;
      id = key("GAMESCOPE_XWAYLAND_SERVER_ID"); capable = key("GAMESCOPE_VRR_CAPABLE"); feedback = key("GAMESCOPE_VRR_FEEDBACK");
      ASSERT_NE(id, 0u); ASSERT_NE(capable, 0u); ASSERT_NE(feedback, 0u);
      write(id, 0); write(capable, 1); write(feedback, 1);
    }
    void TearDown() override {
      if (x) xcb_disconnect(x);
      if (server > 1) { kill(server, SIGTERM); int status; waitpid(server, &status, 0); }
      GameModeFeedbackTests::TearDown();
    }
    auto collect() { return platf::game_mode_feedback::collect_once([&](auto) { return binding(server); }, probe); }
  };
}

TEST_F(GameModeFeedbackXTests, ReadsOnlyTheCredentialBoundRootZeroAndReportsNoCurrentState) {
  const auto value = collect();
  ASSERT_TRUE(value.route_bound) << value.reason;
  EXPECT_EQ(value.reason, "property_read_bracketed");
  EXPECT_EQ(value.properties.feedback.value, 1u);
  EXPECT_EQ(value.version.os_release.at("BUILD_ID"), "fixture-build");
  EXPECT_FALSE(value.connector_bound);
  game_mode_adaptive_sync::store_t store; store.publish(value);
  EXPECT_STREQ(game_mode_adaptive_sync::evaluate(store.read(), std::chrono::steady_clock::now()).current_state, "unknown");
}

TEST_F(GameModeFeedbackXTests, NodeNamesOrMatchingRootPropertiesCannotReplaceSocketPeerIdentity) {
  const auto value = platf::game_mode_feedback::collect_once([](auto) { return binding(getpid()); }, probe);
  EXPECT_FALSE(value.route_bound);
  EXPECT_EQ(value.reason, "root_display_not_found");
}

TEST_F(GameModeFeedbackXTests, UnreadableExecutablePreservesBoundFeedbackWithUnknownVersionProvenance) {
  probe.proc_root = scratch / "proc";
  const auto producer = probe.proc_root / std::to_string(server);
  std::filesystem::create_directories(producer);
  for (const auto *name : {"stat", "status"}) {
    std::ifstream source(std::filesystem::path("/proc") / std::to_string(server) / name);
    std::ofstream destination(producer / name); destination << source.rdbuf();
  }
  std::filesystem::create_symlink(scratch / "not-readable", producer / "exe");
  const auto value = collect();
  ASSERT_TRUE(value.route_bound) << value.reason;
  EXPECT_EQ(value.reason, "property_read_bracketed");
  EXPECT_TRUE(value.version.producer_exe.empty());
  EXPECT_FALSE(value.version.captured());
  EXPECT_EQ(value.generation.exe_inode, 0u);
}

TEST_F(GameModeFeedbackXTests, SecondaryRootOneCannotStandInForRootZero) {
  write(id, 1);
  const auto value = collect();
  EXPECT_FALSE(value.route_bound); EXPECT_EQ(value.reason, "root_display_not_found");
}

TEST_F(GameModeFeedbackXTests, ExactWirePropertySchemaRejectsNonCardinalOutOfDomainAndContradictoryPairs) {
  write(feedback, 1, XCB_ATOM_INTEGER);
  EXPECT_EQ(collect().reason, "property_malformed");
  write(feedback, 2);
  EXPECT_EQ(collect().reason, "property_out_of_domain");
  write(feedback, 1); write(capable, 0);
  EXPECT_EQ(collect().reason, "contradictory_properties");
}

TEST_F(GameModeFeedbackXTests, ZeroPairWithoutAnIndependentlyBoundConnectorStaysUnknown) {
  write(capable, 0); write(feedback, 0);
  const auto value = collect();
  EXPECT_EQ(value.reason, "connector_unbound");
  game_mode_adaptive_sync::store_t store; store.publish(value);
  EXPECT_FALSE(store.read().last_known);
}

TEST_F(GameModeFeedbackXTests, SecondPipeWireBarrierRejectsCaptureRelinkEvenWithReadableActiveProperties) {
  unsigned calls = 0;
  const auto value = platf::game_mode_feedback::collect_once([&](auto) {
    auto result = binding(server); if (++calls == 2) ++result.source_node.serial; return result;
  }, probe);
  EXPECT_EQ(calls, 2u);
  EXPECT_FALSE(value.route_bound); EXPECT_FALSE(value.generation_checked);
  EXPECT_EQ(value.reason, "pipewire_graph_changed");
}
#endif
#endif
