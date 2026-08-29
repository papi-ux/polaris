/**
 * @file src/launch_profile.h
 * @brief Deterministic, evidence-independent launch preset resolution.
 */
#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <string>

namespace launch_profile {

  inline constexpr int k_policy_version = 1;

  struct request_t {
    std::string device_name;
    std::string app_name;
    std::string preset = "auto";

    int requested_width = 0;
    int requested_height = 0;
    // Polaris stores launch FPS as milli-FPS (60000 == 60 FPS).
    int requested_fps = 0;
    bool display_locked = false;

    int paired_width = 0;
    int paired_height = 0;
    int paired_fps = 0;
    // Hard refresh capabilities supplied by the authenticated client and the
    // current host. Values use the same milli-FPS representation as requested_fps.
    std::optional<int> client_max_fps;
    std::optional<int> host_max_fps;

    std::optional<int> paired_bitrate_kbps;
    std::optional<int> explicit_bitrate_kbps;
    bool bitrate_locked = false;
    std::optional<int> configured_bitrate_kbps;

    bool hdr_requested = false;
    bool hdr_locked = false;
    std::optional<bool> host_hdr_capable;
    std::optional<bool> client_profile_hdr;
    std::optional<int> color_range;
  };

  struct resolution_t {
    int width = 0;
    int height = 0;
    int fps = 0;
    std::optional<int> target_bitrate_kbps;
    std::optional<std::string> preferred_codec;
    std::optional<int> nvenc_tune;
    std::optional<int> color_range;
    bool hdr = false;
    std::string preset;
    nlohmann::json fields = nlohmann::json::object();
  };

  std::string normalize_preset(std::string preset);
  std::string preset_label(const std::string &preset);
  std::string resolve_non_linux_topology(
    const std::string &requested_topology,
    bool topology_locked,
    bool paired_always_virtual,
    bool app_virtual_display
  );
  resolution_t resolve(const request_t &request);

}  // namespace launch_profile
