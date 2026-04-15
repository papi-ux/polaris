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
#include <optional>
#include <string>

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

    // EVDI-specific state (opaque handle, managed internally)
    void *evdi_handle = nullptr;
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

  /**
   * @brief Destroy a previously created virtual display.
   * @param display The display instance to destroy (will be marked inactive).
   *
   * For EVDI, disconnects and closes the virtual connector.
   * For Wayland, removes the headless output.
   * For kscreen-doctor, disables the managed output.
   */
  void destroy(vdisplay_t &display);

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
