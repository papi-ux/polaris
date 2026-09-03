/**
 * @file src/platform/linux/stream_path.cpp
 * @brief Stream path registry and host capability probes.
 */

#include "stream_path.h"

#include "desktop_takeover.h"
#include "src/config.h"
#include "virtual_display.h"

#include <cctype>
#include <cstdlib>
#include <unistd.h>

namespace stream_path {

  std::string to_lower_copy(std::string_view value) {
    std::string out;
    out.reserve(value.size());
    for (unsigned char ch : value) {
      out.push_back(static_cast<char>(std::tolower(ch)));
    }
    return out;
  }

  bool binary_on_path(const char *name) {
    if (!name || !*name) {
      return false;
    }
    // Lightweight PATH probe without pulling boost.process into every TU.
    const char *path_env = std::getenv("PATH");
    if (!path_env) {
      return false;
    }
    std::string path {path_env};
    std::size_t start = 0;
    while (start <= path.size()) {
      const auto end = path.find(':', start);
      const auto dir = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
      if (!dir.empty()) {
        const std::string candidate = dir + "/" + name;
        if (access(candidate.c_str(), X_OK) == 0) {
          return true;
        }
      }
      if (end == std::string::npos) {
        break;
      }
      start = end + 1;
    }
    return false;
  }

  std::vector<descriptor_t> registry() {
    // Keep IDs stable. Unwired paths stay out of the registry until integrated.
    return {
      {
        k_headless_stream,
        "Private Stream",
        "Recommended",
        "Apps run in a private labwc compositor without taking over the physical desktop.",
        runtime_kind_e::LABWC,
        capture_kind_e::WLROOTS,
        topology_kind_e::LEAVE_ALONE,
        false,
        true,
        true,
        {},
        "private",
      },
      {
        k_windowed_stream,
        "Private Stream (GPU-native)",
        "Advanced capture",
        "Private labwc session that prefers GPU-native capture, even if the compositor must run windowed.",
        runtime_kind_e::LABWC,
        capture_kind_e::WLROOTS,
        topology_kind_e::LEAVE_ALONE,
        true,
        true,
        true,
        {},
        "private",
      },
      {
        k_host_virtual_display,
        "Host Virtual Display",
        "Compatibility",
        "Create or use a host-visible virtual output (EVDI / wlr / kscreen).",
        runtime_kind_e::NONE,
        capture_kind_e::AUTO,
        topology_kind_e::HOST_VIRTUAL,
        false,
        true,
        true,
        {},
        "host",
      },
      {
        k_desktop_takeover,
        "Desktop Takeover",
        "Hyprland",
        "Move the live Hyprland session to a temporary virtual output and power off the physical monitors until disconnect.",
        runtime_kind_e::NONE,
        capture_kind_e::AUTO,
        topology_kind_e::DESKTOP_TAKEOVER,
        false,
        true,
        true,
        {},
        "host",
      },
      {
        k_desktop_display,
        "Mirror Desktop",
        "Advanced",
        "Stream the visible desktop session (or an external compositor such as gamescope via portal).",
        runtime_kind_e::NONE,
        capture_kind_e::PORTAL,
        topology_kind_e::LEAVE_ALONE,
        false,
        false,
        true,
        {},
        "host",
      },
      {
        k_gamescope_stream,
        "Gamescope Stream",
        "gamescope",
        "Private Gamescope session (attach to idle gamescope-0 or spawn owned headless). Capture via portal/PipeWire.",
        runtime_kind_e::GAMESCOPE,
        capture_kind_e::PORTAL,
        topology_kind_e::LEAVE_ALONE,
        false,
        true,
        true,  // available when gamescope is on PATH (checked at resolve/apply)
        {},
        "private",
      },
      {
        k_headless_dongle,
        "Headless Dongle",
        "Physical dummy",
        "Swap the desktop onto a physical dummy-plug connector. Privacy mode blanks the panel after one-time portal approval is saved; the approval session keeps it on. Off mode leaves it primary and extends onto the dongle. Capture via host portal ScreenCast (default). KMS remains optional for CAP_SYS_ADMIN hosts. Requires linux_streaming_output + linux_primary_output + auto_manage.",
        runtime_kind_e::NONE,
        capture_kind_e::PORTAL,
        topology_kind_e::SWAP_PRIMARY,
        false,
        true,
        true,
        {},
        "host",
      },
    };
  }

  const descriptor_t *find(std::string_view id) {
    // Pointer into process-lifetime static table (not thread_local scratch).
    // Safe for callers that hold the pointer across awaits on this thread.
    static const std::vector<descriptor_t> table = registry();
    const auto key = to_lower_copy(id);
    for (const auto &entry : table) {
      if (key == entry.id) {
        return &entry;
      }
    }
    return nullptr;
  }

  std::string_view runtime_kind_id(runtime_kind_e kind) {
    switch (kind) {
      case runtime_kind_e::LABWC:
        return k_runtime_labwc;
      case runtime_kind_e::GAMESCOPE:
        return k_runtime_gamescope;
      case runtime_kind_e::NONE:
      default:
        return {};
    }
  }

  std::string_view capture_kind_id(capture_kind_e kind) {
    switch (kind) {
      case capture_kind_e::WLROOTS:
        return "wlroots";
      case capture_kind_e::PORTAL:
        return "portal";
      case capture_kind_e::KMS:
        return "kms";
      case capture_kind_e::AUTO:
      default:
        return "auto";
    }
  }

  std::string_view topology_kind_id(topology_kind_e kind) {
    switch (kind) {
      case topology_kind_e::HOST_VIRTUAL:
        return "host_virtual";
      case topology_kind_e::DESKTOP_TAKEOVER:
        return "desktop_takeover";
      case topology_kind_e::SWAP_PRIMARY:
        return "swap_primary";
      case topology_kind_e::LEAVE_ALONE:
      default:
        return "leave_alone";
    }
  }

  host_capabilities_t probe_host_capabilities() {
    host_capabilities_t caps;
    caps.labwc_present = binary_on_path("labwc");
    caps.wlr_randr_present = binary_on_path("wlr-randr");
    caps.gamescope_present = binary_on_path("gamescope");
    caps.virtual_display_available = virtual_display::is_available();
    caps.desktop_takeover_available = desktop_takeover::is_available();
    // Portal availability is environment-dependent; configured capture is the honest hint.
    caps.configured_capture = config::video.capture;
    return caps;
  }

  bool labwc_runtime_available(const host_capabilities_t &caps) {
    return caps.labwc_present && caps.wlr_randr_present;
  }

  std::string labwc_runtime_unavailable_reason(const host_capabilities_t &caps) {
    if (!caps.labwc_present && !caps.wlr_randr_present) {
      return "labwc and wlr-randr binaries not found on PATH";
    }
    if (!caps.labwc_present) {
      return "labwc binary not found on PATH";
    }
    if (!caps.wlr_randr_present) {
      return "wlr-randr binary not found on PATH";
    }
    return {};
  }

  std::string backend_name_for_path(const descriptor_t &path, const host_capabilities_t &caps) {
    switch (path.runtime) {
      case runtime_kind_e::LABWC:
        return std::string {k_runtime_labwc};
      case runtime_kind_e::GAMESCOPE:
        return std::string {k_runtime_gamescope};
      case runtime_kind_e::NONE:
        break;
    }

    if (path.topology == topology_kind_e::HOST_VIRTUAL ||
        path.topology == topology_kind_e::DESKTOP_TAKEOVER) {
      return std::string {k_backend_virtual_display};
    }

    if (path.capture == capture_kind_e::PORTAL ||
        path.capture == capture_kind_e::AUTO) {
      const auto capture = to_lower_copy(caps.configured_capture);
      if (capture == "portal" || capture.find("portal") != std::string::npos) {
        // External gamescope + portal is still "portal" capture on the host path.
        if (caps.gamescope_present && !config::video.linux_display.use_cage_compositor) {
          // Prefer gamescope label when GAMESCOPE_WAYLAND_DISPLAY is set for attach stacks.
          if (const char *gs = std::getenv("GAMESCOPE_WAYLAND_DISPLAY"); gs && *gs) {
            return std::string {k_runtime_gamescope};
          }
        }
        return std::string {k_backend_portal};
      }
      if (capture == "kms" || capture == "drm") {
        return "kms";
      }
      if (!capture.empty()) {
        return capture;
      }
    }

    if (path.capture == capture_kind_e::KMS) {
      return "kms";
    }

    return std::string {k_backend_host};
  }

  resolved_t resolve_path(
    const descriptor_t &path,
    const host_capabilities_t &caps,
    bool active_encoder_requires_gpu_native,
    bool runtime_gpu_native_override_active
  ) {
    resolved_t out;
    out.selection = std::string {path.id};
    out.label = std::string {path.label};
    out.reason = std::string {path.reason};
    out.runtime = path.runtime;
    out.capture = path.capture;
    out.topology = path.topology;
    out.available = path.available;
    out.unavailable_reason = std::string {path.unavailable_reason};
    out.requested_headless = path.request_headless;
    out.prefer_gpu_native_capture =
      path.prefer_gpu_native ||
      active_encoder_requires_gpu_native ||
      runtime_gpu_native_override_active ||
      config::video.linux_display.prefer_gpu_native_capture;
    out.backend_name = backend_name_for_path(path, caps);

    if (path.runtime == runtime_kind_e::LABWC &&
        path.available &&
        !labwc_runtime_available(caps)) {
      out.available = false;
      out.unavailable_reason = labwc_runtime_unavailable_reason(caps);
      out.reason = "Private Stream is unavailable: " + out.unavailable_reason + ".";
    }
    if (path.topology == topology_kind_e::HOST_VIRTUAL && !caps.virtual_display_available) {
      out.reason = "Host virtual display was requested, but no backend is currently available.";
    }
    if (path.topology == topology_kind_e::DESKTOP_TAKEOVER &&
        !caps.desktop_takeover_available) {
      out.available = false;
      out.unavailable_reason = desktop_takeover::unavailable_reason();
      out.reason = "Desktop Takeover is unavailable: " + out.unavailable_reason;
    }

    switch (path.runtime) {
      case runtime_kind_e::LABWC:
        out.use_private_runtime = path.available;
        out.effective_headless = path.request_headless && !runtime_gpu_native_override_active;
        out.should_defer_encoder_probe = path.available;
        if (runtime_gpu_native_override_active) {
          out.effective_headless = false;
          out.label = "Private Stream (GPU-native)";
          out.reason = "Polaris is running the private compositor windowed so capture can stay GPU-native.";
        }
        else if (out.prefer_gpu_native_capture && path.id == k_headless_stream) {
          // Encoder/config promotes headless_stream toward GPU-native labeling.
          out.selection = std::string {k_windowed_stream};
          out.label = "Private Stream (GPU-native)";
          out.reason =
            "Polaris can force a windowed private compositor when hidden Private Stream capture cannot stay GPU-native.";
        }
        else if (!path.request_headless && path.id == k_windowed_stream) {
          out.label = "Private Stream (windowed)";
        }
        break;
      case runtime_kind_e::GAMESCOPE:
        out.use_private_runtime = path.available && caps.gamescope_present;
        out.effective_headless = path.request_headless;
        if (!caps.gamescope_present) {
          out.available = false;
          out.unavailable_reason = "gamescope binary not found on PATH";
          out.use_private_runtime = false;
        }
        break;
      case runtime_kind_e::NONE:
        out.use_private_runtime = false;
        out.use_host_virtual_display =
          path.topology == topology_kind_e::HOST_VIRTUAL ||
          path.topology == topology_kind_e::DESKTOP_TAKEOVER;
        out.effective_headless = false;
        out.requested_headless = path.topology == topology_kind_e::HOST_VIRTUAL ||
                                 path.topology == topology_kind_e::DESKTOP_TAKEOVER ||
                                 path.topology == topology_kind_e::SWAP_PRIMARY;
        break;
    }

    return out;
  }

  std::vector<descriptor_t> options_for_host(const host_capabilities_t &caps) {
    auto options = registry();
    for (auto &opt : options) {
      if (!opt.available) {
        continue;
      }
      // Private runtimes are selectable only when every executable their start
      // path requires is on PATH. Unavailable cards remain listed with guidance.
      if (opt.runtime == runtime_kind_e::LABWC && !labwc_runtime_available(caps)) {
        opt.available = false;
        opt.unavailable_reason = labwc_runtime_unavailable_reason(caps);
      }
      if (opt.runtime == runtime_kind_e::GAMESCOPE && !caps.gamescope_present) {
        opt.available = false;
        opt.unavailable_reason = "gamescope binary not found on PATH";
      }
      if (opt.topology == topology_kind_e::DESKTOP_TAKEOVER &&
          !caps.desktop_takeover_available) {
        opt.available = false;
        opt.unavailable_reason = desktop_takeover::unavailable_reason();
      }
    }
    return options;
  }

}  // namespace stream_path
