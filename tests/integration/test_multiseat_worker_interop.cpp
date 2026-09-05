#include <gtest/gtest.h>

#include "src/platform/linux/multiseat_worker_authority.h"
#include "src/platform/linux/multiseat_worker_client.h"

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

namespace {
  using namespace std::chrono_literals;
  using namespace multiseat::worker_ipc;

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

  void set_child_environment(
    const authority_handle_t &authority,
    const std::filesystem::path &state
  ) {
    const auto &identity = authority.identity();
    const auto set = [](const char *name, const std::string &value) {
      if (::setenv(name, value.c_str(), 1) != 0) {
        _exit(126);
      }
    };
    set("POLARIS_NATIVE_INTEROP", "1");
    set("POLARIS_INTEROP_WORKLOAD_KEY", "native-interop-workload");
    set("POLARIS_INTEROP_IPC_PATH", authority.paths().ipc.native());
    set("POLARIS_INTEROP_AUTH_PATH", authority.paths().auth.native());
    set("POLARIS_INTEROP_STATE_PATH", state.native());
    set("POLARIS_CONTROLLER_EPOCH", identity.controller_epoch);
    set("POLARIS_LOGICAL_GPU_ID", identity.logical_gpu_id);
    set("POLARIS_SEAT_SLOT", std::to_string(identity.slot));
    set("POLARIS_SEAT_GENERATION", std::to_string(identity.generation));
    set("POLARIS_WORKER_NAME", identity.worker_name);
    set("POLARIS_RUNTIME_NAMESPACE", authority.paths().generation.filename().native());
    set("WAYLAND_DISPLAY", "wayland-native-interop");
    set("PULSE_SINK", "audio-native-interop");
    set("POLARIS_INPUT_SEAT", "input-native-interop");
    set("POLARIS_RENDER_NODE", "/dev/dri/renderD128");
    set("POLARIS_COMPOSITOR", "gamescope");
    set("POLARIS_RUNTIME_PROFILE", "steam");
    set("POLARIS_DISPLAY_WIDTH", "1920");
    set("POLARIS_DISPLAY_HEIGHT", "1080");
    set("POLARIS_DISPLAY_REFRESH_MILLIHZ", "60000");
    set("POLARIS_DISPLAY_HDR", "0");
    set("POLARIS_ENCODER_SESSIONS", "1");
  }

  child_process_t launch_go_worker(
    const authority_handle_t &authority,
    const std::filesystem::path &state,
    const std::filesystem::path &log
  ) {
    const auto pid = ::fork();
    if (pid != 0) {
      return child_process_t {pid};
    }
    const auto log_fd = ::open(
      log.c_str(),
      O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC,
      0600
    );
    if (log_fd < 0 ||
        ::dup2(log_fd, STDOUT_FILENO) < 0 ||
        ::dup2(log_fd, STDERR_FILENO) < 0) {
      _exit(126);
    }
    (void) ::close(log_fd);
    set_child_environment(authority, state);
    const char *binary = POLARIS_MULTISEAT_GO_INTEROP_BINARY;
    ::execl(
      binary,
      binary,
      "-test.run=^TestNativeControllerInteropServer$",
      "-test.count=1",
      "-test.timeout=10s",
      static_cast<char *>(nullptr)
    );
    _exit(127);
  }

  std::string read_log(const std::filesystem::path &path) {
    std::ifstream stream {path, std::ios::binary};
    return {
      std::istreambuf_iterator<char> {stream},
      std::istreambuf_iterator<char> {},
    };
  }
}

TEST(MultiseatWorkerInterop, NativeClientAuthenticatesRealGoWorkerOnBothChannels) {
  temporary_tree_t tree;
  authority_store_t store {tree.authority(), interop_capability()};
  auto created = store.create(interop_identity(), "native-interop-71");
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

#endif
