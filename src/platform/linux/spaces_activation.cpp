#include "spaces_activation.h"
#ifdef __linux__
#include "spaces_setup.h"
#include "multiseat_launch_service.h"
#include "multiseat_steam_seccomp.h"
#include "src/config.h"
#include "src/configuration_store.h"
#include "src/crypto.h"
#include "src/utility.h"
#include <algorithm>
#include <cctype>
#include <dlfcn.h>
#include <fstream>
#include <regex>
#include <set>
#include <unistd.h>

namespace multiseat::spaces {
  namespace {
    namespace fs = std::filesystem;
    namespace psf = private_state_file;
    using json = nlohmann::json;
    std::string small_file(const fs::path &path) {
      std::ifstream file(path);
      std::string result(4097, '\0');
      file.read(result.data(), result.size());
      result.resize(file.gcount());
      if (result.size() > 4096) return {};
      while (!result.empty() && std::isspace(static_cast<unsigned char>(result.back()))) result.pop_back();
      return result;
    }
    bool same_file(const fs::path &path, const std::string &payload) {
      // Never overwrite an earlier selection or adopt a different controller.
      return bool(psf::update_atomic(path, 65536, [&](const auto &old) -> std::optional<std::string> {
        if (old.status == psf::read_status_e::missing || (old && old.payload == payload)) return payload;
        return {};
      }));
    }
    bool disabled(const std::unordered_map<std::string, std::string> &values, const std::string &key) {
      const auto found = values.find(key);
      if (found == values.end()) return true;
      return found->second == "false" || found->second == "0" || found->second == "disabled" || found->second == "off";
    }
    std::optional<std::string> worker_label() {
      std::error_code ec;
      if (!fs::exists("/sys/fs/selinux/enforce", ec)) return ec ? std::nullopt : std::optional<std::string> {""};
      // Ask the running policy whether the dedicated worker type is installed.
      // This does not modify the policy, enforcement, device labels or booleans.
      void *library = dlopen("libselinux.so.1", RTLD_NOW | RTLD_LOCAL);
      if (!library) return {};
      auto close = util::fail_guard([&] { dlclose(library); });
      const auto check = reinterpret_cast<int (*)(const char *)>(dlsym(library, "security_check_context"));
      if (!check || check("system_u:system_r:polaris_nvidia_worker_t:s0") != 0) return {};
      return "polaris_nvidia_worker_t";
    }
  }

  std::vector<graphics_t> discover_graphics(container::host_t &host, const graphics_roots_t &roots) {
    std::vector<graphics_t> result;
    std::set<fs::path> physical;
    for (unsigned node = 128; node < 192; ++node) {
      try {
        const std::string render = "renderD" + std::to_string(node);
        const auto device = fs::canonical(roots.drm / render / "device");
        const auto pci = device.filename().string();
        if (!std::regex_match(pci, std::regex("[0-9a-f]{4}:[0-9a-f]{2}:[0-9a-f]{2}\\.[0-7]")) ||
            physical.contains(device)) continue;
        const auto vendor = small_file(device / "vendor");
        if (vendor != "0x10de" && vendor != "0x1002" && vendor != "0x8086") continue;
        graphics_t entry;
        entry.variant = vendor == "0x10de" ? "nvidia" : "default";
        entry.label = (vendor == "0x10de" ? "NVIDIA" : vendor == "0x1002" ? "AMD" : "Intel") + std::string(" graphics ") + std::to_string(result.size() + 1);
        entry.gpu.logical_gpu_id = "pci-" + pci;
        std::replace(entry.gpu.logical_gpu_id.begin(), entry.gpu.logical_gpu_id.end(), ':', '_');
        entry.gpu.render_node = fs::path("/dev/dri") / render;
        entry.gpu.devices.push_back(entry.gpu.render_node);
        for (const auto &child : fs::directory_iterator(device / "drm")) {
          if (std::regex_match(child.path().filename().string(), std::regex("card[0-9]+")))
            entry.gpu.devices.push_back(fs::path("/dev/dri") / child.path().filename());
        }
        if (entry.gpu.devices.size() != 2) continue;
        if (entry.variant == "nvidia") {
          const auto info = small_file(roots.nvidia / pci / "information");
          std::smatch model;
          if (std::regex_search(info, model, std::regex("Model:[ \\t]+([^\\r\\n]+)")) && model[1].length() <= 128)
            entry.label = model[1].str();
          std::smatch match;
          if (!std::regex_search(info, match, std::regex("Device Minor:[ \t]+([0-9]{1,3})([\r\n]|$)"))) continue;
          entry.gpu.devices.push_back("/dev/nvidia" + match[1].str());
          for (const char *path : {"/dev/nvidiactl", "/dev/nvidia-modeset", "/dev/nvidia-uvm"}) entry.gpu.devices.emplace_back(path);
        }
        std::set<std::pair<std::uint64_t, std::uint64_t>> identities;
        bool accessible = true;
        for (const auto &path : entry.gpu.devices) {
          const auto identity = host.read_write_character_device(path);
          if (!identity || !identities.emplace(identity->character_major, identity->character_minor).second) { accessible = false; break; }
        }
        if (!accessible) continue;
        physical.emplace(device);
        result.push_back(std::move(entry));
      } catch (...) { /* An inaccessible or changing GPU cannot be selected. */ }
    }
    return result;
  }

  json graphics_choices(const runtime_t &runtime) {
    container::local_host_t host;
    json choices = json::array();
    for (const auto &entry : discover_graphics(host)) {
      if (entry.variant == runtime.variant) choices.push_back({{"id", entry.gpu.logical_gpu_id}, {"label", entry.label}});
    }
    return choices;
  }

  activation_paths_t activation_paths(const fs::path &directory, const fs::path &native) {
    // Keep Unix socket paths short as controller generations gain digits.
    // A hash collision cannot adopt another root: its full controller marker
    // must match before use. The preview requires UID/GID 1000.
    const auto key = util::hex(crypto::hash(directory.string())).to_string().substr(0, 8);
    return {native, directory / "spaces-controller.json", directory / "spaces-profiles.json",
      fs::path("/run/user") / std::to_string(geteuid()) / ("ps-" + key)};
  }

  bool managed_graphics_current(const activation_paths_t &paths) {
    const auto options = load_controller_options(paths.controller);
    if (!options || options->gpus.size() != 1) return false;
    container::local_host_t host;
    const auto choices = discover_graphics(host);
    const auto &selected = options->gpus.front();
    return std::any_of(choices.begin(), choices.end(), [&](const auto &entry) {
      return entry.gpu.logical_gpu_id == selected.logical_gpu_id && entry.gpu.render_node == selected.render_node &&
        std::set<std::filesystem::path>(entry.gpu.devices.begin(), entry.gpu.devices.end()) ==
          std::set<std::filesystem::path>(selected.devices.begin(), selected.devices.end());
    });
  }

  bool prepare_managed_ipc(const activation_paths_t &paths) {
    container::local_host_t host;
    const auto options = load_controller_options(paths.controller);
    if (!options || options->profile_catalog != paths.profiles || options->container.ipc_root != paths.ipc) return false;
    // Secure persistence creates private parents and refuses symlink/ownership
    // changes. A stable marker binds this directory to this managed controller.
    return same_file(paths.ipc / ".owner", json {{"controller", paths.controller.string()}}.dump()) &&
      host.private_read_write_directory(paths.ipc);
  }

  bool configure_first_space(const activation_paths_t &paths,
    const profiles::first_steam_request_t &request, std::string_view image,
    const graphics_t &graphics, std::string_view selinux_type, container::host_t &host) {
    std::lock_guard lock(configuration_store::mutex());
    if (!profiles::valid_first_steam_request(request) || !host.private_read_write_directory(paths.ipc.parent_path())) return false;
    const auto current = configuration_store::read(paths.native.string());
    if (!current) return false;
    const auto values = config::parse_config(current->contents);
    const auto controller = values.find("multiseat_config");
    const bool already = controller != values.end() && controller->second == paths.controller &&
      values.contains("multiseat_enabled") && values.at("multiseat_enabled") == "true";
    if (!disabled(values, "multiseat_moonlight_input") ||
        (!already && (!disabled(values, "multiseat_enabled") ||
          (controller != values.end() && !controller->second.empty())))) return false;
    const auto loaded = profiles::load(paths.profiles);
    if (!loaded || loaded->catalog.owner_uid != host.effective_uid() || loaded->catalog.owner_gid != host.effective_gid() ||
        loaded->catalog.profiles.size() != 1) return false;
    const auto &profile = loaded->catalog.profiles.front();
    if (profile.storage.profile_key != request.request_id || profile.name != request.name ||
        profile.storage.image_reference != image || profile.workload.kind != workload_kind_e::steam) return false;
    const auto &gpu = graphics.gpu;
    json devices = json::array();
    for (const auto &path : gpu.devices) {
      if (!host.read_write_character_device(path)) return false;
      devices.push_back(path.string());
    }
    const json body {{"schema", 1}, {"deployment_id", "spaces-" + request.request_id},
      {"profile_catalog", paths.profiles.string()}, {"ipc_root", paths.ipc.string()}, {"selinux_type", selinux_type},
      {"gpus", json::array({{{"id", gpu.logical_gpu_id}, {"render_node", gpu.render_node.string()},
        {"devices", devices}, {"max_seats", 1}, {"max_encoder_sessions", 1}}})}};
    if (!same_file(paths.controller, body.dump()) || !load_controller_options(paths.controller) ||
        !prepare_managed_ipc(paths)) return false;
    if (already) return true;
    return configuration_store::patch(paths.native.string(), {
      {"multiseat_enabled", "true"}, {"multiseat_config", paths.controller.string()}}, current->revision) ==
      configuration_store::result::committed;
  }

  bool activate_first_space(const fs::path &directory, const profiles::first_steam_request_t &request,
    const runtime_t &runtime, std::string_view gpu_id, std::stop_token stop) {
    if (stop.stop_requested() || config::multiseat.enabled || config::input.multiseat_moonlight_input ||
        !config::multiseat.config_file.empty()) return false;
    container::local_host_t host(stop);
    if (!inspect_setup(host, false, false).at("host_prerequisites_ready").get<bool>() ||
        !host.trusted_data_file(std::filesystem::path(container::steam_seccomp_path), container::steam_seccomp_data)) return false;
    if (runtime.variant == "nvidia" && small_file("/sys/module/nvidia/version") != runtime.nvidia_driver) return false;
    const auto label = worker_label();
    if (!label) return false;
    const auto choices = discover_graphics(host);
    const auto found = std::find_if(choices.begin(), choices.end(), [&](const auto &g) {
      return g.gpu.logical_gpu_id == gpu_id && g.variant == runtime.variant;
    });
    if (found == choices.end()) return false;
    // Recheck the local exact image and matching NVIDIA driver without pulling.
    auto command = container::command_prefix({});
    command.insert(command.end(), {"image", "inspect", runtime.config_digest});
    const auto image = host.run(command, std::chrono::seconds(5), 65536);
    if ((image.exit_status != 0 || image.timed_out || image.output_truncated) || !matches_runtime_image(runtime, image.output) || stop.stop_requested()) return false;
    return configure_first_space(activation_paths(directory, config::sunshine.config_file), request,
      runtime.config_digest, *found, *label, host);
  }
}
#endif
