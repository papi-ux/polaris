/**
 * @file src/launch_profile.h
 * @brief Deterministic, evidence-independent launch preset resolution.
 */
#pragma once

#include <nlohmann/json.hpp>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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

  struct non_linux_topology_resolution_t {
    std::string topology = "desktop_display";
    bool launch_owns_refresh_rate = false;
    std::string source = "host_capability";
    std::string reason_code = "platform_desktop_only";
    bool normalized = false;
  };

  /** One explicit launch field the client sent that Polaris cannot honor, and why. */
  struct explicit_field_problem_t {
    std::string field;
    std::string reason;
  };

  /** The explicit launch fields of an optimize request, parsed and bound-checked once. */
  struct explicit_launch_fields_t {
    std::optional<int> width;
    std::optional<int> height;
    std::optional<int> bitrate_kbps;
    std::optional<int> fps_millihertz;
    std::optional<int> client_max_fps_millihertz;
    bool display_locked = false;
    bool bitrate_locked = false;
    bool topology_locked = false;
    std::optional<bool> hdr;
    std::vector<explicit_field_problem_t> problems;

    [[nodiscard]] bool any_explicit_mode() const {
      return width.has_value() || height.has_value() || fps_millihertz.has_value();
    }

    [[nodiscard]] bool complete_explicit_mode() const {
      return width.has_value() && height.has_value() && fps_millihertz.has_value();
    }
  };

  using explicit_field_lookup_t = std::function<std::optional<std::string>(std::string_view)>;

  /**
   * Parse the explicit launch fields of a request. Every problem names the
   * field and the value seen, so a rejection can say which field failed
   * instead of the one generic sentence that hid a comma-decimal fps for weeks.
   */
  explicit_launch_fields_t parse_explicit_launch_fields(const explicit_field_lookup_t &lookup);

  /** The sentence a client is shown for rejected explicit fields; generic when nothing is named. */
  std::string describe_explicit_launch_rejection(const explicit_launch_fields_t &fields);

  std::string normalize_preset(std::string preset);
  std::string preset_label(const std::string &preset);
  non_linux_topology_resolution_t resolve_non_linux_topology(
    const std::string &requested_topology,
    bool topology_locked,
    bool paired_always_virtual,
    bool app_virtual_display,
    bool virtual_display_supported,
    bool host_requires_virtual_display
  );
  resolution_t resolve(const request_t &request);

}  // namespace launch_profile
