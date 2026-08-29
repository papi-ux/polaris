/**
 * @file src/platform/linux/stream_display_policy.h
 * @brief Resolve/apply facade over stream_path (path ids + descriptors are SoT).
 *
 * stream_path owns path vocabulary and resolved_t; this layer maps config →
 * resolved session flags and applies user selections into legacy bools.
 */
#pragma once

#include "stream_path.h"

#include <string>
#include <string_view>
#include <vector>

namespace virtual_display {
  enum class backend_e;
}

namespace stream_display_policy {

  struct input_t {
    bool virtual_display_available = false;
    bool active_encoder_requires_gpu_native_capture = false;
    bool runtime_gpu_native_override_active = false;
  };

  /// Sole resolved session description (SoT lives in stream_path).
  using resolved_t = stream_path::resolved_t;

  struct mode_option_t {
    std::string value;
    std::string label;
    std::string reason;
    bool available = true;
    std::string unavailable_reason;
    std::string group;  // private | host | advanced | experimental
    std::string runtime;  // labwc | gamescope | ""
    std::string capture;  // auto | portal | wlroots | kms
    std::string topology;  // leave_alone | host_virtual | swap_primary
  };

  struct legacy_booleans_t {
    bool headless_mode = false;
    bool use_cage_compositor = false;
    bool prefer_gpu_native_capture = false;
  };

  /**
   * @brief Derive a session mode for legacy Virtual Display launches.
   *
   * Explicit accepted streamMode and mirrorDesktop remain authoritative. An
   * app default is used only when the client did not explicitly lock the
   * virtual-display choice.
   */
  std::string effective_session_selection_for_launch(
    std::string_view requested_selection,
    bool mirror_desktop,
    bool launch_virtual_display,
    bool app_virtual_display,
    bool virtual_display_user_locked,
    bool virtual_display_optimization_present = false
  );

  /**
   * @brief Keep the created virtual connector name when display mapping cannot
   *        produce a backend-specific identifier.
   */
  std::string capture_output_name_for_virtual_display(
    std::string_view created_output_name,
    std::string_view mapped_output_name
  );

  /**
   * @brief Resolve the capture backend required by a host virtual display.
   *
   * Native wlroots headless outputs are capturable directly by output name.
   * EVDI/KScreen outputs remain on the portal/KWin path unless an operator
   * explicitly selected another backend.
   */
  std::string capture_for_host_virtual_display_backend(
    virtual_display::backend_e backend,
    std::string_view current_capture
  );

  /**
   * @brief Whether Host Virtual Display creates a new output connector.
   *
   * KScreen manages an existing connector. EVDI and wlroots create their own
   * output, so carrying a dongle connector into those backends would leak game
   * placement and capture authority from the previous topology.
   */
  bool host_virtual_backend_creates_output(
    virtual_display::backend_e backend
  );

  /**
   * @brief Whether retained connector authority is valid for a Host Virtual backend.
   *
   * KScreen manages an existing connector. EVDI and wlroots create a new one,
   * so their final launch fast path must force normalization while any prior
   * connector or capture-output pin remains.
   */
  bool host_virtual_connector_state_matches(
    virtual_display::backend_e backend,
    std::string_view streaming_output,
    std::string_view output_name
  );

  /** Stable selection ids — SoT is stream_path (no dual string tables). */
  constexpr std::string_view k_headless_stream = stream_path::k_headless_stream;
  constexpr std::string_view k_windowed_stream = stream_path::k_windowed_stream;
  constexpr std::string_view k_host_virtual_display = stream_path::k_host_virtual_display;
  constexpr std::string_view k_desktop_display = stream_path::k_desktop_display;
  constexpr std::string_view k_gamescope_stream = stream_path::k_gamescope_stream;
  constexpr std::string_view k_headless_dongle = stream_path::k_headless_dongle;

  constexpr std::string_view k_runtime_labwc = stream_path::k_runtime_labwc;
  constexpr std::string_view k_runtime_gamescope = stream_path::k_runtime_gamescope;

  /**
   * @brief Human label for a selection id.
   */
  std::string label_for_selection(std::string_view selection);

  /**
   * @brief Reason copy for a selection id (may depend on virtual display availability).
   */
  std::string reason_for_selection(std::string_view selection, bool virtual_display_available = false);

  /**
   * @brief Whether a mode is available on this host build.
   */
  bool selection_available(std::string_view selection);

  /**
   * @brief Whether a client may drive this selection for one session.
   *
   * A path that swaps host output topology changes the machine's physical display
   * arrangement, so it stays host-default-only rather than something a single
   * client can turn on for one launch. Derived from the path's topology rather
   * than an id list, so a future swapping path inherits the rule.
   */
  bool selection_session_overridable(std::string_view selection);

  std::string selection_unavailable_reason(std::string_view selection);

  /**
   * @brief Whether a selection would pass apply_selection's id and availability checks.
   *
   * The single validity truth for network-facing validators: apply_selection
   * calls this itself, so a validator delegating here can never accept a mode
   * apply would then reject - the drift that used to 400 modes the host had
   * just advertised in allowed_modes.
   *
   * @param error On failure, the reason to serve (the host's real unavailable
   *              reason for registered-but-unavailable modes).
   */
  bool selection_valid(std::string_view selection, std::string &error);

  /**
   * @brief Validate against a newly probed host capability snapshot.
   *
   * Use this at exact optimize/parse/final-launch boundaries. Status and UI
   * catalogs may use selection_valid(), which accepts the short probe cache.
   */
  bool selection_valid_fresh(std::string_view selection, std::string &error);

  /**
   * @brief Capability-injected form used to keep served availability and the
   *        launch validator on one deterministic rule.
   */
  bool selection_valid_for_capabilities(
    std::string_view selection,
    bool virtual_display_available,
    std::string &error
  );

  /**
   * @brief Derive the configured selection from legacy booleans when
   *        linux_stream_mode is unset.
   */
  std::string selection_from_legacy_booleans(const legacy_booleans_t &booleans);

  /**
   * @brief Return the configured selection after legacy-boolean normalization.
   */
  std::string configured_selection();

  /**
   * @brief Whether a mode creates/owns the stream output refresh rate.
   */
  bool selection_owns_launch_refresh_rate(std::string_view selection);

  /**
   * @brief Map a selection id to the legacy boolean triple used by older clients.
   */
  legacy_booleans_t legacy_booleans_for_selection(std::string_view selection);

  /**
   * @brief Resolve the configured (desired) mode from config + optional inputs.
   */
  resolved_t resolve(const input_t &input = {});

  /**
   * @brief Convenience: resolve using virtual_display::is_available() and flags.
   */
  resolved_t resolve_current(bool active_encoder_requires_gpu_native_capture = false,
                             bool runtime_gpu_native_override_active = false);

  /**
   * @brief Resolve the effective mode while a session is live.
   */
  resolved_t resolve_effective(const input_t &input,
                               bool streaming,
                               bool session_uses_virtual_display,
                               bool runtime_effective_headless);

  /**
   * @brief Whether a selection ID and its deterministic companion state agree.
   */
  bool selection_companion_state_matches(std::string_view selection);

  /**
   * @brief Apply a user/API selection into config (stream_mode + legacy bools + runtime).
   *
   * @return false and sets error on failure.
   */
  bool apply_selection(std::string_view selection, std::string &error);

  /**
   * @brief Normalize config after load: if stream_mode set, sync booleans; else
   *        leave booleans authoritative and fill stream_mode in memory.
   */
  void normalize_config_from_load();

  /**
   * @brief Options list for client-settings / UI (includes unavailable gamescope).
   */
  std::vector<mode_option_t> mode_options(bool virtual_display_available = false);

  /**
   * @brief Allowed launch-mode selection ids for Nova (excludes unavailable modes).
   */
  std::vector<std::string> allowed_launch_modes(bool virtual_display_available,
                                                bool include_unavailable = false);

}  // namespace stream_display_policy
