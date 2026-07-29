/**
 * @file src/platform/linux/session_launch_linux.h
 * @brief Sanitize / child env helpers for Linux private-runtime session start.
 *
 * Extracted from process.cpp (stream-runtime-p1). Stop teardown stays session_media.
 */
#pragma once

#ifdef __linux__

  #include "stream_runtime.h"

  #include <boost/process/v1/environment.hpp>

  #include <string>

namespace session_launch_linux {

  using env_t = boost::process::v1::environment;

  /// Strip leading setsid for labwc kiosk primary (setsid breaks cage child).
  std::string sanitize_cage_command(const std::string &cmd);

  /// Labwc child: WAYLAND_DISPLAY + optional DISPLAY from runtime sockets.
  void apply_labwc_child_env(env_t &env, const stream_runtime::stream_runtime_t *rt);

  /**
   * Gamescope X11-attach env (SB-5): DISPLAY = first gamescope XWayland (:0) with
   * STEAM_MULTIPLE_XWAYLANDS so Steam puts games on :1; pin GAMESCOPE_WAYLAND_DISPLAY;
   * optional DXVK/Proton HDR for present into --hdr-enabled gamescope.
   */
  void apply_gamescope_attach_env(
    env_t &env,
    const stream_runtime::stream_runtime_t *rt,
    bool enable_hdr
  );

}  // namespace session_launch_linux

#endif
