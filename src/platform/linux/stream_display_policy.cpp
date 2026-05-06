/**
 * @file src/platform/linux/stream_display_policy.cpp
 * @brief Linux stream display policy resolver.
 */

#include "stream_display_policy.h"

#include "src/config.h"
#include "virtual_display.h"

namespace stream_display_policy {

  namespace {
    resolved_t desktop_policy() {
      return {
        mode_e::DESKTOP,
        "desktop_display",
        "Desktop Display",
        "Polaris is streaming from the current desktop session.",
        false,
        false,
        false,
        false,
        false,
        false,
      };
    }
  }  // namespace

  resolved_t resolve(const input_t &input) {
    const auto &linux_display = config::video.linux_display;
    const bool requested_headless = linux_display.headless_mode;
    const bool use_cage_runtime = linux_display.use_cage_compositor;

    if (!requested_headless) {
      return desktop_policy();
    }

    resolved_t result;
    result.requested_headless = true;
    result.use_cage_runtime = use_cage_runtime;
    result.should_defer_encoder_probe = use_cage_runtime;
    result.should_probe_against_runtime = use_cage_runtime;

    if (!use_cage_runtime) {
      result.mode = mode_e::HOST_VIRTUAL_DISPLAY;
      result.selection = "host_virtual_display";
      result.label = "Host Virtual Display";
      result.reason = input.virtual_display_available ?
        "Polaris will create or use a host virtual display for this stream." :
        "Polaris requested a host virtual display, but no backend is currently available.";
      result.effective_headless = false;
      result.use_host_virtual_display = true;
      result.should_defer_encoder_probe = false;
      result.should_probe_against_runtime = false;
      return result;
    }

    const bool gpu_native_test =
      linux_display.prefer_gpu_native_capture ||
      input.active_encoder_requires_gpu_native_capture ||
      input.runtime_gpu_native_override_active;

    if (gpu_native_test) {
      result.mode = mode_e::GPU_NATIVE_TEST;
      result.selection = "windowed_stream";
      result.label = "GPU-Native Test";
      result.reason = input.runtime_gpu_native_override_active ?
        "Polaris is running the private compositor windowed so capture can stay GPU-native." :
        "Polaris may override hidden headless at runtime if that is required for DMA-BUF/CUDA capture.";
      result.effective_headless = !input.runtime_gpu_native_override_active;
      return result;
    }

    result.mode = mode_e::HEADLESS;
    result.selection = "headless_stream";
    result.label = "Headless Stream";
    result.reason = "Polaris will stream from the private headless compositor runtime.";
    result.effective_headless = true;
    return result;
  }

  resolved_t resolve_current(bool active_encoder_requires_gpu_native_capture,
                             bool runtime_gpu_native_override_active) {
    return resolve(input_t {
      virtual_display::is_available(),
      active_encoder_requires_gpu_native_capture,
      runtime_gpu_native_override_active,
    });
  }

}  // namespace stream_display_policy
