/**
 * @file src/platform/linux/stream_display_policy.h
 * @brief Resolves the effective Linux stream display runtime.
 */
#pragma once

#include <string>

namespace stream_display_policy {

  enum class mode_e {
    DESKTOP,
    HEADLESS,
    HOST_VIRTUAL_DISPLAY,
    GPU_NATIVE_TEST,
  };

  struct input_t {
    bool virtual_display_available = false;
    bool active_encoder_requires_gpu_native_capture = false;
    bool runtime_gpu_native_override_active = false;
  };

  struct resolved_t {
    mode_e mode = mode_e::DESKTOP;
    std::string selection;
    std::string label;
    std::string reason;
    bool requested_headless = false;
    bool effective_headless = false;
    bool use_cage_runtime = false;
    bool use_host_virtual_display = false;
    bool should_defer_encoder_probe = false;
    bool should_probe_against_runtime = false;
  };

  resolved_t resolve(const input_t &input);
  resolved_t resolve_current(bool active_encoder_requires_gpu_native_capture = false,
                             bool runtime_gpu_native_override_active = false);

}  // namespace stream_display_policy
