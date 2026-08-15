/**
 * @file src/platform/linux/stream_runtime.h
 * @brief Private compositor runtime (labwc + gamescope). See docs/research/linux-stream-modularity.md.
 */
#pragma once

#ifdef __linux__

  #include "src/platform/common.h"
  #include "stream_display_policy.h"
  #include "wlgrab_capture_policy.h"

  #include <optional>
  #include <string>
  #include <string_view>
  #include <unistd.h>

namespace stream_runtime {

  struct start_params_t {
    int width = 1920;
    int height = 1080;
    int refresh_hz = 60;
    std::string game_cmd;
    bool force_windowed = false;
    bool allow_mangohud = true;
    std::string session_instance_id;
    // The client's raw requested FPS (whole hertz or millihertz; 0 = unknown).
    // When refresh_hz runs below it the launch was deliberately clamped, and
    // resume refresh re-applies are held to that ceiling (issue #367).
    int requested_refresh_hz = 0;
  };

  class stream_runtime_t {
  public:
    virtual ~stream_runtime_t() = default;

    virtual std::string_view backend_id() const = 0;

    virtual bool start(const start_params_t &params) = 0;
    virtual void stop() = 0;
    virtual void reset_after_external_stop() = 0;

    virtual bool is_running() const = 0;
    virtual bool is_healthy() const = 0;
    virtual pid_t pid() const = 0;

    virtual std::string wrap_cmd(const std::string &cmd) const = 0;
    virtual std::string wayland_socket() const = 0;
    virtual std::string x11_display() const = 0;
    virtual platf::runtime_state_t runtime_state() const = 0;
  };

  /// Process-lifetime singleton pointer (or nullptr). Not shared ownership.
  stream_runtime_t *acquire(stream_path::runtime_kind_e runtime);

  stream_runtime_t *acquire_for_current_policy(
    bool active_encoder_requires_gpu_native_capture = false,
    bool runtime_gpu_native_override_active = false
  );

  bool drain_gamescope_private_group_for_tests(pid_t pgid);
  bool rollback_gamescope_spawn_for_tests(pid_t pgid, int leader_pidfd);
  bool gamescope_runtime_acquisition_allowed_for_tests();
  bool gamescope_runtime_closes_inherited_descriptors_for_tests();

  /**
   * Labwc-only free functions — only stream_runtime_labwc.cpp may include
   * cage_display_router. All other TUs call through this namespace.
   * Lifecycle free funcs are the sole cage bridge; labwc_runtime_t forwards here.
   */
  namespace labwc {
    bool start(
      int width = 1920,
      int height = 1080,
      int refresh_hz = 60,
      const std::string &game_cmd = "",
      bool force_windowed = false,
      bool allow_mangohud = true,
      const std::string &session_instance_id = "",
      int requested_refresh_hz = 0
    );
    bool is_running();
    bool is_healthy();
    pid_t pid();
    void stop();
    void reset_after_external_stop();
    std::string wayland_socket();
    std::string x11_display();
    platf::runtime_state_t runtime_state();

    /**
     * Re-apply the running cage's output refresh for a resuming session
     * (millihertz-aware); the launch-time resolution is kept. See
     * cage_display_router::ensure_output_refresh (issue #367).
     */
    bool ensure_output_refresh(int session_fps);

    bool should_attempt_windowed_gpu_native_probe(
      bool requested_headless,
      bool prefer_gpu_native_capture,
      bool encoder_requires_gpu_native_capture
    );
    bool gpu_native_dmabuf_is_safe(
      platf::mem_type_e hwdevice_type,
      wlgrab_capture_policy::gpu_native_capture_route_e route,
      std::optional<std::uint64_t> modifier
    );
    bool should_attempt_gpu_native_cage_capture(
      const platf::runtime_state_t &runtime_state,
      platf::mem_type_e hwdevice_type
    );
    bool should_attempt_headless_extcopy_dmabuf(
      const platf::runtime_state_t &runtime_state,
      platf::mem_type_e hwdevice_type
    );
    bool should_disable_headless_extcopy_after_conversion_failure(
      const platf::runtime_state_t &runtime_state,
      const platf::frame_metadata_t &source_metadata
    );
    bool should_disable_windowed_gpu_native_after_conversion_failure(
      const platf::runtime_state_t &runtime_state,
      const platf::frame_metadata_t &source_metadata
    );
    std::optional<bool> cached_windowed_gpu_native_probe_result();
    std::optional<bool> cached_headless_extcopy_dmabuf_probe_result();
    void update_windowed_gpu_native_probe_result(bool supported);
    void update_headless_extcopy_dmabuf_probe_result(bool supported);
    bool headless_extcopy_dmabuf_probe_succeeded(bool capture_initialized, bool live_gpu_frame_converted);
    bool should_report_headless_ram_capture_fallback(const platf::runtime_state_t &runtime_state);
    bool should_report_windowed_ram_capture_fallback(const platf::runtime_state_t &runtime_state);
    bool should_log_headless_ram_capture_warning();
    bool should_log_windowed_ram_capture_warning();
  }  // namespace labwc

}  // namespace stream_runtime

#endif  // __linux__
