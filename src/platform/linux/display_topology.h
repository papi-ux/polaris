/**
 * @file src/platform/linux/display_topology.h
 * @brief Host display topology prepare/restore for stream paths (dongle swap, etc.).
 *
 * Used by headless_dongle (and future headless_evdi) paths. Private labwc/gamescope
 * runtimes must not call these — they own an isolated surface and leave the host layout alone.
 */
#pragma once

#ifdef __linux__

#include <string>
#include <string_view>
#include <vector>

namespace display_topology {

  struct output_info_t {
    std::string name;  ///< Connector name as used by kscreen-doctor (e.g. HDMI-A-2)
    std::string drm_path;  ///< sysfs name (e.g. card1-HDMI-A-2)
    bool connected = false;
    bool enabled = false;
    bool likely_dongle = false;  ///< Heuristic: connected HDMI/DP dummy-style plug
    bool suggested_primary = false;
    bool suggested_streaming = false;
  };

  /**
   * @brief List DRM connectors from sysfs (fast; does not hang like kscreen-doctor on some hosts).
   */
  std::vector<output_info_t> list_outputs();

  /**
   * @brief Fill empty streaming_output / primary_output from discovery when possible.
   * @return true if both fields are non-empty after the call.
   */
  bool ensure_dongle_outputs_configured();

  /**
   * @brief Whether privacy swap mode makes the streaming output primary and blanks primary.
   */
  bool swap_makes_headless_primary(std::string_view swap_mode);

  /**
   * @brief True when the configured path should run kscreen-doctor display swap.
   *
   * Requires auto_manage + streaming_output, and must not run for private labwc sessions.
   */
  bool should_manage_host_topology();

  /**
   * @brief Enable streaming output / optional privacy swap before capture.
   *
   * @param width Requested stream width in pixels (advisory).
   * @param height Requested stream height in pixels (advisory).
   * @param refresh_hz Requested stream refresh in Hz (advisory).
   */
  void prepare_for_stream(int width, int height, int refresh_hz);

  /**
   * @brief Restore primary display layout after the stream ends.
   */
  void restore_after_stream();

  /**
   * @brief Whether a connector with this name is present in sysfs (list_outputs).
   */
  bool output_present(const std::string &name);

  /**
   * @brief Read an output's current mode out of `kscreen-doctor -j` output.
   *
   * Pure so the read-back can be tested without a compositor. Resolves the
   * output's currentModeId against its mode list and renders the result in the
   * same `WxH@RHz` form the mode request is built in, so the two are directly
   * comparable. Refresh is rounded, because kscreen reports 59.987 for a mode
   * it names 60.
   *
   * @param kscreen_json Raw stdout of `kscreen-doctor -j`.
   * @param output_name Connector name, for example "DP-2".
   * @return The current mode, or an empty string when it cannot be determined.
   */
  std::string current_output_mode(std::string_view kscreen_json, std::string_view output_name);

}  // namespace display_topology

#endif
