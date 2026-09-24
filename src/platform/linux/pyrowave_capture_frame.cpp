/**
 * @file src/platform/linux/pyrowave_capture_frame.cpp
 * @brief Reading a captured frame's dmabuf without the headers that describe one.
 */
#include "src/platform/linux/pyrowave_capture_frame.h"

// local includes
#include "src/platform/linux/graphics.h"

namespace pyrowave_encode {

  bool dmabuf_from_frame(platf::img_t &image, dmabuf_t &out) {
    auto *descriptor = dynamic_cast<egl::img_descriptor_t *>(&image);
    if (!descriptor || descriptor->sd.fds[0] < 0) {
      return false;
    }

    const auto &sd = descriptor->sd;
    for (int i = 0; i < 4; i++) {
      out.fds[i] = sd.fds[i];
      out.pitches[i] = sd.pitches[i];
      out.offsets[i] = sd.offsets[i];
    }
    out.fourcc = sd.fourcc;
    out.modifier = sd.modifier;
    out.width = sd.width;
    out.height = sd.height;
    // Which of capture's buffers this came out of. Zero from a backend that does not track them,
    // which costs a description per frame rather than being wrong about which buffer it holds.
    out.buffer_key = descriptor->dmabuf_buffer_key;
    return out.width > 0 && out.height > 0;
  }

}  // namespace pyrowave_encode
