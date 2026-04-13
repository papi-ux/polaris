/**
 * @file src/ai_optimizer.h
 * @brief AI-powered streaming optimization using Claude API.
 *
 * Provides intelligent device + game optimization by calling the Claude API
 * with device specs and game metadata. Results are cached aggressively
 * (1 week TTL) so the API is only called once per device+game combination.
 *
 * Supports two authentication methods:
 * 1. Direct API key (from console.anthropic.com)
 * 2. Claude subscription proxy (via local Claude Code session)
 */
#pragma once

#include "device_db.h"

#include <optional>
#include <string>

namespace ai_optimizer {

  /**
   * @brief Configuration for the AI optimizer.
   */
  struct config_t {
    bool enabled = false;
    std::string api_key;             ///< Anthropic API key (sk-ant-...)
    bool use_subscription = false;   ///< Use Claude subscription instead of API key
    int timeout_ms = 5000;           ///< Max wait for API response
    int cache_ttl_hours = 168;       ///< Cache TTL (default 1 week)
  };

  /**
   * @brief Initialize the AI optimizer (load cache, validate config).
   */
  void init(const config_t &config);

  /**
   * @brief Check if the AI optimizer is enabled and has valid credentials.
   */
  bool is_enabled();

  /**
   * @brief Get a cached optimization for a device+game pair.
   * Returns nullopt if no cached result exists.
   */
  std::optional<device_db::optimization_t> get_cached(
    const std::string &device_name, const std::string &app_name);

  /**
   * @brief Session quality history for a device+game pair.
   * Fed back to the AI to improve future recommendations.
   */
  struct session_history_t {
    double avg_fps = 0;
    double avg_latency_ms = 0;
    int avg_bitrate_kbps = 0;
    int packet_loss_pct = 0;
    std::string quality_grade;  // A/B/C/D/F
    std::string codec;          // h264/hevc/av1
    int session_count = 0;
  };

  /**
   * @brief Request an AI optimization asynchronously.
   * Fires a background thread that calls the Claude API and caches the result.
   * Does NOT block the caller.
   */
  void request_async(const std::string &device_name,
                     const std::string &app_name,
                     const std::string &gpu_info,
                     const std::string &game_category = "",
                     const std::optional<session_history_t> &history = std::nullopt);

  /**
   * @brief Request an AI optimization synchronously (for manual triggers).
   * Blocks until the API responds or times out.
   */
  std::optional<device_db::optimization_t> request_sync(
    const std::string &device_name,
    const std::string &app_name,
    const std::string &gpu_info,
    const std::string &game_category = "",
    const std::optional<session_history_t> &history = std::nullopt);

  /**
   * @brief Record session quality for feedback loop.
   */
  void record_session(const std::string &device_name,
                      const std::string &app_name,
                      const session_history_t &session);

  /**
   * @brief Get session history for a device+game pair.
   */
  std::optional<session_history_t> get_session_history(
    const std::string &device_name, const std::string &app_name);

  /**
   * @brief Get AI status as JSON string.
   */
  std::string get_status_json();

  /**
   * @brief Get all cached optimizations as JSON string.
   */
  std::string get_cache_json();

  /**
   * @brief Get all session history as JSON string.
   */
  std::string get_history_json();

  /**
   * @brief Clear the optimization cache.
   */
  void clear_cache();

}  // namespace ai_optimizer
