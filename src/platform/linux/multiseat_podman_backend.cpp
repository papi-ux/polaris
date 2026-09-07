/**
 * @file src/platform/linux/multiseat_podman_backend.cpp
 * @brief Rootless Podman worker backend for isolated multiseat workers.
 */
#include "multiseat_podman_backend.h"
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

namespace multiseat::podman {
  namespace {
    using json = nlohmann::json;
    using namespace std::literals;

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
    constexpr auto capability_file = worker_ipc::authority_capability_file_name;
    constexpr auto ipc_directory = worker_ipc::authority_ipc_directory_name;
    constexpr auto auth_directory = worker_ipc::authority_auth_directory_name;
    constexpr auto container_ipc_directory = "/run/polaris-ipc"sv;
    constexpr auto container_auth_directory = "/run/polaris-auth"sv;
    constexpr std::size_t maximum_inspected_devices =
      input::maximum_input_allocations + 64;

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

    bool pinned_image_reference(std::string_view value) {
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

    std::string runtime_profile_name(runtime_profile_e profile) {
      switch (profile) {
        case runtime_profile_e::gamescope:
          return "gamescope";
        case runtime_profile_e::steam:
          return "steam";
        case runtime_profile_e::heroic:
          return "heroic";
        case runtime_profile_e::lutris:
          return "lutris";
        case runtime_profile_e::unknown:
          break;
      }
      return {};
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
        {std::string {label_runtime_profile}, runtime_profile_name(spec.runtime_profile)},
        {std::string {label_workload_kind}, workload_kind_name(spec.workload.kind)},
        {std::string {label_workload_target}, spec.workload.target_id},
        {std::string {label_display_topology}, std::string {display_topology_name}},
        {std::string {label_media_pipeline}, std::string {media_pipeline_name}},
        {std::string {label_runtime_image}, profile.image_reference},
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
        throw std::invalid_argument {"rootless Podman worker options are incomplete"};
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
          throw std::invalid_argument {"rootless Podman GPU options are invalid"};
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
            throw std::invalid_argument {"rootless Podman GPU devices are invalid"};
          }
          render_node_present = render_node_present ||
                                device.path == gpu.render_node;
        }
        if (!render_node_present) {
          throw std::invalid_argument {"rootless Podman GPU device set omits its render node"};
        }
      }

      std::unordered_set<std::string> profile_keys;
      std::unordered_set<std::string> profile_volumes;
      for (const auto &profile : options.profiles) {
        if (!opaque_reference(profile.profile_key) ||
            !opaque_name_token(profile.opaque_volume_name) ||
            !concrete_runtime_profile(profile.runtime_profile) ||
            !pinned_image_reference(profile.image_reference) ||
            !profile_keys.emplace(profile.profile_key).second ||
            !profile_volumes.emplace(profile.opaque_volume_name).second) {
          throw std::invalid_argument {"rootless Podman profile options are invalid"};
        }
      }

      std::vector<workload_plan_t> workloads;
      for (const auto &workload : options.workloads) {
        if (!valid_workload_plan(workload) ||
            std::find(workloads.begin(), workloads.end(), workload) != workloads.end()) {
          throw std::invalid_argument {"rootless Podman workload options are invalid"};
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
          throw std::invalid_argument {"rootless Podman game mounts are invalid"};
        }
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

  bool backend_t::launch_host_ready(
    const worker_launch_spec_t &spec,
    const input::allocation_t &input_allocation
  ) const {
    if (!base_host_ready()) {
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
           workload_allowed(spec.workload);
  }

  std::vector<std::string> backend_t::launch_argv(
    const worker_launch_spec_t &spec,
    const gpu_t &gpu,
    const profile_t &profile,
    const input::allocation_t &input_allocation,
    std::string_view input_fingerprint
  ) const {
    std::vector<std::string> argv {
      options_.executable.native(),
      "--remote=false",
      "run",
      "--detach",
      "--rm",
      "--pull=never",
      "--restart=no",
      "--name=" + spec.identity.worker_name,
      "--hostname=" + spec.identity.worker_name,
      "--userns=keep-id",
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
      "--volume=" + profile.opaque_volume_name + ":/var/lib/polaris-seat:rw,nosuid,nodev,nocreate",
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
    add_environment("POLARIS_RUNTIME_PROFILE", runtime_profile_name(spec.runtime_profile));
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
        ",dst=/mnt/games/" + mount.mount_name + ",ro=true"
      );
    }

    argv.push_back("--entrypoint=" + options_.worker_entrypoint.native());
    argv.push_back(profile.image_reference);
    argv.push_back("run");
    argv.push_back("--workload-kind=" + workload_kind_name(spec.workload.kind));
    argv.push_back("--workload-id=" + spec.workload.target_id);
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
    try {
      if (!launch_host_ready(spec, *input_allocation)) {
        return worker_command_result_e::rejected;
      }
    } catch (...) {
      return worker_command_result_e::indeterminate;
    }

    command_result_t result;
    try {
      const auto argv = launch_argv(
        spec,
        *gpu,
        *profile,
        *input_allocation,
        *input_fingerprint
      );
      if (!gpu_catalog_current() ||
          !input_allocation_current(*input_allocation)) {
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

    std::vector<std::string> argv {
      options_.executable.native(),
      "--remote=false",
    };
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
      throw std::runtime_error {"rootless Podman is unavailable"};
    }
    const auto require_current_gpu_catalog = [this, require_input_authority]() {
      if (require_input_authority && !gpu_catalog_current()) {
        throw std::runtime_error {"Podman GPU authority is not current"};
      }
    };
    require_current_gpu_catalog();

    const auto listed = host_.run(
      {
        options_.executable.native(),
        "--remote=false",
        "ps",
        "--all",
        "--no-trunc",
        "--filter=label=" + std::string {label_deployment} + "=" + options_.deployment_id,
        "--format={{.ID}}",
      },
      options_.command_timeout,
      options_.max_command_output_bytes
    );
    if (listed.timed_out || listed.output_truncated || listed.exit_status != 0) {
      throw std::runtime_error {"rootless Podman inventory listing failed"};
    }
    const auto ids = parse_container_ids(listed.output, options_.max_inventory_workers);
    if (ids.empty()) {
      require_current_gpu_catalog();
      return {};
    }

    std::vector<std::string> inspect_argv {
      options_.executable.native(),
      "--remote=false",
      "container",
      "inspect",
      "--type=container",
    };
    inspect_argv.insert(inspect_argv.end(), ids.begin(), ids.end());
    const auto inspected = host_.run(
      inspect_argv,
      options_.command_timeout,
      options_.max_command_output_bytes
    );
    if (inspected.timed_out || inspected.output_truncated || inspected.exit_status != 0) {
      throw std::runtime_error {"rootless Podman inventory inspection failed"};
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
        const auto name = string_member(container, "Name");
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
            !runtime_image || !pinned_image_reference(*runtime_image) ||
            !display_width || !display_height || !display_refresh ||
            !display_hdr || (*display_hdr != "0" && *display_hdr != "1") ||
            !valid_display_mode(display_mode) ||
            !compositor || !valid_compositor_name(*compositor) ||
            !encoders || *encoders == 0) {
          throw std::runtime_error {"incomplete Podman worker identity labels"};
        }

        const seat_handle_t seat_handle {
          .controller_epoch = *controller,
          .logical_gpu_id = *gpu,
          .slot = *slot,
          .generation = *generation,
        };
        bool input_binding_authoritative = false;
        if (require_input_authority) {
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
          const auto allocation = input_allocation_for(seat_handle, *input);
          if (configured_gpu == options_.gpus.end() ||
              !gpu_catalog_current() || !allocation ||
              !input_allocation_current(*allocation)) {
            throw std::runtime_error {"Podman input authority is not current"};
          }
          const auto expected_fingerprint = input_manifest_fingerprint(*allocation);
          if (!expected_fingerprint || *expected_fingerprint != *input_fingerprint) {
            throw std::runtime_error {"Podman input manifest label changed"};
          }

          std::vector<runtime_device_binding_t> bindings;
          bindings.reserve(device_array->size());
          std::set<std::filesystem::path> worker_paths;
          for (const auto &device : *device_array) {
            const auto host_path = string_member(device, "PathOnHost");
            const auto worker_path = string_member(device, "PathInContainer");
            const auto permissions = string_member(device, "CgroupPermissions");
            if (!host_path || !worker_path ||
                !device_path(*host_path) || !device_path(*worker_path) ||
                (permissions && !permissions->empty() && *permissions != "rw") ||
                !worker_paths.emplace(*worker_path).second) {
              throw std::runtime_error {"invalid Podman device binding"};
            }
            const auto identity = host_.read_write_character_device(*host_path);
            if (!identity) {
              throw std::runtime_error {"Podman device binding is unavailable"};
            }
            bindings.push_back({
              .host_path = *host_path,
              .worker_path = *worker_path,
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
            .state = observed_state(*runtime_state, health_state),
          },
          .runtime_state = lowercase_ascii(*runtime_state),
          .labels = std::move(labels),
          .input_binding_authoritative = input_binding_authoritative,
        });
      }
      require_current_gpu_catalog();
      return records;
    } catch (...) {
      throw std::runtime_error {"rootless Podman returned invalid worker inventory"};
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

}  // namespace multiseat::podman

#endif
