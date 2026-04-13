/**
 * @file src/ai_optimizer.cpp
 * @brief Provider-agnostic API integration for streaming optimization.
 */

#include "ai_optimizer.h"
#include "game_classifier.h"
#include "logging.h"
#include "platform/common.h"

#include <cctype>
#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

using namespace std::literals;

namespace ai_optimizer {

  static config_t cfg;
  static std::mutex cache_mutex;

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

  struct http_response_t {
    CURLcode curl_code = CURLE_OK;
    long http_code = 0;
    std::string body;
  };

  static std::unordered_map<std::string, cache_entry_t> cache;
  static std::unordered_map<std::string, session_history_t> session_history;

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

  static std::string history_key(const std::string &device, const std::string &app) {
    return device + ":" + app;
  }

  static std::string cache_key(const std::string &provider,
                               const std::string &model,
                               const std::string &base_url,
                               const std::string &device,
                               const std::string &app) {
    return provider + "\t" + model + "\t" + base_url + "\t" + device + "\t" + app;
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

  static void load_cache() {
    std::lock_guard<std::mutex> lock(cache_mutex);
    std::ifstream file(cache_path());
    if (!file.is_open()) return;

    try {
      auto root = nlohmann::json::parse(file);
      for (auto &[key, val] : root.items()) {
        cache_entry_t entry;
        entry.timestamp = val.value("timestamp", 0);
        entry.provider = normalize_provider(val.value("provider", PROVIDER_ANTHROPIC));
        entry.model = normalize_model(entry.provider, val.value("model", ""));
        entry.base_url = normalize_base_url(entry.provider, val.value("base_url", ""));
        entry.device_name = val.value("device_name", "");
        entry.app_name = val.value("app_name", "");

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

        auto &o = entry.optimization;
        if (val.contains("display_mode")) o.display_mode = val["display_mode"].get<std::string>();
        if (val.contains("color_range")) o.color_range = val["color_range"].get<int>();
        if (val.contains("hdr")) o.hdr = val["hdr"].get<bool>();
        if (val.contains("virtual_display")) o.virtual_display = val["virtual_display"].get<bool>();
        if (val.contains("target_bitrate_kbps")) o.target_bitrate_kbps = val["target_bitrate_kbps"].get<int>();
        if (val.contains("nvenc_tune")) o.nvenc_tune = val["nvenc_tune"].get<int>();
        if (val.contains("preferred_codec")) o.preferred_codec = val["preferred_codec"].get<std::string>();
        o.reasoning = val.value("reasoning", "");
        o.source = "ai_cached";
        cache[cache_key(entry.provider, entry.model, entry.base_url, entry.device_name, entry.app_name)] = entry;
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
      root[key] = val;
    }
    std::ofstream file(cache_path());
    if (file.is_open()) file << root.dump(2);
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
      history_line = "Previous session stats (feedback for improvement):\n"
        "  Sessions: " + std::to_string(history->session_count) +
        ", Avg FPS: " + std::to_string((int)history->avg_fps) +
        ", Avg Latency: " + std::to_string((int)history->avg_latency_ms) + "ms"
        ", Avg Bitrate: " + std::to_string(history->avg_bitrate_kbps) + "kbps"
        ", Packet Loss: " + std::to_string(history->packet_loss_pct) + "%"
        ", Quality Grade: " + history->quality_grade +
        ", Codec Used: " + history->codec + "\n";
    }

    std::string headless_line;
    if (config::video.linux_display.headless_mode) {
      headless_line = "Streaming mode: HEADLESS (virtual display, no physical monitor — "
        "display_mode sets the virtual output resolution, choose optimal for the client device)\n";
    } else {
      headless_line = "Streaming mode: windowed (labwc as Wayland window on desktop)\n";
    }

    std::string adaptive_line;
    if (config::video.adaptive_bitrate.enabled) {
      adaptive_line = "Adaptive bitrate: enabled (server auto-adjusts mid-stream on network degradation)\n";
    }

    return
      "Device: " + device_name + " (" + device_desc + ")\n"
      "Game: " + (app_name.empty() ? "Desktop/General" : app_name) + "\n"
      + category_line + history_line + headless_line + adaptive_line +
      "GPU: " + gpu_info + "\n\n"
      "Respond with ONLY this JSON (fill in values, no other text):\n"
      "{\"display_mode\":\"WIDTHxHEIGHTxFPS\",\"color_range\":0,\"hdr\":false,"
      "\"virtual_display\":true,\"target_bitrate_kbps\":15000,\"nvenc_tune\":2,"
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

  // ---- Public API ----

  void init(const config_t &config) {
    cfg = resolved_config(config);
    load_cache();
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

  std::optional<device_db::optimization_t> get_cached(
      const std::string &device_name, const std::string &app_name) {
    std::lock_guard<std::mutex> lock(cache_mutex);
    auto key = current_cache_key(device_name, app_name);
    auto it = cache.find(key);
    if (it == cache.end()) return std::nullopt;

    // Check TTL
    auto now = std::chrono::system_clock::now().time_since_epoch();
    auto age_hours = (std::chrono::duration_cast<std::chrono::hours>(now).count() - it->second.timestamp / 3600);
    if (age_hours > cfg.cache_ttl_hours) {
      cache.erase(it);
      return std::nullopt;
    }

    return it->second.optimization;
  }

  void request_async(const std::string &device_name,
                     const std::string &app_name,
                     const std::string &gpu_info,
                     const std::string &game_category,
                     const std::optional<session_history_t> &history) {
    if (!is_enabled()) return;
    auto active_cfg = cfg;

    std::thread([active_cfg, device_name, app_name, gpu_info, game_category, history]() {
      auto result = call_provider(active_cfg, device_name, app_name, gpu_info, game_category, history);
      if (result) {
        std::lock_guard<std::mutex> lock(cache_mutex);
        auto now = std::chrono::system_clock::now().time_since_epoch();
        cache[current_cache_key(active_cfg, device_name, app_name)] = {
          active_cfg.provider,
          active_cfg.model,
          active_cfg.base_url,
          device_name,
          app_name,
          *result,
          std::chrono::duration_cast<std::chrono::seconds>(now).count()
        };
        save_cache_locked();
      }
    }).detach();
  }

  std::optional<device_db::optimization_t> request_sync(
      const std::string &device_name,
      const std::string &app_name,
      const std::string &gpu_info,
      const std::string &game_category,
      const std::optional<session_history_t> &history) {
    if (!is_enabled()) return std::nullopt;
    auto active_cfg = cfg;

    auto result = call_provider(active_cfg, device_name, app_name, gpu_info, game_category, history);
    if (result) {
      std::lock_guard<std::mutex> lock(cache_mutex);
      auto now = std::chrono::system_clock::now().time_since_epoch();
      cache[current_cache_key(active_cfg, device_name, app_name)] = {
        active_cfg.provider,
        active_cfg.model,
        active_cfg.base_url,
        device_name,
        app_name,
        *result,
        std::chrono::duration_cast<std::chrono::seconds>(now).count()
      };
      save_cache_locked();
    }
    return result;
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

    return call_provider(active_cfg, device_name, app_name, gpu_info, game_category, history);
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

    return output.dump(2);
  }

  std::string get_status_json() {
    nlohmann::json status;
    status["enabled"] = cfg.enabled;
    status["provider"] = cfg.provider;
    status["model"] = cfg.model;
    status["auth_mode"] = cfg.auth_mode;
    status["base_url"] = cfg.base_url;
    status["has_api_key"] = !cfg.api_key.empty();
    status["use_subscription"] = cfg.auth_mode == AUTH_SUBSCRIPTION;
    status["timeout_ms"] = cfg.timeout_ms;
    status["cache_ttl_hours"] = cfg.cache_ttl_hours;
    status["cache_count"] = cache.size();

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
      val["source"] = o.source;
      root[history_key(entry.device_name, entry.app_name)] = val;
    }
    return root.dump(2);
  }

  void clear_cache() {
    std::lock_guard<std::mutex> lock(cache_mutex);
    cache.clear();
    std::remove(cache_path().c_str());
    BOOST_LOG(info) << "ai_optimizer: Cache cleared"sv;
  }

  // ---- Session history for feedback loop ----

  static std::filesystem::path history_path() {
    return platf::appdata() / "ai_session_history.json";
  }

  static void load_history() {
    std::ifstream file(history_path());
    if (!file.is_open()) return;
    try {
      auto root = nlohmann::json::parse(file);
      for (auto &[key, val] : root.items()) {
        session_history_t h;
        h.avg_fps = val.value("avg_fps", 0.0);
        h.avg_latency_ms = val.value("avg_latency_ms", 0.0);
        h.avg_bitrate_kbps = val.value("avg_bitrate_kbps", 0);
        h.packet_loss_pct = val.value("packet_loss_pct", 0);
        h.quality_grade = val.value("quality_grade", "");
        h.codec = val.value("codec", "");
        h.session_count = val.value("session_count", 0);
        session_history[key] = h;
      }
    } catch (...) {}
  }

  static void save_history() {
    nlohmann::json root;
    for (const auto &[key, h] : session_history) {
      root[key] = {
        {"avg_fps", h.avg_fps},
        {"avg_latency_ms", h.avg_latency_ms},
        {"avg_bitrate_kbps", h.avg_bitrate_kbps},
        {"packet_loss_pct", h.packet_loss_pct},
        {"quality_grade", h.quality_grade},
        {"codec", h.codec},
        {"session_count", h.session_count}
      };
    }
    std::ofstream file(history_path());
    if (file.is_open()) file << root.dump(2);
  }

  void record_session(const std::string &device_name,
                      const std::string &app_name,
                      const session_history_t &session) {
    auto key = history_key(device_name, app_name);
    auto &existing = session_history[key];

    // Rolling average with the new session
    if (existing.session_count > 0) {
      double n = existing.session_count;
      existing.avg_fps = (existing.avg_fps * n + session.avg_fps) / (n + 1);
      existing.avg_latency_ms = (existing.avg_latency_ms * n + session.avg_latency_ms) / (n + 1);
      existing.avg_bitrate_kbps = static_cast<int>((existing.avg_bitrate_kbps * n + session.avg_bitrate_kbps) / (n + 1));
      existing.packet_loss_pct = static_cast<int>((existing.packet_loss_pct * n + session.packet_loss_pct) / (n + 1));
    } else {
      existing = session;
    }
    existing.quality_grade = session.quality_grade;  // always use latest grade
    existing.codec = session.codec;
    existing.session_count++;

    save_history();
    BOOST_LOG(info) << "ai_optimizer: Recorded session for "sv << key
                    << " — grade "sv << session.quality_grade
                    << ", "sv << existing.session_count << " sessions total"sv;

    // If quality is poor (D or F), invalidate cache to force re-optimization
    if (session.quality_grade == "D" || session.quality_grade == "F") {
      std::lock_guard<std::mutex> lock(cache_mutex);
      cache.erase(current_cache_key(device_name, app_name));
      save_cache_locked();
      BOOST_LOG(info) << "ai_optimizer: Poor quality — invalidated cache for "sv << key;
    }
  }

  std::optional<session_history_t> get_session_history(
      const std::string &device_name, const std::string &app_name) {
    static std::once_flag history_load_flag;
    std::call_once(history_load_flag, load_history);

    auto key = history_key(device_name, app_name);
    auto it = session_history.find(key);
    if (it == session_history.end()) return std::nullopt;
    return it->second;
  }

  std::string get_history_json() {
    static std::once_flag history_load_flag;
    std::call_once(history_load_flag, load_history);

    nlohmann::json arr = nlohmann::json::array();
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
      arr.push_back(entry);
    }
    return arr.dump();
  }

}  // namespace ai_optimizer
