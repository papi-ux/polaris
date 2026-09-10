/** @file src/video_rate.h
 * Exact stream rates, independent of launch display rates and Warp budgets.
 */
#pragma once

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <numeric>

extern "C" {
#include <libavutil/rational.h>
}

namespace video::rate {
  inline bool valid(AVRational fps) { return fps.num > 0 && fps.den > 0; }

  inline AVRational fraction(int numerator, int denominator) {
    if (numerator <= 0 || denominator <= 0) return {0, 1};
    const auto divisor = std::gcd(numerator, denominator);
    return {numerator / divisor, denominator / divisor};
  }

  inline AVRational from_millihertz(int millihertz) {
    return fraction(millihertz, 1000);
  }

  inline AVRational from_hundredths(int hundredths) {
    if (hundredths <= 0) return {0, 1};
    if (hundredths == 2397 || hundredths == 2398) return {24000, 1001};
    if (hundredths % 2997 == 0) {
      const auto numerator = static_cast<std::int64_t>(hundredths / 2997) * 30000;
      if (numerator <= std::numeric_limits<int>::max()) return {static_cast<int>(numerator), 1001};
    }
    return fraction(hundredths, 100);
  }

  inline AVRational from_wire(int max_fps, int client_refresh_x100) {
    if (max_fps <= 0) return {0, 1};
    // Polaris/Nova encode explicit millihertz above the existing 4000 FPS
    // Warp boundary. These requests stay exact, including 59940/1000.
    if (max_fps > 4000) return from_millihertz(max_fps);
    // The display hint may refine an integer request only within its rounding
    // interval. A 120 Hz display never changes a 60 FPS stream to 120 FPS.
    if (client_refresh_x100 > 0 &&
        std::abs(static_cast<std::int64_t>(client_refresh_x100) - static_cast<std::int64_t>(max_fps) * 100) < 50) {
      return from_hundredths(client_refresh_x100);
    }
    return {max_fps, 1};
  }

  inline std::chrono::nanoseconds interval(AVRational fps) {
    if (!valid(fps)) return std::chrono::nanoseconds::zero();
    return std::chrono::nanoseconds {static_cast<std::int64_t>(fps.den) * 1'000'000'000 / fps.num};
  }
}
