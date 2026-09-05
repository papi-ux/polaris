/**
 * @file tests/unit/test_multiseat_worker_broker.cpp
 * @brief Executable worker-broker and restart-reconciliation contract.
 */
#include "src/multiseat_worker_broker.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <future>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
  using namespace std::chrono_literals;
  using multiseat::broker_start_result_e;
  using multiseat::broker_stop_result_e;
  using multiseat::compositor_e;
  using multiseat::gpu_capacity_t;
  using multiseat::mutation_result_e;
  using multiseat::registry_t;
  using multiseat::runtime_profile_e;
  using multiseat::seat_request_t;
  using multiseat::seat_snapshot_t;
  using multiseat::seat_state_e;
  using multiseat::worker_backend_t;
  using multiseat::worker_broker_options_t;
  using multiseat::worker_broker_t;
  using multiseat::worker_command_result_e;
  using multiseat::worker_identity_t;
  using multiseat::worker_launch_spec_t;
  using multiseat::worker_observation_t;
  using multiseat::worker_observed_state_e;
  using multiseat::worker_stop_mode_e;
  using multiseat::worker_runtime_component_count;
  using multiseat::worker_runtime_component_stop_timeout;
  using multiseat::worker_runtime_graceful_stop_margin;
  using multiseat::worker_runtime_graceful_stop_timeout;

  constexpr auto gpu_id = "gpu-primary";
  constexpr auto render_node = "/dev/dri/renderD128";

  gpu_capacity_t shared_gpu() {
    return {
      .logical_gpu_id = gpu_id,
      .render_node = render_node,
      .max_seats = 2,
      .max_encoder_sessions = 2,
    };
  }

  worker_identity_t identity_for(const seat_snapshot_t &seat) {
    return {
      .seat = seat.handle,
      .worker_name = seat.resources.worker_name,
    };
  }

  seat_snapshot_t admit_and_bind(
    registry_t &registry,
    std::string client,
    std::string profile,
    std::string workload,
    compositor_e requested,
    compositor_e selected
  ) {
    auto admitted = registry.admit({
      .client_key = std::move(client),
      .profile_key = std::move(profile),
      .workload_key = std::move(workload),
      .logical_gpu_id = gpu_id,
      .runtime_profile = runtime_profile_e::steam,
      .display_mode = {1920, 1080, 60000, false},
      .requested_compositor = requested,
      .encoder_sessions = 1,
    });
    EXPECT_TRUE(admitted.accepted());
    if (!admitted.seat) {
      return {};
    }
    EXPECT_EQ(
      registry.bind_runtime(
        admitted.seat->handle,
        selected,
        requested == compositor_e::automatic ? "automatic test route" : "explicit test route"
      ),
      mutation_result_e::applied
    );
    const auto bound = registry.snapshot(admitted.seat->handle);
    EXPECT_TRUE(bound.has_value());
    return bound.value_or(seat_snapshot_t {});
  }

  class fake_clock_t {
  public:
    worker_broker_t::time_point_t now() const {
      return now_;
    }

    void advance(std::chrono::milliseconds amount) {
      now_ += amount;
    }

  private:
    worker_broker_t::time_point_t now_ {};
  };

  class fake_worker_backend_t final : public worker_backend_t {
  public:
    struct stop_call_t {
      worker_identity_t identity;
      worker_stop_mode_e mode;
    };

    worker_command_result_e launch(const worker_launch_spec_t &spec) override {
      launch_specs_.push_back(spec);
      if (next_launch_result_ != worker_command_result_e::applied) {
        const auto result = next_launch_result_;
        const bool materialize = materialize_next_launch_;
        next_launch_result_ = worker_command_result_e::applied;
        materialize_next_launch_ = false;
        if (materialize) {
          workers_.push_back({
            .spec = spec,
            .state = worker_observed_state_e::starting,
          });
        }
        return result;
      }

      const auto existing = find(spec.identity);
      if (existing != workers_.end()) {
        return existing->spec == spec ?
                 worker_command_result_e::already_applied :
                 worker_command_result_e::rejected;
      }
      workers_.push_back({
        .spec = spec,
        .state = worker_observed_state_e::starting,
      });
      return worker_command_result_e::applied;
    }

    worker_command_result_e stop(
      const worker_identity_t &identity,
      worker_stop_mode_e mode
    ) override {
      stop_calls_.push_back({
        .identity = identity,
        .mode = mode,
      });
      const auto worker = find(identity);
      if (worker == workers_.end()) {
        return worker_command_result_e::not_found;
      }
      if (mode == worker_stop_mode_e::force) {
        if (force_lingers_) {
          worker->state = worker_observed_state_e::stopping;
        } else {
          workers_.erase(worker);
        }
        return worker_command_result_e::applied;
      }
      if (worker->state == worker_observed_state_e::stopping) {
        return worker_command_result_e::already_applied;
      }
      worker->state = worker_observed_state_e::stopping;
      return worker_command_result_e::applied;
    }

    std::vector<worker_observation_t> inventory() override {
      if (fail_inventory_) {
        throw std::runtime_error {"fake inventory failure"};
      }
      auto result = injected_observations_;
      result.reserve(result.size() + workers_.size());
      for (const auto &worker : workers_) {
        result.push_back({
          .identity = worker.spec.identity,
          .state = worker.state,
        });
      }
      return result;
    }

    void set_next_launch_result(worker_command_result_e result, bool materialize = false) {
      next_launch_result_ = result;
      materialize_next_launch_ = materialize;
    }

    bool mark_ready(const worker_identity_t &identity) {
      const auto worker = find(identity);
      if (worker == workers_.end()) {
        return false;
      }
      worker->state = worker_observed_state_e::ready;
      return true;
    }

    bool complete_stop(const worker_identity_t &identity) {
      const auto worker = find(identity);
      if (worker == workers_.end()) {
        return false;
      }
      workers_.erase(worker);
      return true;
    }

    bool drop(const worker_identity_t &identity) {
      return complete_stop(identity);
    }

    [[nodiscard]] std::optional<worker_observed_state_e> state(
      const worker_identity_t &identity
    ) const {
      const auto worker = find(identity);
      if (worker == workers_.end()) {
        return std::nullopt;
      }
      return worker->state;
    }

    [[nodiscard]] std::size_t worker_count() const {
      return workers_.size();
    }

    [[nodiscard]] const std::vector<worker_launch_spec_t> &launch_specs() const {
      return launch_specs_;
    }

    [[nodiscard]] std::size_t stop_count(
      const worker_identity_t &identity,
      worker_stop_mode_e mode
    ) const {
      return static_cast<std::size_t>(std::count_if(
        stop_calls_.begin(),
        stop_calls_.end(),
        [&identity, mode](const auto &call) {
          return call.identity == identity && call.mode == mode;
        }
      ));
    }

    void fail_inventory(bool fail) {
      fail_inventory_ = fail;
    }

    void linger_after_force(bool linger) {
      force_lingers_ = linger;
    }

    void prepend_observation(worker_observation_t observation) {
      injected_observations_.push_back(std::move(observation));
    }

  private:
    struct worker_t {
      worker_launch_spec_t spec;
      worker_observed_state_e state;
    };

    using iterator = std::vector<worker_t>::iterator;
    using const_iterator = std::vector<worker_t>::const_iterator;

    iterator find(const worker_identity_t &identity) {
      return std::find_if(
        workers_.begin(),
        workers_.end(),
        [&identity](const auto &worker) {
          return worker.spec.identity == identity;
        }
      );
    }

    const_iterator find(const worker_identity_t &identity) const {
      return std::find_if(
        workers_.begin(),
        workers_.end(),
        [&identity](const auto &worker) {
          return worker.spec.identity == identity;
        }
      );
    }

    std::vector<worker_t> workers_;
    std::vector<worker_observation_t> injected_observations_;
    std::vector<worker_launch_spec_t> launch_specs_;
    std::vector<stop_call_t> stop_calls_;
    worker_command_result_e next_launch_result_ = worker_command_result_e::applied;
    bool materialize_next_launch_ = false;
    bool fail_inventory_ = false;
    bool force_lingers_ = false;
  };

  worker_broker_t broker_for(
    registry_t &registry,
    fake_worker_backend_t &backend,
    fake_clock_t &clock
  ) {
    return worker_broker_t {
      registry,
      backend,
      {
        .graceful_stop_timeout = 5s,
        .force_stop_timeout = 2s,
      },
      [&clock]() {
        return clock.now();
      },
    };
  }
}  // namespace

TEST(MultiseatWorkerBroker, DefaultGracefulStopBudgetCoversWorkerReverseTeardown) {
  const worker_broker_options_t defaults;
  EXPECT_EQ(worker_runtime_component_count, std::size_t {7});
  EXPECT_EQ(worker_runtime_component_stop_timeout, 5s);
  EXPECT_EQ(worker_runtime_graceful_stop_margin, 10s);
  EXPECT_EQ(worker_runtime_graceful_stop_timeout, 45s);
  EXPECT_EQ(defaults.graceful_stop_timeout, worker_runtime_graceful_stop_timeout);
}

TEST(MultiseatWorkerBroker, DefaultDeadlineDoesNotForceBeforeWorkerBudgetExpires) {
  registry_t registry {"controller-current", {shared_gpu()}};
  fake_worker_backend_t backend;
  fake_clock_t clock;
  worker_broker_t broker {
    registry,
    backend,
    {},
    [&clock]() {
      return clock.now();
    },
  };
  ASSERT_TRUE(broker.reconcile().admission_ready);
  const auto seat = admit_and_bind(
    registry,
    "client-a",
    "profile-a",
    "workload-a",
    compositor_e::gamescope,
    compositor_e::gamescope
  );
  const auto identity = identity_for(seat);
  ASSERT_EQ(broker.start_seat(seat.handle), broker_start_result_e::started);
  ASSERT_EQ(broker.stop_seat(seat.handle), broker_stop_result_e::stop_requested);

  clock.advance(worker_runtime_graceful_stop_timeout - 1ms);
  const auto before_deadline = broker.reconcile();
  EXPECT_EQ(before_deadline.force_stop_requests, std::size_t {0});
  EXPECT_EQ(backend.stop_count(identity, worker_stop_mode_e::force), std::size_t {0});

  clock.advance(1ms);
  const auto at_deadline = broker.reconcile();
  EXPECT_EQ(at_deadline.force_stop_requests, std::size_t {1});
  EXPECT_EQ(backend.stop_count(identity, worker_stop_mode_e::force), std::size_t {1});
}

TEST(MultiseatWorkerBroker, RequiresAuthoritativeInventoryBeforeLaunch) {
  registry_t registry {"controller-current", {shared_gpu()}};
  fake_worker_backend_t backend;
  fake_clock_t clock;
  auto broker = broker_for(registry, backend, clock);
  const auto seat = admit_and_bind(
    registry,
    "client-a",
    "profile-a",
    "workload-a",
    compositor_e::gamescope,
    compositor_e::gamescope
  );

  EXPECT_FALSE(broker.admission_ready());
  EXPECT_EQ(
    broker.start_seat(seat.handle),
    broker_start_result_e::reconciliation_required
  );
  const auto report = broker.reconcile();
  EXPECT_TRUE(report.inventory_authoritative);
  EXPECT_TRUE(report.active_workers.empty());
  EXPECT_TRUE(report.admission_ready);
  EXPECT_TRUE(broker.admission_ready());
  EXPECT_EQ(broker.start_seat(seat.handle), broker_start_result_e::started);
}

TEST(MultiseatWorkerBroker, EndpointReadinessGatesRunningAndShutdownPrecedesBackendStop) {
  registry_t registry {"controller-current", {shared_gpu()}};
  fake_worker_backend_t backend;
  fake_clock_t clock;
  std::size_t readiness_checks = 0;
  std::size_t shutdown_requests = 0;
  worker_broker_t broker {
    registry,
    backend,
    {
      .graceful_stop_timeout = 5s,
      .force_stop_timeout = 2s,
    },
    [&clock]() {
      return clock.now();
    },
    [&readiness_checks](const worker_identity_t &) {
      ++readiness_checks;
      return false;
    },
    [&shutdown_requests](const worker_identity_t &) {
      ++shutdown_requests;
    },
  };
  ASSERT_TRUE(broker.reconcile().admission_ready);
  const auto seat = admit_and_bind(
    registry,
    "client-a",
    "profile-a",
    "workload-a",
    compositor_e::gamescope,
    compositor_e::gamescope
  );
  const auto identity = identity_for(seat);
  ASSERT_EQ(broker.start_seat(seat.handle), broker_start_result_e::started);
  ASSERT_TRUE(backend.mark_ready(identity));

  const auto report = broker.reconcile();
  EXPECT_TRUE(report.inventory_authoritative);
  EXPECT_EQ(report.active_workers, std::vector<worker_identity_t> {identity});
  EXPECT_EQ(report.readiness_rejections, std::size_t {1});
  EXPECT_EQ(readiness_checks, std::size_t {1});
  EXPECT_EQ(shutdown_requests, std::size_t {1});
  EXPECT_EQ(
    backend.stop_count(identity, worker_stop_mode_e::graceful),
    std::size_t {1}
  );
  const auto stopping = registry.snapshot(seat.handle);
  ASSERT_TRUE(stopping);
  EXPECT_EQ(stopping->state, seat_state_e::stopping);
}

TEST(MultiseatWorkerBroker, TwoFakeWorkersRunAndStopIndependently) {
  registry_t registry {"controller-current", {shared_gpu()}};
  fake_worker_backend_t backend;
  fake_clock_t clock;
  auto broker = broker_for(registry, backend, clock);
  ASSERT_TRUE(broker.reconcile().admission_ready);

  const auto first = admit_and_bind(
    registry,
    "client-a",
    "profile-a",
    "steam-game",
    compositor_e::gamescope,
    compositor_e::gamescope
  );
  const auto second = admit_and_bind(
    registry,
    "client-b",
    "profile-b",
    "heroic-game",
    compositor_e::automatic,
    compositor_e::sway
  );

  auto first_start = std::async(std::launch::async, [&broker, &first]() {
    return broker.start_seat(first.handle);
  });
  auto second_start = std::async(std::launch::async, [&broker, &second]() {
    return broker.start_seat(second.handle);
  });
  EXPECT_EQ(first_start.get(), broker_start_result_e::started);
  EXPECT_EQ(second_start.get(), broker_start_result_e::started);
  ASSERT_EQ(backend.worker_count(), std::size_t {2});

  const auto first_identity = identity_for(first);
  const auto second_identity = identity_for(second);
  EXPECT_NE(first_identity, second_identity);
  ASSERT_EQ(backend.launch_specs().size(), std::size_t {2});
  for (const auto &spec : backend.launch_specs()) {
    EXPECT_EQ(spec.render_node, render_node);
    EXPECT_EQ(spec.runtime_profile, runtime_profile_e::steam);
    EXPECT_EQ(spec.display_mode.width, 1920U);
    EXPECT_EQ(spec.display_mode.height, 1080U);
    EXPECT_EQ(spec.display_mode.refresh_millihz, 60000U);
    EXPECT_FALSE(spec.display_mode.hdr);
    EXPECT_EQ(spec.identity.worker_name.find("client-"), std::string::npos);
  }

  ASSERT_TRUE(backend.mark_ready(first_identity));
  ASSERT_TRUE(backend.mark_ready(second_identity));
  const auto ready = broker.reconcile();
  EXPECT_EQ(ready.current_workers, std::size_t {2});
  EXPECT_EQ(ready.ready_transitions, std::size_t {2});
  ASSERT_EQ(registry.snapshot(first.handle)->state, seat_state_e::running);
  ASSERT_EQ(registry.snapshot(second.handle)->state, seat_state_e::running);

  EXPECT_EQ(broker.stop_seat(first.handle), broker_stop_result_e::stop_requested);
  EXPECT_EQ(broker.stop_seat(first.handle), broker_stop_result_e::already_stopping);
  EXPECT_EQ(
    backend.stop_count(first_identity, worker_stop_mode_e::graceful),
    std::size_t {1}
  );
  ASSERT_TRUE(backend.complete_stop(first_identity));

  const auto stopped = broker.reconcile();
  EXPECT_EQ(stopped.released_seats, std::size_t {1});
  EXPECT_FALSE(registry.snapshot(first.handle));
  const auto second_live = registry.snapshot(second.handle);
  ASSERT_TRUE(second_live);
  EXPECT_EQ(second_live->state, seat_state_e::running);
  EXPECT_EQ(backend.state(second_identity), worker_observed_state_e::ready);
}

TEST(MultiseatWorkerBroker, RestartBlocksAdmissionUntilOldEpochWorkerIsGone) {
  fake_worker_backend_t backend;
  fake_clock_t clock;
  registry_t old_registry {"controller-old", {shared_gpu()}};
  auto old_broker = broker_for(old_registry, backend, clock);
  ASSERT_TRUE(old_broker.reconcile().admission_ready);
  const auto old_seat = admit_and_bind(
    old_registry,
    "client-old",
    "profile-old",
    "old-game",
    compositor_e::gamescope,
    compositor_e::gamescope
  );
  ASSERT_EQ(old_broker.start_seat(old_seat.handle), broker_start_result_e::started);
  const auto old_identity = identity_for(old_seat);
  ASSERT_TRUE(backend.mark_ready(old_identity));
  ASSERT_EQ(old_broker.reconcile().ready_transitions, std::size_t {1});

  registry_t replacement_registry {"controller-new", {shared_gpu()}};
  auto replacement_broker = broker_for(replacement_registry, backend, clock);
  const auto replacement = admit_and_bind(
    replacement_registry,
    "client-new",
    "profile-new",
    "new-game",
    compositor_e::automatic,
    compositor_e::labwc
  );
  EXPECT_EQ(
    replacement_broker.start_seat(replacement.handle),
    broker_start_result_e::reconciliation_required
  );

  const auto graceful = replacement_broker.reconcile();
  EXPECT_EQ(graceful.orphan_workers, std::size_t {1});
  EXPECT_EQ(graceful.graceful_stop_requests, std::size_t {1});
  EXPECT_FALSE(graceful.admission_ready);
  EXPECT_EQ(
    backend.stop_count(old_identity, worker_stop_mode_e::graceful),
    std::size_t {1}
  );

  clock.advance(5001ms);
  const auto forced = replacement_broker.reconcile();
  EXPECT_EQ(forced.force_stop_requests, std::size_t {1});
  EXPECT_FALSE(forced.admission_ready);
  EXPECT_EQ(
    backend.stop_count(old_identity, worker_stop_mode_e::force),
    std::size_t {1}
  );
  EXPECT_EQ(backend.worker_count(), std::size_t {0});

  const auto clean = replacement_broker.reconcile();
  EXPECT_TRUE(clean.admission_ready);
  EXPECT_EQ(
    replacement_broker.start_seat(replacement.handle),
    broker_start_result_e::started
  );
}

TEST(MultiseatWorkerBroker, RejectedLaunchReleasesItsSeatBudget) {
  registry_t registry {"controller-current", {shared_gpu()}};
  fake_worker_backend_t backend;
  fake_clock_t clock;
  auto broker = broker_for(registry, backend, clock);
  ASSERT_TRUE(broker.reconcile().admission_ready);
  const auto seat = admit_and_bind(
    registry,
    "client-a",
    "profile-a",
    "workload-a",
    compositor_e::automatic,
    compositor_e::labwc
  );
  backend.set_next_launch_result(worker_command_result_e::rejected);

  EXPECT_EQ(
    broker.start_seat(seat.handle),
    broker_start_result_e::backend_rejected
  );
  EXPECT_FALSE(registry.snapshot(seat.handle));
  const auto usage = registry.gpu_usage(gpu_id);
  ASSERT_TRUE(usage);
  EXPECT_EQ(usage->active_seats, 0U);
  EXPECT_EQ(usage->encoder_sessions, 0U);
}

TEST(MultiseatWorkerBroker, IndeterminateLaunchIsStoppedBeforeRelease) {
  registry_t registry {"controller-current", {shared_gpu()}};
  fake_worker_backend_t backend;
  fake_clock_t clock;
  auto broker = broker_for(registry, backend, clock);
  ASSERT_TRUE(broker.reconcile().admission_ready);
  const auto seat = admit_and_bind(
    registry,
    "client-a",
    "profile-a",
    "workload-a",
    compositor_e::automatic,
    compositor_e::labwc
  );
  const auto identity = identity_for(seat);
  backend.set_next_launch_result(worker_command_result_e::indeterminate, true);

  EXPECT_EQ(
    broker.start_seat(seat.handle),
    broker_start_result_e::backend_indeterminate
  );
  const auto stopping = registry.snapshot(seat.handle);
  ASSERT_TRUE(stopping);
  EXPECT_EQ(stopping->state, seat_state_e::stopping);
  EXPECT_EQ(backend.state(identity), worker_observed_state_e::stopping);
  EXPECT_EQ(
    backend.stop_count(identity, worker_stop_mode_e::graceful),
    std::size_t {1}
  );

  ASSERT_TRUE(backend.complete_stop(identity));
  const auto reconciled = broker.reconcile();
  EXPECT_EQ(reconciled.released_seats, std::size_t {1});
  EXPECT_FALSE(registry.snapshot(seat.handle));
}

TEST(MultiseatWorkerBroker, MissingWorkerReleasesOnlyItsExactSeat) {
  registry_t registry {"controller-current", {shared_gpu()}};
  fake_worker_backend_t backend;
  fake_clock_t clock;
  auto broker = broker_for(registry, backend, clock);
  ASSERT_TRUE(broker.reconcile().admission_ready);
  const auto first = admit_and_bind(
    registry,
    "client-a",
    "profile-a",
    "workload-a",
    compositor_e::automatic,
    compositor_e::labwc
  );
  const auto second = admit_and_bind(
    registry,
    "client-b",
    "profile-b",
    "workload-b",
    compositor_e::gamescope,
    compositor_e::gamescope
  );
  ASSERT_EQ(broker.start_seat(first.handle), broker_start_result_e::started);
  ASSERT_EQ(broker.start_seat(second.handle), broker_start_result_e::started);
  ASSERT_TRUE(backend.mark_ready(identity_for(first)));
  ASSERT_TRUE(backend.mark_ready(identity_for(second)));
  ASSERT_EQ(broker.reconcile().ready_transitions, std::size_t {2});

  ASSERT_TRUE(backend.drop(identity_for(first)));
  const auto report = broker.reconcile();
  EXPECT_EQ(report.missing_workers, std::size_t {1});
  EXPECT_EQ(report.released_seats, std::size_t {1});
  EXPECT_FALSE(registry.snapshot(first.handle));
  const auto second_live = registry.snapshot(second.handle);
  ASSERT_TRUE(second_live);
  EXPECT_EQ(second_live->state, seat_state_e::running);
}

TEST(MultiseatWorkerBroker, InventoryFailureClosesAdmissionWithoutMutation) {
  registry_t registry {"controller-current", {shared_gpu()}};
  fake_worker_backend_t backend;
  fake_clock_t clock;
  auto broker = broker_for(registry, backend, clock);
  const auto seat = admit_and_bind(
    registry,
    "client-a",
    "profile-a",
    "workload-a",
    compositor_e::automatic,
    compositor_e::labwc
  );
  backend.fail_inventory(true);

  const auto report = broker.reconcile();
  EXPECT_TRUE(report.backend_observation_failed);
  EXPECT_FALSE(report.inventory_authoritative);
  EXPECT_TRUE(report.active_workers.empty());
  EXPECT_FALSE(report.admission_ready);
  EXPECT_EQ(
    broker.start_seat(seat.handle),
    broker_start_result_e::reconciliation_required
  );
  const auto unchanged = registry.snapshot(seat.handle);
  ASSERT_TRUE(unchanged);
  EXPECT_EQ(unchanged->state, seat_state_e::reserved);
}

TEST(MultiseatWorkerBroker, DuplicateInventoryFailsClosedWithoutMutation) {
  registry_t registry {"controller-current", {shared_gpu()}};
  fake_worker_backend_t backend;
  fake_clock_t clock;
  auto broker = broker_for(registry, backend, clock);
  ASSERT_TRUE(broker.reconcile().admission_ready);
  const auto seat = admit_and_bind(
    registry,
    "client-a",
    "profile-a",
    "workload-a",
    compositor_e::automatic,
    compositor_e::labwc
  );
  const auto identity = identity_for(seat);
  ASSERT_EQ(broker.start_seat(seat.handle), broker_start_result_e::started);
  backend.prepend_observation({
    .identity = identity,
    .state = worker_observed_state_e::stopped,
  });

  const auto report = broker.reconcile();
  EXPECT_EQ(report.protocol_errors, std::size_t {1});
  EXPECT_FALSE(report.inventory_authoritative);
  EXPECT_TRUE(report.active_workers.empty());
  EXPECT_FALSE(report.admission_ready);
  EXPECT_FALSE(broker.admission_ready());
  const auto unchanged = registry.snapshot(seat.handle);
  ASSERT_TRUE(unchanged);
  EXPECT_EQ(unchanged->state, seat_state_e::starting);
  EXPECT_EQ(backend.worker_count(), std::size_t {1});
  EXPECT_EQ(
    backend.stop_count(identity, worker_stop_mode_e::graceful),
    std::size_t {0}
  );
}

TEST(MultiseatWorkerBroker, MalformedInventoryFailsClosedWithoutCommands) {
  registry_t registry {"controller-current", {shared_gpu()}};
  fake_worker_backend_t backend;
  fake_clock_t clock;
  auto broker = broker_for(registry, backend, clock);
  ASSERT_TRUE(broker.reconcile().admission_ready);
  backend.prepend_observation({
    .identity = {
      .seat = {},
      .worker_name = "malformed-worker",
    },
    .state = worker_observed_state_e::ready,
  });

  const auto report = broker.reconcile();
  EXPECT_EQ(report.protocol_errors, std::size_t {1});
  EXPECT_FALSE(report.inventory_authoritative);
  EXPECT_TRUE(report.active_workers.empty());
  EXPECT_FALSE(report.admission_ready);
  EXPECT_FALSE(broker.admission_ready());
  EXPECT_EQ(backend.worker_count(), std::size_t {0});
}

TEST(MultiseatWorkerBroker, StuckWorkerIsReportedWithoutRepeatingForce) {
  registry_t registry {"controller-current", {shared_gpu()}};
  fake_worker_backend_t backend;
  fake_clock_t clock;
  auto broker = broker_for(registry, backend, clock);
  ASSERT_TRUE(broker.reconcile().admission_ready);
  const auto seat = admit_and_bind(
    registry,
    "client-a",
    "profile-a",
    "workload-a",
    compositor_e::gamescope,
    compositor_e::gamescope
  );
  const auto identity = identity_for(seat);
  ASSERT_EQ(broker.start_seat(seat.handle), broker_start_result_e::started);
  ASSERT_TRUE(backend.mark_ready(identity));
  ASSERT_EQ(broker.reconcile().ready_transitions, std::size_t {1});
  backend.linger_after_force(true);
  ASSERT_EQ(broker.stop_seat(seat.handle), broker_stop_result_e::stop_requested);

  clock.advance(5001ms);
  const auto forced = broker.reconcile();
  EXPECT_EQ(forced.force_stop_requests, std::size_t {1});
  EXPECT_EQ(forced.stuck_workers, std::size_t {0});

  clock.advance(2001ms);
  const auto stuck = broker.reconcile();
  EXPECT_EQ(stuck.stuck_workers, std::size_t {1});
  EXPECT_EQ(stuck.force_stop_requests, std::size_t {0});
  EXPECT_EQ(
    backend.stop_count(identity, worker_stop_mode_e::force),
    std::size_t {1}
  );
  const auto still_stopping = registry.snapshot(seat.handle);
  ASSERT_TRUE(still_stopping);
  EXPECT_EQ(still_stopping->state, seat_state_e::stopping);
}

TEST(MultiseatWorkerBroker, RejectsNonPositiveStopTimeout) {
  registry_t registry {"controller-current", {shared_gpu()}};
  fake_worker_backend_t backend;

  EXPECT_THROW(
    worker_broker_t(
      registry,
      backend,
      {
        .graceful_stop_timeout = 0ms,
        .force_stop_timeout = 2s,
      }
    ),
    std::invalid_argument
  );
}
