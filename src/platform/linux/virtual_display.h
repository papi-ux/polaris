/**
 * @file src/platform/linux/virtual_display.h
 * @brief Declarations for Linux virtual display creation and management.
 *
 * Provides virtual display support on Linux, analogous to SUDOVDA on Windows.
 * Supports multiple backends:
 *   1. EVDI (Extensible Virtual Display Interface) - true virtual DRM connector
 *   2. Wayland compositor headless outputs (wlr-randr, hyprctl, kwin)
 *   3. kscreen-doctor fallback - manages existing physical displays
 */
#pragma once

// standard includes
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace virtual_display {

  /**
   * @brief Identifies which backend is being used for virtual display management.
   */
  enum class backend_e {
    NONE,            ///< No backend available
    EVDI,            ///< EVDI kernel module + libevdi
    WAYLAND_WLR,     ///< wlroots-based compositor (wlr-randr / hyprctl)
    KSCREEN_DOCTOR,  ///< KDE kscreen-doctor (manages existing outputs)
  };

  struct kscreen_output_state_t {
    std::string name;
    bool enabled = false;
    std::string current_mode_id;
    int priority = 0;

    bool operator==(const kscreen_output_state_t &) const = default;
  };

  /**
   * @brief Tracks whether a backend observation should be logged.
   *
   * The first observation of a backend should log. Repeated observations of
   * the same backend stay quiet. If the detected backend changes, log again.
   */
  struct backend_detection_log_cache_t {
    bool initialized = false;
    backend_e last_backend = backend_e::NONE;

    bool note(backend_e backend) {
      const bool should_log = !initialized || last_backend != backend;
      initialized = true;
      last_backend = backend;
      return should_log;
    }
  };

  /**
   * @brief Represents an active virtual display instance.
   */
  struct vdisplay_t {
    std::string device_path;     ///< DRI device path, e.g. /dev/dri/cardN (EVDI only)
    std::string output_name;     ///< Output connector name, e.g. "VIRTUAL-1", "HEADLESS-1"
    int width = 0;               ///< Horizontal resolution
    int height = 0;              ///< Vertical resolution
    int fps = 0;                 ///< Refresh rate (Hz)
    bool active = false;         ///< Whether the display is currently active
    backend_e backend = backend_e::NONE;  ///< Which backend created this display

    // KScreen mutates an existing output rather than creating one. Exact
    // pre-launch state is therefore part of the durable teardown authority.
    std::optional<kscreen_output_state_t> kscreen_output_before;
    std::optional<kscreen_output_state_t> kscreen_primary_before;

    // EVDI-specific state (opaque handle, managed internally)
    void *evdi_handle = nullptr;
  };

  /**
   * @brief One display recorded in the on-disk state, with the pid that owns it.
   */
  struct persisted_display_t {
    int owner_pid = 0;
    vdisplay_t display;
  };

  /**
   * @brief Check if any virtual display backend is available on the system.
   * @return true if at least one backend can create virtual displays.
   *
   * Checks in priority order:
   *   1. EVDI module loaded and libevdi available
   *   2. Wayland compositor with headless output support
   *   3. kscreen-doctor installed (fallback)
   */
  bool is_available();

  /**
   * @brief Detect which backend is available and preferred.
   * @return The best available backend for virtual display creation.
   */
  backend_e detect_backend();

  /**
   * @brief Return whether a detected backend has the configuration it needs to create a display.
   *
   * kscreen-doctor can only manage an existing configured streaming output.
   * Treating the binary alone as available makes clients select host virtual display
   * and then fail launch with a 503 when no output was configured.
   */
  bool backend_has_required_configuration(backend_e backend, const std::string &streaming_output);

  /**
   * @brief Decide whether the Wayland backend may be probed before or after platform init.
   *
   * App discovery runs before platf::init() populates the global window-system
   * state. WAYLAND_DISPLAY is therefore also authoritative for that early probe.
   */
  bool wayland_backend_probe_allowed(bool platform_reports_wayland, std::string_view wayland_display);

  /** @brief Return true only when the compositor can create a caller-named output. */
  bool wayland_compositor_supports_exact_output_creation(std::string_view compositor);

  /**
   * @brief Build the connector name requested from Hyprland for one virtual display.
   *
   * The pid keeps the name out of the user's HEADLESS-N namespace; the slot keeps
   * concurrent displays in the same process — a streaming session and the web UI —
   * from requesting the same connector.
   */
  std::string hyprland_output_name_for_pid(int pid, int slot);

  /**
   * @brief Return exact presence from a valid Hyprland monitor response.
   *
   * `nullopt` means the response cannot prove either presence or absence.
   */
  std::optional<bool> hyprland_monitors_contain_output(std::string_view monitors_json, std::string_view output_name);

  /// An output's active mode as the compositor reports it.
  struct hyprland_mode_t {
    int width = 0;
    int height = 0;
    double refresh_hz = 0.0;
  };

  /**
   * @brief Read an output's active mode out of `hyprctl monitors -j`.
   *
   * The compositor's own answer is the only trustworthy signal that a mode set
   * landed: `hyprctl keyword` exits 0 on Hyprland 0.56 even when it rejects the
   * request outright (#444). Returns nullopt when the output is absent or its
   * geometry is unusable.
   */
  std::optional<hyprland_mode_t> hyprland_monitor_mode(
    std::string_view monitors_json,
    std::string_view output_name
  );

  /** @brief Parse one exact output from `kscreen-doctor --json`. */
  std::optional<kscreen_output_state_t> kscreen_output_state_from_json(
    std::string_view output_json,
    std::string_view output_name
  );

  /** @brief EVDI output identity is proven only by non-empty connector discovery. */
  bool evdi_output_name_is_proven(std::string_view output_name);

  /** @brief Parse a DRM connector status; unknown values are not absence proof. */
  std::optional<bool> evdi_connector_status_is_connected(std::string_view status);

  /**
   * @brief Return whether an output name belongs to Polaris's Hyprland namespace.
   */
  bool hyprland_output_is_polaris_owned(std::string_view output_name);

  /**
   * @brief Parse the persisted state document into the displays it records.
   *
   * Accepts both the list written today and the single bare display object a
   * Polaris that predates concurrent displays wrote, so an upgrade still cleans
   * up what the old build left behind. Entries that are inactive, unnamed, or
   * carry no backend are dropped.
   */
  std::vector<persisted_display_t> parse_persisted_displays(std::string_view state_json);

  /**
   * @brief Pure decision for whether a persisted display is left over from a dead owner.
   *
   * A record owned by the running process is never stale: a streaming session
   * and the web UI each own one, and neither can see the other's.
   */
  bool persisted_display_is_stale(int owner_pid, int self_pid, bool owner_alive);

  /** @brief Persistence may be cleared only after backend success and inactive readback. */
  inline bool teardown_is_verified(bool backend_succeeded, bool display_active) {
    return backend_succeeded && !display_active;
  }

  /**
   * @brief Human-readable reason a virtual display cannot be created right now.
   * @return Empty string when creation is possible; otherwise the reason to serve to clients.
   */
  std::string unavailable_reason();

  /**
   * @brief Pure mapping from probed availability state to the served reason.
   * @param backend The backend detection result.
   * @param evdi_blocked True when the EVDI module and library are usable but no device can be obtained.
   * @param streaming_output_configured True when linux_streaming_output is set.
   * @return Empty string when the combination is usable; otherwise the reason.
   */
  std::string unavailable_reason_for(backend_e backend, bool evdi_blocked, bool streaming_output_configured);

  /**
   * @brief Create a virtual display with the given resolution and refresh rate.
   * @param width Horizontal resolution in pixels.
   * @param height Vertical resolution in pixels.
   * @param fps Refresh rate in Hz (not milliHz).
   * @return A vdisplay_t on success, or std::nullopt on failure.
   *
   * Tries backends in priority order. For EVDI, creates a new DRM virtual
   * connector. For Wayland, asks the compositor to create a headless output.
   * For kscreen-doctor, enables/configures an existing output.
   */
  std::optional<vdisplay_t> create(int width, int height, int fps);

#ifdef POLARIS_TESTS
  /** @brief Execute a callback under the production virtual-display creation mutex. */
  void with_creation_lock_for_tests(const std::function<void()> &callback);
#endif

  /**
   * @brief Destroy a previously created virtual display.
   * @param display The display instance to destroy (will be marked inactive).
   *
   * For EVDI, disconnects and closes the virtual connector.
   * For Wayland, removes the headless output.
   * For kscreen-doctor, disables the managed output.
   */
  [[nodiscard]] bool destroy(vdisplay_t &display);

  /**
   * @brief Clean up a persisted virtual display from a dead Polaris process.
   *
   * If a previous Polaris session crashed after creating a virtual display,
   * this attempts one best-effort destroy during startup or before the next
   * create. Returns true when stale state was found and cleanup was attempted.
   */
  bool cleanup_stale();

  /**
   * @brief Get a human-readable name for a backend.
   * @param backend The backend to describe.
   * @return A string like "EVDI", "Wayland (wlr)", or "kscreen-doctor".
   */
  const char *backend_name(backend_e backend);

}  // namespace virtual_display
