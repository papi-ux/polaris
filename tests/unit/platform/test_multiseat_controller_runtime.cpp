/**
 * @file tests/unit/platform/test_multiseat_controller_runtime.cpp
 * @brief Offline composition tests for the trusted multiseat controller owner.
 */
#include "src/platform/linux/multiseat_controller_runtime.h"
#include "src/platform/linux/multiseat_moonlight_activation.h"
#include "src/rtsp.h"
#include "src/stream.h"

#ifdef __linux__

extern "C" {
  #include <moonlight-common-c/src/Input.h>
}

  #include <algorithm>
  #include <array>
  #include <chrono>
  #include <condition_variable>
  #include <cstddef>
  #include <cstdint>
  #include <cstdlib>
  #include <filesystem>
  #include <fstream>
  #include <future>
  #include <gtest/gtest.h>
  #include <memory>
  #include <mutex>
  #include <optional>
  #include <sstream>
  #include <stdexcept>
  #include <string>
  #include <string_view>
  #include <sys/stat.h>
  #include <utility>
  #include <vector>

namespace {
  using namespace multiseat;
  using namespace multiseat::worker_ipc;
  namespace input = multiseat::input;

  constexpr auto controller_gpu = "gpu-controller-runtime";

  class activation_pause_t {
  public:
    void pause() {
      std::unique_lock lock {mutex_};
      entered_ = true;
      changed_.notify_all();
      changed_.wait(lock, [this]() {
        return released_;
      });
    }

    [[nodiscard]] bool wait_until_entered(
      std::chrono::milliseconds timeout
    ) {
      std::unique_lock lock {mutex_};
      return changed_.wait_for(lock, timeout, [this]() {
        return entered_;
      });
    }

    void release() {
      std::scoped_lock lock {mutex_};
      released_ = true;
      changed_.notify_all();
    }

  private:
    std::mutex mutex_;
    std::condition_variable changed_;
    bool entered_ = false;
    bool released_ = false;
  };

  class activation_hook_guard_t {
  public:
    explicit activation_hook_guard_t(
      input::moonlight_activation_before_bind_hook_t hook
    ) {
      input::set_moonlight_activation_before_bind_hook_for_tests(
        std::move(hook)
      );
    }

    ~activation_hook_guard_t() {
      input::set_moonlight_activation_before_bind_hook_for_tests({});
    }

    activation_hook_guard_t(const activation_hook_guard_t &) = delete;
    activation_hook_guard_t &operator=(
      const activation_hook_guard_t &
    ) = delete;
  };

  class temporary_controller_root_t {
  public:
    temporary_controller_root_t() {
      std::array<char, 64> pattern {};
      const std::string prefix = "/tmp/polaris-controller-runtime-XXXXXX";
      std::copy(prefix.begin(), prefix.end(), pattern.begin());
      const auto *created = ::mkdtemp(pattern.data());
      if (!created) {
        throw std::runtime_error {
          "temporary controller root could not be created"
        };
      }
      path_ = created;
      if (::chmod(path_.c_str(), 0700) != 0) {
        throw std::runtime_error {
          "temporary controller root mode could not be set"
        };
      }
    }

    ~temporary_controller_root_t() {
      std::error_code ignored;
      std::filesystem::remove_all(path_, ignored);
    }

    temporary_controller_root_t(const temporary_controller_root_t &) = delete;
    temporary_controller_root_t &operator=(
      const temporary_controller_root_t &
    ) = delete;

    [[nodiscard]] const std::filesystem::path &path() const {
      return path_;
    }

  private:
    std::filesystem::path path_;
  };

  capability_factory_t deterministic_controller_capability() {
    return [](capability_t &capability) {
      for (std::size_t index = 0; index < capability.size(); ++index) {
        capability[index] = static_cast<std::uint8_t>(0x51 + index);
      }
      return true;
    };
  }

  input::allocation_t controller_allocation(
    const input::expectation_t &expectation
  ) {
    input::allocation_t allocation {
      .handle = expectation.handle,
      .input_seat = expectation.input_seat,
      .plan = expectation.plan,
    };
    std::vector<std::pair<input::device_kind_e, std::uint32_t>> devices {
      {input::device_kind_e::keyboard, 0},
      {input::device_kind_e::mouse_relative, 0},
      {input::device_kind_e::mouse_absolute, 0},
    };
    if (expectation.plan.touch) {
      devices.emplace_back(input::device_kind_e::touch, 0);
    }
    if (expectation.plan.pen) {
      devices.emplace_back(input::device_kind_e::pen, 0);
    }
    for (std::uint32_t slot = 0;
         slot < expectation.plan.gamepad_slots;
         ++slot) {
      devices.emplace_back(input::device_kind_e::gamepad, slot);
    }
    for (std::size_t index = 0; index < devices.size(); ++index) {
      const auto [kind, slot] = devices[index];
      const auto identity = expectation.handle.generation * 32 + index;
      allocation.nodes.push_back({
        .kind = kind,
        .slot = slot,
        .host_path = "/dev/input/event" + std::to_string(identity),
        .worker_path = input::expected_worker_path(kind, slot),
        .filesystem_device = 61,
        .inode = 15000 + identity,
        .character_major = 13,
        .character_minor = static_cast<std::uint32_t>(64 + identity),
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

  struct controller_test_state_t {
    mutable std::mutex mutex;
    std::vector<input::allocation_t> input_allocations;
    std::vector<worker_observation_t> workers;
    input::backend_result_e next_input_create =
      input::backend_result_e::applied;
    input::backend_result_e input_destroy =
      input::backend_result_e::applied;
    worker_command_result_e next_worker_launch =
      worker_command_result_e::applied;
    bool fail_input_inventory = false;
    bool fail_worker_inventory = false;
    bool complete_worker_on_stop = true;
    bool input_present_at_last_worker_launch = false;
    std::size_t input_create_calls = 0;
    std::size_t input_destroy_calls = 0;
    std::size_t input_route_calls = 0;
    std::size_t input_inventory_calls = 0;
    std::size_t worker_launch_calls = 0;
    std::size_t worker_inventory_calls = 0;
    std::size_t session_connects = 0;
    std::size_t session_shutdowns = 0;

    void allow_cleanup() {
      std::scoped_lock lock {mutex};
      fail_input_inventory = false;
      fail_worker_inventory = false;
      complete_worker_on_stop = true;
      input_destroy = input::backend_result_e::applied;
      workers.clear();
    }

    void set_next_input_create(input::backend_result_e result) {
      std::scoped_lock lock {mutex};
      next_input_create = result;
    }

    void set_next_worker_launch(worker_command_result_e result) {
      std::scoped_lock lock {mutex};
      next_worker_launch = result;
    }

    void set_worker_inventory_failure(bool fail) {
      std::scoped_lock lock {mutex};
      fail_worker_inventory = fail;
    }

    void set_complete_worker_on_stop(bool complete) {
      std::scoped_lock lock {mutex};
      complete_worker_on_stop = complete;
    }

    [[nodiscard]] bool mark_worker_ready(
      const worker_identity_t &identity
    ) {
      std::scoped_lock lock {mutex};
      const auto found = std::find_if(
        workers.begin(),
        workers.end(),
        [&identity](const auto &candidate) {
          return candidate.identity == identity;
        }
      );
      if (found == workers.end()) {
        return false;
      }
      found->state = worker_observed_state_e::ready;
      return true;
    }

    [[nodiscard]] bool complete_worker(
      const worker_identity_t &identity
    ) {
      std::scoped_lock lock {mutex};
      const auto before = workers.size();
      std::erase_if(workers, [&identity](const auto &candidate) {
        return candidate.identity == identity;
      });
      return workers.size() != before;
    }

    [[nodiscard]] std::size_t input_count() const {
      std::scoped_lock lock {mutex};
      return input_allocations.size();
    }

    [[nodiscard]] std::size_t input_route_count() const {
      std::scoped_lock lock {mutex};
      return input_route_calls;
    }

    [[nodiscard]] std::size_t input_inventory_count() const {
      std::scoped_lock lock {mutex};
      return input_inventory_calls;
    }

    [[nodiscard]] std::size_t worker_launch_count() const {
      std::scoped_lock lock {mutex};
      return worker_launch_calls;
    }

    [[nodiscard]] bool launch_saw_input() const {
      std::scoped_lock lock {mutex};
      return input_present_at_last_worker_launch;
    }
  };

  class controller_input_backend_t final : public input::backend_t {
  public:
    explicit controller_input_backend_t(
      std::shared_ptr<controller_test_state_t> state
    ) : state_(std::move(state)) {
    }

    input::backend_create_result_t create(
      const input::expectation_t &expectation
    ) override {
      std::scoped_lock lock {state_->mutex};
      ++state_->input_create_calls;
      const auto result = std::exchange(
        state_->next_input_create,
        input::backend_result_e::applied
      );
      if (result != input::backend_result_e::applied &&
          result != input::backend_result_e::already_applied) {
        return {.result = result};
      }
      auto allocation = controller_allocation(expectation);
      state_->input_allocations.push_back(allocation);
      return {
        .result = result,
        .allocation = std::move(allocation),
      };
    }

    input::backend_result_e destroy(
      const seat_handle_t &handle,
      std::string_view input_seat
    ) override {
      std::scoped_lock lock {state_->mutex};
      ++state_->input_destroy_calls;
      if (state_->input_destroy != input::backend_result_e::applied &&
          state_->input_destroy !=
            input::backend_result_e::already_applied &&
          state_->input_destroy != input::backend_result_e::not_found) {
        return state_->input_destroy;
      }
      const auto found = std::find_if(
        state_->input_allocations.begin(),
        state_->input_allocations.end(),
        [&handle, input_seat](const auto &candidate) {
          return candidate.handle == handle &&
                 candidate.input_seat == input_seat;
        }
      );
      if (found == state_->input_allocations.end()) {
        return input::backend_result_e::not_found;
      }
      state_->input_allocations.erase(found);
      return state_->input_destroy;
    }

    input::backend_result_e route(
      const seat_handle_t &,
      std::string_view,
      std::uint64_t,
      const input::input_event_t &
    ) override {
      std::scoped_lock lock {state_->mutex};
      ++state_->input_route_calls;
      return input::backend_result_e::applied;
    }

    std::vector<input::allocation_t> inventory() override {
      std::scoped_lock lock {state_->mutex};
      ++state_->input_inventory_calls;
      if (state_->fail_input_inventory) {
        throw std::runtime_error {"fake input inventory failed"};
      }
      return state_->input_allocations;
    }

  private:
    std::shared_ptr<controller_test_state_t> state_;
  };

  class controller_worker_backend_t final : public worker_backend_t {
  public:
    explicit controller_worker_backend_t(
      std::shared_ptr<controller_test_state_t> state
    ) : state_(std::move(state)) {
    }

    worker_command_result_e launch(
      const worker_launch_spec_t &spec
    ) override {
      std::scoped_lock lock {state_->mutex};
      ++state_->worker_launch_calls;
      state_->input_present_at_last_worker_launch = std::any_of(
        state_->input_allocations.begin(),
        state_->input_allocations.end(),
        [&spec](const auto &allocation) {
          return allocation.handle == spec.identity.seat &&
                 allocation.input_seat == spec.resources.input_seat;
        }
      );
      const auto result = std::exchange(
        state_->next_worker_launch,
        worker_command_result_e::applied
      );
      if (result == worker_command_result_e::rejected ||
          result == worker_command_result_e::not_found) {
        return result;
      }
      state_->workers.push_back({
        .identity = spec.identity,
        .state = worker_observed_state_e::starting,
      });
      return result;
    }

    worker_command_result_e stop(
      const worker_identity_t &identity,
      worker_stop_mode_e mode
    ) override {
      std::scoped_lock lock {state_->mutex};
      const auto found = std::find_if(
        state_->workers.begin(),
        state_->workers.end(),
        [&identity](const auto &candidate) {
          return candidate.identity == identity;
        }
      );
      if (found == state_->workers.end()) {
        return worker_command_result_e::not_found;
      }
      if (state_->complete_worker_on_stop ||
          mode == worker_stop_mode_e::force) {
        state_->workers.erase(found);
      } else {
        found->state = worker_observed_state_e::stopping;
      }
      return worker_command_result_e::applied;
    }

    std::vector<worker_observation_t> inventory() override {
      std::scoped_lock lock {state_->mutex};
      ++state_->worker_inventory_calls;
      if (state_->fail_worker_inventory) {
        throw std::runtime_error {"fake worker inventory failed"};
      }
      return state_->workers;
    }

  private:
    std::shared_ptr<controller_test_state_t> state_;
  };

  class controller_control_session_t final :
      public worker_control_session_t {
  public:
    explicit controller_control_session_t(
      std::shared_ptr<controller_test_state_t> state
    ) : state_(std::move(state)) {
    }

    transport_status_e connect(
      const authority_handle_t &,
      controller_client_options_t
    ) override {
      std::scoped_lock lock {state_->mutex};
      ++state_->session_connects;
      connected_ = true;
      return transport_status_e::applied;
    }

    transport_status_e heartbeat(channel_e) override {
      return connected_ ?
               transport_status_e::applied :
               transport_status_e::closed;
    }

    transport_status_e shutdown() override {
      std::scoped_lock lock {state_->mutex};
      if (!connected_) {
        return transport_status_e::closed;
      }
      ++state_->session_shutdowns;
      connected_ = false;
      return transport_status_e::applied;
    }

    void close() noexcept override {
      connected_ = false;
    }

    [[nodiscard]] bool connected() const noexcept override {
      return connected_;
    }

  private:
    std::shared_ptr<controller_test_state_t> state_;
    bool connected_ = false;
  };

  worker_coordinator_options_t controller_worker_options() {
    return {
      .broker = {
        .graceful_stop_timeout = std::chrono::milliseconds {100},
        .force_stop_timeout = std::chrono::milliseconds {100},
      },
      .client = {
        .connect_timeout = std::chrono::milliseconds {100},
        .handshake_timeout = std::chrono::milliseconds {100},
        .io_timeout = std::chrono::milliseconds {100},
      },
    };
  }

  controller_runtime_create_result_t create_controller(
    const std::filesystem::path &root,
    const std::shared_ptr<controller_test_state_t> &state,
    std::vector<input::expectation_t> recovered = {}
  ) {
    return controller_runtime_t::create(
      {.enabled = true},
      [root, state, recovered = std::move(recovered)]() mutable
        -> std::optional<controller_runtime_dependencies_t> {
        auto store = std::make_unique<authority_store_t>(
          root,
          deterministic_controller_capability()
        );
        auto moonlight = input::moonlight_session_runtime_t::create(
          {.enabled = true},
          [state](input::moonlight_controller_feedback_sink_t) {
            return std::make_unique<controller_input_backend_t>(state);
          },
          state
        );
        if (!moonlight.runtime) {
          return std::nullopt;
        }
        return controller_runtime_dependencies_t {
          .registry = std::make_unique<registry_t>(
            "controller-composition-test",
            std::vector<gpu_capacity_t> {{
              .logical_gpu_id = controller_gpu,
              .render_node = "/dev/dri/renderD128",
              .max_seats = 2,
              .max_encoder_sessions = 2,
            }}
          ),
          .worker_authority_store = std::move(store),
          .moonlight_runtime = std::move(moonlight.runtime),
          .worker_backend_dependencies = state,
          .worker_backend =
            std::make_unique<controller_worker_backend_t>(state),
          .recovered_input_expectations = std::move(recovered),
          .worker_options = controller_worker_options(),
          .now = {},
          .session_factory = [state]() {
            return std::make_unique<controller_control_session_t>(state);
          },
        };
      }
    );
  }

  seat_request_t controller_request(
    std::string client = "paired-client",
    std::string profile = "profile-controller",
    std::string target = "steam-controller-game"
  ) {
    return {
      .client_key = std::move(client),
      .profile_key = std::move(profile),
      .workload = {
        .kind = workload_kind_e::steam,
        .target_id = std::move(target),
      },
      .logical_gpu_id = controller_gpu,
      .runtime_profile = runtime_profile_e::steam,
      .data_plane = {
        .display_topology =
          display_topology_e::capture_host_with_nested_compositor,
        .media_pipeline = media_pipeline_e::worker_local_capture_encode,
      },
      .display_mode = {1920, 1080, 60000, false},
      .requested_compositor = compositor_e::automatic,
      .encoder_sessions = 1,
    };
  }

  input::plan_t controller_input_plan() {
    return {.touch = true, .pen = true, .gamepad_slots = 1};
  }

  worker_identity_t controller_worker_identity(
    const seat_snapshot_t &seat
  ) {
    return {
      .seat = seat.handle,
      .worker_name = seat.resources.worker_name,
    };
  }

  std::shared_ptr<rtsp_stream::launch_session_t> controller_launch(
    std::uint32_t id,
    std::uint64_t generation,
    std::string client = "paired-client"
  ) {
    auto launch = std::make_shared<rtsp_stream::launch_session_t>();
    launch->id = id;
    launch->lifecycle_generation = generation;
    launch->gcm_key.resize(16);
    launch->iv.resize(16);
    launch->device_name = "controller-runtime-client";
    launch->unique_id = std::move(client);
    launch->session_token = "controller-runtime-token";
    launch->perm = crypto::PERM::_game_control;
    launch->watch_only = false;
    return launch;
  }

  std::shared_ptr<stream::session_t> controller_stream(
    rtsp_stream::launch_session_t &launch
  ) {
    stream::config_t config {};
    return stream::session::alloc(config, launch);
  }

  std::vector<std::uint8_t> controller_keyboard_packet(std::uint16_t key) {
    std::vector<std::uint8_t> body {0};
    body.push_back(static_cast<std::uint8_t>(key));
    body.push_back(static_cast<std::uint8_t>(key >> 8U));
    body.push_back(0);
    body.push_back(0);
    body.push_back(0);

    std::vector<std::uint8_t> packet;
    const auto declared = static_cast<std::uint32_t>(body.size() + 4);
    packet.push_back(static_cast<std::uint8_t>(declared >> 24U));
    packet.push_back(static_cast<std::uint8_t>(declared >> 16U));
    packet.push_back(static_cast<std::uint8_t>(declared >> 8U));
    packet.push_back(static_cast<std::uint8_t>(declared));
    const auto magic = static_cast<std::uint32_t>(KEY_DOWN_EVENT_MAGIC);
    for (std::size_t index = 0; index < 4; ++index) {
      packet.push_back(static_cast<std::uint8_t>(magic >> (index * 8U)));
    }
    packet.insert(packet.end(), body.begin(), body.end());
    return packet;
  }

  std::string controller_source(std::string_view relative_path) {
    std::ifstream input {
      std::filesystem::path {POLARIS_SOURCE_DIR} / relative_path
    };
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
  }

  class MultiseatControllerRuntimeTest : public testing::Test {
  protected:
    void create_ready_controller() {
      auto created = create_controller(root_.path(), state_);
      ASSERT_EQ(
        created.status,
        controller_runtime_create_status_e::ready_enabled
      );
      ASSERT_TRUE(created.runtime);
      controller_ = std::move(created.runtime);
      const auto reconciled = controller_->reconcile();
      ASSERT_TRUE(reconciled.ready());
      ASSERT_TRUE(controller_->admission_ready());
    }

    seat_snapshot_t admit_and_bind(
      seat_request_t request = controller_request()
    ) {
      auto admitted = controller_->admit(request);
      EXPECT_TRUE(admitted.accepted());
      if (!admitted.seat) {
        return {};
      }
      EXPECT_EQ(
        controller_->bind_runtime(
          admitted.seat->handle,
          compositor_e::gamescope,
          "offline controller test"
        ),
        mutation_result_e::applied
      );
      return *admitted.seat;
    }

    void TearDown() override {
      if (!controller_) {
        return;
      }
      state_->allow_cleanup();
      for (auto attempt = 0; attempt < 3 && !controller_->closed(); ++attempt) {
        (void) controller_->shutdown();
      }
      EXPECT_TRUE(controller_->closed());
      controller_.reset();
      EXPECT_FALSE(input::moonlight_session_runtime_installed());
      EXPECT_FALSE(input::moonlight_session_activation_gate_installed());
    }

    temporary_controller_root_t root_;
    std::shared_ptr<controller_test_state_t> state_ =
      std::make_shared<controller_test_state_t>();
    std::unique_ptr<controller_runtime_t> controller_;
  };

  TEST_F(
    MultiseatControllerRuntimeTest,
    DisabledCreationDoesNotInvokeDependenciesOrInstallRuntime
  ) {
    ASSERT_FALSE(input::moonlight_session_runtime_installed());
    ASSERT_FALSE(input::moonlight_session_activation_gate_installed());
    std::size_t factory_calls = 0;

    auto created = controller_runtime_t::create(
      {},
      [&factory_calls]()
        -> std::optional<controller_runtime_dependencies_t> {
        ++factory_calls;
        return std::nullopt;
      }
    );

    EXPECT_EQ(
      created.status,
      controller_runtime_create_status_e::ready_disabled
    );
    EXPECT_FALSE(created.runtime);
    EXPECT_EQ(factory_calls, 0U);
    EXPECT_FALSE(input::moonlight_session_runtime_installed());
    EXPECT_FALSE(input::moonlight_session_activation_gate_installed());
  }

  TEST_F(
    MultiseatControllerRuntimeTest,
    PreparesInputBeforeWorkerAndSelectsOnlyAuthenticatedPairedClient
  ) {
    create_ready_controller();
    const auto seat = admit_and_bind();
    ASSERT_TRUE(seat.handle.valid());

    const auto started = controller_->start_seat(
      seat.handle,
      controller_input_plan()
    );
    ASSERT_TRUE(started.started());
    EXPECT_EQ(state_->worker_launch_count(), 1U);
    EXPECT_TRUE(state_->launch_saw_input());
    EXPECT_EQ(controller_->input_allocations(), 1U);

    const auto identity = controller_worker_identity(seat);
    ASSERT_TRUE(state_->mark_worker_ready(identity));
    ASSERT_TRUE(controller_->reconcile().ready());

    const auto wrong_client = controller_->select_authenticated_launch(
      controller_launch(1900, 2900, "different-client"),
      seat.handle
    );
    EXPECT_EQ(
      wrong_client.status,
      input::moonlight_worker_selection_status_e::client_mismatch
    );
    EXPECT_EQ(controller_->tracked_launches(), 0U);

    auto launch = controller_launch(1901, 2901);
    const auto selected = controller_->select_authenticated_launch(
      launch,
      seat.handle
    );
    ASSERT_TRUE(selected.selected());
    EXPECT_EQ(controller_->tracked_launches(), 1U);
    EXPECT_EQ(
      input::cancel_registered_moonlight_launch(launch),
      input::moonlight_runtime_lifecycle_status_e::retired
    );

    EXPECT_EQ(
      controller_->stop_seat(seat.handle).status,
      controller_stop_status_e::stopping
    );
    EXPECT_TRUE(controller_->reconcile().ready());
    EXPECT_EQ(controller_->seats(), 0U);
    EXPECT_EQ(controller_->managed_workers(), 0U);
    EXPECT_EQ(controller_->input_allocations(), 0U);
    EXPECT_EQ(
      controller_->shutdown().status,
      controller_shutdown_status_e::closed
    );
  }

  TEST_F(
    MultiseatControllerRuntimeTest,
    InputFailureNeverLaunchesWorker
  ) {
    create_ready_controller();
    const auto seat = admit_and_bind();
    ASSERT_TRUE(seat.handle.valid());
    state_->set_next_input_create(input::backend_result_e::rejected);

    const auto started = controller_->start_seat(
      seat.handle,
      controller_input_plan()
    );
    EXPECT_EQ(started.status, controller_start_status_e::input_rejected);
    EXPECT_FALSE(started.worker);
    EXPECT_EQ(state_->worker_launch_count(), 0U);
    EXPECT_EQ(controller_->input_allocations(), 0U);
    EXPECT_FALSE(controller_->admission_ready());
    EXPECT_EQ(
      controller_->shutdown().status,
      controller_shutdown_status_e::closed
    );
  }

  TEST_F(
    MultiseatControllerRuntimeTest,
    ProvenWorkerRejectionRollsBackInputAndSeat
  ) {
    create_ready_controller();
    const auto seat = admit_and_bind();
    ASSERT_TRUE(seat.handle.valid());
    state_->set_next_worker_launch(worker_command_result_e::rejected);

    const auto started = controller_->start_seat(
      seat.handle,
      controller_input_plan()
    );
    EXPECT_EQ(started.status, controller_start_status_e::worker_rejected);
    ASSERT_TRUE(started.rollback);
    EXPECT_EQ(
      started.rollback->status,
      input::moonlight_coordinator_operation_status_e::applied
    );
    EXPECT_EQ(controller_->seats(), 0U);
    EXPECT_EQ(controller_->managed_workers(), 0U);
    EXPECT_EQ(controller_->input_allocations(), 0U);
    EXPECT_TRUE(controller_->reconcile().ready());
    EXPECT_EQ(
      controller_->shutdown().status,
      controller_shutdown_status_e::closed
    );
  }

  TEST_F(
    MultiseatControllerRuntimeTest,
    IndeterminateWorkerRetainsInputUntilWorkerFirstReconcileProvesAbsence
  ) {
    create_ready_controller();
    const auto seat = admit_and_bind();
    ASSERT_TRUE(seat.handle.valid());
    const auto identity = controller_worker_identity(seat);
    state_->set_complete_worker_on_stop(false);
    state_->set_next_worker_launch(worker_command_result_e::indeterminate);

    const auto started = controller_->start_seat(
      seat.handle,
      controller_input_plan()
    );
    EXPECT_EQ(started.status, controller_start_status_e::worker_indeterminate);
    EXPECT_EQ(controller_->input_allocations(), 1U);
    EXPECT_FALSE(controller_->admission_ready());

    const auto input_inventory_before = state_->input_inventory_count();
    state_->set_worker_inventory_failure(true);
    const auto uncertain = controller_->reconcile();
    EXPECT_EQ(
      uncertain.status,
      controller_reconcile_status_e::worker_reconciliation_required
    );
    EXPECT_EQ(state_->input_inventory_count(), input_inventory_before);
    EXPECT_EQ(controller_->input_allocations(), 1U);

    state_->set_worker_inventory_failure(false);
    const auto stopping = controller_->reconcile();
    EXPECT_TRUE(stopping.ready());
    EXPECT_EQ(controller_->input_allocations(), 1U);

    ASSERT_TRUE(state_->complete_worker(identity));
    EXPECT_TRUE(controller_->reconcile().ready());
    EXPECT_EQ(controller_->seats(), 0U);
    EXPECT_EQ(controller_->managed_workers(), 0U);
    EXPECT_EQ(controller_->input_allocations(), 0U);
    EXPECT_EQ(
      controller_->shutdown().status,
      controller_shutdown_status_e::closed
    );
  }

  TEST_F(
    MultiseatControllerRuntimeTest,
    ShutdownWaitsForWorkerAbsenceAndClaimedStream
  ) {
    create_ready_controller();
    const auto seat = admit_and_bind();
    ASSERT_TRUE(seat.handle.valid());
    const auto identity = controller_worker_identity(seat);
    state_->set_complete_worker_on_stop(false);
    ASSERT_TRUE(controller_->start_seat(
      seat.handle,
      controller_input_plan()
    ).started());
    ASSERT_TRUE(state_->mark_worker_ready(identity));
    ASSERT_TRUE(controller_->reconcile().ready());

    auto launch = controller_launch(1902, 2902);
    ASSERT_TRUE(controller_->select_authenticated_launch(
      launch,
      seat.handle
    ).selected());
    auto stream = controller_stream(*launch);
    ASSERT_TRUE(stream);
    ASSERT_EQ(
      input::activate_registered_moonlight_session(*stream),
      input::moonlight_session_activation_status_e::bound
    );

    EXPECT_EQ(
      controller_->stop_seat(seat.handle).status,
      controller_stop_status_e::stopping
    );
    ASSERT_TRUE(state_->complete_worker(identity));
    EXPECT_EQ(
      controller_->reconcile().status,
      controller_reconcile_status_e::input_reconciliation_required
    );
    EXPECT_EQ(controller_->seats(), 0U);
    EXPECT_EQ(controller_->managed_workers(), 0U);
    EXPECT_EQ(controller_->input_allocations(), 1U);

    EXPECT_EQ(
      controller_->shutdown().status,
      controller_shutdown_status_e::streams_pending
    );
    EXPECT_TRUE(controller_->shutting_down());
    EXPECT_FALSE(controller_->closed());
    stream::session::stop(*stream);
    EXPECT_EQ(
      controller_->shutdown().status,
      controller_shutdown_status_e::closed
    );
    EXPECT_TRUE(launch->is_cancelled());
  }

  TEST_F(
    MultiseatControllerRuntimeTest,
    ShutdownQuiesceWinsActivationRaceWithoutStoppingWorker
  ) {
    create_ready_controller();
    const auto seat = admit_and_bind();
    ASSERT_TRUE(seat.handle.valid());
    const auto identity = controller_worker_identity(seat);
    ASSERT_TRUE(controller_->start_seat(
      seat.handle,
      controller_input_plan()
    ).started());
    ASSERT_TRUE(state_->mark_worker_ready(identity));
    ASSERT_TRUE(controller_->reconcile().ready());

    auto launch = controller_launch(1903, 2903);
    ASSERT_TRUE(controller_->select_authenticated_launch(
      launch,
      seat.handle
    ).selected());
    auto stream = controller_stream(*launch);
    ASSERT_TRUE(stream);

    auto pause = std::make_shared<activation_pause_t>();
    activation_hook_guard_t hook_guard {[pause]() {
      pause->pause();
    }};
    auto activation = std::async(std::launch::async, [&stream]() {
      return input::activate_registered_moonlight_session(*stream);
    });
    const auto activation_entered = pause->wait_until_entered(
      std::chrono::seconds {2}
    );
    if (!activation_entered) {
      pause->release();
    }
    ASSERT_TRUE(activation_entered);

    auto shutdown = std::async(std::launch::async, [this]() {
      return controller_->shutdown();
    });
    const auto bounded = shutdown.wait_for(std::chrono::seconds {2}) ==
                         std::future_status::ready;
    if (!bounded) {
      pause->release();
    }
    const auto pending = shutdown.get();

    EXPECT_TRUE(bounded);
    EXPECT_EQ(pending.status, controller_shutdown_status_e::streams_pending);
    EXPECT_EQ(pending.stop_requests, 0U);
    EXPECT_FALSE(pending.worker);
    EXPECT_FALSE(stream::session::multiseat_input_bound(*stream));
    EXPECT_EQ(controller_->seats(), 1U);
    EXPECT_EQ(controller_->managed_workers(), 1U);
    EXPECT_EQ(controller_->input_allocations(), 1U);

    pause->release();
    EXPECT_EQ(
      activation.get(),
      input::moonlight_session_activation_status_e::selected_binding_failed
    );
    EXPECT_FALSE(stream::session::multiseat_input_bound(*stream));
    EXPECT_TRUE(launch->is_cancelled());

    const auto closed = controller_->shutdown();
    EXPECT_EQ(closed.status, controller_shutdown_status_e::closed);
    EXPECT_TRUE(controller_->closed());
    EXPECT_EQ(controller_->seats(), 0U);
    EXPECT_EQ(controller_->managed_workers(), 0U);
    EXPECT_EQ(controller_->input_allocations(), 0U);
  }

  // Leaks the retained graph by design; exclude from leak-detecting runs.
  TEST_F(
    MultiseatControllerRuntimeTest,
    DirectDestructorDetachesMoonlightGlobalsAndRetainsGraphForBoundStream
  ) {
    create_ready_controller();
    const auto seat = admit_and_bind();
    ASSERT_TRUE(seat.handle.valid());
    const auto identity = controller_worker_identity(seat);
    ASSERT_TRUE(controller_->start_seat(
      seat.handle,
      controller_input_plan()
    ).started());
    ASSERT_TRUE(state_->mark_worker_ready(identity));
    ASSERT_TRUE(controller_->reconcile().ready());

    auto launch = controller_launch(1904, 2904);
    ASSERT_TRUE(controller_->select_authenticated_launch(
      launch,
      seat.handle
    ).selected());
    auto stream = controller_stream(*launch);
    ASSERT_TRUE(stream);
    ASSERT_EQ(
      input::activate_registered_moonlight_session(*stream),
      input::moonlight_session_activation_status_e::bound
    );
    ASSERT_TRUE(input::moonlight_session_runtime_installed());
    ASSERT_TRUE(input::moonlight_session_activation_gate_installed());

    // The direct owner ignores the retry contract while a stream is bound and
    // a worker is live. Shutdown reports streams_pending, so the destructor
    // must detach both Moonlight globals and retain the graph.
    controller_.reset();

    EXPECT_FALSE(input::moonlight_session_runtime_installed());
    EXPECT_FALSE(input::moonlight_session_activation_gate_installed());
    EXPECT_TRUE(stream::session::multiseat_input_bound(*stream));
    EXPECT_TRUE(stream::session::route_multiseat_input_for_tests(
      *stream,
      controller_keyboard_packet(0x41)
    ));
    EXPECT_EQ(state_->input_route_count(), 1U);
    EXPECT_EQ(state_->input_count(), 1U);
    stream::session::stop(*stream);
    EXPECT_FALSE(stream::session::route_multiseat_input_for_tests(
      *stream,
      controller_keyboard_packet(0x42)
    ));
    EXPECT_EQ(state_->input_route_count(), 1U);
  }

  TEST_F(
    MultiseatControllerRuntimeTest,
    CompositionRootRemainsOutsideCompleteProductionSourceTree
  ) {
    const auto factory = controller_source(
      "src/platform/linux/multiseat_controller_production.cpp"
    );
    ASSERT_NE(
      factory.find("create_production_controller_runtime("),
      std::string::npos
    );

    const auto source_root =
      std::filesystem::path {POLARIS_SOURCE_DIR} / "src";
    std::vector<std::string> unexpected_callers;
    for (const auto &entry :
         std::filesystem::recursive_directory_iterator {source_root}) {
      if (!entry.is_regular_file()) {
        continue;
      }
      const auto relative = std::filesystem::relative(
        entry.path(),
        std::filesystem::path {POLARIS_SOURCE_DIR}
      ).generic_string();
      if (relative ==
            "src/platform/linux/multiseat_controller_production.cpp" ||
          relative ==
            "src/platform/linux/multiseat_controller_production.h") {
        continue;
      }
      const auto source = controller_source(relative);
      ASSERT_FALSE(source.empty()) << relative;
      if (source.find("create_production_controller_runtime") !=
          std::string::npos) {
        unexpected_callers.push_back(relative);
      }
    }
    EXPECT_TRUE(unexpected_callers.empty())
      << testing::PrintToString(unexpected_callers);
  }
}  // namespace

#endif
