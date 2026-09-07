/**
 * @file tests/unit/platform/test_multiseat_moonlight_worker_adapter.cpp
 * @brief Authenticated worker-to-Moonlight selection tests.
 */
#include "src/platform/linux/multiseat_moonlight_worker_adapter.h"
#include "src/rtsp.h"

#ifdef __linux__

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <gtest/gtest.h>
#include <memory>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace {
  using namespace multiseat;
  using namespace multiseat::input;

  seat_handle_t adapter_handle(std::uint64_t generation) {
    return {
      .controller_epoch = "controller-adapter",
      .logical_gpu_id = "gpu-adapter",
      .slot = 0,
      .generation = generation,
    };
  }

  expectation_t adapter_expectation(
    std::uint64_t generation,
    std::uint32_t gamepad_slots = 0
  ) {
    return {
      .handle = adapter_handle(generation),
      .input_seat = "polaris-input-adapter-" +
                    std::to_string(generation),
      .plan = {.gamepad_slots = gamepad_slots},
    };
  }

  allocation_t adapter_allocation(const expectation_t &expectation) {
    allocation_t allocation {
      .handle = expectation.handle,
      .input_seat = expectation.input_seat,
      .plan = expectation.plan,
    };
    std::vector<std::pair<device_kind_e, std::uint32_t>> required {
      {device_kind_e::keyboard, 0},
      {device_kind_e::mouse_relative, 0},
      {device_kind_e::mouse_absolute, 0},
    };
    for (std::uint32_t slot = 0;
         slot < expectation.plan.gamepad_slots;
         ++slot) {
      required.emplace_back(device_kind_e::gamepad, slot);
    }
    for (std::size_t index = 0; index < required.size(); ++index) {
      const auto [kind, slot] = required[index];
      const auto identity = expectation.handle.generation * 32 + index;
      allocation.nodes.push_back({
        .kind = kind,
        .slot = slot,
        .host_path = "/dev/input/event" + std::to_string(256 + identity),
        .worker_path = expected_worker_path(kind, slot),
        .filesystem_device = 53,
        .inode = 11000 + identity,
        .character_major = 13,
        .character_minor = static_cast<std::uint32_t>(256 + identity),
        .kernel_name = expected_kernel_name(
          expectation.input_seat,
          kind,
          slot
        ),
        .host_seat = std::string {isolated_host_seat},
      });
    }
    return allocation;
  }

  class adapter_backend_t final : public backend_t {
  public:
    backend_create_result_t create(const expectation_t &expectation) override {
      auto allocation = adapter_allocation(expectation);
      allocations.push_back(allocation);
      return {
        .result = backend_result_e::applied,
        .allocation = std::move(allocation),
      };
    }

    backend_result_e destroy(
      const seat_handle_t &handle,
      std::string_view input_seat
    ) override {
      const auto found = std::find_if(
        allocations.begin(),
        allocations.end(),
        [&handle, input_seat](const auto &allocation) {
          return allocation.handle == handle &&
                 allocation.input_seat == input_seat;
        }
      );
      if (found == allocations.end()) {
        return backend_result_e::not_found;
      }
      allocations.erase(found);
      return backend_result_e::applied;
    }

    backend_result_e route(
      const seat_handle_t &,
      std::string_view,
      std::uint64_t,
      const input_event_t &
    ) override {
      return backend_result_e::applied;
    }

    std::vector<allocation_t> inventory() override {
      return allocations;
    }

    std::vector<allocation_t> allocations;
  };

  class fake_worker_authority_t final :
      public authenticated_worker_seat_authority_t {
  public:
    explicit fake_worker_authority_t(authenticated_worker_seat_t seat) :
        seat_(std::move(seat)) {
    }

    worker_seat_authorization_status_e with_authenticated_worker_seat(
      const seat_handle_t &handle,
      const authenticated_worker_seat_action_t &action
    ) override {
      ++calls;
      requested_handle = handle;
      if (next_status != worker_seat_authorization_status_e::applied) {
        return next_status;
      }
      try {
        action(seat_);
      } catch (...) {
        return worker_seat_authorization_status_e::action_failed;
      }
      return worker_seat_authorization_status_e::applied;
    }

    authenticated_worker_seat_t seat_;
    worker_seat_authorization_status_e next_status =
      worker_seat_authorization_status_e::applied;
    seat_handle_t requested_handle;
    std::size_t calls = 0;
  };

  authenticated_worker_seat_t worker_seat(
    const expectation_t &expectation,
    std::string client_key = "paired-client"
  ) {
    return {
      .handle = expectation.handle,
      .worker_name = "polaris-worker-adapter-" +
                     std::to_string(expectation.handle.generation),
      .input_seat = expectation.input_seat,
      .client_key = std::move(client_key),
    };
  }

  std::shared_ptr<rtsp_stream::launch_session_t> adapter_launch(
    std::uint32_t id,
    std::uint64_t generation,
    crypto::PERM permission = crypto::PERM::view,
    std::string client_key = "paired-client"
  ) {
    auto launch = std::make_shared<rtsp_stream::launch_session_t>();
    launch->id = id;
    launch->lifecycle_generation = generation;
    launch->unique_id = std::move(client_key);
    launch->perm = permission;
    return launch;
  }

  moonlight_runtime_create_result_t adapter_runtime() {
    return moonlight_session_runtime_t::create(
      {.enabled = true},
      [](moonlight_controller_feedback_sink_t) {
        return std::make_unique<adapter_backend_t>();
      }
    );
  }

  void prepare(
    moonlight_session_runtime_t &runtime,
    const expectation_t &expectation
  ) {
    ASSERT_TRUE(runtime.reconcile_inputs({}).report.admission_ready);
    ASSERT_TRUE(runtime.prepare_input(expectation).input.prepared());
  }

  void retire(
    moonlight_session_runtime_t &runtime,
    const std::shared_ptr<rtsp_stream::launch_session_t> &launch
  ) {
    EXPECT_EQ(
      runtime.cancel_launch(launch),
      moonlight_runtime_lifecycle_status_e::retired
    );
  }

  std::string adapter_source(std::string_view relative_path) {
    std::ifstream input {
      std::filesystem::path {POLARIS_SOURCE_DIR} / relative_path
    };
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
  }

  TEST(MultiseatMoonlightWorkerAdapter, InvalidLaunchNeverConsultsWorker) {
    const auto expectation = adapter_expectation(1);
    fake_worker_authority_t worker {worker_seat(expectation)};
    moonlight_worker_launch_adapter_t adapter {worker};

    const auto result = adapter.select({}, expectation.handle);
    EXPECT_EQ(result.status, moonlight_worker_selection_status_e::invalid_launch);
    EXPECT_EQ(worker.calls, 0U);
    EXPECT_FALSE(result.selection_status);
  }

  TEST(MultiseatMoonlightWorkerAdapter, WorkerRejectionCannotReachRuntime) {
    const auto expectation = adapter_expectation(2);
    fake_worker_authority_t worker {worker_seat(expectation)};
    worker.next_status =
      worker_seat_authorization_status_e::endpoint_not_authenticated;
    moonlight_worker_launch_adapter_t adapter {worker};

    const auto result = adapter.select(
      adapter_launch(1001, 2001),
      expectation.handle
    );
    EXPECT_EQ(
      result.status,
      moonlight_worker_selection_status_e::worker_not_authorized
    );
    EXPECT_EQ(result.authority_status, worker.next_status);
    EXPECT_FALSE(result.selection_status);
  }

  TEST(MultiseatMoonlightWorkerAdapter, RejectsMismatchedAuthorityProjection) {
    const auto expectation = adapter_expectation(3);
    auto wrong = worker_seat(expectation);
    ++wrong.handle.generation;
    fake_worker_authority_t worker {std::move(wrong)};
    moonlight_worker_launch_adapter_t adapter {worker};

    const auto result = adapter.select(
      adapter_launch(1002, 2002),
      expectation.handle
    );
    EXPECT_EQ(
      result.status,
      moonlight_worker_selection_status_e::authority_mismatch
    );
    EXPECT_FALSE(result.selection_status);
  }

  TEST(MultiseatMoonlightWorkerAdapter, PairedClientMustMatchSeatAdmission) {
    const auto expectation = adapter_expectation(4);
    fake_worker_authority_t worker {
      worker_seat(expectation, "different-client")
    };
    moonlight_worker_launch_adapter_t adapter {worker};

    const auto result = adapter.select(
      adapter_launch(1003, 2003),
      expectation.handle
    );
    EXPECT_EQ(
      result.status,
      moonlight_worker_selection_status_e::client_mismatch
    );
    EXPECT_FALSE(result.selection_status);
  }

  TEST(MultiseatMoonlightWorkerAdapter, MatchingWorkerNeedsInstalledRuntime) {
    const auto expectation = adapter_expectation(5);
    fake_worker_authority_t worker {worker_seat(expectation)};
    moonlight_worker_launch_adapter_t adapter {worker};

    ASSERT_FALSE(moonlight_session_runtime_installed());
    const auto result = adapter.select(
      adapter_launch(1004, 2004),
      expectation.handle
    );
    EXPECT_EQ(
      result.status,
      moonlight_worker_selection_status_e::runtime_unavailable
    );
    EXPECT_FALSE(result.selection_status);
  }

  TEST(MultiseatMoonlightWorkerAdapter, ExactInputSeatIsPartOfAuthority) {
    auto created = adapter_runtime();
    ASSERT_TRUE(created.runtime);
    const auto expectation = adapter_expectation(6);
    prepare(*created.runtime, expectation);
    auto authenticated = worker_seat(expectation);
    authenticated.input_seat = "polaris-input-cross-wired";
    fake_worker_authority_t worker {std::move(authenticated)};
    moonlight_worker_launch_adapter_t adapter {worker};

    const auto result = adapter.select(
      adapter_launch(1005, 2005),
      expectation.handle
    );
    EXPECT_EQ(
      result.status,
      moonlight_worker_selection_status_e::selection_rejected
    );
    EXPECT_EQ(
      result.selection_status,
      moonlight_launch_selection_status_e::seat_not_admitted
    );
    EXPECT_EQ(created.runtime->tracked_launches(), 0U);
    EXPECT_EQ(
      created.runtime->shutdown().status,
      moonlight_coordinator_shutdown_status_e::closed
    );
  }

  TEST(MultiseatMoonlightWorkerAdapter, PermissionsDeriveControllerFeedback) {
    auto created = adapter_runtime();
    ASSERT_TRUE(created.runtime);
    const auto expectation = adapter_expectation(7, 0);
    prepare(*created.runtime, expectation);
    fake_worker_authority_t worker {worker_seat(expectation)};
    moonlight_worker_launch_adapter_t adapter {worker};

    auto viewer = adapter_launch(1006, 2006, crypto::PERM::view);
    const auto selected = adapter.select(viewer, expectation.handle);
    ASSERT_TRUE(selected.selected());
    EXPECT_EQ(created.runtime->tracked_launches(), 1U);
    retire(*created.runtime, viewer);

    auto controller = adapter_launch(
      1007,
      2007,
      crypto::PERM::_game_control
    );
    const auto rejected = adapter.select(controller, expectation.handle);
    EXPECT_EQ(
      rejected.status,
      moonlight_worker_selection_status_e::selection_rejected
    );
    EXPECT_EQ(
      rejected.selection_status,
      moonlight_launch_selection_status_e::seat_not_admitted
    );
    EXPECT_EQ(created.runtime->tracked_launches(), 0U);
    EXPECT_EQ(
      created.runtime->shutdown().status,
      moonlight_coordinator_shutdown_status_e::closed
    );
  }

  TEST(MultiseatMoonlightWorkerAdapter, SelectsExactAuthenticatedController) {
    auto created = adapter_runtime();
    ASSERT_TRUE(created.runtime);
    const auto expectation = adapter_expectation(8, 1);
    prepare(*created.runtime, expectation);
    fake_worker_authority_t worker {worker_seat(expectation)};
    moonlight_worker_launch_adapter_t adapter {worker};
    auto launch = adapter_launch(1008, 2008, crypto::PERM::_game_control);

    const auto result = adapter.select(launch, expectation.handle);
    EXPECT_TRUE(result.selected());
    EXPECT_EQ(worker.requested_handle, expectation.handle);
    EXPECT_EQ(created.runtime->tracked_launches(), 1U);
    retire(*created.runtime, launch);
    EXPECT_EQ(
      created.runtime->shutdown().status,
      moonlight_coordinator_shutdown_status_e::closed
    );
  }

  TEST(MultiseatMoonlightWorkerAdapter, RemainsOutsideRequestAndSingletonPaths) {
    const auto adapter = adapter_source(
      "src/platform/linux/multiseat_moonlight_worker_adapter.cpp"
    );
    ASSERT_NE(
      adapter.find("select_authenticated_moonlight_launch("),
      std::string::npos
    );
    for (const auto path : {
           "src/nvhttp.cpp",
           "src/confighttp.cpp",
           "src/main.cpp",
         }) {
      const auto source = adapter_source(path);
      ASSERT_FALSE(source.empty()) << path;
      EXPECT_EQ(
        source.find("moonlight_worker_launch_adapter_t"),
        std::string::npos
      ) << path;
    }
  }
}  // namespace

#endif
