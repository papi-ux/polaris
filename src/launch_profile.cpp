#include "launch_profile.h"

#include "device_db.h"
#include "utility.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <sstream>

namespace launch_profile {
  namespace {
    std::string shown_field_value(std::string_view value) {
      std::string shown;
      for (const unsigned char ch : value.substr(0, 32)) {
        shown.push_back((ch < 0x20 || ch == 0x7f) ? '?' : static_cast<char>(ch));
      }
      if (value.size() > 32) {
        shown += "...";
      }
      return shown;
    }

    std::string lowercase_copy(std::string_view value) {
      std::string lowered(value);
      std::transform(lowered.begin(), lowered.end(), lowered.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
      });
      return lowered;
    }
  }  // namespace

  explicit_launch_fields_t parse_explicit_launch_fields(const explicit_field_lookup_t &lookup) {
    explicit_launch_fields_t fields;
    const auto problem = [&](std::string_view field, std::string reason) {
      fields.problems.push_back({std::string(field), std::move(reason)});
    };
    const auto bounded_integer = [&](std::string_view name, long long minimum, long long maximum) -> std::optional<int> {
      const auto raw = lookup(name);
      if (!raw) {
        return std::nullopt;
      }
      const auto reject = [&]() {
        problem(name, std::string(name) + " must be a whole number between " + std::to_string(minimum) + " and " +
                        std::to_string(maximum) + "; got '" + shown_field_value(*raw) + "'");
        return std::optional<int> {};
      };
      try {
        std::size_t consumed = 0;
        const auto value = std::stoll(*raw, &consumed);
        if (consumed != raw->size() || value < minimum || value > maximum) {
          return reject();
        }
        return static_cast<int>(value);
      } catch (...) {
        return reject();
      }
    };
    const auto bounded_fps = [&](std::string_view name, int minimum, int maximum) -> std::optional<int> {
      const auto raw = lookup(name);
      if (!raw) {
        return std::nullopt;
      }
      const auto fps = util::parse_decimal<double>(*raw);
      if (!fps || *fps < minimum || *fps > maximum) {
        problem(name, std::string(name) + " must be a number between " + std::to_string(minimum) + " and " +
                        std::to_string(maximum) + " with a dot as the decimal separator; got '" + shown_field_value(*raw) + "'");
        return std::nullopt;
      }
      return static_cast<int>(std::round(*fps * 1000.0));
    };
    const auto exact_flag = [&](std::string_view name) -> bool {
      const auto raw = lookup(name);
      if (!raw) {
        return false;
      }
      const auto lowered = lowercase_copy(*raw);
      if (lowered == "1" || lowered == "true") {
        return true;
      }
      if (lowered == "0" || lowered == "false") {
        return false;
      }
      problem(name, std::string(name) + " must be 1 or 0; got '" + shown_field_value(*raw) + "'");
      return false;
    };

    fields.width = bounded_integer("width", 320, 16384);
    fields.height = bounded_integer("height", 240, 16384);
    fields.bitrate_kbps = bounded_integer("bitrate_kbps", 1000, 300000);
    fields.fps_millihertz = bounded_fps("fps", 15, 240);
    fields.client_max_fps_millihertz = bounded_fps("client_max_fps", 15, 360);
    fields.display_locked = exact_flag("display_locked");
    fields.bitrate_locked = exact_flag("bitrate_locked");
    fields.topology_locked = exact_flag("topology_locked");
    if (const auto raw = lookup("hdr")) {
      const auto lowered = lowercase_copy(*raw);
      if (lowered == "1" || lowered == "true" || lowered == "on" || lowered == "yes") {
        fields.hdr = true;
      } else if (lowered == "0" || lowered == "false" || lowered == "off" || lowered == "no") {
        fields.hdr = false;
      } else {
        problem("hdr", "hdr must be 1 or 0; got '" + shown_field_value(*raw) + "'");
      }
    }

    const auto mode_field_already_failed = std::any_of(fields.problems.begin(), fields.problems.end(), [](const auto &entry) {
      return entry.field == "width" || entry.field == "height" || entry.field == "fps";
    });
    if (fields.any_explicit_mode() && !fields.complete_explicit_mode() && !mode_field_already_failed) {
      std::string missing;
      for (const auto &[name, present] : {std::pair {"width", fields.width.has_value()},
                                          std::pair {"height", fields.height.has_value()},
                                          std::pair {"fps", fields.fps_millihertz.has_value()}}) {
        if (!present) {
          missing += (missing.empty() ? "" : ", ") + std::string(name);
        }
      }
      problem("width/height/fps", "width, height, and fps must be sent together; missing " + missing);
    }
    if (fields.display_locked && !fields.complete_explicit_mode()) {
      problem("display_locked", "display_locked requires width, height, and fps");
    }
    if (fields.bitrate_locked && !fields.bitrate_kbps) {
      problem("bitrate_locked", "bitrate_locked requires bitrate_kbps");
    }
    return fields;
  }

  std::string describe_explicit_launch_rejection(const explicit_launch_fields_t &fields) {
    if (fields.problems.empty()) {
      return "Explicit launch fields must be complete and within supported bounds.";
    }
    std::string text = "Explicit launch fields were rejected: ";
    for (std::size_t index = 0; index < fields.problems.size(); ++index) {
      if (index > 0) {
        text += "; ";
      }
      text += fields.problems[index].reason;
    }
    text += ".";
    return text;
  }

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
            const auto fps = util::parse_decimal<double>(segment);
            if (!fps) {
              return std::nullopt;
            }
            result.fps = static_cast<int>(std::round(*fps < 1000.0 ? *fps * 1000.0 : *fps));
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
      return util::format_decimal(static_cast<double>(fps) / 1000.0);
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

  non_linux_topology_resolution_t resolve_non_linux_topology(
      const std::string &requested_topology,
      bool topology_locked,
      bool paired_always_virtual,
      bool app_virtual_display,
      bool virtual_display_supported,
      bool host_requires_virtual_display) {
    non_linux_topology_resolution_t result;
    const auto finish = [&](std::string topology,
                            bool launch_owned,
                            std::string source,
                            std::string reason_code) {
      result.topology = std::move(topology);
      result.launch_owns_refresh_rate = launch_owned;
      result.source = std::move(source);
      result.reason_code = std::move(reason_code);
      result.normalized = !requested_topology.empty() &&
        result.topology != requested_topology;
      return result;
    };

    // macOS and other desktop-only builds do not consume streamMode or create
    // a launch-owned display. Normalize every unsupported/private request to
    // the physical desktop so /optimize and final launch validation agree.
    if (!virtual_display_supported) {
      if (topology_locked &&
          (requested_topology.empty() || requested_topology == "desktop_display")) {
        return finish(
          "desktop_display", false,
          "client_launch_request", requested_topology.empty() ?
            "explicit_desktop_lock" : "explicit_topology_lock"
        );
      }
      return finish(
        "desktop_display", false,
        "host_capability", requested_topology.empty() ?
          "platform_default_desktop" : "unsupported_topology_normalized"
      );
    }

    // Windows may need to create a virtual display when no probeable physical
    // output exists. This hard host semantic wins before launch preferences.
    if (host_requires_virtual_display) {
      return finish(
        "host_virtual_display", true,
        "host_capability", "host_requires_virtual_display"
      );
    }

    // Nova's legacy-compatible wire uses a blank streamMode plus
    // displayModeExplicit=1/virtualDisplay=0 for an explicit desktop choice.
    // Preserve that lock before paired or app virtual-display defaults.
    if (topology_locked && requested_topology.empty()) {
      return finish(
        "desktop_display", false,
        "client_launch_request", "explicit_desktop_lock"
      );
    }

    if (topology_locked && !requested_topology.empty()) {
      if (requested_topology == "host_virtual_display") {
        return finish(
          "host_virtual_display", true,
          "client_launch_request", "explicit_topology_lock"
        );
      }
      if (requested_topology == "desktop_display") {
        return finish(
          "desktop_display", false,
          "client_launch_request", "explicit_topology_lock"
        );
      }
      return finish(
        "desktop_display", false,
        "host_capability", "unsupported_topology_normalized"
      );
    }
    if (paired_always_virtual || app_virtual_display) {
      return finish(
        "host_virtual_display", true,
        paired_always_virtual ? "paired_client_settings" : "app_configuration",
        paired_always_virtual ?
          "paired_always_virtual_display" : "app_virtual_display_default"
      );
    }
    if (!requested_topology.empty()) {
      if (requested_topology == "host_virtual_display") {
        return finish(
          "host_virtual_display", true,
          "client_launch_request", "unlocked_topology_request"
        );
      }
      if (requested_topology == "desktop_display") {
        return finish(
          "desktop_display", false,
          "client_launch_request", "unlocked_topology_request"
        );
      }
      return finish(
        "desktop_display", false,
        "host_capability", "unsupported_topology_normalized"
      );
    }
    return finish(
      "desktop_display", false,
      "host_configuration", "host_default_topology"
    );
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
