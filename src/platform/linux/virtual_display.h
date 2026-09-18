/**
 * @file src/platform/linux/virtual_display.h
 * @brief Declarations for Linux virtual display creation and management.
 *
 * Provides virtual display support on Linux, analogous to SUDOVDA on Windows.
 * Supports multiple backends:
 *   1. EVDI (Extensible Virtual Display Interface) - true virtual DRM connector
 *   2. KWin virtual outputs - a new screen KWin creates for a screencast stream
 *   3. Wayland compositor headless outputs (hyprctl)
 *   4. kscreen-doctor fallback - manages existing physical displays
 */
#pragma once

// standard includes
#include <cstdint>
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
    // Appended last: /api/vdisplay/status sends the backend as its number.
    KWIN_VIRTUAL_OUTPUT,  ///< A new output KWin creates for a zkde screencast stream
  };

  /**
   * @brief Which backend Host Virtual Display uses, from linux_virtual_display_backend.
   */
  enum class backend_preference_e {
    AUTO,  ///< EVDI, then a KWin virtual output, then Hyprland, then kscreen-doctor
    EVDI,
    KWIN,
    WLR,
    KSCREEN,
  };

  /**
   * @brief Read linux_virtual_display_backend.
   * @return AUTO for an empty value; nullopt for a value that names no backend.
   */
  std::optional<backend_preference_e> parse_backend_preference(std::string_view value);

  /** @brief The config value for a preference, the inverse of parse_backend_preference. */
  std::string_view backend_preference_name(backend_preference_e preference);

  /** @brief What one probe found ready to create a display. */
  struct probe_snapshot_t {
    bool evdi = false;
    bool kwin = false;
    bool wlr = false;
    bool kscreen = false;
  };

  /**
   * @brief Choose the backend for a preference from one probe.
   *
   * A forced backend that is not ready yields NONE rather than another backend:
   * a host set to KWin that quietly borrowed a monitor instead would do exactly
   * what the setting was chosen to avoid.
   */
  backend_e select_backend(backend_preference_e preference, const probe_snapshot_t &probe);

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
   *   3. kscreen-doctor installed with a configured streaming output (fallback)
   */
  bool is_available();

  /**
   * @brief Re-probe backend availability without accepting the short-lived cache.
   *
   * Exact launch admission uses this immediately before installing a stream
   * generation so an earlier capability/status request cannot lend stale
   * authority to a backend that has since disappeared.
   */
  bool is_available_fresh();

  /**
   * @brief Detect which backend is installed and preferred.
   * @return The best detected backend. Call backend_has_required_configuration()
   *         before treating it as ready for virtual display creation.
   */
  backend_e detect_backend();

  /** @brief Detect the preferred backend after bypassing the probe cache. */
  backend_e detect_backend_fresh();

  /**
   * @brief Apply a saved linux_virtual_display_backend while Polaris runs.
   *
   * Kept here under the detection lock rather than written into the shared
   * configuration, which other threads copy and restore wholesale.
   */
  void set_backend_preference(const std::string &value);

  /** @brief The linux_virtual_display_backend value in effect, "auto" when unset. */
  std::string backend_preference_value();

  /** @brief Select the highest-priority detected backend from one probe snapshot, KWin aside. */
  backend_e select_preferred_backend(bool evdi_ready, bool wayland_ready, bool kscreen_installed);

  /**
   * @brief The name Polaris asks KWin to give the output it creates for one slot.
   *
   * Slots rather than pids: KWin stores a layout per output name, and a name
   * that changed every run would add an entry to kwinoutputconfig.json each
   * time. The slot keeps a streaming session and the web UI apart.
   */
  std::string kwin_output_request_name(int slot);

  /** @brief The output name KWin publishes for a requested virtual output. */
  std::string kwin_output_expected_name(std::string_view request_name);

  /** @brief Whether an output name is one Polaris asked KWin to create. */
  bool kwin_output_is_polaris_owned(std::string_view output_name);

  /** @brief stream_virtual_output_with_description, the request used, arrived in version 4. */
  bool kwin_screencast_version_supported(std::uint32_t version);

  /** @brief One output as `kscreen-doctor --json` reports it. */
  struct kscreen_output_layout_t {
    std::string name;
    bool enabled = false;
    int priority = 0;
    double scale = 1.0;
    int x = 0;
    int y = 0;
    int mode_width = 0;
    int mode_height = 0;
    double refresh_hz = 0.0;
    std::vector<std::string> mode_names;  ///< "WxH@R", the names kscreen-doctor selects by
  };

  /**
   * @brief Every output in a `kscreen-doctor --json` answer.
   * @return nullopt when the answer is not usable at all. An output whose
   *         current mode cannot be read is kept with a zero-sized mode.
   */
  std::optional<std::vector<kscreen_output_layout_t>> kscreen_layout_from_json(std::string_view json);

  /** @brief The enabled output ranked first, if there is one. */
  std::optional<std::string> kscreen_primary_output(const std::vector<kscreen_output_layout_t> &layout);

  /**
   * @brief The x coordinate just past every enabled output except `excluding`.
   *
   * KWin can give a new virtual output a layout it stored for another one,
   * which on the test host put it at 0,0 on top of the real monitor.
   */
  int kscreen_right_edge(const std::vector<kscreen_output_layout_t> &layout, std::string_view excluding);

  /** @brief The mode name kscreen-doctor selects by, such as `1920x1080@120`. */
  std::string kwin_mode_name(int width, int height, int hz);

  /** @brief kscreen-doctor arguments that add a custom mode (refresh in mHz). */
  std::vector<std::string> kwin_custom_mode_args(std::string_view output, int width, int height, int hz);

  /** @brief kscreen-doctor arguments that select a mode by name. */
  std::vector<std::string> kwin_mode_args(std::string_view output, int width, int height, int hz);

  /** @brief kscreen-doctor arguments that put a new KWin screen at scale 1 at (x, 0). */
  std::vector<std::string> kwin_placement_args(std::string_view output, int x);

  /**
   * @brief kscreen-doctor arguments that rank every screen as before, with the new one last.
   *
   * Plasma gives each rank its own desktop, icons and panel, so whichever screen
   * takes a rank a real monitor held takes that monitor's desktop with it: the
   * first rank moved the main desktop onto the stream, and any rank above a
   * second monitor moves that monitor's. KWin can give a new output a stored
   * layout that ranks it anywhere, so every enabled screen is named, in its old
   * order, and the new one after them. Windows are moved onto it instead (see
   * kwin_window_follow_script).
   */
  std::vector<std::string> kwin_priority_args(std::string_view output, const std::vector<kscreen_output_layout_t> &layout_before);

  /** @brief The enabled screens are ranked as in `layout_before`, with `output` last. */
  bool kwin_ranking_matches(
    const std::vector<kscreen_output_layout_t> &layout,
    const std::vector<kscreen_output_layout_t> &layout_before,
    std::string_view output
  );

  /**
   * @brief The KWin script that moves windows onto a Polaris screen while it exists.
   *
   * Application windows, dialogs and splash screens that open while the screen
   * exists are sent to it, so a game lands on the stream without the screen
   * being primary. Panels, the desktop, notifications and popups stay put, and
   * so do a window already on another Polaris screen and the desktop's own
   * prompts (polkit, ksshaskpass, KWallet, KRunner), which are for whoever sits
   * at the host. It does nothing once the screen is gone, so a script left
   * behind by a crash is harmless.
   */
  std::string kwin_window_follow_script(std::string_view output_name);

  /** @brief The KWin script plugin name for one Polaris screen. */
  std::string kwin_window_follow_plugin_name(std::string_view output_name);

  /** @brief The output runs the requested size, within half a hertz of the requested rate. */
  bool kwin_mode_matches(const kscreen_output_layout_t &output, int width, int height, int hz);

  /** @brief The output is at scale 1 and at (x, 0). */
  bool kwin_placement_matches(const kscreen_output_layout_t &output, int x);

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

  /**
   * @brief The connector the kscreen-doctor fallback may borrow.
   *
   * The active linux_streaming_output while a mode holds one, otherwise the
   * saved one. Modes that own no connector retire the active one on load, and
   * Host Virtual Display must still be offered as a mode to switch to.
   */
  std::string host_virtual_display_connector();

  /**
   * @brief Build the kscreen-doctor call that makes a borrowed output the stream display.
   *
   * The borrowed output is enabled and made priority 1. The configured primary
   * output is moved to priority 2 only when it is a different output: when
   * both settings name one connector this same call has just made it first,
   * and a trailing priority 2 for it would win, because kscreen-doctor applies
   * its arguments in order.
   *
   * @param output The output named by `linux_streaming_output`.
   * @param mode A kscreen-doctor mode such as `1920x1080@60`, or empty to only enable.
   * @param primary_output The output named by `linux_primary_output`; may be empty.
   * @return The full argv, starting with `kscreen-doctor`.
   */
  std::vector<std::string> kscreen_enable_args(
    std::string_view output,
    std::string_view mode,
    std::string_view primary_output
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
   * @brief Why the backend linux_virtual_display_backend forces cannot be used.
   * @param detail What that backend's own probe found, when it said more than no.
   */
  std::string forced_backend_unavailable_reason(backend_preference_e preference, std::string_view detail);

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
