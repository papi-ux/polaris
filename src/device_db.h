/**
 * @file src/device_db.h
 * @brief Device knowledge base for automatic streaming optimization.
 *
 * Contains curated profiles for known streaming client devices (handhelds,
 * phones, tablets, desktops) with optimal streaming settings. When a client
 * connects, the device name is fuzzy-matched against the database to
 * auto-configure resolution, codec, bitrate, and encoder settings.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace device_db {

  /**
   * @brief Known device profile with optimal streaming settings.
   */
  struct device_t {
    std::string type;              ///< "handheld", "phone", "tablet", "desktop"
    std::string display_mode;      ///< Optimal "WxHxFPS" (e.g., "1920x1080x60")
    std::string preferred_codec;   ///< "hevc", "av1", "h264"
    int ideal_bitrate_kbps = 0;    ///< Recommended bitrate
    int color_range = 0;           ///< 0=client, 1=limited, 2=full
    bool hdr_capable = false;      ///< Device supports HDR
    bool virtual_display = true;   ///< Should use virtual display
    int nvenc_tune = 3;            ///< 1=quality, 2=low-latency, 3=ultra-low-latency
    std::string notes;             ///< Human-readable description
  };

  /**
   * @brief Optimization result that can be applied to a streaming session.
   */
  struct optimization_t {
    std::optional<std::string> display_mode;
    std::optional<int> color_range;
    std::optional<bool> hdr;
    std::optional<bool> virtual_display;
    std::optional<int> target_bitrate_kbps;
    std::optional<int> nvenc_tune;
    std::optional<std::string> preferred_codec;  ///< "h264", "hevc", or "av1"
    std::string reasoning;         ///< Human-readable explanation
    std::string source;            ///< "device_db", "ai_cached", "ai_live"
    std::string cache_status;      ///< "hit", "miss", "fallback", "invalidated"
    std::string confidence;        ///< "high", "medium", "low"
    std::string normalization_reason;  ///< Explanation for any server-side normalization
    std::vector<std::string> signals_used;  ///< Main inputs that shaped the recommendation
    int recommendation_version = 0;
    int64_t generated_at = 0;
    int64_t expires_at = 0;
  };

  /**
   * @brief Load device database from appdata or use embedded defaults.
   */
  void load();

  /**
   * @brief Look up a device by name (fuzzy matching).
   * Tries exact match first, then case-insensitive substring match.
   */
  std::optional<device_t> get_device(const std::string &name);

  /**
   * @brief Resolve a device name or alias to the preferred canonical device key.
   */
  std::string canonicalize_name(const std::string &name);

  /**
   * @brief Resolve a known device alias to a friendlier UI label.
   *
   * Returns the original string when the device name is not a known exact alias.
   */
  std::string friendly_name(const std::string &name);

  /**
   * @brief Get optimization settings for a device + app combination.
   */
  optimization_t get_optimization(const std::string &device_name,
                                   const std::string &app_name = "");

  /**
   * @brief Adjust an optimization-suggested codec to fit the active runtime.
   *
   * This is used to keep API recommendations and launch-time session policy
   * aligned when a codec suggestion is technically valid but not a good fit
   * for the current capture stack.
   */
  std::optional<std::string> normalize_preferred_codec(
    const std::string &device_name,
    const std::string &app_name,
    const std::optional<std::string> &preferred_codec,
    const std::optional<int> &target_bitrate_kbps,
    bool hdr_requested
  );

  /**
   * @brief Get all devices as JSON string.
   */
  std::string get_all_devices_json();

  /**
   * @brief Save/update a device in the database.
   */
  void save_device(const std::string &name, const device_t &device);

}  // namespace device_db
