/**
 * @file tests/unit/platform/test_multiseat_moonlight_runtime.cpp
 * @brief Offline lifecycle tests for configured Moonlight multiseat input.
 */
#include "src/platform/linux/multiseat_moonlight_runtime.h"
#include "src/rtsp.h"
#include "src/stream.h"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <future>
#include <gtest/gtest.h>
#include <memory>
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

  seat_handle_t runtime_handle(
    std::uint64_t generation,
    std::uint32_t slot = 0
  ) {
    return {
      .controller_epoch = "controller-runtime",
      .logical_gpu_id = "gpu-runtime",
      .slot = slot,
      .generation = generation,
    };
  }

  expectation_t runtime_expectation(
    std::uint64_t generation,
    std::uint32_t slot = 0
  ) {
    return {
      .handle = runtime_handle(generation, slot),
      .input_seat = "polaris-input-runtime-" +
                    std::to_string(generation),
      .plan = {.touch = true, .pen = true, .gamepad_slots = 1},
    };
  }

  allocation_t runtime_allocation(const expectation_t &expectation) {
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
        .filesystem_device = 47,
        .inode = 9000 + identity,
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

  class runtime_backend_t final: public backend_t {
  public:
    backend_create_result_t create(const expectation_t &expectation) override {
      ++create_calls;
      auto allocation = runtime_allocation(expectation);
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
      return backend_result_e::applied;
    }

    std::vector<allocation_t> inventory() override {
      ++inventory_calls;
      return allocations;
    }

    std::vector<allocation_t> allocations;
    std::size_t create_calls = 0;
    std::size_t destroy_calls = 0;
    std::size_t inventory_calls = 0;
    bool reject_destroy = false;
  };

  struct runtime_factory_state_t {
    runtime_backend_t *backend = nullptr;
    std::size_t calls = 0;
  };

  moonlight_input_backend_factory_t runtime_backend_factory(
    const std::shared_ptr<runtime_factory_state_t> &state
  ) {
    return [state](moonlight_controller_feedback_sink_t) {
      ++state->calls;
      auto backend = std::make_unique<runtime_backend_t>();
      state->backend = backend.get();
      return backend;
    };
  }

  std::shared_ptr<rtsp_stream::launch_session_t> runtime_launch(
    std::uint32_t id,
    std::uint64_t lifecycle_generation
  ) {
    auto launch = std::make_shared<rtsp_stream::launch_session_t>();
    launch->id = id;
    launch->lifecycle_generation = lifecycle_generation;
    launch->gcm_key.resize(16);
    launch->iv.resize(16);
    launch->device_name = "runtime-test-client";
    launch->unique_id = "runtime-test-client-" + std::to_string(id) +
                        "-" + std::to_string(lifecycle_generation);
    launch->session_token = "runtime-test-token";
    launch->perm = crypto::PERM::_game_control;
    launch->watch_only = false;
    return launch;
  }

  std::shared_ptr<stream::session_t> runtime_stream(
    rtsp_stream::launch_session_t &launch
  ) {
    stream::config_t config {};
    return stream::session::alloc(config, launch);
  }

  moonlight_runtime_create_result_t ready_runtime(
    const std::shared_ptr<runtime_factory_state_t> &factory
  ) {
    return moonlight_session_runtime_t::create(
      {.enabled = true},
      runtime_backend_factory(factory)
    );
  }

  void admit(
    moonlight_session_runtime_t &runtime,
    const expectation_t &expectation
  ) {
    ASSERT_TRUE(runtime.reconcile_inputs({}).report.admission_ready);
    ASSERT_TRUE(runtime.prepare_input(expectation).input.prepared());
  }

  TEST(MultiseatMoonlightRuntime, DisabledSkipsConstructionAndInstallation) {
    ASSERT_FALSE(moonlight_session_runtime_installed());
    ASSERT_FALSE(moonlight_session_activation_gate_installed());

    std::size_t factory_calls = 0;
    auto created = moonlight_session_runtime_t::create(
      {},
      [&factory_calls](moonlight_controller_feedback_sink_t) {
        ++factory_calls;
        return std::make_unique<runtime_backend_t>();
      }
    );
    EXPECT_EQ(
      created.status,
      moonlight_runtime_create_status_e::ready_disabled
    );
    EXPECT_FALSE(created.runtime);
    EXPECT_EQ(factory_calls, 0U);
    EXPECT_FALSE(moonlight_session_runtime_installed());
    EXPECT_FALSE(moonlight_session_activation_gate_installed());

    auto production = create_production_moonlight_session_runtime({});
    EXPECT_EQ(
      production.status,
      moonlight_runtime_create_status_e::ready_disabled
    );
    EXPECT_FALSE(production.runtime);
    EXPECT_FALSE(moonlight_session_runtime_installed());
  }

  TEST(MultiseatMoonlightRuntime, BackendFailureLeavesOrdinaryPathUntouched) {
    auto unavailable = moonlight_session_runtime_t::create(
      {.enabled = true},
      [](moonlight_controller_feedback_sink_t) {
        return std::unique_ptr<backend_t> {};
      }
    );
    EXPECT_EQ(
      unavailable.status,
      moonlight_runtime_create_status_e::backend_unavailable
    );
    EXPECT_FALSE(unavailable.runtime);

    auto throwing = moonlight_session_runtime_t::create(
      {.enabled = true},
      [](moonlight_controller_feedback_sink_t)
        -> std::unique_ptr<backend_t> {
        throw std::runtime_error {"runtime backend unavailable"};
      }
    );
    EXPECT_EQ(
      throwing.status,
      moonlight_runtime_create_status_e::backend_unavailable
    );
    EXPECT_FALSE(throwing.runtime);
    EXPECT_FALSE(moonlight_session_runtime_installed());
    EXPECT_FALSE(moonlight_session_activation_gate_installed());

    auto launch = runtime_launch(700, 800);
    auto stream = runtime_stream(*launch);
    ASSERT_TRUE(stream);
    EXPECT_EQ(
      activate_registered_moonlight_session(*stream),
      moonlight_session_activation_status_e::not_installed
    );
  }

  TEST(MultiseatMoonlightRuntime, SelectionPrecedesStartAndNormalStopRetiresExactLaunch) {
    auto factory = std::make_shared<runtime_factory_state_t>();
    auto created = ready_runtime(factory);
    ASSERT_EQ(
      created.status,
      moonlight_runtime_create_status_e::ready_enabled
    );
    ASSERT_TRUE(created.runtime);
    ASSERT_TRUE(created.runtime->installed());
    const auto expectation = runtime_expectation(10);
    admit(*created.runtime, expectation);

    auto launch = runtime_launch(701, 801);
    std::weak_ptr<rtsp_stream::launch_session_t> exact_launch = launch;
    const auto selected = select_authenticated_moonlight_launch(
      launch,
      expectation.handle,
      true
    );
    ASSERT_EQ(selected, moonlight_launch_selection_status_e::registered);
    ASSERT_EQ(created.runtime->tracked_launches(), 1U);

    auto stream = runtime_stream(*launch);
    ASSERT_TRUE(stream);
    ASSERT_EQ(
      activate_registered_moonlight_session(*stream),
      moonlight_session_activation_status_e::bound
    );
    ASSERT_EQ(created.runtime->claimed_sessions(), 1U);

    stream::session::stop(*stream);
    EXPECT_TRUE(launch->is_cancelled());
    EXPECT_EQ(created.runtime->claimed_sessions(), 0U);
    EXPECT_EQ(created.runtime->tracked_launches(), 0U);
    EXPECT_EQ(created.runtime->retained_launches(), 0U);
    launch.reset();
    EXPECT_TRUE(exact_launch.expired());

    const auto report = created.runtime->shutdown();
    EXPECT_EQ(report.status, moonlight_coordinator_shutdown_status_e::closed);
    EXPECT_EQ(report.released_allocations, 1U);
    EXPECT_FALSE(moonlight_session_runtime_installed());
    EXPECT_FALSE(moonlight_session_activation_gate_installed());
  }

  TEST(MultiseatMoonlightRuntime, TimeoutCancelsAndRetiresRaisedSelection) {
    auto factory = std::make_shared<runtime_factory_state_t>();
    auto created = ready_runtime(factory);
    ASSERT_TRUE(created.runtime);
    const auto expectation = runtime_expectation(11);
    admit(*created.runtime, expectation);
    auto launch = runtime_launch(702, 802);
    ASSERT_EQ(
      created.runtime->select_authenticated_launch(
        launch,
        expectation.handle,
        false
      ),
      moonlight_launch_selection_status_e::registered
    );
    ASSERT_TRUE(rtsp_stream::launch_session_raise(launch));
    const auto timer_generation =
      rtsp_stream::launch_timer_generation_for_tests();

    EXPECT_TRUE(rtsp_stream::expire_pending_launch_for_tests(
      launch->id,
      timer_generation
    ));
    EXPECT_TRUE(launch->is_cancelled());
    EXPECT_EQ(created.runtime->tracked_launches(), 0U);
    EXPECT_EQ(created.runtime->retained_launches(), 0U);

    EXPECT_EQ(
      created.runtime->shutdown().status,
      moonlight_coordinator_shutdown_status_e::closed
    );
  }

  TEST(MultiseatMoonlightRuntime, RejectedRaiseRetiresOnlyExactSelection) {
    auto factory = std::make_shared<runtime_factory_state_t>();
    auto created = ready_runtime(factory);
    ASSERT_TRUE(created.runtime);
    const auto expectation = runtime_expectation(12);
    admit(*created.runtime, expectation);

    auto blocker = runtime_launch(703, 803);
    ASSERT_TRUE(rtsp_stream::launch_session_raise(blocker));
    auto selected = runtime_launch(704, 804);
    ASSERT_EQ(
      created.runtime->select_authenticated_launch(
        selected,
        expectation.handle,
        false
      ),
      moonlight_launch_selection_status_e::registered
    );
    auto lookalike = runtime_launch(704, 804);
    EXPECT_EQ(
      cancel_registered_moonlight_launch(lookalike),
      moonlight_runtime_lifecycle_status_e::not_selected
    );
    EXPECT_EQ(created.runtime->tracked_launches(), 1U);
    EXPECT_FALSE(selected->is_cancelled());

    EXPECT_FALSE(rtsp_stream::launch_session_raise(selected));
    EXPECT_TRUE(selected->is_cancelled());
    EXPECT_EQ(created.runtime->tracked_launches(), 0U);
    EXPECT_FALSE(blocker->is_cancelled());
    rtsp_stream::launch_session_finish(
      blocker->id,
      *blocker->lifecycle_generation
    );

    EXPECT_EQ(
      created.runtime->shutdown().status,
      moonlight_coordinator_shutdown_status_e::closed
    );
  }

  TEST(MultiseatMoonlightRuntime, ExplicitAbortAfterRtspReleaseUsesExactKey) {
    auto factory = std::make_shared<runtime_factory_state_t>();
    auto created = ready_runtime(factory);
    ASSERT_TRUE(created.runtime);
    const auto expectation = runtime_expectation(13);
    admit(*created.runtime, expectation);
    auto launch = runtime_launch(705, 805);
    ASSERT_EQ(
      created.runtime->select_authenticated_launch(
        launch,
        expectation.handle,
        false
      ),
      moonlight_launch_selection_status_e::registered
    );
    ASSERT_TRUE(rtsp_stream::launch_session_raise(launch));

    // Control-channel establishment releases handshake state without ending
    // the authenticated launch. A later stream abort must still find the same
    // coordinator-owned object by its exact lifecycle key.
    rtsp_stream::launch_session_clear(launch->id);
    EXPECT_FALSE(launch->is_cancelled());
    EXPECT_EQ(created.runtime->tracked_launches(), 1U);
    rtsp_stream::launch_session_finish(
      launch->id,
      *launch->lifecycle_generation
    );
    EXPECT_TRUE(launch->is_cancelled());
    EXPECT_EQ(created.runtime->tracked_launches(), 0U);

    EXPECT_EQ(
      created.runtime->shutdown().status,
      moonlight_coordinator_shutdown_status_e::closed
    );
  }

  TEST(MultiseatMoonlightRuntime, LastSelectedStopSweepsConservativeTombstones) {
    auto factory = std::make_shared<runtime_factory_state_t>();
    auto created = ready_runtime(factory);
    ASSERT_TRUE(created.runtime);
    ASSERT_TRUE(created.runtime->reconcile_inputs({}).report.admission_ready);
    const auto first_expectation = runtime_expectation(14, 0);
    const auto second_expectation = runtime_expectation(15, 1);
    ASSERT_TRUE(
      created.runtime->prepare_input(first_expectation).input.prepared()
    );
    ASSERT_TRUE(
      created.runtime->prepare_input(second_expectation).input.prepared()
    );
    auto first_launch = runtime_launch(706, 806);
    auto second_launch = runtime_launch(707, 807);
    ASSERT_EQ(
      created.runtime->select_authenticated_launch(
        first_launch,
        first_expectation.handle,
        false
      ),
      moonlight_launch_selection_status_e::registered
    );
    ASSERT_EQ(
      created.runtime->select_authenticated_launch(
        second_launch,
        second_expectation.handle,
        false
      ),
      moonlight_launch_selection_status_e::registered
    );
    auto first_stream = runtime_stream(*first_launch);
    auto second_stream = runtime_stream(*second_launch);
    ASSERT_EQ(
      activate_registered_moonlight_session(*first_stream),
      moonlight_session_activation_status_e::bound
    );
    ASSERT_EQ(
      activate_registered_moonlight_session(*second_stream),
      moonlight_session_activation_status_e::bound
    );
    ASSERT_EQ(created.runtime->claimed_sessions(), 2U);

    stream::session::stop(*first_stream);
    EXPECT_EQ(created.runtime->claimed_sessions(), 1U);
    EXPECT_EQ(created.runtime->tracked_launches(), 2U);
    EXPECT_EQ(created.runtime->active_launches(), 1U);
    stream::session::stop(*second_stream);
    EXPECT_EQ(created.runtime->claimed_sessions(), 0U);
    EXPECT_EQ(created.runtime->tracked_launches(), 0U);
    EXPECT_EQ(created.runtime->retained_launches(), 0U);

    EXPECT_EQ(
      created.runtime->shutdown().status,
      moonlight_coordinator_shutdown_status_e::closed
    );
  }

  TEST(MultiseatMoonlightRuntime, ShutdownRemovesLifecycleBeforeDependencies) {
    auto factory = std::make_shared<runtime_factory_state_t>();
    auto created = ready_runtime(factory);
    ASSERT_TRUE(created.runtime);
    const auto expectation = runtime_expectation(16);
    admit(*created.runtime, expectation);
    auto launch = runtime_launch(708, 808);
    ASSERT_EQ(
      created.runtime->select_authenticated_launch(
        launch,
        expectation.handle,
        false
      ),
      moonlight_launch_selection_status_e::registered
    );
    auto stream = runtime_stream(*launch);
    ASSERT_EQ(
      activate_registered_moonlight_session(*stream),
      moonlight_session_activation_status_e::bound
    );

    auto shutdown = std::async(std::launch::async, [&created]() {
      return created.runtime->shutdown();
    });
    bool uninstalled = false;
    for (auto attempt = 0; attempt < 200 && !uninstalled; ++attempt) {
      uninstalled = !moonlight_session_runtime_installed();
      if (!uninstalled) {
        std::this_thread::sleep_for(1ms);
      }
    }
    EXPECT_TRUE(uninstalled);
    EXPECT_EQ(
      select_authenticated_moonlight_launch(
        runtime_launch(709, 809),
        runtime_handle(17),
        false
      ),
      std::nullopt
    );
    EXPECT_EQ(shutdown.wait_for(20ms), std::future_status::timeout);
    stream::session::stop(*stream);
    const auto report = shutdown.get();
    EXPECT_EQ(report.status, moonlight_coordinator_shutdown_status_e::closed);
    EXPECT_EQ(report.released_allocations, 1U);
    EXPECT_TRUE(launch->is_cancelled());
    EXPECT_TRUE(created.runtime->closed());
    EXPECT_FALSE(moonlight_session_activation_gate_installed());
  }

  TEST(MultiseatMoonlightRuntime, FailedCleanupStaysClosedAndCanRetry) {
    auto factory = std::make_shared<runtime_factory_state_t>();
    auto created = ready_runtime(factory);
    ASSERT_TRUE(created.runtime);
    const auto expectation = runtime_expectation(18);
    admit(*created.runtime, expectation);
    ASSERT_NE(factory->backend, nullptr);
    factory->backend->reject_destroy = true;

    const auto failed = created.runtime->shutdown();
    EXPECT_EQ(
      failed.status,
      moonlight_coordinator_shutdown_status_e::input_cleanup_incomplete
    );
    EXPECT_TRUE(created.runtime->shutting_down());
    EXPECT_FALSE(created.runtime->closed());
    EXPECT_FALSE(created.runtime->installed());
    EXPECT_FALSE(moonlight_session_runtime_installed());
    EXPECT_TRUE(moonlight_session_activation_gate_installed());

    factory->backend->reject_destroy = false;
    const auto retried = created.runtime->shutdown();
    EXPECT_EQ(
      retried.status,
      moonlight_coordinator_shutdown_status_e::closed
    );
    EXPECT_TRUE(created.runtime->closed());
    EXPECT_FALSE(moonlight_session_activation_gate_installed());
  }
}  // namespace
