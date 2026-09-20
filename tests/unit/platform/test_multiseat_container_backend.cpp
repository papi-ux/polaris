/**
 * @file tests/unit/platform/test_multiseat_container_backend.cpp
 * @brief Offline contract tests for the rootless Podman worker backend.
 */
#include "src/platform/linux/multiseat_container_backend.h"
#include "src/platform/linux/multiseat_profile_network.h"

#ifdef __linux__

#include "multiseat_steam_seccomp.h"
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
#include <string_view>
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
  using multiseat::container::backend_t;
  using multiseat::container::character_device_identity_t;
  using multiseat::container::command_result_t;
  using multiseat::container::gpu_device_t;
  using multiseat::container::gpu_t;
  using multiseat::container::host_t;
  using multiseat::container::input_manifest_source_t;
  using multiseat::container::options_t;
  using multiseat::container::profile_t;
  using multiseat::container::shared_game_mount_t;

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

  gpu_device_t admitted_gpu_device(
    std::filesystem::path path,
    std::uint32_t major,
    std::uint32_t minor
  ) {
    return {
      .path = std::move(path),
      .admitted_identity = device_identity(major, minor),
    };
  }

  class fake_host_t final : public host_t {
  public:
    fake_host_t();

    std::uint64_t effective_uid() const override {
      return uid;
    }

    bool executable_file(const std::filesystem::path &path) const override {
      return executable_ready && executable_files.contains(path.native());
    }

    bool trusted_runtime_file(const std::filesystem::path &path) const override {
      return runtime_ready && (path == "/usr/bin/crun" || path == "/usr/bin/runc");
    }

    bool seccomp_ready = true;
    bool trusted_data_file(const std::filesystem::path &path, std::string_view expected) const override {
      return seccomp_ready && path == multiseat::container::steam_seccomp_path &&
             expected == multiseat::container::steam_seccomp_data;
    }

    std::optional<std::vector<std::uint64_t>> supplementary_groups() const override {
      ++group_reads;
      if (groups_change_on_recheck && group_reads > 1) return std::vector<std::uint64_t> {};
      return groups;
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
      ++character_device_calls;
      if (replacement_after_character_device_call != 0 &&
          character_device_calls >= replacement_after_character_device_call &&
          path == replacement_character_device_path &&
          replacement_character_device_identity) {
        return replacement_character_device_identity;
      }
      const auto device = accessible_devices.find(path.native());
      return device == accessible_devices.end() ?
               std::nullopt : std::optional {device->second};
    }

    std::optional<std::string> read_owned_regular_file(
      const std::filesystem::path &path,
      std::size_t max_bytes
    ) const override {
      ++owned_file_reads;
      last_owned_file_limit = max_bytes;
      const auto file = owned_files.find(path.native());
      if (file == owned_files.end() || file->second.size() > max_bytes) {
        return std::nullopt;
      }
      return file->second;
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
    bool runtime_ready = true;
    bool groups_change_on_recheck = false;
    mutable int group_reads = 0;
    std::optional<std::vector<std::uint64_t>> groups = std::vector<std::uint64_t> {39, 104, 105};
    std::set<std::string> executable_files {
      "/usr/bin/podman",
      "/usr/bin/docker",
      "/usr/libexec/podman/catatonit",
    };
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
    mutable std::size_t character_device_calls = 0;
    std::map<std::string, std::string> owned_files;
    mutable std::size_t owned_file_reads = 0;
    mutable std::size_t last_owned_file_limit = 0;
    std::size_t replacement_after_character_device_call = 0;
    std::filesystem::path replacement_character_device_path;
    std::optional<character_device_identity_t>
      replacement_character_device_identity;
    std::deque<command_result_t> results;
    std::vector<std::vector<std::string>> calls;
  };

  options_t options_for_tests() {
    return {
      .engine = multiseat::container::engine_e::podman,
      .executable = "/usr/bin/podman",
      .runtime_executable = "/usr/bin/crun",
      .deployment_id = "deployment-a1b2",
      .worker_entrypoint = "/usr/bin/polaris-seat-worker",
      .ipc_root = "/run/user/1000/polaris-workers",
      .gpus = {
        gpu_t {
          .logical_gpu_id = "gpu-primary",
          .render_node = "/dev/dri/renderD128",
          .devices = {
            admitted_gpu_device("/dev/dri/renderD128", 226, 128),
            admitted_gpu_device("/dev/dri/card0", 226, 0),
          },
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
    const auto profile = spec.runtime_profile == runtime_profile_e::gamescope ? "gamescope" :
                           spec.runtime_profile == runtime_profile_e::heroic ? "heroic" : "steam";
    const auto workload_kind = spec.workload.kind == workload_kind_e::gamescope ? "gamescope" :
                               spec.workload.kind == workload_kind_e::heroic ?
                                 "heroic" : "steam";
    const auto input_fingerprint = multiseat::container::input_manifest_fingerprint(
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
      {"io.polaris.multiseat.volume", spec.profile_key == "profile beta" ? "pv-b8e1" : "pv-a9f0"},
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

  std::string runtime_spec_path_for(std::string_view id);

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
      {"OCIConfigPath", runtime_spec_path_for(id)},
      {"Id", std::move(id)},
      {"Name", spec.identity.worker_name},
      {"Config", {{"Labels", labels_for(spec)}, {"User", "1000"}}},
      {"OCIRuntime", "/usr/bin/crun"},
      {"HostConfig", {{"Devices", inspected_devices_for(spec)}, {"GroupAdd", json::array()}}},
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

  options = options_for_tests();
  options.gpus.front().devices.at(1).admitted_identity =
    options.gpus.front().devices.front().admitted_identity;
  EXPECT_THROW(backend_t(host, input_manifests_for_tests(), options), std::invalid_argument);
}

TEST(MultiseatPodmanBackend, InputManifestFingerprintBindsGenerationAndIdentity) {
  const auto first = input_allocation_for(valid_spec().identity.seat);
  const auto repeated = multiseat::container::input_manifest_fingerprint(first);
  ASSERT_TRUE(repeated);
  EXPECT_EQ(multiseat::container::input_manifest_fingerprint(first), repeated);

  const auto second = input_allocation_for(valid_spec(
    1,
    2,
    "polaris-worker-controller-a1b2-2",
    "profile beta",
    "heroic-game"
  ).identity.seat);
  const auto second_fingerprint = multiseat::container::input_manifest_fingerprint(second);
  ASSERT_TRUE(second_fingerprint);
  EXPECT_NE(*second_fingerprint, *repeated);

  auto changed_identity = first;
  changed_identity.nodes.front().inode += 1000;
  const auto changed_fingerprint =
    multiseat::container::input_manifest_fingerprint(changed_identity);
  ASSERT_TRUE(changed_fingerprint);
  EXPECT_NE(*changed_fingerprint, *repeated);

  auto malformed = first;
  malformed.nodes.front().worker_path = "/dev/input/polaris-gamepad-15";
  EXPECT_FALSE(multiseat::container::input_manifest_fingerprint(malformed));
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
  multiseat::container::authority_input_manifest_source_t source {
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
  changed_authority_host.push({.exit_status = 0});
  EXPECT_EQ(
    changed_authority_backend.launch(spec),
    worker_command_result_e::rejected
  );
  // The authority changed at the recheck that follows the volume check.
  ASSERT_EQ(changed_authority_host.calls.size(), std::size_t {1});
  EXPECT_EQ(changed_authority_host.calls.front().at(2), "volume");

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
  changed_kernel_host.push({.exit_status = 0});
  EXPECT_EQ(changed_kernel_backend.launch(spec), worker_command_result_e::rejected);
  ASSERT_EQ(changed_kernel_host.calls.size(), std::size_t {1});
  EXPECT_EQ(changed_kernel_host.calls.front().at(2), "volume");
  EXPECT_EQ(changed_kernel.observation_calls, std::size_t {5});

  fake_host_t changed_gpu_at_run_host;
  changed_gpu_at_run_host.replacement_after_character_device_call = 7;
  changed_gpu_at_run_host.replacement_character_device_path =
    "/dev/dri/renderD128";
  changed_gpu_at_run_host.replacement_character_device_identity =
    device_identity(226, 0);
  fake_input_manifest_source_t changed_gpu_at_run_inputs;
  backend_t changed_gpu_at_run_backend {
    changed_gpu_at_run_host,
    changed_gpu_at_run_inputs,
    options_for_tests(),
  };
  changed_gpu_at_run_host.push({.exit_status = 0});
  EXPECT_EQ(
    changed_gpu_at_run_backend.launch(spec),
    worker_command_result_e::rejected
  );
  // Only the volume existence check ran; the run itself never did.
  ASSERT_EQ(changed_gpu_at_run_host.calls.size(), std::size_t {1});
  EXPECT_EQ(changed_gpu_at_run_host.calls.front().at(2), "volume");
  EXPECT_GE(changed_gpu_at_run_host.character_device_calls, 7U);
}

TEST(MultiseatPodmanBackend, LaunchBuildsRootlessIsolatedArgumentVector) {
  fake_host_t host;
  backend_t backend {host, input_manifests_for_tests(), options_for_tests()};
  const auto spec = valid_spec();
  host.push({.exit_status = 0});
  host.push({
    .exit_status = 0,
    .output = std::string {first_id} + "\n",
  });

  ASSERT_EQ(backend.launch(spec), worker_command_result_e::applied);
  ASSERT_EQ(host.calls.size(), std::size_t {2});
  const std::vector<std::string> expected_volume_check {
    "/usr/bin/podman", "--remote=false", "volume", "exists", "pv-a9f0",
  };
  EXPECT_EQ(host.calls.front(), expected_volume_check);
  const auto &argv = host.calls.at(1);
  ASSERT_GE(argv.size(), std::size_t {3});
  EXPECT_TRUE(has_argument(
    argv,
    "--volume=pv-a9f0:/var/lib/polaris-seat:rw,nosuid,nodev"
  ));
  EXPECT_FALSE(any_argument_contains(argv, "nocreate"));
  EXPECT_EQ(argv.at(0), "/usr/bin/podman");
  EXPECT_EQ(argv.at(1), "--remote=false");
  EXPECT_EQ(argv.at(2), "--runtime=/usr/bin/crun");
  EXPECT_EQ(argv.at(3), "run");
  EXPECT_TRUE(has_argument(argv, "--group-add=keep-groups"));
  EXPECT_TRUE(has_argument(argv, "--userns=keep-id"));
  EXPECT_TRUE(has_argument(argv, "--user=1000"));
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
  const auto input_fingerprint = multiseat::container::input_manifest_fingerprint(
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
  EXPECT_TRUE(has_argument(argv, "--group-add=keep-groups"));
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
  host.push({.exit_status = 0});
  host.push({
    .exit_status = 0,
    .output = std::string {second_id} + "\n",
  });

  ASSERT_EQ(backend.launch(spec), worker_command_result_e::applied);
  ASSERT_EQ(host.calls.size(), std::size_t {2});
  EXPECT_EQ(host.calls.front().at(4), "pv-b8e1");
  const auto &argv = host.calls.at(1);
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

TEST(MultiseatPodmanBackend, LaunchRequiresPreexistingProfileVolume) {
  const auto spec = valid_spec();

  fake_host_t absent_host;
  backend_t absent_backend {absent_host, input_manifests_for_tests(), options_for_tests()};
  absent_host.push({.exit_status = 1});
  EXPECT_EQ(absent_backend.launch(spec), worker_command_result_e::rejected);
  ASSERT_EQ(absent_host.calls.size(), std::size_t {1});
  const std::vector<std::string> expected_volume_check {
    "/usr/bin/podman", "--remote=false", "volume", "exists", "pv-a9f0",
  };
  EXPECT_EQ(absent_host.calls.front(), expected_volume_check);

  fake_host_t error_host;
  backend_t error_backend {error_host, input_manifests_for_tests(), options_for_tests()};
  error_host.push({.exit_status = 125});
  EXPECT_EQ(error_backend.launch(spec), worker_command_result_e::indeterminate);
  EXPECT_EQ(error_host.calls.size(), std::size_t {1});

  fake_host_t timeout_host;
  backend_t timeout_backend {timeout_host, input_manifests_for_tests(), options_for_tests()};
  timeout_host.push({.exit_status = 124, .timed_out = true});
  EXPECT_EQ(timeout_backend.launch(spec), worker_command_result_e::indeterminate);
  EXPECT_EQ(timeout_host.calls.size(), std::size_t {1});
}

TEST(MultiseatPodmanBackend, TimedOutLaunchIsIndeterminate) {
  fake_host_t host;
  backend_t backend {host, input_manifests_for_tests(), options_for_tests()};
  host.push({.exit_status = 0});
  host.push({
    .exit_status = 124,
    .timed_out = true,
  });

  EXPECT_EQ(backend.launch(valid_spec()), worker_command_result_e::indeterminate);
  EXPECT_EQ(host.calls.size(), std::size_t {2});
}

TEST(MultiseatPodmanBackend, AmbiguousSuccessfulLaunchOutputIsIndeterminate) {
  fake_host_t host;
  backend_t backend {host, input_manifests_for_tests(), options_for_tests()};
  host.push({.exit_status = 0});
  host.push({
    .exit_status = 0,
    .output = std::string {first_id} + "\nunexpected output\n",
  });

  EXPECT_EQ(backend.launch(valid_spec()), worker_command_result_e::indeterminate);
  EXPECT_EQ(host.calls.size(), std::size_t {2});
}

TEST(MultiseatPodmanBackend, NonzeroLaunchReconcilesAnExactExistingWorker) {
  fake_host_t host;
  backend_t backend {host, input_manifests_for_tests(), options_for_tests()};
  const auto spec = valid_spec();
  host.push({.exit_status = 0});
  host.push({
    .exit_status = 125,
  });
  queue_inventory(host, {container_for(spec, first_id, "running", "healthy")});

  EXPECT_EQ(backend.launch(spec), worker_command_result_e::already_applied);
  ASSERT_EQ(host.calls.size(), std::size_t {4});
  EXPECT_EQ(host.calls.at(0).at(2), "volume");
  EXPECT_EQ(host.calls.at(2).at(2), "ps");
  EXPECT_EQ(host.calls.at(3).at(2), "container");
}

TEST(MultiseatPodmanBackend, NonzeroLaunchRejectsMissingAndQuarantinesUnsafeWorkers) {
  const auto spec = valid_spec();

  fake_host_t missing_host;
  backend_t missing_backend {missing_host, input_manifests_for_tests(), options_for_tests()};
  missing_host.push({.exit_status = 0});
  missing_host.push({.exit_status = 125});
  queue_inventory(missing_host, {});
  EXPECT_EQ(missing_backend.launch(spec), worker_command_result_e::rejected);
  EXPECT_EQ(missing_host.calls.size(), std::size_t {3});

  fake_host_t mismatch_host;
  backend_t mismatch_backend {mismatch_host, input_manifests_for_tests(), options_for_tests()};
  auto mismatched = container_for(spec, first_id, "running", "healthy");
  mismatched["Config"]["Labels"]["io.polaris.multiseat.runtime"] =
    "polaris-runtime-controller-a1b2-wrong";
  mismatch_host.push({.exit_status = 0});
  mismatch_host.push({.exit_status = 125});
  queue_inventory(mismatch_host, {std::move(mismatched)});
  EXPECT_EQ(mismatch_backend.launch(spec), worker_command_result_e::indeterminate);
  EXPECT_EQ(mismatch_host.calls.size(), std::size_t {4});

  fake_host_t failed_host;
  backend_t failed_backend {failed_host, input_manifests_for_tests(), options_for_tests()};
  failed_host.push({.exit_status = 0});
  failed_host.push({.exit_status = 125});
  queue_inventory(failed_host, {container_for(spec, first_id, "running", "unhealthy")});
  EXPECT_EQ(failed_backend.launch(spec), worker_command_result_e::indeterminate);

  fake_host_t stopped_host;
  backend_t stopped_backend {stopped_host, input_manifests_for_tests(), options_for_tests()};
  stopped_host.push({.exit_status = 0});
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
    host.push({.exit_status = 0});
    host.push({.exit_status = 125});
    queue_inventory(host, {std::move(existing)});

    EXPECT_EQ(backend.launch(spec), worker_command_result_e::indeterminate);
    EXPECT_EQ(host.calls.size(), std::size_t {4});
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
  expect_inventory_rejected(std::move(missing_binding), [](fake_host_t &host) {
    auto &text = host.owned_files.at(runtime_spec_path_for(first_id));
    auto spec = json::parse(text);
    auto &mounts = spec["mounts"];
    mounts.erase(std::remove_if(mounts.begin(), mounts.end(), [](const json &mount) {
      return mount["destination"] == "/dev/input/polaris-gamepad-0";
    }), mounts.end());
    text = spec.dump();
  });

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

  fake_host_t changed_gpu_host;
  fake_input_manifest_source_t changed_gpu_inputs;
  backend_t changed_gpu_backend {
    changed_gpu_host,
    changed_gpu_inputs,
    options_for_tests(),
  };
  changed_gpu_host.accessible_devices["/dev/dri/renderD128"] =
    device_identity(226, 0);
  queue_inventory(
    changed_gpu_host,
    {container_for(spec, first_id, "running", "healthy")}
  );
  EXPECT_THROW(changed_gpu_backend.inventory(), std::runtime_error);
}

TEST(MultiseatPodmanBackend, EmptyInventoryRejectsGpuDriftAtReturnBoundary) {
  fake_host_t host;
  backend_t backend {host, input_manifests_for_tests(), options_for_tests()};
  host.replacement_after_character_device_call = 3;
  host.replacement_character_device_path = "/dev/dri/renderD128";
  host.replacement_character_device_identity = device_identity(226, 0);
  queue_inventory(host, {});

  EXPECT_THROW(backend.inventory(), std::runtime_error);
  EXPECT_EQ(host.calls.size(), 1U);
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
  auto oci_spec = json::parse(host.owned_files.at(runtime_spec_path_for(first_id)));
  for (auto &mount : oci_spec["mounts"]) {
    if (mount["destination"] == "/dev/input/polaris-keyboard") mount["source"] = "/dev/input/reconstructed-event10";
  }
  host.owned_files[runtime_spec_path_for(first_id)] = oci_spec.dump();
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

TEST(MultiseatPodmanBackend, InventoryKeepsReleasedStoppedWorkerVisibleWithoutInputAuthority) {
  const auto spec = valid_spec();
  fake_host_t host;
  fake_input_manifest_source_t inputs;
  inputs.missing = true;
  backend_t backend {host, inputs, options_for_tests()};
  queue_inventory(host, {container_for(spec, first_id, "exited", "")});

  const auto observations = backend.inventory();

  ASSERT_EQ(observations.size(), 1U);
  EXPECT_EQ(observations.front().identity, spec.identity);
  EXPECT_EQ(observations.front().state, worker_observed_state_e::stopped);
  EXPECT_EQ(host.calls.size(), 2U);

  fake_host_t running_host;
  fake_input_manifest_source_t running_inputs;
  running_inputs.missing = true;
  backend_t running_backend {running_host, running_inputs, options_for_tests()};
  queue_inventory(running_host, {container_for(spec, first_id, "running", "healthy")});
  EXPECT_THROW(running_backend.inventory(), std::runtime_error);

  fake_host_t stopping_host;
  fake_input_manifest_source_t stopping_inputs;
  stopping_inputs.missing = true;
  backend_t stopping_backend {stopping_host, stopping_inputs, options_for_tests()};
  queue_inventory(stopping_host, {container_for(spec, first_id, "stopping", "")});
  EXPECT_THROW(stopping_backend.inventory(), std::runtime_error);

  fake_host_t literal_stopped_host;
  fake_input_manifest_source_t literal_stopped_inputs;
  literal_stopped_inputs.missing = true;
  backend_t literal_stopped_backend {
    literal_stopped_host,
    literal_stopped_inputs,
    options_for_tests(),
  };
  queue_inventory(literal_stopped_host, {container_for(spec, first_id, "stopped", "")});
  const auto literal_stopped = literal_stopped_backend.inventory();
  ASSERT_EQ(literal_stopped.size(), 1U);
  EXPECT_EQ(literal_stopped.front().state, worker_observed_state_e::stopped);
  ASSERT_EQ(literal_stopped_host.calls.size(), 2U);
  EXPECT_EQ(literal_stopped_host.calls.at(0).at(2), "ps");
  EXPECT_EQ(literal_stopped_host.calls.at(1).at(2), "container");
  EXPECT_EQ(literal_stopped_host.calls.at(1).at(3), "inspect");

  // A stopped worker whose allocation still resolves keeps the full check:
  // its host input node vanishing must still fail the inventory.
  fake_host_t stale_stopped_host;
  fake_input_manifest_source_t stale_stopped_inputs;
  backend_t stale_stopped_backend {
    stale_stopped_host,
    stale_stopped_inputs,
    options_for_tests(),
  };
  for (const auto &node : input_allocation_for(spec.identity.seat).nodes) {
    stale_stopped_host.accessible_devices.erase(node.host_path.native());
  }
  queue_inventory(stale_stopped_host, {container_for(spec, first_id, "exited", "")});
  EXPECT_THROW(stale_stopped_backend.inventory(), std::runtime_error);
  // Looked up once to resolve, once more inside the currency check, then the
  // vanished host node failed it: the allocation was consulted, not skipped.
  EXPECT_EQ(stale_stopped_inputs.allocation_calls, 2U);
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

namespace {
  constexpr auto storage_root = "/srv/seat-operator/containers/storage";
  constexpr auto run_root = "/run/user/1000/containers";

  std::string containers_directory_for(std::string_view root, std::string_view id) {
    return std::string {root} + "/overlay-containers/" + std::string {id};
  }

  std::string runtime_userdata_for(std::string_view id) {
    return containers_directory_for(storage_root, id) + "/userdata";
  }

  std::string run_userdata_for(std::string_view id) {
    return containers_directory_for(run_root, id) + "/userdata";
  }

  std::string runtime_spec_path_for(std::string_view id) {
    return runtime_userdata_for(id) + "/config.json";
  }

  std::string volume_data_for(std::string_view volume_name) {
    return std::string {storage_root} + "/volumes/" + std::string {volume_name} + "/_data";
  }

  json runtime_mount(
    std::string destination,
    std::string type,
    std::string source,
    std::vector<std::string> options
  ) {
    return {
      {"destination", std::move(destination)},
      {"type", std::move(type)},
      {"source", std::move(source)},
      {"options", std::move(options)},
    };
  }

  json podman_bind(std::string destination, std::string source) {
    return runtime_mount(std::move(destination), "bind", std::move(source), {"bind", "rprivate"});
  }

  json device_mount(
    std::string source,
    std::string destination,
    std::string access = "rw"
  ) {
    return runtime_mount(
      std::move(destination),
      "bind",
      std::move(source),
      {"slave", "nosuid", "noexec", std::move(access), "rbind"}
    );
  }

  /**
   * The OCI runtime spec rootless Podman 5.8 writes for a worker: its own
   * pseudo-filesystems and per-container files, the controller's tmpfs,
   * volume, authority, and game mounts, the init binary, and one bind mount
   * per `--device`, which is the only place those bindings appear.
   */
  json runtime_spec_for(const worker_launch_spec_t &spec, std::string_view id) {
    const auto userdata = runtime_userdata_for(id);
    const auto run_userdata = run_userdata_for(id);
    const auto authority = std::string {"/run/user/1000/polaris-workers/"} +
                           spec.resources.runtime_namespace;
    const auto volume = spec.profile_key == "profile beta" ? "pv-b8e1" : "pv-a9f0";
    json mounts = json::array({
      runtime_mount("/run", "tmpfs", "tmpfs", {"rw", "rprivate", "nosuid", "nodev", "tmpcopyup"}),
      runtime_mount("/tmp", "tmpfs", "tmpfs", {"rw", "rprivate", "nosuid", "nodev"}),
      runtime_mount("/proc", "proc", "proc", {"nosuid", "noexec", "nodev"}),
      runtime_mount(
        "/dev",
        "tmpfs",
        "tmpfs",
        {"nosuid", "strictatime", "mode=755", "size=65536k"}
      ),
      runtime_mount("/sys", "sysfs", "sysfs", {"nosuid", "noexec", "nodev", "ro"}),
      runtime_mount(
        "/run/polaris-auth",
        "bind",
        authority + "/auth",
        {"ro", "bind", "private", "nosuid", "nodev"}
      ),
      runtime_mount(
        "/run/podman-init",
        "bind",
        "/usr/libexec/podman/catatonit",
        {"bind", "ro", "private"}
      ),
      runtime_mount("/var/tmp", "tmpfs", "tmpfs", {"rw", "rprivate", "nosuid", "nodev", "tmpcopyup"}),
      runtime_mount("/run/polaris", "tmpfs", "tmpfs", {"rw", "rprivate", "nosuid", "nodev"}),
      runtime_mount(
        "/run/polaris-ipc",
        "bind",
        authority + "/ipc",
        {"rw", "bind", "private", "nosuid", "nodev"}
      ),
      runtime_mount(
        "/dev/pts",
        "devpts",
        "devpts",
        {"nosuid", "noexec", "newinstance", "ptmxmode=0666", "mode=0620", "gid=5"}
      ),
      runtime_mount("/dev/mqueue", "mqueue", "mqueue", {"nosuid", "noexec", "nodev"}),
      podman_bind("/etc/resolv.conf", run_userdata + "/resolv.conf"),
      podman_bind("/etc/hosts", run_userdata + "/hosts"),
      runtime_mount(
        "/dev/shm",
        "bind",
        userdata + "/shm",
        {"bind", "rprivate", "nosuid", "noexec", "nodev"}
      ),
      podman_bind("/run/.containerenv", run_userdata + "/.containerenv"),
      podman_bind("/run/secrets", run_userdata + "/run/secrets"),
      podman_bind("/etc/hostname", run_userdata + "/hostname"),
      runtime_mount(
        "/sys/fs/cgroup",
        "cgroup",
        "cgroup",
        {"rprivate", "nosuid", "noexec", "nodev", "relatime", "ro"}
      ),
      runtime_mount(
        "/mnt/games/library-a",
        "bind",
        "/srv/Games Library",
        {"ro", "bind", "private", "nosuid", "nodev"}
      ),
      device_mount("/dev/dri/renderD128", "/dev/dri/renderD128"),
      device_mount("/dev/dri/card0", "/dev/dri/card0"),
    });
    for (const auto &node : input_allocation_for(spec.identity.seat).nodes) {
      mounts.push_back(device_mount(node.host_path.native(), node.worker_path.native()));
    }
    mounts.push_back(runtime_mount(
      "/var/lib/polaris-seat",
      "bind",
      volume_data_for(volume),
      {"rw", "nosuid", "nodev", "rprivate", "rbind"}
    ));
    return {
      {"ociVersion", "1.2.0"},
      {"annotations", {{"run.oci.keep_original_groups", "1"}}},
      {"process", {{"user", {{"uid", 1000}}}}},
      {"mounts", std::move(mounts)},
      {"linux", json::object()},
    };
  }

  fake_host_t::fake_host_t() {
    owned_files[runtime_spec_path_for(first_id)] = runtime_spec_for(valid_spec(), first_id).dump();
    owned_files[runtime_spec_path_for(second_id)] = runtime_spec_for(
      valid_spec(1, 2, "polaris-worker-controller-a1b2-2", "profile beta", "heroic-game"), second_id).dump();
  }

  json rootless_container_for(
    const worker_launch_spec_t &spec,
    std::string id,
    std::string runtime_state,
    std::string health_state = "healthy"
  ) {
    auto container = container_for(
      spec,
      id,
      std::move(runtime_state),
      std::move(health_state)
    );
    container["HostConfig"]["Devices"] = json::array();
    container["OCIConfigPath"] = runtime_spec_path_for(id);
    return container;
  }

  json &runtime_mount_for(json &runtime_spec, std::string_view destination) {
    for (auto &mount : runtime_spec.at("mounts")) {
      if (mount.at("destination").get<std::string>() == destination) {
        return mount;
      }
    }
    throw std::logic_error {"runtime spec mount missing"};
  }

  void erase_runtime_mount(json &runtime_spec, std::string_view destination) {
    auto &mounts = runtime_spec.at("mounts");
    mounts.erase(
      std::remove_if(
        mounts.begin(),
        mounts.end(),
        [destination](const json &mount) {
          return mount.at("destination").get<std::string>() == destination;
        }
      ),
      mounts.end()
    );
  }

  /** The same spec as stored under another storage root or driver. */
  json relocated_runtime_spec(
    const json &runtime_spec,
    std::string_view from,
    std::string_view to
  ) {
    auto dump = runtime_spec.dump();
    for (auto at = dump.find(from); at != std::string::npos; at = dump.find(from, at + to.size())) {
      dump.replace(at, from.size(), to);
    }
    return json::parse(dump);
  }

  std::string relocated(std::string value, std::string_view from, std::string_view to) {
    for (auto at = value.find(from); at != std::string::npos; at = value.find(from, at + to.size())) {
      value.replace(at, from.size(), to);
    }
    return value;
  }

  /** Runs one rootless inventory expecting a throw; returns the spec reads. */
  std::size_t rootless_inventory_rejected(
    const worker_launch_spec_t &spec,
    const json &runtime_spec,
    std::function<void(json &)> mutate_container = {},
    std::function<void(fake_host_t &)> configure_host = {}
  ) {
    auto container = rootless_container_for(spec, first_id, "running", "healthy");
    if (mutate_container) {
      mutate_container(container);
    }
    fake_host_t host;
    host.owned_files[runtime_spec_path_for(first_id)] = runtime_spec.dump();
    if (configure_host) {
      configure_host(host);
    }
    fake_input_manifest_source_t inputs;
    backend_t backend {host, inputs, options_for_tests()};
    queue_inventory(host, {std::move(container)});
    EXPECT_THROW(backend.inventory(), std::runtime_error);
    return host.owned_file_reads;
  }

  void expect_rootless_inventory_rejected(
    const worker_launch_spec_t &spec,
    const json &runtime_spec,
    std::size_t expected_spec_reads = 1,
    std::function<void(json &)> mutate_container = {},
    std::function<void(fake_host_t &)> configure_host = {}
  ) {
    EXPECT_EQ(
      rootless_inventory_rejected(spec, runtime_spec, mutate_container, configure_host),
      expected_spec_reads
    );
  }

  void expect_rootless_inventory_ready(
    const worker_launch_spec_t &spec,
    const json &runtime_spec,
    std::function<void(json &)> mutate_container = {},
    std::string spec_path = runtime_spec_path_for(first_id)
  ) {
    auto container = rootless_container_for(spec, first_id, "running", "healthy");
    container["OCIConfigPath"] = spec_path;
    if (mutate_container) {
      mutate_container(container);
    }
    fake_host_t host;
    host.owned_files[spec_path] = runtime_spec.dump();
    fake_input_manifest_source_t inputs;
    backend_t backend {host, inputs, options_for_tests()};
    queue_inventory(host, {std::move(container)});
    const auto observations = backend.inventory();
    ASSERT_EQ(observations.size(), 1U);
    EXPECT_EQ(observations.front().identity, spec.identity);
    EXPECT_EQ(observations.front().state, worker_observed_state_e::ready);
    EXPECT_EQ(host.owned_file_reads, 1U);
  }
}  // namespace

TEST(MultiseatPodmanBackend, InventoryReadsRootlessDeviceBindingsFromTheRuntimeSpec) {
  fake_host_t host;
  fake_input_manifest_source_t inputs;
  backend_t backend {host, inputs, options_for_tests()};
  const auto spec = valid_spec();
  host.owned_files[runtime_spec_path_for(first_id)] = runtime_spec_for(spec, first_id).dump();
  queue_inventory(host, {rootless_container_for(spec, first_id, "running", "healthy")});

  const auto observations = backend.inventory();

  ASSERT_EQ(observations.size(), 1U);
  EXPECT_EQ(observations.front().identity, spec.identity);
  EXPECT_EQ(observations.front().state, worker_observed_state_e::ready);
  EXPECT_EQ(host.owned_file_reads, 1U);
  EXPECT_EQ(host.last_owned_file_limit, options_for_tests().max_command_output_bytes);
  EXPECT_EQ(host.calls.size(), 2U);
}

TEST(MultiseatPodmanBackend, InventoryAcceptsRuntimeSpecShapesPodmanMayEmit) {
  const auto spec = valid_spec();
  const auto keyboard = "/dev/input/polaris-keyboard";

  {
    SCOPED_TRACE("device mount without options");
    auto runtime_spec = runtime_spec_for(spec, first_id);
    runtime_mount_for(runtime_spec, keyboard).erase("options");
    expect_rootless_inventory_ready(spec, runtime_spec);
  }
  {
    SCOPED_TRACE("no linux object");
    auto runtime_spec = runtime_spec_for(spec, first_id);
    runtime_spec.erase("linux");
    expect_rootless_inventory_ready(spec, runtime_spec);
  }
  {
    SCOPED_TRACE("null device node list");
    auto runtime_spec = runtime_spec_for(spec, first_id);
    runtime_spec["linux"]["devices"] = nullptr;
    expect_rootless_inventory_ready(spec, runtime_spec);
  }
  {
    SCOPED_TRACE("no network files");
    auto runtime_spec = runtime_spec_for(spec, first_id);
    erase_runtime_mount(runtime_spec, "/etc/resolv.conf");
    erase_runtime_mount(runtime_spec, "/etc/hosts");
    expect_rootless_inventory_ready(spec, runtime_spec);
  }
  {
    SCOPED_TRACE("inspected device list that agrees with the spec");
    expect_rootless_inventory_ready(
      spec,
      runtime_spec_for(spec, first_id),
      [&spec](json &container) {
        container["HostConfig"]["Devices"] = inspected_devices_for(spec);
      }
    );
  }
  {
    SCOPED_TRACE("vfs storage driver");
    expect_rootless_inventory_ready(
      spec,
      relocated_runtime_spec(runtime_spec_for(spec, first_id), "overlay-containers", "vfs-containers"),
      {},
      relocated(runtime_spec_path_for(first_id), "overlay-containers", "vfs-containers")
    );
  }
  {
    SCOPED_TRACE("storage root under /dev/shm");
    const auto shm_root = "/dev/shm/containers/storage";
    expect_rootless_inventory_ready(
      spec,
      relocated_runtime_spec(runtime_spec_for(spec, first_id), storage_root, shm_root),
      {},
      relocated(runtime_spec_path_for(first_id), storage_root, shm_root)
    );
  }
}

TEST(MultiseatPodmanBackend, InventoryRejectsRuntimeSpecDeviceDrift) {
  const auto spec = valid_spec();
  const auto keyboard = "/dev/input/polaris-keyboard";

  {
    SCOPED_TRACE("missing device mount");
    auto runtime_spec = runtime_spec_for(spec, first_id);
    erase_runtime_mount(runtime_spec, keyboard);
    expect_rootless_inventory_rejected(spec, runtime_spec);
  }
  {
    SCOPED_TRACE("renamed worker path");
    auto runtime_spec = runtime_spec_for(spec, first_id);
    runtime_mount_for(runtime_spec, keyboard)["destination"] =
      "/dev/input/polaris-keyboard-other";
    expect_rootless_inventory_rejected(spec, runtime_spec);
  }
  {
    SCOPED_TRACE("trailing slash on the worker path");
    auto runtime_spec = runtime_spec_for(spec, first_id);
    runtime_mount_for(runtime_spec, keyboard)["destination"] =
      "/dev/input/polaris-keyboard/";
    expect_rootless_inventory_rejected(spec, runtime_spec);
  }
  {
    SCOPED_TRACE("changed host device");
    auto runtime_spec = runtime_spec_for(spec, first_id);
    runtime_mount_for(runtime_spec, keyboard)["source"] = "/dev/input/event20";
    expect_rootless_inventory_rejected(spec, runtime_spec);
  }
  {
    SCOPED_TRACE("non-normalized host device");
    auto runtime_spec = runtime_spec_for(spec, first_id);
    runtime_mount_for(runtime_spec, keyboard)["source"] = "//dev/input/event10";
    expect_rootless_inventory_rejected(spec, runtime_spec);
  }
  {
    SCOPED_TRACE("read-only binding");
    auto runtime_spec = runtime_spec_for(spec, first_id);
    runtime_mount_for(runtime_spec, keyboard)["options"] =
      json::array({"slave", "nosuid", "noexec", "ro", "rbind"});
    expect_rootless_inventory_rejected(spec, runtime_spec);
  }
  {
    SCOPED_TRACE("same device twice");
    auto runtime_spec = runtime_spec_for(spec, first_id);
    runtime_spec["mounts"].push_back(runtime_mount_for(runtime_spec, keyboard));
    expect_rootless_inventory_rejected(spec, runtime_spec);
  }
  {
    SCOPED_TRACE("device bound outside /dev");
    auto runtime_spec = runtime_spec_for(spec, first_id);
    runtime_spec["mounts"].push_back(device_mount("/dev/input/event20", "/opt/probe"));
    expect_rootless_inventory_rejected(spec, runtime_spec);
  }
  {
    SCOPED_TRACE("/dev bound whole");
    auto runtime_spec = runtime_spec_for(spec, first_id);
    runtime_spec["mounts"].push_back(device_mount("/dev", "/host-dev"));
    expect_rootless_inventory_rejected(spec, runtime_spec);
  }
  {
    SCOPED_TRACE("root bound whole");
    auto runtime_spec = runtime_spec_for(spec, first_id);
    runtime_spec["mounts"].push_back(device_mount("/", "/host"));
    expect_rootless_inventory_rejected(spec, runtime_spec);
  }
  {
    SCOPED_TRACE("unexpected host file");
    auto runtime_spec = runtime_spec_for(spec, first_id);
    runtime_spec["mounts"].push_back(podman_bind("/opt/x", "/srv/seat-operator/x"));
    expect_rootless_inventory_rejected(spec, runtime_spec);
  }
  {
    SCOPED_TRACE("another container's files");
    auto runtime_spec = runtime_spec_for(spec, first_id);
    runtime_mount_for(runtime_spec, "/dev/shm")["source"] =
      runtime_userdata_for(second_id) + "/shm";
    expect_rootless_inventory_rejected(spec, runtime_spec);
  }
  {
    SCOPED_TRACE("own file at an unexpected destination");
    auto runtime_spec = runtime_spec_for(spec, first_id);
    runtime_mount_for(runtime_spec, "/dev/shm")["destination"] = "/opt/shm";
    expect_rootless_inventory_rejected(spec, runtime_spec);
  }
  {
    SCOPED_TRACE("another volume");
    auto runtime_spec = runtime_spec_for(spec, first_id);
    runtime_mount_for(runtime_spec, "/var/lib/polaris-seat")["source"] =
      volume_data_for("pv-other");
    expect_rootless_inventory_rejected(spec, runtime_spec);
  }
  {
    SCOPED_TRACE("another profile's volume");
    auto runtime_spec = runtime_spec_for(spec, first_id);
    runtime_mount_for(runtime_spec, "/var/lib/polaris-seat")["source"] =
      volume_data_for("pv-b8e1");
    expect_rootless_inventory_rejected(spec, runtime_spec);
  }
  {
    SCOPED_TRACE("volume from another storage root");
    auto runtime_spec = runtime_spec_for(spec, first_id);
    runtime_mount_for(runtime_spec, "/var/lib/polaris-seat")["source"] =
      "/srv/other/volumes/pv-a9f0/_data";
    expect_rootless_inventory_rejected(spec, runtime_spec);
  }
  {
    SCOPED_TRACE("volume landing outside its mount point");
    auto runtime_spec = runtime_spec_for(spec, first_id);
    runtime_mount_for(runtime_spec, "/var/lib/polaris-seat")["destination"] = "/usr";
    expect_rootless_inventory_rejected(spec, runtime_spec);
  }
  {
    SCOPED_TRACE("authority directory bound read-write");
    auto runtime_spec = runtime_spec_for(spec, first_id);
    runtime_mount_for(runtime_spec, "/run/polaris-auth")["options"] =
      json::array({"rw", "bind", "private", "nosuid", "nodev"});
    expect_rootless_inventory_rejected(spec, runtime_spec);
  }
  {
    SCOPED_TRACE("authority directory at an unexpected destination");
    auto runtime_spec = runtime_spec_for(spec, first_id);
    runtime_mount_for(runtime_spec, "/run/polaris-ipc")["destination"] = "/run/polaris-auth-other";
    expect_rootless_inventory_rejected(spec, runtime_spec);
  }
  {
    SCOPED_TRACE("game library bound read-write");
    auto runtime_spec = runtime_spec_for(spec, first_id);
    runtime_mount_for(runtime_spec, "/mnt/games/library-a")["options"] =
      json::array({"rw", "bind", "private", "nosuid", "nodev"});
    expect_rootless_inventory_rejected(spec, runtime_spec);
  }
  {
    SCOPED_TRACE("storage file presented as an input device");
    auto runtime_spec = runtime_spec_for(spec, first_id);
    runtime_spec["mounts"].push_back(
      podman_bind("/dev/input/polaris-extra", runtime_userdata_for(first_id) + "/probe")
    );
    expect_rootless_inventory_rejected(spec, runtime_spec);
  }
  {
    SCOPED_TRACE("storage directory over the input directory");
    auto runtime_spec = runtime_spec_for(spec, first_id);
    runtime_spec["mounts"].push_back(
      podman_bind("/dev/input", runtime_userdata_for(first_id) + "/input")
    );
    expect_rootless_inventory_rejected(spec, runtime_spec);
  }
  {
    SCOPED_TRACE("tmpfs over a GPU path");
    auto runtime_spec = runtime_spec_for(spec, first_id);
    runtime_spec["mounts"].push_back(
      runtime_mount("/dev/dri", "tmpfs", "tmpfs", {"nosuid"})
    );
    expect_rootless_inventory_rejected(spec, runtime_spec);
  }
  {
    SCOPED_TRACE("tmpfs over /dev after the devices");
    auto runtime_spec = runtime_spec_for(spec, first_id);
    runtime_spec["mounts"].push_back(
      runtime_mount("/dev", "tmpfs", "tmpfs", {"nosuid", "mode=755"})
    );
    expect_rootless_inventory_rejected(spec, runtime_spec);
  }
  {
    SCOPED_TRACE("raw creation endpoint");
    auto runtime_spec = runtime_spec_for(spec, first_id);
    runtime_spec["mounts"].push_back(device_mount("/dev/uinput", "/dev/uinput"));
    expect_rootless_inventory_rejected(
      spec,
      runtime_spec,
      1,
      {},
      [](fake_host_t &host) {
        host.accessible_devices["/dev/uinput"] = device_identity(10, 223);
      }
    );
  }
  {
    SCOPED_TRACE("init binary bound read-write");
    auto runtime_spec = runtime_spec_for(spec, first_id);
    runtime_mount_for(runtime_spec, "/run/podman-init")["options"] =
      json::array({"bind", "rw", "private"});
    expect_rootless_inventory_rejected(spec, runtime_spec);
  }
  {
    SCOPED_TRACE("init binary that is not an executable file");
    auto runtime_spec = runtime_spec_for(spec, first_id);
    runtime_mount_for(runtime_spec, "/run/podman-init")["source"] = "/srv/seat-operator/x";
    expect_rootless_inventory_rejected(spec, runtime_spec);
  }
  {
    SCOPED_TRACE("device presented as the init binary");
    auto runtime_spec = runtime_spec_for(spec, first_id);
    runtime_mount_for(runtime_spec, "/run/podman-init")["source"] = "/dev/input/event20";
    expect_rootless_inventory_rejected(
      spec,
      runtime_spec,
      1,
      {},
      [](fake_host_t &host) {
        host.executable_files.insert("/dev/input/event20");
      }
    );
  }
  {
    SCOPED_TRACE("device granted as a cgroup node");
    auto runtime_spec = runtime_spec_for(spec, first_id);
    runtime_spec["linux"]["devices"] = json::array({
      {{"path", "/dev/input/event20"}, {"type", "c"}, {"major", 13}, {"minor", 84}},
    });
    expect_rootless_inventory_rejected(spec, runtime_spec);
  }
  {
    SCOPED_TRACE("inspected device list that contradicts the spec");
    expect_rootless_inventory_rejected(
      spec,
      runtime_spec_for(spec, first_id),
      1,
      [&spec, keyboard](json &container) {
        auto devices = inspected_devices_for(spec);
        for (auto &device : devices) {
          if (device.at("PathInContainer") == keyboard) {
            device["PathOnHost"] = "/dev/input/event20";
          }
        }
        container["HostConfig"]["Devices"] = std::move(devices);
      }
    );
  }
  {
    SCOPED_TRACE("inspected device the spec never mounted");
    expect_rootless_inventory_rejected(
      spec,
      runtime_spec_for(spec, first_id),
      1,
      [](json &container) {
        container["HostConfig"]["Devices"] = json::array({
          {{"PathOnHost", "/dev/input/event20"}, {"PathInContainer", "/dev/input/polaris-extra"}},
        });
      }
    );
  }
}

TEST(MultiseatPodmanBackend, InventoryRequiresTheWorkerVolumeLabel) {
  const auto spec = valid_spec();
  const auto valid = runtime_spec_for(spec, first_id);
  {
    SCOPED_TRACE("volume label missing");
    expect_rootless_inventory_rejected(spec, valid, 0, [](json &container) {
      container["Config"]["Labels"].erase("io.polaris.multiseat.volume");
    });
  }
  {
    SCOPED_TRACE("volume label naming an unconfigured volume");
    expect_rootless_inventory_rejected(spec, valid, 0, [](json &container) {
      container["Config"]["Labels"]["io.polaris.multiseat.volume"] = "pv-other";
    });
  }
  {
    SCOPED_TRACE("volume label with an unsafe value");
    expect_rootless_inventory_rejected(spec, valid, 0, [](json &container) {
      container["Config"]["Labels"]["io.polaris.multiseat.volume"] = "../pv-a9f0";
    });
  }
}

TEST(MultiseatPodmanBackend, InventoryRejectsUnusableRuntimeSpecFiles) {
  const auto spec = valid_spec();
  const auto valid = runtime_spec_for(spec, first_id);

  {
    SCOPED_TRACE("spec missing");
    fake_host_t host;
    host.owned_files.clear();
    fake_input_manifest_source_t inputs;
    backend_t backend {host, inputs, options_for_tests()};
    queue_inventory(host, {rootless_container_for(spec, first_id, "running", "healthy")});
    EXPECT_THROW(backend.inventory(), std::runtime_error);
    EXPECT_EQ(host.owned_file_reads, 1U);
  }
  {
    SCOPED_TRACE("spec of another container");
    expect_rootless_inventory_rejected(
      spec,
      valid,
      0,
      [](json &container) {
        container["OCIConfigPath"] = runtime_spec_path_for(second_id);
      },
      [&valid](fake_host_t &host) {
        host.owned_files[runtime_spec_path_for(second_id)] = valid.dump();
      }
    );
  }
  const std::vector<std::string> bad_paths {
    "overlay-containers/" + std::string {first_id} + "/userdata/config.json",
    containers_directory_for(storage_root, first_id) + "/../" +
      std::string {first_id} + "/userdata/config.json",
    runtime_userdata_for(first_id) + "/spec.json",
    containers_directory_for(storage_root, first_id) + "/config.json",
    "/" + std::string {first_id} + "/userdata/config.json",
    "/tmp/" + std::string {first_id} + "/userdata/config.json",
    runtime_spec_path_for(first_id) + "/",
  };
  for (const auto &bad_path : bad_paths) {
    SCOPED_TRACE(bad_path);
    expect_rootless_inventory_rejected(
      spec,
      valid,
      0,
      [&bad_path](json &container) {
        container["OCIConfigPath"] = bad_path;
      },
      [&bad_path, &valid](fake_host_t &host) {
        host.owned_files[bad_path] = valid.dump();
      }
    );
  }
  {
    SCOPED_TRACE("non-string path");
    expect_rootless_inventory_rejected(spec, valid, 0, [](json &container) {
      container["OCIConfigPath"] = 7;
    });
  }
  {
    SCOPED_TRACE("oversized spec");
    expect_rootless_inventory_rejected(
      spec,
      valid,
      1,
      {},
      [&valid](fake_host_t &host) {
        host.owned_files[runtime_spec_path_for(first_id)] =
          valid.dump() + std::string(options_for_tests().max_command_output_bytes, ' ');
      }
    );
  }
  {
    SCOPED_TRACE("malformed spec");
    expect_rootless_inventory_rejected(
      spec,
      valid,
      1,
      {},
      [](fake_host_t &host) {
        host.owned_files[runtime_spec_path_for(first_id)] = "{";
      }
    );
  }
  {
    SCOPED_TRACE("mounts missing");
    expect_rootless_inventory_rejected(spec, json {{"ociVersion", "1.2.0"}});
  }
  {
    SCOPED_TRACE("mount without a destination");
    auto runtime_spec = valid;
    runtime_mount_for(runtime_spec, "/proc").erase("destination");
    expect_rootless_inventory_rejected(spec, runtime_spec);
  }
  {
    SCOPED_TRACE("bind without a source");
    auto runtime_spec = valid;
    runtime_mount_for(runtime_spec, "/etc/hosts").erase("source");
    expect_rootless_inventory_rejected(spec, runtime_spec);
  }
}

TEST(MultiseatPodmanBackend, InventoryFailureCarriesTheReason) {
  const auto spec = valid_spec();
  auto runtime_spec = runtime_spec_for(spec, first_id);
  runtime_spec["mounts"].push_back(device_mount("/dev", "/host-dev"));
  fake_host_t host;
  host.owned_files[runtime_spec_path_for(first_id)] = runtime_spec.dump();
  fake_input_manifest_source_t inputs;
  backend_t backend {host, inputs, options_for_tests()};
  queue_inventory(host, {rootless_container_for(spec, first_id, "running", "healthy")});

  std::string reason;
  try {
    (void) backend.inventory();
  } catch (const std::runtime_error &error) {
    reason = error.what();
  }

  EXPECT_NE(reason.find("container engine returned invalid worker inventory"), std::string::npos);
  EXPECT_NE(reason.find("unexpected Podman runtime spec mount"), std::string::npos);
}

TEST(MultiseatPodmanBackend, ReleasedStoppedRootlessWorkerNeedsNoRuntimeSpec) {
  const auto spec = valid_spec();
  fake_host_t host;
  fake_input_manifest_source_t inputs;
  inputs.missing = true;
  backend_t backend {host, inputs, options_for_tests()};
  queue_inventory(host, {rootless_container_for(spec, first_id, "exited", "")});

  const auto observations = backend.inventory();

  ASSERT_EQ(observations.size(), 1U);
  EXPECT_EQ(observations.front().identity, spec.identity);
  EXPECT_EQ(observations.front().state, worker_observed_state_e::stopped);
  EXPECT_EQ(host.owned_file_reads, 0U);
}

TEST(MultiseatPodmanBackend, NeverStartedRootlessWorkerIsStoppedOnceItsAllocationIsGone) {
  const auto spec = valid_spec();
  for (const auto *state : {"created", "configured"}) {
    SCOPED_TRACE(state);
    fake_host_t host;
    fake_input_manifest_source_t inputs;
    inputs.missing = true;
    backend_t backend {host, inputs, options_for_tests()};
    queue_inventory(host, {rootless_container_for(spec, first_id, state, "")});

    const auto observations = backend.inventory();

    ASSERT_EQ(observations.size(), 1U);
    EXPECT_EQ(observations.front().identity, spec.identity);
    EXPECT_EQ(observations.front().state, worker_observed_state_e::stopped);
    EXPECT_EQ(host.owned_file_reads, 0U);
  }

  // With its allocation still resolving it is a launch in flight: the missing
  // spec makes the inventory indeterminate rather than releasing the seat.
  fake_host_t launching_host;
  launching_host.owned_files.clear();
  fake_input_manifest_source_t launching_inputs;
  backend_t launching_backend {launching_host, launching_inputs, options_for_tests()};
  queue_inventory(launching_host, {rootless_container_for(spec, first_id, "created", "")});
  EXPECT_THROW(launching_backend.inventory(), std::runtime_error);
  EXPECT_EQ(launching_host.owned_file_reads, 1U);

  // Without input authority the state stays what Podman reported.
  fake_host_t stop_host;
  fake_input_manifest_source_t stop_inputs;
  stop_inputs.missing = true;
  backend_t stop_backend {stop_host, stop_inputs, options_for_tests()};
  queue_inventory(stop_host, {rootless_container_for(spec, first_id, "created", "")});
  stop_host.push({.exit_status = 0});
  EXPECT_EQ(
    stop_backend.stop(spec.identity, worker_stop_mode_e::force),
    worker_command_result_e::applied
  );
  EXPECT_EQ(stop_host.owned_file_reads, 0U);
}

TEST(MultiseatPodmanBackend, StopNeverReadsTheRuntimeSpec) {
  const auto spec = valid_spec();
  fake_host_t host;
  fake_input_manifest_source_t inputs;
  backend_t backend {host, inputs, options_for_tests()};
  queue_inventory(host, {rootless_container_for(spec, first_id, "running", "healthy")});
  host.push({.exit_status = 0});

  EXPECT_EQ(
    backend.stop(spec.identity, worker_stop_mode_e::graceful),
    worker_command_result_e::applied
  );
  EXPECT_EQ(host.owned_file_reads, 0U);
  ASSERT_EQ(host.calls.size(), 3U);
  EXPECT_EQ(
    host.calls.back(),
    (std::vector<std::string> {
      "/usr/bin/podman", "--remote=false", "kill", "--signal=TERM", first_id,
    })
  );
}

TEST(MultiseatPodmanBackend, LaunchRequiresTrustedCrunAndActualProcessGroupSnapshot) {
  for (int failure = 0; failure < 4; ++failure) {
    const auto spec = valid_spec();
    fake_host_t host;
    fake_input_manifest_source_t inputs;
    auto options = options_for_tests();
    if (failure == 0) host.runtime_ready = false;
    if (failure == 1) options.runtime_executable = "/usr/bin/runc";
    if (failure == 2) host.groups.reset();
    if (failure == 3) {
      host.groups_change_on_recheck = true;
      host.push({.exit_status = 0}); // Existing profile volume.
    }
    backend_t backend {host, inputs, options};
    EXPECT_EQ(backend.launch(spec), worker_command_result_e::rejected);
    EXPECT_EQ(host.calls.size(), failure == 3 ? 1U : 0U);
  }
}

TEST(MultiseatPodmanBackend, CleanupSurvivesLostRuntimeAndGroups) {
  const auto spec = valid_spec();
  fake_host_t host;
  host.runtime_ready = false;
  host.groups.reset();
  fake_input_manifest_source_t inputs;
  inputs.missing = true;
  auto options = options_for_tests();
  options.runtime_executable = "/usr/bin/missing-crun";
  backend_t backend {host, inputs, options};
  auto container = rootless_container_for(spec, first_id, "running");
  container["OCIRuntime"] = "runc";
  container["HostConfig"]["GroupAdd"] = json::array();
  queue_inventory(host, {container});
  host.push({.exit_status = 0});
  EXPECT_EQ(backend.stop(spec.identity, worker_stop_mode_e::force), worker_command_result_e::applied);
  EXPECT_EQ(host.group_reads, 0);
  EXPECT_EQ(host.owned_file_reads, 0U);
}

TEST(MultiseatPodmanBackend, InventoryRequiresCrunAndKeepGroupsEvidence) {
  const auto spec = valid_spec();
  const auto valid = runtime_spec_for(spec, first_id);
  for (int failure = 0; failure < 5; ++failure) {
    auto runtime_spec = valid;
    if (failure == 0) runtime_spec.erase("annotations");
    if (failure == 1) runtime_spec["annotations"]["run.oci.keep_original_groups"] = "0";
    expect_rootless_inventory_rejected(spec, runtime_spec, failure < 2 ? 1U : 0U,
      [failure](json &container) {
        if (failure == 2) container["OCIRuntime"] = "runc";
        if (failure == 3) container["HostConfig"].erase("GroupAdd");
        if (failure == 4) container["HostConfig"]["GroupAdd"] = json::array({"keep-groups", "104"});
      });
  }
}

TEST(MultiseatPodmanBackend, InventoryRejectsImageUserOverrideOrChangedOciUid) {
  const auto spec = valid_spec();
  for (int failure = 0; failure < 5; ++failure) {
    auto runtime_spec = runtime_spec_for(spec, first_id);
    if (failure == 0) runtime_spec.erase("process");
    if (failure == 1) runtime_spec["process"]["user"]["uid"] = 0;
    if (failure == 2) runtime_spec["process"]["user"]["uid"] = "1000";
    expect_rootless_inventory_rejected(spec, runtime_spec, failure < 3 ? 1U : 0U,
      [failure](json &container) {
        if (failure == 3) container["Config"]["User"] = "0";
        if (failure == 4) container["Config"].erase("User");
      });
  }
}

TEST(MultiseatPodmanBackend, RecoveryRejectsUnsupportedOrUntrustedRuntimeBeforeObservation) {
  for (const auto path : {"/usr/bin/runc", "/tmp/crun", "/usr/bin/crun"}) {
    fake_host_t host;
    host.runtime_ready = false;
    fake_input_manifest_source_t inputs;
    auto options = options_for_tests();
    options.runtime_executable = path;
    backend_t backend {host, inputs, options};
    auto container = container_for(valid_spec(), first_id, "running");
    container["OCIRuntime"] = path;
    queue_inventory(host, {container});
    EXPECT_THROW(backend.inventory(), std::runtime_error);
    EXPECT_TRUE(host.calls.empty());
  }
}


namespace {
  options_t docker_options_for_tests() {
    auto options = options_for_tests();
    options.engine = multiseat::container::engine_e::docker;
    options.executable = "/usr/bin/docker";
    options.runtime_executable = "/usr/bin/runc";
    return options;
  }

  json docker_info_for_tests() {
    return {{"OSType", "linux"}, {"SecurityOptions", {"name=seccomp,profile=builtin", "name=selinux", "name=cgroupns"}},
            {"Runtimes", {{"runc", {{"path", "runc"}}}}}};
  }

  json docker_volume_for_tests(std::string name = "pv-a9f0") {
    return {{"Name", name}, {"Driver", "local"}, {"Scope", "local"}, {"Options", nullptr},
            {"Mountpoint", "/var/lib/docker/volumes/" + name + "/_data"}};
  }

  // The Docker-specific fields follow an actual Docker Engine 29 create/inspect
  // observation. Identity values come from the fixture allocation, never from
  // the backend's command builder or inspection validator.
  json docker_container_for(const worker_launch_spec_t &spec, std::string id, std::string status = "running") {
    auto record = container_for(spec, std::move(id), std::move(status));
    record.erase("OCIConfigPath");
    record.erase("OCIRuntime");
    record["Name"] = "/" + spec.identity.worker_name;
    const auto labels = labels_for(spec);
    const auto label = [&labels](const std::string &key) { return labels.at("io.polaris.multiseat." + key).get<std::string>(); };
    const auto volume = label("volume");
    const auto authority = "/run/user/1000/polaris-workers/" + spec.resources.runtime_namespace;
    auto &config = record["Config"];
    config["User"] = "1000:1000";
    config["Image"] = label("runtime-image");
    config["Entrypoint"] = {"/usr/bin/polaris-seat-worker"};
    config["Cmd"] = {"run", "--workload-kind=" + label("workload-kind"), "--workload-id=" + label("workload-target")};
    config["WorkingDir"] = "/var/lib/polaris-seat";
    config["Healthcheck"] = {
      {"Test", {"CMD-SHELL", "exec '/usr/bin/polaris-seat-worker' health"}},
      {"Interval", 2000000000LL}, {"Timeout", 1000000000LL}, {"StartPeriod", 30000000000LL}, {"Retries", 15},
    };
    config["Env"] = {
      "HOME=/var/lib/polaris-seat", "XDG_CONFIG_HOME=/var/lib/polaris-seat/.config",
      "XDG_CACHE_HOME=/var/lib/polaris-seat/.cache", "XDG_DATA_HOME=/var/lib/polaris-seat/.local/share",
      "XDG_RUNTIME_DIR=/run/polaris", "DBUS_SESSION_BUS_ADDRESS=unix:path=/run/polaris/bus",
      "PIPEWIRE_RUNTIME_DIR=/run/polaris", "PULSE_SERVER=unix:/run/polaris/pulse/native",
      "PULSE_SINK=" + spec.resources.audio_sink, "WAYLAND_DISPLAY=" + spec.resources.wayland_socket,
      "POLARIS_CAPTURE_WAYLAND_DISPLAY=" + spec.resources.capture_wayland_socket,
      "POLARIS_RUNTIME_NAMESPACE=" + spec.resources.runtime_namespace,
      "POLARIS_WORKER_NAME=" + spec.identity.worker_name, "POLARIS_INPUT_SEAT=" + spec.resources.input_seat,
      "POLARIS_CONTROLLER_EPOCH=" + spec.identity.seat.controller_epoch,
      "POLARIS_LOGICAL_GPU_ID=" + spec.identity.seat.logical_gpu_id,
      "POLARIS_SEAT_SLOT=" + std::to_string(spec.identity.seat.slot),
      "POLARIS_SEAT_GENERATION=" + std::to_string(spec.identity.seat.generation),
      "POLARIS_RENDER_NODE=" + spec.render_node, "POLARIS_RUNTIME_PROFILE=" + label("runtime-profile"),
      "POLARIS_DISPLAY_TOPOLOGY=" + label("display-topology"), "POLARIS_MEDIA_PIPELINE=" + label("media-pipeline"),
      "POLARIS_DISPLAY_WIDTH=" + label("display-width"), "POLARIS_DISPLAY_HEIGHT=" + label("display-height"),
      "POLARIS_DISPLAY_REFRESH_MILLIHZ=" + label("display-refresh-millihz"),
      "POLARIS_DISPLAY_HDR=" + label("display-hdr"), "POLARIS_COMPOSITOR=" + label("compositor"),
      "POLARIS_ENCODER_SESSIONS=" + label("encoders"), "PATH=/usr/bin:/bin",
    };
    auto &host = record["HostConfig"];
    for (auto &device : host["Devices"]) device["CgroupPermissions"] = "rw";
    host.update({
      {"GroupAdd", {"39", "104", "105"}}, {"Privileged", false}, {"ReadonlyRootfs", true},
      {"AutoRemove", true}, {"Init", true}, {"Runtime", "runc"}, {"UsernsMode", "host"},
      {"NetworkMode", "none"}, {"IpcMode", "private"}, {"PidMode", ""}, {"UTSMode", ""},
      {"CgroupnsMode", "private"}, {"PidsLimit", 4096}, {"ShmSize", 1073741824},
      {"CapDrop", {"ALL"}}, {"SecurityOpt", {"no-new-privileges"}},
      {"CapAdd", nullptr}, {"DeviceRequests", nullptr}, {"DeviceCgroupRules", nullptr},
      {"VolumesFrom", nullptr}, {"Links", nullptr}, {"ExtraHosts", nullptr},
      {"RestartPolicy", {{"Name", "no"}, {"MaximumRetryCount", 0}}},
      {"LogConfig", {{"Type", "json-file"}, {"Config", {{"max-size", "8388608"}, {"max-file", "1"}}}}},
      {"Tmpfs", {
        {"/run", "rw,nosuid,nodev,size=67108864,mode=0700,uid=1000,gid=1000"},
        {"/run/polaris", "rw,nosuid,nodev,size=67108864,mode=0700,uid=1000,gid=1000"},
        {"/tmp", "rw,nosuid,nodev,size=1073741824,mode=0700,uid=1000,gid=1000"},
        {"/var/tmp", "rw,nosuid,nodev,size=1073741824,mode=1777,uid=1000,gid=1000"},
      }},
      {"Mounts", json::array({{{"Type", "volume"}, {"Source", volume}, {"Target", "/var/lib/polaris-seat"},
                              {"VolumeOptions", {{"NoCopy", true}}}}})},
    });
    record["Mounts"] = json::array({
      {{"Type", "volume"}, {"Name", volume}, {"Driver", "local"}, {"RW", true},
       {"Source", "/var/lib/docker/volumes/" + volume + "/_data"}, {"Destination", "/var/lib/polaris-seat"}},
      {{"Type", "bind"}, {"RW", true}, {"Source", authority + "/ipc"}, {"Destination", "/run/polaris-ipc"}, {"Propagation", "rprivate"}},
      {{"Type", "bind"}, {"RW", false}, {"Source", authority + "/auth"}, {"Destination", "/run/polaris-auth"}, {"Propagation", "rprivate"}},
      {{"Type", "bind"}, {"RW", false}, {"Source", "/srv/Games Library"}, {"Destination", "/mnt/games/library-a"}, {"Propagation", "rprivate"}},
    });
    return record;
  }

  void queue_docker_inventory(fake_host_t &host, const std::vector<json> &records) {
    host.push({.exit_status = 0, .output = docker_info_for_tests().dump()});
    queue_inventory(host, records);
  }
}

TEST(MultiseatDockerBackend, DefaultsToDockerAndPinsTheLocalDaemonWithoutInheritedClientConfiguration) {
  const options_t options;
  EXPECT_EQ(options.engine, multiseat::container::engine_e::docker);
  EXPECT_EQ(options.executable, "/usr/bin/docker");
  EXPECT_EQ(options.runtime_executable, "/usr/bin/runc");
  const std::vector<std::string> expected {
    "/usr/bin/env", "-i", "PATH=/usr/bin:/bin", "HOME=/nonexistent", "/usr/bin/docker",
    "--config=/nonexistent/polaris-docker-cli", "--host=unix:///var/run/docker.sock",
  };
  EXPECT_EQ(multiseat::container::command_prefix(options), expected);
  fake_host_t host;
  fake_input_manifest_source_t inputs;
  auto bad = docker_options_for_tests();
  bad.daemon_socket = "tcp://elsewhere:2375";
  EXPECT_THROW((backend_t {host, inputs, bad}), std::invalid_argument);
}

TEST(MultiseatDockerBackend, LaunchUsesNonRootIdentityExactDevicesAndDockerSupportedOptions) {
  fake_host_t host;
  fake_input_manifest_source_t inputs;
  backend_t backend {host, inputs, docker_options_for_tests()};
  host.push({.exit_status = 0, .output = docker_info_for_tests().dump()});
  host.push({.exit_status = 0, .output = docker_volume_for_tests().dump()});
  host.push({.exit_status = 0, .output = std::string(first_id) + "\n"});
  EXPECT_EQ(backend.launch(valid_spec()), worker_command_result_e::applied);
  ASSERT_EQ(host.calls.size(), 3U);
  const auto &argv = host.calls.back();
  const auto contains = [&](const std::string &arg) { return std::find(argv.begin(), argv.end(), arg) != argv.end(); };
  for (const auto &arg : {"--user=1000:1000", "--userns=host", "--runtime=runc", "--network=none",
                         "--group-add=39", "--group-add=104", "--group-add=105", "--read-only", "--cap-drop=all",
                         "--security-opt=no-new-privileges", "--log-driver=json-file", "--pull=never",
                         "--device=/dev/dri/renderD128:/dev/dri/renderD128:rw",
                         "--mount=type=volume,src=pv-a9f0,dst=/var/lib/polaris-seat,volume-nocopy"}) EXPECT_TRUE(contains(arg)) << arg;
  for (const auto &arg : {"--privileged", "--group-add=keep-groups", "--userns=keep-id", "--remote=false",
                         "--read-only-tmpfs=true", "--pid=private", "--uts=private", "--image-volume=tmpfs"}) EXPECT_FALSE(contains(arg)) << arg;
  for (const auto &arg : argv) {
    EXPECT_EQ(arg.find("/var/run/docker.sock:"), std::string::npos);
    EXPECT_EQ(arg.find("--device=/dev/uinput"), std::string::npos);
    EXPECT_EQ(arg.find("--device=/dev/uhid"), std::string::npos);
  }
}

TEST(MultiseatDockerBackend, SteamOutputMountAndRecoveryRemainBoundToTheAllocatedGeneration) {
  using multiseat::input::device_kind_e;
  using multiseat::input::expected_kernel_name;
  using multiseat::input::expected_phys;
  const auto spec = valid_spec();
  auto allocation = input_allocation_for(spec.identity.seat);
  allocation.plan.steam_input = true;
  auto output = allocation.nodes.back();
  output.kind = device_kind_e::steam_gamepad;
  output.host_path = "/dev/input/event14";
  output.worker_path = output.host_path;
  output.character_minor = 78;
  output.inode = 10078;
  output.kernel_name = expected_kernel_name(allocation.input_seat, output.kind, 0);
  output.phys = expected_phys(allocation.input_seat, output.kind, 0);
  allocation.nodes.push_back(output);
  const auto fingerprint = multiseat::container::input_manifest_fingerprint(allocation);
  ASSERT_TRUE(fingerprint);
  fake_host_t host;
  host.accessible_devices[output.host_path.native()] = device_identity(13, 78);
  fake_input_manifest_source_t inputs;
  inputs.replacement_after_call = 1;
  inputs.replacement = allocation;
  backend_t backend {host, inputs, docker_options_for_tests()};
  host.push({.exit_status = 0, .output = docker_info_for_tests().dump()});
  host.push({.exit_status = 0, .output = docker_volume_for_tests().dump()});
  host.push({.exit_status = 0, .output = std::string(first_id) + "\n"});
  ASSERT_EQ(backend.launch(spec), worker_command_result_e::applied);
  EXPECT_TRUE(has_argument(host.calls.back(), "--device=/dev/input/event14:/dev/input/event14:rw"));
  EXPECT_FALSE(any_argument_contains(host.calls.back(), "/dev/uinput"));
  EXPECT_FALSE(any_argument_contains(host.calls.back(), "/dev/uhid"));
  auto record = docker_container_for(spec, first_id);
  record["Config"]["Labels"]["io.polaris.multiseat.input-manifest"] = *fingerprint;
  record["HostConfig"]["Devices"].push_back({
    {"PathOnHost", "/dev/input/event14"}, {"PathInContainer", "/dev/input/event14"}, {"CgroupPermissions", "rw"}});
  queue_docker_inventory(host, {record});
  EXPECT_EQ(backend.inventory().size(), 1U);
  record["HostConfig"]["Devices"].back()["PathInContainer"] = "/dev/input/event15";
  queue_docker_inventory(host, {record});
  EXPECT_THROW(backend.inventory(), std::runtime_error);
  record["HostConfig"]["Devices"].back()["PathInContainer"] = "/dev/input/event14";
  inputs.replacement->nodes.back().kernel_name = expected_kernel_name("different-generation", output.kind, 0);
  queue_docker_inventory(host, {record});
  EXPECT_THROW(backend.inventory(), std::runtime_error);
}

TEST(MultiseatDockerBackend, MediaRequiresAnExplicitSupportedWorkerAllocation) {
  auto options = docker_options_for_tests();
  EXPECT_FALSE(options.media_enabled);
  options.media_enabled = true;
  options.profiles.front().runtime_profile = runtime_profile_e::gamescope;
  options.workloads = {{workload_kind_e::gamescope, "input-pong-v1"}};
  auto spec = valid_spec();
  spec.runtime_profile = runtime_profile_e::gamescope;
  spec.workload = options.workloads.front();
  spec.display_mode = {1280, 720, 60000, false};
  fake_host_t host;
  fake_input_manifest_source_t inputs;
  backend_t backend {host, inputs, options};
  host.push({.exit_status = 0, .output = docker_info_for_tests().dump()});
  host.push({.exit_status = 0, .output = docker_volume_for_tests().dump()});
  host.push({.exit_status = 0, .output = std::string(first_id) + "\n"});
  ASSERT_EQ(backend.launch(spec), worker_command_result_e::applied);
  ASSERT_FALSE(host.calls.empty());
  EXPECT_EQ(host.calls.back().back(), "--media=enabled");
  auto record = docker_container_for(spec, first_id);
  record["Config"]["Cmd"].push_back("--media=enabled");
  queue_docker_inventory(host, {record});
  EXPECT_EQ(backend.inventory().size(), 1U);
  record["Config"]["Cmd"].erase(record["Config"]["Cmd"].end() - 1);
  queue_docker_inventory(host, {record});
  EXPECT_THROW(backend.inventory(), std::runtime_error);
  spec.display_mode.hdr = true;
  EXPECT_EQ(backend.launch(spec), worker_command_result_e::rejected);
}

namespace {
  json steam_network(std::string profile, std::string id = std::string(64, 'e'), std::string container = {}) {
    json members = json::object();
    if (!container.empty()) members[container] = {{"Name", "worker-test"}};
    return json::array({{{"Name", multiseat::container::profile_network_name(profile)}, {"Id", id},
      {"Driver", "bridge"}, {"Scope", "local"}, {"Internal", false}, {"Attachable", false}, {"Ingress", false},
      {"EnableIPv6", false}, {"Labels", {{"io.polaris.multiseat.profile", profile}}},
      {"Options", {{"com.docker.network.bridge.enable_icc", "false"}, {"com.docker.network.bridge.enable_ip_masquerade", "true"}}},
      {"IPAM", {{"Driver", "default"}, {"Options", nullptr}}}, {"Containers", members}}});
  }
}

TEST(MultiseatProfileNetwork, CreatesOnlyAfterAuthoritativeAbsenceAndVerifiesIdentity) {
  fake_host_t host;
  host.push({.exit_status = 0, .output = "\"bridge\"\n\"host\"\n\"none\"\n"});
  host.push({.exit_status = 0, .output = std::string(64, 'e') + "\n"});
  host.push({.exit_status = 0, .output = steam_network("profile-a").dump()});
  ASSERT_TRUE(multiseat::container::create_profile_network(host, "profile-a"));
  ASSERT_EQ(host.calls.size(), 3U);
  EXPECT_TRUE(has_argument(host.calls[1], "--driver=bridge"));
  EXPECT_TRUE(has_argument(host.calls[1], "--opt=com.docker.network.bridge.enable_icc=false"));
  EXPECT_TRUE(has_argument(host.calls[1], "--label=io.polaris.multiseat.profile=profile-a"));
  EXPECT_EQ(host.calls[1].back(), "pn-profile-a");
}

TEST(MultiseatProfileNetwork, FailedOrConflictingInventoryCannotCreateANetwork) {
  for (const auto &result : std::vector<command_result_t> {
    {.exit_status = 1}, {.exit_status = 0, .timed_out = true}, {.exit_status = 0, .output_truncated = true},
    {.exit_status = 0, .output = "broken"}, {.exit_status = 0, .output = "\"pn-profile-a\"\n"}}) {
    fake_host_t host; host.push(result);
    EXPECT_FALSE(multiseat::container::create_profile_network(host, "profile-a"));
    EXPECT_EQ(host.calls.size(), 1U);
  }
}

TEST(MultiseatProfileNetwork, RejectsChangedPolicyOwnershipAndUnexpectedMembers) {
  for (unsigned kind = 0; kind != 13; ++kind) {
    auto record = steam_network("profile-a");
    auto &network = record[0];
    switch (kind) {
      case 0: network["Name"] = "pn-other"; break;
      case 1: network["Driver"] = "host"; break;
      case 2: network["Scope"] = "swarm"; break;
      case 3: network["Labels"]["io.polaris.multiseat.profile"] = "other"; break;
      case 4: network["Options"]["com.docker.network.bridge.enable_icc"] = "true"; break;
      case 5: network["Options"]["com.docker.network.bridge.name"] = "host-interface"; break;
      case 6: network["Internal"] = true; break;
      case 7: network["EnableIPv6"] = true; break;
      case 8: network["Id"] = "not-an-id"; break;
      case 9: network["Containers"][std::string(64, 'a')] = json::object(); break;
      case 10: network["IPAM"]["Driver"] = "plugin"; break;
      case 11: network["IPAM"]["Options"] = json::array(); break;
      case 12: network["IPAM"]["Options"] = ""; break;
    }
    fake_host_t host; host.push({.exit_status = 0, .output = record.dump()});
    EXPECT_FALSE(multiseat::container::profile_network_id(host, "profile-a", true)) << kind;
  }
  fake_host_t host;
  host.push({.exit_status = 0, .output = steam_network("profile-a", std::string(64, 'e'), std::string(first_id)).dump()});
  EXPECT_FALSE(multiseat::container::profile_network_id(host, "profile-a", false, std::string(second_id)));
}

TEST(MultiseatProfileNetwork, SteamTargetsCannotCarryArbitraryCommands) {
  for (const auto target : {"big-picture-v1", "1", "570", "4294967295"}) EXPECT_TRUE(multiseat::container::valid_steam_target(target));
  for (const auto target : {"", "0", "01", "+1", "-1", "4294967296", "1 --login user", "steam://rungameid/1", "/bin/sh", "$(id)", "1\n"})
    EXPECT_FALSE(multiseat::container::valid_steam_target(target)) << target;
}

TEST(MultiseatDockerBackend, SteamLaunchPinsPrivateBridgeAndRejectsReplacementBeforeRun) {
  for (const bool replaced : {false, true}) {
    auto options = docker_options_for_tests(); options.media_enabled = true;
    options.profiles.front().profile_key = "profile-alpha";
    options.workloads = {{workload_kind_e::steam, "big-picture-v1"}};
    auto spec = valid_spec(); spec.workload = options.workloads.front();
    spec.profile_key = options.profiles.front().profile_key;
    spec.display_mode.hdr = false;
    fake_host_t host; fake_input_manifest_source_t inputs;
    backend_t backend {host, inputs, options};
    host.push({.exit_status = 0, .output = docker_info_for_tests().dump()});
    host.push({.exit_status = 0, .output = docker_volume_for_tests().dump()});
    host.push({.exit_status = 0, .output = steam_network(spec.profile_key).dump()});
    host.push({.exit_status = 0, .output = steam_network(spec.profile_key, std::string(64, replaced ? 'f' : 'e')).dump()});
    if (!replaced) host.push({.exit_status = 0, .output = std::string(first_id) + "\n"});
    EXPECT_EQ(backend.launch(spec), replaced ? worker_command_result_e::rejected : worker_command_result_e::applied);
    ASSERT_EQ(host.calls.size(), replaced ? 4U : 5U);
    if (!replaced) {
      EXPECT_TRUE(has_argument(host.calls.back(), "--network=" + std::string(64, 'e')));
      EXPECT_FALSE(has_argument(host.calls.back(), "--network=host"));
      EXPECT_TRUE(has_argument(host.calls.back(), "--security-opt=seccomp=" +
        std::string(multiseat::container::steam_seccomp_path)));
      EXPECT_FALSE(has_argument(host.calls.back(), "--security-opt=seccomp=unconfined"));
      EXPECT_EQ(host.calls.back().back(), "--media=enabled");
    }
  }
}

TEST(MultiseatDockerBackend, SteamLaunchRequiresTheInstalledExactSeccompPolicy) {
  auto options = docker_options_for_tests(); options.media_enabled = true;
  options.profiles.front().profile_key = "profile-alpha";
  options.workloads = {{workload_kind_e::steam, "big-picture-v1"}};
  auto spec = valid_spec(); spec.workload = options.workloads.front();
  spec.profile_key = options.profiles.front().profile_key; spec.display_mode.hdr = false;
  fake_host_t host; host.seccomp_ready = false;
  fake_input_manifest_source_t inputs;
  backend_t backend {host, inputs, options};
  host.push({.exit_status = 0, .output = docker_info_for_tests().dump()});
  host.push({.exit_status = 0, .output = docker_volume_for_tests().dump()});
  host.push({.exit_status = 0, .output = steam_network(spec.profile_key).dump()});
  host.push({.exit_status = 0, .output = steam_network(spec.profile_key).dump()});
  host.push({.exit_status = 0, .output = std::string(first_id) + "\n"});
  EXPECT_EQ(backend.launch(spec), worker_command_result_e::rejected);
  for (const auto &call : host.calls) EXPECT_FALSE(has_argument(call, "run"));
}

TEST(MultiseatDockerBackend, SteamRecoveryChecksExactNetworkAndDeniesPublishedPorts) {
  for (unsigned kind = 0; kind != 9; ++kind) {
    auto options = docker_options_for_tests(); options.media_enabled = true;
    options.profiles.front().profile_key = "profile-alpha";
    options.workloads = {{workload_kind_e::steam, "big-picture-v1"}};
    auto spec = valid_spec(); spec.workload = options.workloads.front();
    spec.profile_key = options.profiles.front().profile_key;
    spec.display_mode.hdr = false;
    fake_host_t host; fake_input_manifest_source_t inputs;
    backend_t backend {host, inputs, options};
    auto record = docker_container_for(spec, first_id);
    record["Config"]["Cmd"].push_back("--media=enabled");
    record["HostConfig"]["SecurityOpt"].push_back("seccomp=" +
      json::parse(multiseat::container::steam_seccomp_data).dump());
    record["HostConfig"].update({{"NetworkMode", std::string(64, 'e')}, {"PublishAllPorts", false},
      {"PortBindings", nullptr}, {"Dns", nullptr}, {"DnsOptions", nullptr}, {"DnsSearch", nullptr}});
    record["NetworkSettings"] = {{"Ports", nullptr}, {"Networks", {
      {multiseat::container::profile_network_name(spec.profile_key), {{"NetworkID", std::string(64, 'e')}}}}}};
    if (kind == 1) record["HostConfig"]["NetworkMode"] = "host";
    if (kind == 2) record["HostConfig"]["PortBindings"] = {{"22/tcp", {{{"HostPort", "2222"}}}}};
    if (kind == 3) record["NetworkSettings"]["Networks"]["other"] = {{"NetworkID", std::string(64, 'f')}};
    if (kind == 4) record["HostConfig"]["Dns"] = {"192.0.2.53"};
    if (kind == 5) record["HostConfig"]["SecurityOpt"].erase(1);
    if (kind == 6) record["HostConfig"]["SecurityOpt"][1] = "seccomp=unconfined";
    if (kind == 7) record["HostConfig"]["SecurityOpt"].push_back("seccomp=unconfined");
    if (kind == 8) {
      auto changed = json::parse(multiseat::container::steam_seccomp_data);
      changed["defaultAction"] = "SCMP_ACT_ALLOW";
      record["HostConfig"]["SecurityOpt"][1] = "seccomp=" + changed.dump();
    }
    queue_docker_inventory(host, {record});
    host.push({.exit_status = 0, .output = steam_network(spec.profile_key, std::string(64, 'e'), std::string(first_id)).dump()});
    if (kind == 0) EXPECT_EQ(backend.inventory().size(), 1U);
    else EXPECT_THROW(backend.inventory(), std::runtime_error) << kind;
  }
}

TEST(MultiseatDockerBackend, SteamStopSurvivesMissingNetworkAuthority) {
  auto options = docker_options_for_tests(); options.media_enabled = true;
  options.profiles.front().profile_key = "profile-alpha";
  options.workloads = {{workload_kind_e::steam, "big-picture-v1"}};
  auto spec = valid_spec(); spec.workload = options.workloads.front();
  spec.profile_key = options.profiles.front().profile_key;
  spec.display_mode.hdr = false;
  fake_host_t host; fake_input_manifest_source_t inputs;
  backend_t backend {host, inputs, options};
  auto record = docker_container_for(spec, first_id);
  record["Config"]["Cmd"].push_back("--media=enabled");
  record["HostConfig"]["SecurityOpt"].push_back("seccomp=" +
    json::parse(multiseat::container::steam_seccomp_data).dump());
  // A missing bridge makes normal recovery fail closed. Cleanup must still
  // use the exact inspected container ID without depending on that bridge.
  queue_docker_inventory(host, {record});
  host.push({.exit_status = 1});
  EXPECT_THROW(backend.inventory(), std::runtime_error);
  const auto previous_calls = host.calls.size();
  queue_docker_inventory(host, {record});
  host.push({.exit_status = 0});
  ASSERT_EQ(backend.stop(spec.identity, worker_stop_mode_e::force), worker_command_result_e::applied);
  ASSERT_EQ(host.calls.size(), previous_calls + 4U);
  EXPECT_TRUE(has_argument(host.calls.back(), "--force"));
  EXPECT_EQ(host.calls.back().back(), first_id);
}

TEST(MultiseatDockerBackend, AdmitsOnlyTheReviewedSelinuxTypeAndChecksItOnRecovery) {
  fake_host_t host;
  fake_input_manifest_source_t inputs;
  auto options = docker_options_for_tests();
  options.selinux_type = "unconfined_t";
  EXPECT_THROW((backend_t {host, inputs, options}), std::invalid_argument);
  options.selinux_type = "polaris_nvidia_worker_t";
  backend_t backend {host, inputs, options};
  auto record = docker_container_for(valid_spec(), first_id);
  record["HostConfig"]["SecurityOpt"].push_back("label=type:polaris_nvidia_worker_t");
  queue_docker_inventory(host, {record});
  ASSERT_EQ(backend.inventory().size(), 1U);
  record["HostConfig"]["SecurityOpt"] = {"no-new-privileges"};
  queue_docker_inventory(host, {record});
  EXPECT_THROW(backend.inventory(), std::runtime_error);
}

TEST(MultiseatDockerBackend, OfflineImageIdIsImmutableAndBoundToTheExecutedConfig) {
  for (const bool wrong_image : {false, true}) {
    fake_host_t host;
    fake_input_manifest_source_t inputs;
    auto options = docker_options_for_tests();
    const auto image_id = std::string("sha256:") + std::string(64, 'a');
    options.profiles.front().image_reference = image_id;
    backend_t backend {host, inputs, options};
    auto record = docker_container_for(valid_spec(), first_id);
    record["Config"]["Labels"]["io.polaris.multiseat.runtime-image"] = image_id;
    record["Config"]["Image"] = image_id;
    record["Image"] = wrong_image ? std::string("sha256:") + std::string(64, 'b') : image_id;
    queue_docker_inventory(host, {record});
    if (wrong_image) EXPECT_THROW(backend.inventory(), std::runtime_error);
    else ASSERT_EQ(backend.inventory().size(), 1U);
  }
  for (const auto image : {"worker:latest", "sha256:abc", "sha256:"}) {
    fake_host_t host;
    fake_input_manifest_source_t inputs;
    auto options = docker_options_for_tests();
    options.profiles.front().image_reference = image;
    EXPECT_THROW((backend_t {host, inputs, options}), std::invalid_argument);
  }
}

TEST(MultiseatDockerBackend, RejectsUnsupportedDaemonAndUnpreparedOrRedirectedVolumes) {
  for (int variant = 0; variant < 7; ++variant) {
    SCOPED_TRACE(variant);
    fake_host_t host;
    fake_input_manifest_source_t inputs;
    backend_t backend {host, inputs, docker_options_for_tests()};
    auto info = docker_info_for_tests();
    if (variant == 0) info["SecurityOptions"].push_back("name=rootless");
    if (variant == 1) info["OSType"] = "windows";
    if (variant == 2) info["Runtimes"]["runc"]["path"] = "/opt/untrusted-runtime";
    host.push({.exit_status = 0, .output = info.dump()});
    if (variant >= 3) {
      auto volume = docker_volume_for_tests();
      if (variant == 3) volume["Options"] = {{"device", "/"}, {"type", "none"}, {"o", "bind"}};
      if (variant == 4) volume["Name"] = "pv-b8e1";
      if (variant == 5) volume["Driver"] = "untrusted-plugin";
      host.push({.exit_status = variant == 6 ? 1 : 0, .output = volume.dump()});
    }
    EXPECT_NE(backend.launch(valid_spec()), worker_command_result_e::applied);
    EXPECT_EQ(host.calls.size(), variant < 3 ? 1U : 2U);
  }
}

TEST(MultiseatDockerBackend, InventoryAuthenticatesTwoSeatsWithoutPodmanOciFiles) {
  fake_host_t host;
  fake_input_manifest_source_t inputs;
  backend_t backend {host, inputs, docker_options_for_tests()};
  queue_docker_inventory(host, {docker_container_for(valid_spec(), first_id),
    docker_container_for(valid_spec(1, 2, "polaris-worker-controller-a1b2-2", "profile beta", "heroic-game"), second_id)});
  const auto records = backend.inventory();
  ASSERT_EQ(records.size(), 2U);
  EXPECT_EQ(records[0].state, worker_observed_state_e::ready);
  EXPECT_EQ(records[1].state, worker_observed_state_e::ready);
  EXPECT_NE(records[0].identity, records[1].identity);
  EXPECT_EQ(host.owned_file_reads, 0U);
}

TEST(MultiseatDockerBackend, InventoryRejectsDeviceMountIdentityAndPrivilegeDrift) {
  const std::vector<std::function<void(json &)>> mutations {
    [](auto &r) { r["HostConfig"]["Privileged"] = true; },
    [](auto &r) { r["HostConfig"]["CapAdd"] = {"SYS_ADMIN"}; },
    [](auto &r) { r["HostConfig"]["CapDrop"] = nullptr; },
    [](auto &r) { r["HostConfig"]["SecurityOpt"] = {"label=disable"}; },
    [](auto &r) { r["HostConfig"]["UsernsMode"] = ""; },
    [](auto &r) { r["HostConfig"]["Runtime"] = "nvidia"; },
    [](auto &r) { r["HostConfig"]["NetworkMode"] = "host"; },
    [](auto &r) { r["HostConfig"]["PidMode"] = "host"; },
    [](auto &r) { r["HostConfig"]["IpcMode"] = "host"; },
    [](auto &r) { r["HostConfig"]["UTSMode"] = "host"; },
    [](auto &r) { r["HostConfig"]["CgroupnsMode"] = "host"; },
    [](auto &r) { r["HostConfig"]["GroupAdd"] = {"39"}; },
    [](auto &r) { r["HostConfig"]["GroupAdd"].push_back("39"); },
    [](auto &r) { r["Config"]["User"] = "0"; },
    [](auto &r) { r["Config"]["Image"] = "untrusted:latest"; },
    [](auto &r) { r["Config"]["Cmd"] = {"run", "--workload-kind=steam", "--workload-id=other"}; },
    [](auto &r) { r["Config"]["Env"].push_back("POLARIS_INPUT_SEAT=another-seat"); },
    [](auto &r) { r["Config"]["Healthcheck"]["Test"] = {"CMD-SHELL", "true"}; },
    [](auto &r) { r["HostConfig"]["DeviceRequests"] = json::array({{{"Count", -1}}}); },
    [](auto &r) { r["HostConfig"]["DeviceCgroupRules"] = {"c 13:* rw"}; },
    [](auto &r) { r["HostConfig"]["Devices"][0]["CgroupPermissions"] = "rwm"; },
    [](auto &r) { r["HostConfig"]["Devices"][0].erase("CgroupPermissions"); },
    [](auto &r) { r["HostConfig"]["Devices"][0]["PathOnHost"] = "/dev/dri/renderD129"; },
    [](auto &r) { r["HostConfig"]["Devices"].push_back(r["HostConfig"]["Devices"][0]); },
    [](auto &r) { r["Mounts"][0]["Name"] = "pv-b8e1"; },
    [](auto &r) { r["Mounts"][1]["Source"] = "/var/run/docker.sock"; },
    [](auto &r) { r["Mounts"][2]["RW"] = true; },
    [](auto &r) { r["Mounts"][3]["RW"] = true; },
    [](auto &r) { r["Mounts"][1]["Propagation"] = "rshared"; },
    [](auto &r) { r["Mounts"].push_back({{"Type", "bind"}, {"Source", "/dev"}, {"Destination", "/host-dev"}}); },
    [](auto &r) { r["HostConfig"]["Mounts"][0]["VolumeOptions"]["NoCopy"] = false; },
    [](auto &r) { r["HostConfig"]["Tmpfs"]["/run/polaris"] = "rw,mode=0777"; },
    [](auto &r) { r["HostConfig"]["LogConfig"]["Config"]["max-file"] = "100"; },
  };
  for (std::size_t i = 0; i < mutations.size(); ++i) {
    SCOPED_TRACE(i);
    fake_host_t host;
    fake_input_manifest_source_t inputs;
    backend_t backend {host, inputs, docker_options_for_tests()};
    auto record = docker_container_for(valid_spec(), first_id);
    mutations[i](record);
    queue_docker_inventory(host, {record});
    EXPECT_THROW(backend.inventory(), std::runtime_error);
  }
}

TEST(MultiseatDockerBackend, ChangedGroupsCancelLaunchAfterEngineAndVolumeChecks) {
  fake_host_t host;
  fake_input_manifest_source_t inputs;
  host.groups_change_on_recheck = true;
  backend_t backend {host, inputs, docker_options_for_tests()};
  host.push({.exit_status = 0, .output = docker_info_for_tests().dump()});
  host.push({.exit_status = 0, .output = docker_volume_for_tests().dump()});
  EXPECT_EQ(backend.launch(valid_spec()), worker_command_result_e::rejected);
  EXPECT_EQ(host.calls.size(), 2U);
}

TEST(MultiseatDockerBackend, CleanupKeepsExactIdentityWhenDeviceAndGroupAccessAreLost) {
  fake_host_t host;
  fake_input_manifest_source_t inputs;
  const auto first = valid_spec();
  const auto second = valid_spec(1, 2, "polaris-worker-controller-a1b2-2", "profile beta", "heroic-game");
  backend_t backend {host, inputs, docker_options_for_tests()};
  host.runtime_ready = false;
  host.groups = std::nullopt;
  host.accessible_devices.clear();
  queue_docker_inventory(host, {docker_container_for(first, first_id), docker_container_for(second, second_id)});
  host.push({.exit_status = 0});
  EXPECT_EQ(backend.stop(first.identity, worker_stop_mode_e::force), worker_command_result_e::applied);
  const auto &argv = host.calls.back();
  ASSERT_GE(argv.size(), 3U);
  EXPECT_EQ(argv[argv.size()-3], "rm");
  EXPECT_EQ(argv[argv.size()-2], "--force");
  EXPECT_EQ(argv.back(), first_id);
  EXPECT_EQ(std::find(argv.begin(), argv.end(), second_id), argv.end());
}

TEST(MultiseatDockerBackend, FailedLaunchReconcilesOnlyTheMatchingVerifiedWorker) {
  fake_host_t host;
  fake_input_manifest_source_t inputs;
  const auto spec = valid_spec();
  backend_t backend {host, inputs, docker_options_for_tests()};
  host.push({.exit_status = 0, .output = docker_info_for_tests().dump()});
  host.push({.exit_status = 0, .output = docker_volume_for_tests().dump()});
  host.push({.exit_status = 125});
  queue_docker_inventory(host, {docker_container_for(spec, first_id)});
  EXPECT_EQ(backend.launch(spec), worker_command_result_e::already_applied);
}

namespace {
  // A host-driver runtime: the image carries no NVIDIA userspace, so profile
  // alpha borrows this machine's at the paths the loader resolves them by.
  options_t host_driver_options_for_tests() {
    auto options = docker_options_for_tests();
    options.profiles.front().host_driver_libraries = true;
    options.host_driver = {
      .driver_version = "615.71.09",
      .contract = 1,
      .mounts = {
        {"/usr/lib/x86_64-linux-gnu/libcuda.so.1", "/usr/lib64/libcuda.so.615.71.09"},
        {"/usr/lib/i386-linux-gnu/libcuda.so.1", "/usr/lib/libcuda.so.615.71.09"},
        {"/usr/share/vulkan/icd.d/nvidia_icd.json",
         "/home/operator/.config/polaris/spaces-graphics/615.71.09/nvidia_icd.json"},
      },
    };
    return options;
  }

  json host_driver_container_for(const worker_launch_spec_t &spec, const std::string &id) {
    auto record = docker_container_for(spec, id);
    for (const auto &mount : host_driver_options_for_tests().host_driver.mounts)
      record["Mounts"].push_back({{"Type", "bind"}, {"RW", false}, {"Source", mount.host_path.native()},
        {"Destination", mount.destination}, {"Propagation", "rprivate"}});
    return record;
  }
}

TEST(MultiseatDockerBackend, PublishesBorrowedDriverFilesReadOnlyAtTheirContainerPaths) {
  fake_host_t host;
  backend_t backend {host, input_manifests_for_tests(), host_driver_options_for_tests()};
  host.push({.exit_status = 0, .output = docker_info_for_tests().dump()});
  host.push({.exit_status = 0, .output = docker_volume_for_tests().dump()});
  host.push({.exit_status = 0, .output = std::string {first_id} + "\n"});

  ASSERT_EQ(backend.launch(valid_spec()), worker_command_result_e::applied);
  const auto &argv = host.calls.back();

  EXPECT_TRUE(has_argument(argv,
    "--mount=type=bind,src=/usr/lib64/libcuda.so.615.71.09,dst=/usr/lib/x86_64-linux-gnu/libcuda.so.1,ro=true"));
  EXPECT_TRUE(has_argument(argv,
    "--mount=type=bind,src=/usr/lib/libcuda.so.615.71.09,dst=/usr/lib/i386-linux-gnu/libcuda.so.1,ro=true"));
  EXPECT_TRUE(has_argument(argv,
    "--mount=type=bind,src=/home/operator/.config/polaris/spaces-graphics/615.71.09/nvidia_icd.json,"
    "dst=/usr/share/vulkan/icd.d/nvidia_icd.json,ro=true"));
  // A borrowed file is never relabelled: its source is the host's own /usr,
  // and SELinux policy grants the read instead. Polaris's own directories
  // still carry ,Z, so only the driver mounts are checked here.
  for (const auto &argument : argv) {
    if (argument.find("/usr/lib64/") == std::string::npos &&
        argument.find("/spaces-graphics/") == std::string::npos &&
        argument.find("/usr/lib/libcuda") == std::string::npos) continue;
    EXPECT_EQ(argument.find(",z"), std::string::npos) << argument;
    EXPECT_EQ(argument.find(",Z"), std::string::npos) << argument;
  }
}

TEST(MultiseatDockerBackend, GivesNoBorrowedDriverFilesToARuntimeThatCarriesItsOwn) {
  auto options = host_driver_options_for_tests();
  options.profiles.front().host_driver_libraries = false;
  fake_host_t host;
  backend_t backend {host, input_manifests_for_tests(), options};
  host.push({.exit_status = 0, .output = docker_info_for_tests().dump()});
  host.push({.exit_status = 0, .output = docker_volume_for_tests().dump()});
  host.push({.exit_status = 0, .output = std::string {first_id} + "\n"});

  ASSERT_EQ(backend.launch(valid_spec()), worker_command_result_e::applied);
  const auto &argv = host.calls.back();

  for (const auto &argument : argv)
    EXPECT_EQ(argument.find("libcuda.so"), std::string::npos) << argument;
}

TEST(MultiseatDockerBackend, AcceptsAContainerCarryingExactlyTheBorrowedDriverFiles) {
  fake_host_t host;
  backend_t backend {host, input_manifests_for_tests(), host_driver_options_for_tests()};
  queue_docker_inventory(host, {host_driver_container_for(valid_spec(), first_id)});

  EXPECT_NO_THROW(backend.inventory());
}

TEST(MultiseatDockerBackend, RejectsAnInjectedMissingWritableOrRepointedDriverFile) {
  const std::vector<std::function<void(json &)>> mutations {
    // An extra driver-shaped bind nobody asked for.
    [](auto &r) { r["Mounts"].push_back({{"Type", "bind"}, {"RW", false},
      {"Source", "/tmp/libnvidia-evil.so.1"}, {"Destination", "/usr/lib/x86_64-linux-gnu/libnvidia-ml.so.1"},
      {"Propagation", "rprivate"}}); },
    // One of ours missing.
    [](auto &r) { r["Mounts"].erase(r["Mounts"].size() - 1); },
    // Ours, but writable.
    [](auto &r) { r["Mounts"][r["Mounts"].size() - 1]["RW"] = true; },
    // Ours, pointed at a different file in the container.
    [](auto &r) { r["Mounts"][r["Mounts"].size() - 3]["Destination"] = "/usr/lib/x86_64-linux-gnu/libc.so.6"; },
    // Ours, but sourced from somewhere else on the host.
    [](auto &r) { r["Mounts"][r["Mounts"].size() - 3]["Source"] = "/tmp/libcuda.so.1"; },
    // Ours, with propagation that would let the host change it underneath.
    [](auto &r) { r["Mounts"][r["Mounts"].size() - 3]["Propagation"] = "rshared"; },
  };
  for (std::size_t index = 0; index < mutations.size(); ++index) {
    SCOPED_TRACE(index);
    fake_host_t host;
    backend_t backend {host, input_manifests_for_tests(), host_driver_options_for_tests()};
    auto record = host_driver_container_for(valid_spec(), first_id);
    mutations[index](record);
    queue_docker_inventory(host, {record});
    EXPECT_THROW(backend.inventory(), std::runtime_error);
  }
}

TEST(MultiseatDockerBackend, RefusesHostDriverOptionsThatEscapeTheLoaderDirectories) {
  const std::vector<std::function<void(options_t &)>> mutations {
    [](auto &o) { o.host_driver.mounts.front().destination = "/var/lib/polaris-seat/libcuda.so.1"; },
    [](auto &o) { o.host_driver.mounts.front().destination = "/mnt/games/library-a"; },
    [](auto &o) { o.host_driver.mounts.front().destination = "/usr/lib/x86_64-linux-gnu/nested/libcuda.so.1"; },
    [](auto &o) { o.host_driver.mounts.front().host_path = "usr/lib64/libcuda.so.1"; },
    [](auto &o) { o.host_driver.mounts.push_back(o.host_driver.mounts.front()); },
    [](auto &o) { o.host_driver.driver_version.clear(); },
    [](auto &o) { o.host_driver.contract = 2; },
    [](auto &o) { o.engine = multiseat::container::engine_e::podman; },
    [](auto &o) { o.host_driver.mounts.clear(); },
  };
  for (std::size_t index = 0; index < mutations.size(); ++index) {
    SCOPED_TRACE(index);
    fake_host_t host;
    auto options = host_driver_options_for_tests();
    mutations[index](options);
    EXPECT_THROW((backend_t {host, input_manifests_for_tests(), options}), std::invalid_argument);
  }
}

#endif
