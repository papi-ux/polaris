/** First-Space graphics discovery and restart-based configuration. */
#pragma once
#ifdef __linux__
#include "spaces_runtime.h"
#include "multiseat_profile_catalog.h"
#include "multiseat_controller_production.h"

namespace multiseat::spaces {
  struct graphics_t {
    production_controller_gpu_t gpu;
    std::string label, variant;
  };
  // Browser selections name a discovered GPU; no browser paths or device lists.
  struct graphics_roots_t {
    std::filesystem::path drm = "/sys/class/drm", nvidia = "/proc/driver/nvidia/gpus";
  };
  std::vector<graphics_t> discover_graphics(container::host_t &host, const graphics_roots_t &roots = {});
  nlohmann::json graphics_choices(const runtime_t &runtime);
  struct activation_paths_t {
    std::filesystem::path native, controller, profiles, ipc;
  };
  activation_paths_t activation_paths(const std::filesystem::path &directory,
    const std::filesystem::path &native);
  // Final native-config replacement is the activation commit point. Earlier
  // private files are inert, immutable on retry, and never start a controller.
  bool configure_first_space(const activation_paths_t &paths,
    const profiles::first_steam_request_t &request, std::string_view image,
    const graphics_t &graphics, std::string_view selinux_type,
    container::host_t &host);
  bool managed_graphics_current(const activation_paths_t &paths);
  bool prepare_managed_ipc(const activation_paths_t &paths);
  bool activate_first_space(const std::filesystem::path &directory,
    const profiles::first_steam_request_t &request, const runtime_t &runtime,
    std::string_view gpu_id, std::stop_token stop);
}
#endif
