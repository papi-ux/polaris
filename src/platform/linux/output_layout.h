/**
 * @file src/platform/linux/output_layout.h
 * @brief Where each output sits on the desktop, in the desktop's own units.
 *
 * A compositor lays its outputs out in logical units: an output turned a quarter is as tall on the
 * desktop as its mode is wide, and one scaled by two covers half its mode each way. A capture of
 * that output still hands back its mode in output pixels, so the two units meet here and nowhere
 * else. Absolute input is placed in the desktop's units, across every output together.
 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <ostream>
#include <span>
#include <utility>

namespace output_layout {
  /**
   * @brief wl_output.transform, with the protocol's own values, so this header needs no Wayland
   *        include. wayland.cpp checks each one against the protocol's enum.
   */
  namespace transform {
    inline constexpr std::int32_t normal = 0;
    inline constexpr std::int32_t turned_90 = 1;
    inline constexpr std::int32_t turned_180 = 2;
    inline constexpr std::int32_t turned_270 = 3;
    inline constexpr std::int32_t flipped = 4;
    inline constexpr std::int32_t flipped_90 = 5;
    inline constexpr std::int32_t flipped_180 = 6;
    inline constexpr std::int32_t flipped_270 = 7;
  }  // namespace transform

  /**
   * @brief A rectangle on the desktop.
   */
  struct rect_t {
    int x = 0;
    int y = 0;
    int width = 0;
    int height = 0;

    constexpr bool empty() const {
      return width <= 0 || height <= 0;
    }

    friend constexpr bool operator==(const rect_t &, const rect_t &) = default;

    friend std::ostream &operator<<(std::ostream &os, const rect_t &rect) {
      return os << rect.width << 'x' << rect.height << " at " << rect.x << ',' << rect.y;
    }
  };

  /**
   * @brief What a compositor says about one output.
   */
  struct output_t {
    /// xdg_output.logical_position: the output's corner on the desktop.
    int x = 0;
    int y = 0;
    /// xdg_output.logical_size: the output's size on the desktop. Zero until the compositor sends one.
    int logical_width = 0;
    int logical_height = 0;
    /// The current wl_output.mode, in output pixels, before the transform and the scale.
    int mode_width = 0;
    int mode_height = 0;
    /// The transform wl_output.geometry carries, and wl_output.scale.
    std::int32_t transform = transform::normal;
    std::int32_t scale = 1;
  };

  /**
   * @brief Whether a transform turns an output a quarter, so that it is as tall on the desktop as
   *        its mode is wide. A flip alone keeps the shape.
   */
  constexpr bool turns_a_quarter(std::int32_t output_transform) {
    return output_transform == transform::turned_90 || output_transform == transform::turned_270 ||
           output_transform == transform::flipped_90 || output_transform == transform::flipped_270;
  }

  /**
   * @brief The output's rectangle on the desktop.
   *
   * xdg-output says it outright, fractional scales included, so its logical size wins. Without one
   * the mode is turned by the transform and divided by the scale, the way a compositor lays the
   * output out.
   */
  constexpr rect_t logical_rect(const output_t &output) {
    if (output.logical_width > 0 && output.logical_height > 0) {
      return {output.x, output.y, output.logical_width, output.logical_height};
    }

    const auto scale = output.scale > 0 ? output.scale : 1;
    auto width = output.mode_width;
    auto height = output.mode_height;
    if (turns_a_quarter(output.transform)) {
      std::swap(width, height);
    }
    return {output.x, output.y, width / scale, height / scale};
  }

  /**
   * @brief The desktop: the smallest rectangle that holds every output. An output with no area
   *        yet holds nothing, and no outputs make an empty desktop.
   */
  constexpr rect_t bounds(std::span<const rect_t> outputs) {
    bool any = false;
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;
    for (const auto &output : outputs) {
      if (output.empty()) {
        continue;
      }
      if (!any) {
        left = output.x;
        top = output.y;
        right = output.x + output.width;
        bottom = output.y + output.height;
        any = true;
        continue;
      }
      left = std::min(left, output.x);
      top = std::min(top, output.y);
      right = std::max(right, output.x + output.width);
      bottom = std::max(bottom, output.y + output.height);
    }
    return {left, top, right - left, bottom - top};
  }

  /**
   * @brief An output counted from the desktop's corner, which is where absolute input counts from.
   *        A desktop with a screen left of or above the origin has its corner there, not at zero.
   */
  constexpr rect_t on_desktop(const rect_t &output, const rect_t &desktop) {
    return {output.x - desktop.x, output.y - desktop.y, output.width, output.height};
  }
}  // namespace output_layout
