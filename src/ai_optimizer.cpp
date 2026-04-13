/**
 * @file src/ai_optimizer.cpp
 * @brief Claude API integration for streaming optimization.
 */

#include "ai_optimizer.h"
#include "game_classifier.h"
#include "logging.h"
#include "platform/common.h"

#include <chrono>
#include <cstdio>
#include <fstream>
#include <mutex>
#include <sstream>
#include <thread>
#include <unordered_map>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

using namespace std::literals;

namespace ai_optimizer {

  static config_t cfg;
  static std::mutex cache_mutex;

  struct cache_entry_t {
    device_db::optimization_t optimization;
    int64_t timestamp;  // Unix epoch seconds
  };

  static std::unordered_map<std::string, cache_entry_t> cache;
  static std::unordered_map<std::string, session_history_t> session_history;

  static std::string cache_key(const std::string &device, const std::string &app) {
    return device + ":" + app;
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
        cache[key] = entry;
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

  // ---- Claude CLI (subscription mode) ----

  /**
   * @brief Call Claude via the `claude` CLI using the user's subscription.
   * No API key needed — uses the locally authenticated Claude Code session.
   */
  static std::optional<device_db::optimization_t> call_claude_cli(
      const std::string &device_name,
      const std::string &app_name,
      const std::string &gpu_info,
      const std::string &game_category = "",
      const std::optional<session_history_t> &history = std::nullopt) {

    std::string prompt = build_user_prompt(device_name, app_name, gpu_info, game_category, history);
    std::string system = build_system_prompt();

    // Escape single quotes in the prompt for shell safety
    auto escape_sh = [](const std::string &s) {
      std::string out;
      for (char c : s) {
        if (c == '\'') out += "'\\''";
        else out += c;
      }
      return out;
    };

    // Find claude CLI — check common locations since daemon may lack user's PATH
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

    // Write prompt and system prompt to temp files to avoid shell quoting issues
    auto prompt_file = std::filesystem::temp_directory_path() / "polaris_ai_prompt.txt";
    auto system_file = std::filesystem::temp_directory_path() / "polaris_ai_system.txt";
    {
      std::ofstream pf(prompt_file);
      pf << prompt;
      std::ofstream sf(system_file);
      sf << system;
    }

    // Build the command using temp files for reliable prompt passing
    std::string cmd =
      "'" + claude_bin + "' --print "
      "--model haiku "
      "--system-prompt \"$(cat " + system_file.string() + ")\" "
      "< " + prompt_file.string() + " "
      "2>/dev/null";

    BOOST_LOG(debug) << "ai_optimizer: Running claude CLI for "sv << device_name;

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

    // Clean up temp files
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
      return parse_optimization_json(output, "CLI");
    } catch (const std::exception &e) {
      BOOST_LOG(error) << "ai_optimizer: Failed to parse CLI response: "sv << e.what();
      return std::nullopt;
    }
  }

  // ---- Claude API (API key mode) ----

  /**
   * @brief Call the Claude API directly with an API key.
   */
  static std::optional<device_db::optimization_t> call_claude_api(
      const std::string &device_name,
      const std::string &app_name,
      const std::string &gpu_info,
      const std::string &game_category = "",
      const std::optional<session_history_t> &history = std::nullopt) {

    std::string user_msg = build_user_prompt(device_name, app_name, gpu_info, game_category, history);

    nlohmann::json request_body;
    request_body["model"] = "claude-haiku-4-5-20251001";
    request_body["max_tokens"] = 256;
    request_body["temperature"] = 0;
    request_body["messages"] = nlohmann::json::array({
      {{"role", "user"}, {"content", user_msg}}
    });
    request_body["system"] = build_system_prompt();

    std::string post_data = request_body.dump();
    std::string auth_header = "x-api-key: " + cfg.api_key;

    CURL *curl = curl_easy_init();
    if (!curl) return std::nullopt;

    std::string response_data;
    struct curl_slist *headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, auth_header.c_str());
    headers = curl_slist_append(headers, "anthropic-version: 2023-06-01");

    curl_easy_setopt(curl, CURLOPT_URL, "https://api.anthropic.com/v1/messages");
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, post_data.c_str());
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_callback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response_data);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(cfg.timeout_ms));
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
      BOOST_LOG(error) << "ai_optimizer: API request failed: "sv << curl_easy_strerror(res);
      return std::nullopt;
    }

    if (http_code != 200) {
      BOOST_LOG(error) << "ai_optimizer: API returned HTTP "sv << http_code << ": "sv << response_data.substr(0, 200);
      return std::nullopt;
    }

    try {
      auto api_response = nlohmann::json::parse(response_data);

      // Extract text content from Claude's response
      std::string content_text;
      if (api_response.contains("content") && api_response["content"].is_array()) {
        for (const auto &block : api_response["content"]) {
          if (block.value("type", "") == "text") {
            content_text = block["text"].get<std::string>();
            break;
          }
        }
      }

      if (content_text.empty()) {
        BOOST_LOG(error) << "ai_optimizer: Empty response from Claude API"sv;
        return std::nullopt;
      }

      return parse_optimization_json(content_text, "API");

    } catch (const std::exception &e) {
      BOOST_LOG(error) << "ai_optimizer: Failed to parse API response: "sv << e.what();
      return std::nullopt;
    }
  }

  // ---- Unified dispatch ----

  /**
   * @brief Route to either CLI (subscription) or API (key) based on config.
   */
  static std::optional<device_db::optimization_t> call_claude(
      const std::string &device_name,
      const std::string &app_name,
      const std::string &gpu_info,
      const std::string &game_category = "",
      const std::optional<session_history_t> &history = std::nullopt) {
    if (cfg.use_subscription) {
      return call_claude_cli(device_name, app_name, gpu_info, game_category, history);
    }
    return call_claude_api(device_name, app_name, gpu_info, game_category, history);
  }

  // ---- Public API ----

  void init(const config_t &config) {
    cfg = config;
    load_cache();
    if (cfg.enabled) {
      BOOST_LOG(info) << "ai_optimizer: Enabled with "sv
                      << (cfg.use_subscription ? "subscription"sv : "API key"sv)
                      << ", cache TTL "sv << cfg.cache_ttl_hours << "h"sv;
    }
  }

  bool is_enabled() {
    return cfg.enabled && (!cfg.api_key.empty() || cfg.use_subscription);
  }

  std::optional<device_db::optimization_t> get_cached(
      const std::string &device_name, const std::string &app_name) {
    std::lock_guard<std::mutex> lock(cache_mutex);
    auto key = cache_key(device_name, app_name);
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

    std::thread([device_name, app_name, gpu_info, game_category, history]() {
      auto result = call_claude(device_name, app_name, gpu_info, game_category, history);
      if (result) {
        std::lock_guard<std::mutex> lock(cache_mutex);
        auto now = std::chrono::system_clock::now().time_since_epoch();
        cache[cache_key(device_name, app_name)] = {
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

    auto result = call_claude(device_name, app_name, gpu_info, game_category, history);
    if (result) {
      std::lock_guard<std::mutex> lock(cache_mutex);
      auto now = std::chrono::system_clock::now().time_since_epoch();
      cache[cache_key(device_name, app_name)] = {
        *result,
        std::chrono::duration_cast<std::chrono::seconds>(now).count()
      };
      save_cache_locked();
    }
    return result;
  }

  std::string get_status_json() {
    nlohmann::json status;
    status["enabled"] = cfg.enabled;
    status["has_api_key"] = !cfg.api_key.empty();
    status["use_subscription"] = cfg.use_subscription;
    status["timeout_ms"] = cfg.timeout_ms;
    status["cache_ttl_hours"] = cfg.cache_ttl_hours;
    status["cache_count"] = cache.size();

    // Check if claude CLI is available for subscription mode
    if (cfg.use_subscription) {
      int rc = system("command -v claude >/dev/null 2>&1");
      status["cli_available"] = (rc == 0);
    }

    return status.dump();
  }

  std::string get_cache_json() {
    std::lock_guard<std::mutex> lock(cache_mutex);
    nlohmann::json root = nlohmann::json::object();
    for (const auto &[key, entry] : cache) {
      nlohmann::json val;
      val["timestamp"] = entry.timestamp;
      auto &o = entry.optimization;
      if (o.display_mode) val["display_mode"] = *o.display_mode;
      if (o.target_bitrate_kbps) val["target_bitrate_kbps"] = *o.target_bitrate_kbps;
      if (o.nvenc_tune) val["nvenc_tune"] = *o.nvenc_tune;
      if (o.preferred_codec) val["preferred_codec"] = *o.preferred_codec;
      val["reasoning"] = o.reasoning;
      val["source"] = o.source;
      root[key] = val;
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
    auto key = cache_key(device_name, app_name);
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
      cache.erase(key);
      save_cache_locked();
      BOOST_LOG(info) << "ai_optimizer: Poor quality — invalidated cache for "sv << key;
    }
  }

  std::optional<session_history_t> get_session_history(
      const std::string &device_name, const std::string &app_name) {
    static std::once_flag history_load_flag;
    std::call_once(history_load_flag, load_history);

    auto key = cache_key(device_name, app_name);
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
