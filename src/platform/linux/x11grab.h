/**
 * @file src/platform/linux/x11grab.h
 * @brief Declarations for x11 capture.
 */
#pragma once

// standard includes
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>

// local includes
#include "src/platform/common.h"
#include "src/utility.h"

// X11 Display
extern "C" struct _XDisplay;

namespace egl {
  class cursor_t;
}

namespace platf::x11 {
  /**
   * @brief Size an XCB SHM BGRA frame without narrowing or overflowing.
   *
   * XCB's SHM image request carries 16-bit dimensions, and img_t stores the
   * four-byte row pitch in an int. Reject dimensions that either interface
   * cannot represent before allocating shared or process-local storage.
   */
  inline std::optional<std::size_t> checked_shm_frame_size(int width, int height) {
    constexpr std::size_t bytes_per_pixel = 4;
    if (width <= 0 || height <= 0 ||
        width > std::numeric_limits<std::uint16_t>::max() ||
        height > std::numeric_limits<std::uint16_t>::max()) {
      return std::nullopt;
    }

    const auto unsigned_width = static_cast<std::size_t>(width);
    const auto unsigned_height = static_cast<std::size_t>(height);
    if (unsigned_width > std::numeric_limits<std::size_t>::max() / bytes_per_pixel / unsigned_height) {
      return std::nullopt;
    }
    return unsigned_width * unsigned_height * bytes_per_pixel;
  }

  struct cursor_ctx_raw_t;
  void freeCursorCtx(cursor_ctx_raw_t *ctx);
  void freeDisplay(_XDisplay *xdisplay);

  using cursor_ctx_t = util::safe_ptr<cursor_ctx_raw_t, freeCursorCtx>;
  using xdisplay_t = util::safe_ptr<_XDisplay, freeDisplay>;

  class cursor_t {
  public:
    static std::optional<cursor_t> make();

    void capture(egl::cursor_t &img);

    /**
     * Capture and blend the cursor into the image
     *
     * img <-- destination image
     * offsetX, offsetY <--- Top left corner of the virtual screen
     */
    void blend(img_t &img, int offsetX, int offsetY);

    cursor_ctx_t ctx;
  };

  xdisplay_t make_display();
}  // namespace platf::x11
