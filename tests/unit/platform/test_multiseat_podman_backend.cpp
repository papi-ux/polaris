/**
 * @file tests/unit/platform/test_multiseat_podman_backend.cpp
 * @brief Offline contract tests for the rootless Podman worker backend.
 */
#include "src/platform/linux/multiseat_podman_backend.h"

#ifdef __linux__

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <deque>
#include <filesystem>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
  using json = nlohmann::json;
  using multiseat::compositor_e;
  using multiseat::worker_command_result_e;
  using multiseat::worker_identity_t;
  using multiseat::worker_launch_spec_t;
  using multiseat::worker_observed_state_e;
  using multiseat::worker_stop_mode_e;
  using multiseat::podman::backend_t;
  using multiseat::podman::command_result_t;
  using multiseat::podman::gpu_t;
  using multiseat::podman::host_t;
  using multiseat::podman::options_t;
  using multiseat::podman::profile_t;
  using multiseat::podman::shared_game_mount_t;

  constexpr auto first_id =
    "1111111111111111111111111111111111111111111111111111111111111111";
  constexpr auto second_id =
    "2222222222222222222222222222222222222222222222222222222222222222";

  class fake_host_t final : public host_t {
  public:
    std::uint64_t effective_uid() const override {
      return uid;
    }

    bool executable_file(const std::filesystem::path &path) const override {
      return executable_ready && path == "/usr/bin/podman";
    }

    bool readable_directory(const std::filesystem::path &path) const override {
      return readable_directories.contains(path.native());
    }

    bool private_read_write_directory(const std::filesystem::path &path) const override {
      return private_directories.contains(path.native());
    }

    bool private_readable_file(const std::filesystem::path &path) const override {
      return private_files.contains(path.native());
    }

    bool read_write_character_device(const std::filesystem::path &path) const override {
      return accessible_devices.contains(path.native());
    }

    command_result_t run(
      const std::vector<std::string> &argv,
      std::chrono::milliseconds,
      std::size_t
    ) override {
      calls.push_back(argv);
      if (results.empty()) {
        throw std::runtime_error {"fake command result missing"};
      }
      auto result = std::move(results.front());
      results.pop_front();
      return result;
    }

    void push(command_result_t result) {
      results.push_back(std::move(result));
    }

    std::uint64_t uid = 1000;
    bool executable_ready = true;
    std::set<std::string> readable_directories {
      "/srv/Games Library",
    };
    std::set<std::string> private_directories {
      "/run/user/1000/polaris-workers",
      "/run/user/1000/polaris-workers/polaris-runtime-controller-a1b2-1",
      "/run/user/1000/polaris-workers/polaris-runtime-controller-a1b2-1/ipc",
      "/run/user/1000/polaris-workers/polaris-runtime-controller-a1b2-1/auth",
      "/run/user/1000/polaris-workers/polaris-runtime-controller-a1b2-2",
      "/run/user/1000/polaris-workers/polaris-runtime-controller-a1b2-2/ipc",
      "/run/user/1000/polaris-workers/polaris-runtime-controller-a1b2-2/auth",
    };
    std::set<std::string> private_files {
      "/run/user/1000/polaris-workers/polaris-runtime-controller-a1b2-1/auth/auth-token",
      "/run/user/1000/polaris-workers/polaris-runtime-controller-a1b2-2/auth/auth-token",
    };
    std::set<std::string> accessible_devices {
      "/dev/dri/renderD128",
      "/dev/dri/card0",
      "/dev/uinput",
      "/dev/uhid",
    };
    std::deque<command_result_t> results;
    std::vector<std::vector<std::string>> calls;
  };

  options_t options_for_tests() {
    return {
      .executable = "/usr/bin/podman",
      .deployment_id = "deployment-a1b2",
      .image_reference = std::string {"ghcr.io/papi-ux/polaris-seat@sha256:"} +
                         std::string(64, 'a'),
      .worker_entrypoint = "/usr/bin/polaris-seat-worker",
      .ipc_root = "/run/user/1000/polaris-workers",
      .gpus = {
        gpu_t {
          .logical_gpu_id = "gpu-primary",
          .render_node = "/dev/dri/renderD128",
          .devices = {"/dev/dri/renderD128", "/dev/dri/card0"},
          .max_encoder_sessions = 2,
        },
      },
      .profiles = {
        profile_t {
          .profile_key = "profile alpha",
          .opaque_volume_name = "pv-a9f0",
        },
        profile_t {
          .profile_key = "profile beta",
          .opaque_volume_name = "pv-b8e1",
        },
      },
      .workload_keys = {"steam-game;literal", "heroic-game"},
      .input_devices = {"/dev/uinput", "/dev/uhid"},
      .shared_game_mounts = {
        shared_game_mount_t {
          .mount_name = "library-a",
          .host_path = "/srv/Games Library",
        },
      },
    };
  }

  worker_launch_spec_t spec_for(
    std::uint32_t slot = 0,
    std::uint64_t generation = 1,
    std::string worker_name = "polaris-worker-controller-a1b2-1",
    std::string profile_key = "profile alpha",
    std::string workload_key = "steam-game;literal"
  ) {
    const auto suffix = "controller-a1b2-" + std::to_string(generation);
    return {
      .identity = {
        .seat = {
          .controller_epoch = "controller-a1b2",
          .logical_gpu_id = "gpu-primary",
          .slot = slot,
          .generation = generation,
        },
        .worker_name = std::move(worker_name),
      },
      .resources = {
        .worker_name = {},
        .runtime_namespace = "polaris-runtime-" + suffix,
        .wayland_socket = "polaris-wayland-" + suffix,
        .audio_sink = "polaris-audio-" + suffix,
        .input_seat = "polaris-input-" + suffix,
      },
      .profile_key = std::move(profile_key),
      .workload_key = std::move(workload_key),
      .render_node = "/dev/dri/renderD128",
      .compositor = compositor_e::gamescope,
      .encoder_sessions = 1,
    };
  }

  worker_launch_spec_t valid_spec(
    std::uint32_t slot = 0,
    std::uint64_t generation = 1,
    std::string worker_name = "polaris-worker-controller-a1b2-1",
    std::string profile_key = "profile alpha",
    std::string workload_key = "steam-game;literal"
  ) {
    auto spec = spec_for(
      slot,
      generation,
      std::move(worker_name),
      std::move(profile_key),
      std::move(workload_key)
    );
    spec.resources.worker_name = spec.identity.worker_name;
    return spec;
  }

  bool has_argument(const std::vector<std::string> &argv, const std::string &argument) {
    return std::find(argv.begin(), argv.end(), argument) != argv.end();
  }

  bool any_argument_contains(
    const std::vector<std::string> &argv,
    const std::string &fragment
  ) {
    return std::any_of(
      argv.begin(),
      argv.end(),
      [&fragment](const auto &argument) {
        return argument.find(fragment) != std::string::npos;
      }
    );
  }

  json labels_for(const worker_launch_spec_t &spec) {
    return {
      {"io.polaris.multiseat.protocol", "1"},
      {"io.polaris.multiseat.deployment", "deployment-a1b2"},
      {"io.polaris.multiseat.controller", spec.identity.seat.controller_epoch},
      {"io.polaris.multiseat.gpu", spec.identity.seat.logical_gpu_id},
      {"io.polaris.multiseat.slot", std::to_string(spec.identity.seat.slot)},
      {"io.polaris.multiseat.generation", std::to_string(spec.identity.seat.generation)},
      {"io.polaris.multiseat.worker", spec.identity.worker_name},
      {"io.polaris.multiseat.runtime", spec.resources.runtime_namespace},
      {"io.polaris.multiseat.wayland", spec.resources.wayland_socket},
      {"io.polaris.multiseat.audio", spec.resources.audio_sink},
      {"io.polaris.multiseat.input", spec.resources.input_seat},
      {"io.polaris.multiseat.render-node", spec.render_node},
      {"io.polaris.multiseat.compositor", "gamescope"},
      {"io.polaris.multiseat.encoders", std::to_string(spec.encoder_sessions)},
    };
  }

  json container_for(
    const worker_launch_spec_t &spec,
    std::string id,
    std::string runtime_state,
    std::string health_state = "healthy",
    bool alternate_health_key = false
  ) {
    json state {
      {"Status", std::move(runtime_state)},
    };
    state[alternate_health_key ? "Healthcheck" : "Health"] = {
      {"Status", std::move(health_state)},
    };
    return {
      {"Id", std::move(id)},
      {"Name", spec.identity.worker_name},
      {"Config", {{"Labels", labels_for(spec)}}},
      {"State", std::move(state)},
    };
  }

  void queue_inventory(fake_host_t &host, const std::vector<json> &containers) {
    std::string ids;
    for (const auto &container : containers) {
      ids += container.at("Id").get<std::string>() + "\n";
    }
    host.push({
      .exit_status = 0,
      .output = std::move(ids),
    });
    if (!containers.empty()) {
      host.push({
        .exit_status = 0,
        .output = json(containers).dump(),
      });
    }
  }
}  // namespace

TEST(MultiseatPodmanBackend, RejectsUnpinnedImagesAndDuplicateProfileVolumes) {
  fake_host_t host;
  auto options = options_for_tests();
  options.image_reference = "ghcr.io/papi-ux/polaris-seat:latest";
  EXPECT_THROW(backend_t(host, options), std::invalid_argument);

  options = options_for_tests();
  options.image_reference = std::string {"dir:/tmp/worker@sha256:"} +
                            std::string(64, 'a');
  EXPECT_THROW(backend_t(host, options), std::invalid_argument);

  options = options_for_tests();
  options.profiles.at(1).opaque_volume_name = options.profiles.at(0).opaque_volume_name;
  EXPECT_THROW(backend_t(host, options), std::invalid_argument);

  options = options_for_tests();
  options.ipc_root = "/run/user/1000/../polaris-workers";
  EXPECT_THROW(backend_t(host, options), std::invalid_argument);
}

TEST(MultiseatPodmanBackend, LaunchBuildsRootlessIsolatedArgumentVector) {
  fake_host_t host;
  backend_t backend {host, options_for_tests()};
  const auto spec = valid_spec();
  host.push({
    .exit_status = 0,
    .output = std::string {first_id} + "\n",
  });

  ASSERT_EQ(backend.launch(spec), worker_command_result_e::applied);
  ASSERT_EQ(host.calls.size(), std::size_t {1});
  const auto &argv = host.calls.front();
  ASSERT_GE(argv.size(), std::size_t {3});
  EXPECT_EQ(argv.at(0), "/usr/bin/podman");
  EXPECT_EQ(argv.at(1), "--remote=false");
  EXPECT_EQ(argv.at(2), "run");
  EXPECT_TRUE(has_argument(argv, "--userns=keep-id"));
  EXPECT_TRUE(has_argument(argv, "--network=none"));
  EXPECT_TRUE(has_argument(argv, "--no-hosts"));
  EXPECT_TRUE(has_argument(argv, "--http-proxy=false"));
  EXPECT_TRUE(has_argument(argv, "--ipc=private"));
  EXPECT_TRUE(has_argument(argv, "--pid=private"));
  EXPECT_TRUE(has_argument(argv, "--uts=private"));
  EXPECT_TRUE(has_argument(argv, "--cgroupns=private"));
  EXPECT_TRUE(has_argument(argv, "--cap-drop=all"));
  EXPECT_TRUE(has_argument(argv, "--security-opt=no-new-privileges"));
  EXPECT_TRUE(has_argument(argv, "--read-only"));
  EXPECT_TRUE(has_argument(argv, "--read-only-tmpfs=true"));
  EXPECT_TRUE(has_argument(argv, "--shm-size=1073741824b"));
  EXPECT_TRUE(has_argument(
    argv,
    "--mount=type=tmpfs,dst=/run/polaris,rw=true,tmpfs-size=67108864,"
    "tmpfs-mode=0700,U=true,notmpcopyup"
  ));
  EXPECT_TRUE(has_argument(
    argv,
    "--mount=type=tmpfs,dst=/tmp,rw=true,tmpfs-size=1073741824,"
    "tmpfs-mode=0700,U=true,notmpcopyup"
  ));
  EXPECT_FALSE(has_argument(argv, "--read-only-tmpfs=false"));
  EXPECT_TRUE(has_argument(argv, "--pull=never"));
  EXPECT_TRUE(has_argument(argv, "--rm"));
  EXPECT_TRUE(has_argument(
    argv,
    "--volume=pv-a9f0:/var/lib/polaris-seat:rw,nosuid,nodev,nocreate"
  ));
  EXPECT_TRUE(has_argument(argv, "--workdir=/var/lib/polaris-seat"));
  EXPECT_TRUE(has_argument(argv, "--env=HOME=/var/lib/polaris-seat"));
  EXPECT_TRUE(has_argument(
    argv,
    "--mount=type=bind,src=/srv/Games Library,dst=/mnt/games/library-a,ro=true"
  ));
  EXPECT_TRUE(has_argument(
    argv,
    "--device=/dev/dri/renderD128:/dev/dri/renderD128:rw"
  ));
  EXPECT_TRUE(has_argument(argv, "--device=/dev/uinput:/dev/uinput:rw"));
  EXPECT_TRUE(has_argument(argv, "--env=WAYLAND_DISPLAY=polaris-wayland-controller-a1b2-1"));
  EXPECT_TRUE(has_argument(argv, "--env=PULSE_SINK=polaris-audio-controller-a1b2-1"));
  EXPECT_TRUE(has_argument(argv, "--env=POLARIS_INPUT_SEAT=polaris-input-controller-a1b2-1"));
  EXPECT_TRUE(has_argument(argv, "--env=POLARIS_WORKER_NAME=polaris-worker-controller-a1b2-1"));
  EXPECT_TRUE(has_argument(argv, "--env=POLARIS_COMPOSITOR=gamescope"));
  EXPECT_TRUE(has_argument(
    argv,
    "--mount=type=bind,src=/run/user/1000/polaris-workers/"
    "polaris-runtime-controller-a1b2-1/ipc,dst=/run/polaris-ipc,"
    "rw=true,relabel=private,bind-nonrecursive"
  ));
  EXPECT_TRUE(has_argument(
    argv,
    "--mount=type=bind,src=/run/user/1000/polaris-workers/"
    "polaris-runtime-controller-a1b2-1/auth,dst=/run/polaris-auth,"
    "ro=true,relabel=private,bind-nonrecursive"
  ));
  EXPECT_TRUE(has_argument(argv, "--health-on-failure=none"));
  EXPECT_TRUE(has_argument(argv, "--workload-key=steam-game;literal"));
  EXPECT_TRUE(any_argument_contains(argv, "--health-cmd=[\"/usr/bin/polaris-seat-worker\",\"health\"]"));
  EXPECT_FALSE(any_argument_contains(argv, "profile alpha"));
  EXPECT_FALSE(any_argument_contains(argv, "--privileged"));
  EXPECT_FALSE(any_argument_contains(argv, "--network=host"));
  EXPECT_FALSE(any_argument_contains(argv, "--pid=host"));
  EXPECT_FALSE(any_argument_contains(argv, "docker.sock"));
  EXPECT_FALSE(any_argument_contains(argv, "keep-groups"));
  EXPECT_FALSE(any_argument_contains(argv, "POLARIS_AUTH_TOKEN"));
  EXPECT_FALSE(any_argument_contains(argv, std::string(64, '0')));
}

TEST(MultiseatPodmanBackend, LaunchRejectsPrivilegedOrInaccessibleHostsBeforeCommand) {
  fake_host_t host;
  backend_t backend {host, options_for_tests()};
  host.uid = 0;
  EXPECT_EQ(backend.launch(valid_spec()), worker_command_result_e::rejected);
  EXPECT_TRUE(host.calls.empty());

  host.uid = 1000;
  host.accessible_devices.erase("/dev/uhid");
  EXPECT_EQ(backend.launch(valid_spec()), worker_command_result_e::rejected);
  EXPECT_TRUE(host.calls.empty());

  host.accessible_devices.insert("/dev/uhid");
  host.private_files.clear();
  EXPECT_EQ(backend.launch(valid_spec()), worker_command_result_e::rejected);
  EXPECT_TRUE(host.calls.empty());
}

TEST(MultiseatPodmanBackend, LaunchRejectsUnknownOrNonConcreteAllocation) {
  fake_host_t host;
  backend_t backend {host, options_for_tests()};

  auto spec = valid_spec();
  spec.profile_key = "unknown profile";
  EXPECT_EQ(backend.launch(spec), worker_command_result_e::rejected);

  spec = valid_spec();
  spec.workload_key = "unknown workload";
  EXPECT_EQ(backend.launch(spec), worker_command_result_e::rejected);

  spec = valid_spec();
  spec.compositor = compositor_e::automatic;
  EXPECT_EQ(backend.launch(spec), worker_command_result_e::rejected);

  spec = valid_spec();
  spec.render_node = "/dev/dri/renderD129";
  EXPECT_EQ(backend.launch(spec), worker_command_result_e::rejected);
  EXPECT_TRUE(host.calls.empty());
}

TEST(MultiseatPodmanBackend, TimedOutLaunchIsIndeterminate) {
  fake_host_t host;
  backend_t backend {host, options_for_tests()};
  host.push({
    .exit_status = 124,
    .timed_out = true,
  });

  EXPECT_EQ(backend.launch(valid_spec()), worker_command_result_e::indeterminate);
  EXPECT_EQ(host.calls.size(), std::size_t {1});
}

TEST(MultiseatPodmanBackend, AmbiguousSuccessfulLaunchOutputIsIndeterminate) {
  fake_host_t host;
  backend_t backend {host, options_for_tests()};
  host.push({
    .exit_status = 0,
    .output = std::string {first_id} + "\nunexpected output\n",
  });

  EXPECT_EQ(backend.launch(valid_spec()), worker_command_result_e::indeterminate);
  EXPECT_EQ(host.calls.size(), std::size_t {1});
}

TEST(MultiseatPodmanBackend, NonzeroLaunchReconcilesAnExactExistingWorker) {
  fake_host_t host;
  backend_t backend {host, options_for_tests()};
  const auto spec = valid_spec();
  host.push({
    .exit_status = 125,
  });
  queue_inventory(host, {container_for(spec, first_id, "running", "healthy")});

  EXPECT_EQ(backend.launch(spec), worker_command_result_e::already_applied);
  ASSERT_EQ(host.calls.size(), std::size_t {3});
  EXPECT_EQ(host.calls.at(1).at(2), "ps");
  EXPECT_EQ(host.calls.at(2).at(2), "container");
}

TEST(MultiseatPodmanBackend, NonzeroLaunchRejectsMissingAndQuarantinesUnsafeWorkers) {
  const auto spec = valid_spec();

  fake_host_t missing_host;
  backend_t missing_backend {missing_host, options_for_tests()};
  missing_host.push({.exit_status = 125});
  queue_inventory(missing_host, {});
  EXPECT_EQ(missing_backend.launch(spec), worker_command_result_e::rejected);
  EXPECT_EQ(missing_host.calls.size(), std::size_t {2});

  fake_host_t mismatch_host;
  backend_t mismatch_backend {mismatch_host, options_for_tests()};
  auto mismatched = container_for(spec, first_id, "running", "healthy");
  mismatched["Config"]["Labels"]["io.polaris.multiseat.runtime"] =
    "polaris-runtime-controller-a1b2-wrong";
  mismatch_host.push({.exit_status = 125});
  queue_inventory(mismatch_host, {std::move(mismatched)});
  EXPECT_EQ(mismatch_backend.launch(spec), worker_command_result_e::indeterminate);
  EXPECT_EQ(mismatch_host.calls.size(), std::size_t {3});

  fake_host_t failed_host;
  backend_t failed_backend {failed_host, options_for_tests()};
  failed_host.push({.exit_status = 125});
  queue_inventory(failed_host, {container_for(spec, first_id, "running", "unhealthy")});
  EXPECT_EQ(failed_backend.launch(spec), worker_command_result_e::indeterminate);

  fake_host_t stopped_host;
  backend_t stopped_backend {stopped_host, options_for_tests()};
  stopped_host.push({.exit_status = 125});
  queue_inventory(stopped_host, {container_for(spec, first_id, "exited", "")});
  EXPECT_EQ(stopped_backend.launch(spec), worker_command_result_e::rejected);
}

TEST(MultiseatPodmanBackend, InventoryParsesIndependentWorkerHealthAndIdentity) {
  fake_host_t host;
  backend_t backend {host, options_for_tests()};
  const auto first = valid_spec();
  auto second = valid_spec(
    1,
    2,
    "polaris-worker-controller-a1b2-2",
    "profile beta",
    "heroic-game"
  );
  second.compositor = compositor_e::labwc;
  auto second_record = container_for(second, second_id, "stopping", "", true);
  second_record["Config"]["Labels"]["io.polaris.multiseat.compositor"] = "labwc";
  queue_inventory(host, {
    container_for(first, first_id, "running", "healthy"),
    std::move(second_record),
  });

  const auto observations = backend.inventory();
  ASSERT_EQ(observations.size(), std::size_t {2});
  EXPECT_EQ(observations.at(0).identity, first.identity);
  EXPECT_EQ(observations.at(0).state, worker_observed_state_e::ready);
  EXPECT_EQ(observations.at(1).identity, second.identity);
  EXPECT_EQ(observations.at(1).state, worker_observed_state_e::stopping);
  ASSERT_EQ(host.calls.size(), std::size_t {2});
  EXPECT_TRUE(any_argument_contains(
    host.calls.at(0),
    "--filter=label=io.polaris.multiseat.deployment=deployment-a1b2"
  ));
  EXPECT_TRUE(has_argument(host.calls.at(1), first_id));
  EXPECT_TRUE(has_argument(host.calls.at(1), second_id));
}

TEST(MultiseatPodmanBackend, InventoryDoesNotCallRunningReadyWithoutHealthySignal) {
  fake_host_t host;
  backend_t backend {host, options_for_tests()};
  const auto first = valid_spec();
  const auto second = valid_spec(
    1,
    2,
    "polaris-worker-controller-a1b2-2",
    "profile beta",
    "heroic-game"
  );
  queue_inventory(host, {
    container_for(first, first_id, "running", ""),
    container_for(second, second_id, "running", "unhealthy", true),
  });

  const auto observations = backend.inventory();
  ASSERT_EQ(observations.size(), std::size_t {2});
  EXPECT_EQ(observations.at(0).state, worker_observed_state_e::starting);
  EXPECT_EQ(observations.at(1).state, worker_observed_state_e::failed);
}

TEST(MultiseatPodmanBackend, InventoryRejectsTruncationAndIncompleteLabels) {
  fake_host_t truncated_host;
  backend_t truncated_backend {truncated_host, options_for_tests()};
  truncated_host.push({
    .exit_status = 0,
    .output_truncated = true,
    .output = first_id,
  });
  EXPECT_THROW(truncated_backend.inventory(), std::runtime_error);

  fake_host_t malformed_host;
  backend_t malformed_backend {malformed_host, options_for_tests()};
  auto malformed = container_for(valid_spec(), first_id, "running", "healthy");
  malformed["Config"]["Labels"].erase("io.polaris.multiseat.generation");
  queue_inventory(malformed_host, {std::move(malformed)});
  EXPECT_THROW(malformed_backend.inventory(), std::runtime_error);
}

TEST(MultiseatPodmanBackend, InventoryRejectsIdCardinalityAndBoundViolations) {
  const auto spec = valid_spec();

  fake_host_t duplicate_host;
  backend_t duplicate_backend {duplicate_host, options_for_tests()};
  duplicate_host.push({
    .exit_status = 0,
    .output = std::string {first_id} + "\n" + first_id + "\n",
  });
  EXPECT_THROW(duplicate_backend.inventory(), std::runtime_error);
  EXPECT_EQ(duplicate_host.calls.size(), std::size_t {1});

  fake_host_t cardinality_host;
  backend_t cardinality_backend {cardinality_host, options_for_tests()};
  cardinality_host.push({
    .exit_status = 0,
    .output = std::string {first_id} + "\n" + second_id + "\n",
  });
  cardinality_host.push({
    .exit_status = 0,
    .output = json::array({container_for(spec, first_id, "running", "healthy")}).dump(),
  });
  EXPECT_THROW(cardinality_backend.inventory(), std::runtime_error);

  fake_host_t mismatched_id_host;
  backend_t mismatched_id_backend {mismatched_id_host, options_for_tests()};
  mismatched_id_host.push({
    .exit_status = 0,
    .output = std::string {first_id} + "\n",
  });
  mismatched_id_host.push({
    .exit_status = 0,
    .output = json::array({container_for(spec, second_id, "running", "healthy")}).dump(),
  });
  EXPECT_THROW(mismatched_id_backend.inventory(), std::runtime_error);

  fake_host_t bounded_host;
  auto bounded_options = options_for_tests();
  bounded_options.max_inventory_workers = 1;
  backend_t bounded_backend {bounded_host, std::move(bounded_options)};
  bounded_host.push({
    .exit_status = 0,
    .output = std::string {first_id} + "\n" + second_id + "\n",
  });
  EXPECT_THROW(bounded_backend.inventory(), std::runtime_error);
  EXPECT_EQ(bounded_host.calls.size(), std::size_t {1});
}

TEST(MultiseatPodmanBackend, GracefulAndForcedStopTargetOnlyTheInspectedContainerId) {
  const auto spec = valid_spec();

  fake_host_t graceful_host;
  backend_t graceful_backend {graceful_host, options_for_tests()};
  queue_inventory(graceful_host, {container_for(spec, first_id, "running", "healthy")});
  graceful_host.push({.exit_status = 0});
  EXPECT_EQ(
    graceful_backend.stop(spec.identity, worker_stop_mode_e::graceful),
    worker_command_result_e::applied
  );
  ASSERT_EQ(graceful_host.calls.size(), std::size_t {3});
  EXPECT_EQ(
    graceful_host.calls.back(),
    (std::vector<std::string> {
      "/usr/bin/podman", "--remote=false", "kill", "--signal=TERM", first_id,
    })
  );

  fake_host_t force_host;
  backend_t force_backend {force_host, options_for_tests()};
  queue_inventory(force_host, {container_for(spec, first_id, "running", "healthy")});
  force_host.push({.exit_status = 0});
  EXPECT_EQ(
    force_backend.stop(spec.identity, worker_stop_mode_e::force),
    worker_command_result_e::applied
  );
  ASSERT_EQ(force_host.calls.size(), std::size_t {3});
  EXPECT_EQ(
    force_host.calls.back(),
    (std::vector<std::string> {
      "/usr/bin/podman", "--remote=false", "rm", "--force", first_id,
    })
  );
  EXPECT_FALSE(has_argument(force_host.calls.back(), spec.identity.worker_name));
  EXPECT_FALSE(has_argument(force_host.calls.back(), "--all"));
  EXPECT_FALSE(has_argument(force_host.calls.back(), "--latest"));
}

TEST(MultiseatPodmanBackend, GracefulStopRemovesAWorkerThatNeverStarted) {
  fake_host_t host;
  backend_t backend {host, options_for_tests()};
  const auto spec = valid_spec();
  queue_inventory(host, {container_for(spec, first_id, "created", "")});
  host.push({.exit_status = 0});

  EXPECT_EQ(
    backend.stop(spec.identity, worker_stop_mode_e::graceful),
    worker_command_result_e::applied
  );
  EXPECT_EQ(
    host.calls.back(),
    (std::vector<std::string> {"/usr/bin/podman", "--remote=false", "rm", first_id})
  );
}

TEST(MultiseatPodmanBackend, MissingExactWorkerReturnsNotFoundWithoutStopCommand) {
  fake_host_t host;
  backend_t backend {host, options_for_tests()};
  queue_inventory(host, {});

  EXPECT_EQ(
    backend.stop(valid_spec().identity, worker_stop_mode_e::graceful),
    worker_command_result_e::not_found
  );
  ASSERT_EQ(host.calls.size(), std::size_t {1});
  EXPECT_EQ(host.calls.front().at(2), "ps");
}

TEST(MultiseatPodmanBackend, FailedStopReconcilesAuthoritativeAbsence) {
  fake_host_t host;
  backend_t backend {host, options_for_tests()};
  const auto spec = valid_spec();
  queue_inventory(host, {container_for(spec, first_id, "running", "healthy")});
  host.push({.exit_status = 125});
  queue_inventory(host, {});

  EXPECT_EQ(
    backend.stop(spec.identity, worker_stop_mode_e::graceful),
    worker_command_result_e::not_found
  );
  ASSERT_EQ(host.calls.size(), std::size_t {4});
  EXPECT_EQ(host.calls.at(2).at(2), "kill");
  EXPECT_EQ(host.calls.at(3).at(2), "ps");
}

TEST(MultiseatPodmanBackend, InventoryRefusesPrivilegedExecutionWithoutCommands) {
  fake_host_t host;
  host.uid = 0;
  backend_t backend {host, options_for_tests()};

  EXPECT_THROW(backend.inventory(), std::runtime_error);
  EXPECT_TRUE(host.calls.empty());
}

#endif
