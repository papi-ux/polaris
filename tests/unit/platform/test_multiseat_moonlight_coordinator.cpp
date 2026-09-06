/**
 * @file tests/unit/platform/test_multiseat_moonlight_coordinator.cpp
 * @brief Ownership tests for the default-off Moonlight input coordinator.
 */
#include "src/platform/linux/multiseat_moonlight_coordinator.h"
#include "src/rtsp.h"
#include "src/stream.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <future>
#include <gtest/gtest.h>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace {
  using namespace std::chrono_literals;
  using namespace multiseat;
  using namespace multiseat::input;

  seat_handle_t coordinator_handle(
    std::uint64_t generation,
    std::uint32_t slot = 0
  ) {
    return {
      .controller_epoch = "controller-coordinator",
      .logical_gpu_id = "gpu-coordinator",
      .slot = slot,
      .generation = generation,
    };
  }

  expectation_t coordinator_expectation(
    std::uint64_t generation,
    std::uint32_t slot = 0
  ) {
    return {
      .handle = coordinator_handle(generation, slot),
      .input_seat = "polaris-input-coordinator-" +
                    std::to_string(generation),
      .plan = {.touch = true, .pen = true, .gamepad_slots = 1},
    };
  }

  allocation_t coordinator_allocation(const expectation_t &expectation) {
    allocation_t allocation {
      .handle = expectation.handle,
      .input_seat = expectation.input_seat,
      .plan = expectation.plan,
    };
    const std::vector<std::pair<device_kind_e, std::uint32_t>> required {
      {device_kind_e::keyboard, 0},
      {device_kind_e::mouse_relative, 0},
      {device_kind_e::mouse_absolute, 0},
      {device_kind_e::touch, 0},
      {device_kind_e::pen, 0},
      {device_kind_e::gamepad, 0},
    };
    for (std::size_t index = 0; index < required.size(); ++index) {
      const auto [kind, slot] = required[index];
      const auto identity = expectation.handle.generation * 16 + index;
      allocation.nodes.push_back({
        .kind = kind,
        .slot = slot,
        .host_path = "/dev/input/event" + std::to_string(identity),
        .worker_path = expected_worker_path(kind, slot),
        .filesystem_device = 43,
        .inode = 7000 + identity,
        .character_major = 13,
        .character_minor = static_cast<std::uint32_t>(64 + identity),
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

  class coordinator_backend_t final: public backend_t {
  public:
    explicit coordinator_backend_t(
      moonlight_controller_feedback_sink_t feedback_sink
    ):
        feedback_sink_(std::move(feedback_sink)) {
    }

    backend_create_result_t create(const expectation_t &expectation) override {
      ++create_calls;
      auto allocation = coordinator_allocation(expectation);
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
      ++destroy_calls;
      if (reject_destroy) {
        return backend_result_e::rejected;
      }
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
      ++route_calls;
      return backend_result_e::applied;
    }

    std::vector<allocation_t> inventory() override {
      ++inventory_calls;
      return allocations;
    }

    moonlight_controller_feedback_sink_t feedback_sink_;
    std::vector<allocation_t> allocations;
    std::size_t create_calls = 0;
    std::size_t destroy_calls = 0;
    std::size_t route_calls = 0;
    std::size_t inventory_calls = 0;
    bool reject_destroy = false;
  };

  struct coordinator_backend_factory_state_t {
    coordinator_backend_t *backend = nullptr;
    std::size_t calls = 0;
  };

  moonlight_input_backend_factory_t coordinator_backend_factory(
    const std::shared_ptr<coordinator_backend_factory_state_t> &state
  ) {
    return [state](moonlight_controller_feedback_sink_t feedback_sink) {
      ++state->calls;
      auto backend = std::make_unique<coordinator_backend_t>(
        std::move(feedback_sink)
      );
      state->backend = backend.get();
      return backend;
    };
  }

  std::shared_ptr<rtsp_stream::launch_session_t> coordinator_launch(
    std::uint32_t id,
    std::uint64_t lifecycle_generation
  ) {
    auto launch = std::make_shared<rtsp_stream::launch_session_t>();
    launch->id = id;
    launch->lifecycle_generation = lifecycle_generation;
    launch->gcm_key.resize(16);
    launch->iv.resize(16);
    launch->device_name = "coordinator-test-client";
    launch->unique_id = "coordinator-test-client-" + std::to_string(id) +
                        "-" + std::to_string(lifecycle_generation);
    launch->session_token = "test-token";
    launch->perm = crypto::PERM::_game_control;
    launch->watch_only = false;
    return launch;
  }

  std::shared_ptr<stream::session_t> coordinator_stream(
    rtsp_stream::launch_session_t &launch
  ) {
    stream::config_t config {};
    return stream::session::alloc(config, launch);
  }

  std::string coordinator_read_source(const std::filesystem::path &path) {
    std::ifstream input {path};
    std::ostringstream contents;
    contents << input.rdbuf();
    return contents.str();
  }

  std::size_t coordinator_source_occurrences(std::string_view needle) {
    const auto source_root = std::filesystem::path {POLARIS_SOURCE_DIR} / "src";
    std::size_t occurrences = 0;
    for (const auto &entry:
         std::filesystem::recursive_directory_iterator {source_root}) {
      if (!entry.is_regular_file() ||
          (entry.path().extension() != ".h" &&
           entry.path().extension() != ".cpp")) {
        continue;
      }
      const auto contents = coordinator_read_source(entry.path());
      for (auto offset = contents.find(needle); offset != std::string::npos;
           offset = contents.find(needle, offset + needle.size())) {
        ++occurrences;
      }
    }
    return occurrences;
  }

  TEST(MultiseatMoonlightCoordinator, DefaultsDisabledWithoutBackendMutation) {
    ASSERT_FALSE(moonlight_session_activation_gate_installed());

    const auto invalid = moonlight_session_coordinator_t::create({}, {});
    EXPECT_EQ(
      invalid.status,
      moonlight_coordinator_create_status_e::invalid_factory
    );
    EXPECT_FALSE(invalid.coordinator);

    const auto unavailable = moonlight_session_coordinator_t::create(
      {},
      [](moonlight_controller_feedback_sink_t) {
        return std::unique_ptr<backend_t> {};
      }
    );
    EXPECT_EQ(
      unavailable.status,
      moonlight_coordinator_create_status_e::backend_unavailable
    );
    EXPECT_FALSE(unavailable.coordinator);

    const auto throwing = moonlight_session_coordinator_t::create(
      {},
      [](moonlight_controller_feedback_sink_t)
        -> std::unique_ptr<backend_t> {
        throw std::runtime_error {"backend construction failed"};
      }
    );
    EXPECT_EQ(
      throwing.status,
      moonlight_coordinator_create_status_e::backend_unavailable
    );
    EXPECT_FALSE(throwing.coordinator);

    auto factory = std::make_shared<coordinator_backend_factory_state_t>();
    auto created = moonlight_session_coordinator_t::create(
      {},
      coordinator_backend_factory(factory)
    );
    ASSERT_EQ(
      created.status,
      moonlight_coordinator_create_status_e::ready_disabled
    );
    ASSERT_TRUE(created.coordinator);
    ASSERT_NE(factory->backend, nullptr);
    EXPECT_EQ(factory->calls, 1U);
    EXPECT_FALSE(created.coordinator->enabled());
    EXPECT_FALSE(created.coordinator->activation_installed());
    EXPECT_FALSE(moonlight_session_activation_gate_installed());

    const auto expectation = coordinator_expectation(1);
    EXPECT_EQ(
      created.coordinator->reconcile_inputs({}).status,
      moonlight_coordinator_operation_status_e::disabled
    );
    EXPECT_EQ(
      created.coordinator->prepare_input(expectation).status,
      moonlight_coordinator_operation_status_e::disabled
    );
    auto launch = coordinator_launch(400, 500);
    EXPECT_EQ(
      created.coordinator->select_launch(launch, expectation.handle, true),
      moonlight_launch_selection_status_e::gate_disabled
    );
    EXPECT_EQ(
      created.coordinator->cancel_launch(launch),
      moonlight_coordinator_cancel_status_e::coordinator_disabled
    );
    EXPECT_EQ(factory->backend->inventory_calls, 0U);
    EXPECT_EQ(factory->backend->create_calls, 0U);
    EXPECT_EQ(factory->backend->destroy_calls, 0U);

    EXPECT_EQ(
      created.coordinator->shutdown().status,
      moonlight_coordinator_shutdown_status_e::closed
    );
    EXPECT_TRUE(created.coordinator->closed());
    EXPECT_FALSE(moonlight_session_activation_gate_installed());
  }

  TEST(MultiseatMoonlightCoordinator, InstallationConflictKeepsFirstOwner) {
    auto first_factory =
      std::make_shared<coordinator_backend_factory_state_t>();
    auto first = moonlight_session_coordinator_t::create(
      {.enabled = true},
      coordinator_backend_factory(first_factory)
    );
    ASSERT_TRUE(first.coordinator);
    ASSERT_TRUE(first.coordinator->activation_installed());

    auto second_factory =
      std::make_shared<coordinator_backend_factory_state_t>();
    const auto second = moonlight_session_coordinator_t::create(
      {.enabled = true},
      coordinator_backend_factory(second_factory)
    );
    EXPECT_EQ(
      second.status,
      moonlight_coordinator_create_status_e::activation_install_rejected
    );
    EXPECT_EQ(
      second.activation_status,
      moonlight_activation_install_status_e::already_installed
    );
    EXPECT_FALSE(second.coordinator);
    EXPECT_TRUE(first.coordinator->activation_installed());
    EXPECT_TRUE(moonlight_session_activation_gate_installed());

    EXPECT_EQ(
      first.coordinator->shutdown().status,
      moonlight_coordinator_shutdown_status_e::closed
    );
    EXPECT_FALSE(moonlight_session_activation_gate_installed());
  }

  TEST(MultiseatMoonlightCoordinator, OwnsEnabledGateAndInputAuthority) {
    auto factory = std::make_shared<coordinator_backend_factory_state_t>();
    auto created = moonlight_session_coordinator_t::create(
      {.enabled = true},
      coordinator_backend_factory(factory)
    );
    ASSERT_EQ(
      created.status,
      moonlight_coordinator_create_status_e::ready_enabled
    );
    ASSERT_EQ(
      created.activation_status,
      moonlight_activation_install_status_e::installed
    );
    ASSERT_TRUE(created.coordinator);
    ASSERT_NE(factory->backend, nullptr);
    EXPECT_TRUE(created.coordinator->activation_installed());

    const auto reconciled = created.coordinator->reconcile_inputs({});
    EXPECT_EQ(
      reconciled.status,
      moonlight_coordinator_operation_status_e::applied
    );
    EXPECT_TRUE(reconciled.report.admission_ready);
    EXPECT_EQ(factory->backend->inventory_calls, 1U);

    const auto expectation = coordinator_expectation(2);
    const auto prepared = created.coordinator->prepare_input(expectation);
    EXPECT_EQ(
      prepared.status,
      moonlight_coordinator_operation_status_e::applied
    );
    EXPECT_TRUE(prepared.input.prepared());
    EXPECT_EQ(created.coordinator->input_allocations(), 1U);

    const auto released = created.coordinator->release_input(expectation.handle);
    EXPECT_EQ(
      released.status,
      moonlight_coordinator_operation_status_e::applied
    );
    EXPECT_EQ(released.input_status, status_e::applied);
    EXPECT_EQ(factory->backend->destroy_calls, 1U);
    EXPECT_EQ(created.coordinator->input_allocations(), 0U);

    EXPECT_EQ(
      created.coordinator->shutdown().status,
      moonlight_coordinator_shutdown_status_e::closed
    );
    EXPECT_EQ(
      created.coordinator->shutdown().status,
      moonlight_coordinator_shutdown_status_e::already_closed
    );
    EXPECT_FALSE(moonlight_session_activation_gate_installed());
  }

  TEST(MultiseatMoonlightCoordinator, SelectedAndUnselectedLaunchesCoexist) {
    auto factory = std::make_shared<coordinator_backend_factory_state_t>();
    auto created = moonlight_session_coordinator_t::create(
      {.enabled = true},
      coordinator_backend_factory(factory)
    );
    ASSERT_TRUE(created.coordinator);
    ASSERT_TRUE(created.coordinator->reconcile_inputs({}).report.admission_ready);
    const auto selected_expectation = coordinator_expectation(3, 0);
    const auto unselected_expectation = coordinator_expectation(4, 1);
    ASSERT_TRUE(
      created.coordinator->prepare_input(selected_expectation).input.prepared()
    );
    ASSERT_TRUE(
      created.coordinator->prepare_input(unselected_expectation).input.prepared()
    );

    auto selected_launch = coordinator_launch(401, 501);
    auto unselected_launch = coordinator_launch(402, 502);
    ASSERT_EQ(
      created.coordinator->select_launch(
        selected_launch,
        selected_expectation.handle,
        true
      ),
      moonlight_launch_selection_status_e::registered
    );
    EXPECT_EQ(created.coordinator->active_launches(), 1U);
    EXPECT_EQ(created.coordinator->retained_launches(), 1U);
    EXPECT_EQ(
      created.coordinator->reconcile_inputs({unselected_expectation}).status,
      moonlight_coordinator_operation_status_e::selection_retained
    );
    EXPECT_EQ(factory->backend->destroy_calls, 0U);
    EXPECT_EQ(created.coordinator->input_allocations(), 2U);

    auto selected_stream = coordinator_stream(*selected_launch);
    auto unselected_stream = coordinator_stream(*unselected_launch);
    ASSERT_TRUE(selected_stream);
    ASSERT_TRUE(unselected_stream);
    EXPECT_EQ(
      activate_registered_moonlight_session(*unselected_stream),
      moonlight_session_activation_status_e::unselected
    );
    EXPECT_EQ(
      activate_registered_moonlight_session(*selected_stream),
      moonlight_session_activation_status_e::bound
    );
    EXPECT_TRUE(stream::session::multiseat_input_bound(*selected_stream));
    EXPECT_FALSE(stream::session::multiseat_input_bound(*unselected_stream));
    EXPECT_EQ(created.coordinator->registered_sessions(), 1U);
    EXPECT_EQ(created.coordinator->claimed_sessions(), 1U);
    EXPECT_EQ(created.coordinator->feedback_subscriptions(), 1U);

    EXPECT_EQ(
      created.coordinator->release_input(selected_expectation.handle).status,
      moonlight_coordinator_operation_status_e::selection_retained
    );
    EXPECT_EQ(
      created.coordinator->cancel_launch(selected_launch),
      moonlight_coordinator_cancel_status_e::cancelled
    );
    selected_launch->cancel();
    EXPECT_EQ(
      created.coordinator->retire_cancelled_launch(selected_launch),
      moonlight_coordinator_retire_status_e::stream_still_bound
    );
    stream::session::stop(*selected_stream);
    EXPECT_EQ(created.coordinator->claimed_sessions(), 0U);
    EXPECT_EQ(created.coordinator->feedback_subscriptions(), 0U);
    EXPECT_EQ(
      created.coordinator->retire_cancelled_launch(selected_launch),
      moonlight_coordinator_retire_status_e::retired
    );
    EXPECT_EQ(
      created.coordinator->release_input(selected_expectation.handle).input_status,
      status_e::applied
    );
    EXPECT_EQ(
      created.coordinator->release_input(unselected_expectation.handle).input_status,
      status_e::applied
    );
    EXPECT_EQ(
      created.coordinator->shutdown().status,
      moonlight_coordinator_shutdown_status_e::closed
    );
  }

  TEST(MultiseatMoonlightCoordinator, CancellationStaysFailClosedUntilRtspRetires) {
    auto factory = std::make_shared<coordinator_backend_factory_state_t>();
    auto created = moonlight_session_coordinator_t::create(
      {.enabled = true},
      coordinator_backend_factory(factory)
    );
    ASSERT_TRUE(created.coordinator);
    ASSERT_TRUE(created.coordinator->reconcile_inputs({}).report.admission_ready);
    const auto expectation = coordinator_expectation(5);
    ASSERT_TRUE(created.coordinator->prepare_input(expectation).input.prepared());
    auto launch = coordinator_launch(403, 503);
    ASSERT_EQ(
      created.coordinator->select_launch(launch, expectation.handle, false),
      moonlight_launch_selection_status_e::registered
    );
    auto lookalike = coordinator_launch(403, 503);
    lookalike->cancel();
    EXPECT_EQ(
      created.coordinator->cancel_launch(lookalike),
      moonlight_coordinator_cancel_status_e::invalid_launch
    );
    EXPECT_EQ(
      created.coordinator->retire_cancelled_launch(lookalike),
      moonlight_coordinator_retire_status_e::invalid_launch
    );

    EXPECT_EQ(
      created.coordinator->cancel_launch(launch),
      moonlight_coordinator_cancel_status_e::cancelled
    );
    EXPECT_EQ(
      created.coordinator->cancel_launch(launch),
      moonlight_coordinator_cancel_status_e::already_cancelled
    );
    EXPECT_EQ(created.coordinator->active_launches(), 0U);
    EXPECT_EQ(created.coordinator->retained_launches(), 1U);
    auto stream = coordinator_stream(*launch);
    ASSERT_TRUE(stream);
    EXPECT_EQ(
      activate_registered_moonlight_session(*stream),
      moonlight_session_activation_status_e::selection_cancelled
    );
    EXPECT_EQ(
      created.coordinator->select_launch(launch, expectation.handle, false),
      moonlight_launch_selection_status_e::duplicate_session
    );
    EXPECT_EQ(
      created.coordinator->retire_cancelled_launch(launch),
      moonlight_coordinator_retire_status_e::launch_still_admissible
    );
    EXPECT_EQ(
      created.coordinator->release_input(expectation.handle).status,
      moonlight_coordinator_operation_status_e::selection_retained
    );

    launch->cancel();
    ASSERT_TRUE(launch->is_cancelled());
    EXPECT_EQ(
      created.coordinator->retire_cancelled_launch(launch),
      moonlight_coordinator_retire_status_e::retired
    );
    EXPECT_EQ(created.coordinator->retained_launches(), 0U);
    EXPECT_EQ(
      activate_registered_moonlight_session(*stream),
      moonlight_session_activation_status_e::unselected
    );
    EXPECT_EQ(
      created.coordinator->release_input(expectation.handle).input_status,
      status_e::applied
    );
    EXPECT_EQ(
      created.coordinator->shutdown().status,
      moonlight_coordinator_shutdown_status_e::closed
    );
  }

  TEST(MultiseatMoonlightCoordinator, ShutdownWaitsForBoundStreamAndClosesGate) {
    auto factory = std::make_shared<coordinator_backend_factory_state_t>();
    auto created = moonlight_session_coordinator_t::create(
      {.enabled = true},
      coordinator_backend_factory(factory)
    );
    ASSERT_TRUE(created.coordinator);
    ASSERT_TRUE(created.coordinator->reconcile_inputs({}).report.admission_ready);
    const auto expectation = coordinator_expectation(6);
    ASSERT_TRUE(created.coordinator->prepare_input(expectation).input.prepared());
    auto launch = coordinator_launch(404, 504);
    ASSERT_EQ(
      created.coordinator->select_launch(launch, expectation.handle, true),
      moonlight_launch_selection_status_e::registered
    );
    auto stream = coordinator_stream(*launch);
    ASSERT_TRUE(stream);
    ASSERT_EQ(
      activate_registered_moonlight_session(*stream),
      moonlight_session_activation_status_e::bound
    );

    auto shutdown = std::async(std::launch::async, [&created]() {
      return created.coordinator->shutdown();
    });
    auto control_launch = coordinator_launch(405, 505);
    auto control_stream = coordinator_stream(*control_launch);
    auto gate_closed = false;
    for (auto attempt = 0; attempt < 200 && !gate_closed; ++attempt) {
      gate_closed =
        activate_registered_moonlight_session(*control_stream) ==
        moonlight_session_activation_status_e::gate_closed;
      if (!gate_closed) {
        std::this_thread::sleep_for(1ms);
      }
    }
    const auto waited_for_owner = shutdown.wait_for(20ms) ==
                                  std::future_status::timeout;
    stream::session::stop(*stream);
    const auto report = shutdown.get();

    EXPECT_TRUE(gate_closed);
    EXPECT_TRUE(waited_for_owner);
    EXPECT_EQ(report.status, moonlight_coordinator_shutdown_status_e::closed);
    EXPECT_EQ(report.released_allocations, 1U);
    EXPECT_EQ(report.cleanup_failures, 0U);
    EXPECT_EQ(factory->backend->destroy_calls, 1U);
    EXPECT_EQ(created.coordinator->claimed_sessions(), 0U);
    EXPECT_EQ(created.coordinator->feedback_subscriptions(), 0U);
    EXPECT_TRUE(created.coordinator->closed());
    EXPECT_FALSE(moonlight_session_activation_gate_installed());
  }

  TEST(MultiseatMoonlightCoordinator, FailedInputCleanupKeepsClosedGateForRetry) {
    auto factory = std::make_shared<coordinator_backend_factory_state_t>();
    auto created = moonlight_session_coordinator_t::create(
      {.enabled = true},
      coordinator_backend_factory(factory)
    );
    ASSERT_TRUE(created.coordinator);
    ASSERT_TRUE(created.coordinator->reconcile_inputs({}).report.admission_ready);
    const auto expectation = coordinator_expectation(7);
    ASSERT_TRUE(created.coordinator->prepare_input(expectation).input.prepared());
    factory->backend->reject_destroy = true;

    const auto failed = created.coordinator->shutdown();
    EXPECT_EQ(
      failed.status,
      moonlight_coordinator_shutdown_status_e::input_cleanup_incomplete
    );
    EXPECT_EQ(failed.cleanup_failures, 1U);
    EXPECT_TRUE(created.coordinator->shutting_down());
    EXPECT_FALSE(created.coordinator->closed());
    EXPECT_TRUE(created.coordinator->activation_installed());
    auto control_launch = coordinator_launch(406, 506);
    auto control_stream = coordinator_stream(*control_launch);
    EXPECT_EQ(
      activate_registered_moonlight_session(*control_stream),
      moonlight_session_activation_status_e::gate_closed
    );
    EXPECT_EQ(
      created.coordinator->prepare_input(coordinator_expectation(8)).status,
      moonlight_coordinator_operation_status_e::shutting_down
    );

    factory->backend->reject_destroy = false;
    const auto retried = created.coordinator->shutdown();
    EXPECT_EQ(retried.status, moonlight_coordinator_shutdown_status_e::closed);
    EXPECT_EQ(retried.released_allocations, 1U);
    EXPECT_EQ(retried.cleanup_failures, 0U);
    EXPECT_FALSE(moonlight_session_activation_gate_installed());
  }

  TEST(MultiseatMoonlightCoordinator, SourceRemainsDefaultOffAndRuntimeOwned) {
    const auto header = coordinator_read_source(
      std::filesystem::path {POLARIS_SOURCE_DIR} /
      "src/platform/linux/multiseat_moonlight_coordinator.h"
    );
    EXPECT_NE(header.find("bool enabled = false;"), std::string::npos);

    // One occurrence is the factory definition and one is the new runtime
    // owner. No request handler or ordinary stream path constructs it.
    EXPECT_EQ(
      coordinator_source_occurrences(
        "moonlight_session_coordinator_t::create("
      ),
      2U
    );

    const auto runtime = coordinator_read_source(
      std::filesystem::path {POLARIS_SOURCE_DIR} /
      "src/platform/linux/multiseat_moonlight_runtime.cpp"
    );
    EXPECT_NE(
      runtime.find("if (!options.enabled)"),
      std::string::npos
    );
    EXPECT_LT(
      runtime.find("if (!options.enabled)"),
      runtime.find("moonlight_session_coordinator_t::create(")
    );
  }
}  // namespace
