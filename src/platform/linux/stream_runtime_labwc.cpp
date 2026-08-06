/**
 * @file src/platform/linux/stream_runtime_labwc.cpp
 * @brief labwc stream_runtime adapter over cage_display_router.
 *
 * Single forward pattern: stream_runtime::labwc free funcs are the only cage
 * callers for lifecycle + probes. labwc_runtime_t methods forward to those free
 * funcs (wrap_cmd is virtual-only and still hits cage directly).
 */

#include "stream_runtime.h"

#ifdef __linux__

  #include "cage_display_router.h"

  #include <optional>
  #include <string>

namespace stream_runtime {

  // Provided by stream_runtime_gamescope.cpp
  stream_runtime_t *gamescope_runtime_instance();

  namespace labwc {
    bool start(
      int width,
      int height,
      int refresh_hz,
      const std::string &game_cmd,
      bool force_windowed,
      bool allow_mangohud,
      const std::string &session_instance_id
    ) {
      return cage_display_router::start(
        width,
        height,
        refresh_hz,
        game_cmd,
        force_windowed,
        allow_mangohud,
        session_instance_id
      );
    }

    bool is_running() {
      return cage_display_router::is_running();
    }

    bool is_healthy() {
      return cage_display_router::is_healthy();
    }

    pid_t pid() {
      return cage_display_router::get_pid();
    }

    void stop() {
      cage_display_router::stop();
    }

    void reset_after_external_stop() {
      cage_display_router::reset_after_external_stop();
    }

    std::string wayland_socket() {
      return cage_display_router::get_wayland_socket();
    }

    std::string x11_display() {
      return cage_display_router::get_x11_display();
    }

    platf::runtime_state_t runtime_state() {
      return cage_display_router::runtime_state();
    }

    // Trivial AND/negation helpers live here only (not dual-exported via cage).
    bool should_attempt_windowed_gpu_native_probe(
      bool requested_headless,
      bool prefer_gpu_native_capture,
      bool encoder_requires_gpu_native_capture
    ) {
      return requested_headless && prefer_gpu_native_capture && encoder_requires_gpu_native_capture;
    }

    bool gpu_native_dmabuf_is_safe(platf::mem_type_e hwdevice_type) {
      return cage_display_router::gpu_native_dmabuf_is_safe(hwdevice_type);
    }

    bool should_attempt_gpu_native_cage_capture(
      const platf::runtime_state_t &runtime_state,
      platf::mem_type_e hwdevice_type
    ) {
      return cage_display_router::should_attempt_gpu_native_cage_capture(runtime_state, hwdevice_type);
    }

    bool should_attempt_headless_extcopy_dmabuf(
      const platf::runtime_state_t &runtime_state,
      platf::mem_type_e hwdevice_type
    ) {
      return cage_display_router::should_attempt_headless_extcopy_dmabuf(runtime_state, hwdevice_type);
    }

    bool should_disable_headless_extcopy_after_conversion_failure(
      const platf::runtime_state_t &runtime_state,
      const platf::frame_metadata_t &source_metadata
    ) {
      return cage_display_router::should_disable_headless_extcopy_after_conversion_failure(
        runtime_state,
        source_metadata
      );
    }

    std::optional<bool> cached_windowed_gpu_native_probe_result() {
      return cage_display_router::cached_windowed_gpu_native_probe_result();
    }

    std::optional<bool> cached_headless_extcopy_dmabuf_probe_result() {
      return cage_display_router::cached_headless_extcopy_dmabuf_probe_result();
    }

    void update_windowed_gpu_native_probe_result(bool supported) {
      cage_display_router::update_windowed_gpu_native_probe_result(supported);
    }

    void update_headless_extcopy_dmabuf_probe_result(bool supported) {
      cage_display_router::update_headless_extcopy_dmabuf_probe_result(supported);
    }

    bool headless_extcopy_dmabuf_probe_succeeded(bool capture_initialized, bool live_gpu_frame_converted) {
      return capture_initialized && live_gpu_frame_converted;
    }

    bool should_report_headless_ram_capture_fallback(const platf::runtime_state_t &runtime_state) {
      return cage_display_router::should_report_headless_ram_capture_fallback(runtime_state);
    }

    bool should_report_windowed_ram_capture_fallback(const platf::runtime_state_t &runtime_state) {
      return !should_report_headless_ram_capture_fallback(runtime_state);
    }

    bool should_log_headless_ram_capture_warning() {
      return cage_display_router::should_log_headless_ram_capture_warning();
    }

    bool should_log_windowed_ram_capture_warning() {
      return cage_display_router::should_log_windowed_ram_capture_warning();
    }
  }  // namespace labwc

  namespace {

    class labwc_runtime_t: public stream_runtime_t {
    public:
      std::string_view backend_id() const override {
        return stream_display_policy::k_runtime_labwc;
      }

      bool start(const start_params_t &params) override {
        return labwc::start(
          params.width,
          params.height,
          params.refresh_hz,
          params.game_cmd,
          params.force_windowed,
          params.allow_mangohud,
          params.session_instance_id
        );
      }

      void stop() override {
        labwc::stop();
      }

      void reset_after_external_stop() override {
        labwc::reset_after_external_stop();
      }

      bool is_running() const override {
        return labwc::is_running();
      }

      bool is_healthy() const override {
        return labwc::is_healthy();
      }

      pid_t pid() const override {
        return labwc::pid();
      }

      // Virtual-only: process spawn uses sanitize + apply_*_child_env, not wrap_cmd for labwc.
      std::string wrap_cmd(const std::string &cmd) const override {
        return cage_display_router::wrap_cmd(cmd);
      }

      std::string wayland_socket() const override {
        return labwc::wayland_socket();
      }

      std::string x11_display() const override {
        return labwc::x11_display();
      }

      platf::runtime_state_t runtime_state() const override {
        return labwc::runtime_state();
      }
    };

    // Process-lifetime singleton (not shared ownership).
    labwc_runtime_t g_labwc_runtime;

  }  // namespace

  stream_runtime_t *acquire(stream_path::runtime_kind_e runtime) {
    switch (runtime) {
      case stream_path::runtime_kind_e::LABWC:
        return &g_labwc_runtime;
      case stream_path::runtime_kind_e::GAMESCOPE:
        return gamescope_runtime_instance();
      case stream_path::runtime_kind_e::NONE:
      default:
        return nullptr;
    }
  }

  stream_runtime_t *acquire_for_current_policy(
    bool active_encoder_requires_gpu_native_capture,
    bool runtime_gpu_native_override_active
  ) {
    const auto resolved = stream_display_policy::resolve_current(
      active_encoder_requires_gpu_native_capture,
      runtime_gpu_native_override_active
    );
    if (!resolved.use_private_runtime) {
      return nullptr;
    }
    return acquire(resolved.runtime);
  }

}  // namespace stream_runtime

#endif  // __linux__
