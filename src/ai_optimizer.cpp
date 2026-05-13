/**
 * @file src/ai_optimizer.cpp
 * @brief Provider-agnostic API integration for streaming optimization.
 */

#include "ai_optimizer.h"
#include "config.h"
#include "game_classifier.h"
#include "logging.h"
#include "platform/common.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <future>
#include <iterator>
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

    constexpr int OPTIMIZATION_SCHEMA_VERSION = 3;
    constexpr int MAX_REASONABLE_SESSION_COUNT = 10000;
    constexpr int MAX_REASONABLE_POOR_OUTCOME_COUNT = 10000;
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

  static bool load_history();
  static void save_history();

  struct http_response_t {
    CURLcode curl_code = CURLE_OK;
    long http_code = 0;
    std::string body;
  };

  struct shell_command_result_t {
    int exit_code = -1;
    std::string output;
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

  static bool is_frame_or_host_pacing_issue(const std::string &issue) {
    const auto normalized = to_lower_copy(issue);
    return normalized == "frame_pacing" || normalized == "host_render_limited";
  }

  static bool has_frame_or_host_pacing_issue(const session_history_t &session) {
    return is_frame_or_host_pacing_issue(session.last_primary_issue) ||
      std::any_of(session.last_issues.begin(), session.last_issues.end(), [](const std::string &issue) {
        return is_frame_or_host_pacing_issue(issue);
      });
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

  static bool provider_supports_subscription(const std::string &provider) {
    return provider == PROVIDER_ANTHROPIC || provider == PROVIDER_OPENAI;
  }

  static std::string subscription_cli_binary(const std::string &provider) {
    return provider == PROVIDER_OPENAI ? "codex" : "claude";
  }

  static std::string subscription_cli_label(const std::string &provider) {
    return provider == PROVIDER_OPENAI ? "Codex CLI" : "Claude CLI";
  }

  static std::string subscription_login_command(const std::string &provider) {
    return provider == PROVIDER_OPENAI ? "codex login" : "";
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
      if (legacy_subscription && provider_supports_subscription(provider)) {
        return AUTH_SUBSCRIPTION;
      }
      if (provider == PROVIDER_LOCAL && api_key.empty()) {
        return AUTH_NONE;
      }
      return AUTH_API_KEY;
    }

    if (auth_mode == "subscription" || auth_mode == "cli" || auth_mode == "claude_cli" || auth_mode == "codex_cli" || auth_mode == "codex") {
      return provider_supports_subscription(provider) ? AUTH_SUBSCRIPTION : (provider == PROVIDER_LOCAL && api_key.empty() ? AUTH_NONE : AUTH_API_KEY);
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

  static std::string shell_escape_single_quotes(const std::string &value) {
    std::string escaped;
    for (char ch : value) {
      if (ch == '\'') {
        escaped += "'\\''";
      } else {
        escaped += ch;
      }
    }
    return escaped;
  }

  static std::string resolve_existing_binary(const std::vector<std::string> &candidates, const std::string &fallback) {
    for (const auto &candidate : candidates) {
      if (std::filesystem::exists(candidate)) {
        return candidate;
      }
    }
    return fallback;
  }

  static shell_command_result_t run_command_capture(const std::string &command) {
    shell_command_result_t result;
    FILE *pipe = popen(command.c_str(), "r");
    if (!pipe) {
      return result;
    }

    char buffer[4096];
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) {
      result.output += buffer;
    }

    result.exit_code = pclose(pipe);
    return result;
  }

  static std::optional<bool> codex_login_ready() {
    auto status = run_command_capture("codex login status 2>/dev/null");
    if (status.exit_code == -1) {
      return std::nullopt;
    }

    const auto normalized = to_lower_copy(status.output);
    if (normalized.find("logged in") != std::string::npos) {
      return true;
    }
    if (normalized.find("not logged in") != std::string::npos || normalized.find("logged out") != std::string::npos) {
      return false;
    }
    return status.exit_code == 0;
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

  static bool is_poor_quality_grade(const std::string &grade) {
    const auto normalized = to_lower_copy(grade);
    return normalized == "d" || normalized == "f";
  }

  static std::string normalize_end_reason(const std::string &reason) {
    auto normalized = to_lower_copy(reason);
    normalized.erase(normalized.begin(), std::find_if(normalized.begin(), normalized.end(), [](unsigned char ch) {
      return !std::isspace(ch);
    }));
    normalized.erase(std::find_if(normalized.rbegin(), normalized.rend(), [](unsigned char ch) {
      return !std::isspace(ch);
    }).base(), normalized.end());
    if (normalized.empty()) {
      return "disconnect";
    }
    return normalized;
  }

  static bool is_explicit_end_reason(const std::string &reason) {
    const auto normalized = normalize_end_reason(reason);
    return normalized == "end" || normalized == "user_end" || normalized == "quit";
  }

  static bool is_soft_end_reason(const std::string &reason) {
    const auto normalized = normalize_end_reason(reason);
    return normalized == "host_pause" ||
      normalized == "disconnect" ||
      normalized == "cancelled";
  }

  static bool has_confirming_soft_end_recovery_signal(const session_history_t &session) {
    return is_soft_end_reason(session.last_end_reason) &&
      session.last_duration_s >= 60 &&
      session.last_sample_count >= 30 &&
      session.last_safe_target_fps > 0.0 &&
      has_frame_or_host_pacing_issue(session);
  }

  static bool is_low_confidence_soft_end_without_confirming_signal(const session_history_t &session) {
    const auto confidence = to_lower_copy(session.last_sample_confidence);
    const bool soft_end = is_soft_end_reason(session.last_end_reason);
    return confidence == "low" &&
      soft_end &&
      session.poor_outcome_count <= 0 &&
      session.consecutive_poor_outcomes <= 0 &&
      !has_confirming_soft_end_recovery_signal(session);
  }

  static bool is_unconfirmed_poor_recovery_feedback(const session_history_t &session) {
    return is_poor_quality_grade(latest_quality_grade(session)) &&
      is_low_confidence_soft_end_without_confirming_signal(session);
  }

  static bool has_session_observation(const session_history_t &session) {
    return session.avg_fps > 0.0 ||
      session.last_fps > 0.0 ||
      session.last_target_fps > 0.0 ||
      session.last_updated_at > 0 ||
      !latest_quality_grade(session).empty();
  }

  static int sanitize_session_count(const session_history_t &session) {
    if (session.session_count > 0 &&
        session.session_count <= MAX_REASONABLE_SESSION_COUNT) {
      return session.session_count;
    }

    return has_session_observation(session) ? 1 : 0;
  }

  static int sanitize_poor_outcome_count(int value, int session_count) {
    if (value < 0 ||
        value > MAX_REASONABLE_POOR_OUTCOME_COUNT ||
        session_count <= 0 ||
        value > session_count) {
      return 0;
    }

    return value;
  }

  static int sanitize_consecutive_poor_outcome_count(int value, int poor_outcome_count) {
    if (value < 0 ||
        value > MAX_REASONABLE_POOR_OUTCOME_COUNT ||
        poor_outcome_count <= 0 ||
        value > poor_outcome_count) {
      return 0;
    }

    return value;
  }

  session_history_t sanitize_session_history(session_history_t session) {
    session.session_count = sanitize_session_count(session);
    session.poor_outcome_count =
      sanitize_poor_outcome_count(session.poor_outcome_count, session.session_count);
    session.consecutive_poor_outcomes =
      sanitize_consecutive_poor_outcome_count(
        session.consecutive_poor_outcomes,
        session.poor_outcome_count
      );

    if (session.last_safe_target_fps < 0.0 ||
        session.last_safe_target_fps > 240.0 ||
        !std::isfinite(session.last_safe_target_fps)) {
      session.last_safe_target_fps = 0.0;
    }

    if (is_low_confidence_soft_end_without_confirming_signal(session)) {
      session.last_safe_target_fps = 0.0;
      session.last_relaunch_recommended = false;
      if (to_lower_copy(session.last_health_grade) == "degraded") {
        session.last_health_grade = "watch";
      }
    }

    return session;
  }

  static bool history_repair_should_persist(const session_history_t &before,
                                            const session_history_t &after) {
    return before.session_count != after.session_count ||
      before.poor_outcome_count != after.poor_outcome_count ||
      before.consecutive_poor_outcomes != after.consecutive_poor_outcomes ||
      before.last_safe_target_fps != after.last_safe_target_fps ||
      before.last_relaunch_recommended != after.last_relaunch_recommended ||
      before.last_health_grade != after.last_health_grade;
  }

  static bool is_control_shell_app(const std::string &app_name) {
    const auto normalized = to_lower_copy(app_name);
    return normalized.empty() ||
           normalized == "desktop" ||
           normalized == "low res desktop" ||
           normalized == "input only" ||
           normalized.find("steam gamepad ui") != std::string::npos ||
           normalized.find("steam big picture") != std::string::npos ||
           normalized.find("big picture") != std::string::npos;
  }

  session_feedback_policy_t classify_session_feedback(
      const std::string &app_name,
      const session_history_t &session) {
    session_feedback_policy_t policy;
    policy.end_reason = normalize_end_reason(session.last_end_reason);

    const bool has_quality_metrics =
      session.avg_fps > 0.0 &&
      (session.last_target_fps > 0.0 || session.avg_fps > 0.0);
    if (!has_quality_metrics) {
      policy.confidence = "ignored";
      policy.ignored_reason = "missing_quality_metrics";
      return policy;
    }

    const bool explicit_end = is_explicit_end_reason(policy.end_reason);
    const bool sampled_recovery_end = has_confirming_soft_end_recovery_signal(session);
    const bool high_signal =
      (explicit_end || sampled_recovery_end) &&
      session.last_duration_s >= 30 &&
      session.last_sample_count >= 10;

    policy.accepted = true;
    policy.confidence = high_signal ? "high" : "low";
    policy.counts_poor_outcome = high_signal && !is_control_shell_app(app_name);
    policy.can_invalidate_cache = policy.counts_poor_outcome;
    return policy;
  }

  std::string grade_session_quality(const session_history_t &session) {
    const double target_fps =
      session.last_target_fps > 0.0 ? session.last_target_fps : session.avg_fps;
    const double fps_ratio =
      (target_fps > 0.0 && session.avg_fps > 0.0) ?
        std::clamp(session.avg_fps / target_fps, 0.0, 1.5) :
        0.0;
    const double low_ratio =
      (target_fps > 0.0 && session.last_low_1_percent_fps > 0.0) ?
        std::clamp(session.last_low_1_percent_fps / target_fps, 0.0, 1.5) :
        1.5;
    const bool has_low_percentile = session.last_low_1_percent_fps > 0.0;
    const bool isolated_min_drop =
      session.last_min_fps > 0.0 &&
      target_fps > 0.0 &&
      session.last_min_fps < target_fps * 0.45;
    const bool min_drop_confirms_severe_pacing =
      isolated_min_drop &&
      (
        !has_low_percentile ||
        low_ratio < 0.70 ||
        fps_ratio < 0.85 ||
        session.last_frame_pacing_bad_pct >= 5.0
      );
    const bool severe_pacing =
      (has_low_percentile && low_ratio < 0.55) ||
      min_drop_confirms_severe_pacing ||
      session.last_frame_pacing_bad_pct >= 20.0;
    const bool moderate_pacing =
      (has_low_percentile && low_ratio < 0.75) ||
      session.last_frame_pacing_bad_pct >= 10.0;
    const bool mild_pacing =
      (has_low_percentile && low_ratio < 0.85) ||
      isolated_min_drop ||
      session.last_frame_pacing_bad_pct >= 5.0;

    if (session.avg_fps <= 0.0) {
      return "F";
    }
    if (severe_pacing) {
      return "D";
    }
    if (moderate_pacing) {
      return "C";
    }
    if (mild_pacing) {
      return "B";
    }
    if (fps_ratio >= 0.95 && session.packet_loss_pct < 0.5 && session.avg_latency_ms < 20.0) {
      return "A";
    }
    if (fps_ratio >= 0.85 && session.packet_loss_pct < 2.0 && session.avg_latency_ms < 40.0) {
      return "B";
    }
    if (fps_ratio >= 0.70 && session.packet_loss_pct < 5.0) {
      return "C";
    }
    if (fps_ratio >= 0.50) {
      return "D";
    }
    return "F";
  }

  static bool should_invalidate_cache_for_poor_quality(
      const std::string &app_name,
      const session_history_t &session,
      const session_feedback_policy_t &policy,
      int consecutive_poor_outcomes) {
    return is_poor_quality_grade(session.quality_grade) &&
           policy.can_invalidate_cache &&
           consecutive_poor_outcomes >= 2 &&
           !is_control_shell_app(app_name);
  }

  static std::string codec_family(const std::string &codec) {
    const auto normalized = to_lower_copy(codec);
    if (normalized.find("av1") != std::string::npos) {
      return "av1";
    }
    if (normalized.find("hevc") != std::string::npos || normalized.find("h265") != std::string::npos) {
      return "hevc";
    }
    if (normalized.find("avc") != std::string::npos || normalized.find("h264") != std::string::npos) {
      return "h264";
    }
    return normalized;
  }

  static bool is_mobile_client_type(const std::optional<device_db::device_t> &device_profile) {
    return device_profile &&
      (device_profile->type == "handheld" || device_profile->type == "phone");
  }

  static int derive_safe_bitrate_kbps(int baseline_kbps,
                                      const std::optional<device_db::device_t> &device_profile,
                                      bool degraded_history) {
    int safe_kbps = baseline_kbps > 0 ? baseline_kbps : 15000;

    if (device_profile) {
      if (device_profile->type == "handheld") {
        safe_kbps = std::min(safe_kbps, 16000);
      } else if (device_profile->type == "phone") {
        safe_kbps = std::min(safe_kbps, 18000);
      } else if (device_profile->type == "tablet") {
        safe_kbps = std::min(safe_kbps, 22000);
      } else if (device_profile->type == "desktop") {
        safe_kbps = std::min(safe_kbps, 30000);
      }
    }

    if (degraded_history) {
      safe_kbps = static_cast<int>(std::lround(static_cast<double>(safe_kbps) * 0.75));
    }

    return std::clamp(safe_kbps, 6000, std::max(6000, baseline_kbps > 0 ? baseline_kbps : safe_kbps));
  }

  double derive_safe_target_fps(double target_fps,
                                double avg_fps,
                                double low_1_percent_fps,
                                double min_fps,
                                double frame_pacing_bad_pct,
                                bool mobile_client,
                                bool degraded_history,
                                bool pacing_risk) {
    if (target_fps <= 30.0) {
      return 0.0;
    }

    const double low_signal =
      low_1_percent_fps > 0.0 ? low_1_percent_fps :
      min_fps > 0.0 ? min_fps :
      avg_fps;
    const bool severe_pacing =
      (low_signal > 0.0 && low_signal < target_fps * 0.72) ||
      frame_pacing_bad_pct >= 18.0 ||
      (avg_fps > 0.0 && avg_fps < target_fps * 0.82);
    const bool moderate_pacing =
      (low_signal > 0.0 && low_signal < target_fps * 0.85) ||
      frame_pacing_bad_pct >= 8.0 ||
      (avg_fps > 0.0 && avg_fps < target_fps * 0.90);

    if (target_fps >= 90.0 && (severe_pacing || moderate_pacing || pacing_risk || degraded_history)) {
      const bool can_hold_sixty =
        avg_fps >= 58.0 &&
        low_signal >= 45.0 &&
        frame_pacing_bad_pct < 18.0;
      return can_hold_sixty ? 60.0 : 30.0;
    }

    if (severe_pacing) {
      return 30.0;
    }

    if ((mobile_client && degraded_history && moderate_pacing) ||
        (mobile_client && pacing_risk && moderate_pacing)) {
      const bool can_hold_stable_40 =
        target_fps >= 55.0 &&
        target_fps <= 75.0 &&
        avg_fps > 0.0 &&
        avg_fps >= target_fps * 0.82 &&
        low_signal > 0.0 &&
        low_signal >= target_fps * 0.70;
      if (can_hold_stable_40) {
        return 40.0;
      }

      return 30.0;
    }

    return 0.0;
  }

  double effective_history_safe_target_fps(const std::string &device_name,
                                           const session_history_t &raw_session) {
    const auto session = sanitize_session_history(raw_session);
    if (session.last_safe_target_fps <= 0.0) {
      return 0.0;
    }
    if (is_unconfirmed_poor_recovery_feedback(session)) {
      return 0.0;
    }

    const auto device_profile = device_db::get_device(device_name);
    const bool mobile_client = is_mobile_client_type(device_profile);
    const std::string quality_grade = latest_quality_grade(session);
    const bool degraded_history =
      session.consecutive_poor_outcomes > 0 ||
      session.poor_outcome_count > 0 ||
      is_poor_quality_grade(quality_grade) ||
      to_lower_copy(session.last_health_grade) == "degraded";
    const double target_fps = session.last_target_fps > 0.0 ? session.last_target_fps : session.last_fps;
    const double history_fps =
      session.avg_fps > 0.0 ? session.avg_fps : session.last_fps;
    const double optimizer_fps =
      history_fps > 0.0 && session.last_fps > 0.0 ? std::min(session.last_fps, history_fps) :
      history_fps > 0.0 ? history_fps :
      session.last_fps;
    const double fps_gap = target_fps > 0.0 ? std::max(0.0, target_fps - session.last_fps) : 0.0;
    const double fps_ratio =
      (target_fps > 0.0 && session.last_fps > 0.0) ? std::clamp(session.last_fps / target_fps, 0.0, 1.5) : 0.0;
    const bool prior_frame_pacing = has_frame_or_host_pacing_issue(session);
    const bool client_pacing_risk =
      (target_fps > 0.0 && session.last_low_1_percent_fps > 0.0 &&
       session.last_low_1_percent_fps < target_fps * 0.85) ||
      (target_fps > 0.0 && session.last_min_fps > 0.0 &&
       session.last_min_fps < target_fps * 0.60) ||
      session.last_frame_pacing_bad_pct >= 8.0;
    const bool pacing_risk =
      fps_gap >= 4.0 ||
      (target_fps > 0.0 && fps_ratio < 0.88) ||
      client_pacing_risk ||
      prior_frame_pacing ||
      is_poor_quality_grade(quality_grade);
    const double revised_target = derive_safe_target_fps(
      target_fps,
      optimizer_fps,
      session.last_low_1_percent_fps,
      session.last_min_fps,
      session.last_frame_pacing_bad_pct,
      mobile_client,
      degraded_history,
      pacing_risk
    );

    if (session.last_safe_target_fps <= 30.5 &&
        revised_target > session.last_safe_target_fps + 0.5 &&
        target_fps > session.last_safe_target_fps + 0.5) {
      return revised_target;
    }

    return session.last_safe_target_fps;
  }

  static void enrich_session_reliability_context(const std::string &device_name,
                                                 const std::string &app_name,
                                                 session_history_t &session) {
    const auto device_profile = device_db::get_device(device_name);
    const bool mobile_client = is_mobile_client_type(device_profile);
    const std::string quality_grade = latest_quality_grade(session);
    const bool degraded_history =
      session.consecutive_poor_outcomes > 0 ||
      session.poor_outcome_count > 0 ||
      is_poor_quality_grade(quality_grade) ||
      to_lower_copy(session.last_health_grade) == "degraded";

    const double target_fps = session.last_target_fps > 0.0 ? session.last_target_fps : session.last_fps;
    const auto optimization_source = to_lower_copy(session.last_optimization_source);
    const bool launched_with_history_safe =
      optimization_source.find("history_safe") != std::string::npos;
    const double fps_gap = target_fps > 0.0 ? std::max(0.0, target_fps - session.last_fps) : 0.0;
    const double fps_ratio =
      (target_fps > 0.0 && session.last_fps > 0.0) ? std::clamp(session.last_fps / target_fps, 0.0, 1.5) : 0.0;
    const double history_fps =
      session.avg_fps > 0.0 ? session.avg_fps : session.last_fps;
    const double optimizer_fps =
      history_fps > 0.0 && session.last_fps > 0.0 ? std::min(session.last_fps, history_fps) :
      history_fps > 0.0 ? history_fps :
      session.last_fps;
    const bool prior_frame_pacing = has_frame_or_host_pacing_issue(session);
    const bool network_risk = session.last_packet_loss_pct >= 0.35 || session.last_latency_ms >= 28.0;
    const bool client_pacing_risk =
      (target_fps > 0.0 && session.last_low_1_percent_fps > 0.0 &&
       session.last_low_1_percent_fps < target_fps * 0.85) ||
      (target_fps > 0.0 && session.last_min_fps > 0.0 &&
       session.last_min_fps < target_fps * 0.60) ||
      session.last_frame_pacing_bad_pct >= 8.0;
    const bool pacing_risk =
      fps_gap >= 4.0 ||
      (target_fps > 0.0 && fps_ratio < 0.88) ||
      client_pacing_risk ||
      prior_frame_pacing ||
      is_poor_quality_grade(quality_grade);

    const auto active_codec_family = codec_family(latest_codec(session));
    const bool decoder_risk =
      ((active_codec_family == "av1") || (mobile_client && active_codec_family == "hevc" && fps_gap >= 8.0)) &&
      (pacing_risk || fps_gap >= 4.0) &&
      !network_risk;

    bool capture_fallback = false;
    if (!session.last_capture_path.empty()) {
      const auto capture_path = to_lower_copy(session.last_capture_path);
      capture_fallback =
        capture_path.find("cpu") != std::string::npos ||
        capture_path.find("shm") != std::string::npos ||
        capture_path.find("fallback") != std::string::npos;
    }

    const bool virtual_display_risk =
      session.last_capture_path == "virtual_display" &&
      (pacing_risk || degraded_history);

    const bool hdr_risk =
      session.last_hdr_risk == "elevated" ||
      (session.last_safe_hdr.has_value() && !session.last_safe_hdr.value() && degraded_history);
    const bool host_render_limited =
      pacing_risk &&
      !network_risk &&
      !capture_fallback &&
      !decoder_risk &&
      !virtual_display_risk &&
      !hdr_risk;

    if (session.last_network_risk.empty()) {
      session.last_network_risk = network_risk ? "elevated" : "normal";
    }
    if (session.last_decoder_risk.empty()) {
      session.last_decoder_risk = decoder_risk ? "elevated" : "normal";
    }
    if (session.last_hdr_risk.empty()) {
      session.last_hdr_risk = hdr_risk ? "elevated" : "normal";
    }
    if (session.last_capture_path.empty()) {
      session.last_capture_path = "unknown";
    }

    if (session.last_primary_issue.empty()) {
      if (network_risk) session.last_primary_issue = "network_jitter";
      else if (hdr_risk) session.last_primary_issue = "hdr_path";
      else if (virtual_display_risk) session.last_primary_issue = "virtual_display_path";
      else if (decoder_risk) session.last_primary_issue = "decoder_path";
      else if (capture_fallback) session.last_primary_issue = "capture_fallback";
      else if (host_render_limited) session.last_primary_issue = "host_render_limited";
      else if (pacing_risk) session.last_primary_issue = "frame_pacing";
      else session.last_primary_issue = "steady";
    }

    if (session.last_issues.empty()) {
      if (network_risk) session.last_issues.push_back("network_jitter");
      if (host_render_limited) session.last_issues.push_back("host_render_limited");
      else if (pacing_risk) session.last_issues.push_back("frame_pacing");
      if (capture_fallback) session.last_issues.push_back("capture_fallback");
      if (hdr_risk) session.last_issues.push_back("hdr_path");
      if (decoder_risk) session.last_issues.push_back("decoder_path");
      if (virtual_display_risk) session.last_issues.push_back("virtual_display_path");
    }

    if (session.last_health_grade.empty()) {
      const int concern_count =
        static_cast<int>(network_risk) +
        static_cast<int>(pacing_risk) +
        static_cast<int>(capture_fallback) +
        static_cast<int>(hdr_risk) +
        static_cast<int>(decoder_risk) +
        static_cast<int>(virtual_display_risk);
      session.last_health_grade =
        concern_count >= 2 || hdr_risk || decoder_risk ? "degraded" :
        concern_count == 1 ? "watch" :
        "good";
    }

    const int baseline_bitrate_kbps =
      session.last_bitrate_kbps > 0 ? session.last_bitrate_kbps :
      device_profile ? device_profile->ideal_bitrate_kbps :
      15000;
    if (session.last_safe_bitrate_kbps <= 0) {
      session.last_safe_bitrate_kbps = derive_safe_bitrate_kbps(
        baseline_bitrate_kbps,
        device_profile,
        degraded_history || session.last_health_grade == "degraded"
      );
    }

    if (session.last_safe_codec.empty()) {
      auto safe_codec = active_codec_family.empty() ? std::optional<std::string> {} : std::optional<std::string> {active_codec_family};
      if (decoder_risk || (mobile_client && active_codec_family == "av1")) {
        safe_codec = device_db::normalize_preferred_codec(
          device_name,
          app_name,
          std::optional<std::string> {std::string {"hevc"}},
          session.last_safe_bitrate_kbps,
          false
        );
        if (!safe_codec) {
          safe_codec = "hevc";
        }
      }
      if (safe_codec) {
        session.last_safe_codec = *safe_codec;
      }
    }

    if (session.last_safe_display_mode.empty()) {
      if (virtual_display_risk || config::video.linux_display.headless_mode) {
        session.last_safe_display_mode = "headless";
      } else if (session.last_capture_path == "virtual_display") {
        session.last_safe_display_mode = degraded_history ? "headless" : "virtual_display";
      } else if (device_profile && device_profile->virtual_display && !degraded_history && !mobile_client) {
        session.last_safe_display_mode = "virtual_display";
      } else {
        session.last_safe_display_mode = "headless";
      }
    }

    if (session.last_safe_target_fps <= 0.0 &&
        launched_with_history_safe &&
        target_fps > 0.0 &&
        target_fps <= 30.0) {
      session.last_safe_target_fps = target_fps;
    }

    if (session.last_safe_target_fps <= 0.0) {
      session.last_safe_target_fps = derive_safe_target_fps(
        target_fps,
        optimizer_fps,
        session.last_low_1_percent_fps,
        session.last_min_fps,
        session.last_frame_pacing_bad_pct,
        mobile_client,
        degraded_history,
        pacing_risk
      );
    }

    if (!session.last_safe_hdr.has_value()) {
      if (hdr_risk) {
        session.last_safe_hdr = false;
      } else if (device_profile && device_profile->hdr_capable && !degraded_history) {
        session.last_safe_hdr = true;
      }
    }

    session.last_relaunch_recommended =
      session.last_relaunch_recommended ||
      decoder_risk ||
      hdr_risk ||
      virtual_display_risk ||
      (session.last_safe_target_fps > 0.0 &&
       target_fps > 0.0 &&
       session.last_safe_target_fps < target_fps);
  }

  static config_t resolved_config(config_t config) {
    config.provider = normalize_provider(config.provider);
    config.model = normalize_model(config.provider, config.model);
    config.base_url = normalize_base_url(config.provider, config.base_url);
    config.auth_mode = normalize_auth_mode(config.auth_mode, config.provider, config.use_subscription, config.api_key);
    if (!provider_supports_subscription(config.provider)) {
      config.use_subscription = false;
    }
    return config;
  }

  static bool is_config_enabled(const config_t &active_cfg) {
    if (!active_cfg.enabled || active_cfg.model.empty()) return false;
    if (provider_supports_subscription(active_cfg.provider)) {
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

  static session_history_t merge_history_entries(const session_history_t &raw_lhs, const session_history_t &raw_rhs) {
    const auto lhs = sanitize_session_history(raw_lhs);
    const auto rhs = sanitize_session_history(raw_rhs);
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
    merged.last_duration_s = latest.last_duration_s;
    merged.last_sample_count = latest.last_sample_count;
    merged.last_low_1_percent_fps = latest.last_low_1_percent_fps;
    merged.last_min_fps = latest.last_min_fps;
    merged.last_frame_pacing_bad_pct = latest.last_frame_pacing_bad_pct;
    merged.last_end_reason = latest.last_end_reason;
    merged.last_sample_confidence = latest.last_sample_confidence;
    merged.last_feedback_ignored_reason = latest.last_feedback_ignored_reason;
    merged.consecutive_poor_outcomes = latest.consecutive_poor_outcomes;
    merged.last_optimization_source = latest.last_optimization_source;
    merged.last_optimization_confidence = latest.last_optimization_confidence;
    merged.last_recommendation_version = latest.last_recommendation_version;
    merged.last_health_grade = latest.last_health_grade.empty() ? merged.last_health_grade : latest.last_health_grade;
    merged.last_primary_issue = latest.last_primary_issue.empty() ? merged.last_primary_issue : latest.last_primary_issue;
    merged.last_issues = latest.last_issues.empty() ? merged.last_issues : latest.last_issues;
    merged.last_decoder_risk = latest.last_decoder_risk.empty() ? merged.last_decoder_risk : latest.last_decoder_risk;
    merged.last_hdr_risk = latest.last_hdr_risk.empty() ? merged.last_hdr_risk : latest.last_hdr_risk;
    merged.last_network_risk = latest.last_network_risk.empty() ? merged.last_network_risk : latest.last_network_risk;
    merged.last_capture_path = latest.last_capture_path.empty() ? merged.last_capture_path : latest.last_capture_path;
    merged.last_safe_bitrate_kbps = latest.last_safe_bitrate_kbps > 0 ? latest.last_safe_bitrate_kbps : merged.last_safe_bitrate_kbps;
    merged.last_safe_codec = latest.last_safe_codec.empty() ? merged.last_safe_codec : latest.last_safe_codec;
    merged.last_safe_display_mode = latest.last_safe_display_mode.empty() ? merged.last_safe_display_mode : latest.last_safe_display_mode;
    merged.last_safe_target_fps = latest.last_safe_target_fps > 0.0 ? latest.last_safe_target_fps : merged.last_safe_target_fps;
    merged.last_safe_hdr = latest.last_safe_hdr.has_value() ? latest.last_safe_hdr : merged.last_safe_hdr;
    merged.last_relaunch_recommended = latest.last_relaunch_recommended || merged.last_relaunch_recommended;
    merged.last_updated_at = latest.last_updated_at;
    return sanitize_session_history(merged);
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

  static std::string display_mode_with_fps(const std::string &value, int target_fps) {
    int width = 0;
    int height = 0;
    int fps = 0;
    if (!parse_display_mode(value, width, height, fps)) {
      return value;
    }
    target_fps = std::clamp(target_fps, 24, 240);
    return std::to_string(width) + "x" + std::to_string(height) + "x" + std::to_string(target_fps);
  }

  static bool display_mode_fps_at_or_below(const std::optional<std::string> &display_mode,
                                           double target_fps,
                                           double tolerance_fps = 0.5) {
    if (!display_mode || target_fps <= 0.0) {
      return false;
    }

    int width = 0;
    int height = 0;
    int fps = 0;
    return parse_display_mode(*display_mode, width, height, fps) &&
      static_cast<double>(fps) <= target_fps + tolerance_fps;
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
    std::optional<session_history_t> sanitized_history;
    if (history) {
      sanitized_history = sanitize_session_history(*history);
    }
    if (fallback.source != "device_db" && device_name.empty()) {
      confidence = "low";
    }
    if (!game_category.empty() && game_category != "unknown") {
      confidence = confidence == "low" ? "medium" : confidence;
    }
    if (sanitized_history) {
      const auto health_grade = to_lower_copy(sanitized_history->last_health_grade);
      if (sanitized_history->poor_outcome_count > 0 ||
          sanitized_history->consecutive_poor_outcomes > 0 ||
          health_grade == "degraded" ||
          sanitized_history->last_relaunch_recommended) {
        confidence = "low";
      } else if (health_grade == "watch" && confidence == "high") {
        confidence = "medium";
      } else {
        const auto grade = latest_quality_grade(*sanitized_history);
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
    std::optional<session_history_t> sanitized_history;
    if (history) {
      sanitized_history = sanitize_session_history(*history);
    }
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
    const double effective_safe_target_fps =
      sanitized_history ? effective_history_safe_target_fps(device_name, *sanitized_history) : 0.0;
    if (sanitized_history &&
        effective_safe_target_fps > 0.0 &&
        !should_relax_history_safe_target_fps(*sanitized_history) &&
        optimization.display_mode) {
      int width = 0;
      int height = 0;
      int fps = 0;
      const int safe_fps = static_cast<int>(std::round(effective_safe_target_fps));
      if (parse_display_mode(*optimization.display_mode, width, height, fps) &&
          safe_fps > 0 &&
          safe_fps < fps) {
        optimization.display_mode = display_mode_with_fps(*optimization.display_mode, safe_fps);
        normalization_notes.push_back("Lowered stream FPS from session pacing feedback.");
        normalized = true;
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
      sanitized_history,
      fallback,
      normalized,
      from_cache
    );

    optimization.signals_used.clear();
    push_signal(optimization.signals_used, fallback.source == "device_db" ? "device_profile" : "safe_defaults");
    if (!game_category.empty() && game_category != "unknown") {
      push_signal(optimization.signals_used, "game_category");
    }
    if (sanitized_history && sanitized_history->session_count > 0) {
      push_signal(optimization.signals_used, "session_history");
      if (!sanitized_history->last_primary_issue.empty() || !sanitized_history->last_health_grade.empty()) {
        push_signal(optimization.signals_used, "reliability_feedback");
      }
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
      "If previous session scored A, keep current settings. If B/C, adjust. "
      "If confirmed or repeated previous session feedback scored D/F, significantly reduce quality; low-confidence soft-end observations should be treated as watch signals only.";
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
      const auto prompt_history = sanitize_session_history(*history);
      std::ostringstream history_stream;
      history_stream << "Previous session stats (feedback for improvement):\n";
      if (prompt_history.last_fps > 0 || prompt_history.last_latency_ms > 0 || !latest_quality_grade(prompt_history).empty()) {
        history_stream << "  Latest session: target "
                       << static_cast<int>(std::round(prompt_history.last_target_fps > 0 ? prompt_history.last_target_fps : prompt_history.last_fps))
                       << " FPS, achieved " << static_cast<int>(std::round(prompt_history.last_fps))
                       << " FPS, 1% low " << static_cast<int>(std::round(prompt_history.last_low_1_percent_fps))
                       << " FPS, min " << static_cast<int>(std::round(prompt_history.last_min_fps))
                       << " FPS, bad pacing " << prompt_history.last_frame_pacing_bad_pct
                       << "%, latency " << static_cast<int>(std::round(prompt_history.last_latency_ms))
                       << "ms, bitrate " << prompt_history.last_bitrate_kbps
                       << "kbps, packet loss " << prompt_history.last_packet_loss_pct
                       << "%, grade " << latest_quality_grade(prompt_history)
                       << ", codec " << latest_codec(prompt_history) << "\n";
      }
      history_stream << "  Rolling history: " << prompt_history.session_count
                     << " sessions, Avg FPS " << static_cast<int>(std::round(prompt_history.avg_fps))
                     << ", Avg Latency " << static_cast<int>(std::round(prompt_history.avg_latency_ms))
                     << "ms, Avg Bitrate " << prompt_history.avg_bitrate_kbps
                     << "kbps, Avg Packet Loss " << prompt_history.packet_loss_pct
                     << "%, Poor Outcomes " << prompt_history.poor_outcome_count
                     << ", Last Confidence " << prompt_history.last_optimization_confidence << "\n";

      const bool has_reliability_context =
        !prompt_history.last_health_grade.empty() ||
        !prompt_history.last_primary_issue.empty() ||
        !prompt_history.last_issues.empty() ||
        !prompt_history.last_safe_codec.empty() ||
        prompt_history.last_safe_bitrate_kbps > 0 ||
        !prompt_history.last_safe_display_mode.empty() ||
        prompt_history.last_safe_target_fps > 0.0 ||
        prompt_history.last_safe_hdr.has_value();

      if (has_reliability_context) {
        history_stream << "  Reliability diagnosis: health="
                       << (prompt_history.last_health_grade.empty() ? "unknown" : prompt_history.last_health_grade)
                       << ", primary issue="
                       << (prompt_history.last_primary_issue.empty() ? "steady" : prompt_history.last_primary_issue);
        if (!prompt_history.last_issues.empty()) {
          history_stream << ", issue tags=";
          for (std::size_t i = 0; i < prompt_history.last_issues.size(); ++i) {
            if (i > 0) {
              history_stream << "/";
            }
            history_stream << prompt_history.last_issues[i];
          }
        }
        history_stream << "\n";

        history_stream << "  Risk levels: network="
                       << (prompt_history.last_network_risk.empty() ? "unknown" : prompt_history.last_network_risk)
                       << ", decoder="
                       << (prompt_history.last_decoder_risk.empty() ? "unknown" : prompt_history.last_decoder_risk)
                       << ", hdr="
                       << (prompt_history.last_hdr_risk.empty() ? "unknown" : prompt_history.last_hdr_risk)
                       << ", capture="
                       << (prompt_history.last_capture_path.empty() ? "unknown" : prompt_history.last_capture_path)
                       << "\n";

        history_stream << "  Safe fallback bias: bitrate="
                       << (prompt_history.last_safe_bitrate_kbps > 0 ?
                             std::to_string(prompt_history.last_safe_bitrate_kbps) + "kbps" :
                             std::string {"unchanged"})
                       << ", codec="
                       << (prompt_history.last_safe_codec.empty() ? "unchanged" : prompt_history.last_safe_codec)
                       << ", display="
                       << (prompt_history.last_safe_display_mode.empty() ? "unchanged" : prompt_history.last_safe_display_mode)
                       << ", target_fps="
                       << (prompt_history.last_safe_target_fps > 0.0 ?
                             std::to_string(static_cast<int>(std::round(prompt_history.last_safe_target_fps))) :
                             std::string {"unchanged"})
                       << ", hdr=";
        if (prompt_history.last_safe_hdr.has_value()) {
          history_stream << (*prompt_history.last_safe_hdr ? "keep" : "off");
        } else {
          history_stream << "unchanged";
        }
        history_stream << ", relaunch="
                       << (prompt_history.last_relaunch_recommended ? "yes" : "no")
                       << "\n";
      }
      if (is_low_confidence_soft_end_without_confirming_signal(prompt_history)) {
        history_stream << "  Feedback confidence: low soft-end observation only; do not apply recovery, resolution, or FPS downgrades from this sample unless future confirmed poor outcomes repeat.\n";
      }

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
      "If the last session degraded, bias the recommendation toward the safe fallback path and only keep a higher-end codec, HDR, or virtual display when the evidence strongly supports it.\n"
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
    const std::string home = getenv("HOME") ? getenv("HOME") : "/home";
    const auto claude_bin = resolve_existing_binary(
      {
        home + "/.local/bin/claude",
        "/usr/local/bin/claude",
        "/usr/bin/claude",
      },
      "claude");

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
      "--model '" + shell_escape_single_quotes(anthropic_cli_model(active_cfg)) + "' "
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

  // ---- OpenAI Codex CLI (subscription mode) ----

  static std::optional<device_db::optimization_t> call_openai_codex_cli(
      const config_t &active_cfg,
      const std::string &device_name,
      const std::string &app_name,
      const std::string &gpu_info,
      const std::string &game_category = "",
      const std::optional<session_history_t> &history = std::nullopt) {

    const std::string home = getenv("HOME") ? getenv("HOME") : "/home";
    const auto codex_bin = resolve_existing_binary(
      {
        home + "/.local/bin/codex",
        "/usr/local/bin/codex",
        "/usr/bin/codex",
      },
      "codex");

    auto prompt_file = std::filesystem::temp_directory_path() / "polaris_codex_prompt.txt";
    auto output_file = std::filesystem::temp_directory_path() / "polaris_codex_output.txt";
    {
      std::ofstream pf(prompt_file);
      pf << build_system_prompt() << "\n\n"
         << build_user_prompt(device_name, app_name, gpu_info, game_category, history) << "\n\n"
         << "Return only the JSON object in the requested schema.";
    }

    std::string cmd =
      "'" + codex_bin + "' exec "
      "--skip-git-repo-check "
      "--ignore-user-config "
      "--ignore-rules "
      "--sandbox read-only "
      "-C /tmp "
      "-m '" + shell_escape_single_quotes(active_cfg.model) + "' "
      "-o '" + shell_escape_single_quotes(output_file.string()) + "' "
      "- < '" + shell_escape_single_quotes(prompt_file.string()) + "' "
      "2>/dev/null";

    BOOST_LOG(debug) << "ai_optimizer: Running Codex CLI for "sv << device_name;
    const auto result = run_command_capture(cmd);

    std::filesystem::remove(prompt_file);

    std::string output;
    if (std::filesystem::exists(output_file)) {
      std::ifstream out_file(output_file);
      output.assign(std::istreambuf_iterator<char>(out_file), std::istreambuf_iterator<char>());
      std::filesystem::remove(output_file);
    }

    if (result.exit_code != 0) {
      BOOST_LOG(error) << "ai_optimizer: codex CLI exited with code "sv << result.exit_code;
      if (!result.output.empty()) {
        BOOST_LOG(error) << "ai_optimizer: Codex CLI output: "sv << result.output.substr(0, 200);
      }
      return std::nullopt;
    }

    if (output.empty()) {
      BOOST_LOG(error) << "ai_optimizer: codex CLI returned empty output"sv;
      return std::nullopt;
    }

    try {
      return parse_optimization_json(output, "Codex CLI");
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "ai_optimizer: Failed to parse Codex CLI response: "sv << e.what();
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

    if (active_cfg.provider == PROVIDER_OPENAI && active_cfg.auth_mode == AUTH_SUBSCRIPTION) {
      return call_openai_codex_cli(active_cfg, device_name, app_name, gpu_info, game_category, history);
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
    const bool history_repaired = load_history();
    if (history_repaired) {
      save_history();
      BOOST_LOG(info) << "ai_optimizer: Repaired invalid session history values during load"sv;
    }
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

  bool should_sync_on_cache_miss() {
    return is_config_enabled(cfg) && cfg.auth_mode == AUTH_SUBSCRIPTION;
  }

  void set_enabled(bool enabled) {
    cfg.enabled = enabled;
    config::video.ai_optimizer.enabled = enabled;
    BOOST_LOG(info) << "ai_optimizer: "sv << (enabled ? "enabled" : "disabled") << " at runtime"sv;
  }

  std::optional<device_db::optimization_t> get_cached(
      const std::string &device_name, const std::string &app_name) {
    const auto key = current_cache_key(device_name, app_name);
    std::optional<cache_entry_t> entry;
    {
      std::lock_guard<std::mutex> lock(cache_mutex);
      auto it = cache.find(key);
      if (it == cache.end()) return std::nullopt;

      const auto now = unix_time_now();
      if (it->second.optimization.expires_at > 0 && now >= it->second.optimization.expires_at) {
        cache.erase(it);
        save_cache_locked();
        return std::nullopt;
      }
      entry = it->second;
    }

    if (auto history = get_session_history(device_name, app_name);
        history && should_refresh_recovery_cache_after_stable_trial(*history, entry->optimization)) {
      std::lock_guard<std::mutex> lock(cache_mutex);
      auto it = cache.find(key);
      if (it != cache.end()) {
        cache.erase(it);
        save_cache_locked();
      }
      BOOST_LOG(info) << "ai_optimizer: Retired stale recovery cache for "sv
                      << history_key(device_name, app_name)
                      << " after stable "sv
                      << static_cast<int>(std::round(history->last_target_fps))
                      << " FPS trial; requesting a higher-quality recommendation"sv;
      return std::nullopt;
    }

    auto optimization = entry->optimization;
    optimization.source = "ai_cached";
    optimization.cache_status = "hit";
    return optimization;
  }

  std::optional<device_db::optimization_t> get_history_safe_fallback(
      const std::string &device_name,
      const std::string &app_name,
      const std::optional<session_history_t> &history) {
    const auto effective_history = history ? history : get_session_history(device_name, app_name);
    if (!effective_history) {
      return std::nullopt;
    }

    const auto session = sanitize_session_history(*effective_history);
    if (session.session_count <= 0) {
      return std::nullopt;
    }
    const auto latest_grade = latest_quality_grade(session);
    const auto last_health_grade = to_lower_copy(session.last_health_grade);
    if (is_unconfirmed_poor_recovery_feedback(session)) {
      return std::nullopt;
    }

    const bool has_safe_bias =
      session.last_safe_bitrate_kbps > 0 ||
      !session.last_safe_codec.empty() ||
      session.last_safe_hdr.has_value() ||
      !session.last_safe_display_mode.empty() ||
      session.last_safe_target_fps > 0.0;

    const auto primary_issue = to_lower_copy(session.last_primary_issue);
    const bool meaningful_primary_issue =
      !primary_issue.empty() && primary_issue != "steady";
    const bool high_confidence_sample =
      to_lower_copy(session.last_sample_confidence) == "high" ||
      has_confirming_soft_end_recovery_signal(session);
    const bool host_render_only_relaunch =
      session.last_relaunch_recommended &&
      (primary_issue == "host_render_limited" || primary_issue == "host_render") &&
      session.poor_outcome_count <= 0 &&
      session.consecutive_poor_outcomes <= 0 &&
      !high_confidence_sample;
    const bool reliability_pressure =
      session.poor_outcome_count > 0 ||
      session.consecutive_poor_outcomes > 0 ||
      (session.last_relaunch_recommended && !host_render_only_relaunch) ||
      (
        high_confidence_sample &&
        (
          last_health_grade == "watch" ||
          last_health_grade == "degraded" ||
          meaningful_primary_issue
        )
      );

    const bool reuse_recent_success =
      (latest_grade == "A" || latest_grade == "B") &&
      (session.last_bitrate_kbps > 0 || !session.last_codec.empty());

    if ((!has_safe_bias && !reuse_recent_success) ||
        (!reliability_pressure && !reuse_recent_success)) {
      return std::nullopt;
    }

    auto fallback = fallback_optimization_for_device(device_name, app_name);
    device_db::optimization_t optimization;
    optimization.source = "history_safe";
    optimization.cache_status = "fallback";
    optimization.generated_at = unix_time_now();
    optimization.recommendation_version = OPTIMIZATION_SCHEMA_VERSION;
    optimization.signals_used = {"session_history", "reliability_feedback"};

    if (session.last_safe_bitrate_kbps > 0) {
      optimization.target_bitrate_kbps = clamp_value(session.last_safe_bitrate_kbps, 2000, 100000);
    } else if (reuse_recent_success && session.last_bitrate_kbps > 0) {
      optimization.target_bitrate_kbps = clamp_value(session.last_bitrate_kbps, 2000, 100000);
    }

    if (!session.last_safe_codec.empty()) {
      optimization.preferred_codec = to_lower_copy(session.last_safe_codec);
    } else if (reuse_recent_success && !session.last_codec.empty()) {
      optimization.preferred_codec = to_lower_copy(session.last_codec);
    }

    if (session.last_safe_hdr.has_value()) {
      optimization.hdr = session.last_safe_hdr;
    }

    if (session.last_safe_display_mode == "headless") {
      optimization.virtual_display = false;
    } else if (session.last_safe_display_mode == "virtual_display") {
      optimization.virtual_display = true;
    }

    const bool relax_safe_target_fps = should_relax_history_safe_target_fps(session);
    const double effective_safe_target_fps = effective_history_safe_target_fps(device_name, session);
    if (effective_safe_target_fps > 0.0 && !relax_safe_target_fps) {
      const auto base_display_mode =
        optimization.display_mode.value_or(fallback.display_mode.value_or("1280x720x60"));
      optimization.display_mode = display_mode_with_fps(
        base_display_mode,
        static_cast<int>(std::round(effective_safe_target_fps))
      );
    }

    optimization.confidence =
      (latest_grade == "A" || latest_grade == "B") && !reliability_pressure ? "medium" : "low";

    std::ostringstream reasoning;
    reasoning << "Using recent session fallback";
    if (!latest_grade.empty()) {
      reasoning << " after grade " << latest_grade;
    }
    if (!session.last_primary_issue.empty()) {
      reasoning << " (" << session.last_primary_issue << ")";
    }
    reasoning << " while waiting for a refreshed AI recommendation.";
    if (optimization.preferred_codec) {
      reasoning << " Keep codec " << *optimization.preferred_codec << '.';
    }
    if (optimization.target_bitrate_kbps) {
      reasoning << " Hold bitrate near " << *optimization.target_bitrate_kbps << "kbps.";
    }
    if (effective_safe_target_fps > 0.0) {
      if (relax_safe_target_fps) {
        reasoning << " Relax the prior "
                  << static_cast<int>(std::round(effective_safe_target_fps))
                  << " FPS cap because that safe-target trial already ran; retry a smoother profile.";
      } else {
        reasoning << " Target " << static_cast<int>(std::round(effective_safe_target_fps))
                  << " FPS on the next launch to avoid visible frame pacing drops.";
      }
    }
    optimization.reasoning = reasoning.str();

    optimization = normalize_optimization(
      cfg,
      device_name,
      app_name,
      "",
      effective_history,
      optimization,
      false
    );
    optimization.source = "history_safe";
    optimization.cache_status = "fallback";
    return optimization;
  }

  bool should_relax_history_safe_target_fps(const session_history_t &raw_session) {
    const auto session = sanitize_session_history(raw_session);
    if (session.last_safe_target_fps <= 0.0 ||
        session.last_target_fps <= 0.0) {
      return false;
    }

    const auto confidence = to_lower_copy(session.last_sample_confidence);
    const auto end_reason = normalize_end_reason(session.last_end_reason);
    const bool soft_end_has_enough_signal =
      session.last_duration_s >= 60 &&
      session.last_sample_count >= 30;
    const bool explicit_completed_end =
      end_reason == "end" ||
      end_reason == "user_end" ||
      end_reason == "quit";
    const bool completed_safe_trial =
      explicit_completed_end ||
      soft_end_has_enough_signal;
    const bool low_confidence_soft_end =
      confidence == "low" &&
      (end_reason == "host_pause" || end_reason == "disconnect" || end_reason == "cancelled") &&
      !soft_end_has_enough_signal;
    const auto grade = latest_quality_grade(session);
    const auto normalized_grade = to_lower_copy(grade);
    const bool poor_grade_signal = normalized_grade == "d" || normalized_grade == "f";
    const bool frame_pacing_issue = has_frame_or_host_pacing_issue(session);
    if (session.last_target_fps > session.last_safe_target_fps + 0.5) {
      if (low_confidence_soft_end && (poor_grade_signal || frame_pacing_issue)) {
        return false;
      }
      return low_confidence_soft_end && session.consecutive_poor_outcomes <= 0;
    }

    if (!completed_safe_trial) {
      return false;
    }

    const bool severe_grade =
      !low_confidence_soft_end &&
      poor_grade_signal;
    const double fps_ratio =
      session.last_fps > 0.0 ? std::clamp(session.last_fps / session.last_target_fps, 0.0, 1.5) : 0.0;
    const bool severe_fps_miss = !low_confidence_soft_end && fps_ratio > 0.0 && fps_ratio < 0.82;
    const bool severe_low =
      !low_confidence_soft_end &&
      session.last_low_1_percent_fps > 0.0 &&
      session.last_low_1_percent_fps < session.last_target_fps * 0.65;
    const bool severe_pacing = !low_confidence_soft_end && session.last_frame_pacing_bad_pct >= 18.0;

    return !(severe_grade ||
             severe_fps_miss ||
             severe_low ||
             severe_pacing ||
             session.consecutive_poor_outcomes > 0);
  }

  bool should_graduate_history_safe_target_fps(
      const session_history_t &raw_session,
      double previous_safe_target_fps) {
    const auto session = sanitize_session_history(raw_session);
    if (previous_safe_target_fps <= 0.0 ||
        session.last_target_fps < 55.0 ||
        session.last_target_fps <= previous_safe_target_fps + 15.0) {
      return false;
    }
    if (session.last_duration_s < 60 || session.last_sample_count < 30) {
      return false;
    }

    const double delivered_fps =
      session.last_fps > 0.0 ? session.last_fps : session.avg_fps;
    if (delivered_fps <= 0.0 ||
        delivered_fps < session.last_target_fps * 0.90) {
      return false;
    }
    if (session.avg_fps > 0.0 &&
        session.avg_fps < session.last_target_fps * 0.90) {
      return false;
    }
    if (session.last_low_1_percent_fps > 0.0 &&
        session.last_low_1_percent_fps < session.last_target_fps * 0.65) {
      return false;
    }
    if (session.last_min_fps > 0.0 &&
        session.last_min_fps < session.last_target_fps * 0.45) {
      return false;
    }
    if (session.last_frame_pacing_bad_pct >= 25.0 ||
        session.last_packet_loss_pct >= 0.35 ||
        session.last_latency_ms >= 28.0) {
      return false;
    }

    const auto elevated = [](const std::string &risk) {
      return to_lower_copy(risk) == "elevated";
    };
    if (elevated(session.last_network_risk) ||
        elevated(session.last_decoder_risk) ||
        elevated(session.last_hdr_risk)) {
      return false;
    }

    const auto capture_path = to_lower_copy(session.last_capture_path);
    if (capture_path.find("cpu") != std::string::npos ||
        capture_path.find("shm") != std::string::npos ||
        capture_path.find("fallback") != std::string::npos) {
      return false;
    }

    return true;
  }

  bool should_refresh_recovery_cache_after_stable_trial(
      const session_history_t &raw_session,
      const device_db::optimization_t &optimization) {
    const auto session = sanitize_session_history(raw_session);
    if (session.last_target_fps <= 0.0 || session.last_target_fps > 45.0) {
      return false;
    }

    const auto grade = to_lower_copy(latest_quality_grade(session));
    if (grade != "a" && grade != "b") {
      return false;
    }
    if (session.last_duration_s < 120 || session.last_sample_count < 60) {
      return false;
    }
    if (session.last_fps <= 0.0 || session.last_fps < session.last_target_fps * 0.95) {
      return false;
    }
    if (session.last_low_1_percent_fps > 0.0 &&
        session.last_low_1_percent_fps < session.last_target_fps * 0.90) {
      return false;
    }
    if (session.last_min_fps > 0.0 &&
        session.last_min_fps < session.last_target_fps * 0.85) {
      return false;
    }
    if (session.last_frame_pacing_bad_pct >= 3.0 ||
        session.last_packet_loss_pct >= 0.35 ||
        session.last_latency_ms >= 28.0 ||
        session.consecutive_poor_outcomes > 0) {
      return false;
    }
    if (!display_mode_fps_at_or_below(optimization.display_mode, session.last_target_fps)) {
      return false;
    }

    const auto confidence = to_lower_copy(optimization.confidence);
    const auto reasoning = to_lower_copy(optimization.reasoning);
    const auto normalization = to_lower_copy(optimization.normalization_reason);
    const bool recovery_capped =
      confidence == "low" ||
      normalization.find("lowered stream fps") != std::string::npos ||
      reasoning.find("pacing feedback") != std::string::npos ||
      reasoning.find("safe-target") != std::string::npos ||
      reasoning.find("safe target") != std::string::npos ||
      reasoning.find("recovery") != std::string::npos;

    return recovery_capped;
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

    if (active_cfg.auth_mode == AUTH_SUBSCRIPTION) {
      output["error"] = "Live model discovery is unavailable for " + subscription_cli_label(active_cfg.provider) + " subscription mode.";
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

    if (cfg.auth_mode == AUTH_SUBSCRIPTION) {
      const auto cli_binary = subscription_cli_binary(cfg.provider);
      const auto cli_label = subscription_cli_label(cfg.provider);
      const auto login_command = subscription_login_command(cfg.provider);
      int rc = system(("command -v " + cli_binary + " >/dev/null 2>&1").c_str());
      status["subscription_cli"] = cli_label;
      status["cli_binary"] = cli_binary;
      status["cli_available"] = (rc == 0);
      if (!login_command.empty()) {
        status["cli_login_command"] = login_command;
      }
      if (cfg.provider == PROVIDER_OPENAI && status["cli_available"].get<bool>()) {
        auto authenticated = codex_login_ready();
        if (authenticated.has_value()) {
          status["cli_authenticated"] = *authenticated;
        }
      }
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

  static bool load_history() {
    std::lock_guard<std::mutex> lock(history_mutex);
    std::ifstream file(history_path());
    if (!file.is_open()) return false;
    bool repaired_history = false;
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
        h.last_duration_s = val.value("last_duration_s", 0);
        h.last_sample_count = val.value("last_sample_count", 0);
        h.last_low_1_percent_fps = val.value("last_low_1_percent_fps", 0.0);
        h.last_min_fps = val.value("last_min_fps", 0.0);
        h.last_frame_pacing_bad_pct = val.value("last_frame_pacing_bad_pct", 0.0);
        h.last_end_reason = val.value("last_end_reason", "");
        h.last_sample_confidence = val.value("last_sample_confidence", "");
        h.last_feedback_ignored_reason = val.value("last_feedback_ignored_reason", "");
        h.poor_outcome_count = val.value("poor_outcome_count", 0);
        h.consecutive_poor_outcomes = val.value("consecutive_poor_outcomes", 0);
        h.last_optimization_source = val.value("last_optimization_source", "");
        h.last_optimization_confidence = val.value("last_optimization_confidence", "");
        h.last_recommendation_version = val.value("last_recommendation_version", 0);
        h.last_health_grade = val.value("last_health_grade", "");
        h.last_primary_issue = val.value("last_primary_issue", "");
        h.last_issues = val.value("last_issues", std::vector<std::string> {});
        h.last_decoder_risk = val.value("last_decoder_risk", "");
        h.last_hdr_risk = val.value("last_hdr_risk", "");
        h.last_network_risk = val.value("last_network_risk", "");
        h.last_capture_path = val.value("last_capture_path", "");
        h.last_safe_bitrate_kbps = val.value("last_safe_bitrate_kbps", 0);
        h.last_safe_codec = val.value("last_safe_codec", "");
        h.last_safe_display_mode = val.value("last_safe_display_mode", "");
        h.last_safe_target_fps = val.value("last_safe_target_fps", 0.0);
        if (val.contains("last_safe_hdr") && val["last_safe_hdr"].is_boolean()) {
          h.last_safe_hdr = val["last_safe_hdr"].get<bool>();
        }
        h.last_relaunch_recommended = val.value("last_relaunch_recommended", false);
        h.last_updated_at = val.value("last_updated_at", 0);
        h.last_invalidated_at = val.value("last_invalidated_at", 0);
        auto pos = key.find(':');
        const auto device_name = pos == std::string::npos ? key : key.substr(0, pos);
        const auto app_name = pos == std::string::npos ? std::string {} : key.substr(pos + 1);
        auto before_repair = h;
        h = sanitize_session_history(h);
        repaired_history = repaired_history || history_repair_should_persist(before_repair, h);
        before_repair = h;
        enrich_session_reliability_context(device_name, app_name, h);
        h = sanitize_session_history(h);
        repaired_history = repaired_history || history_repair_should_persist(before_repair, h);
        const auto normalized_key = history_key(device_name, app_name);
        auto existing = session_history.find(normalized_key);
        if (existing == session_history.end()) {
          session_history[normalized_key] = h;
        } else {
          existing->second = merge_history_entries(existing->second, h);
        }
      }
    } catch (...) {
      return false;
    }
    return repaired_history;
  }

  static void save_history() {
    std::lock_guard<std::mutex> lock(history_mutex);
    nlohmann::json root;
    for (const auto &[key, h] : session_history) {
      nlohmann::json entry = {
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
        {"last_duration_s", h.last_duration_s},
        {"last_sample_count", h.last_sample_count},
        {"last_low_1_percent_fps", h.last_low_1_percent_fps},
        {"last_min_fps", h.last_min_fps},
        {"last_frame_pacing_bad_pct", h.last_frame_pacing_bad_pct},
        {"last_end_reason", h.last_end_reason},
        {"last_sample_confidence", h.last_sample_confidence},
        {"last_feedback_ignored_reason", h.last_feedback_ignored_reason},
        {"poor_outcome_count", h.poor_outcome_count},
        {"consecutive_poor_outcomes", h.consecutive_poor_outcomes},
        {"last_optimization_source", h.last_optimization_source},
        {"last_optimization_confidence", h.last_optimization_confidence},
        {"last_recommendation_version", h.last_recommendation_version},
        {"last_health_grade", h.last_health_grade},
        {"last_primary_issue", h.last_primary_issue},
        {"last_issues", h.last_issues},
        {"last_decoder_risk", h.last_decoder_risk},
        {"last_hdr_risk", h.last_hdr_risk},
        {"last_network_risk", h.last_network_risk},
        {"last_capture_path", h.last_capture_path},
        {"last_safe_bitrate_kbps", h.last_safe_bitrate_kbps},
        {"last_safe_codec", h.last_safe_codec},
        {"last_safe_display_mode", h.last_safe_display_mode},
        {"last_safe_target_fps", h.last_safe_target_fps},
        {"last_relaunch_recommended", h.last_relaunch_recommended},
        {"last_updated_at", h.last_updated_at},
        {"last_invalidated_at", h.last_invalidated_at}
      };
      if (h.last_safe_hdr.has_value()) {
        entry["last_safe_hdr"] = h.last_safe_hdr.value();
      }
      root[key] = std::move(entry);
    }
    std::ofstream file(history_path());
    if (file.is_open()) file << root.dump(2);
  }

  void record_session(const std::string &device_name,
                      const std::string &app_name,
                      const session_history_t &session) {
    auto normalized_session = session;
    if (normalized_session.last_target_fps <= 0.0 && normalized_session.avg_fps > 0.0) {
      normalized_session.last_target_fps = normalized_session.avg_fps;
    }
    const auto canonical_device = canonical_device_name(device_name);
    enrich_session_reliability_context(canonical_device, app_name, normalized_session);
    auto feedback_policy = classify_session_feedback(app_name, normalized_session);
    normalized_session.last_end_reason = feedback_policy.end_reason;
    normalized_session.last_sample_confidence = feedback_policy.confidence;
    normalized_session.last_feedback_ignored_reason = feedback_policy.ignored_reason;
    normalized_session = sanitize_session_history(normalized_session);

    auto key = history_key(canonical_device, app_name);
    if (!feedback_policy.accepted) {
      BOOST_LOG(info) << "ai_optimizer: Ignoring low-signal session report for "sv << key
                      << " end_reason="sv << normalized_session.last_end_reason
                      << " reason="sv << feedback_policy.ignored_reason;
      return;
    }

    const bool poor_quality = is_poor_quality_grade(normalized_session.quality_grade);
    const bool recovery_feedback_allowed = !poor_quality || feedback_policy.counts_poor_outcome;
    int session_count_total = 0;
    bool invalidate_cache = false;
    bool graduated_safe_target = false;
    double graduated_previous_safe_target_fps = 0.0;
    {
      std::lock_guard<std::mutex> lock(history_mutex);
      auto &existing = session_history[key];
      existing = sanitize_session_history(existing);
      const bool had_existing = existing.session_count > 0;
      const int preserved_safe_bitrate_kbps = existing.last_safe_bitrate_kbps;
      const std::string preserved_safe_codec = existing.last_safe_codec;
      const std::string preserved_safe_display_mode = existing.last_safe_display_mode;
      const double preserved_safe_target_fps = existing.last_safe_target_fps;
      const std::optional<bool> preserved_safe_hdr = existing.last_safe_hdr;
      const bool preserved_relaunch_recommended = existing.last_relaunch_recommended;
      graduated_safe_target =
        had_existing &&
        should_graduate_history_safe_target_fps(
          normalized_session,
          preserved_safe_target_fps
        );
      if (graduated_safe_target) {
        graduated_previous_safe_target_fps = preserved_safe_target_fps;
      }

      // Rolling average with the new session
      if (had_existing) {
        double n = existing.session_count;
        existing.avg_fps = (existing.avg_fps * n + normalized_session.avg_fps) / (n + 1);
        existing.avg_latency_ms = (existing.avg_latency_ms * n + normalized_session.avg_latency_ms) / (n + 1);
        existing.avg_bitrate_kbps = static_cast<int>((existing.avg_bitrate_kbps * n + normalized_session.avg_bitrate_kbps) / (n + 1));
        existing.packet_loss_pct = (existing.packet_loss_pct * n + normalized_session.packet_loss_pct) / (n + 1);
        existing.session_count++;
      } else {
        existing = normalized_session;
        existing.session_count = 1;
      }
      existing.quality_grade = normalized_session.quality_grade;
      existing.codec = normalized_session.codec;
      existing.last_fps = normalized_session.last_fps;
      existing.last_target_fps = normalized_session.last_target_fps;
      existing.last_latency_ms = normalized_session.last_latency_ms;
      existing.last_bitrate_kbps = normalized_session.last_bitrate_kbps;
      existing.last_packet_loss_pct = normalized_session.last_packet_loss_pct;
      existing.last_quality_grade = normalized_session.last_quality_grade;
      existing.last_codec = normalized_session.last_codec;
      existing.last_duration_s = normalized_session.last_duration_s;
      existing.last_sample_count = normalized_session.last_sample_count;
      existing.last_low_1_percent_fps = normalized_session.last_low_1_percent_fps;
      existing.last_min_fps = normalized_session.last_min_fps;
      existing.last_frame_pacing_bad_pct = normalized_session.last_frame_pacing_bad_pct;
      existing.last_end_reason = normalized_session.last_end_reason;
      existing.last_sample_confidence = normalized_session.last_sample_confidence;
      existing.last_feedback_ignored_reason = normalized_session.last_feedback_ignored_reason;
      existing.last_updated_at = unix_time_now();
      existing.last_optimization_source = normalized_session.last_optimization_source;
      existing.last_optimization_confidence = normalized_session.last_optimization_confidence;
      existing.last_recommendation_version = normalized_session.last_recommendation_version;
      existing.last_health_grade = normalized_session.last_health_grade;
      existing.last_primary_issue = normalized_session.last_primary_issue;
      existing.last_issues = normalized_session.last_issues;
      existing.last_decoder_risk = normalized_session.last_decoder_risk;
      existing.last_hdr_risk = normalized_session.last_hdr_risk;
      existing.last_network_risk = normalized_session.last_network_risk;
      existing.last_capture_path = normalized_session.last_capture_path;
      if (recovery_feedback_allowed) {
        existing.last_safe_bitrate_kbps = normalized_session.last_safe_bitrate_kbps;
        existing.last_safe_codec = normalized_session.last_safe_codec;
        existing.last_safe_display_mode = normalized_session.last_safe_display_mode;
        existing.last_safe_target_fps = normalized_session.last_safe_target_fps;
        existing.last_safe_hdr = normalized_session.last_safe_hdr;
        existing.last_relaunch_recommended = normalized_session.last_relaunch_recommended;
      } else if (had_existing) {
        existing.last_safe_bitrate_kbps = preserved_safe_bitrate_kbps;
        existing.last_safe_codec = preserved_safe_codec;
        existing.last_safe_display_mode = preserved_safe_display_mode;
        existing.last_safe_target_fps = preserved_safe_target_fps;
        existing.last_safe_hdr = preserved_safe_hdr;
        existing.last_relaunch_recommended = preserved_relaunch_recommended;
      } else {
        existing.last_safe_bitrate_kbps = 0;
        existing.last_safe_codec.clear();
        existing.last_safe_display_mode.clear();
        existing.last_safe_target_fps = 0.0;
        existing.last_safe_hdr.reset();
        existing.last_relaunch_recommended = false;
      }

      if (graduated_safe_target) {
        existing.poor_outcome_count = 0;
        existing.consecutive_poor_outcomes = 0;
      } else if (poor_quality && feedback_policy.counts_poor_outcome) {
        existing.poor_outcome_count++;
        existing.consecutive_poor_outcomes++;
        invalidate_cache = should_invalidate_cache_for_poor_quality(
          app_name,
          normalized_session,
          feedback_policy,
          existing.consecutive_poor_outcomes
        );
        if (invalidate_cache) {
          existing.last_invalidated_at = unix_time_now();
        }
      } else if (!poor_quality) {
        existing.consecutive_poor_outcomes = 0;
      }

      if (recovery_feedback_allowed) {
        enrich_session_reliability_context(canonical_device, app_name, existing);
      }
      if (graduated_safe_target) {
        existing.last_safe_target_fps = 0.0;
        existing.last_relaunch_recommended = false;
      }
      existing = sanitize_session_history(existing);
      session_count_total = existing.session_count;
    }

    save_history();
    BOOST_LOG(info) << "ai_optimizer: Recorded session for "sv << key
                    << " — grade "sv << normalized_session.quality_grade
                    << " against target "sv << normalized_session.last_target_fps
                    << "fps, confidence="sv << normalized_session.last_sample_confidence
                    << ", end_reason="sv << normalized_session.last_end_reason
                    << ", "sv << session_count_total << " sessions total"sv;

    // Poor game sessions should force re-optimization, but low-signal control shells
    // like Desktop or Steam Big Picture should keep their existing cache entry.
    if (invalidate_cache) {
      std::lock_guard<std::mutex> lock(cache_mutex);
      cache.erase(current_cache_key(canonical_device, app_name));
      save_cache_locked();
      BOOST_LOG(info) << "ai_optimizer: Poor quality — invalidated cache for "sv << key;
    } else if (graduated_safe_target) {
      bool removed_stale_cache = false;
      {
        std::lock_guard<std::mutex> lock(cache_mutex);
        auto it = cache.find(current_cache_key(canonical_device, app_name));
        if (it != cache.end() &&
            display_mode_fps_at_or_below(
              it->second.optimization.display_mode,
              graduated_previous_safe_target_fps
            )) {
          cache.erase(it);
          save_cache_locked();
          removed_stale_cache = true;
        }
      }
      BOOST_LOG(info) << "ai_optimizer: Graduated history-safe FPS cap for "sv << key
                      << " after stable "sv
                      << static_cast<int>(std::round(normalized_session.last_target_fps))
                      << " FPS retry"
                      << (removed_stale_cache ? "; cleared stale cache" : "");
    } else if (poor_quality) {
      BOOST_LOG(info) << "ai_optimizer: Poor quality sample preserved without cache invalidation for "sv << key
                      << " confidence="sv << normalized_session.last_sample_confidence
                      << " end_reason="sv << normalized_session.last_end_reason
                      << (feedback_policy.counts_poor_outcome ? " awaiting_confirming_sample" : " low_signal_or_soft_end");
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

  bool clear_game_profile(const std::string &device_name,
                          const std::string &app_name) {
    if (device_name.empty() || app_name.empty()) {
      return false;
    }

    static std::once_flag history_load_flag;
    std::call_once(history_load_flag, load_history);

    const auto canonical_device = canonical_device_name(device_name);
    const auto session_key = history_key(canonical_device, app_name);
    bool removed_history = false;
    {
      std::lock_guard<std::mutex> lock(history_mutex);
      removed_history = session_history.erase(session_key) > 0;
    }
    if (removed_history) {
      save_history();
    }

    bool removed_cache = false;
    {
      std::lock_guard<std::mutex> lock(cache_mutex);
      for (auto it = cache.begin(); it != cache.end();) {
        if (canonical_device_name(it->second.device_name) == canonical_device &&
            it->second.app_name == app_name) {
          it = cache.erase(it);
          removed_cache = true;
        } else {
          ++it;
        }
      }
      if (removed_cache) {
        save_cache_locked();
      }
    }

    if (removed_history || removed_cache) {
      BOOST_LOG(info) << "ai_optimizer: Cleared game profile for "sv
                      << session_key
                      << " history="sv << (removed_history ? "yes" : "no")
                      << " cache="sv << (removed_cache ? "yes" : "no");
    }

    return removed_history || removed_cache;
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
      entry["last_duration_s"] = h.last_duration_s;
      entry["last_sample_count"] = h.last_sample_count;
      entry["last_low_1_percent_fps"] = h.last_low_1_percent_fps;
      entry["last_min_fps"] = h.last_min_fps;
      entry["last_frame_pacing_bad_pct"] = h.last_frame_pacing_bad_pct;
      entry["last_end_reason"] = h.last_end_reason;
      entry["last_sample_confidence"] = h.last_sample_confidence;
      entry["last_feedback_ignored_reason"] = h.last_feedback_ignored_reason;
      entry["poor_outcome_count"] = h.poor_outcome_count;
      entry["consecutive_poor_outcomes"] = h.consecutive_poor_outcomes;
      entry["last_optimization_source"] = h.last_optimization_source;
      entry["last_optimization_confidence"] = h.last_optimization_confidence;
      entry["last_recommendation_version"] = h.last_recommendation_version;
      entry["last_health_grade"] = h.last_health_grade;
      entry["last_primary_issue"] = h.last_primary_issue;
      entry["last_issues"] = h.last_issues;
      entry["last_decoder_risk"] = h.last_decoder_risk;
      entry["last_hdr_risk"] = h.last_hdr_risk;
      entry["last_network_risk"] = h.last_network_risk;
      entry["last_capture_path"] = h.last_capture_path;
      entry["last_safe_bitrate_kbps"] = h.last_safe_bitrate_kbps;
      entry["last_safe_codec"] = h.last_safe_codec;
      entry["last_safe_display_mode"] = h.last_safe_display_mode;
      entry["last_safe_target_fps"] = h.last_safe_target_fps;
      if (h.last_safe_hdr.has_value()) {
        entry["last_safe_hdr"] = h.last_safe_hdr.value();
      }
      entry["last_relaunch_recommended"] = h.last_relaunch_recommended;
      entry["last_updated_at"] = h.last_updated_at;
      entry["last_invalidated_at"] = h.last_invalidated_at;
      arr.push_back(entry);
    }
    return arr.dump();
  }

  std::string get_profiles_json(const std::string &device_name) {
    static std::once_flag history_load_flag;
    std::call_once(history_load_flag, load_history);

    const auto requested_device = canonical_device_name(device_name);
    std::unordered_map<std::string, nlohmann::json> profiles_by_key;

    {
      std::lock_guard<std::mutex> lock(history_mutex);
      for (const auto &[key, h] : session_history) {
        const auto separator = key.find(':');
        const auto profile_device = separator == std::string::npos ? key : key.substr(0, separator);
        const auto app_name = separator == std::string::npos ? std::string {} : key.substr(separator + 1);
        const auto canonical_device = canonical_device_name(profile_device);
        if (!requested_device.empty() && canonical_device != requested_device) {
          continue;
        }

        auto &entry = profiles_by_key[history_key(canonical_device, app_name)];
        entry["device"] = canonical_device;
        entry["game"] = app_name;
        entry["has_history"] = true;
        entry["can_reset"] = true;
        entry["session_count"] = h.session_count;
        entry["quality_grade"] = latest_quality_grade(h);
        entry["avg_fps"] = h.avg_fps;
        entry["avg_latency_ms"] = h.avg_latency_ms;
        entry["avg_bitrate_kbps"] = h.avg_bitrate_kbps;
        entry["last_fps"] = h.last_fps;
        entry["last_target_fps"] = h.last_target_fps;
        entry["last_low_1_percent_fps"] = h.last_low_1_percent_fps;
        entry["last_min_fps"] = h.last_min_fps;
        entry["last_frame_pacing_bad_pct"] = h.last_frame_pacing_bad_pct;
        entry["last_end_reason"] = h.last_end_reason;
        entry["last_sample_confidence"] = h.last_sample_confidence;
        entry["last_health_grade"] = h.last_health_grade;
        entry["last_primary_issue"] = h.last_primary_issue;
        entry["last_issues"] = h.last_issues;
        entry["poor_outcome_count"] = h.poor_outcome_count;
        entry["consecutive_poor_outcomes"] = h.consecutive_poor_outcomes;
        entry["last_safe_bitrate_kbps"] = h.last_safe_bitrate_kbps;
        entry["last_safe_codec"] = h.last_safe_codec;
        entry["last_safe_display_mode"] = h.last_safe_display_mode;
        entry["last_safe_target_fps"] = h.last_safe_target_fps;
        if (h.last_safe_hdr.has_value()) {
          entry["last_safe_hdr"] = h.last_safe_hdr.value();
        }
        entry["last_relaunch_recommended"] = h.last_relaunch_recommended;
        entry["last_updated_at"] = h.last_updated_at;
        entry["last_invalidated_at"] = h.last_invalidated_at;
      }
    }

    {
      std::lock_guard<std::mutex> lock(cache_mutex);
      for (const auto &[key, entry] : cache) {
        if (entry.provider != cfg.provider || entry.model != cfg.model || entry.base_url != cfg.base_url) {
          continue;
        }
        const auto canonical_device = canonical_device_name(entry.device_name);
        if (!requested_device.empty() && canonical_device != requested_device) {
          continue;
        }

        auto &profile = profiles_by_key[history_key(canonical_device, entry.app_name)];
        profile["device"] = canonical_device;
        profile["game"] = entry.app_name;
        profile["has_cache"] = true;
        profile["can_reset"] = true;
        profile["provider"] = entry.provider;
        profile["model"] = entry.model;
        profile["timestamp"] = entry.timestamp;
        const auto &optimization = entry.optimization;
        profile["source"] = optimization.source;
        profile["cache_status"] = optimization.cache_status;
        profile["confidence"] = optimization.confidence;
        profile["reasoning"] = optimization.reasoning;
        profile["normalization_reason"] = optimization.normalization_reason;
        profile["signals_used"] = optimization.signals_used;
        profile["recommendation_version"] = optimization.recommendation_version;
        profile["generated_at"] = optimization.generated_at;
        profile["expires_at"] = optimization.expires_at;
        if (optimization.display_mode) {
          profile["display_mode"] = *optimization.display_mode;
        }
        if (optimization.target_bitrate_kbps) {
          profile["target_bitrate_kbps"] = *optimization.target_bitrate_kbps;
        }
        if (optimization.preferred_codec) {
          profile["preferred_codec"] = *optimization.preferred_codec;
        }
        if (optimization.hdr.has_value()) {
          profile["hdr"] = *optimization.hdr;
        }
      }
    }

    std::vector<std::string> keys;
    keys.reserve(profiles_by_key.size());
    for (const auto &[key, _] : profiles_by_key) {
      keys.push_back(key);
    }
    std::sort(keys.begin(), keys.end());

    nlohmann::json profiles = nlohmann::json::array();
    for (const auto &key : keys) {
      auto profile = profiles_by_key[key];
      profile["key"] = key;
      profile["has_history"] = profile.value("has_history", false);
      profile["has_cache"] = profile.value("has_cache", false);
      profiles.push_back(std::move(profile));
    }

    nlohmann::json output;
    output["status"] = true;
    output["device"] = requested_device;
    output["count"] = profiles.size();
    output["profiles"] = std::move(profiles);
    return output.dump();
  }

  bool clear_device_profiles(const std::string &device_name) {
    if (device_name.empty()) {
      return false;
    }

    static std::once_flag history_load_flag;
    std::call_once(history_load_flag, load_history);

    const auto canonical_device = canonical_device_name(device_name);
    bool removed_history = false;
    {
      std::lock_guard<std::mutex> lock(history_mutex);
      for (auto it = session_history.begin(); it != session_history.end();) {
        const auto separator = it->first.find(':');
        const auto profile_device = separator == std::string::npos ? it->first : it->first.substr(0, separator);
        if (canonical_device_name(profile_device) == canonical_device) {
          it = session_history.erase(it);
          removed_history = true;
        } else {
          ++it;
        }
      }
    }
    if (removed_history) {
      save_history();
    }

    bool removed_cache = false;
    {
      std::lock_guard<std::mutex> lock(cache_mutex);
      for (auto it = cache.begin(); it != cache.end();) {
        if (canonical_device_name(it->second.device_name) == canonical_device) {
          it = cache.erase(it);
          removed_cache = true;
        } else {
          ++it;
        }
      }
      if (removed_cache) {
        save_cache_locked();
      }
    }

    if (removed_history || removed_cache) {
      BOOST_LOG(info) << "ai_optimizer: Cleared all optimizer profiles for "sv
                      << canonical_device
                      << " history="sv << (removed_history ? "yes" : "no")
                      << " cache="sv << (removed_cache ? "yes" : "no");
    }

    return removed_history || removed_cache;
  }

}  // namespace ai_optimizer
