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
        .host_path = "/dev/input/event" + std::to_string(40 + index),
        .worker_path = input::expected_worker_path(kind, slot),
        .filesystem_device = 73,
        .inode = 18000 + index,
        .character_major = 13,
        .character_minor = static_cast<std::uint32_t>(104 + index),
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
  };

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
      return path == "/usr/bin/podman";
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
      const std::filesystem::path &
    ) const override {
      return podman::character_device_identity_t {
        .filesystem_device = 81,
        .inode = 19000,
        .character_major = 226,
        .character_minor = 128,
      };
    }

    podman::command_result_t run(
      const std::vector<std::string> &argv,
      std::chrono::milliseconds,
      std::size_t
    ) override {
      std::scoped_lock lock {state_->mutex};
      state_->commands.push_back(argv);
      return {
        .exit_status = 0,
        .output = {},
      };
    }

  private:
    std::shared_ptr<production_host_state_t> state_;
  };

  class production_fake_probe_t final : public input::kernel_node_probe_t {
  public:
    explicit production_fake_probe_t(
      std::shared_ptr<production_host_state_t> state
    ) :
        state_(std::move(state)) {
    }

    input::node_observation_t observe(
      const std::filesystem::path &
    ) override {
      std::scoped_lock lock {state_->mutex};
      ++state_->probe_calls;
      return {.status = input::node_observation_status_e::unsafe};
    }

  private:
    std::shared_ptr<production_host_state_t> state_;
  };

  struct production_factory_state_t {
    std::size_t epoch_calls = 0;
    std::size_t moonlight_calls = 0;
    std::size_t host_calls = 0;
    std::size_t probe_calls = 0;
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
      .kernel_probe = [factory_state, host_state]() {
        ++factory_state->probe_calls;
        return std::make_unique<production_fake_probe_t>(host_state);
      },
      .worker_session = {},
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
      .devices = {"/dev/dri/renderD129"},
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
    EXPECT_FALSE(input::moonlight_session_runtime_installed());
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

    EXPECT_TRUE(controller->reconcile().ready());
    EXPECT_EQ(factory_state->epoch_calls, 1U);
    EXPECT_EQ(factory_state->moonlight_calls, 1U);
    EXPECT_EQ(factory_state->host_calls, 1U);
    EXPECT_EQ(factory_state->probe_calls, 1U);

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
