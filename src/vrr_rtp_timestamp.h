/** @file src/vrr_rtp_timestamp.h
 * Signed source-clock deltas, rounded to 90 kHz with protocol wrap preserved.
 */
#pragma once

#include <cstdint>
#include <limits>
#include <optional>

namespace vrr_pacing {
  inline constexpr std::optional<std::uint32_t> rtp_timestamp(std::int64_t source_ns,
                                                            std::int64_t epoch_ns) {
    if ((epoch_ns > 0 && source_ns < std::numeric_limits<std::int64_t>::min() + epoch_ns) ||
        (epoch_ns < 0 && source_ns > std::numeric_limits<std::int64_t>::max() + epoch_ns)) {
      return std::nullopt;
    }
    const auto delta = source_ns - epoch_ns;
    const auto magnitude = delta < 0 ? std::uint64_t(-(delta + 1)) + 1 : std::uint64_t(delta);
    // 90000 / 1e9 = 9 / 100000. Splitting first avoids multiplication overflow.
    auto ticks = (magnitude / 100000) * 9 + (magnitude % 100000) * 9 / 100000;
    const auto remainder = (magnitude % 100000) * 9 % 100000;
    if (remainder > 50000 || (remainder == 50000 && (ticks & 1))) ++ticks;
    const auto modular = static_cast<std::uint32_t>(ticks);
    return delta < 0 ? std::uint32_t(0) - modular : modular;
  }
}  // namespace vrr_pacing
