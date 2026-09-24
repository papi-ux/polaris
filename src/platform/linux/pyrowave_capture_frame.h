/**
 * @file src/platform/linux/pyrowave_capture_frame.h
 * @brief Reading a captured frame's dmabuf without the headers that describe one.
 */
#pragma once

// local includes
#include "src/platform/common.h"
#include "src/platform/linux/pyrowave_encode.h"

namespace pyrowave_encode {

  /**
   * @brief Describe a captured frame's dmabuf, or say that it does not have one.
   *
   * The capture backends describe a dmabuf with a type that lives behind the EGL headers, and the
   * encoder has no business including those: it needs four integers and a modifier, not a way to
   * make textures. This is the one place that knows both, and it is why video.cpp does not.
   *
   * The descriptors stay the frame's. Importing duplicates what it uses.
   *
   * @param image The captured frame.
   * @param out Filled only when true is returned.
   * @return false when this frame is in system memory, which is most of them.
   */
  bool dmabuf_from_frame(platf::img_t &image, dmabuf_t &out);

}  // namespace pyrowave_encode
