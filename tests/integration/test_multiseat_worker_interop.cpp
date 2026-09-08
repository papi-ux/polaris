#include <gtest/gtest.h>

#include "src/platform/linux/multiseat_worker_authority.h"
#include "src/platform/linux/multiseat_worker_client.h"
#include "src/platform/linux/multiseat_worker_coordinator.h"

#if defined(__linux__) && defined(POLARIS_MULTISEAT_GO_INTEROP_BINARY)

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <fstream>
#include <future>
#include <memory>
#include <map>
#include <spawn.h>
#include <mutex>
#include <utility>
#include <iterator>
#include <optional>
#include <signal.h>
#include <stdexcept>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {
  using namespace std::chrono_literals;
  using namespace multiseat::worker_ipc;
  using namespace multiseat;

  provider_catalog_selection_t interop_provider_selection() {
    return {
      .compositor = multiseat::compositor_e::gamescope,
      .workload = {
        .kind = multiseat::workload_kind_e::steam,
        .target_id = "native-interop-workload",
      },
    };
  }

  class temporary_tree_t {
  public:
    temporary_tree_t() {
      std::array<char, 48> pattern {};
      const std::string prefix = "/tmp/polaris-native-interop-XXXXXX";
      std::copy(prefix.begin(), prefix.end(), pattern.begin());
      const auto *created = ::mkdtemp(pattern.data());
      if (!created) {
        throw std::runtime_error {"native interop root could not be created"};
      }
      root_ = created;
      authority_ = root_ / "authority";
      state_ = root_ / "state";
      if (::chmod(root_.c_str(), 0700) != 0 ||
          ::mkdir(authority_.c_str(), 0700) != 0 ||
          ::chmod(authority_.c_str(), 0700) != 0 ||
          ::mkdir(state_.c_str(), 0700) != 0 ||
          ::chmod(state_.c_str(), 0700) != 0) {
        throw std::runtime_error {"native interop private directories could not be created"};
      }
    }

    ~temporary_tree_t() {
      std::error_code ignored;
      std::filesystem::remove_all(root_, ignored);
    }

    temporary_tree_t(const temporary_tree_t &) = delete;
    temporary_tree_t &operator=(const temporary_tree_t &) = delete;

    [[nodiscard]] const std::filesystem::path &root() const {
      return root_;
    }

    [[nodiscard]] const std::filesystem::path &authority() const {
      return authority_;
    }

    [[nodiscard]] const std::filesystem::path &state() const {
      return state_;
    }

  private:
    std::filesystem::path root_;
    std::filesystem::path authority_;
    std::filesystem::path state_;
  };

  class child_process_t {
  public:
    explicit child_process_t(pid_t pid = -1) : pid_(pid) {
    }

    ~child_process_t() {
      terminate();
    }

    child_process_t(const child_process_t &) = delete;
    child_process_t &operator=(const child_process_t &) = delete;

    [[nodiscard]] pid_t pid() const {
      return pid_;
    }

    [[nodiscard]] std::optional<int> wait_for(std::chrono::milliseconds timeout) {
      if (pid_ <= 0) {
        return std::nullopt;
      }
      const auto deadline = std::chrono::steady_clock::now() + timeout;
      while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        const auto result = ::waitpid(pid_, &status, WNOHANG);
        if (result == pid_) {
          pid_ = -1;
          return status;
        }
        if (result < 0) {
          if (errno == EINTR) {
            continue;
          }
          pid_ = -1;
          return std::nullopt;
        }
        std::this_thread::sleep_for(5ms);
      }
      return std::nullopt;
    }

    void terminate() noexcept {
      if (pid_ <= 0) {
        return;
      }
      (void) ::kill(pid_, SIGTERM);
      const auto deadline = std::chrono::steady_clock::now() + 1s;
      while (std::chrono::steady_clock::now() < deadline) {
        int status = 0;
        const auto result = ::waitpid(pid_, &status, WNOHANG);
        if (result == pid_) {
          pid_ = -1;
          return;
        }
        if (result < 0) {
          if (errno == EINTR) {
            continue;
          }
          pid_ = -1;
          return;
        }
        std::this_thread::sleep_for(5ms);
      }
      (void) ::kill(pid_, SIGKILL);
      while (::waitpid(pid_, nullptr, 0) < 0 && errno == EINTR) {
      }
      pid_ = -1;
    }

  private:
    pid_t pid_ = -1;
  };

  endpoint_identity_t interop_identity() {
    return {
      .controller_epoch = "native-interop-controller",
      .logical_gpu_id = "gpu-native-interop",
      .slot = 1,
      .generation = 71,
      .worker_name = "polaris-worker-native-interop-71",
    };
  }

  capability_factory_t interop_capability() {
    return [](capability_t &capability) {
      for (std::size_t index = 0; index < capability.size(); ++index) {
        capability[index] = static_cast<std::uint8_t>(0x61 + index);
      }
      return true;
    };
  }

  bool private_socket(const std::filesystem::path &path) {
    struct stat metadata {};
    return ::lstat(path.c_str(), &metadata) == 0 &&
           S_ISSOCK(metadata.st_mode) &&
           static_cast<std::uint32_t>(metadata.st_uid) ==
             static_cast<std::uint32_t>(::geteuid()) &&
           (metadata.st_mode & 07777) == 0600;
  }

  bool wait_for_worker(const authority_handle_t &authority, pid_t child) {
    const auto deadline = std::chrono::steady_clock::now() + 5s;
    while (std::chrono::steady_clock::now() < deadline) {
      if (private_socket(authority.paths().control_socket) &&
          private_socket(authority.paths().media_socket)) {
        return true;
      }
      if (::kill(child, 0) != 0) {
        return false;
      }
      std::this_thread::sleep_for(5ms);
    }
    return false;
  }

  std::vector<std::string> child_environment(
    const authority_handle_t &authority,
    const std::filesystem::path &state
  ) {
    const auto &identity = authority.identity();
    std::map<std::string, std::string> values;
    for (auto entry = ::environ; *entry; ++entry) {
      const std::string value {*entry};
      const auto separator = value.find('=');
      if (separator != std::string::npos) {
        values[value.substr(0, separator)] = value.substr(separator + 1);
      }
    }
    const auto set = [&](const char *name, const std::string &value) {
      values[name] = value;
    };
    set("POLARIS_NATIVE_INTEROP", "1");
    set("POLARIS_INTEROP_WORKLOAD_KIND", "steam");
    set("POLARIS_INTEROP_WORKLOAD_ID", "native-interop-workload");
    set("POLARIS_INTEROP_IPC_PATH", authority.paths().ipc.native());
    set("POLARIS_INTEROP_AUTH_PATH", authority.paths().auth.native());
    set("POLARIS_INTEROP_STATE_PATH", state.native());
    set("POLARIS_CONTROLLER_EPOCH", identity.controller_epoch);
    set("POLARIS_LOGICAL_GPU_ID", identity.logical_gpu_id);
    set("POLARIS_SEAT_SLOT", std::to_string(identity.slot));
    set("POLARIS_SEAT_GENERATION", std::to_string(identity.generation));
    set("POLARIS_WORKER_NAME", identity.worker_name);
    set("POLARIS_RUNTIME_NAMESPACE", authority.paths().generation.filename().native());
    set("POLARIS_CAPTURE_WAYLAND_DISPLAY", "capture-native-interop");
    set("WAYLAND_DISPLAY", "wayland-native-interop");
    set("PULSE_SINK", "audio-native-interop");
    set("POLARIS_INPUT_SEAT", "input-native-interop");
    set("POLARIS_RENDER_NODE", "/dev/dri/renderD128");
    set("POLARIS_COMPOSITOR", "gamescope");
    set("POLARIS_RUNTIME_PROFILE", "steam");
    set("POLARIS_DISPLAY_TOPOLOGY", "capture-host-with-nested-compositor");
    set("POLARIS_MEDIA_PIPELINE", "worker-local-capture-encode");
    set("POLARIS_DISPLAY_WIDTH", "1920");
    set("POLARIS_DISPLAY_HEIGHT", "1080");
    set("POLARIS_DISPLAY_REFRESH_MILLIHZ", "60000");
    set("POLARIS_DISPLAY_HDR", "0");
    set("POLARIS_ENCODER_SESSIONS", "1");
    std::vector<std::string> result;
    for (const auto &[name, value] : values) result.push_back(name + "=" + value);
    return result;
  }

  child_process_t launch_go_worker(
    const authority_handle_t &authority,
    const std::filesystem::path &state,
    const std::filesystem::path &log
  ) {
    // A second worker starts while the first client's reader threads run.
    // Build all strings in the parent; no allocation or setenv after fork.
    auto environment = child_environment(authority, state);
    std::vector<char *> envp;
    for (auto &entry : environment) envp.push_back(entry.data());
    envp.push_back(nullptr);
    const char *binary = POLARIS_MULTISEAT_GO_INTEROP_BINARY;
    const char *arguments[] {binary, "-test.run=^TestNativeControllerInteropServer$",
      "-test.count=1", "-test.timeout=10s", nullptr};
    posix_spawn_file_actions_t actions;
    if (::posix_spawn_file_actions_init(&actions) != 0) return child_process_t {-1};
    int status = ::posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO,
      log.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_NOFOLLOW, 0600);
    if (!status) status = ::posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    pid_t pid = -1;
    if (!status) status = ::posix_spawn(&pid, binary, &actions, nullptr,
      const_cast<char **>(arguments), envp.data());
    (void) ::posix_spawn_file_actions_destroy(&actions);
    return child_process_t {status == 0 ? pid : -1};
  }

  std::string read_log(const std::filesystem::path &path) {
    std::ifstream stream {path, std::ios::binary};
    return {
      std::istreambuf_iterator<char> {stream},
      std::istreambuf_iterator<char> {},
    };
  }

  // The backend only models authoritative inventory. Real mutually
  // authenticated Unix channels are created by the owned Go test processes.
  class interop_backend_t: public worker_backend_t {
  public:
    worker_command_result_e launch(const worker_launch_spec_t &spec) override {
      workers.push_back({spec.identity, worker_observed_state_e::ready});
      return worker_command_result_e::applied;
    }
    worker_command_result_e stop(const worker_identity_t &identity, worker_stop_mode_e) override {
      std::erase_if(workers, [&](const auto &worker) { return worker.identity == identity; });
      return worker_command_result_e::applied;
    }
    std::vector<worker_observation_t> inventory() override { return workers; }
    std::vector<worker_observation_t> workers;
  };

  class interop_control_session_t: public worker_control_session_t {
  public:
    interop_control_session_t(temporary_tree_t &tree,
      std::optional<controller_connection_t> &override_connection):
        tree_(tree), override_connection_(override_connection) {}

    ~interop_control_session_t() override { close(); }

    transport_status_e connect(const authority_handle_t &authority,
      controller_client_options_t options) override {
      const auto state = tree_.state() / authority.identity().worker_name;
      if (::mkdir(state.c_str(), 0700) != 0) return transport_status_e::io_error;
      log_ = tree_.root() / (authority.identity().worker_name + ".log");
      child_.reset(new child_process_t(launch_go_worker(authority, state, log_)));
      if (child_->pid() <= 0 || !wait_for_worker(authority, child_->pid())) {
        return transport_status_e::unavailable;
      }
      return client_.connect(authority, options);
    }
    transport_status_e heartbeat(channel_e channel) override { return client_.heartbeat(channel); }
    transport_status_e shutdown() override {
      const auto result = client_.shutdown();
      const auto status = child_->wait_for(5s);
      if (!status || !WIFEXITED(*status) || WEXITSTATUS(*status) != 0) {
        ADD_FAILURE() << read_log(log_);
        return transport_status_e::io_error;
      }
      return result;
    }
    void close() noexcept override {
      client_.close();
      if (child_) child_->terminate();
    }
    bool connected() const noexcept override { return client_.connected(); }
    controller_connection_t lease_connection() const override {
      return override_connection_.value_or(client_.lease_connection());
    }

  private:
    temporary_tree_t &tree_;
    // Only the serialized test thread mutates this override; it deliberately
    // simulates a buggy session factory handing back another live connection.
    std::optional<controller_connection_t> &override_connection_;
    std::unique_ptr<child_process_t> child_;
    std::filesystem::path log_;
    controller_client_t client_;
  };

  class WorkerConnectionAuthority: public testing::Test {
  protected:
    temporary_tree_t tree;
    authority_store_t store {tree.authority(), interop_capability()};
    registry_t registry {"interop-authority", {{
      .logical_gpu_id = "gpu-native-interop", .render_node = "/dev/dri/renderD128",
      .max_seats = 2, .max_encoder_sessions = 2,
    }}};
    interop_backend_t backend;
    std::optional<controller_connection_t> override_connection;
    worker_coordinator_t coordinator {registry, backend, store, {}, {}, [&] {
      return std::make_unique<interop_control_session_t>(tree, override_connection);
    }};

    void SetUp() override { ASSERT_TRUE(coordinator.reconcile().admission_ready); }

    seat_handle_t start(std::string client) {
      auto admitted = registry.admit({
        .client_key = client, .profile_key = client,
        .workload = {workload_kind_e::steam, "native-interop-workload"},
        .logical_gpu_id = "gpu-native-interop", .runtime_profile = runtime_profile_e::steam,
        .data_plane = {display_topology_e::capture_host_with_nested_compositor,
          media_pipeline_e::worker_local_capture_encode},
        .display_mode = {1920, 1080, 60000, false},
        .requested_compositor = compositor_e::gamescope, .encoder_sessions = 1,
      });
      EXPECT_TRUE(admitted.accepted());
      if (!admitted.seat) return {};
      const auto handle = admitted.seat->handle;
      EXPECT_EQ(registry.bind_runtime(handle, compositor_e::gamescope, "interop"), mutation_result_e::applied);
      EXPECT_EQ(coordinator.start_seat(handle), coordinator_start_result_e::started);
      EXPECT_EQ(coordinator.reconcile().broker.ready_transitions, std::size_t {1});
      return handle;
    }

    void send_fixture_input(const controller_connection_t &connection) {
      ASSERT_EQ(connection.attach_data_plane(), transport_status_e::applied);
      const std::vector<std::uint8_t> input {'n', 'a', 't', 'i', 'v', 'e', '-', 'i', 'n', 'p', 'u', 't'};
      ASSERT_EQ(connection.send_input(input), transport_status_e::applied);
      std::vector<std::uint8_t> feedback;
      ASSERT_EQ(connection.receive_feedback(feedback), transport_status_e::applied);
      EXPECT_FALSE(feedback.empty());
    }

    controller_connection_t lease(const seat_handle_t &handle) {
      controller_connection_t result;
      EXPECT_EQ(coordinator.with_authenticated_worker_connection(handle, [&](const auto &seat, const auto &connection) {
        EXPECT_EQ(seat.handle, handle);
        EXPECT_EQ(connection.identity()->worker_name, seat.worker_name);
        result = connection;
      }), worker_seat_authorization_status_e::applied);
      return result;
    }
  };

}

TEST(MultiseatWorkerInterop, NativeClientAuthenticatesRealGoWorkerOnBothChannels) {
  temporary_tree_t tree;
  authority_store_t store {tree.authority(), interop_capability()};
  auto created = store.create(
    interop_identity(),
    "native-interop-71",
    interop_provider_selection()
  );
  ASSERT_TRUE(created.created());
  auto authority = std::move(*created.authority);
  const auto log = tree.root() / "go-worker.log";
  auto child = launch_go_worker(authority, tree.state(), log);
  ASSERT_GT(child.pid(), 0);
  ASSERT_TRUE(wait_for_worker(authority, child.pid())) << read_log(log);

  controller_client_t client;
  const controller_client_options_t options {
    .connect_timeout = 2s,
    .handshake_timeout = 2s,
    .io_timeout = 2s,
  };
  ASSERT_EQ(client.connect(authority, options), transport_status_e::applied)
    << read_log(log);
  EXPECT_TRUE(client.connected());
  ASSERT_EQ(client.attach_data_plane(), transport_status_e::applied);
  EXPECT_TRUE(client.data_plane_attached());
  const std::vector<std::uint8_t> input {
    'n', 'a', 't', 'i', 'v', 'e', '-', 'i', 'n', 'p', 'u', 't',
  };
  EXPECT_EQ(client.send_input(input), transport_status_e::applied);
  std::vector<std::uint8_t> feedback;
  ASSERT_EQ(client.receive_feedback(feedback), transport_status_e::applied);
  EXPECT_EQ(
    feedback,
    (std::vector<std::uint8_t> {
      'n', 'a', 't', 'i', 'v', 'e', '-', 'f', 'e', 'e', 'd', 'b', 'a', 'c', 'k',
    })
  );
  encoded_media_packet_t media;
  ASSERT_EQ(client.receive_media(media), transport_status_e::applied);
  EXPECT_EQ(media.message, message_e::video);
  EXPECT_EQ(
    media.payload,
    (std::vector<std::uint8_t> {
      'n', 'a', 't', 'i', 'v', 'e', '-', 'v', 'i', 'd', 'e', 'o',
    })
  );
  EXPECT_EQ(client.heartbeat(channel_e::control), transport_status_e::applied);
  EXPECT_EQ(client.heartbeat(channel_e::media), transport_status_e::applied);
  EXPECT_EQ(client.shutdown(), transport_status_e::applied);
  EXPECT_FALSE(client.connected());

  const auto status = child.wait_for(5s);
  ASSERT_TRUE(status.has_value()) << read_log(log);
  ASSERT_TRUE(WIFEXITED(*status)) << read_log(log);
  EXPECT_EQ(WEXITSTATUS(*status), 0) << read_log(log);
  EXPECT_FALSE(std::filesystem::exists(authority.paths().control_socket));
  EXPECT_FALSE(std::filesystem::exists(authority.paths().media_socket));
  EXPECT_EQ(store.validate(authority), authority_status_e::applied);
  EXPECT_EQ(store.remove(authority), authority_status_e::applied);
}


TEST_F(WorkerConnectionAuthority, RetainedLeaseRetiresWithItsSeatAndLeavesOtherSeatUsable) {
  const auto a = start("client-a");
  const auto b = start("client-b");
  const auto first = lease(a);
  const auto second = lease(b);
  ASSERT_TRUE(first.connected());
  ASSERT_TRUE(second.connected());
  EXPECT_NE(first, second);
  EXPECT_FALSE(first.data_plane_attached());
  EXPECT_FALSE(second.data_plane_attached());
  send_fixture_input(first);
  send_fixture_input(second);
  ASSERT_EQ(coordinator.stop_seat(a).transport, transport_status_e::applied);
  EXPECT_FALSE(first.connected());
  encoded_media_packet_t queued;
  EXPECT_EQ(first.receive_media(queued), transport_status_e::closed);
  EXPECT_TRUE(queued.payload.empty());
  EXPECT_EQ(second.heartbeat(channel_e::control), transport_status_e::applied);
  EXPECT_EQ(second.heartbeat(channel_e::media), transport_status_e::applied);
  int calls = 0;
  EXPECT_NE(coordinator.with_authenticated_worker_connection(a, [&](const auto &, const auto &) { ++calls; }),
    worker_seat_authorization_status_e::applied);
  EXPECT_EQ(calls, 0);
  EXPECT_EQ(coordinator.stop_seat(b).transport, transport_status_e::applied);
  EXPECT_FALSE(second.connected());
}

TEST_F(WorkerConnectionAuthority, WrongOrEmptyConnectionNeverInvokesAuthorizedAction) {
  const auto a = start("client-a");
  const auto b = start("client-b");
  const auto first = lease(a);
  int calls = 0;
  const authenticated_worker_connection_action_t action = [&](const auto &, const auto &) { ++calls; };
  override_connection = first;
  EXPECT_EQ(coordinator.with_authenticated_worker_connection(b, action),
    worker_seat_authorization_status_e::endpoint_not_authenticated);
  override_connection = controller_connection_t {};
  EXPECT_EQ(coordinator.with_authenticated_worker_connection(a, action),
    worker_seat_authorization_status_e::endpoint_not_authenticated);
  EXPECT_EQ(calls, 0);
  override_connection.reset();
  const auto second = lease(b);
  EXPECT_TRUE(second.connected());
  send_fixture_input(first);
  send_fixture_input(second);
  EXPECT_EQ(coordinator.with_authenticated_worker_connection(b, [](const auto &, const auto &) {
    throw std::runtime_error {"test callback failure"};
  }), worker_seat_authorization_status_e::action_failed);
  EXPECT_EQ(coordinator.stop_seat(a).transport, transport_status_e::applied);
  EXPECT_EQ(coordinator.stop_seat(b).transport, transport_status_e::applied);
}

TEST_F(WorkerConnectionAuthority, ConnectionAuthorizationSerializesWithStop) {
  const auto seat = start("client-a");
  send_fixture_input(lease(seat));
  std::promise<void> entered;
  std::promise<void> release;
  const auto released = release.get_future().share();
  auto authorization = std::async(std::launch::async, [&] {
    return coordinator.with_authenticated_worker_connection(seat, [&](const auto &, const auto &connection) {
      EXPECT_TRUE(connection.connected());
      entered.set_value();
      EXPECT_EQ(released.wait_for(3s), std::future_status::ready);
      EXPECT_TRUE(connection.connected());
    });
  });
  ASSERT_EQ(entered.get_future().wait_for(3s), std::future_status::ready);
  auto stopped = std::async(std::launch::async, [&] { return coordinator.stop_seat(seat); });
  EXPECT_EQ(stopped.wait_for(30ms), std::future_status::timeout);
  release.set_value();
  EXPECT_EQ(authorization.get(), worker_seat_authorization_status_e::applied);
  EXPECT_EQ(stopped.get().transport, transport_status_e::applied);
}


TEST_F(WorkerConnectionAuthority, TamperedAuthorityCannotExportALiveConnection) {
  const auto seat = start("client-a");
  const auto connection = lease(seat);
  send_fixture_input(connection);
  const auto snapshot = registry.snapshot(seat);
  ASSERT_TRUE(snapshot);
  const auto record = tree.authority() / snapshot->resources.runtime_namespace /
    std::string(authority_auth_directory_name) / std::string(authority_record_file_name);
  std::fstream file {record, std::ios::in | std::ios::out | std::ios::binary};
  ASSERT_TRUE(file.is_open());
  char first = 0;
  file.get(first);
  ASSERT_TRUE(file.good());
  file.seekp(0);
  file.put(first ^ 0x01);
  file.flush();
  ASSERT_TRUE(file.good());
  int calls = 0;
  EXPECT_EQ(coordinator.with_authenticated_worker_connection(seat, [&](const auto &, const auto &) { ++calls; }),
    worker_seat_authorization_status_e::authority_rejected);
  EXPECT_EQ(calls, 0);
  EXPECT_TRUE(connection.connected());
  file.seekp(0);
  file.put(first);
  file.flush();
  ASSERT_TRUE(file.good());
  file.close();
  EXPECT_EQ(coordinator.stop_seat(seat).transport, transport_status_e::applied);
  EXPECT_FALSE(connection.connected());
}

TEST_F(WorkerConnectionAuthority, ClosedTransportCannotBeExportedFromRunningInventory) {
  const auto seat = start("client-a");
  const auto connection = lease(seat);
  connection.close();
  ASSERT_EQ(registry.snapshot(seat)->state, seat_state_e::running);
  int calls = 0;
  EXPECT_EQ(coordinator.with_authenticated_worker_connection(seat, [&](const auto &, const auto &) { ++calls; }),
    worker_seat_authorization_status_e::endpoint_not_authenticated);
  EXPECT_EQ(calls, 0);
  // The owned fixture destructor closes/reaps this disconnected Go worker.
}

#endif
