/** @file src/platform/linux/pipewire_rate.h
 * PipeWire frame-rate negotiation and delivery policy.
 */
#pragma once
#include "src/video_rate.h"
#include <algorithm>
#include <charconv>
#include <string_view>
#include <spa/param/video/format.h>
#include <spa/pod/builder.h>

namespace pipewire_capture {
  inline bool kwin_uses_fixed_rate(std::string_view support_information) {
    constexpr std::string_view label = "KWin version: ";
    const auto found = support_information.find(label);
    if (found == std::string_view::npos) return false;
    auto version = support_information.substr(found + label.size());
    int parts[3] {};
    for (int i = 0; i < 3; ++i) {
      const auto parsed = std::from_chars(version.data(), version.data() + version.size(), parts[i]);
      if (parsed.ec != std::errc {} || parsed.ptr == version.data() || parts[i] < 0) return false;
      version.remove_prefix(parsed.ptr - version.data());
      if (i < 2) {
        if (version.empty() || version.front() != '.') return false;
        version.remove_prefix(1);
      }
    }
    // KWin 5.x-6.7.x needs variable capture; 6.8 fixes the fixed-rate path.
    return parts[0] > 6 || (parts[0] == 6 && parts[1] >= 8);
  }

  inline void append_rate_properties(spa_pod_builder *builder, AVRational requested,
                                     bool fixed_rate, bool use_max_framerate,
                                     bool permit_fixed_rate = false) {
    if (!video::rate::valid(requested)) return;
    const spa_fraction variable {0, 1};
    const spa_fraction target {static_cast<uint32_t>(requested.num), static_cast<uint32_t>(requested.den)};
    const auto preferred = fixed_rate ? target : variable;
    const spa_fraction maximum {static_cast<uint32_t>(std::max<int64_t>(1000, (requested.num + requested.den - 1LL) / requested.den)), 1};
    if (permit_fixed_rate) {
      // Appended after the original offers: fixed-only producers cannot match
      // literal framerate=0/1, even if they ignore maxFramerate. Do not replace
      // the primary offer with this range; producer defaults could then choose
      // fixed capture when variable capture is available.
      spa_pod_builder_add(builder,
        SPA_FORMAT_VIDEO_framerate, SPA_POD_CHOICE_RANGE_Fraction(&preferred, &variable, &maximum), 0);
    } else if (use_max_framerate) {
      spa_pod_builder_add(builder,
        SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&variable),
        SPA_FORMAT_VIDEO_maxFramerate, SPA_POD_CHOICE_RANGE_Fraction(&preferred, &variable, &maximum), 0);
    } else {
      spa_pod_builder_add(builder, SPA_FORMAT_VIDEO_framerate, SPA_POD_Fraction(&preferred), 0);
    }
  }

  inline AVRational negotiated_capture_rate(spa_fraction framerate, spa_fraction maximum) {
    // maxFramerate governs variable framerate=0/1. Older producers populate
    // only framerate; missing or malformed rates remain unknown/variable.
    const auto selected = framerate.num != 0 && framerate.denom != 0 ? framerate : maximum;
    if (selected.num > INT32_MAX || selected.denom == 0 || selected.denom > INT32_MAX) return {0, 1};
    return {static_cast<int>(selected.num), static_cast<int>(selected.denom)};
  }

  inline bool requires_host_pacing(AVRational requested, AVRational negotiated) {
    return video::rate::valid(requested) &&
           (!video::rate::valid(negotiated) || av_cmp_q(negotiated, requested) > 0);
  }
}
