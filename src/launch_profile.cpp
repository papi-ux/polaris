#include "launch_profile.h"

#include "device_db.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace launch_profile {
  namespace {
    struct display_mode_t {
      int width = 0;
      int height = 0;
      int fps = 0;
    };

    struct field_provenance_t {
      std::string source;
      std::string reason_code;
      bool locked = false;
      bool normalized = false;
    };

    std::optional<display_mode_t> parse_display_mode(const std::string &value) {
      std::stringstream input(value);
      std::string segment;
      display_mode_t result;
      int index = 0;
      while (std::getline(input, segment, 'x')) {
        try {
          if (index == 0) result.width = std::stoi(segment);
          else if (index == 1) result.height = std::stoi(segment);
          else if (index == 2) {
            const auto fps = std::stod(segment);
            result.fps = static_cast<int>(std::round(fps < 1000.0 ? fps * 1000.0 : fps));
          } else {
            return std::nullopt;
          }
        } catch (...) {
          return std::nullopt;
        }
        ++index;
      }
      if (index != 3 || result.width <= 0 || result.height <= 0 || result.fps <= 0) {
        return std::nullopt;
      }
      return result;
    }

    std::string format_fps(int fps) {
      if (fps <= 0) return "0";
      if (fps % 1000 == 0) return std::to_string(fps / 1000);
      std::ostringstream output;
      output << static_cast<double>(fps) / 1000.0;
      return output.str();
    }

    std::string format_display_mode(const display_mode_t &mode) {
      return std::to_string(mode.width) + "x" + std::to_string(mode.height) + "x" + format_fps(mode.fps);
    }

    void add_field(nlohmann::json &fields,
                   const std::string &name,
                   nlohmann::json value,
                   const std::string &source,
                   const std::string &reason_code,
                   bool locked,
                   bool normalized = false) {
      fields[name] = {
        {"value", std::move(value)},
        {"source", source},
        {"reason_code", reason_code},
        {"locked", locked},
        {"normalized", normalized}
      };
    }

    display_mode_t conservative_mode(const request_t &request,
                                     const std::optional<device_db::device_t> &device,
                                     display_mode_t selected) {
      display_mode_t candidate = selected;
      if (device && !device->display_mode.empty()) {
        if (const auto parsed = parse_display_mode(device->display_mode)) {
          candidate = *parsed;
        }
      }

      // Versioned v1 conservative policy: never raise a requested dimension,
      // and limit the stability preset to 1080p60. This is a selected preset,
      // not a conclusion learned from a previous session.
      if (selected.width > 0) candidate.width = std::min(candidate.width, selected.width);
      if (selected.height > 0) candidate.height = std::min(candidate.height, selected.height);
      if (selected.fps > 0) candidate.fps = std::min(candidate.fps, selected.fps);
      candidate.width = std::min(candidate.width, 1920);
      candidate.height = std::min(candidate.height, 1080);
      candidate.fps = std::min(candidate.fps, 60000);
      return candidate;
    }
  }  // namespace

  std::string normalize_preset(std::string preset) {
    std::transform(preset.begin(), preset.end(), preset.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    if (preset == "quality" || preset == "high_fps" || preset == "stability") {
      return preset;
    }
    return "auto";
  }

  std::string preset_label(const std::string &preset) {
    if (preset == "quality") return "Quality";
    if (preset == "high_fps") return "High FPS";
    if (preset == "stability") return "Stability";
    return "Auto";
  }

  std::string resolve_non_linux_topology(
      const std::string &requested_topology,
      bool topology_locked,
      bool paired_always_virtual,
      bool app_virtual_display) {
    if (topology_locked && !requested_topology.empty()) {
      return requested_topology;
    }
    if (paired_always_virtual || app_virtual_display) {
      return "host_virtual_display";
    }
    if (!requested_topology.empty()) {
      return requested_topology;
    }
    return "desktop_display";
  }

  resolution_t resolve(const request_t &request) {
    resolution_t result;
    result.preset = normalize_preset(request.preset);
    result.hdr = request.hdr_requested;
    result.color_range = request.color_range;

    const auto device = device_db::get_device(request.device_name);
    const bool requested_display_complete = request.requested_width > 0 &&
      request.requested_height > 0 && request.requested_fps > 0;
    const bool paired_display_complete = request.paired_width > 0 &&
      request.paired_height > 0 && request.paired_fps > 0;
    field_provenance_t width_provenance;
    field_provenance_t height_provenance;
    field_provenance_t fps_provenance;
    const auto set_display_provenance = [&](const std::string &source,
                                            const std::string &reason,
                                            bool locked) {
      width_provenance = {source, reason, locked, false};
      height_provenance = width_provenance;
      fps_provenance = width_provenance;
    };

    if (request.display_locked && requested_display_complete) {
      result.width = request.requested_width;
      result.height = request.requested_height;
      result.fps = request.requested_fps;
      set_display_provenance("explicit_launch_request", "requested_display_lock", true);
    } else if (paired_display_complete) {
      result.width = request.paired_width;
      result.height = request.paired_height;
      result.fps = request.paired_fps;
      set_display_provenance("paired_client", "paired_display_setting", false);
    } else if (requested_display_complete) {
      result.width = request.requested_width;
      result.height = request.requested_height;
      result.fps = request.requested_fps;
      set_display_provenance("client_launch_request", "requested_display_setting", false);
    }

    if (result.preset == "high_fps" && request.requested_fps > 0 &&
        !request.display_locked) {
      // High FPS is a cadence lock, not a whole-display lock. Paired width and
      // height still win at their precedence layer, while the launch request's
      // FPS remains exact.
      result.fps = request.requested_fps;
      fps_provenance = {
        "client_launch_request",
        "high_fps_cadence_lock",
        true,
        false,
      };
    }

    if (result.preset == "stability" && !request.display_locked) {
      const auto stable = conservative_mode(
        request, device, {result.width, result.height, result.fps}
      );
      const bool width_normalized = stable.width != result.width;
      const bool height_normalized = stable.height != result.height;
      const bool fps_normalized = stable.fps != result.fps;
      result.width = stable.width;
      result.height = stable.height;
      result.fps = stable.fps;
      width_provenance = {"device_profile_v1", "stability_preset_selected", false, width_normalized};
      height_provenance = {"device_profile_v1", "stability_preset_selected", false, height_normalized};
      fps_provenance = {"device_profile_v1", "stability_preset_selected", false, fps_normalized};
    }

    if (result.fps > 0) {
      std::optional<int> effective_fps_cap;
      std::string fps_cap_reason;
      if (request.client_max_fps && *request.client_max_fps > 0) {
        effective_fps_cap = *request.client_max_fps;
        fps_cap_reason = "client_refresh_cap";
      }
      if (request.host_max_fps && *request.host_max_fps > 0 &&
          (!effective_fps_cap || *request.host_max_fps < *effective_fps_cap)) {
        effective_fps_cap = *request.host_max_fps;
        fps_cap_reason = "host_refresh_cap";
      }
      if (effective_fps_cap && result.fps > *effective_fps_cap) {
        result.fps = *effective_fps_cap;
        fps_provenance = {
          "capability_validation",
          fps_cap_reason,
          fps_provenance.locked,
          true,
        };
      }
    }

    if (result.width > 0 && result.height > 0 && result.fps > 0) {
      add_field(result.fields, "display_width", result.width,
                width_provenance.source, width_provenance.reason_code,
                width_provenance.locked, width_provenance.normalized);
      add_field(result.fields, "display_height", result.height,
                height_provenance.source, height_provenance.reason_code,
                height_provenance.locked, height_provenance.normalized);
      add_field(result.fields, "target_fps", static_cast<double>(result.fps) / 1000.0,
                fps_provenance.source, fps_provenance.reason_code,
                fps_provenance.locked, fps_provenance.normalized);

      const bool uniform_provenance =
        width_provenance.source == height_provenance.source &&
        width_provenance.source == fps_provenance.source &&
        width_provenance.reason_code == height_provenance.reason_code &&
        width_provenance.reason_code == fps_provenance.reason_code &&
        width_provenance.locked == height_provenance.locked &&
        width_provenance.locked == fps_provenance.locked;
      add_field(
        result.fields,
        "display_mode",
        format_display_mode({result.width, result.height, result.fps}),
        uniform_provenance ? width_provenance.source : "composed_display_components",
        uniform_provenance ? width_provenance.reason_code : "mixed_display_provenance",
        uniform_provenance && width_provenance.locked,
        width_provenance.normalized || height_provenance.normalized || fps_provenance.normalized
      );
    }

    if (request.bitrate_locked && request.explicit_bitrate_kbps &&
        *request.explicit_bitrate_kbps > 0) {
      result.target_bitrate_kbps = request.explicit_bitrate_kbps;
      add_field(result.fields, "target_bitrate_kbps", *result.target_bitrate_kbps,
                "explicit_launch_request", "requested_bitrate_lock", true);
    } else if (request.paired_bitrate_kbps && *request.paired_bitrate_kbps > 0) {
      result.target_bitrate_kbps = request.paired_bitrate_kbps;
      add_field(result.fields, "target_bitrate_kbps", *result.target_bitrate_kbps,
                "paired_client", "paired_bitrate_setting", false);
    } else if (request.explicit_bitrate_kbps && *request.explicit_bitrate_kbps > 0) {
      result.target_bitrate_kbps = request.explicit_bitrate_kbps;
      add_field(result.fields, "target_bitrate_kbps", *result.target_bitrate_kbps,
                "client_launch_request", "requested_bitrate_setting", false);
    } else if (result.preset != "auto" && device && device->ideal_bitrate_kbps > 0) {
      const int target = result.preset == "stability" ?
        std::min(device->ideal_bitrate_kbps, 15000) : device->ideal_bitrate_kbps;
      result.target_bitrate_kbps = target;
      add_field(result.fields, "target_bitrate_kbps", target,
                "device_profile_v1",
                result.preset == "stability" ? "stability_preset_selected" : "preset_device_capability",
                false,
                result.preset == "stability" && target != device->ideal_bitrate_kbps);
    }
    if (result.preset == "stability" && !request.bitrate_locked && result.target_bitrate_kbps) {
      const int conservative_target = std::min(
        *result.target_bitrate_kbps,
        device && device->ideal_bitrate_kbps > 0 ?
          std::min(device->ideal_bitrate_kbps, 15000) : 15000
      );
      const bool normalized = conservative_target != *result.target_bitrate_kbps;
      result.target_bitrate_kbps = conservative_target;
      add_field(result.fields, "target_bitrate_kbps", conservative_target,
                "device_profile_v1", "stability_preset_selected", false, normalized);
    }
    if (result.target_bitrate_kbps && request.configured_bitrate_kbps &&
        *request.configured_bitrate_kbps > 0 &&
        *result.target_bitrate_kbps > *request.configured_bitrate_kbps) {
      result.target_bitrate_kbps = request.configured_bitrate_kbps;
      add_field(result.fields, "target_bitrate_kbps", *result.target_bitrate_kbps,
                "capability_validation", "host_bitrate_cap", request.bitrate_locked, true);
    }

    if (result.preset != "auto" && device) {
      // Codec remains client-selected until the resolver receives an exact
      // decoder-capability set. An advisory device preference is not an
      // executable resolved value and therefore is deliberately omitted.
      result.nvenc_tune = device->nvenc_tune;
      add_field(result.fields, "nvenc_tune", *result.nvenc_tune,
                "device_profile_v1", "preset_encoder_tuning", false);
    }

    const bool hdr_from_explicit_lock = request.hdr_locked;
    const bool hdr_from_client_profile = !hdr_from_explicit_lock && request.client_profile_hdr.has_value();
    if (hdr_from_client_profile) {
      result.hdr = *request.client_profile_hdr;
    }
    const bool resolved_hdr_locked = hdr_from_explicit_lock || hdr_from_client_profile;
    if (result.hdr && request.host_hdr_capable == false) {
      result.hdr = false;
      add_field(result.fields, "hdr", false, "capability_validation",
                "host_encoder_hdr_unsupported", resolved_hdr_locked, true);
    } else if (result.hdr && device && !device->hdr_capable) {
      result.hdr = false;
      add_field(result.fields, "hdr", false, "capability_validation",
                "paired_device_hdr_unsupported", resolved_hdr_locked, true);
    } else if (hdr_from_explicit_lock) {
      add_field(result.fields, "hdr", result.hdr,
                "explicit_launch_request", "requested_hdr_lock", true);
    } else if (hdr_from_client_profile) {
      add_field(result.fields, "hdr", result.hdr,
                "client_profile", "client_profile_hdr_lock", true);
    } else {
      add_field(result.fields, "hdr", result.hdr,
                "client_launch_request", "requested_hdr_setting", false);
    }

    if (result.color_range) {
      add_field(result.fields, "color_range", *result.color_range,
                "client_profile", "client_profile_color_range", true);
    }

    return result;
  }

}  // namespace launch_profile
