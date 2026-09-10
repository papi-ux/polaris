#include <gtest/gtest.h>

#include "src/platform/linux/multiseat_worker_coordinator.h"

#ifdef __linux__

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fcntl.h>
#include <future>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <utility>
#include <vector>

namespace {
  using namespace multiseat;
  using namespace multiseat::worker_ipc;

  constexpr auto gpu_id = "gpu-primary";

  class temporary_root_t {
  public:
    temporary_root_t() {
      std::array<char, 52> pattern {};
      const std::string prefix = "/tmp/polaris-seat-coordinator-XXXXXX";
      std::copy(prefix.begin(), prefix.end(), pattern.begin());
      const auto *created = ::mkdtemp(pattern.data());
      if (!created) {
        throw std::runtime_error {"temporary coordinator root could not be created"};
      }
      path_ = created;
      if (::chmod(path_.c_str(), 0700) != 0) {
        throw std::runtime_error {"temporary coordinator root mode could not be set"};
      }
    }

    ~temporary_root_t() {
      std::error_code ignored;
      std::filesystem::remove_all(path_, ignored);
    }

    temporary_root_t(const temporary_root_t &) = delete;
    temporary_root_t &operator=(const temporary_root_t &) = delete;

    [[nodiscard]] const std::filesystem::path &path() const {
      return path_;
    }

  private:
    std::filesystem::path path_;
  };

  capability_factory_t deterministic_capability(std::uint8_t seed = 0x31) {
    return [seed](capability_t &capability) {
      for (std::size_t index = 0; index < capability.size(); ++index) {
        capability[index] = static_cast<std::uint8_t>(seed + index);
      }
      return true;
    };
  }

  gpu_capacity_t shared_gpu() {
    return {
      .logical_gpu_id = gpu_id,
      .render_node = "/dev/dri/renderD128",
      .max_seats = 2,
      .max_encoder_sessions = 2,
    };
  }

  provider_catalog_selection_t orphan_provider_selection() {
    return {
      .compositor = compositor_e::gamescope,
      .workload = {
        .kind = workload_kind_e::steam,
        .target_id = "orphan-test-workload",
      },
    };
  }

  seat_snapshot_t admit_and_bind(
    registry_t &registry,
    std::string client,
    std::string profile,
    std::string workload,
    compositor_e selected = compositor_e::gamescope
  ) {
    auto result = registry.admit({
      .client_key = std::move(client),
      .profile_key = std::move(profile),
      .workload = {workload_kind_e::steam, std::move(workload)},
      .logical_gpu_id = gpu_id,
      .runtime_profile = runtime_profile_e::steam,
      .data_plane = {
        .display_topology = display_topology_e::capture_host_with_nested_compositor,
        .media_pipeline = media_pipeline_e::worker_local_capture_encode,
      },
      .display_mode = {1920, 1080, 60000, false},
      .requested_compositor = compositor_e::automatic,
      .encoder_sessions = 1,
    });
    EXPECT_TRUE(result.accepted());
    if (!result.seat) {
      return {};
    }
    EXPECT_EQ(
      registry.bind_runtime(result.seat->handle, selected, "test route"),
      mutation_result_e::applied
    );
    return registry.snapshot(result.seat->handle).value_or(seat_snapshot_t {});
  }

  worker_identity_t identity_for(const seat_snapshot_t &seat) {
    return {
      .seat = seat.handle,
      .worker_name = seat.resources.worker_name,
    };
  }

  endpoint_identity_t endpoint_for(const worker_identity_t &identity) {
    return {
      .controller_epoch = identity.seat.controller_epoch,
      .logical_gpu_id = identity.seat.logical_gpu_id,
      .slot = identity.seat.slot,
      .generation = identity.seat.generation,
      .worker_name = identity.worker_name,
    };
  }

  struct session_state_t {
    mutable std::mutex mutex;
    std::set<std::string> fail_connect;
    std::set<std::string> fail_heartbeat;
    std::map<std::string, std::size_t> connections;
    std::map<std::string, std::size_t> heartbeats;
    std::map<std::string, std::size_t> shutdowns;
    std::vector<std::string> events;

    void add_event(std::string event) {
      std::scoped_lock lock {mutex};
      events.push_back(std::move(event));
    }

    [[nodiscard]] std::vector<std::string> event_snapshot() const {
      std::scoped_lock lock {mutex};
      return events;
    }
  };

  class fake_control_session_t final : public worker_control_session_t {
  public:
    explicit fake_control_session_t(std::shared_ptr<session_state_t> state) :
        state_(std::move(state)) {
    }

    transport_status_e connect(
      const authority_handle_t &authority,
      controller_client_options_t
    ) override {
      worker_name_ = authority.identity().worker_name;
      std::scoped_lock lock {state_->mutex};
      if (state_->fail_connect.contains(worker_name_)) {
        return transport_status_e::authentication_rejected;
      }
      ++state_->connections[worker_name_];
      state_->events.push_back("transport-connect:" + worker_name_);
      connected_ = true;
      return transport_status_e::applied;
    }

    transport_status_e heartbeat(channel_e) override {
      std::scoped_lock lock {state_->mutex};
      if (!connected_) {
        return transport_status_e::closed;
      }
      if (state_->fail_heartbeat.contains(worker_name_)) {
        connected_ = false;
        return transport_status_e::io_error;
      }
      ++state_->heartbeats[worker_name_];
      return transport_status_e::applied;
    }

    transport_status_e shutdown() override {
      std::scoped_lock lock {state_->mutex};
      if (!connected_) {
        return transport_status_e::closed;
      }
      ++state_->shutdowns[worker_name_];
      state_->events.push_back("transport-shutdown:" + worker_name_);
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
    std::shared_ptr<session_state_t> state_;
    std::string worker_name_;
    bool connected_ = false;
  };

  worker_control_session_factory_t session_factory(
    const std::shared_ptr<session_state_t> &state
  ) {
    return [state]() {
      return std::make_unique<fake_control_session_t>(state);
    };
  }

  class fake_worker_backend_t final : public worker_backend_t {
  public:
    explicit fake_worker_backend_t(std::shared_ptr<session_state_t> events = {}) :
        events_(std::move(events)) {
    }

    worker_command_result_e launch(const worker_launch_spec_t &spec) override {
      std::scoped_lock lock {mutex_};
      const auto result = std::exchange(
        next_launch_result_,
        worker_command_result_e::applied
      );
      if (result == worker_command_result_e::rejected ||
          result == worker_command_result_e::not_found) {
        return result;
      }
      workers_.push_back({
        .identity = spec.identity,
        .state = worker_observed_state_e::starting,
      });
      return result;
    }

    worker_command_result_e stop(
      const worker_identity_t &identity,
      worker_stop_mode_e mode
    ) override {
      std::scoped_lock lock {mutex_};
      stop_calls_.push_back({identity, mode});
      if (events_) {
        events_->add_event("backend-stop:" + identity.worker_name);
      }
      const auto worker = find_locked(identity);
      if (worker == workers_.end()) {
        return worker_command_result_e::not_found;
      }
      if (mode == worker_stop_mode_e::force) {
        workers_.erase(worker);
      } else {
        worker->state = worker_observed_state_e::stopping;
      }
      return worker_command_result_e::applied;
    }

    std::vector<worker_observation_t> inventory() override {
      std::scoped_lock lock {mutex_};
      if (fail_inventory_) {
        throw std::runtime_error {"fake inventory failed"};
      }
      return workers_;
    }

    void set_next_launch_result(worker_command_result_e result) {
      std::scoped_lock lock {mutex_};
      next_launch_result_ = result;
    }

    void inject(worker_identity_t identity, worker_observed_state_e state) {
      std::scoped_lock lock {mutex_};
      workers_.push_back({
        .identity = std::move(identity),
        .state = state,
      });
    }

    bool mark_ready(const worker_identity_t &identity) {
      std::scoped_lock lock {mutex_};
      const auto worker = find_locked(identity);
      if (worker == workers_.end()) {
        return false;
      }
      worker->state = worker_observed_state_e::ready;
      return true;
    }

    bool complete(const worker_identity_t &identity) {
      std::scoped_lock lock {mutex_};
      const auto worker = find_locked(identity);
      if (worker == workers_.end()) {
        return false;
      }
      workers_.erase(worker);
      return true;
    }

    void fail_inventory(bool fail) {
      std::scoped_lock lock {mutex_};
      fail_inventory_ = fail;
    }

    [[nodiscard]] std::optional<worker_observed_state_e> state(
      const worker_identity_t &identity
    ) const {
      std::scoped_lock lock {mutex_};
      const auto worker = find_locked(identity);
      return worker == workers_.end() ?
               std::nullopt :
               std::optional {worker->state};
    }

    [[nodiscard]] std::size_t worker_count() const {
      std::scoped_lock lock {mutex_};
      return workers_.size();
    }

  private:
    using iterator = std::vector<worker_observation_t>::iterator;
    using const_iterator = std::vector<worker_observation_t>::const_iterator;

    iterator find_locked(const worker_identity_t &identity) {
      return std::find_if(
        workers_.begin(),
        workers_.end(),
        [&identity](const auto &worker) {
          return worker.identity == identity;
        }
      );
    }

    const_iterator find_locked(const worker_identity_t &identity) const {
      return std::find_if(
        workers_.begin(),
        workers_.end(),
        [&identity](const auto &worker) {
          return worker.identity == identity;
        }
      );
    }

    mutable std::mutex mutex_;
    std::vector<worker_observation_t> workers_;
    std::vector<std::pair<worker_identity_t, worker_stop_mode_e>> stop_calls_;
    std::shared_ptr<session_state_t> events_;
    worker_command_result_e next_launch_result_ = worker_command_result_e::applied;
    bool fail_inventory_ = false;
  };

  worker_coordinator_options_t short_options() {
    return {
      .broker = {
        .graceful_stop_timeout = std::chrono::seconds {1},
        .force_stop_timeout = std::chrono::seconds {1},
      },
      .client = {
        .connect_timeout = std::chrono::milliseconds {100},
        .handshake_timeout = std::chrono::milliseconds {100},
        .io_timeout = std::chrono::milliseconds {100},
      },
    };
  }

  int bind_live_socket(const std::filesystem::path &path) {
    const auto descriptor = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (descriptor < 0) {
      return -1;
    }
    sockaddr_un address {};
    address.sun_family = AF_UNIX;
    const auto native = path.native();
    if (native.size() >= sizeof(address.sun_path)) {
      (void) ::close(descriptor);
      return -1;
    }
    std::copy(native.begin(), native.end(), address.sun_path);
    address.sun_path[native.size()] = '\0';
    if (::bind(
          descriptor,
          reinterpret_cast<const sockaddr *>(&address),
          static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + native.size() + 1)
        ) != 0 ||
        ::chmod(path.c_str(), 0600) != 0 ||
        ::listen(descriptor, 2) != 0) {
      (void) ::close(descriptor);
      return -1;
    }
    return descriptor;
  }
}

TEST(MultiseatWorkerCoordinator, OwnsAuthorityFromLaunchThroughAuthenticatedStopAndAbsence) {
  temporary_root_t root;
  authority_store_t store {root.path(), deterministic_capability()};
  registry_t registry {"controller-current", {shared_gpu()}};
  auto sessions = std::make_shared<session_state_t>();
  fake_worker_backend_t backend {sessions};
  worker_coordinator_t coordinator {
    registry,
    backend,
    store,
    short_options(),
    {},
    session_factory(sessions),
  };
  const auto startup = coordinator.reconcile();
  ASSERT_TRUE(startup.startup_recovery_complete);
  ASSERT_TRUE(startup.admission_ready);

  const auto seat = admit_and_bind(
    registry,
    "client-a",
    "profile-a",
    "steam-game"
  );
  const auto identity = identity_for(seat);
  ASSERT_EQ(
    coordinator.start_seat(seat.handle),
    coordinator_start_result_e::started
  );
  EXPECT_TRUE(std::filesystem::exists(
    root.path() /
    seat.resources.runtime_namespace /
    std::string {authority_auth_directory_name} /
    std::string {authority_record_file_name}
  ));
  const auto catalog_path = root.path() /
                            seat.resources.runtime_namespace /
                            std::string {authority_auth_directory_name} /
                            std::string {authority_provider_catalog_file_name};
  std::ifstream catalog_stream {catalog_path, std::ios::binary};
  const std::string catalog {
    std::istreambuf_iterator<char> {catalog_stream},
    std::istreambuf_iterator<char> {},
  };
  EXPECT_NE(catalog.find("\"selector\":\"gamescope\""), std::string::npos);
  EXPECT_NE(catalog.find("\"selector\":\"steam\""), std::string::npos);
  EXPECT_NE(catalog.find("\"target_id\":\"steam-game\""), std::string::npos);
  EXPECT_EQ(coordinator.managed_workers(), std::vector<worker_identity_t> {identity});

  ASSERT_TRUE(backend.mark_ready(identity));
  const auto ready = coordinator.reconcile();
  EXPECT_EQ(ready.broker.ready_transitions, std::size_t {1});
  EXPECT_EQ(ready.endpoint_connections, std::size_t {1});
  EXPECT_EQ(ready.endpoint_heartbeats, std::size_t {2});
  ASSERT_TRUE(registry.snapshot(seat.handle));
  EXPECT_EQ(registry.snapshot(seat.handle)->state, seat_state_e::running);

  const auto stopped = coordinator.stop_seat(seat.handle);
  EXPECT_EQ(stopped.broker, broker_stop_result_e::stop_requested);
  ASSERT_TRUE(stopped.transport);
  EXPECT_EQ(*stopped.transport, transport_status_e::applied);
  const auto events = sessions->event_snapshot();
  const auto transport = std::find(
    events.begin(),
    events.end(),
    "transport-shutdown:" + identity.worker_name
  );
  const auto backend_stop = std::find(
    events.begin(),
    events.end(),
    "backend-stop:" + identity.worker_name
  );
  ASSERT_NE(transport, events.end());
  ASSERT_NE(backend_stop, events.end());
  EXPECT_LT(transport, backend_stop);

  ASSERT_TRUE(backend.complete(identity));
  const auto released = coordinator.reconcile();
  EXPECT_EQ(released.broker.released_seats, std::size_t {1});
  EXPECT_EQ(released.removed_authorities, std::size_t {1});
  EXPECT_TRUE(coordinator.managed_workers().empty());
  EXPECT_TRUE(std::filesystem::is_empty(root.path()));
}

TEST(MultiseatWorkerCoordinator, EndpointAuthenticationFailureCannotBecomeRunning) {
  temporary_root_t root;
  authority_store_t store {root.path(), deterministic_capability()};
  registry_t registry {"controller-current", {shared_gpu()}};
  auto sessions = std::make_shared<session_state_t>();
  fake_worker_backend_t backend;
  worker_coordinator_t coordinator {
    registry, backend, store, short_options(), {}, session_factory(sessions)
  };
  ASSERT_TRUE(coordinator.reconcile().admission_ready);
  const auto seat = admit_and_bind(
    registry,
    "client-a",
    "profile-a",
    "heroic-game"
  );
  const auto identity = identity_for(seat);
  {
    std::scoped_lock lock {sessions->mutex};
    sessions->fail_connect.insert(identity.worker_name);
  }
  ASSERT_EQ(
    coordinator.start_seat(seat.handle),
    coordinator_start_result_e::started
  );
  ASSERT_TRUE(backend.mark_ready(identity));

  const auto report = coordinator.reconcile();
  EXPECT_EQ(report.broker.ready_transitions, std::size_t {0});
  EXPECT_EQ(report.broker.readiness_rejections, std::size_t {1});
  EXPECT_EQ(report.endpoint_failures, std::size_t {1});
  const auto stopping = registry.snapshot(seat.handle);
  ASSERT_TRUE(stopping);
  EXPECT_EQ(stopping->state, seat_state_e::stopping);
  EXPECT_EQ(backend.state(identity), worker_observed_state_e::stopping);

  ASSERT_TRUE(backend.complete(identity));
  EXPECT_EQ(coordinator.reconcile().removed_authorities, std::size_t {1});
}

TEST(MultiseatWorkerCoordinator, AuthoritativeStopAbsenceCleansAuthorityImmediately) {
  temporary_root_t root;
  authority_store_t store {root.path(), deterministic_capability()};
  registry_t registry {"controller-current", {shared_gpu()}};
  auto sessions = std::make_shared<session_state_t>();
  fake_worker_backend_t backend;
  worker_coordinator_t coordinator {
    registry, backend, store, short_options(), {}, session_factory(sessions)
  };
  ASSERT_TRUE(coordinator.reconcile().admission_ready);
  const auto seat = admit_and_bind(
    registry,
    "client-a",
    "profile-a",
    "disappearing-game"
  );
  const auto identity = identity_for(seat);
  ASSERT_EQ(
    coordinator.start_seat(seat.handle),
    coordinator_start_result_e::started
  );
  ASSERT_TRUE(backend.mark_ready(identity));
  ASSERT_EQ(coordinator.reconcile().broker.ready_transitions, std::size_t {1});
  ASSERT_TRUE(backend.complete(identity));

  const auto stopped = coordinator.stop_seat(seat.handle);
  EXPECT_EQ(stopped.broker, broker_stop_result_e::released);
  ASSERT_TRUE(stopped.transport);
  EXPECT_EQ(*stopped.transport, transport_status_e::applied);
  ASSERT_TRUE(stopped.cleanup);
  EXPECT_EQ(*stopped.cleanup, authority_status_e::applied);
  EXPECT_FALSE(registry.snapshot(seat.handle));
  EXPECT_TRUE(coordinator.managed_workers().empty());
  EXPECT_TRUE(std::filesystem::is_empty(root.path()));
}

TEST(MultiseatWorkerCoordinator, RestartRetainsActiveOrphanUntilInventoryProvesAbsence) {
  temporary_root_t root;
  fake_worker_backend_t backend;
  worker_identity_t orphan;
  std::string runtime_namespace;
  {
    authority_store_t original {root.path(), deterministic_capability(0x41)};
    registry_t old_registry {"controller-old", {shared_gpu()}};
    auto old_sessions = std::make_shared<session_state_t>();
    worker_coordinator_t old_coordinator {
      old_registry,
      backend,
      original,
      short_options(),
      {},
      session_factory(old_sessions),
    };
    ASSERT_TRUE(old_coordinator.reconcile().admission_ready);
    const auto old_seat = admit_and_bind(
      old_registry,
      "client-old",
      "profile-old",
      "old-game"
    );
    orphan = identity_for(old_seat);
    runtime_namespace = old_seat.resources.runtime_namespace;
    ASSERT_EQ(
      old_coordinator.start_seat(old_seat.handle),
      coordinator_start_result_e::started
    );
    ASSERT_TRUE(backend.mark_ready(orphan));
    ASSERT_EQ(
      old_coordinator.reconcile().broker.ready_transitions,
      std::size_t {1}
    );
  }
  registry_t replacement {"controller-new", {shared_gpu()}};
  authority_store_t recovered_store {root.path(), deterministic_capability(0x42)};
  auto sessions = std::make_shared<session_state_t>();
  worker_coordinator_t coordinator {
    replacement,
    backend,
    recovered_store,
    short_options(),
    {},
    session_factory(sessions),
  };

  const auto active = coordinator.reconcile();
  EXPECT_EQ(active.broker.orphan_workers, std::size_t {1});
  EXPECT_EQ(active.active_orphan_authorities, std::size_t {1});
  EXPECT_FALSE(active.startup_recovery_complete);
  EXPECT_FALSE(active.admission_ready);
  EXPECT_TRUE(std::filesystem::exists(root.path() / runtime_namespace));

  ASSERT_TRUE(backend.complete(orphan));
  const auto absent = coordinator.reconcile();
  EXPECT_EQ(absent.recovered_authorities, std::size_t {1});
  EXPECT_EQ(absent.removed_authorities, std::size_t {1});
  EXPECT_TRUE(absent.startup_recovery_complete);
  EXPECT_TRUE(absent.admission_ready);
  EXPECT_TRUE(std::filesystem::is_empty(root.path()));
}

TEST(MultiseatWorkerCoordinator, InventoryFailureNeverAuthorizesOrphanCleanup) {
  temporary_root_t root;
  const worker_identity_t orphan {
    .seat = {"controller-old", gpu_id, 0, 10},
    .worker_name = "polaris-worker-controller-old-10",
  };
  const auto runtime_namespace = "polaris-runtime-controller-old-10";
  {
    authority_store_t original {root.path(), deterministic_capability(0x51)};
    ASSERT_TRUE(original.create(
      endpoint_for(orphan),
      runtime_namespace,
      orphan_provider_selection()
    ).created());
  }
  fake_worker_backend_t backend;
  backend.fail_inventory(true);
  registry_t registry {"controller-new", {shared_gpu()}};
  authority_store_t replacement {root.path(), deterministic_capability(0x52)};
  auto sessions = std::make_shared<session_state_t>();
  worker_coordinator_t coordinator {
    registry, backend, replacement, short_options(), {}, session_factory(sessions)
  };

  const auto failed = coordinator.reconcile();
  EXPECT_TRUE(failed.broker.backend_observation_failed);
  EXPECT_FALSE(failed.startup_recovery_complete);
  EXPECT_TRUE(std::filesystem::exists(root.path() / runtime_namespace));

  backend.fail_inventory(false);
  const auto clean = coordinator.reconcile();
  EXPECT_TRUE(clean.startup_recovery_complete);
  EXPECT_TRUE(clean.admission_ready);
  EXPECT_FALSE(std::filesystem::exists(root.path() / runtime_namespace));
}

TEST(MultiseatWorkerCoordinator, TamperedOrphanRecordBlocksAdmissionWithoutDeletion) {
  temporary_root_t root;
  const worker_identity_t orphan {
    .seat = {"controller-old", gpu_id, 0, 11},
    .worker_name = "polaris-worker-controller-old-11",
  };
  const auto runtime_namespace = "polaris-runtime-controller-old-11";
  std::filesystem::path record;
  {
    authority_store_t original {root.path(), deterministic_capability(0x61)};
    auto created = original.create(
      endpoint_for(orphan),
      runtime_namespace,
      orphan_provider_selection()
    );
    ASSERT_TRUE(created.created());
    record = created.authority->paths().record;
  }
  {
    const auto descriptor = ::open(record.c_str(), O_WRONLY | O_CLOEXEC);
    ASSERT_GE(descriptor, 0);
    const char changed = 'X';
    ASSERT_EQ(::write(descriptor, &changed, 1), 1);
    ASSERT_EQ(::close(descriptor), 0);
  }
  fake_worker_backend_t backend;
  registry_t registry {"controller-new", {shared_gpu()}};
  authority_store_t replacement {root.path(), deterministic_capability(0x62)};
  auto sessions = std::make_shared<session_state_t>();
  worker_coordinator_t coordinator {
    registry, backend, replacement, short_options(), {}, session_factory(sessions)
  };

  const auto report = coordinator.reconcile();
  EXPECT_EQ(report.authority_status, authority_status_e::integrity_violation);
  EXPECT_TRUE(report.authority_blocked);
  EXPECT_FALSE(report.admission_ready);
  EXPECT_TRUE(std::filesystem::exists(root.path() / runtime_namespace));
}

TEST(MultiseatWorkerCoordinator, LiveSocketContradictingInventoryBlocksCleanup) {
  temporary_root_t root;
  const worker_identity_t orphan {
    .seat = {"controller-old", gpu_id, 0, 12},
    .worker_name = "polaris-worker-controller-old-12",
  };
  const auto runtime_namespace = "polaris-runtime-controller-old-12";
  std::filesystem::path socket_path;
  {
    authority_store_t original {root.path(), deterministic_capability(0x71)};
    auto created = original.create(
      endpoint_for(orphan),
      runtime_namespace,
      orphan_provider_selection()
    );
    ASSERT_TRUE(created.created());
    socket_path = created.authority->paths().control_socket;
  }
  const auto listener = bind_live_socket(socket_path);
  ASSERT_GE(listener, 0);
  fake_worker_backend_t backend;
  registry_t registry {"controller-new", {shared_gpu()}};
  authority_store_t replacement {root.path(), deterministic_capability(0x72)};
  auto sessions = std::make_shared<session_state_t>();
  worker_coordinator_t coordinator {
    registry, backend, replacement, short_options(), {}, session_factory(sessions)
  };

  const auto blocked = coordinator.reconcile();
  EXPECT_EQ(blocked.authority_failures, std::size_t {1});
  EXPECT_TRUE(blocked.authority_blocked);
  EXPECT_FALSE(blocked.admission_ready);
  EXPECT_TRUE(std::filesystem::exists(root.path() / runtime_namespace));
  ASSERT_EQ(::close(listener), 0);

  const auto cleaned = coordinator.reconcile();
  EXPECT_TRUE(cleaned.startup_recovery_complete);
  EXPECT_TRUE(cleaned.admission_ready);
  EXPECT_TRUE(std::filesystem::is_empty(root.path()));
}

TEST(MultiseatWorkerCoordinator, AuthoritativeReconcileAuditsRootWhileWorkersAreActive) {
  temporary_root_t root;
  authority_store_t store {root.path(), deterministic_capability()};
  registry_t registry {"controller-current", {shared_gpu()}};
  auto sessions = std::make_shared<session_state_t>();
  fake_worker_backend_t backend;
  worker_coordinator_t coordinator {
    registry, backend, store, short_options(), {}, session_factory(sessions)
  };
  ASSERT_TRUE(coordinator.reconcile().admission_ready);
  const auto seat = admit_and_bind(
    registry,
    "client-a",
    "profile-a",
    "active-game"
  );
  ASSERT_EQ(
    coordinator.start_seat(seat.handle),
    coordinator_start_result_e::started
  );
  {
    std::ofstream unexpected {root.path() / "unexpected-root-entry"};
    unexpected << "do not delete";
  }

  const auto report = coordinator.reconcile();
  EXPECT_EQ(report.authority_status, authority_status_e::integrity_violation);
  EXPECT_TRUE(report.authority_blocked);
  EXPECT_FALSE(report.startup_recovery_complete);
  EXPECT_FALSE(report.admission_ready);
  EXPECT_TRUE(std::filesystem::exists(root.path() / "unexpected-root-entry"));
  EXPECT_TRUE(registry.snapshot(seat.handle));
}

TEST(MultiseatWorkerCoordinator, TwoSeatsRemainIndependentAcrossStopAndHeartbeatFailure) {
  temporary_root_t root;
  authority_store_t store {root.path(), deterministic_capability()};
  registry_t registry {"controller-current", {shared_gpu()}};
  auto sessions = std::make_shared<session_state_t>();
  fake_worker_backend_t backend;
  worker_coordinator_t coordinator {
    registry, backend, store, short_options(), {}, session_factory(sessions)
  };
  ASSERT_TRUE(coordinator.reconcile().admission_ready);
  const auto first = admit_and_bind(
    registry,
    "client-a",
    "profile-a",
    "steam-game"
  );
  const auto second = admit_and_bind(
    registry,
    "client-b",
    "profile-b",
    "lutris-game"
  );
  const auto first_identity = identity_for(first);
  const auto second_identity = identity_for(second);
  auto first_start = std::async(std::launch::async, [&]() {
    return coordinator.start_seat(first.handle);
  });
  auto second_start = std::async(std::launch::async, [&]() {
    return coordinator.start_seat(second.handle);
  });
  EXPECT_EQ(first_start.get(), coordinator_start_result_e::started);
  EXPECT_EQ(second_start.get(), coordinator_start_result_e::started);
  ASSERT_TRUE(backend.mark_ready(first_identity));
  ASSERT_TRUE(backend.mark_ready(second_identity));
  EXPECT_EQ(coordinator.reconcile().broker.ready_transitions, std::size_t {2});

  {
    std::scoped_lock lock {sessions->mutex};
    sessions->fail_heartbeat.insert(first_identity.worker_name);
  }
  EXPECT_EQ(
    coordinator.heartbeat(first.handle, channel_e::control),
    transport_status_e::io_error
  );
  ASSERT_TRUE(registry.snapshot(first.handle));
  EXPECT_EQ(registry.snapshot(first.handle)->state, seat_state_e::stopping);
  ASSERT_TRUE(registry.snapshot(second.handle));
  EXPECT_EQ(registry.snapshot(second.handle)->state, seat_state_e::running);
  EXPECT_EQ(
    coordinator.heartbeat(second.handle, channel_e::media),
    transport_status_e::applied
  );

  ASSERT_TRUE(backend.complete(first_identity));
  const auto after_first = coordinator.reconcile();
  EXPECT_EQ(after_first.removed_authorities, std::size_t {1});
  EXPECT_FALSE(registry.snapshot(first.handle));
  EXPECT_TRUE(registry.snapshot(second.handle));
  EXPECT_EQ(backend.worker_count(), std::size_t {1});
}

TEST(MultiseatWorkerCoordinator, RejectedLaunchRollsBackAuthorityAndSeatBudget) {
  temporary_root_t root;
  authority_store_t store {root.path(), deterministic_capability()};
  registry_t registry {"controller-current", {shared_gpu()}};
  auto sessions = std::make_shared<session_state_t>();
  fake_worker_backend_t backend;
  worker_coordinator_t coordinator {
    registry, backend, store, short_options(), {}, session_factory(sessions)
  };
  ASSERT_TRUE(coordinator.reconcile().admission_ready);
  const auto seat = admit_and_bind(
    registry,
    "client-a",
    "profile-a",
    "rejected-game"
  );
  backend.set_next_launch_result(worker_command_result_e::rejected);

  EXPECT_EQ(
    coordinator.start_seat(seat.handle),
    coordinator_start_result_e::backend_rejected
  );
  EXPECT_FALSE(registry.snapshot(seat.handle));
  EXPECT_TRUE(coordinator.managed_workers().empty());
  EXPECT_TRUE(std::filesystem::is_empty(root.path()));
  const auto usage = registry.gpu_usage(gpu_id);
  ASSERT_TRUE(usage);
  EXPECT_EQ(usage->active_seats, 0U);
  EXPECT_EQ(usage->encoder_sessions, 0U);
}

TEST(MultiseatWorkerCoordinator, AuthorizesOnlyExactRunningAuthenticatedSeat) {
  temporary_root_t root;
  authority_store_t store {root.path(), deterministic_capability()};
  registry_t registry {"controller-current", {shared_gpu()}};
  auto sessions = std::make_shared<session_state_t>();
  fake_worker_backend_t backend;
  worker_coordinator_t coordinator {
    registry, backend, store, short_options(), {}, session_factory(sessions)
  };
  ASSERT_TRUE(coordinator.reconcile().admission_ready);
  const auto seat = admit_and_bind(
    registry,
    "paired-client",
    "profile-authorized",
    "authorized-game"
  );
  authenticated_worker_seat_t observed;
  auto capture = [&observed](const authenticated_worker_seat_t &authorized) {
    observed = authorized;
  };

  EXPECT_EQ(
    coordinator.with_authenticated_worker_seat({}, capture),
    worker_seat_authorization_status_e::invalid_request
  );
  EXPECT_EQ(
    coordinator.with_authenticated_worker_seat(seat.handle, capture),
    worker_seat_authorization_status_e::seat_not_running
  );
  ASSERT_EQ(
    coordinator.start_seat(seat.handle),
    coordinator_start_result_e::started
  );
  EXPECT_EQ(
    coordinator.with_authenticated_worker_seat(seat.handle, capture),
    worker_seat_authorization_status_e::seat_not_running
  );
  const auto identity = identity_for(seat);
  ASSERT_TRUE(backend.mark_ready(identity));
  ASSERT_EQ(coordinator.reconcile().broker.ready_transitions, std::size_t {1});

  EXPECT_EQ(
    coordinator.with_authenticated_worker_seat(seat.handle, capture),
    worker_seat_authorization_status_e::applied
  );
  EXPECT_EQ(observed.handle, seat.handle);
  EXPECT_EQ(observed.worker_name, seat.resources.worker_name);
  EXPECT_EQ(observed.input_seat, seat.resources.input_seat);
  EXPECT_EQ(observed.client_key, seat.client_key);

  auto stale = seat.handle;
  ++stale.generation;
  EXPECT_EQ(
    coordinator.with_authenticated_worker_seat(stale, capture),
    worker_seat_authorization_status_e::seat_not_found
  );
  EXPECT_EQ(
    coordinator.with_authenticated_worker_seat(
      seat.handle,
      [](const authenticated_worker_seat_t &) {
        throw std::runtime_error {"test action failure"};
      }
    ),
    worker_seat_authorization_status_e::action_failed
  );
}

TEST(MultiseatWorkerCoordinator, SeatMetadataCannotGrantConnectionAccess) {
  temporary_root_t root;
  authority_store_t store {root.path(), deterministic_capability()};
  registry_t registry {"controller-current", {shared_gpu()}};
  auto sessions = std::make_shared<session_state_t>();
  fake_worker_backend_t backend;
  worker_coordinator_t coordinator {
    registry, backend, store, short_options(), {}, session_factory(sessions)
  };
  const auto seat = admit_and_bind(registry, "paired-client", "profile", "game");
  int calls = 0;
  const authenticated_worker_connection_action_t action = [&](const auto &, const auto &) {
    ++calls;
  };
  EXPECT_EQ(coordinator.with_authenticated_worker_connection({}, action),
    worker_seat_authorization_status_e::invalid_request);
  EXPECT_EQ(coordinator.with_authenticated_worker_connection(seat.handle, {}),
    worker_seat_authorization_status_e::invalid_request);
  EXPECT_EQ(coordinator.with_authenticated_worker_connection(seat.handle, action),
    worker_seat_authorization_status_e::reconciliation_required);
  ASSERT_TRUE(coordinator.reconcile().admission_ready);
  EXPECT_EQ(coordinator.with_authenticated_worker_connection(seat.handle, action),
    worker_seat_authorization_status_e::seat_not_running);
  ASSERT_EQ(coordinator.start_seat(seat.handle), coordinator_start_result_e::started);
  ASSERT_TRUE(backend.mark_ready(identity_for(seat)));
  ASSERT_EQ(coordinator.reconcile().broker.ready_transitions, std::size_t {1});
  // This test double reports authenticated metadata but has no real transport.
  EXPECT_EQ(coordinator.with_authenticated_worker_seat(seat.handle, [](const auto &) {}),
    worker_seat_authorization_status_e::applied);
  EXPECT_EQ(coordinator.with_authenticated_worker_connection(seat.handle, action),
    worker_seat_authorization_status_e::endpoint_not_authenticated);
  auto stale = seat.handle;
  ++stale.generation;
  EXPECT_EQ(coordinator.with_authenticated_worker_connection(stale, action),
    worker_seat_authorization_status_e::seat_not_found);
  EXPECT_EQ(calls, 0);
}

TEST(MultiseatWorkerCoordinator, AuthorizationSerializesWithExactStop) {
  temporary_root_t root;
  authority_store_t store {root.path(), deterministic_capability()};
  registry_t registry {"controller-current", {shared_gpu()}};
  auto sessions = std::make_shared<session_state_t>();
  fake_worker_backend_t backend;
  worker_coordinator_t coordinator {
    registry, backend, store, short_options(), {}, session_factory(sessions)
  };
  ASSERT_TRUE(coordinator.reconcile().admission_ready);
  const auto seat = admit_and_bind(
    registry,
    "paired-client",
    "profile-serialized",
    "serialized-game"
  );
  const auto identity = identity_for(seat);
  ASSERT_EQ(
    coordinator.start_seat(seat.handle),
    coordinator_start_result_e::started
  );
  ASSERT_TRUE(backend.mark_ready(identity));
  ASSERT_EQ(coordinator.reconcile().broker.ready_transitions, std::size_t {1});

  std::promise<void> entered;
  std::promise<void> release;
  auto release_future = release.get_future().share();
  auto authorization = std::async(std::launch::async, [&]() {
    return coordinator.with_authenticated_worker_seat(
      seat.handle,
      [&](const authenticated_worker_seat_t &) {
        entered.set_value();
        release_future.wait();
      }
    );
  });
  entered.get_future().wait();
  auto stop = std::async(std::launch::async, [&]() {
    return coordinator.stop_seat(seat.handle);
  });
  EXPECT_EQ(
    stop.wait_for(std::chrono::milliseconds {20}),
    std::future_status::timeout
  );

  release.set_value();
  EXPECT_EQ(
    authorization.get(),
    worker_seat_authorization_status_e::applied
  );
  EXPECT_EQ(stop.get().broker, broker_stop_result_e::stop_requested);
  ASSERT_TRUE(backend.complete(identity));
  EXPECT_EQ(coordinator.reconcile().removed_authorities, std::size_t {1});
}

TEST(MultiseatWorkerCoordinator, TamperedAuthorityCannotAuthorizeLaunchAction) {
  temporary_root_t root;
  authority_store_t store {root.path(), deterministic_capability()};
  registry_t registry {"controller-current", {shared_gpu()}};
  auto sessions = std::make_shared<session_state_t>();
  fake_worker_backend_t backend;
  worker_coordinator_t coordinator {
    registry, backend, store, short_options(), {}, session_factory(sessions)
  };
  ASSERT_TRUE(coordinator.reconcile().admission_ready);
  const auto seat = admit_and_bind(
    registry,
    "paired-client",
    "profile-tampered",
    "tampered-game"
  );
  const auto identity = identity_for(seat);
  ASSERT_EQ(
    coordinator.start_seat(seat.handle),
    coordinator_start_result_e::started
  );
  ASSERT_TRUE(backend.mark_ready(identity));
  ASSERT_EQ(coordinator.reconcile().broker.ready_transitions, std::size_t {1});

  const auto record = root.path() /
                      seat.resources.runtime_namespace /
                      std::string {authority_auth_directory_name} /
                      std::string {authority_record_file_name};
  const auto descriptor = ::open(record.c_str(), O_WRONLY | O_CLOEXEC);
  ASSERT_GE(descriptor, 0);
  const char changed = 'X';
  ASSERT_EQ(::write(descriptor, &changed, 1), 1);
  ASSERT_EQ(::close(descriptor), 0);
  bool invoked = false;

  EXPECT_EQ(
    coordinator.with_authenticated_worker_seat(
      seat.handle,
      [&invoked](const authenticated_worker_seat_t &) {
        invoked = true;
      }
    ),
    worker_seat_authorization_status_e::authority_rejected
  );
  EXPECT_FALSE(invoked);
  EXPECT_FALSE(coordinator.admission_ready());
}

#endif
