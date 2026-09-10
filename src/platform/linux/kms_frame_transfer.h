/** @file src/platform/linux/kms_frame_transfer.h
 * Transfer exported descriptors only after a complete successful capture.
 */
#pragma once
#include "graphics.h"

namespace platf::kms_capture {
  template<class Refresh>
  capture_e refresh_owned_frame(egl::img_descriptor_t &image, Refresh &&refresh) {
    image.reset();
    file_t descriptors[4];
    egl::surface_descriptor_t staged {.fds = {-1, -1, -1, -1}};
    std::optional<std::chrono::steady_clock::time_point> timestamp;
    const auto status = refresh(descriptors, &staged, timestamp);
    if (status != capture_e::ok) return status;
    // These assignments and release operations cannot throw. All subsequent
    // cursor/metadata work sees one owner, including on exceptional exits.
    image.sd = staged;
    image.frame_timestamp = timestamp;
    for (auto &descriptor : descriptors) descriptor.release();
    return capture_e::ok;
  }
}
