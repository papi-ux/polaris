/**
 * @file src/platform/linux/output_layout.h
 * @brief Where each output sits on the desktop, and where absolute input places a capture of one.
 *
 * A compositor lays its outputs out in logical units: an output turned a quarter is as tall on the
 * desktop as its mode is wide, and one scaled by two covers half its mode each way. A capture of
 * that output still hands back its mode in output pixels, so the two units meet here and nowhere
 * else. Absolute input is placed across every output together, in desktop pixels: logical units
 * times the largest whole scale among the outputs, so the most detailed monitor keeps every one
 * of its pixels within reach of the pointer.
 */
#pragma once

#include <algorithm>
#include <cstdint>
#include <optional>
#include <ostream>
#include <span>
#include <utility>
#include <vector>

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

  /**
   * @brief Desktop pixels per logical unit: the largest whole scale among the outputs, and at
   *        least one.
   *
   * Absolute pointer devices take whole units. Counted in logical units, a pointer on a scale 2
   * monitor could only land on every other pixel. Counted in these, a lone scale 2 monitor
   * measures what its mode does, and every other monitor at least that finely.
   */
  constexpr std::int32_t desktop_scale(std::span<const output_t> outputs) {
    std::int32_t scale = 1;
    for (const auto &output : outputs) {
      scale = std::max(scale, output.scale);
    }
    return scale;
  }

  /**
   * @brief The output's rectangle on the desktop, in desktop pixels.
   */
  constexpr rect_t desktop_rect(const output_t &output, std::int32_t scale) {
    const auto logical = logical_rect(output);
    return {logical.x * scale, logical.y * scale, logical.width * scale, logical.height * scale};
  }

  /**
   * @brief The desktop absolute input spans.
   */
  struct desktop_t {
    /// Every output together, in desktop pixels.
    rect_t rect;
    /// Desktop pixels per logical unit.
    std::int32_t scale = 1;
  };

  /**
   * @brief The desktop the outputs make together, in desktop pixels.
   */
  inline desktop_t measure_desktop(std::span<const output_t> outputs) {
    const auto scale = desktop_scale(outputs);
    std::vector<rect_t> rects;
    rects.reserve(outputs.size());
    for (const auto &output : outputs) {
      rects.emplace_back(desktop_rect(output, scale));
    }
    return {bounds(rects), scale};
  }

  /**
   * @brief Where absolute input places a capture of the output: its rectangle in desktop pixels,
   *        counted from the desktop's corner.
   */
  constexpr rect_t input_rect(const output_t &output, const desktop_t &desktop) {
    return on_desktop(desktop_rect(output, desktop.scale), desktop.rect);
  }

  /**
   * @brief One active CRTC as KMS capture finds it, with the Wayland output on its connector.
   */
  struct crtc_output_t {
    /// The CRTC's mode, placed at Wayland's position for its output when one matched and at the
    /// CRTC's own otherwise. Empty for a CRTC no connector named, which has no place on the desktop.
    rect_t crtc;
    /// What Wayland said about the output on this CRTC's connector, when one matched it.
    std::optional<output_t> wayland;
  };

  /**
   * @brief The desktop KMS capture places absolute input on: every active CRTC together.
   */
  struct crtc_desktop_t: desktop_t {
    /// Measured by Wayland's rectangles, in desktop pixels. Otherwise by each CRTC's, in the CRTC's
    /// pixels, which is what KMS capture always measured by.
    bool by_wayland = false;
  };

  /**
   * @brief The desktop the active CRTCs make together.
   *
   * Wayland's rectangles are turned and scaled and a CRTC's mode is not, so the two never share a
   * desktop. Wayland's are taken only when Wayland matched an output to every active CRTC; one CRTC
   * without keeps the whole desktop in CRTC rectangles, where a rotated or scaled monitor is
   * measured wrong but every monitor is measured the same way.
   */
  inline crtc_desktop_t measure_crtc_desktop(std::span<const crtc_output_t> crtcs) {
    const bool by_wayland = !crtcs.empty() && std::ranges::all_of(crtcs, [](const crtc_output_t &crtc) {
                              return crtc.wayland.has_value();
                            });

    if (by_wayland) {
      std::vector<output_t> outputs;
      outputs.reserve(crtcs.size());
      for (const auto &crtc : crtcs) {
        outputs.emplace_back(*crtc.wayland);
      }
      return {measure_desktop(outputs), true};
    }

    std::vector<rect_t> rects;
    rects.reserve(crtcs.size());
    for (const auto &crtc : crtcs) {
      rects.emplace_back(crtc.crtc);
    }
    return {{bounds(rects), 1}, false};
  }

  /**
   * @brief Where absolute input places a KMS capture of one CRTC, counted from the desktop's corner
   *        like every other. On a desktop of CRTC rectangles the size stays zero, so input maps
   *        onto the frame, as it always did there.
   */
  constexpr rect_t crtc_input_rect(const crtc_output_t &crtc, const crtc_desktop_t &desktop) {
    if (desktop.by_wayland && crtc.wayland) {
      return input_rect(*crtc.wayland, desktop);
    }
    return {crtc.crtc.x - desktop.rect.x, crtc.crtc.y - desktop.rect.y, 0, 0};
  }
}  // namespace output_layout
