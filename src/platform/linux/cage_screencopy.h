/**
 * @file src/platform/linux/cage_screencopy.h
 * @brief Direct wlr-screencopy capture against a cage/labwc Wayland socket.
 *
 * Self-contained Wayland client (no XDG portal, no PipeWire). Used by
 * portal_grab when labwc/cage is configured; lives next to wlgrab as the
 * private-compositor SHM fallback path.
 *
 * Teardown: request_stop() from the display dtor (or session end) so the
 * capture loop exits. session_media remains the sole ordered media teardown
 * owner for portal/PipeWire; this only stops the screencopy client thread.
 */
#pragma once

#ifdef __linux__

  #include <string>

  #include "src/platform/common.h"

namespace cage_screencopy {

  /**
   * @brief Signal the active screencopy capture loop to exit.
   *
   * Safe when no capture is running. Called from portal_display_t dtor so
   * capture() does not outlive the display backend.
   */
  void request_stop();

  /**
   * @brief Capture frames via wlr-screencopy SHM from cage_socket.
   *
   * @param cage_socket  Wayland display name / path for the cage compositor
   * @param req_width    Requested stream width (unused for SHM size; compositor decides)
   * @param req_height   Requested stream height
   * @param push_cb      Encoder push callback
   * @param pull_cb      Encoder free-image pull callback
   * @param cursor       Whether to include the cursor in frames
   * @param out_width    Updated to actual frame width when known
   * @param out_height   Updated to actual frame height when known
   */
  platf::capture_e capture(
    const std::string &cage_socket,
    int req_width,
    int req_height,
    const platf::display_t::push_captured_image_cb_t &push_cb,
    const platf::display_t::pull_free_image_cb_t &pull_cb,
    bool *cursor,
    int &out_width,
    int &out_height
  );

}  // namespace cage_screencopy

#endif  // __linux__
