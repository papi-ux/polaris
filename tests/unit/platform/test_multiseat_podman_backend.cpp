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
#include <functional>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace {
  using json = nlohmann::json;
  using multiseat::compositor_e;
  using multiseat::display_topology_e;
  using multiseat::media_pipeline_e;
  using multiseat::runtime_profile_e;
  using multiseat::workload_kind_e;
  using multiseat::worker_command_result_e;
  using multiseat::worker_identity_t;
  using multiseat::worker_launch_spec_t;
  using multiseat::worker_observed_state_e;
  using multiseat::worker_stop_mode_e;
  using multiseat::podman::backend_t;
  using multiseat::podman::character_device_identity_t;
  using multiseat::podman::command_result_t;
  using multiseat::podman::gpu_t;
  using multiseat::podman::host_t;
  using multiseat::podman::input_manifest_source_t;
  using multiseat::podman::options_t;
  using multiseat::podman::profile_t;
  using multiseat::podman::shared_game_mount_t;

  constexpr auto first_id =
    "1111111111111111111111111111111111111111111111111111111111111111";
  constexpr auto second_id =
    "2222222222222222222222222222222222222222222222222222222222222222";

  character_device_identity_t device_identity(
    std::uint32_t major,
    std::uint32_t minor
  ) {
    return {
      .filesystem_device = 1,
      .inode = 10000 + minor,
      .character_major = major,
      .character_minor = minor,
    };
  }

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

    std::optional<character_device_identity_t> read_write_character_device(
      const std::filesystem::path &path
    ) const override {
      const auto device = accessible_devices.find(path.native());
      return device == accessible_devices.end() ?
               std::nullopt : std::optional {device->second};
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
    std::map<std::string, character_device_identity_t> accessible_devices {
      {"/dev/dri/renderD128", device_identity(226, 128)},
      {"/dev/dri/card0", device_identity(226, 0)},
      {"/dev/input/event10", device_identity(13, 74)},
      {"/dev/input/event11", device_identity(13, 75)},
      {"/dev/input/event12", device_identity(13, 76)},
      {"/dev/input/event13", device_identity(13, 77)},
      {"/dev/input/event20", device_identity(13, 84)},
      {"/dev/input/event21", device_identity(13, 85)},
      {"/dev/input/event22", device_identity(13, 86)},
      {"/dev/input/event23", device_identity(13, 87)},
    };
    std::deque<command_result_t> results;
    std::vector<std::vector<std::string>> calls;
  };

  options_t options_for_tests() {
    return {
      .executable = "/usr/bin/podman",
      .deployment_id = "deployment-a1b2",
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
          .runtime_profile = runtime_profile_e::steam,
          .image_reference = std::string {"ghcr.io/papi-ux/polaris-seat-steam@sha256:"} +
                             std::string(64, 'a'),
        },
        profile_t {
          .profile_key = "profile beta",
          .opaque_volume_name = "pv-b8e1",
          .runtime_profile = runtime_profile_e::heroic,
          .image_reference = std::string {"ghcr.io/papi-ux/polaris-seat-heroic@sha256:"} +
                             std::string(64, 'b'),
        },
      },
      .workloads = {
        {workload_kind_e::steam, "steam-game"},
        {workload_kind_e::heroic, "heroic-game"},
      },
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
    std::string workload_id = "steam-game"
  ) {
    const auto suffix = "controller-a1b2-" + std::to_string(generation);
    const auto runtime_profile = profile_key == "profile beta" ?
                                   runtime_profile_e::heroic :
                                   runtime_profile_e::steam;
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
        .capture_wayland_socket = "polaris-capture-" + suffix,
        .wayland_socket = "polaris-wayland-" + suffix,
        .audio_sink = "polaris-audio-" + suffix,
        .input_seat = "polaris-input-" + suffix,
      },
      .profile_key = std::move(profile_key),
      .workload = {
        runtime_profile == runtime_profile_e::heroic ?
          workload_kind_e::heroic : workload_kind_e::steam,
        std::move(workload_id),
      },
      .render_node = "/dev/dri/renderD128",
      .runtime_profile = runtime_profile,
      .data_plane = {
        .display_topology = display_topology_e::capture_host_with_nested_compositor,
        .media_pipeline = media_pipeline_e::worker_local_capture_encode,
      },
      .display_mode = {3840, 2160, 97000, true},
      .compositor = compositor_e::gamescope,
      .encoder_sessions = 1,
    };
  }

  worker_launch_spec_t valid_spec(
    std::uint32_t slot = 0,
    std::uint64_t generation = 1,
    std::string worker_name = "polaris-worker-controller-a1b2-1",
    std::string profile_key = "profile alpha",
    std::string workload_id = "steam-game"
  ) {
    auto spec = spec_for(
      slot,
      generation,
      std::move(worker_name),
      std::move(profile_key),
      std::move(workload_id)
    );
    spec.resources.worker_name = spec.identity.worker_name;
    return spec;
  }

  std::string input_seat_for(const multiseat::seat_handle_t &handle) {
    return "polaris-input-" + handle.controller_epoch + "-" +
           std::to_string(handle.generation);
  }

  multiseat::input::allocation_t input_allocation_for(
    const multiseat::seat_handle_t &handle
  ) {
    using multiseat::input::device_kind_e;
    const auto input_seat = input_seat_for(handle);
    const auto base_event = static_cast<std::uint32_t>(handle.generation * 10);
    const auto node = [&input_seat](
                        device_kind_e kind,
                        std::uint32_t slot,
                        std::uint32_t event_number
                      ) {
      const auto identity = device_identity(13, 64 + event_number);
      return multiseat::input::device_node_t {
        .kind = kind,
        .slot = slot,
        .host_path = "/dev/input/event" + std::to_string(event_number),
        .worker_path = multiseat::input::expected_worker_path(kind, slot),
        .filesystem_device = identity.filesystem_device,
        .inode = identity.inode,
        .character_major = identity.character_major,
        .character_minor = identity.character_minor,
        .kernel_name = multiseat::input::expected_kernel_name(
          input_seat,
          kind,
          slot
        ),
        .phys = multiseat::input::expected_phys(input_seat, kind, slot),
        .host_seat = std::string {multiseat::input::isolated_host_seat},
      };
    };
    return {
      .handle = handle,
      .input_seat = input_seat,
      .plan = {
        .touch = false,
        .pen = false,
        .gamepad_slots = 1,
      },
      .nodes = {
        node(device_kind_e::keyboard, 0, base_event),
        node(device_kind_e::mouse_relative, 0, base_event + 1),
        node(device_kind_e::mouse_absolute, 0, base_event + 2),
        node(device_kind_e::gamepad, 0, base_event + 3),
      },
    };
  }

  multiseat::input::node_observation_t observation_for(
    const multiseat::input::device_node_t &node
  ) {
    return {
      .status = multiseat::input::node_observation_status_e::observed,
      .snapshot = multiseat::input::kernel_node_snapshot_t {
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

  class fake_input_manifest_source_t final : public input_manifest_source_t {
  public:
    std::optional<multiseat::input::allocation_t> allocation(
      const multiseat::seat_handle_t &handle
    ) override {
      ++allocation_calls;
      if (throw_on_allocation_call == allocation_calls) {
        throw std::runtime_error {"fake input authority failure"};
      }
      if (missing ||
          (replacement_after_call != 0 &&
           allocation_calls >= replacement_after_call && !replacement)) {
        return std::nullopt;
      }
      auto result = replacement_after_call != 0 &&
                        allocation_calls >= replacement_after_call ?
                      *replacement : input_allocation_for(handle);
      const auto existing = std::find_if(
        issued.begin(),
        issued.end(),
        [&handle](const auto &candidate) {
          return candidate.handle == handle;
        }
      );
      if (existing == issued.end()) {
        issued.push_back(result);
      } else {
        *existing = result;
      }
      return result;
    }

    multiseat::input::node_observation_t observe(
      const std::filesystem::path &path
    ) override {
      ++observation_calls;
      const auto sequence = observations.find(path.native());
      if (sequence != observations.end() && !sequence->second.empty()) {
        auto result = sequence->second.front();
        if (sequence->second.size() > 1) {
          sequence->second.pop_front();
        }
        return result;
      }
      for (auto allocation = issued.rbegin(); allocation != issued.rend(); ++allocation) {
        const auto node = std::find_if(
          allocation->nodes.begin(),
          allocation->nodes.end(),
          [&path](const auto &candidate) {
            return candidate.host_path == path;
          }
        );
        if (node != allocation->nodes.end()) {
          return observation_for(*node);
        }
      }
      return {.status = multiseat::input::node_observation_status_e::absent};
    }

    bool missing = false;
    std::size_t allocation_calls = 0;
    std::size_t observation_calls = 0;
    std::size_t throw_on_allocation_call = 0;
    std::size_t replacement_after_call = 0;
    std::optional<multiseat::input::allocation_t> replacement;
    std::map<
      std::string,
      std::deque<multiseat::input::node_observation_t>
    > observations;
    std::vector<multiseat::input::allocation_t> issued;
  };

  class fixed_input_backend_t final : public multiseat::input::backend_t {
  public:
    explicit fixed_input_backend_t(multiseat::input::allocation_t allocation) :
        allocation_(std::move(allocation)) {
    }

    multiseat::input::backend_create_result_t create(
      const multiseat::input::expectation_t &
    ) override {
      return {.result = multiseat::input::backend_result_e::rejected};
    }

    multiseat::input::backend_result_e destroy(
      const multiseat::seat_handle_t &,
      std::string_view
    ) override {
      return multiseat::input::backend_result_e::rejected;
    }

    multiseat::input::backend_result_e route(
      const multiseat::seat_handle_t &,
      std::string_view,
      std::uint64_t,
      const multiseat::input::input_event_t &
    ) override {
      return multiseat::input::backend_result_e::rejected;
    }

    std::vector<multiseat::input::allocation_t> inventory() override {
      return {allocation_};
    }

  private:
    multiseat::input::allocation_t allocation_;
  };

  class fixed_kernel_probe_t final : public multiseat::input::kernel_node_probe_t {
  public:
    explicit fixed_kernel_probe_t(const multiseat::input::allocation_t &allocation) {
      for (const auto &node : allocation.nodes) {
        observations_.emplace(node.host_path.native(), observation_for(node));
      }
    }

    multiseat::input::node_observation_t observe(
      const std::filesystem::path &path
    ) override {
      const auto observation = observations_.find(path.native());
      return observation == observations_.end() ?
               multiseat::input::node_observation_t {
                 .status = multiseat::input::node_observation_status_e::absent,
               } : observation->second;
    }

  private:
    std::map<std::string, multiseat::input::node_observation_t> observations_;
  };

  fake_input_manifest_source_t &input_manifests_for_tests() {
    static fake_input_manifest_source_t source;
    return source;
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
    const auto image = spec.runtime_profile == runtime_profile_e::heroic ?
                         std::string {"ghcr.io/papi-ux/polaris-seat-heroic@sha256:"} +
                           std::string(64, 'b') :
                         std::string {"ghcr.io/papi-ux/polaris-seat-steam@sha256:"} +
                           std::string(64, 'a');
    const auto profile = spec.runtime_profile == runtime_profile_e::heroic ? "heroic" : "steam";
    const auto workload_kind = spec.workload.kind == workload_kind_e::heroic ?
                                 "heroic" : "steam";
    const auto input_fingerprint = multiseat::podman::input_manifest_fingerprint(
      input_allocation_for(spec.identity.seat)
    );
    if (!input_fingerprint) {
      throw std::runtime_error {"test input manifest is invalid"};
    }
    return {
      {"io.polaris.multiseat.protocol", "3"},
      {"io.polaris.multiseat.deployment", "deployment-a1b2"},
      {"io.polaris.multiseat.controller", spec.identity.seat.controller_epoch},
      {"io.polaris.multiseat.gpu", spec.identity.seat.logical_gpu_id},
      {"io.polaris.multiseat.slot", std::to_string(spec.identity.seat.slot)},
      {"io.polaris.multiseat.generation", std::to_string(spec.identity.seat.generation)},
      {"io.polaris.multiseat.worker", spec.identity.worker_name},
      {"io.polaris.multiseat.runtime", spec.resources.runtime_namespace},
      {"io.polaris.multiseat.capture-wayland", spec.resources.capture_wayland_socket},
      {"io.polaris.multiseat.wayland", spec.resources.wayland_socket},
      {"io.polaris.multiseat.audio", spec.resources.audio_sink},
      {"io.polaris.multiseat.input", spec.resources.input_seat},
      {"io.polaris.multiseat.input-manifest", *input_fingerprint},
      {"io.polaris.multiseat.render-node", spec.render_node},
      {"io.polaris.multiseat.runtime-profile", profile},
      {"io.polaris.multiseat.workload-kind", workload_kind},
      {"io.polaris.multiseat.workload-target", spec.workload.target_id},
      {"io.polaris.multiseat.display-topology", "capture-host-with-nested-compositor"},
      {"io.polaris.multiseat.media-pipeline", "worker-local-capture-encode"},
      {"io.polaris.multiseat.runtime-image", image},
      {"io.polaris.multiseat.display-width", std::to_string(spec.display_mode.width)},
      {"io.polaris.multiseat.display-height", std::to_string(spec.display_mode.height)},
      {"io.polaris.multiseat.display-refresh-millihz", std::to_string(spec.display_mode.refresh_millihz)},
      {"io.polaris.multiseat.display-hdr", spec.display_mode.hdr ? "1" : "0"},
      {"io.polaris.multiseat.compositor", "gamescope"},
      {"io.polaris.multiseat.encoders", std::to_string(spec.encoder_sessions)},
    };
  }

  json inspected_devices_for(const worker_launch_spec_t &spec) {
    json devices = json::array({
      {
        {"PathOnHost", "/dev/dri/renderD128"},
        {"PathInContainer", "/dev/dri/renderD128"},
      },
      {
        {"PathOnHost", "/dev/dri/card0"},
        {"PathInContainer", "/dev/dri/card0"},
      },
    });
    for (const auto &node : input_allocation_for(spec.identity.seat).nodes) {
      devices.push_back({
        {"PathOnHost", node.host_path.native()},
        {"PathInContainer", node.worker_path.native()},
      });
    }
    return devices;
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
      {"HostConfig", {{"Devices", inspected_devices_for(spec)}}},
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

  void expect_inventory_rejected(
    json container,
    std::function<void(fake_host_t &)> configure_host = {}
  ) {
    fake_host_t host;
    if (configure_host) {
      configure_host(host);
    }
    fake_input_manifest_source_t inputs;
    backend_t backend {host, inputs, options_for_tests()};
    queue_inventory(host, {std::move(container)});
    EXPECT_THROW(backend.inventory(), std::runtime_error);
  }
}  // namespace

TEST(MultiseatPodmanBackend, RejectsUnpinnedImagesAndDuplicateProfileVolumes) {
  fake_host_t host;
  auto options = options_for_tests();
  options.profiles.at(0).image_reference = "ghcr.io/papi-ux/polaris-seat:latest";
  EXPECT_THROW(backend_t(host, input_manifests_for_tests(), options), std::invalid_argument);

  options = options_for_tests();
  options.profiles.at(0).image_reference = std::string {"dir:/tmp/worker@sha256:"} +
                                           std::string(64, 'a');
  EXPECT_THROW(backend_t(host, input_manifests_for_tests(), options), std::invalid_argument);

  options = options_for_tests();
  options.profiles.at(0).runtime_profile = runtime_profile_e::unknown;
  EXPECT_THROW(backend_t(host, input_manifests_for_tests(), options), std::invalid_argument);

  options = options_for_tests();
  options.profiles.at(1).opaque_volume_name = options.profiles.at(0).opaque_volume_name;
  EXPECT_THROW(backend_t(host, input_manifests_for_tests(), options), std::invalid_argument);

  options = options_for_tests();
  options.ipc_root = "/run/user/1000/../polaris-workers";
  EXPECT_THROW(backend_t(host, input_manifests_for_tests(), options), std::invalid_argument);

  options = options_for_tests();
  options.workloads.push_back(options.workloads.front());
  EXPECT_THROW(backend_t(host, input_manifests_for_tests(), options), std::invalid_argument);

  options = options_for_tests();
  options.workloads.front().target_id = "game;$(command)";
  EXPECT_THROW(backend_t(host, input_manifests_for_tests(), options), std::invalid_argument);
}

TEST(MultiseatPodmanBackend, InputManifestFingerprintBindsGenerationAndIdentity) {
  const auto first = input_allocation_for(valid_spec().identity.seat);
  const auto repeated = multiseat::podman::input_manifest_fingerprint(first);
  ASSERT_TRUE(repeated);
  EXPECT_EQ(multiseat::podman::input_manifest_fingerprint(first), repeated);

  const auto second = input_allocation_for(valid_spec(
    1,
    2,
    "polaris-worker-controller-a1b2-2",
    "profile beta",
    "heroic-game"
  ).identity.seat);
  const auto second_fingerprint = multiseat::podman::input_manifest_fingerprint(second);
  ASSERT_TRUE(second_fingerprint);
  EXPECT_NE(*second_fingerprint, *repeated);

  auto changed_identity = first;
  changed_identity.nodes.front().inode += 1000;
  const auto changed_fingerprint =
    multiseat::podman::input_manifest_fingerprint(changed_identity);
  ASSERT_TRUE(changed_fingerprint);
  EXPECT_NE(*changed_fingerprint, *repeated);

  auto malformed = first;
  malformed.nodes.front().worker_path = "/dev/input/polaris-gamepad-15";
  EXPECT_FALSE(multiseat::podman::input_manifest_fingerprint(malformed));
}

TEST(MultiseatPodmanBackend, AuthorityManifestSourceUsesExactAllocationAndProbe) {
  const auto allocation = input_allocation_for(valid_spec().identity.seat);
  fixed_input_backend_t lifecycle {allocation};
  multiseat::input::authority_t authority {lifecycle};
  const multiseat::input::expectation_t expectation {
    .handle = allocation.handle,
    .input_seat = allocation.input_seat,
    .plan = allocation.plan,
  };
  const auto report = authority.reconcile({expectation});
  ASSERT_TRUE(report.inventory_authoritative);
  ASSERT_TRUE(report.admission_ready);
  fixed_kernel_probe_t probe {allocation};
  multiseat::podman::authority_input_manifest_source_t source {
    authority,
    probe,
  };

  const auto observed_allocation = source.allocation(allocation.handle);
  ASSERT_TRUE(observed_allocation);
  EXPECT_EQ(*observed_allocation, allocation);
  const auto observed_node = source.observe(allocation.nodes.front().host_path);
  ASSERT_TRUE(observed_node.snapshot);
  EXPECT_EQ(observed_node.snapshot->inode, allocation.nodes.front().inode);
}

TEST(MultiseatPodmanBackend, LaunchRequiresExactInputAuthorityBeforeCommand) {
  fake_host_t missing_host;
  fake_input_manifest_source_t missing_inputs;
  missing_inputs.missing = true;
  backend_t missing_backend {missing_host, missing_inputs, options_for_tests()};
  EXPECT_EQ(missing_backend.launch(valid_spec()), worker_command_result_e::rejected);
  EXPECT_TRUE(missing_host.calls.empty());

  fake_host_t stale_host;
  fake_input_manifest_source_t stale_inputs;
  stale_inputs.replacement_after_call = 1;
  stale_inputs.replacement = input_allocation_for(valid_spec(
    1,
    2,
    "polaris-worker-controller-a1b2-2",
    "profile beta",
    "heroic-game"
  ).identity.seat);
  backend_t stale_backend {stale_host, stale_inputs, options_for_tests()};
  EXPECT_EQ(stale_backend.launch(valid_spec()), worker_command_result_e::rejected);
  EXPECT_TRUE(stale_host.calls.empty());

  fake_host_t failed_host;
  fake_input_manifest_source_t failed_inputs;
  failed_inputs.throw_on_allocation_call = 1;
  backend_t failed_backend {failed_host, failed_inputs, options_for_tests()};
  EXPECT_EQ(failed_backend.launch(valid_spec()), worker_command_result_e::indeterminate);
  EXPECT_TRUE(failed_host.calls.empty());
}

TEST(MultiseatPodmanBackend, LaunchRevalidatesIdentityAtInvocationBoundary) {
  const auto spec = valid_spec();
  const auto allocation = input_allocation_for(spec.identity.seat);

  fake_host_t replaced_path_host;
  ++replaced_path_host.accessible_devices["/dev/input/event10"].inode;
  fake_input_manifest_source_t replaced_path_inputs;
  backend_t replaced_path_backend {
    replaced_path_host,
    replaced_path_inputs,
    options_for_tests(),
  };
  EXPECT_EQ(replaced_path_backend.launch(spec), worker_command_result_e::rejected);
  EXPECT_TRUE(replaced_path_host.calls.empty());

  fake_host_t changed_authority_host;
  fake_input_manifest_source_t changed_authority;
  auto replacement = allocation;
  replacement.nodes.front().inode += 1000;
  changed_authority.replacement_after_call = 3;
  changed_authority.replacement = replacement;
  backend_t changed_authority_backend {
    changed_authority_host,
    changed_authority,
    options_for_tests(),
  };
  EXPECT_EQ(
    changed_authority_backend.launch(spec),
    worker_command_result_e::rejected
  );
  EXPECT_TRUE(changed_authority_host.calls.empty());

  fake_host_t changed_kernel_host;
  fake_input_manifest_source_t changed_kernel;
  auto changed_observation = observation_for(allocation.nodes.front());
  ++changed_observation.snapshot->inode;
  changed_kernel.observations[allocation.nodes.front().host_path.native()] = {
    observation_for(allocation.nodes.front()),
    changed_observation,
  };
  backend_t changed_kernel_backend {
    changed_kernel_host,
    changed_kernel,
    options_for_tests(),
  };
  EXPECT_EQ(changed_kernel_backend.launch(spec), worker_command_result_e::rejected);
  EXPECT_TRUE(changed_kernel_host.calls.empty());
  EXPECT_EQ(changed_kernel.observation_calls, std::size_t {5});
}

TEST(MultiseatPodmanBackend, LaunchBuildsRootlessIsolatedArgumentVector) {
  fake_host_t host;
  backend_t backend {host, input_manifests_for_tests(), options_for_tests()};
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
  EXPECT_TRUE(has_argument(
    argv,
    "--device=/dev/input/event10:/dev/input/polaris-keyboard:rw"
  ));
  EXPECT_TRUE(has_argument(
    argv,
    "--device=/dev/input/event11:/dev/input/polaris-mouse-relative:rw"
  ));
  EXPECT_TRUE(has_argument(
    argv,
    "--device=/dev/input/event12:/dev/input/polaris-mouse-absolute:rw"
  ));
  EXPECT_TRUE(has_argument(
    argv,
    "--device=/dev/input/event13:/dev/input/polaris-gamepad-0:rw"
  ));
  EXPECT_TRUE(has_argument(argv, "--env=WAYLAND_DISPLAY=polaris-wayland-controller-a1b2-1"));
  EXPECT_TRUE(has_argument(argv, "--env=POLARIS_CAPTURE_WAYLAND_DISPLAY=polaris-capture-controller-a1b2-1"));
  EXPECT_TRUE(has_argument(argv, "--env=PULSE_SINK=polaris-audio-controller-a1b2-1"));
  EXPECT_TRUE(has_argument(argv, "--env=POLARIS_INPUT_SEAT=polaris-input-controller-a1b2-1"));
  const auto input_fingerprint = multiseat::podman::input_manifest_fingerprint(
    input_allocation_for(spec.identity.seat)
  );
  ASSERT_TRUE(input_fingerprint);
  EXPECT_TRUE(has_argument(
    argv,
    "--label=io.polaris.multiseat.input-manifest=" + *input_fingerprint
  ));
  EXPECT_TRUE(has_argument(argv, "--env=POLARIS_WORKER_NAME=polaris-worker-controller-a1b2-1"));
  EXPECT_TRUE(has_argument(argv, "--env=POLARIS_COMPOSITOR=gamescope"));
  EXPECT_TRUE(has_argument(argv, "--env=POLARIS_RUNTIME_PROFILE=steam"));
  EXPECT_TRUE(has_argument(argv, "--env=POLARIS_DISPLAY_TOPOLOGY=capture-host-with-nested-compositor"));
  EXPECT_TRUE(has_argument(argv, "--env=POLARIS_MEDIA_PIPELINE=worker-local-capture-encode"));
  EXPECT_TRUE(has_argument(argv, "--env=POLARIS_DISPLAY_WIDTH=3840"));
  EXPECT_TRUE(has_argument(argv, "--env=POLARIS_DISPLAY_HEIGHT=2160"));
  EXPECT_TRUE(has_argument(argv, "--env=POLARIS_DISPLAY_REFRESH_MILLIHZ=97000"));
  EXPECT_TRUE(has_argument(argv, "--env=POLARIS_DISPLAY_HDR=1"));
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
  EXPECT_TRUE(has_argument(argv, "--workload-kind=steam"));
  EXPECT_TRUE(has_argument(argv, "--workload-id=steam-game"));
  EXPECT_TRUE(has_argument(
    argv,
    std::string {"ghcr.io/papi-ux/polaris-seat-steam@sha256:"} + std::string(64, 'a')
  ));
  EXPECT_TRUE(any_argument_contains(argv, "--health-cmd=[\"/usr/bin/polaris-seat-worker\",\"health\"]"));
  EXPECT_FALSE(any_argument_contains(argv, "profile alpha"));
  EXPECT_FALSE(any_argument_contains(argv, "--privileged"));
  EXPECT_FALSE(any_argument_contains(argv, "--network=host"));
  EXPECT_FALSE(any_argument_contains(argv, "--pid=host"));
  EXPECT_FALSE(any_argument_contains(argv, "docker.sock"));
  EXPECT_FALSE(any_argument_contains(argv, "/dev/uinput"));
  EXPECT_FALSE(any_argument_contains(argv, "/dev/uhid"));
  EXPECT_FALSE(has_argument(argv, "--device=/dev/input:/dev/input:rw"));
  EXPECT_FALSE(any_argument_contains(argv, "keep-groups"));
  EXPECT_FALSE(any_argument_contains(argv, "POLARIS_AUTH_TOKEN"));
  EXPECT_FALSE(any_argument_contains(argv, std::string(64, '0')));
}

TEST(MultiseatPodmanBackend, ProfileSelectsOneExactRuntimeImageAndTypedWorkerProfile) {
  fake_host_t host;
  backend_t backend {host, input_manifests_for_tests(), options_for_tests()};
  const auto spec = valid_spec(
    1,
    2,
    "polaris-worker-controller-a1b2-2",
    "profile beta",
    "heroic-game"
  );
  host.push({
    .exit_status = 0,
    .output = std::string {second_id} + "\n",
  });

  ASSERT_EQ(backend.launch(spec), worker_command_result_e::applied);
  ASSERT_EQ(host.calls.size(), std::size_t {1});
  const auto &argv = host.calls.front();
  EXPECT_TRUE(has_argument(argv, "--env=POLARIS_RUNTIME_PROFILE=heroic"));
  EXPECT_TRUE(has_argument(
    argv,
    std::string {"ghcr.io/papi-ux/polaris-seat-heroic@sha256:"} + std::string(64, 'b')
  ));
  EXPECT_FALSE(has_argument(
    argv,
    std::string {"ghcr.io/papi-ux/polaris-seat-steam@sha256:"} + std::string(64, 'a')
  ));
  EXPECT_TRUE(has_argument(
    argv,
    "--device=/dev/input/event20:/dev/input/polaris-keyboard:rw"
  ));
  EXPECT_FALSE(any_argument_contains(argv, "/dev/input/event10"));
  EXPECT_FALSE(any_argument_contains(argv, "profile beta"));
}

TEST(MultiseatPodmanBackend, LaunchRejectsPrivilegedOrInaccessibleHostsBeforeCommand) {
  fake_host_t host;
  backend_t backend {host, input_manifests_for_tests(), options_for_tests()};
  host.uid = 0;
  EXPECT_EQ(backend.launch(valid_spec()), worker_command_result_e::rejected);
  EXPECT_TRUE(host.calls.empty());

  host.uid = 1000;
  host.accessible_devices.erase("/dev/input/event11");
  EXPECT_EQ(backend.launch(valid_spec()), worker_command_result_e::rejected);
  EXPECT_TRUE(host.calls.empty());

  host.accessible_devices["/dev/input/event11"] = device_identity(13, 75);
  host.private_files.clear();
  EXPECT_EQ(backend.launch(valid_spec()), worker_command_result_e::rejected);
  EXPECT_TRUE(host.calls.empty());
}

TEST(MultiseatPodmanBackend, LaunchRejectsUnknownOrNonConcreteAllocation) {
  fake_host_t host;
  backend_t backend {host, input_manifests_for_tests(), options_for_tests()};

  auto spec = valid_spec();
  spec.profile_key = "unknown profile";
  EXPECT_EQ(backend.launch(spec), worker_command_result_e::rejected);

  spec = valid_spec();
  spec.workload.target_id = "unknown-workload";
  EXPECT_EQ(backend.launch(spec), worker_command_result_e::rejected);

  spec = valid_spec();
  spec.workload.kind = workload_kind_e::heroic;
  EXPECT_EQ(backend.launch(spec), worker_command_result_e::rejected);

  spec = valid_spec();
  spec.compositor = compositor_e::automatic;
  EXPECT_EQ(backend.launch(spec), worker_command_result_e::rejected);

  spec = valid_spec();
  spec.render_node = "/dev/dri/renderD129";
  EXPECT_EQ(backend.launch(spec), worker_command_result_e::rejected);

  spec = valid_spec();
  spec.runtime_profile = runtime_profile_e::heroic;
  EXPECT_EQ(backend.launch(spec), worker_command_result_e::rejected);

  spec = valid_spec();
  spec.data_plane.media_pipeline = media_pipeline_e::unknown;
  EXPECT_EQ(backend.launch(spec), worker_command_result_e::rejected);

  spec = valid_spec();
  spec.resources.capture_wayland_socket = "../capture";
  EXPECT_EQ(backend.launch(spec), worker_command_result_e::rejected);

  spec = valid_spec();
  spec.display_mode.refresh_millihz = 0;
  EXPECT_EQ(backend.launch(spec), worker_command_result_e::rejected);
  EXPECT_TRUE(host.calls.empty());
}

TEST(MultiseatPodmanBackend, TimedOutLaunchIsIndeterminate) {
  fake_host_t host;
  backend_t backend {host, input_manifests_for_tests(), options_for_tests()};
  host.push({
    .exit_status = 124,
    .timed_out = true,
  });

  EXPECT_EQ(backend.launch(valid_spec()), worker_command_result_e::indeterminate);
  EXPECT_EQ(host.calls.size(), std::size_t {1});
}

TEST(MultiseatPodmanBackend, AmbiguousSuccessfulLaunchOutputIsIndeterminate) {
  fake_host_t host;
  backend_t backend {host, input_manifests_for_tests(), options_for_tests()};
  host.push({
    .exit_status = 0,
    .output = std::string {first_id} + "\nunexpected output\n",
  });

  EXPECT_EQ(backend.launch(valid_spec()), worker_command_result_e::indeterminate);
  EXPECT_EQ(host.calls.size(), std::size_t {1});
}

TEST(MultiseatPodmanBackend, NonzeroLaunchReconcilesAnExactExistingWorker) {
  fake_host_t host;
  backend_t backend {host, input_manifests_for_tests(), options_for_tests()};
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
  backend_t missing_backend {missing_host, input_manifests_for_tests(), options_for_tests()};
  missing_host.push({.exit_status = 125});
  queue_inventory(missing_host, {});
  EXPECT_EQ(missing_backend.launch(spec), worker_command_result_e::rejected);
  EXPECT_EQ(missing_host.calls.size(), std::size_t {2});

  fake_host_t mismatch_host;
  backend_t mismatch_backend {mismatch_host, input_manifests_for_tests(), options_for_tests()};
  auto mismatched = container_for(spec, first_id, "running", "healthy");
  mismatched["Config"]["Labels"]["io.polaris.multiseat.runtime"] =
    "polaris-runtime-controller-a1b2-wrong";
  mismatch_host.push({.exit_status = 125});
  queue_inventory(mismatch_host, {std::move(mismatched)});
  EXPECT_EQ(mismatch_backend.launch(spec), worker_command_result_e::indeterminate);
  EXPECT_EQ(mismatch_host.calls.size(), std::size_t {3});

  fake_host_t failed_host;
  backend_t failed_backend {failed_host, input_manifests_for_tests(), options_for_tests()};
  failed_host.push({.exit_status = 125});
  queue_inventory(failed_host, {container_for(spec, first_id, "running", "unhealthy")});
  EXPECT_EQ(failed_backend.launch(spec), worker_command_result_e::indeterminate);

  fake_host_t stopped_host;
  backend_t stopped_backend {stopped_host, input_manifests_for_tests(), options_for_tests()};
  stopped_host.push({.exit_status = 125});
  queue_inventory(stopped_host, {container_for(spec, first_id, "exited", "")});
  EXPECT_EQ(stopped_backend.launch(spec), worker_command_result_e::rejected);
}

TEST(MultiseatPodmanBackend, NonzeroLaunchQuarantinesChangedRuntimeBindings) {
  struct mismatch_t {
    const char *label;
    std::string value;
  };
  const std::vector<mismatch_t> mismatches {
    {"io.polaris.multiseat.runtime-profile", "heroic"},
    {"io.polaris.multiseat.workload-kind", "heroic"},
    {"io.polaris.multiseat.workload-target", "other-game"},
    {"io.polaris.multiseat.display-topology", "nested-only"},
    {"io.polaris.multiseat.media-pipeline", "controller-encode"},
    {"io.polaris.multiseat.capture-wayland", "other-capture"},
    {
      "io.polaris.multiseat.runtime-image",
      std::string {"ghcr.io/papi-ux/polaris-seat-steam@sha256:"} + std::string(64, 'c'),
    },
    {"io.polaris.multiseat.display-width", "1920"},
    {"io.polaris.multiseat.display-height", "1080"},
    {"io.polaris.multiseat.display-refresh-millihz", "60000"},
    {"io.polaris.multiseat.display-hdr", "0"},
  };

  for (const auto &mismatch : mismatches) {
    SCOPED_TRACE(mismatch.label);
    fake_host_t host;
    backend_t backend {host, input_manifests_for_tests(), options_for_tests()};
    const auto spec = valid_spec();
    auto existing = container_for(spec, first_id, "running", "healthy");
    existing["Config"]["Labels"][mismatch.label] = mismatch.value;
    host.push({.exit_status = 125});
    queue_inventory(host, {std::move(existing)});

    EXPECT_EQ(backend.launch(spec), worker_command_result_e::indeterminate);
    EXPECT_EQ(host.calls.size(), std::size_t {3});
  }
}

TEST(MultiseatPodmanBackend, InventoryRejectsChangedInputManifestAndBindings) {
  const auto spec = valid_spec();

  auto changed_fingerprint = container_for(spec, first_id, "running", "healthy");
  changed_fingerprint["Config"]["Labels"]["io.polaris.multiseat.input-manifest"] =
    std::string(64, '0');
  expect_inventory_rejected(std::move(changed_fingerprint));

  auto missing_binding = container_for(spec, first_id, "running", "healthy");
  missing_binding["HostConfig"]["Devices"].erase(
    missing_binding["HostConfig"]["Devices"].end() - 1
  );
  expect_inventory_rejected(std::move(missing_binding));

  auto changed_alias = container_for(spec, first_id, "running", "healthy");
  changed_alias["HostConfig"]["Devices"][2]["PathInContainer"] =
    "/dev/input/polaris-keyboard-other";
  expect_inventory_rejected(std::move(changed_alias));

  auto changed_device = container_for(spec, first_id, "running", "healthy");
  changed_device["HostConfig"]["Devices"][2]["PathOnHost"] =
    "/dev/input/event20";
  expect_inventory_rejected(std::move(changed_device));

  auto broadened_permissions = container_for(spec, first_id, "running", "healthy");
  broadened_permissions["HostConfig"]["Devices"][2]["CgroupPermissions"] = "rwm";
  expect_inventory_rejected(std::move(broadened_permissions));

  auto raw_creation_endpoint = container_for(spec, first_id, "running", "healthy");
  raw_creation_endpoint["HostConfig"]["Devices"][2]["PathOnHost"] = "/dev/uinput";
  expect_inventory_rejected(
    std::move(raw_creation_endpoint),
    [](fake_host_t &host) {
      host.accessible_devices["/dev/uinput"] = device_identity(10, 223);
    }
  );

  auto missing_host_config = container_for(spec, first_id, "running", "healthy");
  missing_host_config.erase("HostConfig");
  expect_inventory_rejected(std::move(missing_host_config));
}

TEST(MultiseatPodmanBackend, InventoryAcceptsEquivalentReconstructedHostNode) {
  fake_host_t host;
  host.accessible_devices["/dev/input/reconstructed-event10"] = {
    .filesystem_device = 2,
    .inode = 20000,
    .character_major = 13,
    .character_minor = 74,
  };
  fake_input_manifest_source_t inputs;
  backend_t backend {host, inputs, options_for_tests()};
  const auto spec = valid_spec();
  auto container = container_for(spec, first_id, "running", "healthy");
  container["HostConfig"]["Devices"][2]["PathOnHost"] =
    "/dev/input/reconstructed-event10";
  queue_inventory(host, {std::move(container)});

  const auto observations = backend.inventory();
  ASSERT_EQ(observations.size(), std::size_t {1});
  EXPECT_EQ(observations.front().identity, spec.identity);
  EXPECT_EQ(observations.front().state, worker_observed_state_e::ready);
}

TEST(MultiseatPodmanBackend, InventoryParsesIndependentWorkerHealthAndIdentity) {
  fake_host_t host;
  backend_t backend {host, input_manifests_for_tests(), options_for_tests()};
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
  backend_t backend {host, input_manifests_for_tests(), options_for_tests()};
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
  backend_t truncated_backend {truncated_host, input_manifests_for_tests(), options_for_tests()};
  truncated_host.push({
    .exit_status = 0,
    .output_truncated = true,
    .output = first_id,
  });
  EXPECT_THROW(truncated_backend.inventory(), std::runtime_error);

  fake_host_t malformed_host;
  backend_t malformed_backend {malformed_host, input_manifests_for_tests(), options_for_tests()};
  auto malformed = container_for(valid_spec(), first_id, "running", "healthy");
  malformed["Config"]["Labels"].erase("io.polaris.multiseat.generation");
  queue_inventory(malformed_host, {std::move(malformed)});
  EXPECT_THROW(malformed_backend.inventory(), std::runtime_error);

  auto old_protocol = container_for(valid_spec(), first_id, "running", "healthy");
  old_protocol["Config"]["Labels"]["io.polaris.multiseat.protocol"] = "2";
  expect_inventory_rejected(std::move(old_protocol));
}

TEST(MultiseatPodmanBackend, InventoryRejectsMalformedRuntimeBindingLabels) {
  struct malformed_t {
    const char *label;
    std::string value;
  };
  const std::vector<malformed_t> malformed_values {
    {"io.polaris.multiseat.runtime-profile", "automatic"},
    {"io.polaris.multiseat.workload-kind", "shell"},
    {"io.polaris.multiseat.workload-target", "game;command"},
    {"io.polaris.multiseat.display-topology", "nested-only"},
    {"io.polaris.multiseat.media-pipeline", "controller-encode"},
    {"io.polaris.multiseat.capture-wayland", "../capture"},
    {"io.polaris.multiseat.runtime-image", "ghcr.io/papi-ux/polaris-seat:latest"},
    {"io.polaris.multiseat.display-width", "0"},
    {"io.polaris.multiseat.display-height", "16385"},
    {"io.polaris.multiseat.display-refresh-millihz", "999"},
    {"io.polaris.multiseat.display-hdr", "true"},
  };

  for (const auto &malformed_value : malformed_values) {
    SCOPED_TRACE(malformed_value.label);
    fake_host_t host;
    backend_t backend {host, input_manifests_for_tests(), options_for_tests()};
    auto malformed = container_for(valid_spec(), first_id, "running", "healthy");
    malformed["Config"]["Labels"][malformed_value.label] = malformed_value.value;
    queue_inventory(host, {std::move(malformed)});

    EXPECT_THROW(backend.inventory(), std::runtime_error);
  }
}

TEST(MultiseatPodmanBackend, InventoryRejectsIdCardinalityAndBoundViolations) {
  const auto spec = valid_spec();

  fake_host_t duplicate_host;
  backend_t duplicate_backend {duplicate_host, input_manifests_for_tests(), options_for_tests()};
  duplicate_host.push({
    .exit_status = 0,
    .output = std::string {first_id} + "\n" + first_id + "\n",
  });
  EXPECT_THROW(duplicate_backend.inventory(), std::runtime_error);
  EXPECT_EQ(duplicate_host.calls.size(), std::size_t {1});

  fake_host_t cardinality_host;
  backend_t cardinality_backend {cardinality_host, input_manifests_for_tests(), options_for_tests()};
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
  backend_t mismatched_id_backend {mismatched_id_host, input_manifests_for_tests(), options_for_tests()};
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
  backend_t bounded_backend {bounded_host, input_manifests_for_tests(), std::move(bounded_options)};
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
  backend_t graceful_backend {graceful_host, input_manifests_for_tests(), options_for_tests()};
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
  backend_t force_backend {force_host, input_manifests_for_tests(), options_for_tests()};
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
  backend_t backend {host, input_manifests_for_tests(), options_for_tests()};
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

TEST(MultiseatPodmanBackend, StopRemainsAvailableAfterInputAuthorityIsGone) {
  fake_host_t host;
  fake_input_manifest_source_t inputs;
  inputs.missing = true;
  backend_t backend {host, inputs, options_for_tests()};
  const auto spec = valid_spec();
  queue_inventory(host, {container_for(spec, first_id, "running", "healthy")});
  host.push({.exit_status = 0});

  EXPECT_EQ(
    backend.stop(spec.identity, worker_stop_mode_e::graceful),
    worker_command_result_e::applied
  );
  ASSERT_EQ(host.calls.size(), std::size_t {3});
  EXPECT_EQ(host.calls.back().at(2), "kill");
  EXPECT_EQ(inputs.allocation_calls, std::size_t {0});
}

TEST(MultiseatPodmanBackend, MissingExactWorkerReturnsNotFoundWithoutStopCommand) {
  fake_host_t host;
  backend_t backend {host, input_manifests_for_tests(), options_for_tests()};
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
  backend_t backend {host, input_manifests_for_tests(), options_for_tests()};
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
  backend_t backend {host, input_manifests_for_tests(), options_for_tests()};

  EXPECT_THROW(backend.inventory(), std::runtime_error);
  EXPECT_TRUE(host.calls.empty());
}

#endif
