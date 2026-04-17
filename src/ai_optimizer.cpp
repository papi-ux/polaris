/**
 * @file src/ai_optimizer.cpp
 * @brief Provider-agnostic API integration for streaming optimization.
 */

#include "ai_optimizer.h"
#include "config.h"
#include "game_classifier.h"
#include "logging.h"
#include "platform/common.h"

#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <future>
#include <mutex>
#include <sstream>
#include <thread>
#include <atomic>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

using namespace std::literals;

namespace ai_optimizer {

  static config_t cfg;
  static std::mutex cache_mutex;
  static std::mutex history_mutex;
  static std::mutex inflight_mutex;
  static std::mutex model_catalog_mutex;
  static std::atomic<int> in_flight_requests {0};

  namespace {
    constexpr const char *PROVIDER_ANTHROPIC = "anthropic";
    constexpr const char *PROVIDER_OPENAI = "openai";
    constexpr const char *PROVIDER_GEMINI = "gemini";
    constexpr const char *PROVIDER_LOCAL = "local";

    constexpr const char *AUTH_API_KEY = "api_key";
    constexpr const char *AUTH_SUBSCRIPTION = "subscription";
    constexpr const char *AUTH_NONE = "none";

    constexpr const char *DEFAULT_ANTHROPIC_MODEL = "claude-haiku-4-5-20251001";
    constexpr const char *DEFAULT_OPENAI_MODEL = "gpt-5.4-mini";
    constexpr const char *DEFAULT_GEMINI_MODEL = "gemini-2.5-flash";
    constexpr const char *DEFAULT_LOCAL_MODEL = "gpt-oss";

    constexpr const char *DEFAULT_ANTHROPIC_BASE_URL = "https://api.anthropic.com";
    constexpr const char *DEFAULT_OPENAI_BASE_URL = "https://api.openai.com/v1";
    constexpr const char *DEFAULT_GEMINI_BASE_URL = "https://generativelanguage.googleapis.com/v1beta/openai";
    constexpr const char *DEFAULT_LOCAL_BASE_URL = "http://127.0.0.1:11434/v1";

    constexpr int OPTIMIZATION_SCHEMA_VERSION = 2;
    constexpr int LOW_CONFIDENCE_TTL_HOURS = 24;
    constexpr int MEDIUM_CONFIDENCE_TTL_HOURS = 72;
    constexpr int RECOVERY_TTL_HOURS = 12;
    constexpr int MODEL_DISCOVERY_TTL_SECONDS = 300;
  }  // namespace

  struct cache_entry_t {
    std::string provider;
    std::string model;
    std::string base_url;
    std::string device_name;
    std::string app_name;
    device_db::optimization_t optimization;
    int64_t timestamp;  // Unix epoch seconds
  };

  struct provider_runtime_status_t {
    std::int64_t last_success_at = 0;
    std::int64_t last_failure_at = 0;
    long last_latency_ms = 0;
    std::string last_error;
  };

  struct model_catalog_cache_entry_t {
    std::string payload;
    std::int64_t cached_at = 0;
  };

  static void load_history();
  static void save_history();

  struct http_response_t {
    CURLcode curl_code = CURLE_OK;
    long http_code = 0;
    std::string body;
  };

  static std::unordered_map<std::string, cache_entry_t> cache;
  static std::unordered_map<std::string, session_history_t> session_history;
  static std::unordered_map<std::string, std::shared_future<std::optional<device_db::optimization_t>>> inflight_requests_by_key;
  static std::unordered_map<std::string, model_catalog_cache_entry_t> model_catalog_cache;
  static provider_runtime_status_t provider_runtime_status;

  static std::string to_lower_copy(std::string value) {
    for (char &ch : value) {
      ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
  }

  static std::string trim_trailing_slashes(std::string value) {
    while (!value.empty() && value.back() == '/') {
      value.pop_back();
    }
    return value;
  }

  static std::string normalize_provider(std::string provider) {
    provider = to_lower_copy(provider);
    if (provider == "claude" || provider == "anthropic") return PROVIDER_ANTHROPIC;
    if (provider == "openai") return PROVIDER_OPENAI;
    if (provider == "google" || provider == "google-ai" || provider == "gemini") return PROVIDER_GEMINI;
    if (provider == "ollama" || provider == "lmstudio" || provider == "lm-studio" || provider == "openai_compatible" || provider == "local") {
      return PROVIDER_LOCAL;
    }
    return PROVIDER_ANTHROPIC;
  }

  static std::string default_model_for_provider(const std::string &provider) {
    if (provider == PROVIDER_OPENAI) return DEFAULT_OPENAI_MODEL;
    if (provider == PROVIDER_GEMINI) return DEFAULT_GEMINI_MODEL;
    if (provider == PROVIDER_LOCAL) return DEFAULT_LOCAL_MODEL;
    return DEFAULT_ANTHROPIC_MODEL;
  }

  static std::string default_base_url_for_provider(const std::string &provider) {
    if (provider == PROVIDER_OPENAI) return DEFAULT_OPENAI_BASE_URL;
    if (provider == PROVIDER_GEMINI) return DEFAULT_GEMINI_BASE_URL;
    if (provider == PROVIDER_LOCAL) return DEFAULT_LOCAL_BASE_URL;
    return DEFAULT_ANTHROPIC_BASE_URL;
  }

  static std::string normalize_auth_mode(std::string auth_mode, const std::string &provider, bool legacy_subscription, const std::string &api_key) {
    auth_mode = to_lower_copy(auth_mode);

    if (auth_mode.empty()) {
      if (legacy_subscription && provider == PROVIDER_ANTHROPIC) {
        return AUTH_SUBSCRIPTION;
      }
      if (provider == PROVIDER_LOCAL && api_key.empty()) {
        return AUTH_NONE;
      }
      return AUTH_API_KEY;
    }

    if (auth_mode == "subscription" || auth_mode == "cli" || auth_mode == "claude_cli") {
      return provider == PROVIDER_ANTHROPIC ? AUTH_SUBSCRIPTION : (provider == PROVIDER_LOCAL && api_key.empty() ? AUTH_NONE : AUTH_API_KEY);
    }
    if (auth_mode == "none" || auth_mode == "disabled") {
      return AUTH_NONE;
    }
    return AUTH_API_KEY;
  }

  static std::string normalize_base_url(const std::string &provider, std::string base_url) {
    base_url = trim_trailing_slashes(base_url);
    if (!base_url.empty()) {
      return base_url;
    }
    return default_base_url_for_provider(provider);
  }

  static std::string normalize_model(const std::string &provider, std::string model) {
    if (!model.empty()) {
      return model;
    }
    return default_model_for_provider(provider);
  }

  static std::string canonical_device_name(const std::string &device) {
    return device_db::canonicalize_name(device);
  }

  static std::string history_key(const std::string &device, const std::string &app) {
    return canonical_device_name(device) + ":" + app;
  }

  static std::string cache_key(const std::string &provider,
                               const std::string &model,
                               const std::string &base_url,
                               const std::string &device,
                               const std::string &app) {
    return std::to_string(OPTIMIZATION_SCHEMA_VERSION) + "\t" +
      provider + "\t" + model + "\t" + base_url + "\t" + canonical_device_name(device) + "\t" + app;
  }

  static std::string current_cache_key(const std::string &device, const std::string &app) {
    return cache_key(cfg.provider, cfg.model, cfg.base_url, device, app);
  }

  static std::string current_cache_key(const config_t &active_cfg, const std::string &device, const std::string &app) {
    return cache_key(active_cfg.provider, active_cfg.model, active_cfg.base_url, device, app);
  }

  static std::string join_url(const std::string &base_url, const std::string &path) {
    return trim_trailing_slashes(base_url) + path;
  }

  static std::int64_t unix_time_now() {
    return std::chrono::duration_cast<std::chrono::seconds>(
      std::chrono::system_clock::now().time_since_epoch()).count();
  }

  static std::string hashed_secret_fragment(const std::string &value) {
    if (value.empty()) {
      return "none";
    }
    return std::to_string(std::hash<std::string> {}(value));
  }

  static std::string model_catalog_key(const config_t &active_cfg) {
    return active_cfg.provider + "\t" +
      active_cfg.auth_mode + "\t" +
      active_cfg.base_url + "\t" +
      hashed_secret_fragment(active_cfg.api_key);
  }

  static int effective_ttl_hours(const cache_entry_t &entry, int base_ttl_hours = 0) {
    auto ttl_hours = base_ttl_hours > 0 ? base_ttl_hours : cfg.cache_ttl_hours;
    const auto confidence = to_lower_copy(entry.optimization.confidence);
    if (confidence == "low") {
      ttl_hours = std::min(ttl_hours, LOW_CONFIDENCE_TTL_HOURS);
    } else if (confidence == "medium") {
      ttl_hours = std::min(ttl_hours, MEDIUM_CONFIDENCE_TTL_HOURS);
    }
    return std::max(ttl_hours, 1);
  }

  static void push_signal(std::vector<std::string> &signals, const std::string &signal) {
    if (signal.empty() || std::find(signals.begin(), signals.end(), signal) != signals.end()) {
      return;
    }
    signals.push_back(signal);
  }

  static std::string latest_quality_grade(const session_history_t &history) {
    return history.last_quality_grade.empty() ? history.quality_grade : history.last_quality_grade;
  }

  static std::string latest_codec(const session_history_t &history) {
    return history.last_codec.empty() ? history.codec : history.last_codec;
  }

  static config_t resolved_config(config_t config) {
    config.provider = normalize_provider(config.provider);
    config.model = normalize_model(config.provider, config.model);
    config.base_url = normalize_base_url(config.provider, config.base_url);
    config.auth_mode = normalize_auth_mode(config.auth_mode, config.provider, config.use_subscription, config.api_key);
    if (config.provider != PROVIDER_ANTHROPIC) {
      config.use_subscription = false;
    }
    return config;
  }

  static bool is_config_enabled(const config_t &active_cfg) {
    if (!active_cfg.enabled || active_cfg.model.empty()) return false;
    if (active_cfg.provider == PROVIDER_ANTHROPIC) {
      return active_cfg.auth_mode == AUTH_SUBSCRIPTION || (active_cfg.auth_mode == AUTH_API_KEY && !active_cfg.api_key.empty());
    }
    if (active_cfg.auth_mode == AUTH_NONE) {
      return true;
    }
    return !active_cfg.api_key.empty();
  }

  // ---- Cache persistence ----

  static std::filesystem::path cache_path() {
    return platf::appdata() / "ai_optimization_cache.json";
  }

  static session_history_t merge_history_entries(const session_history_t &lhs, const session_history_t &rhs) {
    if (lhs.session_count <= 0) {
      return rhs;
    }
    if (rhs.session_count <= 0) {
      return lhs;
    }

    session_history_t merged = lhs;
    const double lhs_count = static_cast<double>(lhs.session_count);
    const double rhs_count = static_cast<double>(rhs.session_count);
    const double total_count = lhs_count + rhs_count;

    merged.avg_fps = (lhs.avg_fps * lhs_count + rhs.avg_fps * rhs_count) / total_count;
    merged.avg_latency_ms = (lhs.avg_latency_ms * lhs_count + rhs.avg_latency_ms * rhs_count) / total_count;
    merged.avg_bitrate_kbps = static_cast<int>(std::lround((lhs.avg_bitrate_kbps * lhs_count + rhs.avg_bitrate_kbps * rhs_count) / total_count));
    merged.packet_loss_pct = (lhs.packet_loss_pct * lhs_count + rhs.packet_loss_pct * rhs_count) / total_count;
    merged.session_count = lhs.session_count + rhs.session_count;
    merged.poor_outcome_count = lhs.poor_outcome_count + rhs.poor_outcome_count;
    merged.last_invalidated_at = std::max(lhs.last_invalidated_at, rhs.last_invalidated_at);

    const auto &latest = rhs.last_updated_at >= lhs.last_updated_at ? rhs : lhs;
    merged.quality_grade = latest.quality_grade;
    merged.codec = latest.codec;
    merged.last_fps = latest.last_fps;
    merged.last_target_fps = latest.last_target_fps;
    merged.last_latency_ms = latest.last_latency_ms;
    merged.last_bitrate_kbps = latest.last_bitrate_kbps;
    merged.last_packet_loss_pct = latest.last_packet_loss_pct;
    merged.last_quality_grade = latest.last_quality_grade;
    merged.last_codec = latest.last_codec;
    merged.consecutive_poor_outcomes = latest.consecutive_poor_outcomes;
    merged.last_optimization_source = latest.last_optimization_source;
    merged.last_optimization_confidence = latest.last_optimization_confidence;
    merged.last_recommendation_version = latest.last_recommendation_version;
    merged.last_updated_at = latest.last_updated_at;
    return merged;
  }

  static void load_cache() {
    std::lock_guard<std::mutex> lock(cache_mutex);
    std::ifstream file(cache_path());
    if (!file.is_open()) return;

    try {
      auto root = nlohmann::json::parse(file);
      for (auto &[key, val] : root.items()) {
        cache_entry_t entry;
        entry.timestamp = val.value("timestamp", 0);
        entry.optimization.recommendation_version =
          val.value("recommendation_version", 0);
        entry.provider = normalize_provider(val.value("provider", PROVIDER_ANTHROPIC));
        entry.model = normalize_model(entry.provider, val.value("model", ""));
        entry.base_url = normalize_base_url(entry.provider, val.value("base_url", ""));
        entry.device_name = val.value("device_name", "");
        entry.app_name = val.value("app_name", "");

        if (entry.optimization.recommendation_version != OPTIMIZATION_SCHEMA_VERSION) {
          continue;
        }

        if (entry.device_name.empty()) {
          if (key.find('\t') != std::string::npos) {
            std::stringstream ss(key);
            std::getline(ss, entry.provider, '\t');
            std::getline(ss, entry.model, '\t');
            std::getline(ss, entry.base_url, '\t');
            std::getline(ss, entry.device_name, '\t');
            std::getline(ss, entry.app_name, '\t');
            entry.provider = normalize_provider(entry.provider);
            entry.model = normalize_model(entry.provider, entry.model);
            entry.base_url = normalize_base_url(entry.provider, entry.base_url);
          } else {
            auto pos = key.find(':');
            entry.device_name = pos == std::string::npos ? key : key.substr(0, pos);
            entry.app_name = pos == std::string::npos ? std::string {} : key.substr(pos + 1);
          }
        }
        entry.device_name = canonical_device_name(entry.device_name);

        auto &o = entry.optimization;
        if (val.contains("display_mode")) o.display_mode = val["display_mode"].get<std::string>();
        if (val.contains("color_range")) o.color_range = val["color_range"].get<int>();
        if (val.contains("hdr")) o.hdr = val["hdr"].get<bool>();
        if (val.contains("virtual_display")) o.virtual_display = val["virtual_display"].get<bool>();
        if (val.contains("target_bitrate_kbps")) o.target_bitrate_kbps = val["target_bitrate_kbps"].get<int>();
        if (val.contains("nvenc_tune")) o.nvenc_tune = val["nvenc_tune"].get<int>();
        if (val.contains("preferred_codec")) o.preferred_codec = val["preferred_codec"].get<std::string>();
        o.reasoning = val.value("reasoning", "");
        o.cache_status = val.value("cache_status", "hit");
        o.confidence = val.value("confidence", "");
        o.normalization_reason = val.value("normalization_reason", "");
        o.signals_used = val.value("signals_used", std::vector<std::string> {});
        o.generated_at = val.value("generated_at", entry.timestamp);
        o.expires_at = val.value("expires_at", 0);
        o.source = "ai_cached";
        auto normalized_key = cache_key(entry.provider, entry.model, entry.base_url, entry.device_name, entry.app_name);
        auto existing = cache.find(normalized_key);
        if (existing == cache.end() || entry.timestamp >= existing->second.timestamp) {
          cache[normalized_key] = entry;
        }
      }
      BOOST_LOG(info) << "ai_optimizer: Loaded "sv << cache.size() << " cached optimizations"sv;
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "ai_optimizer: Failed to load cache: "sv << e.what();
    }
  }

  // Save cache to disk — caller must already hold cache_mutex.
  static void save_cache_locked() {
    nlohmann::json root = nlohmann::json::object();
    for (const auto &[key, entry] : cache) {
      nlohmann::json val;
      val["timestamp"] = entry.timestamp;
      val["recommendation_version"] = entry.optimization.recommendation_version;
      val["provider"] = entry.provider;
      val["model"] = entry.model;
      val["base_url"] = entry.base_url;
      val["device_name"] = entry.device_name;
      val["app_name"] = entry.app_name;
      auto &o = entry.optimization;
      if (o.display_mode) val["display_mode"] = *o.display_mode;
      if (o.color_range) val["color_range"] = *o.color_range;
      if (o.hdr) val["hdr"] = *o.hdr;
      if (o.virtual_display) val["virtual_display"] = *o.virtual_display;
      if (o.target_bitrate_kbps) val["target_bitrate_kbps"] = *o.target_bitrate_kbps;
      if (o.nvenc_tune) val["nvenc_tune"] = *o.nvenc_tune;
      if (o.preferred_codec) val["preferred_codec"] = *o.preferred_codec;
      val["reasoning"] = o.reasoning;
      val["cache_status"] = o.cache_status;
      val["confidence"] = o.confidence;
      val["normalization_reason"] = o.normalization_reason;
      val["signals_used"] = o.signals_used;
      val["generated_at"] = o.generated_at;
      val["expires_at"] = o.expires_at;
      root[key] = val;
    }
    std::ofstream file(cache_path());
    if (file.is_open()) file << root.dump(2);
  }

  static bool parse_display_mode(const std::string &value, int &width, int &height, int &fps) {
    width = 0;
    height = 0;
    float fps_value = 0.0f;
    if (sscanf(value.c_str(), "%dx%dx%f", &width, &height, &fps_value) < 2) {
      return false;
    }
    if (width <= 0 || height <= 0) {
      return false;
    }
    if (fps_value <= 0.0f) {
      fps_value = 60.0f;
    }
    fps = static_cast<int>(std::round(fps_value));
    return fps > 0;
  }

  template<typename T>
  static T clamp_value(T value, T min_value, T max_value) {
    return std::max(min_value, std::min(value, max_value));
  }

  static device_db::optimization_t fallback_optimization_for_device(const std::string &device_name,
                                                                    const std::string &app_name) {
    auto fallback = device_db::get_optimization(device_name, app_name);
    if (!fallback.display_mode) {
      fallback.display_mode = "1280x720x60";
    }
    if (!fallback.target_bitrate_kbps) {
      fallback.target_bitrate_kbps = 15000;
    }
    if (!fallback.preferred_codec) {
      fallback.preferred_codec = "hevc";
    }
    if (!fallback.nvenc_tune) {
      fallback.nvenc_tune = 2;
    }
    return fallback;
  }

  static void update_provider_runtime_status(bool success, long latency_ms, const std::string &error = {}) {
    std::lock_guard<std::mutex> lock(inflight_mutex);
    provider_runtime_status.last_latency_ms = latency_ms;
    if (success) {
      provider_runtime_status.last_success_at = unix_time_now();
      provider_runtime_status.last_error.clear();
    } else {
      provider_runtime_status.last_failure_at = unix_time_now();
      provider_runtime_status.last_error = error;
    }
  }

  static std::string infer_confidence(const std::string &device_name,
                                      const std::string &game_category,
                                      const std::optional<session_history_t> &history,
                                      const device_db::optimization_t &fallback,
                                      bool normalized,
                                      bool from_cache) {
    auto confidence = fallback.source == "device_db" ? "high"s : "medium"s;
    if (fallback.source != "device_db" && device_name.empty()) {
      confidence = "low";
    }
    if (!game_category.empty() && game_category != "unknown") {
      confidence = confidence == "low" ? "medium" : confidence;
    }
    if (history) {
      if (history->poor_outcome_count > 0 || history->consecutive_poor_outcomes > 0) {
        confidence = "low";
      } else {
        const auto grade = latest_quality_grade(*history);
        if (grade == "A" || grade == "B") {
          confidence = from_cache ? "high" : confidence;
        }
      }
    }
    if (normalized) {
      confidence = confidence == "high" ? "medium" : "low";
    }
    return confidence;
  }

  static device_db::optimization_t normalize_optimization(const config_t &active_cfg,
                                                          const std::string &device_name,
                                                          const std::string &app_name,
                                                          const std::string &game_category,
                                                          const std::optional<session_history_t> &history,
                                                          device_db::optimization_t optimization,
                                                          bool from_cache) {
    const auto fallback = fallback_optimization_for_device(device_name, app_name);
    const auto now = unix_time_now();
    std::vector<std::string> normalization_notes;
    bool normalized = false;

    optimization.recommendation_version = OPTIMIZATION_SCHEMA_VERSION;
    optimization.generated_at = now;
    if (optimization.source.empty()) {
      optimization.source = "ai_live";
    }

    if (!optimization.display_mode || optimization.display_mode->empty()) {
      optimization.display_mode = fallback.display_mode;
      normalization_notes.push_back("Filled missing display mode from device defaults.");
      normalized = true;
    } else {
      int width = 0;
      int height = 0;
      int fps = 0;
      if (!parse_display_mode(*optimization.display_mode, width, height, fps)) {
        optimization.display_mode = fallback.display_mode;
        normalization_notes.push_back("Replaced invalid display mode with a safe baseline.");
        normalized = true;
      } else {
        width = clamp_value(width, 640, 7680);
        height = clamp_value(height, 360, 4320);
        fps = clamp_value(fps, 24, 240);
        const auto normalized_mode = std::to_string(width) + "x" + std::to_string(height) + "x" + std::to_string(fps);
        if (normalized_mode != *optimization.display_mode) {
          optimization.display_mode = normalized_mode;
          normalization_notes.push_back("Clamped display mode to a supported range.");
          normalized = true;
        }
      }
    }

    if (!optimization.target_bitrate_kbps) {
      optimization.target_bitrate_kbps = fallback.target_bitrate_kbps;
      normalization_notes.push_back("Filled missing bitrate from the baseline profile.");
      normalized = true;
    } else {
      const auto clamped = clamp_value(*optimization.target_bitrate_kbps, 2000, 100000);
      if (clamped != *optimization.target_bitrate_kbps) {
        optimization.target_bitrate_kbps = clamped;
        normalization_notes.push_back("Clamped bitrate into the supported streaming range.");
        normalized = true;
      }
    }

    if (optimization.color_range && (*optimization.color_range < 0 || *optimization.color_range > 2)) {
      optimization.color_range = fallback.color_range.value_or(0);
      normalization_notes.push_back("Reset color range to a supported value.");
      normalized = true;
    }

    if (!optimization.nvenc_tune) {
      optimization.nvenc_tune = fallback.nvenc_tune;
      normalized = true;
    } else {
      const auto clamped_tune = clamp_value(*optimization.nvenc_tune, 1, 3);
      if (clamped_tune != *optimization.nvenc_tune) {
        optimization.nvenc_tune = clamped_tune;
        normalization_notes.push_back("Clamped NVENC tune to a supported preset.");
        normalized = true;
      }
    }

    if (optimization.preferred_codec) {
      optimization.preferred_codec = to_lower_copy(*optimization.preferred_codec);
      if (*optimization.preferred_codec != "h264" &&
          *optimization.preferred_codec != "hevc" &&
          *optimization.preferred_codec != "av1") {
        optimization.preferred_codec = fallback.preferred_codec;
        normalization_notes.push_back("Replaced an unsupported codec choice with the baseline codec.");
        normalized = true;
      }
    } else {
      optimization.preferred_codec = fallback.preferred_codec;
      normalized = true;
    }

    if (optimization.source.find("ai_") == 0) {
      optimization.cache_status = from_cache ? "hit" : "miss";
    } else if (optimization.source == "device_db") {
      optimization.cache_status = "fallback";
    } else {
      optimization.cache_status = optimization.cache_status.empty() ? "fallback" : optimization.cache_status;
    }

    optimization.confidence = infer_confidence(
      device_name,
      game_category,
      history,
      fallback,
      normalized,
      from_cache
    );

    optimization.signals_used.clear();
    push_signal(optimization.signals_used, fallback.source == "device_db" ? "device_profile" : "safe_defaults");
    if (!game_category.empty() && game_category != "unknown") {
      push_signal(optimization.signals_used, "game_category");
    }
    if (history && history->session_count > 0) {
      push_signal(optimization.signals_used, "session_history");
    }
    if (config::video.linux_display.headless_mode) {
      push_signal(optimization.signals_used, "headless_mode");
    }
    if (config::video.adaptive_bitrate.enabled) {
      push_signal(optimization.signals_used, "adaptive_bitrate");
    }
    if (normalized) {
      push_signal(optimization.signals_used, "runtime_normalization");
    }

    optimization.expires_at = now + static_cast<std::int64_t>(
      effective_ttl_hours(
        {active_cfg.provider, active_cfg.model, active_cfg.base_url, device_name, app_name, optimization, now},
        active_cfg.cache_ttl_hours
      ) * 3600LL
    );

    if (!normalization_notes.empty()) {
      optimization.normalization_reason = normalization_notes.front();
      for (std::size_t index = 1; index < normalization_notes.size(); ++index) {
        optimization.normalization_reason += " " + normalization_notes[index];
      }
    }

    if (optimization.reasoning.empty()) {
      optimization.reasoning = fallback.reasoning;
    } else if (!optimization.normalization_reason.empty()) {
      optimization.reasoning += " " + optimization.normalization_reason;
    }

    return optimization;
  }

  // ---- libcurl helpers ----

  static size_t write_callback(void *contents, size_t size, size_t nmemb, std::string *out) {
    out->append(static_cast<char *>(contents), size * nmemb);
    return size * nmemb;
  }

  static http_response_t post_json(const std::string &url,
                                   const nlohmann::json &request_body,
                                   int timeout_ms,
                                   const std::vector<std::string> &header_values) {
    http_response_t response;

    CURL *curl = curl_easy_init();
    if (!curl) {
      response.curl_code = CURLE_FAILED_INIT;
      return response;
    }

    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    for (const auto &header_value : header_values) {
      headers = curl_slist_append(headers, header_value.c_str());
    }

    std::string post_data = request_body.dump();
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms));
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

    response.curl_code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return response;
  }

  static http_response_t get_json(const std::string &url,
                                  int timeout_ms,
                                  const std::vector<std::string> &header_values) {
    http_response_t response;

    CURL *curl = curl_easy_init();
    if (!curl) {
      response.curl_code = CURLE_FAILED_INIT;
      return response;
    }

    struct curl_slist *headers = nullptr;
    for (const auto &header_value : header_values) {
      headers = curl_slist_append(headers, header_value.c_str());
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(timeout_ms));
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

    response.curl_code = curl_easy_perform(curl);
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.http_code);

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);
    return response;
  }

  // ---- Prompt builder (shared by both API and CLI paths) ----

  static std::string build_system_prompt() {
    return "You are a game streaming optimization engine for Polaris (Linux NVIDIA game streaming server). "
      "ALWAYS respond with ONLY a valid JSON object — no explanation, no markdown, no code fences, no questions. "
      "Even if the device or game is unknown, return your best-guess defaults as JSON. "
      "Never ask for more information. Never refuse. "
      "Start from the known device baseline when one is provided, then adjust only where the game category, runtime mode, or session history suggests it. "
      "Consider: device screen resolution, refresh rate, codec decode capability, "
      "WiFi bandwidth, battery life, game performance requirements, and any session quality feedback. "
      "nvenc_tune: 1=quality (slow/cinematic games), 2=low-latency (action), 3=ultra-low-latency (competitive/VR). "
      "color_range: 0=client decides, 1=limited/MPEG, 2=full/JPEG. "
      "preferred_codec: 'hevc' for most devices (better quality/bitrate), 'h264' for legacy/low-power devices, 'av1' for newest devices with AV1 decode. "
      "If previous session data shows packet loss >2% or latency >30ms, reduce bitrate. "
      "If previous session scored A, keep current settings. If B/C, adjust. If D/F, significantly reduce quality.";
  }

  static std::string build_user_prompt(
      const std::string &device_name,
      const std::string &app_name,
      const std::string &gpu_info,
      const std::string &game_category = "",
      const std::optional<session_history_t> &history = std::nullopt) {
    auto device_info = device_db::get_device(device_name);
    auto device_baseline = fallback_optimization_for_device(device_name, app_name);
    std::string device_desc = device_info
      ? device_info->type + " — " + device_info->notes
      : "unknown device";

    std::string category_line;
    if (!game_category.empty() && game_category != "unknown") {
      auto cat = game_classifier::string_to_category(game_category);
      auto hint = game_classifier::category_hint(cat);
      if (!hint.empty()) {
        category_line = "Game category: " + hint + "\n";
      }
    }

    std::string history_line;
    if (history) {
      std::ostringstream history_stream;
      history_stream << "Previous session stats (feedback for improvement):\n";
      if (history->last_fps > 0 || history->last_latency_ms > 0 || !latest_quality_grade(*history).empty()) {
        history_stream << "  Latest session: target "
                       << static_cast<int>(std::round(history->last_target_fps > 0 ? history->last_target_fps : history->last_fps))
                       << " FPS, achieved " << static_cast<int>(std::round(history->last_fps))
                       << " FPS, latency " << static_cast<int>(std::round(history->last_latency_ms))
                       << "ms, bitrate " << history->last_bitrate_kbps
                       << "kbps, packet loss " << history->last_packet_loss_pct
                       << "%, grade " << latest_quality_grade(*history)
                       << ", codec " << latest_codec(*history) << "\n";
      }
      history_stream << "  Rolling history: " << history->session_count
                     << " sessions, Avg FPS " << static_cast<int>(std::round(history->avg_fps))
                     << ", Avg Latency " << static_cast<int>(std::round(history->avg_latency_ms))
                     << "ms, Avg Bitrate " << history->avg_bitrate_kbps
                     << "kbps, Avg Packet Loss " << history->packet_loss_pct
                     << "%, Poor Outcomes " << history->poor_outcome_count
                     << ", Last Confidence " << history->last_optimization_confidence << "\n";
      history_line = history_stream.str();
    }

    std::ostringstream baseline_line;
    baseline_line << "Known device baseline: "
                  << device_baseline.display_mode.value_or("1280x720x60")
                  << ", codec=" << device_baseline.preferred_codec.value_or("hevc")
                  << ", bitrate=" << device_baseline.target_bitrate_kbps.value_or(15000) << "kbps"
                  << ", virtual_display=" << (device_baseline.virtual_display.value_or(true) ? "true" : "false")
                  << ", hdr=" << (device_baseline.hdr.value_or(false) ? "true" : "false")
                  << ", reasoning=" << device_baseline.reasoning << "\n";

    std::string headless_line;
    if (config::video.linux_display.headless_mode) {
      headless_line = config::video.linux_display.use_cage_compositor
        ? "Streaming mode: HEADLESS via labwc hidden compositor. Polaris already provides the headless output, so prefer virtual_display=false unless a separate virtual display is explicitly required.\n"
        : "Streaming mode: HEADLESS (no physical monitor). Choose a display_mode that fits the client device and only enable virtual_display when the runtime explicitly needs it.\n";
    } else {
      headless_line = "Streaming mode: windowed (labwc as Wayland window on desktop)\n";
    }

    const std::string example_virtual_display =
      (config::video.linux_display.headless_mode && config::video.linux_display.use_cage_compositor) ? "false" : "true";

    std::string adaptive_line;
    if (config::video.adaptive_bitrate.enabled) {
      adaptive_line = "Adaptive bitrate: enabled (server auto-adjusts mid-stream on network degradation)\n";
    }

    return
      "Device: " + device_name + " (" + device_desc + ")\n"
      "Game: " + (app_name.empty() ? "Desktop/General" : app_name) + "\n"
      + category_line + baseline_line.str() + history_line + headless_line + adaptive_line +
      "GPU: " + gpu_info + "\n\n"
      "Respond with ONLY this JSON (fill in values, no other text):\n"
      "{\"display_mode\":\"WIDTHxHEIGHTxFPS\",\"color_range\":0,\"hdr\":false,"
      "\"virtual_display\":" + example_virtual_display + ",\"target_bitrate_kbps\":15000,\"nvenc_tune\":2,"
      "\"preferred_codec\":\"hevc\",\"reasoning\":\"brief explanation\"}";
  }

  static std::optional<device_db::optimization_t> parse_optimization_json(const std::string &text, const std::string &label) {
    auto json_start = text.find('{');
    auto json_end = text.rfind('}');
    if (json_start == std::string::npos || json_end == std::string::npos) {
      BOOST_LOG(error) << "ai_optimizer: No JSON in "sv << label << " response: "sv << text.substr(0, 100);
      return std::nullopt;
    }
    auto result_json = nlohmann::json::parse(text.substr(json_start, json_end - json_start + 1));

    device_db::optimization_t opt;
    if (result_json.contains("display_mode")) opt.display_mode = result_json["display_mode"].get<std::string>();
    if (result_json.contains("color_range")) opt.color_range = result_json["color_range"].get<int>();
    if (result_json.contains("hdr")) opt.hdr = result_json["hdr"].get<bool>();
    if (result_json.contains("virtual_display")) opt.virtual_display = result_json["virtual_display"].get<bool>();
    if (result_json.contains("target_bitrate_kbps")) opt.target_bitrate_kbps = result_json["target_bitrate_kbps"].get<int>();
    if (result_json.contains("nvenc_tune")) opt.nvenc_tune = result_json["nvenc_tune"].get<int>();
    if (result_json.contains("preferred_codec")) opt.preferred_codec = result_json["preferred_codec"].get<std::string>();
    opt.reasoning = result_json.value("reasoning", "AI-optimized");
    opt.source = "ai_live";
    opt.recommendation_version = OPTIMIZATION_SCHEMA_VERSION;

    BOOST_LOG(info) << "ai_optimizer: "sv << label << " returned: "sv << opt.display_mode.value_or("(none)")
                    << ", "sv << opt.target_bitrate_kbps.value_or(0) << "kbps — "sv << opt.reasoning;
    return opt;
  }

  static nlohmann::json optimization_response_format() {
    nlohmann::json schema;
    schema["type"] = "object";
    schema["additionalProperties"] = false;
    schema["properties"] = {
      {"display_mode", {{"type", "string"}, {"description", "Display mode in WIDTHxHEIGHTxFPS format, such as 1920x1080x60."}}},
      {"color_range", {{"type", "integer"}, {"enum", nlohmann::json::array({0, 1, 2})}}},
      {"hdr", {{"type", "boolean"}}},
      {"virtual_display", {{"type", "boolean"}}},
      {"target_bitrate_kbps", {{"type", "integer"}, {"minimum", 1000}, {"maximum", 200000}}},
      {"nvenc_tune", {{"type", "integer"}, {"enum", nlohmann::json::array({1, 2, 3})}}},
      {"preferred_codec", {{"type", "string"}, {"enum", nlohmann::json::array({"h264", "hevc", "av1"})}}},
      {"reasoning", {{"type", "string"}}},
    };
    schema["required"] = nlohmann::json::array({
      "display_mode",
      "color_range",
      "hdr",
      "virtual_display",
      "target_bitrate_kbps",
      "nvenc_tune",
      "preferred_codec",
      "reasoning"
    });

    return {
      {"type", "json_schema"},
      {"json_schema", {
        {"name", "polaris_optimization"},
        {"strict", true},
        {"schema", schema},
      }}
    };
  }

  static std::string anthropic_cli_model(const config_t &active_cfg) {
    auto model = to_lower_copy(active_cfg.model);
    if (model.find("haiku") != std::string::npos) return "haiku";
    if (model.find("sonnet") != std::string::npos) return "sonnet";
    if (model.find("opus") != std::string::npos) return "opus";
    return active_cfg.model.empty() ? "haiku" : active_cfg.model;
  }

  static std::string extract_anthropic_text(const nlohmann::json &api_response) {
    if (api_response.contains("content") && api_response["content"].is_array()) {
      for (const auto &block : api_response["content"]) {
        if (block.value("type", "") == "text") {
          return block.value("text", "");
        }
      }
    }
    return {};
  }

  static std::string extract_openai_compatible_text(const nlohmann::json &api_response) {
    if (!api_response.contains("choices") || !api_response["choices"].is_array() || api_response["choices"].empty()) {
      return {};
    }

    const auto &message = api_response["choices"][0].value("message", nlohmann::json::object());
    if (message.contains("content")) {
      if (message["content"].is_string()) {
        return message["content"].get<std::string>();
      }
      if (message["content"].is_array()) {
        for (const auto &part : message["content"]) {
          if (part.value("type", "") == "text") {
            return part.value("text", "");
          }
        }
      }
    }

    if (message.contains("refusal")) {
      BOOST_LOG(error) << "ai_optimizer: Provider refused optimization request: "sv << message["refusal"].dump();
    }

    return {};
  }

  static void append_unique_model(nlohmann::json &models,
                                  std::unordered_set<std::string> &seen_ids,
                                  const std::string &id,
                                  const std::string &label = "") {
    if (id.empty() || !seen_ids.emplace(id).second) {
      return;
    }

    nlohmann::json model;
    model["id"] = id;
    model["label"] = label.empty() ? id : label;
    models.push_back(model);
  }

  static nlohmann::json fallback_model_list(const config_t &active_cfg) {
    nlohmann::json models = nlohmann::json::array();
    std::unordered_set<std::string> seen_ids;

    append_unique_model(models, seen_ids, active_cfg.model, "Current selection");
    append_unique_model(models, seen_ids, default_model_for_provider(active_cfg.provider), "Provider default");

    if (active_cfg.provider == PROVIDER_ANTHROPIC) {
      append_unique_model(models, seen_ids, "claude-haiku-4-5-20251001");
      append_unique_model(models, seen_ids, "claude-sonnet-4-20250514");
      append_unique_model(models, seen_ids, "claude-opus-4-20250514");
    } else if (active_cfg.provider == PROVIDER_OPENAI) {
      append_unique_model(models, seen_ids, "gpt-5.4-mini");
      append_unique_model(models, seen_ids, "gpt-5.4");
      append_unique_model(models, seen_ids, "gpt-4.1-mini");
    } else if (active_cfg.provider == PROVIDER_GEMINI) {
      append_unique_model(models, seen_ids, "gemini-2.5-flash");
      append_unique_model(models, seen_ids, "gemini-2.5-pro");
    } else if (active_cfg.provider == PROVIDER_LOCAL) {
      append_unique_model(models, seen_ids, "gpt-oss");
      append_unique_model(models, seen_ids, "qwen3-8b");
    }

    return models;
  }

  // ---- Anthropic CLI (subscription mode) ----

  static std::optional<device_db::optimization_t> call_anthropic_cli(
      const config_t &active_cfg,
      const std::string &device_name,
      const std::string &app_name,
      const std::string &gpu_info,
      const std::string &game_category = "",
      const std::optional<session_history_t> &history = std::nullopt) {

    std::string prompt = build_user_prompt(device_name, app_name, gpu_info, game_category, history);
    std::string system = build_system_prompt();

    auto escape_sh = [](const std::string &s) {
      std::string out;
      for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
      }
      return out;
    };

    std::string claude_bin;
    for (const auto &path : {
      std::string(getenv("HOME") ? getenv("HOME") : "/home") + "/.local/bin/claude",
      std::string("/usr/local/bin/claude"),
      std::string("/usr/bin/claude"),
    }) {
      if (std::filesystem::exists(path)) {
        claude_bin = path;
        break;
      }
    }
    if (claude_bin.empty()) {
      claude_bin = "claude";
    }

    auto prompt_file = std::filesystem::temp_directory_path() / "polaris_ai_prompt.txt";
    auto system_file = std::filesystem::temp_directory_path() / "polaris_ai_system.txt";
    {
      std::ofstream pf(prompt_file);
      pf << prompt;
      std::ofstream sf(system_file);
      sf << system;
    }

    std::string cmd =
      "'" + claude_bin + "' --print "
      "--model '" + escape_sh(anthropic_cli_model(active_cfg)) + "' "
      "--system-prompt \"$(cat " + system_file.string() + ")\" "
      "< " + prompt_file.string() + " "
      "2>/dev/null";

    BOOST_LOG(debug) << "ai_optimizer: Running anthropic CLI for "sv << device_name;

    FILE *pipe = popen(cmd.c_str(), "r");
    if (!pipe) {
      BOOST_LOG(error) << "ai_optimizer: Failed to launch claude CLI"sv;
      return std::nullopt;
    }

    std::string output;
    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
      output += buffer;
    }

    int exit_code = pclose(pipe);

    std::filesystem::remove(prompt_file);
    std::filesystem::remove(system_file);

    if (exit_code != 0) {
      BOOST_LOG(error) << "ai_optimizer: claude CLI exited with code "sv << exit_code;
      if (!output.empty()) {
        BOOST_LOG(error) << "ai_optimizer: CLI output: "sv << output.substr(0, 200);
      }
      return std::nullopt;
    }

    if (output.empty()) {
      BOOST_LOG(error) << "ai_optimizer: claude CLI returned empty output"sv;
      return std::nullopt;
    }

    try {
      return parse_optimization_json(output, "Anthropic CLI");
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "ai_optimizer: Failed to parse CLI response: "sv << e.what();
      return std::nullopt;
    }
  }

  // ---- Anthropic API ----

  static std::optional<device_db::optimization_t> call_anthropic_api(
      const config_t &active_cfg,
      const std::string &device_name,
      const std::string &app_name,
      const std::string &gpu_info,
      const std::string &game_category = "",
      const std::optional<session_history_t> &history = std::nullopt) {

    nlohmann::json request_body;
    request_body["model"] = active_cfg.model;
    request_body["max_tokens"] = 256;
    request_body["temperature"] = 0;
    request_body["messages"] = nlohmann::json::array({
      {{"role", "user"}, {"content", build_user_prompt(device_name, app_name, gpu_info, game_category, history)}}
    });
    request_body["system"] = build_system_prompt();

    auto response = post_json(
      join_url(active_cfg.base_url, "/v1/messages"),
      request_body,
      active_cfg.timeout_ms,
      {
        "x-api-key: " + active_cfg.api_key,
        "anthropic-version: 2023-06-01"
      });

    if (response.curl_code != CURLE_OK) {
      BOOST_LOG(error) << "ai_optimizer: Anthropic request failed: "sv << curl_easy_strerror(response.curl_code);
      return std::nullopt;
    }

    if (response.http_code != 200) {
      BOOST_LOG(error) << "ai_optimizer: Anthropic returned HTTP "sv << response.http_code << ": "sv << response.body.substr(0, 200);
      return std::nullopt;
    }

    try {
      auto api_response = nlohmann::json::parse(response.body);
      auto content_text = extract_anthropic_text(api_response);
      if (content_text.empty()) {
        BOOST_LOG(error) << "ai_optimizer: Empty response from Anthropic API"sv;
        return std::nullopt;
      }
      return parse_optimization_json(content_text, "Anthropic API");
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "ai_optimizer: Failed to parse Anthropic response: "sv << e.what();
      return std::nullopt;
    }
  }

  // ---- OpenAI-compatible API ----

  static std::optional<device_db::optimization_t> call_openai_compatible_api(
      const config_t &active_cfg,
      const std::string &device_name,
      const std::string &app_name,
      const std::string &gpu_info,
      const std::string &game_category = "",
      const std::optional<session_history_t> &history = std::nullopt) {

    nlohmann::json request_body;
    request_body["model"] = active_cfg.model;
    request_body["temperature"] = 0;
    request_body["max_tokens"] = 256;
    request_body["messages"] = nlohmann::json::array({
      {{"role", "system"}, {"content", build_system_prompt()}},
      {{"role", "user"}, {"content", build_user_prompt(device_name, app_name, gpu_info, game_category, history)}}
    });
    request_body["response_format"] = optimization_response_format();

    std::vector<std::string> headers;
    if (active_cfg.auth_mode == AUTH_API_KEY && !active_cfg.api_key.empty()) {
      headers.push_back("Authorization: Bearer " + active_cfg.api_key);
    }

    auto response = post_json(join_url(active_cfg.base_url, "/chat/completions"), request_body, active_cfg.timeout_ms, headers);
    if (response.curl_code != CURLE_OK) {
      BOOST_LOG(error) << "ai_optimizer: OpenAI-compatible request failed: "sv << curl_easy_strerror(response.curl_code);
      return std::nullopt;
    }

    if (response.http_code != 200) {
      BOOST_LOG(error) << "ai_optimizer: OpenAI-compatible endpoint returned HTTP "sv << response.http_code << ": "sv << response.body.substr(0, 200);
      return std::nullopt;
    }

    try {
      auto api_response = nlohmann::json::parse(response.body);
      auto content_text = extract_openai_compatible_text(api_response);
      if (content_text.empty()) {
        BOOST_LOG(error) << "ai_optimizer: Empty response from OpenAI-compatible endpoint"sv;
        return std::nullopt;
      }
      return parse_optimization_json(content_text, "OpenAI-compatible API");
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "ai_optimizer: Failed to parse OpenAI-compatible response: "sv << e.what();
      return std::nullopt;
    }
  }

  // ---- Unified dispatch ----

  static std::optional<device_db::optimization_t> call_provider(
      const config_t &active_cfg,
      const std::string &device_name,
      const std::string &app_name,
      const std::string &gpu_info,
      const std::string &game_category = "",
      const std::optional<session_history_t> &history = std::nullopt) {
    if (active_cfg.provider == PROVIDER_ANTHROPIC) {
      if (active_cfg.auth_mode == AUTH_SUBSCRIPTION) {
        return call_anthropic_cli(active_cfg, device_name, app_name, gpu_info, game_category, history);
      }
      return call_anthropic_api(active_cfg, device_name, app_name, gpu_info, game_category, history);
    }

    return call_openai_compatible_api(active_cfg, device_name, app_name, gpu_info, game_category, history);
  }

  static void store_cache_entry(const config_t &active_cfg,
                                const std::string &device_name,
                                const std::string &app_name,
                                const device_db::optimization_t &optimization) {
    std::lock_guard<std::mutex> lock(cache_mutex);
    cache[current_cache_key(active_cfg, device_name, app_name)] = {
      active_cfg.provider,
      active_cfg.model,
      active_cfg.base_url,
      canonical_device_name(device_name),
      app_name,
      optimization,
      unix_time_now()
    };
    save_cache_locked();
  }

  static std::shared_future<std::optional<device_db::optimization_t>> get_or_start_request(
      const config_t &active_cfg,
      const std::string &device_name,
      const std::string &app_name,
      const std::string &gpu_info,
      const std::string &game_category,
      const std::optional<session_history_t> &history) {
    const auto key = current_cache_key(active_cfg, device_name, app_name);

    {
      std::lock_guard<std::mutex> lock(inflight_mutex);
      auto it = inflight_requests_by_key.find(key);
      if (it != inflight_requests_by_key.end()) {
        return it->second;
      }
    }

    auto promise = std::make_shared<std::promise<std::optional<device_db::optimization_t>>>();
    auto future = promise->get_future().share();

    {
      std::lock_guard<std::mutex> lock(inflight_mutex);
      inflight_requests_by_key[key] = future;
      ++in_flight_requests;
    }

    std::thread([promise, key, active_cfg, device_name, app_name, gpu_info, game_category, history]() {
      const auto started_at = std::chrono::steady_clock::now();
      try {
        auto result = call_provider(active_cfg, device_name, app_name, gpu_info, game_category, history);
        auto latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - started_at).count();

        if (result) {
          auto normalized = normalize_optimization(
            active_cfg,
            device_name,
            app_name,
            game_category,
            history,
            *result,
            false
          );
          update_provider_runtime_status(true, static_cast<long>(latency_ms));
          store_cache_entry(active_cfg, device_name, app_name, normalized);
          promise->set_value(normalized);
        } else {
          update_provider_runtime_status(false, static_cast<long>(latency_ms), "Provider returned no optimization result");
          promise->set_value(std::nullopt);
        }
      } catch (const std::exception &e) {
        auto latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::steady_clock::now() - started_at).count();
        update_provider_runtime_status(false, static_cast<long>(latency_ms), e.what());
        promise->set_value(std::nullopt);
      }

      std::lock_guard<std::mutex> lock(inflight_mutex);
      inflight_requests_by_key.erase(key);
      --in_flight_requests;
    }).detach();

    return future;
  }

  // ---- Public API ----

  void init(const config_t &config) {
    cfg = resolved_config(config);
    load_cache();
    load_history();
    if (cfg.enabled) {
      BOOST_LOG(info) << "ai_optimizer: Enabled provider="sv << cfg.provider
                      << ", model="sv << cfg.model
                      << ", auth="sv << cfg.auth_mode
                      << ", cache TTL "sv << cfg.cache_ttl_hours << "h"sv;
    }
  }

  bool is_enabled() {
    return is_config_enabled(cfg);
  }

  void set_enabled(bool enabled) {
    cfg.enabled = enabled;
    config::video.ai_optimizer.enabled = enabled;
    BOOST_LOG(info) << "ai_optimizer: "sv << (enabled ? "enabled" : "disabled") << " at runtime"sv;
  }

  std::optional<device_db::optimization_t> get_cached(
      const std::string &device_name, const std::string &app_name) {
    std::lock_guard<std::mutex> lock(cache_mutex);
    auto key = current_cache_key(device_name, app_name);
    auto it = cache.find(key);
    if (it == cache.end()) return std::nullopt;

    const auto now = unix_time_now();
    if (it->second.optimization.expires_at > 0 && now >= it->second.optimization.expires_at) {
      cache.erase(it);
      save_cache_locked();
      return std::nullopt;
    }

    auto optimization = it->second.optimization;
    optimization.source = "ai_cached";
    optimization.cache_status = "hit";
    return optimization;
  }

  void request_async(const std::string &device_name,
                     const std::string &app_name,
                     const std::string &gpu_info,
                     const std::string &game_category,
                     const std::optional<session_history_t> &history) {
    if (!is_enabled()) return;
    auto active_cfg = cfg;
    (void)get_or_start_request(active_cfg, device_name, app_name, gpu_info, game_category, history);
  }

  std::optional<device_db::optimization_t> request_sync(
      const std::string &device_name,
      const std::string &app_name,
      const std::string &gpu_info,
      const std::string &game_category,
      const std::optional<session_history_t> &history) {
    if (!is_enabled()) return std::nullopt;
    auto active_cfg = cfg;
    auto future = get_or_start_request(active_cfg, device_name, app_name, gpu_info, game_category, history);
    return future.get();
  }

  std::optional<device_db::optimization_t> request_sync_with_config(
      const config_t &config,
      const std::string &device_name,
      const std::string &app_name,
      const std::string &gpu_info,
      const std::string &game_category,
      const std::optional<session_history_t> &history) {
    auto active_cfg = resolved_config(config);
    if (!is_config_enabled(active_cfg)) {
      return std::nullopt;
    }
    const auto started_at = std::chrono::steady_clock::now();
    auto result = call_provider(active_cfg, device_name, app_name, gpu_info, game_category, history);
    const auto latency_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
      std::chrono::steady_clock::now() - started_at).count();
    if (!result) {
      update_provider_runtime_status(false, static_cast<long>(latency_ms), "Draft test returned no optimization result");
      return std::nullopt;
    }

    update_provider_runtime_status(true, static_cast<long>(latency_ms));
    return normalize_optimization(active_cfg, device_name, app_name, game_category, history, *result, false);
  }

  std::string get_models_json_with_config(const config_t &config) {
    auto active_cfg = resolved_config(config);

    nlohmann::json output;
    output["status"] = true;
    output["provider"] = active_cfg.provider;
    output["model"] = active_cfg.model;
    output["auth_mode"] = active_cfg.auth_mode;
    output["base_url"] = active_cfg.base_url;
    output["discovered"] = false;
    output["source"] = "fallback";
    output["models"] = nlohmann::json::array();
    output["fallback_models"] = fallback_model_list(active_cfg);

    const auto discovery_key = model_catalog_key(active_cfg);
    {
      std::lock_guard<std::mutex> lock(model_catalog_mutex);
      auto it = model_catalog_cache.find(discovery_key);
      if (it != model_catalog_cache.end() &&
          unix_time_now() - it->second.cached_at < MODEL_DISCOVERY_TTL_SECONDS) {
        return it->second.payload;
      }
    }

    if (active_cfg.provider == PROVIDER_ANTHROPIC && active_cfg.auth_mode == AUTH_SUBSCRIPTION) {
      output["error"] = "Live model discovery is unavailable for Claude CLI subscription mode.";
      return output.dump(2);
    }

    if (active_cfg.auth_mode == AUTH_API_KEY && active_cfg.api_key.empty()) {
      output["error"] = "Enter an API key to fetch the live model list.";
      return output.dump(2);
    }

    http_response_t response;
    if (active_cfg.provider == PROVIDER_ANTHROPIC) {
      response = get_json(
        join_url(active_cfg.base_url, "/v1/models"),
        active_cfg.timeout_ms,
        {
          "x-api-key: " + active_cfg.api_key,
          "anthropic-version: 2023-06-01"
        });
    } else {
      std::vector<std::string> headers;
      if (active_cfg.auth_mode == AUTH_API_KEY && !active_cfg.api_key.empty()) {
        headers.push_back("Authorization: Bearer " + active_cfg.api_key);
      }
      response = get_json(join_url(active_cfg.base_url, "/models"), active_cfg.timeout_ms, headers);
    }

    if (response.curl_code != CURLE_OK) {
      output["error"] = std::string("Model discovery request failed: ") + curl_easy_strerror(response.curl_code);
      return output.dump(2);
    }

    if (response.http_code != 200) {
      output["error"] = "Model discovery endpoint returned HTTP " + std::to_string(response.http_code);
      return output.dump(2);
    }

    try {
      auto api_response = nlohmann::json::parse(response.body);
      nlohmann::json discovered_models = nlohmann::json::array();
      std::unordered_set<std::string> seen_ids;

      if (active_cfg.provider == PROVIDER_ANTHROPIC) {
        for (const auto &item : api_response.value("data", nlohmann::json::array())) {
          append_unique_model(
            discovered_models,
            seen_ids,
            item.value("id", std::string {}),
            item.value("display_name", std::string {}));
        }
      } else {
        for (const auto &item : api_response.value("data", nlohmann::json::array())) {
          append_unique_model(
            discovered_models,
            seen_ids,
            item.value("id", std::string {}),
            item.value("id", std::string {}));
        }
      }

      if (discovered_models.empty()) {
        output["error"] = "Provider returned an empty model list.";
        return output.dump(2);
      }

      output["discovered"] = true;
      output["source"] = "live";
      output["models"] = discovered_models;
      output["model_count"] = discovered_models.size();
    } catch (const std::exception &e) {
      output["error"] = std::string("Failed to parse model list: ") + e.what();
    }
    const auto payload = output.dump(2);
    if (output.value("discovered", false)) {
      std::lock_guard<std::mutex> lock(model_catalog_mutex);
      model_catalog_cache[discovery_key] = {payload, unix_time_now()};
    }
    return payload;
  }

  std::string get_status_json() {
    nlohmann::json status;
    std::size_t cache_count = 0;
    {
      std::lock_guard<std::mutex> lock(cache_mutex);
      cache_count = cache.size();
    }
    provider_runtime_status_t runtime_status_snapshot;
    {
      std::lock_guard<std::mutex> lock(inflight_mutex);
      runtime_status_snapshot = provider_runtime_status;
    }
    status["enabled"] = cfg.enabled;
    status["provider"] = cfg.provider;
    status["model"] = cfg.model;
    status["auth_mode"] = cfg.auth_mode;
    status["base_url"] = cfg.base_url;
    status["has_api_key"] = !cfg.api_key.empty();
    status["use_subscription"] = cfg.auth_mode == AUTH_SUBSCRIPTION;
    status["timeout_ms"] = cfg.timeout_ms;
    status["cache_ttl_hours"] = cfg.cache_ttl_hours;
    status["cache_count"] = cache_count;
    status["recommendation_version"] = OPTIMIZATION_SCHEMA_VERSION;
    status["in_flight_requests"] = in_flight_requests.load();
    status["last_success_at"] = runtime_status_snapshot.last_success_at;
    status["last_failure_at"] = runtime_status_snapshot.last_failure_at;
    status["last_latency_ms"] = runtime_status_snapshot.last_latency_ms;
    status["last_error"] = runtime_status_snapshot.last_error;

    if (cfg.provider == PROVIDER_ANTHROPIC && cfg.auth_mode == AUTH_SUBSCRIPTION) {
      int rc = system("command -v claude >/dev/null 2>&1");
      status["cli_available"] = (rc == 0);
    }

    return status.dump();
  }

  std::string get_cache_json() {
    std::lock_guard<std::mutex> lock(cache_mutex);
    nlohmann::json root = nlohmann::json::object();
    for (const auto &[key, entry] : cache) {
      if (entry.provider != cfg.provider || entry.model != cfg.model || entry.base_url != cfg.base_url) {
        continue;
      }
      nlohmann::json val;
      val["timestamp"] = entry.timestamp;
      val["provider"] = entry.provider;
      val["model"] = entry.model;
      val["base_url"] = entry.base_url;
      auto &o = entry.optimization;
      if (o.display_mode) val["display_mode"] = *o.display_mode;
      if (o.target_bitrate_kbps) val["target_bitrate_kbps"] = *o.target_bitrate_kbps;
      if (o.nvenc_tune) val["nvenc_tune"] = *o.nvenc_tune;
      if (o.preferred_codec) val["preferred_codec"] = *o.preferred_codec;
      val["reasoning"] = o.reasoning;
      val["cache_status"] = "hit";
      val["confidence"] = o.confidence;
      val["signals_used"] = o.signals_used;
      val["normalization_reason"] = o.normalization_reason;
      val["generated_at"] = o.generated_at;
      val["expires_at"] = o.expires_at;
      val["recommendation_version"] = o.recommendation_version;
      val["source"] = o.source;
      root[history_key(entry.device_name, entry.app_name)] = val;
    }
    return root.dump(2);
  }

  void clear_cache() {
    {
      std::lock_guard<std::mutex> lock(cache_mutex);
      cache.clear();
      std::remove(cache_path().c_str());
    }
    {
      std::lock_guard<std::mutex> lock(model_catalog_mutex);
      model_catalog_cache.clear();
    }
    BOOST_LOG(info) << "ai_optimizer: Cache cleared"sv;
  }

  // ---- Session history for feedback loop ----

  static std::filesystem::path history_path() {
    return platf::appdata() / "ai_session_history.json";
  }

  static void load_history() {
    std::lock_guard<std::mutex> lock(history_mutex);
    std::ifstream file(history_path());
    if (!file.is_open()) return;
    try {
      auto root = nlohmann::json::parse(file);
      for (auto &[key, val] : root.items()) {
        session_history_t h;
        h.avg_fps = val.value("avg_fps", 0.0);
        h.avg_latency_ms = val.value("avg_latency_ms", 0.0);
        h.avg_bitrate_kbps = val.value("avg_bitrate_kbps", 0);
        h.packet_loss_pct = val.value("packet_loss_pct", 0.0);
        h.quality_grade = val.value("quality_grade", "");
        h.codec = val.value("codec", "");
        h.session_count = val.value("session_count", 0);
        h.last_fps = val.value("last_fps", h.avg_fps);
        h.last_target_fps = val.value("last_target_fps", 0.0);
        h.last_latency_ms = val.value("last_latency_ms", h.avg_latency_ms);
        h.last_bitrate_kbps = val.value("last_bitrate_kbps", h.avg_bitrate_kbps);
        h.last_packet_loss_pct = val.value("last_packet_loss_pct", h.packet_loss_pct);
        h.last_quality_grade = val.value("last_quality_grade", h.quality_grade);
        h.last_codec = val.value("last_codec", h.codec);
        h.poor_outcome_count = val.value("poor_outcome_count", 0);
        h.consecutive_poor_outcomes = val.value("consecutive_poor_outcomes", 0);
        h.last_optimization_source = val.value("last_optimization_source", "");
        h.last_optimization_confidence = val.value("last_optimization_confidence", "");
        h.last_recommendation_version = val.value("last_recommendation_version", 0);
        h.last_updated_at = val.value("last_updated_at", 0);
        h.last_invalidated_at = val.value("last_invalidated_at", 0);
        auto pos = key.find(':');
        const auto device_name = pos == std::string::npos ? key : key.substr(0, pos);
        const auto app_name = pos == std::string::npos ? std::string {} : key.substr(pos + 1);
        const auto normalized_key = history_key(device_name, app_name);
        auto existing = session_history.find(normalized_key);
        if (existing == session_history.end()) {
          session_history[normalized_key] = h;
        } else {
          existing->second = merge_history_entries(existing->second, h);
        }
      }
    } catch (...) {}
  }

  static void save_history() {
    std::lock_guard<std::mutex> lock(history_mutex);
    nlohmann::json root;
    for (const auto &[key, h] : session_history) {
      root[key] = {
        {"avg_fps", h.avg_fps},
        {"avg_latency_ms", h.avg_latency_ms},
        {"avg_bitrate_kbps", h.avg_bitrate_kbps},
        {"packet_loss_pct", h.packet_loss_pct},
        {"quality_grade", h.quality_grade},
        {"codec", h.codec},
        {"session_count", h.session_count},
        {"last_fps", h.last_fps},
        {"last_target_fps", h.last_target_fps},
        {"last_latency_ms", h.last_latency_ms},
        {"last_bitrate_kbps", h.last_bitrate_kbps},
        {"last_packet_loss_pct", h.last_packet_loss_pct},
        {"last_quality_grade", h.last_quality_grade},
        {"last_codec", h.last_codec},
        {"poor_outcome_count", h.poor_outcome_count},
        {"consecutive_poor_outcomes", h.consecutive_poor_outcomes},
        {"last_optimization_source", h.last_optimization_source},
        {"last_optimization_confidence", h.last_optimization_confidence},
        {"last_recommendation_version", h.last_recommendation_version},
        {"last_updated_at", h.last_updated_at},
        {"last_invalidated_at", h.last_invalidated_at}
      };
    }
    std::ofstream file(history_path());
    if (file.is_open()) file << root.dump(2);
  }

  void record_session(const std::string &device_name,
                      const std::string &app_name,
                      const session_history_t &session) {
    const auto canonical_device = canonical_device_name(device_name);
    auto key = history_key(canonical_device, app_name);
    int session_count_total = 0;
    {
      std::lock_guard<std::mutex> lock(history_mutex);
      auto &existing = session_history[key];

      // Rolling average with the new session
      if (existing.session_count > 0) {
        double n = existing.session_count;
        existing.avg_fps = (existing.avg_fps * n + session.avg_fps) / (n + 1);
        existing.avg_latency_ms = (existing.avg_latency_ms * n + session.avg_latency_ms) / (n + 1);
        existing.avg_bitrate_kbps = static_cast<int>((existing.avg_bitrate_kbps * n + session.avg_bitrate_kbps) / (n + 1));
        existing.packet_loss_pct = (existing.packet_loss_pct * n + session.packet_loss_pct) / (n + 1);
        existing.session_count++;
      } else {
        existing = session;
        existing.session_count = 1;
      }
      existing.quality_grade = session.quality_grade;
      existing.codec = session.codec;
      existing.last_fps = session.last_fps;
      existing.last_target_fps = session.last_target_fps;
      existing.last_latency_ms = session.last_latency_ms;
      existing.last_bitrate_kbps = session.last_bitrate_kbps;
      existing.last_packet_loss_pct = session.last_packet_loss_pct;
      existing.last_quality_grade = session.last_quality_grade;
      existing.last_codec = session.last_codec;
      existing.last_updated_at = unix_time_now();
      existing.last_optimization_source = session.last_optimization_source;
      existing.last_optimization_confidence = session.last_optimization_confidence;
      existing.last_recommendation_version = session.last_recommendation_version;

      if (session.quality_grade == "D" || session.quality_grade == "F") {
        existing.poor_outcome_count++;
        existing.consecutive_poor_outcomes++;
        existing.last_invalidated_at = unix_time_now();
      } else {
        existing.consecutive_poor_outcomes = 0;
      }
      session_count_total = existing.session_count;
    }

    save_history();
    BOOST_LOG(info) << "ai_optimizer: Recorded session for "sv << key
                    << " — grade "sv << session.quality_grade
                    << " against target "sv << session.last_target_fps
                    << "fps, "sv << session_count_total << " sessions total"sv;

    // If quality is poor (D or F), invalidate cache to force re-optimization
    if (session.quality_grade == "D" || session.quality_grade == "F") {
      std::lock_guard<std::mutex> lock(cache_mutex);
      cache.erase(current_cache_key(canonical_device, app_name));
      save_cache_locked();
      BOOST_LOG(info) << "ai_optimizer: Poor quality — invalidated cache for "sv << key;
    }
  }

  std::optional<session_history_t> get_session_history(
      const std::string &device_name, const std::string &app_name) {
    static std::once_flag history_load_flag;
    std::call_once(history_load_flag, load_history);

    std::lock_guard<std::mutex> lock(history_mutex);
    auto key = history_key(device_name, app_name);
    auto it = session_history.find(key);
    if (it == session_history.end()) return std::nullopt;
    return it->second;
  }

  std::string get_history_json() {
    static std::once_flag history_load_flag;
    std::call_once(history_load_flag, load_history);

    nlohmann::json arr = nlohmann::json::array();
    std::lock_guard<std::mutex> lock(history_mutex);
    for (const auto &[key, h] : session_history) {
      nlohmann::json entry;
      entry["key"] = key;
      entry["avg_fps"] = h.avg_fps;
      entry["avg_latency_ms"] = h.avg_latency_ms;
      entry["avg_bitrate_kbps"] = h.avg_bitrate_kbps;
      entry["packet_loss_pct"] = h.packet_loss_pct;
      entry["codec"] = h.codec;
      entry["quality_grade"] = h.quality_grade;
      entry["session_count"] = h.session_count;
      entry["last_fps"] = h.last_fps;
      entry["last_target_fps"] = h.last_target_fps;
      entry["last_latency_ms"] = h.last_latency_ms;
      entry["last_bitrate_kbps"] = h.last_bitrate_kbps;
      entry["last_packet_loss_pct"] = h.last_packet_loss_pct;
      entry["last_quality_grade"] = h.last_quality_grade;
      entry["last_codec"] = h.last_codec;
      entry["poor_outcome_count"] = h.poor_outcome_count;
      entry["consecutive_poor_outcomes"] = h.consecutive_poor_outcomes;
      entry["last_optimization_source"] = h.last_optimization_source;
      entry["last_optimization_confidence"] = h.last_optimization_confidence;
      entry["last_recommendation_version"] = h.last_recommendation_version;
      entry["last_updated_at"] = h.last_updated_at;
      entry["last_invalidated_at"] = h.last_invalidated_at;
      arr.push_back(entry);
    }
    return arr.dump();
  }

}  // namespace ai_optimizer
