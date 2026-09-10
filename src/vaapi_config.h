/** @file src/vaapi_config.h
 * Portable VA-API configuration values. Driver constants stay in the backend.
 */
#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <optional>
#include <string_view>
#include <utility>

namespace config::vaapi {
  enum class quality_e { automatic, speed, balanced, quality };
  enum class rc_e { automatic, avbr, cbr, cqp, icq, qvbr, vbr };

  struct settings_t {
    bool strict_rc_buffer {false};
    quality_e quality {quality_e::automatic};
    rc_e rc {rc_e::automatic};
    std::optional<bool> blbrc; // Unset preserves the codec's existing default.
  };

  // A complete immutable value is published after a successful config write.
  // Session initialization reads once; an active codec is never reconfigured.
  inline std::atomic<uint32_t> published_settings {0};
  inline void publish(const settings_t &settings) {
    published_settings.store(static_cast<uint32_t>(settings.quality) |
      (static_cast<uint32_t>(settings.rc) << 2) |
      (static_cast<uint32_t>(settings.strict_rc_buffer) << 5) |
      (static_cast<uint32_t>(settings.blbrc ? (*settings.blbrc ? 2 : 1) : 0) << 6));
  }
  inline settings_t snapshot() {
    const auto value = published_settings.load();
    settings_t result;
    result.quality = static_cast<quality_e>(value & 3);
    result.rc = static_cast<rc_e>((value >> 2) & 7);
    result.strict_rc_buffer = (value & (1 << 5)) != 0;
    const auto block = (value >> 6) & 3;
    if (block != 0) result.blbrc = block == 2;
    return result;
  }

  inline constexpr std::array quality_names {
    std::pair {std::string_view {"auto"}, quality_e::automatic},
    std::pair {std::string_view {"speed"}, quality_e::speed},
    std::pair {std::string_view {"balanced"}, quality_e::balanced},
    std::pair {std::string_view {"quality"}, quality_e::quality},
  };
  inline constexpr std::array rc_names {
    std::pair {std::string_view {"auto"}, rc_e::automatic},
    std::pair {std::string_view {"avbr"}, rc_e::avbr},
    std::pair {std::string_view {"cbr"}, rc_e::cbr},
    std::pair {std::string_view {"cqp"}, rc_e::cqp},
    std::pair {std::string_view {"icq"}, rc_e::icq},
    std::pair {std::string_view {"qvbr"}, rc_e::qvbr},
    std::pair {std::string_view {"vbr"}, rc_e::vbr},
  };

  template<class T, std::size_t N>
  inline std::optional<T> parse(std::string_view value, const std::array<std::pair<std::string_view, T>, N> &names) {
    for (const auto &[name, option] : names) {
      if (value == name) return option;
    }
    return std::nullopt;
  }
}
