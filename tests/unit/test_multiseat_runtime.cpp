/**
 * @file tests/unit/test_multiseat_runtime.cpp
 * @brief Executable contract for two independent seats sharing one GPU.
 */
#include "src/multiseat_runtime.h"

#include <gtest/gtest.h>

#include <future>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
  using multiseat::admission_rejection_e;
  using multiseat::compositor_e;
  using multiseat::gpu_capacity_t;
  using multiseat::mutation_result_e;
  using multiseat::registry_t;
  using multiseat::seat_handle_t;
  using multiseat::seat_request_t;
  using multiseat::seat_snapshot_t;
  using multiseat::seat_state_e;

  constexpr auto gpu_id = "gpu-primary";
  constexpr auto controller_epoch = "controller-a1b2c3d4";
  constexpr auto render_node = "/dev/dri/renderD128";

  gpu_capacity_t shared_gpu(
    std::uint32_t max_seats = 2,
    std::uint32_t max_encoder_sessions = 2
  ) {
    return {
      .logical_gpu_id = gpu_id,
      .render_node = render_node,
      .max_seats = max_seats,
      .max_encoder_sessions = max_encoder_sessions,
    };
  }

  seat_request_t request_for(
    std::string client,
    std::string profile,
    std::string workload,
    compositor_e compositor = compositor_e::automatic,
    std::uint32_t encoder_sessions = 1
  ) {
    return {
      .client_key = std::move(client),
      .profile_key = std::move(profile),
      .workload_key = std::move(workload),
      .logical_gpu_id = gpu_id,
      .requested_compositor = compositor,
      .encoder_sessions = encoder_sessions,
    };
  }

  seat_snapshot_t admit_or_fail(registry_t &registry, const seat_request_t &request) {
    auto result = registry.admit(request);
    EXPECT_TRUE(result.accepted());
    if (!result.seat) {
      return {};
    }
    return *result.seat;
  }

  void mark_running(
    registry_t &registry,
    const seat_handle_t &handle,
    compositor_e selected,
    std::string reason
  ) {
    ASSERT_EQ(
      registry.bind_runtime(handle, selected, std::move(reason)),
      mutation_result_e::applied
    );
    ASSERT_EQ(registry.mark_starting(handle), mutation_result_e::applied);
    ASSERT_EQ(registry.mark_running(handle), mutation_result_e::applied);
  }

  std::vector<std::string> resource_names(const seat_snapshot_t &seat) {
    return {
      seat.resources.worker_name,
      seat.resources.runtime_namespace,
      seat.resources.wayland_socket,
      seat.resources.audio_sink,
      seat.resources.input_seat,
    };
  }
}  // namespace

TEST(MultiseatRuntime, TwoIndependentSeatsShareOneGpuWithDistinctResources) {
  registry_t registry {controller_epoch, {shared_gpu()}};
  const auto first = admit_or_fail(
    registry,
    request_for("client-alpha", "profile-alpha", "steam-game", compositor_e::gamescope)
  );
  const auto second = admit_or_fail(
    registry,
    request_for("client-beta", "profile-beta", "heroic-game")
  );

  ASSERT_TRUE(first.handle.valid());
  ASSERT_TRUE(second.handle.valid());
  EXPECT_EQ(first.handle.logical_gpu_id, second.handle.logical_gpu_id);
  EXPECT_EQ(first.render_node, second.render_node);
  EXPECT_NE(first.handle.slot, second.handle.slot);
  EXPECT_NE(first.handle.generation, second.handle.generation);
  EXPECT_NE(first.resources, second.resources);

  const auto first_names = resource_names(first);
  const auto second_names = resource_names(second);
  for (std::size_t index = 0; index < first_names.size(); ++index) {
    EXPECT_NE(first_names[index], second_names[index]);
    EXPECT_EQ(first_names[index].find("client-alpha"), std::string::npos);
    EXPECT_EQ(first_names[index].find("profile-alpha"), std::string::npos);
    EXPECT_EQ(second_names[index].find("client-beta"), std::string::npos);
    EXPECT_EQ(second_names[index].find("profile-beta"), std::string::npos);
  }

  mark_running(registry, first.handle, compositor_e::gamescope, "explicit Gamescope");
  mark_running(registry, second.handle, compositor_e::sway, "automatic multi-window route");

  const auto first_live = registry.snapshot(first.handle);
  const auto second_live = registry.snapshot(second.handle);
  ASSERT_TRUE(first_live);
  ASSERT_TRUE(second_live);
  EXPECT_EQ(first_live->state, seat_state_e::running);
  EXPECT_EQ(second_live->state, seat_state_e::running);
  EXPECT_EQ(first_live->selected_compositor, compositor_e::gamescope);
  EXPECT_EQ(second_live->selected_compositor, compositor_e::sway);

  const auto usage = registry.gpu_usage(gpu_id);
  ASSERT_TRUE(usage);
  EXPECT_EQ(usage->active_seats, 2U);
  EXPECT_EQ(usage->encoder_sessions, 2U);
}

TEST(MultiseatRuntime, StoppingOneSeatLeavesTheOtherSeatRunning) {
  registry_t registry {controller_epoch, {shared_gpu()}};
  const auto first = admit_or_fail(
    registry,
    request_for("client-alpha", "profile-alpha", "steam-game")
  );
  const auto second = admit_or_fail(
    registry,
    request_for("client-beta", "profile-beta", "lutris-game")
  );
  mark_running(registry, first.handle, compositor_e::gamescope, "automatic game route");
  mark_running(registry, second.handle, compositor_e::labwc, "automatic launcher route");

  ASSERT_EQ(registry.begin_stop(first.handle), mutation_result_e::applied);
  ASSERT_EQ(registry.release(first.handle), mutation_result_e::applied);

  EXPECT_FALSE(registry.snapshot(first.handle));
  const auto second_live = registry.snapshot(second.handle);
  ASSERT_TRUE(second_live);
  EXPECT_EQ(second_live->state, seat_state_e::running);
  EXPECT_EQ(second_live->workload_key, "lutris-game");

  const auto usage = registry.gpu_usage(gpu_id);
  ASSERT_TRUE(usage);
  EXPECT_EQ(usage->active_seats, 1U);
  EXPECT_EQ(usage->encoder_sessions, 1U);
}

TEST(MultiseatRuntime, StaleHandleCannotMutateAReusedSlot) {
  registry_t registry {controller_epoch, {shared_gpu(1, 1)}};
  const auto original = admit_or_fail(
    registry,
    request_for("client-old", "profile-old", "old-game")
  );
  ASSERT_EQ(registry.begin_stop(original.handle), mutation_result_e::applied);
  ASSERT_EQ(registry.release(original.handle), mutation_result_e::applied);

  const auto replacement = admit_or_fail(
    registry,
    request_for("client-new", "profile-new", "new-game")
  );
  ASSERT_EQ(replacement.handle.slot, original.handle.slot);
  ASSERT_NE(replacement.handle.generation, original.handle.generation);

  EXPECT_EQ(
    registry.begin_stop(original.handle),
    mutation_result_e::stale_generation
  );
  const auto replacement_live = registry.snapshot(replacement.handle);
  ASSERT_TRUE(replacement_live);
  EXPECT_EQ(replacement_live->state, seat_state_e::reserved);
  EXPECT_EQ(replacement_live->workload_key, "new-game");
}

TEST(MultiseatRuntime, ControllerEpochFencesAReplacementControlPlane) {
  registry_t original_registry {"controller-old", {shared_gpu(1, 1)}};
  const auto original = admit_or_fail(
    original_registry,
    request_for("client-old", "profile-old", "old-game")
  );

  registry_t replacement_registry {"controller-new", {shared_gpu(1, 1)}};
  const auto replacement = admit_or_fail(
    replacement_registry,
    request_for("client-new", "profile-new", "new-game")
  );

  ASSERT_EQ(original.handle.slot, replacement.handle.slot);
  ASSERT_EQ(original.handle.generation, replacement.handle.generation);
  ASSERT_NE(original.handle.controller_epoch, replacement.handle.controller_epoch);
  EXPECT_NE(original.resources, replacement.resources);
  EXPECT_EQ(
    replacement_registry.begin_stop(original.handle),
    mutation_result_e::stale_controller
  );
  const auto replacement_live = replacement_registry.snapshot(replacement.handle);
  ASSERT_TRUE(replacement_live);
  EXPECT_EQ(replacement_live->state, seat_state_e::reserved);
}

TEST(MultiseatRuntime, SeatAndEncoderBudgetsFailClosedIndependently) {
  registry_t seat_limited {controller_epoch, {shared_gpu(2, 4)}};
  EXPECT_TRUE(seat_limited.admit(
    request_for("client-a", "profile-a", "game-a")
  ).accepted());
  EXPECT_TRUE(seat_limited.admit(
    request_for("client-b", "profile-b", "game-b")
  ).accepted());
  EXPECT_EQ(
    seat_limited.admit(
      request_for("client-c", "profile-c", "game-c")
    ).rejection,
    admission_rejection_e::seat_capacity_reached
  );

  registry_t encoder_limited {controller_epoch, {shared_gpu(3, 2)}};
  EXPECT_TRUE(encoder_limited.admit(
    request_for("client-a", "profile-a", "game-a", compositor_e::automatic, 2)
  ).accepted());
  EXPECT_EQ(
    encoder_limited.admit(
      request_for("client-b", "profile-b", "game-b")
    ).rejection,
    admission_rejection_e::encoder_capacity_reached
  );
}

TEST(MultiseatRuntime, OneClientCannotOccupyTwoSeats) {
  registry_t registry {controller_epoch, {shared_gpu()}};
  EXPECT_TRUE(registry.admit(
    request_for("same-client", "profile-a", "game-a")
  ).accepted());
  EXPECT_EQ(
    registry.admit(
      request_for("same-client", "profile-b", "game-b")
    ).rejection,
    admission_rejection_e::client_already_active
  );
}

TEST(MultiseatRuntime, RuntimeSelectionIsConcreteAndExplicitChoicesDoNotFallback) {
  registry_t registry {controller_epoch, {shared_gpu()}};
  const auto explicit_gamescope = admit_or_fail(
    registry,
    request_for("client-a", "profile-a", "game-a", compositor_e::gamescope)
  );
  EXPECT_EQ(
    registry.bind_runtime(
      explicit_gamescope.handle,
      compositor_e::sway,
      "Gamescope unavailable"
    ),
    mutation_result_e::invalid_selection
  );
  EXPECT_EQ(
    registry.mark_starting(explicit_gamescope.handle),
    mutation_result_e::invalid_state
  );
  EXPECT_EQ(
    registry.bind_runtime(
      explicit_gamescope.handle,
      compositor_e::gamescope,
      "explicit Gamescope"
    ),
    mutation_result_e::applied
  );

  const auto automatic = admit_or_fail(
    registry,
    request_for("client-b", "profile-b", "game-b")
  );
  EXPECT_EQ(
    registry.bind_runtime(
      automatic.handle,
      compositor_e::automatic,
      "not a concrete route"
    ),
    mutation_result_e::invalid_selection
  );
  EXPECT_EQ(
    registry.bind_runtime(automatic.handle, compositor_e::labwc, {}),
    mutation_result_e::invalid_selection
  );
  EXPECT_EQ(
    registry.bind_runtime(
      automatic.handle,
      compositor_e::labwc,
      "automatic headless route"
    ),
    mutation_result_e::applied
  );
}

TEST(MultiseatRuntime, ConcurrentAdmissionNeverDoubleAllocatesASeat) {
  registry_t registry {controller_epoch, {shared_gpu(2, 2)}};
  std::promise<void> release;
  const auto ready = release.get_future().share();
  std::vector<std::future<multiseat::admission_result_t>> attempts;

  for (int index = 0; index < 8; ++index) {
    attempts.push_back(std::async(
      std::launch::async,
      [&registry, ready, index]() {
        ready.wait();
        return registry.admit(request_for(
          "client-" + std::to_string(index),
          "profile-" + std::to_string(index),
          "game-" + std::to_string(index)
        ));
      }
    ));
  }
  release.set_value();

  std::size_t accepted = 0;
  std::set<std::uint32_t> slots;
  std::set<std::uint64_t> generations;
  for (auto &attempt : attempts) {
    const auto result = attempt.get();
    if (!result.accepted()) {
      EXPECT_EQ(result.rejection, admission_rejection_e::seat_capacity_reached);
      continue;
    }
    ++accepted;
    slots.insert(result.seat->handle.slot);
    generations.insert(result.seat->handle.generation);
  }

  EXPECT_EQ(accepted, std::size_t {2});
  EXPECT_EQ(slots.size(), std::size_t {2});
  EXPECT_EQ(generations.size(), std::size_t {2});
}

TEST(MultiseatRuntime, InvalidGpuDefinitionsAndRequestsAreRejected) {
  EXPECT_THROW(
    registry_t(std::string {}, {shared_gpu()}),
    std::invalid_argument
  );
  EXPECT_THROW(
    registry_t("bad/epoch", {shared_gpu()}),
    std::invalid_argument
  );
  EXPECT_THROW(
    registry_t(controller_epoch, std::vector<gpu_capacity_t> {}),
    std::invalid_argument
  );
  EXPECT_THROW(
    registry_t(controller_epoch, {gpu_capacity_t {
      .logical_gpu_id = gpu_id,
      .render_node = render_node,
      .max_seats = 0,
      .max_encoder_sessions = 1,
    }}),
    std::invalid_argument
  );
  EXPECT_THROW(
    registry_t(controller_epoch, {
      shared_gpu(),
      shared_gpu(),
    }),
    std::invalid_argument
  );

  registry_t registry {controller_epoch, {shared_gpu()}};
  auto invalid = request_for("client", "profile", "game");
  invalid.workload_key.clear();
  EXPECT_EQ(
    registry.admit(invalid).rejection,
    admission_rejection_e::invalid_request
  );
  invalid = request_for("client", "profile", "game");
  invalid.logical_gpu_id = "missing-gpu";
  EXPECT_EQ(
    registry.admit(invalid).rejection,
    admission_rejection_e::unknown_gpu
  );
}
