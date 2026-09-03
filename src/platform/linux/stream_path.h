/**
 * @file src/platform/linux/stream_path.h
 * @brief Path-id registry (runtime/capture/topology). SoT for linux_stream_mode ids.
 *        resolve/apply facade: stream_display_policy. Map: docs/stream-paths.md.
 */
#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace stream_path {

  /**
   * @brief Who hosts app rendering for the stream session.
   */
  enum class runtime_kind_e {
    NONE,  ///< Host desktop or host virtual output; no private compositor
    LABWC,  ///< Private labwc (stream_runtime::labwc)
    GAMESCOPE,  ///< Private or attach gamescope (stream_runtime_gamescope)
  };

  /**
   * @brief Preferred capture family. Actual transport still negotiates at runtime.
   */
  enum class capture_kind_e {
    AUTO,  ///< Let platform pick (wlroots on labwc, portal on desktop, …)
    WLROOTS,  ///< wlr-screencopy / ext-image-copy on a private compositor
    PORTAL,  ///< XDG Desktop Portal ScreenCast → PipeWire
    KMS,  ///< DRM/KMS plane capture
  };

  /**
   * @brief Host display topology policy for the session.
   */
  enum class topology_kind_e {
    LEAVE_ALONE,  ///< Do not rearrange host outputs (private stream, mirror)
    HOST_VIRTUAL,  ///< Create/use host-visible virtual output (wlr/kscreen)
    DESKTOP_TAKEOVER,  ///< Move Hyprland workspaces to a new virtual output and blank sources
    SWAP_PRIMARY,  ///< Move desktop onto dongle and restore on stop (#226-style)
  };

  /**
   * @brief Static description of a selectable stream path.
   *
   * Add new modes by appending to the registry in stream_path.cpp — keep IDs stable
   * for Nova / client-settings / config files.
   */
  struct descriptor_t {
    std::string_view id;
    std::string_view label;
    std::string_view badge;
    std::string_view reason;
    runtime_kind_e runtime = runtime_kind_e::NONE;
    capture_kind_e capture = capture_kind_e::AUTO;
    topology_kind_e topology = topology_kind_e::LEAVE_ALONE;
    /// When true, linux_prefer_gpu_native_capture defaults on for this path.
    bool prefer_gpu_native = false;
    /// Request true-headless private runtime when runtime is LABWC/GAMESCOPE.
    bool request_headless = false;
    /// Path is shipped but not ready to select (or host probe failed).
    bool available = true;
    // Availability probes can produce dynamic text. Keep ownership with each
    // descriptor so callers may safely retain and serialize the catalog.
    std::string unavailable_reason;
    /// UI group hint: private | host | advanced | experimental
    std::string_view group;
  };

  /**
   * @brief Probed host features used for availability and honest backend labels.
   */
  struct host_capabilities_t {
    bool labwc_present = false;
    bool wlr_randr_present = false;
    bool gamescope_present = false;
    bool virtual_display_available = false;
    bool desktop_takeover_available = false;
    /// Capture backend currently configured (e.g. "portal", "kms", "wlr").
    std::string configured_capture;
  };

  /**
   * @brief Resolved path for the current config + live session flags (sole resolved_t).
   */
  struct resolved_t {
    std::string selection;  // owned copy of path id
    std::string label;
    std::string reason;
    std::string backend_name;  // honest: labwc | gamescope | portal | host | virtual_display
    runtime_kind_e runtime = runtime_kind_e::NONE;
    capture_kind_e capture = capture_kind_e::AUTO;
    topology_kind_e topology = topology_kind_e::LEAVE_ALONE;
    bool available = true;
    std::string unavailable_reason;
    bool requested_headless = false;
    bool effective_headless = false;
    bool use_private_runtime = false;
    bool use_host_virtual_display = false;
    bool prefer_gpu_native_capture = false;
    bool should_defer_encoder_probe = false;

    bool uses_labwc() const {
      return use_private_runtime && runtime == runtime_kind_e::LABWC;
    }
  };

  // Stable path IDs (also used as linux_stream_mode / client stream_display_mode).
  constexpr std::string_view k_headless_stream = "headless_stream";
  constexpr std::string_view k_windowed_stream = "windowed_stream";
  constexpr std::string_view k_host_virtual_display = "host_virtual_display";
  constexpr std::string_view k_desktop_takeover = "desktop_takeover";
  constexpr std::string_view k_desktop_display = "desktop_display";
  constexpr std::string_view k_gamescope_stream = "gamescope_stream";
  constexpr std::string_view k_headless_dongle = "headless_dongle";

  constexpr std::string_view k_runtime_labwc = "labwc";
  constexpr std::string_view k_runtime_gamescope = "gamescope";
  constexpr std::string_view k_backend_portal = "portal";
  constexpr std::string_view k_backend_host = "host";
  constexpr std::string_view k_backend_virtual_display = "virtual_display";

  /**
   * @brief Built-in path registry (includes unavailable reserved paths).
   */
  std::vector<descriptor_t> registry();

  const descriptor_t *find(std::string_view id);

  /// Shared ASCII lower-case helper (path/policy/runtime probes).
  std::string to_lower_copy(std::string_view value);

  /// Lightweight PATH probe without boost.process (X_OK).
  bool binary_on_path(const char *name);

  std::string_view runtime_kind_id(runtime_kind_e kind);
  std::string_view capture_kind_id(capture_kind_e kind);
  std::string_view topology_kind_id(topology_kind_e kind);

  /**
   * @brief Probe host capabilities (binaries / backends). Safe to call often;
   *        expensive probes may cache internally later.
   */
  host_capabilities_t probe_host_capabilities();

  /** @brief Whether every executable required by the labwc runtime is on PATH. */
  bool labwc_runtime_available(const host_capabilities_t &caps);

  /** @brief Exact missing-runtime reason served to selectors and launch validation. */
  std::string labwc_runtime_unavailable_reason(const host_capabilities_t &caps);

  /**
   * @brief Map a path + live flags to a fully resolved session description.
   */
  resolved_t resolve_path(
    const descriptor_t &path,
    const host_capabilities_t &caps,
    bool active_encoder_requires_gpu_native = false,
    bool runtime_gpu_native_override_active = false
  );

  /**
   * @brief Honest backend_name for stats/UI when a private runtime is not running.
   */
  std::string backend_name_for_path(const descriptor_t &path, const host_capabilities_t &caps);

  /**
   * @brief Options list for API/UI, with availability applied from caps.
   */
  std::vector<descriptor_t> options_for_host(const host_capabilities_t &caps);

}  // namespace stream_path
