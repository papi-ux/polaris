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
    std::string display_source;
    std::string display_reason;
    bool display_field_locked = false;

    if (request.display_locked && requested_display_complete) {
      result.width = request.requested_width;
      result.height = request.requested_height;
      result.fps = request.requested_fps;
      display_source = "explicit_launch_request";
      display_reason = "requested_display_lock";
      display_field_locked = true;
    } else if (paired_display_complete) {
      result.width = request.paired_width;
      result.height = request.paired_height;
      result.fps = request.paired_fps;
      display_source = "paired_client";
      display_reason = "paired_display_setting";
    } else if (requested_display_complete) {
      result.width = request.requested_width;
      result.height = request.requested_height;
      result.fps = request.requested_fps;
      display_source = "client_launch_request";
      display_reason = result.preset == "high_fps" ?
        "high_fps_cadence_lock" : "requested_display_setting";
    }

    if (result.preset == "high_fps" && request.requested_fps > 0 &&
        !request.display_locked) {
      // High FPS is a cadence lock, not a whole-display lock. Paired width and
      // height still win at their precedence layer, while the launch request's
      // FPS remains exact.
      result.fps = request.requested_fps;
      display_source = "client_launch_request";
      display_reason = "high_fps_cadence_lock";
    }

    if (result.width > 0 && result.height > 0 && result.fps > 0) {
      add_field(
        result.fields,
        "display_mode",
        format_display_mode({result.width, result.height, result.fps}),
        display_source,
        display_reason,
        display_field_locked
      );
      if (result.preset == "high_fps" && !display_field_locked) {
        result.fields["display_mode"]["locked_components"] = {"fps"};
      }
    }

    if (result.preset == "stability" && !request.display_locked) {
      const auto stable = conservative_mode(
        request, device, {result.width, result.height, result.fps}
      );
      const bool normalized = stable.width != result.width || stable.height != result.height || stable.fps != result.fps;
      result.width = stable.width;
      result.height = stable.height;
      result.fps = stable.fps;
      add_field(
        result.fields,
        "display_mode",
        format_display_mode(stable),
        "device_profile_v1",
        "stability_preset_selected",
        false,
        normalized
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

    if (request.hdr_requested && device && !device->hdr_capable) {
      result.hdr = false;
      add_field(result.fields, "hdr", false, "capability_validation",
                "paired_device_hdr_unsupported", request.hdr_locked, true);
    } else {
      add_field(result.fields, "hdr", result.hdr,
                request.hdr_locked ? "client_profile" : "explicit_launch_request",
                request.hdr_locked ? "client_profile_hdr_lock" : "requested_hdr_setting",
                request.hdr_locked);
    }

    if (result.color_range) {
      add_field(result.fields, "color_range", *result.color_range,
                "client_profile", "client_profile_color_range", true);
    }

    return result;
  }

}  // namespace launch_profile
