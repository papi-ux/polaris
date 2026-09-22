/**
 * @file src/platform/linux/multiseat_container_backend.cpp
 * @brief Docker worker backend with retained Podman lifecycle support.
 */
#include "multiseat_container_backend.h"
#include "multiseat_steam_seccomp.h"
#include "multiseat_profile_network.h"
#include "multiseat_worker_authority.h"

#ifdef __linux__

#include <nlohmann/json.hpp>
#include <openssl/evp.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <limits>
#include <optional>
#include <set>
#include <stdexcept>
#include <string_view>
#include <unordered_set>
#include <utility>

namespace multiseat::container {
  namespace {
    using json = nlohmann::json;
    using namespace std::literals;

    // Which way a bounded engine command failed, for the words of the error it raises.
    std::string describe_command_failure(const command_result_t &result) {
      if (result.timed_out) return "timed out";
      if (result.output_truncated) return "returned more output than Polaris reads";
      return "exited with status " + std::to_string(result.exit_status);
    }

    constexpr auto label_protocol = "io.polaris.multiseat.protocol"sv;
    constexpr auto label_deployment = "io.polaris.multiseat.deployment"sv;
    constexpr auto label_controller = "io.polaris.multiseat.controller"sv;
    constexpr auto label_gpu = "io.polaris.multiseat.gpu"sv;
    constexpr auto label_slot = "io.polaris.multiseat.slot"sv;
    constexpr auto label_generation = "io.polaris.multiseat.generation"sv;
    constexpr auto label_worker = "io.polaris.multiseat.worker"sv;
    constexpr auto label_runtime = "io.polaris.multiseat.runtime"sv;
    constexpr auto label_capture_wayland = "io.polaris.multiseat.capture-wayland"sv;
    constexpr auto label_wayland = "io.polaris.multiseat.wayland"sv;
    constexpr auto label_audio = "io.polaris.multiseat.audio"sv;
    constexpr auto label_input = "io.polaris.multiseat.input"sv;
    constexpr auto label_input_manifest = "io.polaris.multiseat.input-manifest"sv;
    constexpr auto label_render_node = "io.polaris.multiseat.render-node"sv;
    constexpr auto label_runtime_profile = "io.polaris.multiseat.runtime-profile"sv;
    constexpr auto label_workload_kind = "io.polaris.multiseat.workload-kind"sv;
    constexpr auto label_workload_target = "io.polaris.multiseat.workload-target"sv;
    constexpr auto label_display_topology = "io.polaris.multiseat.display-topology"sv;
    constexpr auto label_media_pipeline = "io.polaris.multiseat.media-pipeline"sv;
    constexpr auto label_runtime_image = "io.polaris.multiseat.runtime-image"sv;
    constexpr auto label_display_width = "io.polaris.multiseat.display-width"sv;
    constexpr auto label_display_height = "io.polaris.multiseat.display-height"sv;
    constexpr auto label_display_refresh = "io.polaris.multiseat.display-refresh-millihz"sv;
    constexpr auto label_display_hdr = "io.polaris.multiseat.display-hdr"sv;
    constexpr auto label_compositor = "io.polaris.multiseat.compositor"sv;
    constexpr auto label_encoders = "io.polaris.multiseat.encoders"sv;
    constexpr auto label_volume = "io.polaris.multiseat.volume"sv;
    constexpr auto capability_file = worker_ipc::authority_capability_file_name;
    constexpr auto ipc_directory = worker_ipc::authority_ipc_directory_name;
    constexpr auto auth_directory = worker_ipc::authority_auth_directory_name;
    constexpr auto container_ipc_directory = "/run/polaris-ipc"sv;
    constexpr auto container_auth_directory = "/run/polaris-auth"sv;
    constexpr auto podman_init_destination = "/run/podman-init"sv;
    constexpr auto profile_volume_destination = "/var/lib/polaris-seat"sv;
    constexpr auto shared_game_mount_root = "/mnt/games/"sv;
    // Where a borrowed driver file may land: the two loader directories and the
    // vendor descriptions beside them. Nothing else in the image is writable
    // over, and a Space's own home, sockets and game shares are elsewhere.
    constexpr auto host_driver_amd64_root = "/usr/lib/x86_64-linux-gnu/"sv;
    constexpr auto host_driver_i386_root = "/usr/lib/i386-linux-gnu/"sv;
    constexpr auto host_driver_share_root = "/usr/share/"sv;
    constexpr std::size_t maximum_host_driver_mounts = 128;
    constexpr auto host_driver_cache_directory = "/etc/polaris-ld"sv;
    /**
     * A library lands directly in its loader directory; a vendor description
     * lands under /usr/share in the directory its loader reads.
     */
    bool mount_destination_within(
      const std::filesystem::path &destination, std::string_view prefix, bool allow_subdirectories
    ) {
      const auto &text = destination.native();
      if (!text.starts_with(prefix) || text.size() <= prefix.size()) return false;
      return allow_subdirectories || text.find('/', prefix.size()) == std::string::npos;
    }
    constexpr std::size_t maximum_inspected_devices =
      input::maximum_input_allocations + 64;
    constexpr std::size_t maximum_runtime_spec_mounts =
      maximum_inspected_devices + 64;

    bool ascii_alphanumeric(char value) {
      return (value >= 'a' && value <= 'z') ||
             (value >= 'A' && value <= 'Z') ||
             (value >= '0' && value <= '9');
    }

    bool opaque_name_token(std::string_view value, std::size_t max_size = 128) {
      return !value.empty() &&
             value.size() <= max_size &&
             ascii_alphanumeric(value.front()) &&
             std::all_of(
               value.begin(),
               value.end(),
               [](char character) {
                 return ascii_alphanumeric(character) ||
                        character == '-' ||
                        character == '_' ||
                        character == '.';
               }
             );
    }

    bool opaque_reference(std::string_view value) {
      return !value.empty() &&
             value.size() <= 256 &&
             std::all_of(
               value.begin(),
               value.end(),
               [](unsigned char character) {
                 return character >= 0x20 && character <= 0x7e;
               }
             );
    }

    bool safe_path(const std::filesystem::path &path) {
      if (!path.is_absolute() || path.empty() || path.lexically_normal() != path) {
        return false;
      }
      const auto value = path.native();
      return value.find(',') == std::string::npos &&
             value.find(':') == std::string::npos &&
             value.find('\n') == std::string::npos &&
             value.find('\r') == std::string::npos;
    }

    bool device_path(const std::filesystem::path &path) {
      return safe_path(path) && path.native().starts_with("/dev/");
    }

    bool normalized_absolute_path(const std::filesystem::path &path) {
      return path.is_absolute() && path.lexically_normal() == path;
    }

    std::vector<std::string> path_components(const std::filesystem::path &path) {
      std::vector<std::string> components;
      for (const auto &component : path) {
        components.push_back(component.native());
      }
      return components;
    }

    /**
     * Podman stores every container's OCI runtime spec at
     * `<storage>/<driver>-containers/<id>/userdata/config.json`. The path is
     * accepted only in that shape, with the record's own immutable ID as the
     * container directory, so the spec read is bound to the inspected record.
     */
    bool runtime_spec_path(
      const std::filesystem::path &path,
      std::string_view container_id_value
    ) {
      if (!normalized_absolute_path(path)) {
        return false;
      }
      const auto components = path_components(path);
      const auto count = components.size();
      return count >= 5 &&
             components[count - 1] == "config.json" &&
             components[count - 2] == "userdata" &&
             components[count - 3] == container_id_value &&
             components[count - 4].ends_with("-containers");
    }

    /**
     * Podman keeps a container's own files (resolv.conf, hosts, hostname,
     * .containerenv, secrets, shm) under `<root>/<driver>-containers/<id>/
     * userdata/`, below the storage root or the run root.
     */
    bool own_container_userdata_path(
      const std::filesystem::path &path,
      std::string_view container_id_value
    ) {
      if (!normalized_absolute_path(path)) {
        return false;
      }
      const auto components = path_components(path);
      for (std::size_t index = 1; index + 3 < components.size(); ++index) {
        if (components[index].ends_with("-containers") &&
            components[index + 1] == container_id_value &&
            components[index + 2] == "userdata") {
          return true;
        }
      }
      return false;
    }

    /** Where Podman lands its own per-container files inside the worker. */
    bool podman_own_destination(std::string_view destination) {
      return destination == "/etc/resolv.conf" || destination == "/etc/hosts" ||
             destination == "/etc/hostname" || destination == "/dev/shm" ||
             destination == "/run/.containerenv" || destination == "/run/secrets";
    }

    /** Destinations that shadow or expose the worker's device set. */
    bool device_directory_destination(std::string_view destination) {
      return destination == "/dev/dri" || destination == "/dev/input" ||
             destination.starts_with("/dev/dri/") ||
             destination.starts_with("/dev/input/");
    }

    bool lowercase_sha256(std::string_view value) {
      return value.size() == 64 &&
             std::all_of(
               value.begin(),
               value.end(),
               [](char character) {
                 return (character >= '0' && character <= '9') ||
                        (character >= 'a' && character <= 'f');
               }
             );
    }

    void append_fingerprint_field(std::string &record, std::string_view value) {
      record += std::to_string(value.size());
      record.push_back(':');
      record.append(value);
    }

    template<class Integer>
    void append_fingerprint_integer(std::string &record, Integer value) {
      append_fingerprint_field(record, std::to_string(value));
    }

    bool same_character_device(
      const character_device_identity_t &left,
      const character_device_identity_t &right
    ) {
      return left.character_major == right.character_major &&
             left.character_minor == right.character_minor;
    }

    bool valid_character_device_identity(
      const character_device_identity_t &identity
    ) {
      return identity.filesystem_device != 0 && identity.inode != 0;
    }

    bool exact_input_identity(
      const character_device_identity_t &identity,
      const input::device_node_t &node
    ) {
      return identity.filesystem_device == node.filesystem_device &&
             identity.inode == node.inode &&
             identity.character_major == node.character_major &&
             identity.character_minor == node.character_minor;
    }

    bool exact_input_snapshot(
      const input::kernel_node_snapshot_t &snapshot,
      const input::device_node_t &node
    ) {
      return snapshot.host_path == node.host_path &&
             snapshot.filesystem_device == node.filesystem_device &&
             snapshot.inode == node.inode &&
             snapshot.character_major == node.character_major &&
             snapshot.character_minor == node.character_minor &&
             snapshot.kernel_name == node.kernel_name &&
             snapshot.phys == node.phys &&
             snapshot.host_seat == node.host_seat;
    }

    bool pinned_image_reference(std::string_view value, engine_e engine) {
      if (engine == engine_e::docker && value.starts_with("sha256:")) {
        return lowercase_sha256(value.substr(7));
      }
      constexpr auto marker = "@sha256:"sv;
      const auto marker_position = value.rfind(marker);
      if (marker_position == std::string_view::npos || marker_position == 0) {
        return false;
      }
      const auto digest = value.substr(marker_position + marker.size());
      const auto image_name = value.substr(0, marker_position);
      return ascii_alphanumeric(image_name.front()) &&
             image_name.back() != '/' &&
             image_name.find(":/") == std::string_view::npos &&
             image_name.find("//") == std::string_view::npos &&
             std::all_of(
               image_name.begin(),
               image_name.end(),
               [](char character) {
                 return ascii_alphanumeric(character) ||
                        character == '-' ||
                        character == '_' ||
                        character == '.' ||
                        character == '/' ||
                        character == ':';
               }
             ) &&
             digest.size() == 64 &&
             std::all_of(
               digest.begin(),
               digest.end(),
               [](char character) {
                 return (character >= '0' && character <= '9') ||
                        (character >= 'a' && character <= 'f');
               }
             ) &&
             opaque_reference(value);
    }

    bool concrete_compositor(compositor_e compositor) {
      return compositor == compositor_e::gamescope ||
             compositor == compositor_e::sway ||
             compositor == compositor_e::labwc;
    }

    bool concrete_runtime_profile(runtime_profile_e profile) {
      return profile == runtime_profile_e::gamescope ||
             profile == runtime_profile_e::steam ||
             profile == runtime_profile_e::heroic ||
             profile == runtime_profile_e::lutris;
    }

    std::string workload_kind_name(workload_kind_e kind) {
      switch (kind) {
        case workload_kind_e::gamescope:
          return "gamescope";
        case workload_kind_e::steam:
          return "steam";
        case workload_kind_e::heroic:
          return "heroic";
        case workload_kind_e::lutris:
          return "lutris";
        case workload_kind_e::unknown:
          break;
      }
      return {};
    }

    bool valid_workload_kind_name(std::string_view value) {
      return value == "gamescope" || value == "steam" ||
             value == "heroic" || value == "lutris";
    }

    workload_kind_e workload_kind_from_name(std::string_view value) {
      if (value == "gamescope") {
        return workload_kind_e::gamescope;
      }
      if (value == "steam") {
        return workload_kind_e::steam;
      }
      if (value == "heroic") {
        return workload_kind_e::heroic;
      }
      if (value == "lutris") {
        return workload_kind_e::lutris;
      }
      return workload_kind_e::unknown;
    }

    runtime_profile_e runtime_profile_from_name(std::string_view value) {
      if (value == "gamescope") {
        return runtime_profile_e::gamescope;
      }
      if (value == "steam") {
        return runtime_profile_e::steam;
      }
      if (value == "heroic") {
        return runtime_profile_e::heroic;
      }
      if (value == "lutris") {
        return runtime_profile_e::lutris;
      }
      return runtime_profile_e::unknown;
    }

    constexpr auto display_topology_name =
      "capture-host-with-nested-compositor"sv;
    constexpr auto media_pipeline_name = "worker-local-capture-encode"sv;

    bool valid_runtime_profile_name(std::string_view value) {
      return value == "gamescope" || value == "steam" ||
             value == "heroic" || value == "lutris";
    }

    bool valid_display_mode(const seat_display_mode_t &mode) {
      return mode.width > 0 && mode.width <= 16384 &&
             mode.height > 0 && mode.height <= 16384 &&
             mode.refresh_millihz >= 1000 && mode.refresh_millihz <= 1000000;
    }

    std::string compositor_name(compositor_e compositor) {
      switch (compositor) {
        case compositor_e::gamescope:
          return "gamescope";
        case compositor_e::sway:
          return "sway";
        case compositor_e::labwc:
          return "labwc";
        case compositor_e::automatic:
          break;
      }
      return {};
    }

    bool valid_compositor_name(std::string_view value) {
      return value == "gamescope" || value == "sway" || value == "labwc";
    }

    bool container_id(std::string_view value) {
      return value.size() == 64 &&
             std::all_of(
               value.begin(),
               value.end(),
               [](char character) {
                 return (character >= '0' && character <= '9') ||
                        (character >= 'a' && character <= 'f');
               }
             );
    }

    std::string trim_ascii(std::string value) {
      const auto whitespace = [](unsigned char character) {
        return character == ' ' || character == '\t' ||
               character == '\n' || character == '\r';
      };
      const auto first = std::find_if_not(value.begin(), value.end(), whitespace);
      const auto last = std::find_if_not(value.rbegin(), value.rend(), whitespace).base();
      if (first >= last) {
        return {};
      }
      return std::string {first, last};
    }

    std::string lowercase_ascii(std::string value) {
      std::transform(
        value.begin(),
        value.end(),
        value.begin(),
        [](unsigned char character) {
          return character >= 'A' && character <= 'Z' ?
                   static_cast<char>(character - 'A' + 'a') :
                   static_cast<char>(character);
        }
      );
      return value;
    }

    template<class Integer>
    std::optional<Integer> parse_decimal(std::string_view value) {
      Integer parsed {};
      const auto result = std::from_chars(value.data(), value.data() + value.size(), parsed);
      if (result.ec != std::errc {} || result.ptr != value.data() + value.size()) {
        return std::nullopt;
      }
      return parsed;
    }

    std::vector<std::string> parse_container_ids(
      const std::string &output,
      std::size_t max_workers
    ) {
      std::vector<std::string> ids;
      std::unordered_set<std::string> unique;
      std::size_t start = 0;
      while (start <= output.size()) {
        const auto end = output.find('\n', start);
        auto line = trim_ascii(output.substr(
          start,
          end == std::string::npos ? std::string::npos : end - start
        ));
        if (!line.empty()) {
          if (!container_id(line) || !unique.emplace(line).second) {
            throw std::runtime_error {"Podman returned an invalid container id inventory"};
          }
          ids.push_back(std::move(line));
          if (ids.size() > max_workers) {
            throw std::runtime_error {"Podman worker inventory exceeds the configured bound"};
          }
        }
        if (end == std::string::npos) {
          break;
        }
        start = end + 1;
      }
      return ids;
    }

    const json *object_member(const json &object, std::string_view name) {
      if (!object.is_object()) {
        return nullptr;
      }
      const auto member = object.find(std::string {name});
      return member == object.end() ? nullptr : &*member;
    }

    std::optional<std::string> string_member(const json &object, std::string_view name) {
      const auto *member = object_member(object, name);
      if (!member || !member->is_string()) {
        return std::nullopt;
      }
      return member->get<std::string>();
    }

    std::optional<std::string> label_value(
      const std::vector<std::pair<std::string, std::string>> &labels,
      std::string_view name
    ) {
      const auto label = std::find_if(
        labels.begin(),
        labels.end(),
        [name](const auto &entry) {
          return entry.first == name;
        }
      );
      return label == labels.end() ? std::nullopt : std::optional {label->second};
    }

    std::vector<std::pair<std::string, std::string>> labels_for(
      const options_t &options,
      const worker_launch_spec_t &spec,
      const profile_t &profile,
      std::string_view input_fingerprint
    ) {
      return {
        {std::string {label_protocol}, "3"},
        {std::string {label_deployment}, options.deployment_id},
        {std::string {label_controller}, spec.identity.seat.controller_epoch},
        {std::string {label_gpu}, spec.identity.seat.logical_gpu_id},
        {std::string {label_slot}, std::to_string(spec.identity.seat.slot)},
        {std::string {label_generation}, std::to_string(spec.identity.seat.generation)},
        {std::string {label_worker}, spec.identity.worker_name},
        {std::string {label_runtime}, spec.resources.runtime_namespace},
        {std::string {label_capture_wayland}, spec.resources.capture_wayland_socket},
        {std::string {label_wayland}, spec.resources.wayland_socket},
        {std::string {label_audio}, spec.resources.audio_sink},
        {std::string {label_input}, spec.resources.input_seat},
        {std::string {label_input_manifest}, std::string {input_fingerprint}},
        {std::string {label_render_node}, spec.render_node},
        {std::string {label_runtime_profile}, std::string {runtime_profile_name(spec.runtime_profile)}},
        {std::string {label_workload_kind}, workload_kind_name(spec.workload.kind)},
        {std::string {label_workload_target}, spec.workload.target_id},
        {std::string {label_display_topology}, std::string {display_topology_name}},
        {std::string {label_media_pipeline}, std::string {media_pipeline_name}},
        {std::string {label_runtime_image}, profile.image_reference},
        {std::string {label_volume}, profile.opaque_volume_name},
        {std::string {label_display_width}, std::to_string(spec.display_mode.width)},
        {std::string {label_display_height}, std::to_string(spec.display_mode.height)},
        {std::string {label_display_refresh}, std::to_string(spec.display_mode.refresh_millihz)},
        {std::string {label_display_hdr}, spec.display_mode.hdr ? "1" : "0"},
        {std::string {label_compositor}, compositor_name(spec.compositor)},
        {std::string {label_encoders}, std::to_string(spec.encoder_sessions)},
      };
    }

    worker_observed_state_e observed_state(
      std::string runtime_state,
      std::string health_state
    ) {
      runtime_state = lowercase_ascii(std::move(runtime_state));
      health_state = lowercase_ascii(std::move(health_state));
      if (runtime_state == "created" ||
          runtime_state == "configured" ||
          runtime_state == "initialized") {
        return worker_observed_state_e::starting;
      }
      if (runtime_state == "stopping" || runtime_state == "removing") {
        return worker_observed_state_e::stopping;
      }
      if (runtime_state == "stopped" || runtime_state == "exited") {
        return worker_observed_state_e::stopped;
      }
      if (runtime_state == "running") {
        if (health_state == "healthy") {
          return worker_observed_state_e::ready;
        }
        if (health_state.empty() || health_state == "starting") {
          return worker_observed_state_e::starting;
        }
        return worker_observed_state_e::failed;
      }
      return worker_observed_state_e::failed;
    }

    void validate_options(const options_t &options) {
      if ((options.engine != engine_e::docker && options.engine != engine_e::podman) ||
          (options.engine == engine_e::docker && !safe_path(options.daemon_socket)) ||
          (!options.selinux_type.empty() && options.selinux_type != "polaris_nvidia_worker_t")) {
        throw std::invalid_argument {"container engine or local Docker socket is invalid"};
      }
      if (!safe_path(options.executable) ||
          !safe_path(options.worker_entrypoint) ||
          !safe_path(options.ipc_root) ||
          !opaque_name_token(options.deployment_id, 64) ||
          options.gpus.empty() ||
          options.profiles.empty() ||
          options.workloads.empty() ||
          options.command_timeout <= std::chrono::milliseconds::zero() ||
          options.max_command_output_bytes == 0 ||
          options.max_inventory_workers == 0 ||
          options.pids_limit == 0 ||
          options.shared_memory_bytes == 0 ||
          options.runtime_tmpfs_bytes == 0 ||
          options.temporary_tmpfs_bytes == 0 ||
          options.log_size_bytes == 0 ||
          options.health_interval <= std::chrono::milliseconds::zero() ||
          options.health_timeout <= std::chrono::milliseconds::zero() ||
          options.health_start_period <= std::chrono::milliseconds::zero() ||
          options.health_retries == 0 ||
          options.health_log_count == 0 ||
          options.health_log_size == 0) {
        throw std::invalid_argument {"container worker options are incomplete"};
      }

      std::unordered_set<std::string> gpu_ids;
      std::unordered_set<std::string> gpu_device_paths;
      std::set<std::pair<std::uint64_t, std::uint64_t>> gpu_inode_identities;
      std::set<std::pair<std::uint32_t, std::uint32_t>> gpu_character_identities;
      for (const auto &gpu : options.gpus) {
        if (!opaque_name_token(gpu.logical_gpu_id) ||
            !device_path(gpu.render_node) ||
            gpu.devices.empty() || gpu.devices.size() > 64 ||
            gpu.max_encoder_sessions == 0 ||
            !gpu_ids.emplace(gpu.logical_gpu_id).second) {
          throw std::invalid_argument {"container GPU options are invalid"};
        }
        bool render_node_present = false;
        for (const auto &device : gpu.devices) {
          if (!device_path(device.path) ||
              !valid_character_device_identity(device.admitted_identity) ||
              !gpu_device_paths.emplace(device.path.native()).second ||
              !gpu_inode_identities.emplace(
                device.admitted_identity.filesystem_device,
                device.admitted_identity.inode
              ).second ||
              !gpu_character_identities.emplace(
                device.admitted_identity.character_major,
                device.admitted_identity.character_minor
              ).second) {
            throw std::invalid_argument {"container GPU devices are invalid"};
          }
          render_node_present = render_node_present ||
                                device.path == gpu.render_node;
        }
        if (!render_node_present) {
          throw std::invalid_argument {"container GPU device set omits its render node"};
        }
      }

      std::unordered_set<std::string> profile_keys;
      std::unordered_set<std::string> profile_volumes;
      for (const auto &profile : options.profiles) {
        if (!opaque_reference(profile.profile_key) ||
            !opaque_name_token(profile.opaque_volume_name) ||
            !concrete_runtime_profile(profile.runtime_profile) ||
            !pinned_image_reference(profile.image_reference, options.engine) ||
            !profile_keys.emplace(profile.profile_key).second ||
            !profile_volumes.emplace(profile.opaque_volume_name).second) {
          throw std::invalid_argument {"container profile options are invalid"};
        }
      }

      std::vector<workload_plan_t> workloads;
      for (const auto &workload : options.workloads) {
        if (!valid_workload_plan(workload) ||
            std::find(workloads.begin(), workloads.end(), workload) != workloads.end()) {
          throw std::invalid_argument {"container workload options are invalid"};
        }
        workloads.push_back(workload);
      }

      std::unordered_set<std::string> mount_names;
      std::unordered_set<std::string> mount_paths;
      for (const auto &mount : options.shared_game_mounts) {
        if (!opaque_name_token(mount.mount_name) ||
            !safe_path(mount.host_path) ||
            !mount_names.emplace(mount.mount_name).second ||
            !mount_paths.emplace(mount.host_path.native()).second) {
          throw std::invalid_argument {"container game mounts are invalid"};
        }
      }

      const auto &driver = options.host_driver;
      if (!driver.mounts.empty()) {
        // Borrowed driver files land in the loader's own directories, and
        // nowhere a Space keeps its home, its sockets or a game share.
        if (options.engine != engine_e::docker || driver.driver_version.empty() || driver.contract != 1 ||
            driver.mounts.size() > maximum_host_driver_mounts) {
          throw std::invalid_argument {"host driver options are invalid"};
        }
        std::unordered_set<std::string> destinations;
        for (const auto &mount : driver.mounts) {
          const std::filesystem::path destination {mount.destination};
          const auto library_of = [&destination](std::string_view prefix) {
            return mount_destination_within(destination, prefix, false);
          };
          if (!safe_path(mount.host_path) || !safe_path(destination) ||
              !(library_of(host_driver_amd64_root) || library_of(host_driver_i386_root) ||
                mount_destination_within(destination, host_driver_share_root, true)) ||
              !destinations.emplace(mount.destination).second ||
              !mount_paths.emplace(mount.host_path.native()).second) {
            throw std::invalid_argument {"host driver mounts are invalid"};
          }
        }
      } else if (std::any_of(options.profiles.begin(), options.profiles.end(),
                   [](const auto &profile) { return profile.host_driver_libraries; })) {
        throw std::invalid_argument {"host driver runtime has no driver files"};
      }
    }
  }  // namespace

  std::optional<std::string> input_manifest_fingerprint(
    const input::allocation_t &allocation
  ) {
    const input::expectation_t expectation {
      .handle = allocation.handle,
      .input_seat = allocation.input_seat,
      .plan = allocation.plan,
    };
    if (!input::valid_allocation(allocation, expectation)) {
      return std::nullopt;
    }

    std::string record;
    record.reserve(512 + allocation.nodes.size() * 256);
    append_fingerprint_field(record, "polaris-input-manifest-v1");
    append_fingerprint_field(record, allocation.handle.controller_epoch);
    append_fingerprint_field(record, allocation.handle.logical_gpu_id);
    append_fingerprint_integer(record, allocation.handle.slot);
    append_fingerprint_integer(record, allocation.handle.generation);
    append_fingerprint_field(record, allocation.input_seat);
    append_fingerprint_integer(record, allocation.plan.touch ? 1 : 0);
    append_fingerprint_integer(record, allocation.plan.pen ? 1 : 0);
    append_fingerprint_integer(record, allocation.plan.gamepad_slots);
    if (allocation.plan.steam_input) {
      append_fingerprint_field(record, "steam-input-output-v1");
    }
    append_fingerprint_integer(record, allocation.nodes.size());
    for (const auto &node : allocation.nodes) {
      append_fingerprint_integer(record, static_cast<unsigned int>(node.kind));
      append_fingerprint_integer(record, node.slot);
      append_fingerprint_field(record, node.host_path.native());
      append_fingerprint_field(record, node.worker_path.native());
      append_fingerprint_integer(record, node.filesystem_device);
      append_fingerprint_integer(record, node.inode);
      append_fingerprint_integer(record, node.character_major);
      append_fingerprint_integer(record, node.character_minor);
      append_fingerprint_field(record, node.kernel_name);
      append_fingerprint_field(record, node.phys);
      append_fingerprint_field(record, node.host_seat);
    }

    std::array<unsigned char, EVP_MAX_MD_SIZE> digest {};
    unsigned int digest_size = 0;
    if (EVP_Digest(
          record.data(),
          record.size(),
          digest.data(),
          &digest_size,
          EVP_sha256(),
          nullptr
        ) != 1 || digest_size != 32) {
      return std::nullopt;
    }
    constexpr std::array<char, 16> hex {
      '0', '1', '2', '3', '4', '5', '6', '7',
      '8', '9', 'a', 'b', 'c', 'd', 'e', 'f'
    };
    std::string fingerprint;
    fingerprint.reserve(64);
    for (std::size_t index = 0; index < digest_size; ++index) {
      fingerprint.push_back(hex[digest[index] >> 4]);
      fingerprint.push_back(hex[digest[index] & 0x0f]);
    }
    return fingerprint;
  }

  authority_input_manifest_source_t::authority_input_manifest_source_t(
    input::authority_t &authority,
    input::kernel_node_probe_t &probe
  ) :
      authority_(authority),
      probe_(probe) {
  }

  std::optional<input::allocation_t>
  authority_input_manifest_source_t::allocation(const seat_handle_t &handle) {
    return authority_.allocation(handle);
  }

  input::node_observation_t authority_input_manifest_source_t::observe(
    const std::filesystem::path &path
  ) {
    return probe_.observe(path);
  }

  std::vector<std::string> command_prefix(const options_t &options) {
    if (options.engine == engine_e::podman) {
      return {options.executable.native(), "--remote=false"};
    }
    // The CLI's inherited context, proxy configuration and credential helpers
    // must never redirect host-device authority or modify the worker request.
    return {
      "/usr/bin/env", "-i", "PATH=/usr/bin:/bin", "HOME=/nonexistent",
      options.executable.native(), "--config=/nonexistent/polaris-docker-cli",
      "--host=unix://" + options.daemon_socket.native(),
    };
  }

  bool backend_t::runtime_ready() const {
    const auto expected = options_.engine == engine_e::docker ? "runc" : "crun";
    return options_.runtime_executable.filename() == expected &&
           host_.trusted_runtime_file(options_.runtime_executable);
  }

  void backend_t::require_docker_engine() {
    if (options_.engine != engine_e::docker) return;
    auto argv = command_prefix(options_);
    argv.insert(argv.end(), {"info", "--format={{json .}}"});
    const auto result = host_.run(argv, options_.command_timeout, options_.max_command_output_bytes);
    if (result.exit_status != 0 || result.timed_out || result.output_truncated) {
      throw std::runtime_error {"the local Docker Engine is unavailable"};
    }
    const auto info = json::parse(result.output);
    const auto *security = object_member(info, "SecurityOptions");
    const auto *runtimes = object_member(info, "Runtimes");
    const auto *runtime = runtimes ? object_member(*runtimes, "runc") : nullptr;
    const auto runtime_path = runtime ? string_member(*runtime, "path") : std::nullopt;
    if (string_member(info, "OSType") != "linux" ||
        !security || !security->is_array() || !runtime_path ||
        (*runtime_path != "runc" && *runtime_path != options_.runtime_executable.native())) {
      throw std::runtime_error {"Docker must provide a local Linux runc worker runtime"};
    }
    for (const auto &entry : *security) {
      if (!entry.is_string() || entry.get<std::string>().starts_with("name=rootless")) {
        throw std::runtime_error {"rootless Docker UID and device mappings are not admitted"};
      }
    }
  }

  namespace {
    std::string health_command(const options_t &options) {
      std::string quoted = "exec '";
      for (const auto ch : options.worker_entrypoint.native()) {
        quoted += ch == '\'' ? "'\\''" : std::string(1, ch);
      }
      return quoted + "' health";
    }

    json docker_tmpfs(const options_t &options, const host_t &host) {
      const auto owner = ",uid=" + std::to_string(host.effective_uid()) +
                         ",gid=" + std::to_string(host.effective_gid());
      const auto bounded = [&owner](std::uint64_t bytes, std::string_view mode) {
        return "rw,nosuid,nodev,size=" + std::to_string(bytes) +
               ",mode=" + std::string(mode) + owner;
      };
      json entries {
        {"/run", bounded(options.runtime_tmpfs_bytes, "0700")},
        {"/run/polaris", bounded(options.runtime_tmpfs_bytes, "0700")},
        {"/tmp", bounded(options.temporary_tmpfs_bytes, "0700")},
        {"/var/tmp", bounded(options.temporary_tmpfs_bytes, "1777")},
      };
      // A borrowed driver needs a writable loader cache: the rootfs is read
      // only, and Steam's pressure-vessel finds the graphics stack by matching
      // sonames against that cache before it copies them into a game's own
      // namespace.
      if (!options.host_driver.mounts.empty())
        entries[std::string {host_driver_cache_directory}] = bounded(8ULL * 1024ULL * 1024ULL, "0700");
      return entries;
    }

    bool empty_array_or_null(const json &object, std::string_view key) {
      const auto *value = object_member(object, key);
      return value && (value->is_null() || (value->is_array() && value->empty()));
    }
  }

  std::vector<std::string> backend_t::docker_launch_arguments(
    const worker_launch_spec_t &spec,
    const profile_t &profile,
    const std::vector<std::uint64_t> &groups
  ) const {
    auto argv = command_prefix(options_);
    std::string network = "none";
    if (options_.media_enabled && needs_profile_network(profile.runtime_profile)) {
      auto id = profile_network_id(host_, profile.profile_key, true);
      // A Space's network is unused whenever the Space is not running, so a
      // plain `docker network prune` takes it, and the Space then refuses to
      // start with nothing to say why. It holds no player data and only
      // Polaris makes one, so an absent network is made again, on this path
      // alone so a healthy launch asks Docker nothing extra. A network that
      // exists under that name with any other identity is never adopted:
      // creation refuses a name already taken, and launch still demands
      // Polaris's exact network again before anything is run.
      if (!id && create_profile_network(host_, profile.profile_key))
        id = profile_network_id(host_, profile.profile_key, true);
      if (!id) throw std::runtime_error {"Space network is unavailable or occupied"};
      network = *id;
    }
    const std::vector<std::string> arguments {
      "run", "--detach", "--rm", "--pull=never", "--restart=no",
      "--runtime=runc", "--name=" + spec.identity.worker_name,
      "--hostname=" + spec.identity.worker_name,
      "--user=" + std::to_string(host_.effective_uid()) + ":" + std::to_string(host_.effective_gid()),
      "--userns=host", "--network=" + network, "--ipc=private", "--cgroupns=private",
      "--cap-drop=all", "--security-opt=no-new-privileges", "--read-only", "--init",
      "--pids-limit=" + std::to_string(options_.pids_limit),
      "--shm-size=" + std::to_string(options_.shared_memory_bytes),
      "--log-driver=json-file", "--log-opt=max-size=" + std::to_string(options_.log_size_bytes),
      "--log-opt=max-file=1",
      "--health-cmd=" + health_command(options_),
      "--health-interval=" + std::to_string(options_.health_interval.count()) + "ms",
      "--health-timeout=" + std::to_string(options_.health_timeout.count()) + "ms",
      "--health-start-period=" + std::to_string(options_.health_start_period.count()) + "ms",
      "--health-retries=" + std::to_string(options_.health_retries),
      "--stop-signal=TERM",
      "--mount=type=volume,src=" + profile.opaque_volume_name +
        ",dst=" + std::string(profile_volume_destination) + ",volume-nocopy",
      "--volume=" + (options_.ipc_root / spec.resources.runtime_namespace / ipc_directory).native() +
        ":" + std::string(container_ipc_directory) + ":rw,Z",
      "--volume=" + (options_.ipc_root / spec.resources.runtime_namespace / auth_directory).native() +
        ":" + std::string(container_auth_directory) + ":ro,Z",
      "--workdir=" + std::string(profile_volume_destination),
    };
    argv.insert(argv.end(), arguments.begin(), arguments.end());
    if (!options_.selinux_type.empty()) {
      argv.push_back("--security-opt=label=type:" + options_.selinux_type);
    }
    if (options_.media_enabled && needs_profile_network(profile.runtime_profile)) {
      argv.push_back("--security-opt=seccomp=" + std::string(steam_seccomp_path));
    }
    for (const auto group : groups) argv.push_back("--group-add=" + std::to_string(group));
    const auto tmpfs = docker_tmpfs(options_, host_);
    for (const auto &[path, options] : tmpfs.items()) {
      argv.push_back("--tmpfs=" + path + ":" + options.get<std::string>());
    }
    return argv;
  }

  void backend_t::validate_docker_record(
    const json &record,
    const runtime_spec_expectations_t &expectations,
    const std::vector<std::pair<std::string, std::string>> &labels
  ) const {
    const auto &config = record.at("Config");
    const auto &host = record.at("HostConfig");
    const auto fail = []() { throw std::runtime_error {"Docker worker isolation or launch configuration changed"}; };
    const auto exact = [&fail](const json &object, std::string_view key, const json &expected) {
      const auto *value = object_member(object, key);
      if (!value || *value != expected) fail();
    };
    exact(config, "User", std::to_string(host_.effective_uid()) + ":" + std::to_string(host_.effective_gid()));
    exact(config, "Image", label_value(labels, label_runtime_image).value());
    const auto image_reference = label_value(labels, label_runtime_image).value();
    if (image_reference.starts_with("sha256:")) exact(record, "Image", image_reference);
    exact(config, "Entrypoint", json::array({options_.worker_entrypoint.native()}));
    auto expected_command = json::array({"run",
      "--workload-kind=" + label_value(labels, label_workload_kind).value(),
      "--workload-id=" + label_value(labels, label_workload_target).value()});
    if (options_.media_enabled) expected_command.push_back("--media=enabled");
    exact(config, "Cmd", expected_command);
    exact(config, "WorkingDir", std::string(profile_volume_destination));
    exact(host, "Privileged", false);
    exact(host, "ReadonlyRootfs", true);
    exact(host, "AutoRemove", true);
    exact(host, "Init", true);
    exact(host, "Runtime", "runc");
    exact(host, "UsernsMode", "host");
    exact(host, "IpcMode", "private");
    exact(host, "PidMode", "");
    exact(host, "UTSMode", "");
    exact(host, "CgroupnsMode", "private");
    exact(host, "PidsLimit", options_.pids_limit);
    exact(host, "ShmSize", options_.shared_memory_bytes);
    exact(host, "CapDrop", json::array({"ALL"}));
    auto security = json::array({"no-new-privileges"});
    if (!options_.selinux_type.empty()) security.push_back("label=type:" + options_.selinux_type);
    // The same rule the launch applies, asked the same way. Expecting the
    // policy for Steam alone while launching it for every launcher made
    // Polaris reject its own healthy worker: the record carried an option the
    // validator said could not be there, the inventory stopped being
    // authoritative, and the Space timed out with a container that was fine.
    if (options_.media_enabled &&
        needs_profile_network(runtime_profile_from_name(label_value(labels, label_runtime_profile).value_or("")))) {
      security.push_back("seccomp=" + json::parse(steam_seccomp_data).dump());
    }
    exact(host, "SecurityOpt", security);
    exact(host, "Tmpfs", docker_tmpfs(options_, host_));
    for (const auto key : {"CapAdd", "DeviceRequests", "DeviceCgroupRules", "VolumesFrom", "Links", "ExtraHosts"}) {
      if (!empty_array_or_null(host, key)) fail();
    }
    exact(host.at("RestartPolicy"), "Name", "no");
    exact(host.at("LogConfig"), "Type", "json-file");
    exact(host.at("LogConfig"), "Config", json({
      {"max-size", std::to_string(options_.log_size_bytes)}, {"max-file", "1"},
    }));
    exact(config.at("Healthcheck"), "Test", json::array({"CMD-SHELL", health_command(options_)}));
    exact(config.at("Healthcheck"), "Interval", std::chrono::duration_cast<std::chrono::nanoseconds>(options_.health_interval).count());
    exact(config.at("Healthcheck"), "Timeout", std::chrono::duration_cast<std::chrono::nanoseconds>(options_.health_timeout).count());
    exact(config.at("Healthcheck"), "StartPeriod", std::chrono::duration_cast<std::chrono::nanoseconds>(options_.health_start_period).count());
    exact(config.at("Healthcheck"), "Retries", options_.health_retries);

    const auto profile = std::find_if(options_.profiles.begin(), options_.profiles.end(), [&](const auto &candidate) {
      return candidate.opaque_volume_name == expectations.volume_name;
    });
    if (profile == options_.profiles.end() ||
        profile->image_reference != label_value(labels, label_runtime_image) ||
        runtime_profile_name(profile->runtime_profile) != label_value(labels, label_runtime_profile)) fail();

    if (options_.media_enabled && needs_profile_network(profile->runtime_profile)) {
      const auto id = profile_network_id(host_, profile->profile_key, false, record.at("Id").get<std::string>());
      if (!id) fail();
      exact(host, "NetworkMode", *id);
      exact(host, "PublishAllPorts", false);
      const auto &networks = record.at("NetworkSettings").at("Networks");
      if (!networks.is_object() || networks.size() != 1) fail();
      exact(networks.at(profile_network_name(profile->profile_key)), "NetworkID", *id);
      for (const auto key : {"Dns", "DnsOptions", "DnsSearch"}) if (!empty_array_or_null(host, key)) fail();
      for (const auto *object : {object_member(host, "PortBindings"), object_member(config, "ExposedPorts"),
                                object_member(record.at("NetworkSettings"), "Ports")}) {
        if (object && !object->is_null() && (!object->is_object() || !object->empty())) fail();
      }
    } else {
      exact(host, "NetworkMode", "none");
    }

    const auto groups = host_.supplementary_groups();
    if (!groups) fail();
    std::set<std::uint64_t> expected_groups(groups->begin(), groups->end());
    std::set<std::uint64_t> observed_groups;
    const auto &group_array = host.at("GroupAdd");
    if (!group_array.is_array() && !(group_array.is_null() && expected_groups.empty())) fail();
    if (group_array.is_array()) for (const auto &value : group_array) {
      if (!value.is_string()) fail();
      const auto group = parse_decimal<std::uint64_t>(value.get<std::string>());
      if (!group || !observed_groups.emplace(*group).second) fail();
    }
    if (expected_groups != observed_groups) fail();

    const auto &mounts = record.at("Mounts");
    if (!mounts.is_array() || mounts.size() != expectations.controller_binds.size() + 1) fail();
    std::set<std::string> destinations;
    bool profile_present = false;
    for (const auto &mount : mounts) {
      const auto destination = string_member(mount, "Destination");
      const auto source = string_member(mount, "Source");
      const auto type = string_member(mount, "Type");
      if (!destination || !source || !type || !destinations.emplace(*destination).second) fail();
      if (*type == "volume") {
        if (profile_present || *destination != profile_volume_destination) fail();
        exact(mount, "Name", expectations.volume_name);
        exact(mount, "Driver", "local");
        exact(mount, "RW", true);
        profile_present = true;
      } else if (*type == "bind") {
        const auto expected = expectations.controller_binds.find(*source);
        if (expected == expectations.controller_binds.end() || expected->second.destination != *destination) fail();
        exact(mount, "RW", expected->second.permissions == "rw");
        exact(mount, "Propagation", "rprivate");
      } else fail();
    }
    if (!profile_present) fail();
    const auto &requested_mounts = host.at("Mounts");
    if (!requested_mounts.is_array()) fail();
    bool no_copy = false;
    for (const auto &mount : requested_mounts) {
      if (string_member(mount, "Type") != "volume") continue;
      if (no_copy) fail();
      exact(mount, "Source", expectations.volume_name);
      exact(mount, "Target", std::string(profile_volume_destination));
      exact(mount.at("VolumeOptions"), "NoCopy", true);
      no_copy = true;
    }
    if (!no_copy) fail();

    // Authenticate routing metadata from the executed environment as well as
    // labels. A correct-looking label must never excuse a differently routed worker.
    const std::map<std::string, std::string> required {
      {"HOME", std::string(profile_volume_destination)},
      {"XDG_CONFIG_HOME", "/var/lib/polaris-seat/.config"},
      {"XDG_CACHE_HOME", "/var/lib/polaris-seat/.cache"},
      {"XDG_DATA_HOME", "/var/lib/polaris-seat/.local/share"},
      {"XDG_RUNTIME_DIR", "/run/polaris"},
      {"DBUS_SESSION_BUS_ADDRESS", "unix:path=/run/polaris/bus"},
      {"PIPEWIRE_RUNTIME_DIR", "/run/polaris"},
      {"PULSE_SERVER", "unix:/run/polaris/pulse/native"},
      {"POLARIS_WORKER_NAME", label_value(labels, label_worker).value()},
      {"POLARIS_RUNTIME_NAMESPACE", label_value(labels, label_runtime).value()},
      {"POLARIS_CONTROLLER_EPOCH", label_value(labels, label_controller).value()},
      {"POLARIS_LOGICAL_GPU_ID", label_value(labels, label_gpu).value()},
      {"POLARIS_SEAT_SLOT", label_value(labels, label_slot).value()},
      {"POLARIS_SEAT_GENERATION", label_value(labels, label_generation).value()},
      {"POLARIS_RENDER_NODE", label_value(labels, label_render_node).value()},
      {"POLARIS_INPUT_SEAT", label_value(labels, label_input).value()},
      {"POLARIS_RUNTIME_PROFILE", label_value(labels, label_runtime_profile).value()},
      {"POLARIS_DISPLAY_TOPOLOGY", label_value(labels, label_display_topology).value()},
      {"POLARIS_MEDIA_PIPELINE", label_value(labels, label_media_pipeline).value()},
      {"POLARIS_DISPLAY_WIDTH", label_value(labels, label_display_width).value()},
      {"POLARIS_DISPLAY_HEIGHT", label_value(labels, label_display_height).value()},
      {"POLARIS_DISPLAY_REFRESH_MILLIHZ", label_value(labels, label_display_refresh).value()},
      {"POLARIS_DISPLAY_HDR", label_value(labels, label_display_hdr).value()},
      {"POLARIS_COMPOSITOR", label_value(labels, label_compositor).value()},
      {"POLARIS_ENCODER_SESSIONS", label_value(labels, label_encoders).value()},
      {"WAYLAND_DISPLAY", label_value(labels, label_wayland).value()},
      {"POLARIS_CAPTURE_WAYLAND_DISPLAY", label_value(labels, label_capture_wayland).value()},
      {"PULSE_SINK", label_value(labels, label_audio).value()},
    };
    std::map<std::string, std::string> environment;
    if (!config.at("Env").is_array()) fail();
    for (const auto &value : config.at("Env")) {
      if (!value.is_string()) fail();
      const auto entry = value.get<std::string>();
      const auto separator = entry.find('=');
      if (separator == std::string::npos ||
          !environment.emplace(entry.substr(0, separator), entry.substr(separator + 1)).second) fail();
    }
    for (const auto &[key, value] : required) {
      const auto found = environment.find(key);
      if (found == environment.end() || found->second != value) fail();
    }
  }

  backend_t::backend_t(
    host_t &host,
    input_manifest_source_t &input_manifests,
    options_t options
  ) :
      host_(host),
      input_manifests_(input_manifests),
      options_(std::move(options)) {
    validate_options(options_);
  }

  const gpu_t *backend_t::gpu_for(const worker_launch_spec_t &spec) const {
    const auto gpu = std::find_if(
      options_.gpus.begin(),
      options_.gpus.end(),
      [&spec](const auto &candidate) {
        return candidate.logical_gpu_id == spec.identity.seat.logical_gpu_id &&
               candidate.render_node == spec.render_node;
      }
    );
    return gpu == options_.gpus.end() ? nullptr : &*gpu;
  }

  const profile_t *backend_t::profile_for(const std::string &profile_key) const {
    const auto profile = std::find_if(
      options_.profiles.begin(),
      options_.profiles.end(),
      [&profile_key](const auto &candidate) {
        return candidate.profile_key == profile_key;
      }
    );
    return profile == options_.profiles.end() ? nullptr : &*profile;
  }

  bool backend_t::workload_allowed(const workload_plan_t &workload) const {
    return std::find(
             options_.workloads.begin(),
             options_.workloads.end(),
             workload
           ) != options_.workloads.end();
  }

  std::optional<input::allocation_t> backend_t::input_allocation_for(
    const seat_handle_t &handle,
    std::string_view input_seat
  ) const {
    auto allocation = input_manifests_.allocation(handle);
    if (!allocation) {
      return std::nullopt;
    }
    const input::expectation_t expectation {
      .handle = handle,
      .input_seat = std::string {input_seat},
      .plan = allocation->plan,
    };
    if (!input::valid_allocation(*allocation, expectation) ||
        !input_manifest_fingerprint(*allocation)) {
      return std::nullopt;
    }
    return allocation;
  }

  bool backend_t::input_allocation_current(
    const input::allocation_t &allocation
  ) const {
    const auto current = input_manifests_.allocation(allocation.handle);
    if (!current || *current != allocation) {
      return false;
    }
    for (const auto &node : allocation.nodes) {
      const auto accessible = host_.read_write_character_device(node.host_path);
      if (!accessible || !exact_input_identity(*accessible, node)) {
        return false;
      }
      const auto observation = input_manifests_.observe(node.host_path);
      if (observation.status != input::node_observation_status_e::observed ||
          !observation.snapshot ||
          !exact_input_snapshot(*observation.snapshot, node)) {
        return false;
      }
    }
    return true;
  }

  bool backend_t::inspected_bindings_match(
    const std::vector<runtime_device_binding_t> &bindings,
    const gpu_t &gpu,
    const input::allocation_t &input_allocation
  ) const {
    struct expected_binding_t {
      std::filesystem::path host_path;
      std::filesystem::path worker_path;
      character_device_identity_t identity;
      bool exact_host_identity = false;
    };
    std::vector<expected_binding_t> expected;
    expected.reserve(gpu.devices.size() + input_allocation.nodes.size());
    for (const auto &device : gpu.devices) {
      expected.push_back({
        .host_path = device.path,
        .worker_path = device.path,
        .identity = device.admitted_identity,
        .exact_host_identity = true,
      });
    }
    for (const auto &node : input_allocation.nodes) {
      expected.push_back({
        .host_path = node.host_path,
        .worker_path = node.worker_path,
        .identity = {
          .filesystem_device = node.filesystem_device,
          .inode = node.inode,
          .character_major = node.character_major,
          .character_minor = node.character_minor,
        },
      });
    }
    if (bindings.size() != expected.size()) {
      return false;
    }
    std::vector<bool> consumed(expected.size(), false);
    for (const auto &binding : bindings) {
      std::optional<std::size_t> match;
      for (std::size_t index = 0; index < expected.size(); ++index) {
        if (!consumed[index] &&
            binding.worker_path == expected[index].worker_path &&
            same_character_device(binding.identity, expected[index].identity) &&
            (!expected[index].exact_host_identity ||
             (binding.host_path == expected[index].host_path &&
              binding.identity == expected[index].identity))) {
          match = index;
          break;
        }
      }
      if (!match) {
        return false;
      }
      consumed[*match] = true;
    }
    return std::all_of(consumed.begin(), consumed.end(), [](bool value) {
      return value;
    });
  }

  std::vector<backend_t::declared_device_binding_t>
  backend_t::runtime_spec_device_bindings(
    const std::filesystem::path &spec_path,
    const runtime_spec_expectations_t &expectations
  ) const {
    if (!runtime_spec_path(spec_path, expectations.container_id)) {
      throw std::runtime_error {"invalid Podman runtime spec path"};
    }
    const auto text = host_.read_owned_regular_file(
      spec_path,
      options_.max_command_output_bytes
    );
    if (!text) {
      throw std::runtime_error {"Podman runtime spec is unreadable"};
    }
    json spec;
    try {
      spec = json::parse(*text);
    } catch (const json::exception &) {
      throw std::runtime_error {"invalid Podman runtime spec"};
    }
    const auto *process = object_member(spec, "process");
    const auto *user = process ? object_member(*process, "user") : nullptr;
    if (!user || !user->contains("uid") || !(*user)["uid"].is_number_unsigned() ||
        (*user)["uid"].get<std::uint64_t>() != host_.effective_uid()) {
      throw std::runtime_error {"Podman runtime spec does not preserve the launching uid"};
    }
    const auto *annotations = object_member(spec, "annotations");
    if (!annotations || string_member(*annotations, "run.oci.keep_original_groups") != "1") {
      throw std::runtime_error {"Podman runtime spec does not preserve supplementary groups"};
    }
    const auto *mounts = object_member(spec, "mounts");
    if (!mounts || !mounts->is_array() ||
        mounts->size() > maximum_runtime_spec_mounts) {
      throw std::runtime_error {"invalid Podman runtime spec"};
    }
    // The storage root is the spec path without its last four components
    // (`<driver>-containers/<id>/userdata/config.json`), which anchors the
    // profile volume to the same storage the record lives in.
    const auto storage_root =
      spec_path.parent_path().parent_path().parent_path().parent_path();
    const auto volume_source =
      (storage_root / "volumes" / expectations.volume_name / "_data").native();
    std::vector<declared_device_binding_t> bindings;
    auto device_seen = false;
    for (const auto &mount : *mounts) {
      const auto destination = string_member(mount, "destination");
      const auto type = string_member(mount, "type");
      const auto source = string_member(mount, "source");
      if (!destination || !type || !normalized_absolute_path(*destination)) {
        throw std::runtime_error {"invalid Podman runtime spec mount"};
      }
      const auto device_destination = device_directory_destination(*destination);
      if (*type != "bind") {
        // Podman's /proc, /dev tmpfs, sysfs, devpts, mqueue, and cgroup, and
        // the controller's tmpfs mounts. None of them may sit on a device
        // directory, and the runtime applies mounts in order, so none may
        // cover /dev or the root after a device binding either.
        if (device_destination ||
            (device_seen && (*destination == "/dev" || *destination == "/"))) {
          throw std::runtime_error {"Podman runtime spec shadows device bindings"};
        }
        continue;
      }
      if (!source || !normalized_absolute_path(*source)) {
        throw std::runtime_error {"invalid Podman runtime spec mount"};
      }
      std::string permissions {"rw"};
      if (const auto *options = object_member(mount, "options")) {
        if (!options->is_array()) {
          throw std::runtime_error {"invalid Podman runtime spec mount"};
        }
        for (const auto &option : *options) {
          if (!option.is_string()) {
            throw std::runtime_error {"invalid Podman runtime spec mount"};
          }
          const auto value = option.get<std::string>();
          if (value == "ro" || value == "rw") {
            permissions = value;
          }
        }
      }
      if (!device_destination) {
        // Podman's own per-container files, the worker's profile volume, the
        // authority directories, and the shared game mounts are the only host
        // content a worker may see besides its devices and Podman's init
        // binary, and each must land where the controller asked with the
        // permission it asked for. Checked before the device rule so a
        // storage root that itself lives under /dev/shm still passes.
        if (own_container_userdata_path(*source, expectations.container_id)) {
          if (!podman_own_destination(*destination)) {
            throw std::runtime_error {"unexpected Podman runtime spec mount"};
          }
          continue;
        }
        if (*source == volume_source) {
          if (*destination != profile_volume_destination || permissions != "rw") {
            throw std::runtime_error {"unexpected Podman runtime spec mount"};
          }
          continue;
        }
        if (const auto bind = expectations.controller_binds.find(*source);
            bind != expectations.controller_binds.end()) {
          if (*destination != bind->second.destination ||
              permissions != bind->second.permissions) {
            throw std::runtime_error {"unexpected Podman runtime spec mount"};
          }
          continue;
        }
        if (*destination == podman_init_destination) {
          // `--init` binds the configured init binary read-only; it must be
          // an executable regular file on the host, never a device.
          if (source->starts_with("/dev/") || permissions != "ro" ||
              !host_.executable_file(*source)) {
            throw std::runtime_error {"invalid Podman init binding"};
          }
          continue;
        }
        if (!source->starts_with("/dev/")) {
          throw std::runtime_error {"unexpected Podman runtime spec mount"};
        }
      }
      if (!device_path(*source) || !device_path(*destination)) {
        throw std::runtime_error {"invalid Podman device binding"};
      }
      bindings.push_back({
        .host_path = *source,
        .worker_path = *destination,
        .permissions = std::move(permissions),
      });
      device_seen = true;
    }
    // A rootless runtime creates no device nodes through the spec's device
    // node list; a spec that does describes a mode this backend never runs
    // in, so it is refused rather than reasoned about.
    const auto *linux_object = object_member(spec, "linux");
    const auto *devices = linux_object ? object_member(*linux_object, "devices") : nullptr;
    if (devices && !devices->is_null() && !(devices->is_array() && devices->empty())) {
      throw std::runtime_error {"Podman runtime spec grants device nodes"};
    }
    return bindings;
  }

  bool backend_t::base_host_ready() const {
    return host_.effective_uid() != 0 && host_.executable_file(options_.executable);
  }

  bool backend_t::gpu_catalog_current() const {
    std::set<std::pair<std::uint64_t, std::uint64_t>> inode_identities;
    std::set<std::pair<std::uint32_t, std::uint32_t>> character_identities;
    for (const auto &gpu : options_.gpus) {
      for (const auto &device : gpu.devices) {
        const auto current = host_.read_write_character_device(device.path);
        if (!current || *current != device.admitted_identity ||
            !inode_identities.emplace(
              current->filesystem_device,
              current->inode
            ).second ||
            !character_identities.emplace(
              current->character_major,
              current->character_minor
            ).second) {
          return false;
        }
      }
    }
    return true;
  }

  std::optional<bool> backend_t::profile_volume_exists(const profile_t &profile) {
    auto argv = command_prefix(options_);
    if (options_.engine == engine_e::docker) {
      argv.insert(argv.end(), {"volume", "inspect", "--format={{json .}}", profile.opaque_volume_name});
      const auto result = host_.run(argv, options_.command_timeout, options_.max_command_output_bytes);
      if (result.exit_status != 0 || result.timed_out || result.output_truncated) return std::nullopt;
      const auto volume = json::parse(result.output);
      const auto *driver_options = object_member(volume, "Options");
      const auto mountpoint = string_member(volume, "Mountpoint");
      if (string_member(volume, "Name") != profile.opaque_volume_name ||
          string_member(volume, "Driver") != "local" || string_member(volume, "Scope") != "local" ||
          !driver_options || !(driver_options->is_null() || (driver_options->is_object() && driver_options->empty())) ||
          !mountpoint || !safe_path(*mountpoint) ||
          !mountpoint->ends_with("/volumes/" + profile.opaque_volume_name + "/_data")) {
        return false;
      }
      return true;
    }
    argv.insert(argv.end(), {"volume", "exists", profile.opaque_volume_name});
    const auto result = host_.run(
      argv,
      options_.command_timeout,
      options_.max_command_output_bytes
    );
    if (result.timed_out || result.output_truncated) {
      return std::nullopt;
    }
    if (result.exit_status == 0) {
      return true;
    }
    if (result.exit_status == 1) {
      return false;
    }
    return std::nullopt;
  }

  bool backend_t::launch_host_ready(
    const worker_launch_spec_t &spec,
    const input::allocation_t &input_allocation
  ) const {
    if (!base_host_ready() || !runtime_ready()) {
      return false;
    }
    const auto worker_authority_directory =
      options_.ipc_root / spec.resources.runtime_namespace;
    const auto worker_ipc_directory = worker_authority_directory / ipc_directory;
    const auto worker_auth_directory = worker_authority_directory / auth_directory;
    if (!safe_path(worker_authority_directory) ||
        !host_.private_read_write_directory(options_.ipc_root) ||
        !host_.private_read_write_directory(worker_authority_directory) ||
        !host_.private_read_write_directory(worker_ipc_directory) ||
        !host_.private_read_write_directory(worker_auth_directory) ||
        !host_.private_readable_file(worker_auth_directory / capability_file)) {
      return false;
    }
    const auto devices_ready = gpu_catalog_current() &&
                               input_allocation_current(input_allocation);
    if (!devices_ready) {
      return false;
    }
    return std::all_of(
      options_.shared_game_mounts.begin(),
      options_.shared_game_mounts.end(),
      [this](const auto &mount) {
        return host_.readable_directory(mount.host_path);
      }
    );
  }

  bool backend_t::valid_spec(const worker_launch_spec_t &spec) const {
    if (options_.media_enabled &&
        (options_.engine != engine_e::docker ||
         !supported_streaming_workload(spec.runtime_profile, spec.workload) || spec.display_mode.hdr)) {
      return false;
    }
    if (!spec.identity.seat.valid() ||
        spec.identity.seat.logical_gpu_id.empty() ||
        spec.identity.worker_name != spec.resources.worker_name ||
        !opaque_name_token(spec.identity.seat.controller_epoch, 64) ||
        !opaque_name_token(spec.identity.seat.logical_gpu_id) ||
        !opaque_name_token(spec.identity.worker_name) ||
        !opaque_name_token(spec.resources.runtime_namespace) ||
        !opaque_name_token(spec.resources.capture_wayland_socket) ||
        !opaque_name_token(spec.resources.wayland_socket) ||
        !opaque_name_token(spec.resources.audio_sink) ||
        !opaque_name_token(spec.resources.input_seat) ||
        !opaque_reference(spec.profile_key) ||
        !valid_workload_plan(spec.workload) ||
        !device_path(spec.render_node) ||
        !concrete_runtime_profile(spec.runtime_profile) ||
        !workload_matches_runtime_profile(spec.workload, spec.runtime_profile) ||
        !valid_data_plane(spec.data_plane) ||
        !valid_display_mode(spec.display_mode) ||
        !concrete_compositor(spec.compositor) ||
        spec.encoder_sessions == 0) {
      return false;
    }
    const auto *gpu = gpu_for(spec);
    const auto *profile = profile_for(spec.profile_key);
    return gpu && profile &&
           spec.encoder_sessions <= gpu->max_encoder_sessions &&
           profile->runtime_profile == spec.runtime_profile &&
           // A title from the Space's own library is admitted without being
           // listed as a configured workload, but only through its family's
           // grammar.
           (workload_allowed(spec.workload) ||
            (profile->library_enabled &&
             supported_streaming_workload(profile->runtime_profile, spec.workload)));
  }

  std::vector<std::string> backend_t::launch_argv(
    const worker_launch_spec_t &spec,
    const gpu_t &gpu,
    const profile_t &profile,
    const input::allocation_t &input_allocation,
    std::string_view input_fingerprint,
    const std::vector<std::uint64_t> &groups
  ) const {
    std::vector<std::string> argv;
    if (options_.engine == engine_e::docker) {
      argv = docker_launch_arguments(spec, profile, groups);
    } else {
      argv = {
        options_.executable.native(),
        "--remote=false",
        "--runtime=" + options_.runtime_executable.native(),
        "run",
        "--detach",
        "--rm",
        "--pull=never",
        "--restart=no",
        "--name=" + spec.identity.worker_name,
        "--hostname=" + spec.identity.worker_name,
        "--userns=keep-id",
        "--user=" + std::to_string(host_.effective_uid()),
        "--group-add=keep-groups",
        "--network=none",
        "--no-hosts",
        "--http-proxy=false",
        "--ipc=private",
        "--pid=private",
        "--uts=private",
        "--cgroupns=private",
        "--cap-drop=all",
        "--security-opt=no-new-privileges",
        "--read-only",
        "--read-only-tmpfs=true",
        "--image-volume=tmpfs",
        "--init",
        "--pids-limit=" + std::to_string(options_.pids_limit),
        "--shm-size=" + std::to_string(options_.shared_memory_bytes) + "b",
        "--mount=type=tmpfs,dst=/run/polaris,rw=true,tmpfs-size=" +
          std::to_string(options_.runtime_tmpfs_bytes) +
          ",tmpfs-mode=0700,U=true,notmpcopyup",
        "--mount=type=tmpfs,dst=/tmp,rw=true,tmpfs-size=" +
          std::to_string(options_.temporary_tmpfs_bytes) +
          ",tmpfs-mode=0700,U=true,notmpcopyup",
        "--log-driver=k8s-file",
        "--log-opt=max-size=" + std::to_string(options_.log_size_bytes) + "b",
        "--health-cmd=" + json::array({options_.worker_entrypoint.native(), "health"}).dump(),
        "--health-interval=" + std::to_string(options_.health_interval.count()) + "ms",
        "--health-timeout=" + std::to_string(options_.health_timeout.count()) + "ms",
        "--health-start-period=" + std::to_string(options_.health_start_period.count()) + "ms",
        "--health-retries=" + std::to_string(options_.health_retries),
        "--health-on-failure=none",
        "--health-max-log-count=" + std::to_string(options_.health_log_count),
        "--health-max-log-size=" + std::to_string(options_.health_log_size),
        "--stop-signal=TERM",
        "--volume=" + profile.opaque_volume_name + ":" +
          std::string {profile_volume_destination} + ":rw,nosuid,nodev",
        "--mount=type=bind,src=" +
          (options_.ipc_root / spec.resources.runtime_namespace / ipc_directory).native() +
          ",dst=" + std::string {container_ipc_directory} +
          ",rw=true,relabel=private,bind-nonrecursive",
        "--mount=type=bind,src=" +
          (options_.ipc_root / spec.resources.runtime_namespace / auth_directory).native() +
          ",dst=" + std::string {container_auth_directory} +
          ",ro=true,relabel=private,bind-nonrecursive",
        "--workdir=/var/lib/polaris-seat",
      };
    }

    for (const auto &[name, value] : labels_for(
           options_,
           spec,
           profile,
           input_fingerprint
         )) {
      argv.push_back("--label=" + name + "=" + value);
    }

    const auto add_environment = [&argv](std::string name, std::string value) {
      argv.push_back("--env=" + std::move(name) + "=" + std::move(value));
    };
    add_environment("HOME", "/var/lib/polaris-seat");
    add_environment("XDG_CONFIG_HOME", "/var/lib/polaris-seat/.config");
    add_environment("XDG_CACHE_HOME", "/var/lib/polaris-seat/.cache");
    add_environment("XDG_DATA_HOME", "/var/lib/polaris-seat/.local/share");
    add_environment("XDG_RUNTIME_DIR", "/run/polaris");
    add_environment("DBUS_SESSION_BUS_ADDRESS", "unix:path=/run/polaris/bus");
    add_environment("PIPEWIRE_RUNTIME_DIR", "/run/polaris");
    add_environment("PULSE_SERVER", "unix:/run/polaris/pulse/native");
    add_environment("PULSE_SINK", spec.resources.audio_sink);
    add_environment("WAYLAND_DISPLAY", spec.resources.wayland_socket);
    add_environment("POLARIS_CAPTURE_WAYLAND_DISPLAY", spec.resources.capture_wayland_socket);
    add_environment("POLARIS_RUNTIME_NAMESPACE", spec.resources.runtime_namespace);
    add_environment("POLARIS_WORKER_NAME", spec.identity.worker_name);
    add_environment("POLARIS_INPUT_SEAT", spec.resources.input_seat);
    add_environment("POLARIS_CONTROLLER_EPOCH", spec.identity.seat.controller_epoch);
    add_environment("POLARIS_LOGICAL_GPU_ID", spec.identity.seat.logical_gpu_id);
    add_environment("POLARIS_SEAT_SLOT", std::to_string(spec.identity.seat.slot));
    add_environment("POLARIS_SEAT_GENERATION", std::to_string(spec.identity.seat.generation));
    add_environment("POLARIS_RENDER_NODE", spec.render_node);
    add_environment("POLARIS_RUNTIME_PROFILE", std::string {runtime_profile_name(spec.runtime_profile)});
    add_environment("POLARIS_DISPLAY_TOPOLOGY", std::string {display_topology_name});
    add_environment("POLARIS_MEDIA_PIPELINE", std::string {media_pipeline_name});
    add_environment("POLARIS_DISPLAY_WIDTH", std::to_string(spec.display_mode.width));
    add_environment("POLARIS_DISPLAY_HEIGHT", std::to_string(spec.display_mode.height));
    add_environment(
      "POLARIS_DISPLAY_REFRESH_MILLIHZ",
      std::to_string(spec.display_mode.refresh_millihz)
    );
    add_environment("POLARIS_DISPLAY_HDR", spec.display_mode.hdr ? "1" : "0");
    add_environment("POLARIS_COMPOSITOR", compositor_name(spec.compositor));
    add_environment("POLARIS_ENCODER_SESSIONS", std::to_string(spec.encoder_sessions));

    for (const auto &device : gpu.devices) {
      argv.push_back(
        "--device=" + device.path.native() + ":" +
        device.path.native() + ":rw"
      );
    }
    for (const auto &node : input_allocation.nodes) {
      argv.push_back(
        "--device=" + node.host_path.native() + ":" +
        node.worker_path.native() + ":rw"
      );
    }
    for (const auto &mount : options_.shared_game_mounts) {
      argv.push_back(
        "--mount=type=bind,src=" + mount.host_path.native() +
        ",dst=" + std::string {shared_game_mount_root} + mount.mount_name + ",ro=true"
      );
    }
    if (profile.host_driver_libraries) {
      // A runtime built without driver libraries of its own borrows this
      // machine's, read only and at the paths the loader resolves them by.
      // Deliberately unlabelled: relabelling the host's own /usr would damage
      // the host, so SELinux policy grants the read instead.
      for (const auto &mount : options_.host_driver.mounts) {
        argv.push_back(
          "--mount=type=bind,src=" + mount.host_path.native() +
          ",dst=" + mount.destination + ",ro=true"
        );
      }
    }

    argv.push_back("--entrypoint=" + options_.worker_entrypoint.native());
    argv.push_back(profile.image_reference);
    argv.push_back("run");
    argv.push_back("--workload-kind=" + workload_kind_name(spec.workload.kind));
    argv.push_back("--workload-id=" + spec.workload.target_id);
    if (options_.media_enabled) argv.push_back("--media=enabled");
    return argv;
  }

  worker_command_result_e backend_t::launch(const worker_launch_spec_t &spec) {
    if (!valid_spec(spec)) {
      return worker_command_result_e::rejected;
    }
    const auto *gpu = gpu_for(spec);
    const auto *profile = profile_for(spec.profile_key);
    if (!gpu || !profile) {
      return worker_command_result_e::rejected;
    }
    std::optional<input::allocation_t> input_allocation;
    try {
      input_allocation = input_allocation_for(
        spec.identity.seat,
        spec.resources.input_seat
      );
    } catch (...) {
      return worker_command_result_e::indeterminate;
    }
    if (!input_allocation) {
      return worker_command_result_e::rejected;
    }
    const auto input_fingerprint = input_manifest_fingerprint(*input_allocation);
    if (!input_fingerprint) {
      return worker_command_result_e::rejected;
    }
    std::optional<std::vector<std::uint64_t>> launching_groups;
    try {
      launching_groups = host_.supplementary_groups();
      if (!launching_groups || !launch_host_ready(spec, *input_allocation)) {
        return worker_command_result_e::rejected;
      }
      require_docker_engine();
    } catch (...) {
      return worker_command_result_e::indeterminate;
    }
    // podman run silently creates a missing named volume and offers no option
    // to refuse, so the pre-created-volume contract is checked explicitly here,
    // before the final identity recheck that stays adjacent to the run.
    std::optional<bool> volume_present;
    try {
      volume_present = profile_volume_exists(*profile);
    } catch (...) {
      return worker_command_result_e::indeterminate;
    }
    if (!volume_present) {
      return worker_command_result_e::indeterminate;
    }
    if (!*volume_present) {
      return worker_command_result_e::rejected;
    }

    command_result_t result;
    try {
      // A missing or changed policy is a definite refusal before Docker run.
      // Do not quarantine this as an uncertain container creation outcome.
      if (options_.media_enabled && needs_profile_network(profile->runtime_profile) &&
          !host_.trusted_data_file(std::filesystem::path(steam_seccomp_path), steam_seccomp_data)) {
        return worker_command_result_e::rejected;
      }
      const auto argv = launch_argv(
        spec,
        *gpu,
        *profile,
        *input_allocation,
        *input_fingerprint,
        *launching_groups
      );
      if (!runtime_ready() ||
          host_.supplementary_groups() != launching_groups ||
          !gpu_catalog_current() || !input_allocation_current(*input_allocation)) {
        return worker_command_result_e::rejected;
      }
      if (options_.media_enabled && needs_profile_network(profile->runtime_profile)) {
        const auto network = profile_network_id(host_, profile->profile_key, true);
        if (!network || std::find(argv.begin(), argv.end(), "--network=" + *network) == argv.end())
          return worker_command_result_e::rejected;
      }
      result = host_.run(
        argv,
        options_.command_timeout,
        options_.max_command_output_bytes
      );
    } catch (...) {
      return worker_command_result_e::indeterminate;
    }
    if (result.timed_out || result.output_truncated) {
      return worker_command_result_e::indeterminate;
    }
    if (result.exit_status == 0) {
      const auto launched_id = trim_ascii(std::move(result.output));
      return container_id(launched_id) ?
               worker_command_result_e::applied :
               worker_command_result_e::indeterminate;
    }

    try {
      const auto records = inventory_records();
      const auto exact = std::find_if(
        records.begin(),
        records.end(),
        [&spec](const auto &record) {
          return record.observation.identity == spec.identity;
        }
      );
      if (exact == records.end()) {
        return worker_command_result_e::rejected;
      }
      if (!record_matches_spec(*exact, spec)) {
        return worker_command_result_e::indeterminate;
      }
      if (exact->observation.state == worker_observed_state_e::starting ||
          exact->observation.state == worker_observed_state_e::ready) {
        return worker_command_result_e::already_applied;
      }
      return exact->observation.state == worker_observed_state_e::stopped ?
               worker_command_result_e::rejected :
               worker_command_result_e::indeterminate;
    } catch (...) {
      return worker_command_result_e::indeterminate;
    }
  }

  worker_command_result_e backend_t::stop(
    const worker_identity_t &identity,
    worker_stop_mode_e mode
  ) {
    if (!identity.seat.valid() ||
        !opaque_name_token(identity.seat.controller_epoch, 64) ||
        !opaque_name_token(identity.seat.logical_gpu_id) ||
        !opaque_name_token(identity.worker_name)) {
      return worker_command_result_e::rejected;
    }

    std::vector<container_record_t> records;
    try {
      records = inventory_records(false);
    } catch (...) {
      return worker_command_result_e::indeterminate;
    }
    const auto exact = std::find_if(
      records.begin(),
      records.end(),
      [&identity](const auto &record) {
        return record.observation.identity == identity;
      }
    );
    if (exact == records.end() ||
        exact->observation.state == worker_observed_state_e::stopped) {
      return worker_command_result_e::not_found;
    }
    if (exact->observation.state == worker_observed_state_e::stopping) {
      return worker_command_result_e::already_applied;
    }

    auto argv = command_prefix(options_);
    if (mode == worker_stop_mode_e::force) {
      argv.insert(argv.end(), {"rm", "--force", exact->container_id});
    } else if (exact->runtime_state == "created" ||
               exact->runtime_state == "configured" ||
               exact->runtime_state == "initialized") {
      argv.insert(argv.end(), {"rm", exact->container_id});
    } else {
      argv.insert(argv.end(), {"kill", "--signal=TERM", exact->container_id});
    }

    command_result_t result;
    try {
      result = host_.run(
        argv,
        options_.command_timeout,
        options_.max_command_output_bytes
      );
    } catch (...) {
      return worker_command_result_e::indeterminate;
    }
    if (result.timed_out || result.output_truncated) {
      return worker_command_result_e::indeterminate;
    }
    if (result.exit_status == 0) {
      return worker_command_result_e::applied;
    }

    try {
      records = inventory_records(false);
      const auto still_present = std::find_if(
        records.begin(),
        records.end(),
        [&identity](const auto &record) {
          return record.observation.identity == identity &&
                 record.observation.state != worker_observed_state_e::stopped;
        }
      );
      return still_present == records.end() ?
               worker_command_result_e::not_found :
               worker_command_result_e::rejected;
    } catch (...) {
      return worker_command_result_e::indeterminate;
    }
  }

  std::vector<worker_observation_t> backend_t::inventory() {
    const auto records = inventory_records();
    std::vector<worker_observation_t> observations;
    observations.reserve(records.size());
    for (const auto &record : records) {
      observations.push_back(record.observation);
    }
    return observations;
  }

  std::vector<backend_t::container_record_t> backend_t::inventory_records(
    bool require_input_authority
  ) {
    if (!base_host_ready()) {
      throw std::runtime_error {"container engine client is unavailable"};
    }
    if (require_input_authority && !runtime_ready()) {
      throw std::runtime_error {"the admitted crun runtime is unavailable"};
    }
    const auto require_current_gpu_catalog = [this, require_input_authority]() {
      if (require_input_authority && !gpu_catalog_current()) {
        throw std::runtime_error {"Podman GPU authority is not current"};
      }
    };
    require_current_gpu_catalog();

    require_docker_engine();
    auto listing_argv = command_prefix(options_);
    listing_argv.insert(listing_argv.end(), {
      "ps", "--all", "--no-trunc",
      "--filter=label=" + std::string {label_deployment} + "=" + options_.deployment_id,
      "--format={{.ID}}",
    });
    const auto listed = host_.run(
      listing_argv,
      options_.command_timeout,
      options_.max_command_output_bytes
    );
    if (listed.timed_out || listed.output_truncated || listed.exit_status != 0) {
      throw std::runtime_error {"container inventory listing " + describe_command_failure(listed)};
    }
    const auto ids = parse_container_ids(listed.output, options_.max_inventory_workers);
    if (ids.empty()) {
      require_current_gpu_catalog();
      return {};
    }

    auto inspect_argv = command_prefix(options_);
    inspect_argv.insert(inspect_argv.end(), {"container", "inspect"});
    inspect_argv.insert(inspect_argv.end(), ids.begin(), ids.end());
    const auto inspected = host_.run(
      inspect_argv,
      options_.command_timeout,
      options_.max_command_output_bytes
    );
    if (inspected.timed_out || inspected.output_truncated || inspected.exit_status != 0) {
      // A worker that exits between the listing and this call is the usual one.
      throw std::runtime_error {"container inventory inspection " + describe_command_failure(inspected)};
    }

    try {
      const auto document = json::parse(inspected.output);
      if (!document.is_array() || document.size() != ids.size()) {
        throw std::runtime_error {"unexpected Podman inspection cardinality"};
      }

      std::unordered_set<std::string> expected_ids {ids.begin(), ids.end()};
      std::unordered_set<std::string> observed_ids;
      std::vector<container_record_t> records;
      records.reserve(document.size());
      for (const auto &container : document) {
        const auto id = string_member(container, "Id");
        auto name = string_member(container, "Name");
        if (options_.engine == engine_e::docker && name && name->starts_with("/")) name->erase(0, 1);
        const auto *config = object_member(container, "Config");
        const auto *label_object = config ? object_member(*config, "Labels") : nullptr;
        const auto *host_config = object_member(container, "HostConfig");
        const auto *device_array = host_config ? object_member(*host_config, "Devices") : nullptr;
        const auto *state = object_member(container, "State");
        const auto runtime_state = state ? string_member(*state, "Status") : std::nullopt;
        if (!id || !name || !container_id(*id) ||
            !expected_ids.contains(*id) || !observed_ids.emplace(*id).second ||
            !label_object || !label_object->is_object() || !runtime_state) {
          throw std::runtime_error {"invalid Podman inspection record"};
        }

        std::vector<std::pair<std::string, std::string>> labels;
        labels.reserve(label_object->size());
        for (const auto &[key, value] : label_object->items()) {
          if (!value.is_string()) {
            throw std::runtime_error {"invalid Podman worker label"};
          }
          labels.emplace_back(key, value.get<std::string>());
        }

        const auto protocol = label_value(labels, label_protocol);
        const auto deployment = label_value(labels, label_deployment);
        const auto controller = label_value(labels, label_controller);
        const auto gpu = label_value(labels, label_gpu);
        const auto slot_text = label_value(labels, label_slot);
        const auto generation_text = label_value(labels, label_generation);
        const auto worker = label_value(labels, label_worker);
        const auto runtime = label_value(labels, label_runtime);
        const auto capture_wayland = label_value(labels, label_capture_wayland);
        const auto wayland = label_value(labels, label_wayland);
        const auto audio = label_value(labels, label_audio);
        const auto input = label_value(labels, label_input);
        const auto input_fingerprint = label_value(labels, label_input_manifest);
        const auto render = label_value(labels, label_render_node);
        const auto runtime_profile = label_value(labels, label_runtime_profile);
        const auto workload_kind = label_value(labels, label_workload_kind);
        const auto workload_target = label_value(labels, label_workload_target);
        const auto display_topology = label_value(labels, label_display_topology);
        const auto media_pipeline = label_value(labels, label_media_pipeline);
        const auto runtime_image = label_value(labels, label_runtime_image);
        const auto display_width_text = label_value(labels, label_display_width);
        const auto display_height_text = label_value(labels, label_display_height);
        const auto display_refresh_text = label_value(labels, label_display_refresh);
        const auto display_hdr = label_value(labels, label_display_hdr);
        const auto compositor = label_value(labels, label_compositor);
        const auto encoders_text = label_value(labels, label_encoders);
        const auto volume = label_value(labels, label_volume);
        const auto slot = slot_text ? parse_decimal<std::uint32_t>(*slot_text) : std::nullopt;
        const auto generation = generation_text ?
                                  parse_decimal<std::uint64_t>(*generation_text) :
                                  std::nullopt;
        const auto encoders = encoders_text ?
                                parse_decimal<std::uint32_t>(*encoders_text) :
                                std::nullopt;
        const auto display_width = display_width_text ?
                                     parse_decimal<std::uint32_t>(*display_width_text) :
                                     std::nullopt;
        const auto display_height = display_height_text ?
                                      parse_decimal<std::uint32_t>(*display_height_text) :
                                      std::nullopt;
        const auto display_refresh = display_refresh_text ?
                                       parse_decimal<std::uint32_t>(*display_refresh_text) :
                                       std::nullopt;
        const seat_display_mode_t display_mode {
          .width = display_width.value_or(0),
          .height = display_height.value_or(0),
          .refresh_millihz = display_refresh.value_or(0),
          .hdr = display_hdr && *display_hdr == "1",
        };
        const workload_plan_t workload {
          .kind = workload_kind ?
                    workload_kind_from_name(*workload_kind) :
                    workload_kind_e::unknown,
          .target_id = workload_target.value_or(std::string {}),
        };
        const auto parsed_runtime_profile = runtime_profile ?
                                              runtime_profile_from_name(*runtime_profile) :
                                              runtime_profile_e::unknown;
        if (!protocol || *protocol != "3" ||
            !deployment || *deployment != options_.deployment_id ||
            !controller || !opaque_name_token(*controller, 64) ||
            !gpu || !opaque_name_token(*gpu) ||
            !slot || !generation || *generation == 0 ||
            !worker || !opaque_name_token(*worker) || *worker != *name ||
            !runtime || !opaque_name_token(*runtime) ||
            !capture_wayland || !opaque_name_token(*capture_wayland) ||
            !wayland || !opaque_name_token(*wayland) ||
            !audio || !opaque_name_token(*audio) ||
            !input || !opaque_name_token(*input) ||
            !input_fingerprint || !lowercase_sha256(*input_fingerprint) ||
            !render || !device_path(*render) ||
            !runtime_profile || !valid_runtime_profile_name(*runtime_profile) ||
            !workload_kind || !valid_workload_kind_name(*workload_kind) ||
            !workload_target || !valid_workload_plan(workload) ||
            !workload_matches_runtime_profile(workload, parsed_runtime_profile) ||
            !display_topology || *display_topology != display_topology_name ||
            !media_pipeline || *media_pipeline != media_pipeline_name ||
            !runtime_image || !pinned_image_reference(*runtime_image, options_.engine) ||
            !display_width || !display_height || !display_refresh ||
            !display_hdr || (*display_hdr != "0" && *display_hdr != "1") ||
            !valid_display_mode(display_mode) ||
            !compositor || !valid_compositor_name(*compositor) ||
            !volume || !opaque_name_token(*volume) ||
            !encoders || *encoders == 0) {
          throw std::runtime_error {"incomplete Podman worker identity labels"};
        }
        if (std::none_of(
              options_.profiles.begin(),
              options_.profiles.end(),
              [&volume](const auto &profile) {
                return profile.opaque_volume_name == *volume;
              }
            )) {
          throw std::runtime_error {"unknown Podman worker volume"};
        }

        const seat_handle_t seat_handle {
          .controller_epoch = *controller,
          .logical_gpu_id = *gpu,
          .slot = *slot,
          .generation = *generation,
        };
        bool input_binding_authoritative = false;
        const auto allocation = require_input_authority ?
                                  input_allocation_for(seat_handle, *input) :
                                  std::nullopt;
        // A stopped worker with no exact allocation to authenticate against
        // (released, or no longer resolving for this seat label, plan, or
        // fingerprint) cannot use input and has nothing left to prove. It
        // stays visible as stopped without input authority, so the broker can
        // release its seat and the container can be reaped instead of failing
        // every inventory until Podman removes it. A stopped worker whose
        // allocation still resolves keeps the full check.
        // A container Podman created but never initialized has no runtime
        // spec yet and can never start under this controller, so it takes
        // the same path once its allocation is gone: visible as stopped, so
        // it gets reaped instead of blinding the inventory.
        const auto lowercase_state = lowercase_ascii(*runtime_state);
        const bool never_started =
          lowercase_state == "created" || lowercase_state == "configured";
        const bool released_stopped_worker =
          require_input_authority && !allocation &&
          (observed_state(*runtime_state, std::string {}) ==
             worker_observed_state_e::stopped ||
           never_started);
        if (require_input_authority && !released_stopped_worker) {
          const auto oci_runtime = string_member(container, "OCIRuntime");
          const auto *groups = host_config ? object_member(*host_config, "GroupAdd") : nullptr;
          if (options_.engine == engine_e::podman &&
              (string_member(*config, "User") != std::to_string(host_.effective_uid()) ||
              !oci_runtime || *oci_runtime != options_.runtime_executable.native() ||
              !groups || !groups->is_array() || !groups->empty())) {
            throw std::runtime_error {"Podman worker does not use the admitted input access policy"};
          }
          if (!device_array || !device_array->is_array() ||
              device_array->size() > maximum_inspected_devices) {
            throw std::runtime_error {"invalid Podman device inventory"};
          }
          const auto configured_gpu = std::find_if(
            options_.gpus.begin(),
            options_.gpus.end(),
            [&gpu, &render](const auto &candidate) {
              return candidate.logical_gpu_id == *gpu &&
                     candidate.render_node == *render;
            }
          );
          if (configured_gpu == options_.gpus.end() ||
              !gpu_catalog_current() || !allocation ||
              !input_allocation_current(*allocation)) {
            throw std::runtime_error {"Podman input authority is not current"};
          }
          const auto expected_fingerprint = input_manifest_fingerprint(*allocation);
          if (!expected_fingerprint || *expected_fingerprint != *input_fingerprint) {
            throw std::runtime_error {"Podman input manifest label changed"};
          }

          std::vector<declared_device_binding_t> inspected;
          inspected.reserve(device_array->size());
          for (const auto &device : *device_array) {
            const auto host_path = string_member(device, "PathOnHost");
            const auto worker_path = string_member(device, "PathInContainer");
            const auto permissions = string_member(device, "CgroupPermissions");
            if (!host_path || !worker_path ||
                (options_.engine == engine_e::docker && permissions != "rw")) {
              throw std::runtime_error {"invalid Podman device binding"};
            }
            inspected.push_back({
              .host_path = *host_path,
              .worker_path = *worker_path,
              .permissions = permissions.value_or(std::string {}),
            });
          }
          // The OCI runtime spec is what the runtime applied, so it is the
          // device set when Podman points at it; rootless Podman reports its
          // device bind mounts nowhere else. An inspected device list, when
          // Podman fills one in, must then agree with the spec entry for
          // entry rather than add to it. A live worker also needs the OCI
          // keep-groups annotation, so inspection alone is insufficient.
          runtime_spec_expectations_t expectations {
            .container_id = *id,
            .volume_name = *volume,
          };
          const auto authority = options_.ipc_root / *runtime;
          expectations.controller_binds.emplace(
            (authority / ipc_directory).native(),
            expected_bind_t {std::string {container_ipc_directory}, "rw"}
          );
          expectations.controller_binds.emplace(
            (authority / auth_directory).native(),
            expected_bind_t {std::string {container_auth_directory}, "ro"}
          );
          for (const auto &mount : options_.shared_game_mounts) {
            expectations.controller_binds.emplace(
              mount.host_path.native(),
              expected_bind_t {
                std::string {shared_game_mount_root} + mount.mount_name,
                "ro",
              }
            );
          }
          const auto driver_profile = std::find_if(
            options_.profiles.begin(),
            options_.profiles.end(),
            [&volume](const auto &candidate) { return candidate.opaque_volume_name == *volume; }
          );
          if (driver_profile != options_.profiles.end() && driver_profile->host_driver_libraries) {
            // Borrowed driver files are part of the expected set, so the count
            // check and the one-for-one comparison below keep holding: an
            // injected or re-pointed driver bind is still a rejected container.
            for (const auto &mount : options_.host_driver.mounts) {
              expectations.controller_binds.emplace(
                mount.host_path.native(),
                expected_bind_t {mount.destination, "ro"}
              );
            }
          }
          std::vector<declared_device_binding_t> declared;
          if (options_.engine == engine_e::docker) {
            validate_docker_record(container, expectations, labels);
            declared = inspected;
          } else if (const auto *spec_path = object_member(container, "OCIConfigPath")) {
            if (!spec_path->is_string()) {
              throw std::runtime_error {"invalid Podman runtime spec path"};
            }
            declared = runtime_spec_device_bindings(
              spec_path->get<std::string>(),
              expectations
            );
            for (const auto &binding : inspected) {
              const auto agrees = std::any_of(
                declared.begin(),
                declared.end(),
                [&binding](const auto &candidate) {
                  return candidate.host_path == binding.host_path &&
                         candidate.worker_path == binding.worker_path &&
                         (binding.permissions.empty() ||
                          binding.permissions == candidate.permissions);
                }
              );
              if (!agrees) {
                throw std::runtime_error {
                  "Podman device inventory contradicts the runtime spec"
                };
              }
            }
          } else {
            throw std::runtime_error {"Podman worker is missing its OCI input access evidence"};
          }
          if (declared.size() > maximum_inspected_devices) {
            throw std::runtime_error {"invalid Podman device inventory"};
          }

          std::vector<runtime_device_binding_t> bindings;
          bindings.reserve(declared.size());
          std::set<std::filesystem::path> worker_paths;
          for (const auto &binding : declared) {
            const std::filesystem::path host_path {binding.host_path};
            const std::filesystem::path worker_path {binding.worker_path};
            if (!device_path(host_path) || !device_path(worker_path) ||
                (!binding.permissions.empty() && binding.permissions != "rw") ||
                !worker_paths.emplace(worker_path).second) {
              throw std::runtime_error {"invalid Podman device binding"};
            }
            const auto identity = host_.read_write_character_device(host_path);
            if (!identity) {
              throw std::runtime_error {"Podman device binding is unavailable"};
            }
            bindings.push_back({
              .host_path = host_path,
              .worker_path = worker_path,
              .identity = *identity,
            });
          }
          if (!inspected_bindings_match(bindings, *configured_gpu, *allocation)) {
            throw std::runtime_error {"Podman device bindings changed"};
          }
          input_binding_authoritative = true;
        }

        std::string health_state;
        if (const auto *health = object_member(*state, "Health")) {
          health_state = string_member(*health, "Status").value_or(std::string {});
        } else if (const auto *healthcheck = object_member(*state, "Healthcheck")) {
          health_state = string_member(*healthcheck, "Status").value_or(std::string {});
        }
        records.push_back({
          .container_id = *id,
          .observation = {
            .identity = {
              .seat = {
                .controller_epoch = seat_handle.controller_epoch,
                .logical_gpu_id = seat_handle.logical_gpu_id,
                .slot = seat_handle.slot,
                .generation = seat_handle.generation,
              },
              .worker_name = *worker,
            },
            .state = released_stopped_worker && never_started ?
                       worker_observed_state_e::stopped :
                       observed_state(*runtime_state, health_state),
          },
          .runtime_state = lowercase_ascii(*runtime_state),
          .labels = std::move(labels),
          .input_binding_authoritative = input_binding_authoritative,
        });
      }
      require_current_gpu_catalog();
      return records;
    } catch (const std::exception &error) {
      throw std::runtime_error {
        std::string {"container engine returned invalid worker inventory: "} +
        error.what()
      };
    } catch (...) {
      throw std::runtime_error {"container engine returned invalid worker inventory"};
    }
  }

  bool backend_t::record_matches_spec(
    const container_record_t &record,
    const worker_launch_spec_t &spec
  ) const {
    if (record.observation.identity != spec.identity ||
        !record.input_binding_authoritative) {
      return false;
    }
    const auto *profile = profile_for(spec.profile_key);
    if (!profile || profile->runtime_profile != spec.runtime_profile) {
      return false;
    }
    const auto allocation = input_allocation_for(
      spec.identity.seat,
      spec.resources.input_seat
    );
    if (!allocation || !input_allocation_current(*allocation)) {
      return false;
    }
    const auto fingerprint = input_manifest_fingerprint(*allocation);
    if (!fingerprint) {
      return false;
    }
    for (const auto &[name, value] : labels_for(
           options_,
           spec,
           *profile,
           *fingerprint
         )) {
      const auto actual = label_value(record.labels, name);
      if (!actual || *actual != value) {
        return false;
      }
    }
    return true;
  }

}  // namespace multiseat::container

#endif
