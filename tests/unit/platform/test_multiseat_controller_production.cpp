/**
 * @file tests/unit/platform/test_multiseat_controller_production.cpp
 * @brief Offline tests for default-off production multiseat composition.
 */
#include "src/platform/linux/multiseat_controller_production.h"
#include "src/platform/linux/multiseat_moonlight_activation.h"

#ifdef __linux__

  #include <algorithm>
  #include <array>
  #include <chrono>
  #include <cstddef>
  #include <cstdint>
  #include <cstdlib>
  #include <filesystem>
  #include <gtest/gtest.h>
  #include <map>
  #include <nlohmann/json.hpp>
  #include <memory>
  #include <mutex>
  #include <optional>
  #include <stdexcept>
  #include <string>
  #include <string_view>
  #include <sys/stat.h>
  #include <utility>
  #include <vector>

namespace {
  using namespace multiseat;
  namespace input = multiseat::input;
  namespace podman = multiseat::podman;

  class temporary_production_root_t {
  public:
    temporary_production_root_t() {
      std::array<char, 64> pattern {};
      const std::string prefix = "/tmp/polaris-controller-production-XXXXXX";
      std::copy(prefix.begin(), prefix.end(), pattern.begin());
      const auto *created = ::mkdtemp(pattern.data());
      if (!created) {
        throw std::runtime_error {
          "temporary production root could not be created"
        };
      }
      path_ = created;
      if (::chmod(path_.c_str(), 0700) != 0) {
        throw std::runtime_error {
          "temporary production root mode could not be set"
        };
      }
    }

    ~temporary_production_root_t() {
      std::error_code ignored;
      std::filesystem::remove_all(path_, ignored);
    }

    temporary_production_root_t(const temporary_production_root_t &) = delete;
    temporary_production_root_t &operator=(
      const temporary_production_root_t &
    ) = delete;

    [[nodiscard]] const std::filesystem::path &path() const {
      return path_;
    }

  private:
    std::filesystem::path path_;
  };

  input::allocation_t production_allocation(
    const input::expectation_t &expectation
  ) {
    input::allocation_t allocation {
      .handle = expectation.handle,
      .input_seat = expectation.input_seat,
      .plan = expectation.plan,
    };
    const std::vector<std::pair<input::device_kind_e, std::uint32_t>> devices {
      {input::device_kind_e::keyboard, 0},
      {input::device_kind_e::mouse_relative, 0},
      {input::device_kind_e::mouse_absolute, 0},
    };
    for (std::size_t index = 0; index < devices.size(); ++index) {
      const auto [kind, slot] = devices[index];
      allocation.nodes.push_back({
        .kind = kind,
        .slot = slot,
        .host_path = "/dev/input/event" + std::to_string(256 + index),
        .worker_path = input::expected_worker_path(kind, slot),
        .filesystem_device = 73,
        .inode = 18000 + index,
        .character_major = 13,
        .character_minor = static_cast<std::uint32_t>(256 + index),
        .kernel_name = input::expected_kernel_name(
          expectation.input_seat,
          kind,
          slot
        ),
        .host_seat = std::string {input::isolated_host_seat},
      });
    }
    return allocation;
  }

  struct production_input_state_t {
    std::mutex mutex;
    std::vector<input::allocation_t> allocations;
    std::size_t inventory_calls = 0;
  };

  class production_input_backend_t final : public input::backend_t {
  public:
    explicit production_input_backend_t(
      std::shared_ptr<production_input_state_t> state
    ) :
        state_(std::move(state)) {
    }

    input::backend_create_result_t create(
      const input::expectation_t &expectation
    ) override {
      std::scoped_lock lock {state_->mutex};
      auto allocation = production_allocation(expectation);
      state_->allocations.push_back(allocation);
      return {
        .result = input::backend_result_e::applied,
        .allocation = std::move(allocation),
      };
    }

    input::backend_result_e destroy(
      const seat_handle_t &handle,
      std::string_view input_seat
    ) override {
      std::scoped_lock lock {state_->mutex};
      const auto found = std::find_if(
        state_->allocations.begin(),
        state_->allocations.end(),
        [&handle, input_seat](const auto &candidate) {
          return candidate.handle == handle &&
                 candidate.input_seat == input_seat;
        }
      );
      if (found == state_->allocations.end()) {
        return input::backend_result_e::not_found;
      }
      state_->allocations.erase(found);
      return input::backend_result_e::applied;
    }

    input::backend_result_e route(
      const seat_handle_t &,
      std::string_view,
      std::uint64_t,
      const input::input_event_t &
    ) override {
      return input::backend_result_e::applied;
    }

    std::vector<input::allocation_t> inventory() override {
      std::scoped_lock lock {state_->mutex};
      ++state_->inventory_calls;
      return state_->allocations;
    }

  private:
    std::shared_ptr<production_input_state_t> state_;
  };

  struct production_host_state_t {
    std::mutex mutex;
    std::vector<std::vector<std::string>> commands;
    std::size_t probe_calls = 0;
    std::size_t device_identity_calls = 0;
    std::size_t runtime_spec_reads = 0;

    /**
     * Opt-in offline Podman emulation: `run` records a container from its own
     * argv, `ps` and `container inspect` replay it, `kill` marks it exited and
     * keeps it listed, `rm` removes it. Off by default so the other tests keep
     * observing an empty deployment.
     */
    struct simulated_container_t {
      std::string id;
      std::string name;
      std::string status;
      std::vector<std::pair<std::string, std::string>> labels;
      std::vector<std::array<std::string, 3>> devices;
      std::vector<std::array<std::string, 3>> binds;
      std::vector<std::array<std::string, 2>> volumes;
      std::vector<std::string> tmpfs;
      bool init = false;
    };
    bool simulate_containers = false;
    bool observe_allocated_input_nodes = false;
    std::vector<simulated_container_t> containers;

    std::map<std::string, podman::character_device_identity_t>
      character_devices {
        {
          "/dev/dri/renderD128",
          {
            .filesystem_device = 81,
            .inode = 19000,
            .character_major = 226,
            .character_minor = 128,
          },
        },
        {
          "/dev/dri/card0",
          {
            .filesystem_device = 81,
            .inode = 19001,
            .character_major = 226,
            .character_minor = 0,
          },
        },
        {
          "/dev/dri/renderD129",
          {
            .filesystem_device = 81,
            .inode = 19002,
            .character_major = 226,
            .character_minor = 129,
          },
        },
        {
          "/dev/dri/card1",
          {
            .filesystem_device = 81,
            .inode = 19003,
            .character_major = 226,
            .character_minor = 1,
          },
        },
      };
  };

  /**
   * Rootless Podman 5.8 shape: `container inspect` reports no devices in
   * `HostConfig` and points at the OCI runtime spec, whose bind mounts are
   * the only record of the `--device` bindings the runtime applied.
   */
  std::string runtime_spec_path_for(const std::string &id) {
    return "/srv/seat-operator/containers/storage/overlay-containers/" +
           id + "/userdata/config.json";
  }

  nlohmann::json runtime_spec_for(
    const production_host_state_t::simulated_container_t &container
  ) {
    const auto userdata =
      "/srv/seat-operator/containers/storage/overlay-containers/" +
      container.id + "/userdata";
    const auto run_userdata =
      "/run/user/1000/containers/overlay-containers/" + container.id + "/userdata";
    const auto podman_bind = [](std::string destination, std::string source) {
      return nlohmann::json {
        {"destination", std::move(destination)},
        {"type", "bind"},
        {"source", std::move(source)},
        {"options", {"bind", "rprivate"}},
      };
    };
    auto mounts = nlohmann::json::array({
      {
        {"destination", "/proc"},
        {"type", "proc"},
        {"source", "proc"},
        {"options", {"nosuid", "noexec", "nodev"}},
      },
      {
        {"destination", "/dev"},
        {"type", "tmpfs"},
        {"source", "tmpfs"},
        {"options", {"nosuid", "strictatime", "mode=755", "size=65536k"}},
      },
      {
        {"destination", "/sys"},
        {"type", "sysfs"},
        {"source", "sysfs"},
        {"options", {"nosuid", "noexec", "nodev", "ro"}},
      },
      {
        {"destination", "/dev/pts"},
        {"type", "devpts"},
        {"source", "devpts"},
        {"options", {"nosuid", "noexec", "newinstance", "ptmxmode=0666", "mode=0620", "gid=5"}},
      },
      {
        {"destination", "/dev/mqueue"},
        {"type", "mqueue"},
        {"source", "mqueue"},
        {"options", {"nosuid", "noexec", "nodev"}},
      },
      podman_bind("/etc/resolv.conf", run_userdata + "/resolv.conf"),
      podman_bind("/etc/hosts", run_userdata + "/hosts"),
      {
        {"destination", "/dev/shm"},
        {"type", "bind"},
        {"source", userdata + "/shm"},
        {"options", {"bind", "rprivate", "nosuid", "noexec", "nodev"}},
      },
      podman_bind("/run/.containerenv", run_userdata + "/.containerenv"),
      podman_bind("/run/secrets", run_userdata + "/run/secrets"),
      podman_bind("/etc/hostname", run_userdata + "/hostname"),
      {
        {"destination", "/sys/fs/cgroup"},
        {"type", "cgroup"},
        {"source", "cgroup"},
        {"options", {"rprivate", "nosuid", "noexec", "nodev", "relatime", "ro"}},
      },
    });
    if (container.init) {
      mounts.push_back({
        {"destination", "/run/podman-init"},
        {"type", "bind"},
        {"source", "/usr/libexec/podman/catatonit"},
        {"options", {"bind", "ro", "private"}},
      });
    }
    for (const auto &destination : container.tmpfs) {
      mounts.push_back({
        {"destination", destination},
        {"type", "tmpfs"},
        {"source", "tmpfs"},
        {"options", {"rw", "rprivate", "nosuid", "nodev", "tmpcopyup"}},
      });
    }
    for (const auto &volume : container.volumes) {
      mounts.push_back({
        {"destination", volume.at(1)},
        {"type", "bind"},
        {"source", "/srv/seat-operator/containers/storage/volumes/" + volume.at(0) + "/_data"},
        {"options", {"rw", "rprivate", "nosuid", "nodev", "rbind"}},
      });
    }
    for (const auto &bind : container.binds) {
      mounts.push_back({
        {"destination", bind.at(1)},
        {"type", "bind"},
        {"source", bind.at(0)},
        {"options", {bind.at(2), "bind", "private", "nosuid", "nodev"}},
      });
    }
    for (const auto &device : container.devices) {
      mounts.push_back({
        {"destination", device.at(1)},
        {"type", "bind"},
        {"source", device.at(0)},
        {"options", {"slave", "nosuid", "noexec", device.at(2), "rbind"}},
      });
    }
    return {
      {"ociVersion", "1.2.0"},
      {"annotations", {{"run.oci.keep_original_groups", "1"}}},
      {"process", {{"user", {{"uid", 1000}}}}},
      {"mounts", std::move(mounts)},
      {"linux", nlohmann::json::object()},
    };
  }

  class production_fake_host_t final : public podman::host_t {
  public:
    explicit production_fake_host_t(
      std::shared_ptr<production_host_state_t> state
    ) :
        state_(std::move(state)) {
    }

    std::uint64_t effective_uid() const override {
      return 1000;
    }

    bool executable_file(const std::filesystem::path &path) const override {
      return path == "/usr/bin/podman" || path == "/usr/libexec/podman/catatonit";
    }

    bool trusted_runtime_file(const std::filesystem::path &path) const override {
      return path == "/usr/bin/crun";
    }

    std::optional<std::vector<std::uint64_t>> supplementary_groups() const override {
      return std::vector<std::uint64_t> {104, 105};
    }

    bool readable_directory(const std::filesystem::path &) const override {
      return true;
    }

    bool private_read_write_directory(
      const std::filesystem::path &
    ) const override {
      return true;
    }

    bool private_readable_file(const std::filesystem::path &) const override {
      return true;
    }

    std::optional<podman::character_device_identity_t>
    read_write_character_device(
      const std::filesystem::path &path
    ) const override {
      std::scoped_lock lock {state_->mutex};
      ++state_->device_identity_calls;
      const auto found = state_->character_devices.find(path.native());
      return found == state_->character_devices.end() ?
               std::nullopt : std::optional {found->second};
    }

    std::optional<std::string> read_owned_regular_file(
      const std::filesystem::path &path,
      std::size_t max_bytes
    ) const override {
      std::scoped_lock lock {state_->mutex};
      ++state_->runtime_spec_reads;
      for (const auto &container : state_->containers) {
        if (runtime_spec_path_for(container.id) != path.native()) {
          continue;
        }
        auto text = runtime_spec_for(container).dump();
        if (text.size() > max_bytes) {
          return std::nullopt;
        }
        return text;
      }
      return std::nullopt;
    }

    podman::command_result_t run(
      const std::vector<std::string> &argv,
      std::chrono::milliseconds,
      std::size_t
    ) override {
      std::scoped_lock lock {state_->mutex};
      state_->commands.push_back(argv);
      if (!state_->simulate_containers || argv.size() < 3) {
        return {
          .exit_status = 0,
          .output = {},
        };
      }
      auto command = argv;
      if (command.at(2) == "--runtime=/usr/bin/crun") command.erase(command.begin() + 2);
      return simulate_locked(command);
    }

  private:
    using simulated_container_t =
      production_host_state_t::simulated_container_t;

    std::vector<simulated_container_t>::iterator find_container_locked(
      const std::string &id
    ) {
      return std::find_if(
        state_->containers.begin(),
        state_->containers.end(),
        [&id](const auto &container) {
          return container.id == id;
        }
      );
    }

    podman::command_result_t simulate_locked(
      const std::vector<std::string> &argv
    ) {
      const podman::command_result_t failure {.exit_status = 125};
      const auto &verb = argv.at(2);
      if (verb == "run") {
        simulated_container_t container {
          .id = std::string(
            64,
            static_cast<char>('1' + state_->containers.size())
          ),
          .status = "running",
        };
        for (const auto &argument : argv) {
          if (argument.starts_with("--name=")) {
            container.name = argument.substr(std::string_view {"--name="}.size());
          } else if (argument.starts_with("--label=")) {
            const auto label = argument.substr(std::string_view {"--label="}.size());
            const auto separator = label.find('=');
            if (separator == std::string::npos) {
              return failure;
            }
            container.labels.emplace_back(
              label.substr(0, separator),
              label.substr(separator + 1)
            );
          } else if (argument.starts_with("--device=")) {
            const auto binding = argument.substr(std::string_view {"--device="}.size());
            const auto first = binding.find(':');
            const auto second = first == std::string::npos ?
                                  std::string::npos :
                                  binding.find(':', first + 1);
            if (first == std::string::npos || second == std::string::npos) {
              return failure;
            }
            container.devices.push_back({
              binding.substr(0, first),
              binding.substr(first + 1, second - first - 1),
              binding.substr(second + 1),
            });
          } else if (argument.starts_with("--mount=type=bind,src=")) {
            const auto rest = argument.substr(std::string_view {"--mount=type=bind,src="}.size());
            const auto separator = rest.find(",dst=");
            if (separator == std::string::npos) {
              return failure;
            }
            const auto tail = rest.substr(separator + std::string_view {",dst="}.size());
            container.binds.push_back({
              rest.substr(0, separator),
              tail.substr(0, tail.find(',')),
              tail.find(",ro=true") == std::string::npos ? "rw" : "ro",
            });
          } else if (argument == "--init") {
            container.init = true;
          } else if (argument.starts_with("--mount=type=tmpfs,dst=")) {
            const auto rest = argument.substr(std::string_view {"--mount=type=tmpfs,dst="}.size());
            container.tmpfs.push_back(rest.substr(0, rest.find(',')));
          } else if (argument.starts_with("--volume=")) {
            const auto binding = argument.substr(std::string_view {"--volume="}.size());
            const auto first = binding.find(':');
            if (first == std::string::npos) {
              return failure;
            }
            const auto tail = binding.substr(first + 1);
            container.volumes.push_back({
              binding.substr(0, first),
              tail.substr(0, tail.find(':')),
            });
          }
        }
        state_->containers.push_back(std::move(container));
        return {
          .exit_status = 0,
          .output = state_->containers.back().id + "\n",
        };
      }
      if (verb == "ps") {
        std::string ids;
        for (const auto &container : state_->containers) {
          ids += container.id + "\n";
        }
        return {
          .exit_status = 0,
          .output = std::move(ids),
        };
      }
      if (verb == "container" && argv.size() > 4 && argv.at(3) == "inspect") {
        auto document = nlohmann::json::array();
        for (std::size_t index = 4; index < argv.size(); ++index) {
          const auto found = find_container_locked(argv.at(index));
          if (found == state_->containers.end()) {
            return failure;
          }
          auto labels = nlohmann::json::object();
          for (const auto &[key, value] : found->labels) {
            labels[key] = value;
          }
          document.push_back({
            {"Id", found->id},
            {"Name", found->name},
            {"Config", {{"Labels", std::move(labels)}, {"User", "1000"}}},
            {"OCIRuntime", "/usr/bin/crun"},
            {"HostConfig", {{"Devices", nlohmann::json::array()}, {"GroupAdd", nlohmann::json::array()}}},
            {"OCIConfigPath", runtime_spec_path_for(found->id)},
            {"State", {{"Status", found->status}}},
          });
        }
        return {
          .exit_status = 0,
          .output = document.dump(),
        };
      }
      if (verb == "kill" || verb == "rm") {
        const auto found = find_container_locked(argv.back());
        if (found == state_->containers.end()) {
          return failure;
        }
        if (verb == "kill") {
          found->status = "exited";
        } else {
          state_->containers.erase(found);
        }
        return {
          .exit_status = 0,
          .output = {},
        };
      }
      return {
        .exit_status = 0,
        .output = {},
      };
    }

    std::shared_ptr<production_host_state_t> state_;
  };

  class production_fake_probe_t final : public input::kernel_node_probe_t {
  public:
    production_fake_probe_t(
      std::shared_ptr<production_host_state_t> state,
      std::shared_ptr<production_input_state_t> input_state
    ) :
        state_(std::move(state)),
        input_state_(std::move(input_state)) {
    }

    input::node_observation_t observe(
      const std::filesystem::path &path
    ) override {
      std::scoped_lock lock {state_->mutex};
      ++state_->probe_calls;
      if (!state_->observe_allocated_input_nodes || !input_state_) {
        return {.status = input::node_observation_status_e::unsafe};
      }
      // Report exactly the node the fake input backend allocated, so the
      // launch and inventory identity checks see a current kernel node.
      std::scoped_lock input_lock {input_state_->mutex};
      for (const auto &allocation : input_state_->allocations) {
        for (const auto &node : allocation.nodes) {
          if (node.host_path == path) {
            return {
              .status = input::node_observation_status_e::observed,
              .snapshot = input::kernel_node_snapshot_t {
                .host_path = node.host_path,
                .filesystem_device = node.filesystem_device,
                .inode = node.inode,
                .character_major = node.character_major,
                .character_minor = node.character_minor,
                .kernel_name = node.kernel_name,
                .phys = node.phys,
                .host_seat = node.host_seat,
              },
            };
          }
        }
      }
      return {.status = input::node_observation_status_e::absent};
    }

  private:
    std::shared_ptr<production_host_state_t> state_;
    std::shared_ptr<production_input_state_t> input_state_;
  };

  struct production_factory_state_t {
    std::size_t epoch_calls = 0;
    std::size_t moonlight_calls = 0;
    std::size_t host_calls = 0;
    std::size_t probe_calls = 0;
    std::size_t worker_session_calls = 0;
  };

  class production_fail_closed_worker_session_t final :
      public worker_control_session_t {
  public:
    worker_ipc::transport_status_e connect(
      const worker_ipc::authority_handle_t &,
      worker_ipc::controller_client_options_t
    ) override {
      return worker_ipc::transport_status_e::peer_rejected;
    }

    worker_ipc::transport_status_e heartbeat(
      worker_ipc::channel_e
    ) override {
      return worker_ipc::transport_status_e::closed;
    }

    worker_ipc::transport_status_e shutdown() override {
      return worker_ipc::transport_status_e::closed;
    }

    void close() noexcept override {
    }

    [[nodiscard]] bool connected() const noexcept override {
      return false;
    }
  };

  production_controller_options_t production_options(
    const std::filesystem::path &root
  ) {
    production_controller_options_t options;
    options.enabled = true;
    options.gpus = {{
      .logical_gpu_id = "gpu-production",
      .render_node = "/dev/dri/renderD128",
      .devices = {
        "/dev/dri/renderD128",
        "/dev/dri/card0",
      },
      .max_seats = 2,
      .max_encoder_sessions = 2,
    }};
    options.podman.executable = "/usr/bin/podman";
    options.podman.deployment_id = "production-test-deployment";
    options.podman.worker_entrypoint = "/usr/bin/polaris-seat-worker";
    options.podman.ipc_root = root;
    options.podman.profiles = {{
      .profile_key = "profile-production",
      .opaque_volume_name = "pv-production",
      .runtime_profile = runtime_profile_e::steam,
      .image_reference =
        std::string {"ghcr.io/papi-ux/polaris-seat-steam@sha256:"} +
        std::string(64, 'a'),
    }};
    options.podman.workloads = {{
      .kind = workload_kind_e::steam,
      .target_id = "steam-production-game",
    }};
    return options;
  }

  production_controller_gpu_t second_production_gpu() {
    return {
      .logical_gpu_id = "gpu-production-secondary",
      .render_node = "/dev/dri/renderD129",
      .devices = {
        "/dev/dri/renderD129",
        "/dev/dri/card1",
      },
      .max_seats = 2,
      .max_encoder_sessions = 2,
    };
  }

  production_controller_factories_t production_factories(
    const std::shared_ptr<production_factory_state_t> &factory_state,
    const std::shared_ptr<production_input_state_t> &input_state,
    const std::shared_ptr<production_host_state_t> &host_state
  ) {
    return {
      .controller_epoch = [factory_state]() {
        ++factory_state->epoch_calls;
        return std::optional<std::string> {"controller-production"};
      },
      .moonlight_runtime = [factory_state, input_state]() {
        ++factory_state->moonlight_calls;
        return input::moonlight_session_runtime_t::create(
          {.enabled = true},
          [input_state](input::moonlight_controller_feedback_sink_t) {
            return std::make_unique<production_input_backend_t>(input_state);
          }
        );
      },
      .podman_host = [factory_state, host_state]() {
        ++factory_state->host_calls;
        return std::make_unique<production_fake_host_t>(host_state);
      },
      .kernel_probe = [factory_state, host_state, input_state]() {
        ++factory_state->probe_calls;
        return std::make_unique<production_fake_probe_t>(
          host_state,
          input_state
        );
      },
      .worker_session = [factory_state]() {
        ++factory_state->worker_session_calls;
        return std::make_unique<production_fail_closed_worker_session_t>();
      },
    };
  }

  seat_request_t production_request(
    std::string client,
    std::string profile
  ) {
    return {
      .client_key = std::move(client),
      .profile_key = std::move(profile),
      .workload = {
        .kind = workload_kind_e::steam,
        .target_id = "steam-production-game",
      },
      .logical_gpu_id = "gpu-production",
      .runtime_profile = runtime_profile_e::steam,
      .data_plane = {
        .display_topology =
          display_topology_e::capture_host_with_nested_compositor,
        .media_pipeline = media_pipeline_e::worker_local_capture_encode,
      },
      .display_mode = {1920, 1080, 60000, false},
      .requested_compositor = compositor_e::gamescope,
      .encoder_sessions = 1,
    };
  }

  TEST(
    MultiseatControllerProduction,
    DisabledCreationDoesNotValidateCatalogOrInvokeFactories
  ) {
    ASSERT_FALSE(input::moonlight_session_runtime_installed());
    ASSERT_FALSE(input::moonlight_session_activation_gate_installed());
    auto factory_state = std::make_shared<production_factory_state_t>();
    auto input_state = std::make_shared<production_input_state_t>();
    auto host_state = std::make_shared<production_host_state_t>();

    auto created = create_production_controller_runtime(
      {},
      production_factories(factory_state, input_state, host_state)
    );

    EXPECT_EQ(
      created.status,
      controller_runtime_create_status_e::ready_disabled
    );
    EXPECT_FALSE(created.runtime);
    EXPECT_EQ(factory_state->epoch_calls, 0U);
    EXPECT_EQ(factory_state->moonlight_calls, 0U);
    EXPECT_EQ(factory_state->host_calls, 0U);
    EXPECT_EQ(factory_state->probe_calls, 0U);
    EXPECT_EQ(factory_state->worker_session_calls, 0U);
    EXPECT_FALSE(input::moonlight_session_runtime_installed());
    EXPECT_FALSE(input::moonlight_session_activation_gate_installed());
  }

  TEST(
    MultiseatControllerProduction,
    RejectsAmbiguousPodmanGpuCatalogBeforeInvokingFactories
  ) {
    temporary_production_root_t root;
    auto options = production_options(root.path());
    options.podman.gpus.push_back({
      .logical_gpu_id = "caller-supplied",
      .render_node = "/dev/dri/renderD129",
      .devices = {{
        .path = "/dev/dri/renderD129",
        .admitted_identity = {
          .filesystem_device = 81,
          .inode = 19002,
          .character_major = 226,
          .character_minor = 129,
        },
      }},
      .max_encoder_sessions = 1,
    });
    auto factory_state = std::make_shared<production_factory_state_t>();
    auto input_state = std::make_shared<production_input_state_t>();
    auto host_state = std::make_shared<production_host_state_t>();

    auto created = create_production_controller_runtime(
      std::move(options),
      production_factories(factory_state, input_state, host_state)
    );

    EXPECT_EQ(
      created.status,
      controller_runtime_create_status_e::invalid_dependencies
    );
    EXPECT_FALSE(created.runtime);
    EXPECT_EQ(factory_state->epoch_calls, 0U);
    EXPECT_EQ(factory_state->moonlight_calls, 0U);
    EXPECT_EQ(factory_state->host_calls, 0U);
    EXPECT_EQ(factory_state->probe_calls, 0U);
    EXPECT_EQ(factory_state->worker_session_calls, 0U);
    EXPECT_FALSE(input::moonlight_session_runtime_installed());
  }

  TEST(
    MultiseatControllerProduction,
    RejectsDuplicateRenderNodeAcrossLogicalGpusBeforeInvokingFactories
  ) {
    temporary_production_root_t root;
    auto options = production_options(root.path());
    auto second = second_production_gpu();
    second.render_node = options.gpus.front().render_node;
    second.devices = {
      options.gpus.front().render_node,
      "/dev/dri/card1",
    };
    options.gpus.push_back(std::move(second));
    auto factory_state = std::make_shared<production_factory_state_t>();
    auto input_state = std::make_shared<production_input_state_t>();
    auto host_state = std::make_shared<production_host_state_t>();

    auto created = create_production_controller_runtime(
      std::move(options),
      production_factories(factory_state, input_state, host_state)
    );

    EXPECT_EQ(
      created.status,
      controller_runtime_create_status_e::invalid_dependencies
    );
    EXPECT_FALSE(created.runtime);
    EXPECT_EQ(factory_state->epoch_calls, 0U);
    EXPECT_EQ(factory_state->moonlight_calls, 0U);
    EXPECT_EQ(factory_state->host_calls, 0U);
    EXPECT_EQ(factory_state->probe_calls, 0U);
    EXPECT_EQ(factory_state->worker_session_calls, 0U);
    EXPECT_EQ(host_state->device_identity_calls, 0U);
    EXPECT_FALSE(input::moonlight_session_runtime_installed());
  }

  TEST(
    MultiseatControllerProduction,
    RejectsAnotherGpuRenderNodeInAnExclusiveDeviceList
  ) {
    temporary_production_root_t root;
    auto options = production_options(root.path());
    auto second = second_production_gpu();
    second.devices.push_back(options.gpus.front().render_node);
    options.gpus.push_back(std::move(second));
    auto factory_state = std::make_shared<production_factory_state_t>();
    auto input_state = std::make_shared<production_input_state_t>();
    auto host_state = std::make_shared<production_host_state_t>();

    auto created = create_production_controller_runtime(
      std::move(options),
      production_factories(factory_state, input_state, host_state)
    );

    EXPECT_EQ(
      created.status,
      controller_runtime_create_status_e::invalid_dependencies
    );
    EXPECT_FALSE(created.runtime);
    EXPECT_EQ(factory_state->epoch_calls, 0U);
    EXPECT_EQ(factory_state->moonlight_calls, 0U);
    EXPECT_EQ(factory_state->host_calls, 0U);
    EXPECT_EQ(factory_state->probe_calls, 0U);
    EXPECT_EQ(factory_state->worker_session_calls, 0U);
    EXPECT_EQ(host_state->device_identity_calls, 0U);
    EXPECT_FALSE(input::moonlight_session_runtime_installed());
  }

  TEST(
    MultiseatControllerProduction,
    RejectsDistinctRenderPathsThatAliasOneCharacterDevice
  ) {
    temporary_production_root_t root;
    auto options = production_options(root.path());
    options.gpus.push_back(second_production_gpu());
    auto factory_state = std::make_shared<production_factory_state_t>();
    auto input_state = std::make_shared<production_input_state_t>();
    auto host_state = std::make_shared<production_host_state_t>();
    host_state->character_devices.at("/dev/dri/renderD129") = {
      .filesystem_device = 82,
      .inode = 29002,
      .character_major = 226,
      .character_minor = 128,
    };

    auto created = create_production_controller_runtime(
      std::move(options),
      production_factories(factory_state, input_state, host_state)
    );

    EXPECT_EQ(
      created.status,
      controller_runtime_create_status_e::dependencies_unavailable
    );
    EXPECT_FALSE(created.runtime);
    EXPECT_EQ(factory_state->epoch_calls, 1U);
    EXPECT_EQ(factory_state->host_calls, 1U);
    EXPECT_EQ(factory_state->probe_calls, 0U);
    EXPECT_EQ(factory_state->moonlight_calls, 0U);
    EXPECT_EQ(factory_state->worker_session_calls, 0U);
    EXPECT_EQ(host_state->device_identity_calls, 3U);
    EXPECT_FALSE(input::moonlight_session_runtime_installed());
    EXPECT_FALSE(input::moonlight_session_activation_gate_installed());
  }

  TEST(
    MultiseatControllerProduction,
    AcceptsDisjointCharacterDevicesForTwoLogicalGpus
  ) {
    temporary_production_root_t root;
    auto options = production_options(root.path());
    options.gpus.push_back(second_production_gpu());
    auto factory_state = std::make_shared<production_factory_state_t>();
    auto input_state = std::make_shared<production_input_state_t>();
    auto host_state = std::make_shared<production_host_state_t>();
    auto created = create_production_controller_runtime(
      std::move(options),
      production_factories(factory_state, input_state, host_state)
    );
    ASSERT_EQ(
      created.status,
      controller_runtime_create_status_e::ready_enabled
    );
    ASSERT_TRUE(created.runtime);
    auto controller = std::move(created.runtime);
    {
      std::scoped_lock lock {host_state->mutex};
      EXPECT_EQ(host_state->device_identity_calls, 4U);
    }
    ASSERT_TRUE(controller->reconcile().ready());

    auto first_request = production_request("client-one", "profile-one");
    auto second_request = production_request("client-two", "profile-two");
    second_request.logical_gpu_id = "gpu-production-secondary";
    EXPECT_TRUE(controller->admit(first_request).accepted());
    EXPECT_TRUE(controller->admit(second_request).accepted());
    EXPECT_EQ(controller->seats(), 2U);

    EXPECT_EQ(
      controller->shutdown().status,
      controller_shutdown_status_e::closed
    );
    controller.reset();
    EXPECT_EQ(factory_state->worker_session_calls, 0U);
    EXPECT_FALSE(input::moonlight_session_runtime_installed());
    EXPECT_FALSE(input::moonlight_session_activation_gate_installed());
    std::scoped_lock lock {host_state->mutex};
    EXPECT_GT(host_state->device_identity_calls, 4U);
  }

  TEST(
    MultiseatControllerProduction,
    RejectsGpuIdentityReassignmentAfterCatalogAdmissionBeforePodmanRun
  ) {
    temporary_production_root_t root;
    auto options = production_options(root.path());
    options.gpus.push_back(second_production_gpu());
    auto factory_state = std::make_shared<production_factory_state_t>();
    auto input_state = std::make_shared<production_input_state_t>();
    auto host_state = std::make_shared<production_host_state_t>();
    auto created = create_production_controller_runtime(
      std::move(options),
      production_factories(factory_state, input_state, host_state)
    );
    ASSERT_EQ(
      created.status,
      controller_runtime_create_status_e::ready_enabled
    );
    ASSERT_TRUE(created.runtime);
    auto controller = std::move(created.runtime);
    ASSERT_TRUE(controller->reconcile().ready());

    const auto admitted = controller->admit(
      production_request("client-one", "profile-one")
    );
    ASSERT_TRUE(admitted.accepted());
    ASSERT_TRUE(admitted.seat);
    ASSERT_EQ(
      controller->bind_runtime(
        admitted.seat->handle,
        compositor_e::gamescope,
        "offline production identity mutation test"
      ),
      mutation_result_e::applied
    );
    podman::character_device_identity_t admitted_secondary_identity;
    {
      std::scoped_lock lock {host_state->mutex};
      admitted_secondary_identity =
        host_state->character_devices.at("/dev/dri/renderD129");
      host_state->character_devices.at("/dev/dri/renderD129") =
        host_state->character_devices.at("/dev/dri/renderD128");
    }

    const auto started = controller->start_seat(
      admitted.seat->handle,
      {.gamepad_slots = 0}
    );

    EXPECT_EQ(started.status, controller_start_status_e::worker_rejected);
    ASSERT_TRUE(started.rollback);
    EXPECT_EQ(controller->seats(), 0U);
    EXPECT_EQ(controller->managed_workers(), 0U);
    EXPECT_EQ(controller->input_allocations(), 0U);
    EXPECT_EQ(factory_state->worker_session_calls, 0U);
    {
      std::scoped_lock lock {host_state->mutex};
      EXPECT_TRUE(std::none_of(
        host_state->commands.begin(),
        host_state->commands.end(),
        [](const auto &command) {
          return command.size() > 3 && command.at(2) == "--runtime=/usr/bin/crun" && command.at(3) == "run";
        }
      ));
    }
    {
      std::scoped_lock lock {host_state->mutex};
      host_state->character_devices.at("/dev/dri/renderD129") =
        admitted_secondary_identity;
    }

    EXPECT_EQ(
      controller->shutdown().status,
      controller_shutdown_status_e::closed
    );
    controller.reset();
    EXPECT_FALSE(input::moonlight_session_runtime_installed());
    EXPECT_FALSE(input::moonlight_session_activation_gate_installed());
  }

  TEST(
    MultiseatControllerProduction,
    QuiescedShutdownReconcilesListedStoppedWorkerThroughReadableManifest
  ) {
    temporary_production_root_t root;
    auto factory_state = std::make_shared<production_factory_state_t>();
    auto input_state = std::make_shared<production_input_state_t>();
    auto host_state = std::make_shared<production_host_state_t>();
    host_state->simulate_containers = true;
    host_state->observe_allocated_input_nodes = true;
    for (std::uint32_t index = 0; index < 3; ++index) {
      host_state->character_devices.emplace(
        "/dev/input/event" + std::to_string(256 + index),
        podman::character_device_identity_t {
          .filesystem_device = 73,
          .inode = 18000 + index,
          .character_major = 13,
          .character_minor = 256 + index,
        }
      );
    }
    auto created = create_production_controller_runtime(
      production_options(root.path()),
      production_factories(factory_state, input_state, host_state)
    );
    ASSERT_EQ(
      created.status,
      controller_runtime_create_status_e::ready_enabled
    );
    ASSERT_TRUE(created.runtime);
    auto controller = std::move(created.runtime);
    ASSERT_TRUE(controller->reconcile().ready());

    const auto admitted = controller->admit(
      production_request("client-one", "profile-production")
    );
    ASSERT_TRUE(admitted.accepted());
    ASSERT_TRUE(admitted.seat);
    ASSERT_EQ(
      controller->bind_runtime(
        admitted.seat->handle,
        compositor_e::gamescope,
        "offline production quiesced shutdown test"
      ),
      mutation_result_e::applied
    );
    const auto started = controller->start_seat(
      admitted.seat->handle,
      {.gamepad_slots = 0}
    );
    ASSERT_EQ(started.status, controller_start_status_e::started);
    EXPECT_EQ(controller->seats(), 1U);
    EXPECT_EQ(controller->managed_workers(), 1U);
    EXPECT_EQ(controller->input_allocations(), 1U);
    {
      std::scoped_lock lock {host_state->mutex};
      ASSERT_EQ(host_state->containers.size(), 1U);
      EXPECT_EQ(host_state->containers.front().status, "running");
      // The post-run launch check is identity-only; the runtime spec is
      // evidence for the authoritative inventory, not for launch.
      EXPECT_EQ(host_state->runtime_spec_reads, 0U);
    }

    // One pass: quiesce fences Moonlight first, the graceful stop leaves an
    // exited container listed, and authoritative reconcile must still read
    // the exact input allocation to prove that worker stopped and release it.
    const auto report = controller->shutdown();

    EXPECT_EQ(report.status, controller_shutdown_status_e::closed);
    EXPECT_EQ(report.stop_requests, 1U);
    ASSERT_TRUE(report.worker);
    EXPECT_FALSE(report.worker->broker.backend_observation_failed);
    EXPECT_TRUE(report.worker->broker.inventory_authoritative);
    EXPECT_EQ(report.worker->broker.observations, 1U);
    EXPECT_EQ(report.worker->broker.released_seats, 1U);
    EXPECT_TRUE(report.worker->admission_ready);
    ASSERT_TRUE(report.input);
    EXPECT_EQ(
      report.input->status,
      input::moonlight_coordinator_shutdown_status_e::closed
    );
    EXPECT_EQ(report.input->released_allocations, 1U);
    EXPECT_TRUE(controller->closed());
    EXPECT_EQ(controller->seats(), 0U);
    EXPECT_EQ(controller->managed_workers(), 0U);
    EXPECT_EQ(controller->input_allocations(), 0U);
    EXPECT_EQ(factory_state->worker_session_calls, 0U);
    {
      std::scoped_lock lock {host_state->mutex};
      ASSERT_EQ(host_state->containers.size(), 1U);
      EXPECT_EQ(host_state->containers.front().status, "exited");
      // Rootless Podman reports no devices in HostConfig, so the one
      // authoritative inventory proved the stopped worker through its OCI
      // runtime spec.
      EXPECT_EQ(host_state->runtime_spec_reads, 1U);
      const auto verb_count = [&host_state](std::string_view verb) {
        return std::count_if(
          host_state->commands.begin(),
          host_state->commands.end(),
          [verb](const auto &command) {
            const auto index = command.size() > 2 && command.at(2) == "--runtime=/usr/bin/crun" ? 3U : 2U;
            return command.size() > index && command.at(index) == verb;
          }
        );
      };
      EXPECT_EQ(verb_count("run"), 1);
      EXPECT_EQ(verb_count("kill"), 1);
      EXPECT_EQ(verb_count("rm"), 0);
    }
    {
      std::scoped_lock lock {input_state->mutex};
      EXPECT_TRUE(input_state->allocations.empty());
    }
    controller.reset();
    EXPECT_FALSE(input::moonlight_session_runtime_installed());
    EXPECT_FALSE(input::moonlight_session_activation_gate_installed());
  }

  TEST(
    MultiseatControllerProduction,
    ComposesOneCatalogAndReconcilesThroughInjectedOfflineHost
  ) {
    temporary_production_root_t root;
    auto factory_state = std::make_shared<production_factory_state_t>();
    auto input_state = std::make_shared<production_input_state_t>();
    auto host_state = std::make_shared<production_host_state_t>();
    auto created = create_production_controller_runtime(
      production_options(root.path()),
      production_factories(factory_state, input_state, host_state)
    );
    ASSERT_EQ(
      created.status,
      controller_runtime_create_status_e::ready_enabled
    );
    ASSERT_TRUE(created.runtime);
    auto controller = std::move(created.runtime);
    {
      std::scoped_lock lock {host_state->mutex};
      EXPECT_EQ(host_state->device_identity_calls, 2U);
    }

    EXPECT_TRUE(controller->reconcile().ready());
    EXPECT_EQ(factory_state->epoch_calls, 1U);
    EXPECT_EQ(factory_state->moonlight_calls, 1U);
    EXPECT_EQ(factory_state->host_calls, 1U);
    EXPECT_EQ(factory_state->probe_calls, 1U);
    EXPECT_EQ(factory_state->worker_session_calls, 0U);

    const auto first = controller->admit(
      production_request("client-one", "profile-one")
    );
    const auto second = controller->admit(
      production_request("client-two", "profile-two")
    );
    const auto full = controller->admit(
      production_request("client-three", "profile-three")
    );
    EXPECT_TRUE(first.accepted());
    EXPECT_TRUE(second.accepted());
    EXPECT_EQ(full.rejection, admission_rejection_e::seat_capacity_reached);

    EXPECT_EQ(
      controller->shutdown().status,
      controller_shutdown_status_e::closed
    );
    controller.reset();
    EXPECT_FALSE(input::moonlight_session_runtime_installed());
    EXPECT_FALSE(input::moonlight_session_activation_gate_installed());

    std::scoped_lock lock {host_state->mutex};
    EXPECT_GT(host_state->device_identity_calls, 2U);
    ASSERT_GE(host_state->commands.size(), 2U);
    for (const auto &command : host_state->commands) {
      ASSERT_GE(command.size(), 3U);
      EXPECT_EQ(command.at(0), "/usr/bin/podman");
      EXPECT_EQ(command.at(1), "--remote=false");
      EXPECT_EQ(command.at(2), "ps");
      EXPECT_EQ(
        std::find(command.begin(), command.end(), "run"),
        command.end()
      );
    }
    EXPECT_EQ(host_state->probe_calls, 0U);
  }

  TEST(
    MultiseatControllerProduction,
    InvalidWorkerCatalogUnwindsInstalledMoonlightRuntime
  ) {
    temporary_production_root_t root;
    auto options = production_options(root.path());
    options.podman.profiles.front().image_reference =
      "ghcr.io/papi-ux/polaris-seat-steam:latest";
    auto factory_state = std::make_shared<production_factory_state_t>();
    auto input_state = std::make_shared<production_input_state_t>();
    auto host_state = std::make_shared<production_host_state_t>();

    auto created = create_production_controller_runtime(
      std::move(options),
      production_factories(factory_state, input_state, host_state)
    );

    EXPECT_EQ(
      created.status,
      controller_runtime_create_status_e::dependencies_unavailable
    );
    EXPECT_FALSE(created.runtime);
    EXPECT_EQ(factory_state->epoch_calls, 1U);
    EXPECT_EQ(factory_state->moonlight_calls, 1U);
    EXPECT_EQ(factory_state->host_calls, 1U);
    EXPECT_EQ(factory_state->probe_calls, 1U);
    EXPECT_EQ(factory_state->worker_session_calls, 0U);
    EXPECT_FALSE(input::moonlight_session_runtime_installed());
    EXPECT_FALSE(input::moonlight_session_activation_gate_installed());
  }

  TEST(
    MultiseatControllerProduction,
    RuntimeManifestIsReadOnlyAndExactGenerationFenced
  ) {
    auto input_state = std::make_shared<production_input_state_t>();
    auto created = input::moonlight_session_runtime_t::create(
      {.enabled = true},
      [input_state](input::moonlight_controller_feedback_sink_t) {
        return std::make_unique<production_input_backend_t>(input_state);
      }
    );
    ASSERT_TRUE(created.runtime);
    auto runtime = std::move(created.runtime);
    ASSERT_TRUE(runtime->reconcile_inputs({}).report.admission_ready);
    const input::expectation_t expectation {
      .handle = {
        .controller_epoch = "controller-production",
        .logical_gpu_id = "gpu-production",
        .slot = 0,
        .generation = 7,
      },
      .input_seat = "polaris-input-controller-production-7",
      .plan = {.gamepad_slots = 0},
    };
    ASSERT_TRUE(runtime->prepare_input(expectation).input.prepared());

    const auto exact = runtime->input_allocation(expectation.handle);
    ASSERT_TRUE(exact);
    EXPECT_EQ(exact->handle, expectation.handle);
    EXPECT_EQ(exact->input_seat, expectation.input_seat);
    auto stale = expectation.handle;
    ++stale.generation;
    EXPECT_FALSE(runtime->input_allocation(stale));

    EXPECT_EQ(
      runtime->release_input(expectation.handle).input_status,
      input::status_e::applied
    );
    EXPECT_FALSE(runtime->input_allocation(expectation.handle));
    EXPECT_EQ(
      runtime->shutdown().status,
      input::moonlight_coordinator_shutdown_status_e::closed
    );
    EXPECT_FALSE(runtime->input_allocation(expectation.handle));
    runtime.reset();
    EXPECT_FALSE(input::moonlight_session_runtime_installed());
  }
}  // namespace

#endif
