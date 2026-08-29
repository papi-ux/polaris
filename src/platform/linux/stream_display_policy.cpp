/**
 * @file src/platform/linux/stream_display_policy.cpp
 * @brief Linux stream display policy — facade over stream_path registry.
 */

#include "stream_display_policy.h"

#include "display_topology.h"
#include "stream_path.h"
#include "src/config.h"
#include "virtual_display.h"

#include <cctype>

namespace stream_display_policy {

  namespace {

    using stream_path::to_lower_copy;

    void normalize_host_virtual_display_state() {
      auto &linux_display = config::video.linux_display;
      linux_display.auto_manage_displays = false;
      config::video.capture = capture_for_host_virtual_display_backend(
        virtual_display::detect_backend(),
        config::video.capture
      );
    }

    bool selection_available_for_capabilities(
      std::string_view selection,
      bool virtual_display_available
    ) {
      if (const auto *path = stream_path::find(selection)) {
        if (!path->available) {
          return false;
        }
        if (path->id == stream_path::k_gamescope_stream) {
          return stream_path::probe_host_capabilities().gamescope_present;
        }
        if (path->id == stream_path::k_host_virtual_display) {
          return virtual_display_available;
        }
        return true;
      }
      return false;
    }

    std::string selection_unavailable_reason_for_capabilities(
      std::string_view selection,
      bool virtual_display_available
    ) {
      if (const auto *path = stream_path::find(selection)) {
        if (path->id == stream_path::k_gamescope_stream &&
            !stream_path::probe_host_capabilities().gamescope_present) {
          return "gamescope binary not found on PATH";
        }
        if (path->id == stream_path::k_host_virtual_display &&
            !virtual_display_available) {
          return "Host virtual display is not available on this host.";
        }
        if (!path->unavailable_reason.empty()) {
          return std::string {path->unavailable_reason};
        }
        return std::string {path->label} + " is not available on this host.";
      }
      return "Unknown stream display mode.";
    }

  }  // namespace

  std::string configured_selection() {
    auto &linux_display = config::video.linux_display;
    if (!linux_display.stream_mode.empty() && stream_path::find(linux_display.stream_mode)) {
      return stream_path::to_lower_copy(linux_display.stream_mode);
    }
    return selection_from_legacy_booleans({
      linux_display.headless_mode,
      linux_display.use_cage_compositor,
      linux_display.prefer_gpu_native_capture,
    });
  }

  bool selection_owns_launch_refresh_rate(std::string_view selection) {
    const auto key = stream_path::to_lower_copy(selection);
    return key == k_headless_stream ||
           key == k_windowed_stream ||
           key == k_host_virtual_display ||
           key == k_gamescope_stream;
  }

  std::string label_for_selection(std::string_view selection) {
    if (const auto *path = stream_path::find(selection)) {
      return std::string {path->label};
    }
    return {};
  }

  std::string reason_for_selection(std::string_view selection, bool virtual_display_available) {
    if (const auto *path = stream_path::find(selection)) {
      if (path->id == stream_path::k_host_virtual_display && !virtual_display_available) {
        return "Polaris requested a host virtual display, but no backend is currently available.";
      }
      return std::string {path->reason};
    }
    return "Polaris will mirror the current desktop session.";
  }

  bool selection_available(std::string_view selection) {
    const auto key = to_lower_copy(selection);
    return selection_available_for_capabilities(
      key,
      key != k_host_virtual_display || virtual_display::is_available()
    );
  }

  bool selection_session_overridable(std::string_view selection) {
    if (const auto *path = stream_path::find(selection)) {
      // Swapping the host's primary output rearranges the machine itself, so it
      // is a host decision rather than something one client turns on per launch.
      return path->topology != stream_path::topology_kind_e::SWAP_PRIMARY;
    }
    return false;
  }

  std::string selection_unavailable_reason(std::string_view selection) {
    const auto key = to_lower_copy(selection);
    const bool virtual_display_available =
      key != k_host_virtual_display || virtual_display::is_available();
    if (key == k_host_virtual_display && !virtual_display_available) {
      const auto backend_reason = virtual_display::unavailable_reason();
      if (!backend_reason.empty()) {
        return backend_reason;
      }
    }
    return selection_unavailable_reason_for_capabilities(
      key,
      virtual_display_available
    );
  }

  std::string selection_from_legacy_booleans(const legacy_booleans_t &booleans) {
    if (!booleans.headless_mode) {
      if (booleans.use_cage_compositor) {
        return std::string {k_windowed_stream};
      }
      return std::string {k_desktop_display};
    }
    if (!booleans.use_cage_compositor) {
      return std::string {k_host_virtual_display};
    }
    if (booleans.prefer_gpu_native_capture) {
      return std::string {k_windowed_stream};
    }
    return std::string {k_headless_stream};
  }

  legacy_booleans_t legacy_booleans_for_selection(std::string_view selection) {
    legacy_booleans_t booleans;
    const auto key = to_lower_copy(selection);
    if (key == k_headless_stream) {
      booleans.headless_mode = true;
      booleans.use_cage_compositor = true;
      booleans.prefer_gpu_native_capture = false;
    }
    else if (key == k_windowed_stream) {
      booleans.headless_mode = true;
      booleans.use_cage_compositor = true;
      booleans.prefer_gpu_native_capture = true;
    }
    else if (key == k_host_virtual_display ||
             key == stream_path::k_headless_dongle) {
      // Dongle: host desktop path with topology swap — not private labwc.
      booleans.headless_mode = true;
      booleans.use_cage_compositor = false;
      booleans.prefer_gpu_native_capture = false;
    }
    else if (key == k_gamescope_stream) {
      booleans.headless_mode = true;
      booleans.use_cage_compositor = false;
      booleans.prefer_gpu_native_capture = false;
    }
    else {
      booleans.headless_mode = false;
      booleans.use_cage_compositor = false;
      booleans.prefer_gpu_native_capture = false;
    }
    return booleans;
  }

  std::string effective_session_selection_for_launch(
    std::string_view requested_selection,
    bool mirror_desktop,
    bool launch_virtual_display,
    bool app_virtual_display,
    bool virtual_display_user_locked,
    bool virtual_display_optimization_present
  ) {
    if (mirror_desktop) {
      return std::string {k_desktop_display};
    }
    if (virtual_display_user_locked) {
      if (!requested_selection.empty()) {
        return std::string {requested_selection};
      }
      if (launch_virtual_display) {
        return std::string {k_host_virtual_display};
      }
    }
    if (!virtual_display_optimization_present &&
        app_virtual_display &&
        !virtual_display_user_locked) {
      return std::string {k_host_virtual_display};
    }
    if (!requested_selection.empty()) {
      return std::string {requested_selection};
    }
    if (launch_virtual_display) {
      return std::string {k_host_virtual_display};
    }
    return {};
  }

  std::string capture_output_name_for_virtual_display(
    std::string_view created_output_name,
    std::string_view mapped_output_name
  ) {
    return std::string {mapped_output_name.empty() ? created_output_name : mapped_output_name};
  }

  std::string capture_for_host_virtual_display_backend(
    virtual_display::backend_e backend,
    std::string_view current_capture
  ) {
    if (backend == virtual_display::backend_e::WAYLAND_WLR) {
      return "wlr";
    }

    if (backend == virtual_display::backend_e::EVDI ||
        backend == virtual_display::backend_e::KSCREEN_DOCTOR) {
      return "portal";
    }

    return std::string {current_capture};
  }

  resolved_t resolve(const input_t &input) {
    const auto selection = configured_selection();
    const auto *path = stream_path::find(selection);
    stream_path::descriptor_t desc {};
    if (path) {
      desc = *path;
    }
    else {
      desc = *stream_path::find(stream_path::k_desktop_display);
    }

    // Edge: legacy !headless + cage without stream_mode → windowed private labwc.
    if (config::video.linux_display.stream_mode.empty() &&
        !config::video.linux_display.headless_mode &&
        config::video.linux_display.use_cage_compositor) {
      if (const auto *windowed = stream_path::find(stream_path::k_windowed_stream)) {
        desc = *windowed;
        desc.request_headless = false;
      }
    }

    auto caps = stream_path::probe_host_capabilities();
    if (input.virtual_display_available) {
      caps.virtual_display_available = true;
    }

    return stream_path::resolve_path(
      desc,
      caps,
      input.active_encoder_requires_gpu_native_capture,
      input.runtime_gpu_native_override_active
    );
  }

  resolved_t resolve_current(bool active_encoder_requires_gpu_native_capture,
                             bool runtime_gpu_native_override_active) {
    return resolve(input_t {
      virtual_display::is_available(),
      active_encoder_requires_gpu_native_capture,
      runtime_gpu_native_override_active,
    });
  }

  resolved_t resolve_effective(const input_t &input,
                               bool streaming,
                               bool session_uses_virtual_display,
                               bool runtime_effective_headless) {
    if (!streaming) {
      return resolve(input);
    }

    auto configured = resolve(input);
    if (input.runtime_gpu_native_override_active) {
      configured.selection = std::string {k_windowed_stream};
      configured.label = label_for_selection(k_windowed_stream);
      configured.effective_headless = false;
      configured.prefer_gpu_native_capture = true;
      configured.backend_name = "labwc";
      return configured;
    }
    if (session_uses_virtual_display) {
      if (const auto *path = stream_path::find(stream_path::k_host_virtual_display)) {
        auto caps = stream_path::probe_host_capabilities();
        caps.virtual_display_available = input.virtual_display_available;
        return stream_path::resolve_path(*path, caps);
      }
    }
    if (configured.selection == k_windowed_stream && runtime_effective_headless) {
      return configured;
    }
    if (runtime_effective_headless && configured.uses_labwc()) {
      configured.selection = std::string {k_headless_stream};
      configured.label = label_for_selection(k_headless_stream);
      configured.effective_headless = true;
      return configured;
    }
    return configured;
  }

  bool selection_valid_for_capabilities(
    std::string_view selection,
    bool virtual_display_available,
    std::string &error
  ) {
    const auto key = to_lower_copy(selection);
    if (!stream_path::find(key) && key != k_desktop_display) {
      error = "stream_display_mode must be a known stream path id (see /client-settings modes)";
      return false;
    }
    if (!selection_available_for_capabilities(key, virtual_display_available)) {
      error = selection_unavailable_reason_for_capabilities(
        key,
        virtual_display_available
      );
      return false;
    }
    return true;
  }

  bool selection_valid(std::string_view selection, std::string &error) {
    const auto key = to_lower_copy(selection);
    return selection_valid_for_capabilities(
      key,
      key != k_host_virtual_display || virtual_display::is_available(),
      error
    );
  }

  bool selection_companion_state_matches(std::string_view selection) {
    const auto key = to_lower_copy(selection);
    const auto &linux_display = config::video.linux_display;
    if (linux_display.stream_mode != key) {
      return false;
    }

    const auto expected = legacy_booleans_for_selection(key);
    if (linux_display.headless_mode != expected.headless_mode ||
        linux_display.use_cage_compositor != expected.use_cage_compositor ||
        linux_display.prefer_gpu_native_capture != expected.prefer_gpu_native_capture) {
      return false;
    }

    if (const auto *path = stream_path::find(key)) {
      auto expected_runtime = std::string {stream_path::runtime_kind_id(path->runtime)};
      if (expected_runtime.empty() && expected.use_cage_compositor) {
        expected_runtime = std::string {k_runtime_labwc};
      }
      if (linux_display.private_runtime != expected_runtime) {
        return false;
      }

      if (key == k_host_virtual_display && linux_display.auto_manage_displays) {
        return false;
      }
      if (key == k_host_virtual_display &&
          config::video.capture != capture_for_host_virtual_display_backend(
                                     virtual_display::detect_backend(),
                                     config::video.capture
                                   )) {
        return false;
      }
      if (key == stream_path::k_gamescope_stream && config::video.capture.empty()) {
        return false;
      }
      if (key == stream_path::k_headless_dongle) {
        return linux_display.auto_manage_displays &&
               !linux_display.headless_swap_mode.empty() &&
               !linux_display.streaming_output.empty() &&
               !linux_display.primary_output.empty() &&
               linux_display.streaming_output != linux_display.primary_output &&
               config::video.capture != "auto" &&
               !config::video.capture.empty() &&
               !config::video.output_name.empty();
      }
      return true;
    }

    return key == k_desktop_display;
  }

  bool apply_selection(std::string_view selection, std::string &error) {
    if (!selection_valid(selection, error)) {
      return false;
    }
    const auto key = to_lower_copy(selection);

    auto &linux_display = config::video.linux_display;

    if (key == k_host_virtual_display) {
      normalize_host_virtual_display_state();
    }

    if (key == stream_path::k_gamescope_stream) {
      auto caps = stream_path::probe_host_capabilities();
      if (!caps.gamescope_present) {
        error = "gamescope_stream requires the gamescope binary on PATH";
        return false;
      }
      // Prefer portal capture of gamescope; leave idle unit free to attach.
      if (config::video.capture.empty()) {
        config::video.capture = "portal";
      }
    }

    // Dongle path: auto-detect connectors from DRM if unset (sysfs), then validate.
    if (key == stream_path::k_headless_dongle) {
      if (!display_topology::ensure_dongle_outputs_configured()) {
        error = "headless_dongle: could not auto-detect distinct dongle and panel outputs; set linux_streaming_output and linux_primary_output (see kscreen-doctor -o / sysfs drm)";
        return false;
      }
      if (linux_display.streaming_output == linux_display.primary_output) {
        error = "headless_dongle needs distinct streaming and primary outputs";
        return false;
      }
      linux_display.auto_manage_displays = true;
      if (linux_display.headless_swap_mode.empty()) {
        linux_display.headless_swap_mode = "privacy";
      }
      // Prefer portal on Wayland hosts: KMS needs CAP_SYS_ADMIN and returns an empty
      // monitor list without it (lea). Portal ScreenCast after topology prepare is the
      // working path once the portal lock-contract is fixed. Explicit capture=kms is kept.
      if (config::video.capture.empty() || config::video.capture == "auto") {
        config::video.capture = "portal";
      }
      if (config::video.output_name.empty()) {
        config::video.output_name = linux_display.streaming_output;
      }
    }

    const auto booleans = legacy_booleans_for_selection(key);
    linux_display.stream_mode = key;
    linux_display.headless_mode = booleans.headless_mode;
    linux_display.use_cage_compositor = booleans.use_cage_compositor;
    linux_display.prefer_gpu_native_capture = booleans.prefer_gpu_native_capture;

    if (const auto *path = stream_path::find(key)) {
      linux_display.private_runtime = std::string {stream_path::runtime_kind_id(path->runtime)};
      if (linux_display.private_runtime.empty() && booleans.use_cage_compositor) {
        linux_display.private_runtime = std::string {k_runtime_labwc};
      }
    }
    else if (booleans.use_cage_compositor) {
      linux_display.private_runtime = std::string {k_runtime_labwc};
    }

    return true;
  }

  void normalize_config_from_load() {
    auto &linux_display = config::video.linux_display;

    if (!linux_display.stream_mode.empty()) {
      if (const auto *path = stream_path::find(linux_display.stream_mode)) {
        const auto booleans = legacy_booleans_for_selection(path->id);
        linux_display.headless_mode = booleans.headless_mode;
        linux_display.use_cage_compositor = booleans.use_cage_compositor;
        linux_display.prefer_gpu_native_capture = booleans.prefer_gpu_native_capture;
        linux_display.private_runtime = std::string {
          stream_path::runtime_kind_id(path->runtime)
        };
        if (linux_display.private_runtime.empty() && booleans.use_cage_compositor) {
          linux_display.private_runtime = std::string {k_runtime_labwc};
        }
        if (path->id == k_host_virtual_display) {
          normalize_host_virtual_display_state();
        }
        // headless_dongle: default to portal (host desktop after topology swap).
        // Do not force KMS — without CAP_SYS_ADMIN encoder probe fails empty.
        if (path->id == stream_path::k_headless_dongle) {
          if (config::video.capture.empty() || config::video.capture == "auto") {
            config::video.capture = "portal";
          }
        }
        return;
      }
      linux_display.stream_mode.clear();
    }

    linux_display.stream_mode = selection_from_legacy_booleans({
      linux_display.headless_mode,
      linux_display.use_cage_compositor,
      linux_display.prefer_gpu_native_capture,
    });
    if (linux_display.stream_mode == k_host_virtual_display) {
      normalize_host_virtual_display_state();
    }

    if (const auto *path = stream_path::find(linux_display.stream_mode)) {
      linux_display.private_runtime = std::string {
        stream_path::runtime_kind_id(path->runtime)
      };
      if (linux_display.private_runtime.empty() && linux_display.use_cage_compositor) {
        linux_display.private_runtime = std::string {k_runtime_labwc};
      }
    }
  }

  std::vector<mode_option_t> mode_options(bool virtual_display_available) {
    auto caps = stream_path::probe_host_capabilities();
    caps.virtual_display_available = virtual_display_available;
    std::vector<mode_option_t> options;
    for (const auto &path : stream_path::options_for_host(caps)) {
      mode_option_t option;
      option.value = std::string {path.id};
      option.label = std::string {path.label};
      option.reason = reason_for_selection(path.id, virtual_display_available);
      option.available = path.available &&
        (path.id != stream_path::k_host_virtual_display || virtual_display_available);
      option.unavailable_reason = std::string {path.unavailable_reason};
      if (!option.available &&
          path.id == stream_path::k_host_virtual_display &&
          option.unavailable_reason.empty()) {
        option.unavailable_reason = "Host virtual display is not available on this host.";
      }
      option.group = std::string {path.group};
      option.runtime = std::string {stream_path::runtime_kind_id(path.runtime)};
      option.capture = std::string {stream_path::capture_kind_id(path.capture)};
      option.topology = std::string {stream_path::topology_kind_id(path.topology)};
      options.push_back(std::move(option));
    }
    return options;
  }

  std::vector<std::string> allowed_launch_modes(bool virtual_display_available,
                                                bool include_unavailable) {
    std::vector<std::string> modes;
    auto caps = stream_path::probe_host_capabilities();
    caps.virtual_display_available = virtual_display_available;
    // Use options_for_host so gamescope_present (and future host probes) match
    // mode_options / selection_available — no dual-truth availability.
    for (const auto &path : stream_path::options_for_host(caps)) {
      if (!path.available && !include_unavailable) {
        continue;
      }
      // Launch contract only lists primary user paths by default.
      if (path.group == "experimental" && !include_unavailable) {
        continue;
      }
      if (path.id == stream_path::k_host_virtual_display && !virtual_display_available) {
        continue;
      }
      // Dongle is always listable when available; apply auto-fills outputs.
      modes.emplace_back(path.id);
    }
    return modes;
  }

}  // namespace stream_display_policy
