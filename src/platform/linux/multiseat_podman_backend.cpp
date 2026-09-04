/**
 * @file src/platform/linux/multiseat_podman_backend.cpp
 * @brief Rootless Podman worker backend for isolated multiseat workers.
 */
#include "multiseat_podman_backend.h"
#include "multiseat_worker_authority.h"

#ifdef __linux__

#include <nlohmann/json.hpp>

#include <algorithm>
#include <charconv>
#include <limits>
#include <optional>
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
    constexpr auto label_wayland = "io.polaris.multiseat.wayland"sv;
    constexpr auto label_audio = "io.polaris.multiseat.audio"sv;
    constexpr auto label_input = "io.polaris.multiseat.input"sv;
    constexpr auto label_render_node = "io.polaris.multiseat.render-node"sv;
    constexpr auto label_compositor = "io.polaris.multiseat.compositor"sv;
    constexpr auto label_encoders = "io.polaris.multiseat.encoders"sv;
    constexpr auto capability_file = worker_ipc::authority_capability_file_name;
    constexpr auto ipc_directory = worker_ipc::authority_ipc_directory_name;
    constexpr auto auth_directory = worker_ipc::authority_auth_directory_name;
    constexpr auto container_ipc_directory = "/run/polaris-ipc"sv;
    constexpr auto container_auth_directory = "/run/polaris-auth"sv;

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
      const worker_launch_spec_t &spec
    ) {
      return {
        {std::string {label_protocol}, "1"},
        {std::string {label_deployment}, options.deployment_id},
        {std::string {label_controller}, spec.identity.seat.controller_epoch},
        {std::string {label_gpu}, spec.identity.seat.logical_gpu_id},
        {std::string {label_slot}, std::to_string(spec.identity.seat.slot)},
        {std::string {label_generation}, std::to_string(spec.identity.seat.generation)},
        {std::string {label_worker}, spec.identity.worker_name},
        {std::string {label_runtime}, spec.resources.runtime_namespace},
        {std::string {label_wayland}, spec.resources.wayland_socket},
        {std::string {label_audio}, spec.resources.audio_sink},
        {std::string {label_input}, spec.resources.input_seat},
        {std::string {label_render_node}, spec.render_node},
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
          !pinned_image_reference(options.image_reference) ||
          options.gpus.empty() ||
          options.profiles.empty() ||
          options.workload_keys.empty() ||
          options.input_devices.empty() ||
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
      for (const auto &gpu : options.gpus) {
        if (!opaque_name_token(gpu.logical_gpu_id) ||
            !device_path(gpu.render_node) ||
            gpu.devices.empty() ||
            gpu.max_encoder_sessions == 0 ||
            !gpu_ids.emplace(gpu.logical_gpu_id).second) {
          throw std::invalid_argument {"rootless Podman GPU options are invalid"};
        }
        std::unordered_set<std::string> devices;
        bool render_node_present = false;
        for (const auto &device : gpu.devices) {
          if (!device_path(device) || !devices.emplace(device.native()).second) {
            throw std::invalid_argument {"rootless Podman GPU devices are invalid"};
          }
          render_node_present = render_node_present || device == gpu.render_node;
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
            !profile_keys.emplace(profile.profile_key).second ||
            !profile_volumes.emplace(profile.opaque_volume_name).second) {
          throw std::invalid_argument {"rootless Podman profile options are invalid"};
        }
      }

      std::unordered_set<std::string> workloads;
      for (const auto &workload : options.workload_keys) {
        if (!opaque_reference(workload) || !workloads.emplace(workload).second) {
          throw std::invalid_argument {"rootless Podman workload options are invalid"};
        }
      }

      std::unordered_set<std::string> input_devices;
      for (const auto &device : options.input_devices) {
        if (!device_path(device) || !input_devices.emplace(device.native()).second) {
          throw std::invalid_argument {"rootless Podman input devices are invalid"};
        }
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

  backend_t::backend_t(host_t &host, options_t options) :
      host_(host),
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

  bool backend_t::workload_allowed(const std::string &workload_key) const {
    return std::find(
             options_.workload_keys.begin(),
             options_.workload_keys.end(),
             workload_key
           ) != options_.workload_keys.end();
  }

  bool backend_t::base_host_ready() const {
    return host_.effective_uid() != 0 && host_.executable_file(options_.executable);
  }

  bool backend_t::launch_host_ready(
    const worker_launch_spec_t &spec,
    const gpu_t &gpu
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
    const auto devices_ready = [&]() {
      for (const auto &device : gpu.devices) {
        if (!host_.read_write_character_device(device)) {
          return false;
        }
      }
      for (const auto &device : options_.input_devices) {
        if (!host_.read_write_character_device(device)) {
          return false;
        }
      }
      return true;
    }();
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
        !opaque_name_token(spec.resources.wayland_socket) ||
        !opaque_name_token(spec.resources.audio_sink) ||
        !opaque_name_token(spec.resources.input_seat) ||
        !opaque_reference(spec.profile_key) ||
        !opaque_reference(spec.workload_key) ||
        !device_path(spec.render_node) ||
        !concrete_compositor(spec.compositor) ||
        spec.encoder_sessions == 0) {
      return false;
    }
    const auto *gpu = gpu_for(spec);
    return gpu &&
           spec.encoder_sessions <= gpu->max_encoder_sessions &&
           profile_for(spec.profile_key) &&
           workload_allowed(spec.workload_key);
  }

  std::vector<std::string> backend_t::launch_argv(
    const worker_launch_spec_t &spec,
    const gpu_t &gpu,
    const profile_t &profile
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

    for (const auto &[name, value] : labels_for(options_, spec)) {
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
    add_environment("POLARIS_RUNTIME_NAMESPACE", spec.resources.runtime_namespace);
    add_environment("POLARIS_WORKER_NAME", spec.identity.worker_name);
    add_environment("POLARIS_INPUT_SEAT", spec.resources.input_seat);
    add_environment("POLARIS_CONTROLLER_EPOCH", spec.identity.seat.controller_epoch);
    add_environment("POLARIS_LOGICAL_GPU_ID", spec.identity.seat.logical_gpu_id);
    add_environment("POLARIS_SEAT_SLOT", std::to_string(spec.identity.seat.slot));
    add_environment("POLARIS_SEAT_GENERATION", std::to_string(spec.identity.seat.generation));
    add_environment("POLARIS_RENDER_NODE", spec.render_node);
    add_environment("POLARIS_COMPOSITOR", compositor_name(spec.compositor));
    add_environment("POLARIS_ENCODER_SESSIONS", std::to_string(spec.encoder_sessions));

    for (const auto &device : gpu.devices) {
      argv.push_back(
        "--device=" + device.native() + ":" + device.native() + ":rw"
      );
    }
    for (const auto &device : options_.input_devices) {
      argv.push_back(
        "--device=" + device.native() + ":" + device.native() + ":rw"
      );
    }
    for (const auto &mount : options_.shared_game_mounts) {
      argv.push_back(
        "--mount=type=bind,src=" + mount.host_path.native() +
        ",dst=/mnt/games/" + mount.mount_name + ",ro=true"
      );
    }

    argv.push_back("--entrypoint=" + options_.worker_entrypoint.native());
    argv.push_back(options_.image_reference);
    argv.push_back("run");
    argv.push_back("--workload-key=" + spec.workload_key);
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
    try {
      if (!launch_host_ready(spec, *gpu)) {
        return worker_command_result_e::rejected;
      }
    } catch (...) {
      return worker_command_result_e::rejected;
    }

    command_result_t result;
    try {
      result = host_.run(
        launch_argv(spec, *gpu, *profile),
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
      records = inventory_records();
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
      records = inventory_records();
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

  std::vector<backend_t::container_record_t> backend_t::inventory_records() {
    if (!base_host_ready()) {
      throw std::runtime_error {"rootless Podman is unavailable"};
    }

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
        const auto wayland = label_value(labels, label_wayland);
        const auto audio = label_value(labels, label_audio);
        const auto input = label_value(labels, label_input);
        const auto render = label_value(labels, label_render_node);
        const auto compositor = label_value(labels, label_compositor);
        const auto encoders_text = label_value(labels, label_encoders);
        const auto slot = slot_text ? parse_decimal<std::uint32_t>(*slot_text) : std::nullopt;
        const auto generation = generation_text ?
                                  parse_decimal<std::uint64_t>(*generation_text) :
                                  std::nullopt;
        const auto encoders = encoders_text ?
                                parse_decimal<std::uint32_t>(*encoders_text) :
                                std::nullopt;
        if (!protocol || *protocol != "1" ||
            !deployment || *deployment != options_.deployment_id ||
            !controller || !opaque_name_token(*controller, 64) ||
            !gpu || !opaque_name_token(*gpu) ||
            !slot || !generation || *generation == 0 ||
            !worker || !opaque_name_token(*worker) || *worker != *name ||
            !runtime || !opaque_name_token(*runtime) ||
            !wayland || !opaque_name_token(*wayland) ||
            !audio || !opaque_name_token(*audio) ||
            !input || !opaque_name_token(*input) ||
            !render || !device_path(*render) ||
            !compositor || !valid_compositor_name(*compositor) ||
            !encoders || *encoders == 0) {
          throw std::runtime_error {"incomplete Podman worker identity labels"};
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
                .controller_epoch = *controller,
                .logical_gpu_id = *gpu,
                .slot = *slot,
                .generation = *generation,
              },
              .worker_name = *worker,
            },
            .state = observed_state(*runtime_state, health_state),
          },
          .runtime_state = lowercase_ascii(*runtime_state),
          .labels = std::move(labels),
        });
      }
      return records;
    } catch (...) {
      throw std::runtime_error {"rootless Podman returned invalid worker inventory"};
    }
  }

  bool backend_t::record_matches_spec(
    const container_record_t &record,
    const worker_launch_spec_t &spec
  ) const {
    if (record.observation.identity != spec.identity) {
      return false;
    }
    for (const auto &[name, value] : labels_for(options_, spec)) {
      const auto actual = label_value(record.labels, name);
      if (!actual || *actual != value) {
        return false;
      }
    }
    return true;
  }

}  // namespace multiseat::podman

#endif
