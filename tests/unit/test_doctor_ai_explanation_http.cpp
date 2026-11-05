/**
 * @file tests/unit/test_doctor_ai_explanation_http.cpp
 * @brief The paired Doctor AI explanation request (#613), from strict input to bounded output.
 *
 * The route cases drive the production POST handler, scope reader, request store and worker on a
 * real HTTPS server with real paired client certificates and the production session, stream and
 * controller state. Only the I/O boundary is fake: a counting provider, a fake clock, a localhost
 * HTTP endpoint and a fake Claude CLI. No case reaches a real AI provider or the network.
 */
#include "../tests_common.h"

#include <src/adaptive_bitrate.h>
#include <src/ai_optimizer.h>
#include <src/config.h>
#include <src/doctor_actions.h>
#include <src/doctor_ai_explanation.h>
#include <src/nvhttp.h>
#include <src/platform/common.h>
#include <src/stream_stats.h>

#include <Simple-Web-Server/server_http.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <limits>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <tuple>
#include <vector>

#ifndef _WIN32
  #include <unistd.h>
#endif

namespace {
  namespace dae = doctor_ai_explanation;
  using namespace std::chrono_literals;
  namespace fs = std::filesystem;

  nlohmann::json row(const char *id, const char *label, nlohmann::json value, const char *unit,
                     const char *status, const char *source, const char *detail) {
    return {{"id", id}, {"label", label}, {"value", std::move(value)}, {"unit", unit},
            {"status", status}, {"source", source}, {"redacted", false}, {"detail", detail}};
  }

  // A final served Doctor in the production shape, with a canary in every field the projection excludes.
  nlohmann::json synthetic_doctor() {
    return {
      {"version", 2},
      {"result_id", "doctor-v2-needs_action-network_jitter-shm_capture-1a2b3c"},
      {"scope", "stream"},
      {"status", "needs_action"},
      {"traffic_light", "amber"},
      {"status_color", "amber"},
      {"severity", "warning"},
      {"simple_state", "Needs attention canary"},
      {"primary_issue", "network_jitter"},
      {"confidence", {{"level", "medium"}, {"score", 0.68}, {"basis", "live_evidence_recheck"},
                      {"sample_window", {{"samples", 12}, {"seconds", 10}}}}},
      {"summary", "Canary summary from 10.1.2.3 at /" "home/canary-user/secret"},
      {"recommendation", "Canary recommendation token=sk-canary-0123456789"},
      {"evidence", nlohmann::json::array({
        row("streaming", "Active stream", true, "", "pass", "stream_stats", "A stream is active."),
        row("running_binary", "Running binary", "/" "home/canary-user/bin/polaris", "", "pass", "process",
            "Polaris is running from /" "home/canary-user/bin/polaris."),
        row("capture_path", "Capture path", "shm Canary-GPU", "", "watch", "stream_stats", "canary capture detail"),
        row("encoder", "Encoder", "Canary RTX 9090 /dev/dri/renderD128", "", "pass", "stream_stats", "canary"),
        row("packet_loss", "Video frame loss", 2.5, "%", "fail", "media_transport", "canary loss detail"),
        row("control_channel_packet_loss", "Control-channel loss estimate", 7.0, "%", "watch",
            "enet_control_channel", "canary control detail"),
        row("latency", "Network latency", 42.0, "ms", "pass", "stream_stats", "canary rtt"),
        row("client_family", "Client", "nova", "", "info", "pairing_record", "Canary-Phone is Nova"),
        row("client_network_path", "Client network path", "tailscale canary.ts.net 100.64.0.9", "", "info",
            "stream_stats", "canary path"),
        row("bitrate", "Live bitrate", 18000, "kbps", "watch", "stream_stats", "canary bitrate detail"),
        row("live_bitrate_owner", "Live bitrate owner", "doctor_available", "", "pass", "deterministic_controller", "canary"),
        row("live_bitrate_control", "Live bitrate control", true, "", "pass", "encoder_capability", "canary"),
        row("target_fps_gap", "Target FPS gap", 3.5, "FPS", "pass", "stream_stats", "canary"),
        row("frame_pacing", "Mean target interval error", 1.25, "ms", "unknown", "stream_stats", "canary"),
      })},
      {"advanced_evidence", {
        {"health", {{"client_ip", "192.168.77.88"}, {"device_name", "Canary-Phone"}}},
        {"linux_gpu_profile", {{"device", "/dev/dri/canary"}}},
        {"pacing_coverage", {{"ready", true}, {"confirmed", false}, {"available", true}, {"sample_count", 9}}},
        {"raw_fields_redacted", true},
      }},
      {"safe_recovery_action", {
        {"id", "lower_bitrate"}, {"label", "Lower bitrate canary"}, {"capability", "auto_fix"}, {"kind", "bitrate"},
        {"destructive", false}, {"requires_confirmation", false}, {"requires_owner", true},
        {"allowed_in_viewer_mode", false}, {"owner_tuning_allowed", false},
        {"payload_preview", {{"app_session_id", "canary-app-session"}, {"session_generation", 7},
                             {"controller_revision", 9}, {"evidence_revision", 33}}},
      }},
      {"redaction", {{"policy", "polaris-diagnostics-redaction-v1"}}},
      {"ai_explanation", {{"enabled", false}, {"provider", "canary-provider"}}},
      {"contract", "doctor_v1_observational"},
    };
  }

  nlohmann::json *row_of(nlohmann::json &doctor, std::string_view id) {
    for (auto &entry : doctor["evidence"]) {
      if (entry["id"] == id) return &entry;
    }
    return nullptr;
  }

  nlohmann::json valid_explanation() {
    return {
      {"likely_cause", "The captured Doctor reading identifies network pressure."},
      {"evidence", nlohmann::json::array({"The captured media-loss reading was available to deterministic Doctor."})},
      {"try_first", nlohmann::json::array({"Review the existing Doctor evidence and available controls."})},
      {"advanced_detail", "This is a plain explanation of the captured reading; measurements may continue to change."},
      {"confidence", "medium"},
      {"destructive_action_allowed", false},
    };
  }

  nlohmann::json baseline_body() {
    return {
      {"schema", 1},
      {"request_id", "11111111-1111-4111-8111-111111111111"},
      {"app_session_id", "synthetic-app-session"},
      {"session_generation", 7},
      {"source_result_id", "doctor-v2-needs_action-network_jitter-shm_capture-1a2b3c"},
      {"finding_fingerprint", std::string(64, 'a')},
      {"controller_revision", 9},
    };
  }

  std::string request_uuid(int n) {
    char buffer[40];
    std::snprintf(buffer, sizeof(buffer), "11111111-1111-4111-8111-%012d", n);
    return buffer;
  }

  bool contains_ci(const std::string &haystack, const std::string &needle) {
    auto lower = [](std::string text) {
      for (auto &c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
      return text;
    };
    return lower(haystack).find(lower(needle)) != std::string::npos;
  }

  const std::vector<std::string> k_canaries {
    "canary", "10.1.2.3", "192.168.77.88", "100.64.0.9", "/" "home/", "sk-canary", "renderD128", "ts.net",
    "Canary-Phone", "lower bitrate canary",
  };
}  // namespace

// ---- Strict request admission (no host state is read) ----

TEST(DoctorAiExplanationParse, TheContractBaselineIsAcceptedExactly) {
  const auto parsed = dae::parse_request("application/json", "", baseline_body().dump());
  ASSERT_TRUE(parsed.request);
  EXPECT_EQ(parsed.request->request_id, "11111111-1111-4111-8111-111111111111");
  EXPECT_EQ(parsed.request->session_generation, 7u);
  EXPECT_EQ(parsed.request->controller_revision, 9u);
  EXPECT_TRUE(dae::parse_request("application/json; charset=utf-8", "", baseline_body().dump()).request);
}

TEST(DoctorAiExplanationParse, EveryCoercionUnknownKeyAndOverreachIsRefused) {
  struct refusal_t { std::string label; std::string body; dae::code_e code; std::string type = "application/json"; std::string query; };
  std::vector<refusal_t> cases;
  const auto with = [](const char *key, nlohmann::json value) { auto body = baseline_body(); body[key] = std::move(value); return body.dump(); };
  const auto without = [](const char *key) { auto body = baseline_body(); body.erase(key); return body.dump(); };
  for (const char *forged : {"provider", "model", "endpoint", "base_url", "executable", "api_key", "credentials",
                             "evidence", "prompt", "text", "enabled", "ai_enabled", "timeout_ms"}) {
    cases.push_back({std::string {"forged "} + forged, with(forged, "forged"), dae::code_e::invalid_request});
  }
  for (const char *key : {"schema", "request_id", "app_session_id", "session_generation", "source_result_id",
                          "finding_fingerprint", "controller_revision"}) {
    cases.push_back({std::string {"missing "} + key, without(key), dae::code_e::invalid_request});
  }
  cases.push_back({"schema 2", with("schema", 2), dae::code_e::invalid_request});
  cases.push_back({"schema 1.0", R"({"schema":1.0,"request_id":"11111111-1111-4111-8111-111111111111","app_session_id":"a","session_generation":7,"source_result_id":"","finding_fingerprint":")" + std::string(64, 'a') + R"(","controller_revision":9})", dae::code_e::invalid_request});
  cases.push_back({"schema true", with("schema", true), dae::code_e::invalid_request});
  cases.push_back({"generation string", with("session_generation", "7"), dae::code_e::invalid_request});
  cases.push_back({"generation fraction", with("session_generation", 7.5), dae::code_e::invalid_request});
  cases.push_back({"generation zero", with("session_generation", 0), dae::code_e::invalid_request});
  cases.push_back({"generation negative", with("session_generation", -7), dae::code_e::invalid_request});
  cases.push_back({"generation above 2^53-1", with("session_generation", 9007199254740992ULL), dae::code_e::invalid_request});
  cases.push_back({"controller zero", with("controller_revision", 0), dae::code_e::invalid_request});
  cases.push_back({"uppercase uuid", with("request_id", "11111111-1111-4111-8111-11111111111A"), dae::code_e::invalid_request});
  cases.push_back({"uuid v1", with("request_id", "11111111-1111-1111-8111-111111111111"), dae::code_e::invalid_request});
  cases.push_back({"short fingerprint", with("finding_fingerprint", std::string(63, 'a')), dae::code_e::invalid_request});
  cases.push_back({"uppercase fingerprint", with("finding_fingerprint", std::string(64, 'A')), dae::code_e::invalid_request});
  cases.push_back({"empty app session", with("app_session_id", ""), dae::code_e::invalid_request});
  cases.push_back({"long app session", with("app_session_id", std::string(257, 'a')), dae::code_e::invalid_request});
  cases.push_back({"long source id", with("source_result_id", std::string(257, 'a')), dae::code_e::invalid_request});
  cases.push_back({"nested object", with("app_session_id", nlohmann::json {{"a", 1}}), dae::code_e::invalid_request});
  cases.push_back({"array body", "[1]", dae::code_e::invalid_request});
  cases.push_back({"trailing json", baseline_body().dump() + "{}", dae::code_e::invalid_request});
  auto invalid_utf8 = baseline_body().dump();
  invalid_utf8.replace(invalid_utf8.find("synthetic-app-session"), std::string_view {"synthetic-app-session"}.size(), "\xff");
  cases.push_back({"invalid utf-8", invalid_utf8, dae::code_e::invalid_request});
  auto duplicate = baseline_body().dump();
  duplicate.insert(1, R"("controller_revision":10,)");
  cases.push_back({"duplicate key", duplicate, dae::code_e::invalid_request});
  cases.push_back({"query string", baseline_body().dump(), dae::code_e::invalid_request, "application/json", "provider=x"});
  cases.push_back({"text/plain", baseline_body().dump(), dae::code_e::unsupported_media_type, "text/plain"});
  cases.push_back({"missing type", baseline_body().dump(), dae::code_e::unsupported_media_type, ""});
  cases.push_back({"latin-1 charset", baseline_body().dump(), dae::code_e::unsupported_media_type, "application/json; charset=latin1"});
  cases.push_back({"1025 bytes", std::string(1025, ' '), dae::code_e::body_too_large});
  for (const auto &refusal : cases) {
    SCOPED_TRACE(refusal.label);
    const auto parsed = dae::parse_request(refusal.type, refusal.query, refusal.body);
    EXPECT_FALSE(parsed.request);
    EXPECT_EQ(parsed.code, refusal.code);
  }
}

// ---- The meaningful finding and the provider projection ----

TEST(DoctorAiExplanationFinding, NumericDriftKeepsTheFingerprintAndMeaningfulChangesMoveIt) {
  const auto base = dae::finding_of(synthetic_doctor());
  ASSERT_TRUE(base.supported);
  ASSERT_EQ(base.fingerprint.size(), 64u);
  EXPECT_EQ(base.source_result_id, "doctor-v2-needs_action-network_jitter-shm_capture-1a2b3c");

  auto drift = synthetic_doctor();
  (*row_of(drift, "packet_loss"))["value"] = 2.9;
  (*row_of(drift, "latency"))["value"] = 57.0;
  (*row_of(drift, "bitrate"))["value"] = 16500;
  (*row_of(drift, "frame_pacing"))["value"] = 4.75;
  (*row_of(drift, "packet_loss"))["detail"] = "different prose";
  drift["confidence"]["score"] = 0.71;
  drift["confidence"]["sample_window"]["samples"] = 40;
  drift["summary"] = "New summary text";
  drift["safe_recovery_action"]["payload_preview"]["evidence_revision"] = 34;
  drift["ai_explanation_request"] = {{"schema", 1}};
  EXPECT_EQ(dae::finding_of(drift).fingerprint, base.fingerprint);

  std::vector<std::pair<std::string, std::function<void(nlohmann::json &)>>> meaningful {
    {"status", [](auto &d) { d["status"] = "ok"; }},
    {"primary issue", [](auto &d) { d["primary_issue"] = "encoder_load"; }},
    {"traffic light", [](auto &d) { d["traffic_light"] = "red"; }},
    {"severity", [](auto &d) { d["severity"] = "critical"; }},
    {"confidence level", [](auto &d) { d["confidence"]["level"] = "high"; }},
    {"confidence basis", [](auto &d) { d["confidence"]["basis"] = "direct_telemetry"; }},
    {"doctor_v2 overlay", [](auto &d) { d["derived_from"] = "doctor_v2"; }},
    {"loss row status", [](auto &d) { (*row_of(d, "packet_loss"))["status"] = "pass"; }},
    {"loss became unavailable", [](auto &d) { (*row_of(d, "packet_loss"))["value"] = nullptr; (*row_of(d, "packet_loss"))["source"] = "unavailable"; }},
    {"live bitrate owner", [](auto &d) { (*row_of(d, "live_bitrate_owner"))["value"] = "fixed_session"; }},
    {"action id", [](auto &d) { d["safe_recovery_action"]["id"] = "none"; }},
    {"action permission", [](auto &d) { d["safe_recovery_action"]["owner_tuning_allowed"] = true; }},
  };
  for (auto &[label, change] : meaningful) {
    SCOPED_TRACE(label);
    auto changed = synthetic_doctor();
    change(changed);
    const auto finding = dae::finding_of(changed);
    ASSERT_TRUE(finding.supported);
    EXPECT_NE(finding.fingerprint, base.fingerprint);
  }

  // A missing legacy result ID still binds the finding; malformed meaningful fields disable the request.
  auto no_id = synthetic_doctor();
  no_id.erase("result_id");
  EXPECT_TRUE(dae::finding_of(no_id).supported);
  EXPECT_EQ(dae::finding_of(no_id).source_result_id, "");
  auto malformed = synthetic_doctor();
  malformed["primary_issue"] = "Network Jitter From Canary 10.0.0.1";
  EXPECT_FALSE(dae::finding_of(malformed).supported);
  EXPECT_TRUE(dae::finding_of(malformed).fingerprint.empty());
}

TEST(DoctorAiExplanationProjection, OnlyFixedFieldsTypedRowsAndHostTemplatesReachTheProvider) {
  const auto projected = dae::project_evidence(synthetic_doctor(), {.capture_reason = "shm_capture", .observed_encoder_kbps = 17500});
  ASSERT_TRUE(projected);
  EXPECT_LE(projected->size(), dae::k_max_evidence_bytes);
  for (const auto &canary : k_canaries) {
    EXPECT_FALSE(contains_ci(*projected, canary)) << canary << " reached the provider input";
  }
  const auto evidence = nlohmann::json::parse(*projected);
  std::set<std::string> top;
  for (const auto &[key, _] : evidence.items()) top.insert(key);
  EXPECT_EQ(top, (std::set<std::string> {"schema", "deterministic_source_of_truth", "doctor"}));
  std::set<std::string> doctor_keys;
  for (const auto &[key, _] : evidence["doctor"].items()) doctor_keys.insert(key);
  EXPECT_EQ(doctor_keys, (std::set<std::string> {"evidence", "capture_reason", "live_bitrate_owner", "pacing_coverage", "limits"}));
  const auto &rows = evidence["doctor"]["evidence"];
  ASSERT_EQ(rows.size(), 6u);
  std::vector<std::string> ids;
  for (const auto &entry : rows) {
    std::set<std::string> keys;
    for (const auto &[key, _] : entry.items()) keys.insert(key);
    EXPECT_EQ(keys, (std::set<std::string> {"id", "status", "source", "value", "unit"}));
    ids.push_back(entry["id"]);
  }
  EXPECT_EQ(ids, (std::vector<std::string> {"bitrate", "control_channel_packet_loss", "frame_pacing", "latency", "packet_loss", "target_fps_gap"}));
  EXPECT_EQ(rows[0]["value"], 17500) << "bitrate is the observed encoder rate, not the Doctor's live target";
  EXPECT_EQ(rows[1]["source"], "enet_control_channel");
  EXPECT_EQ(rows[1]["value"], 7.0);
  EXPECT_EQ(rows[4]["source"], "media_transport");
  EXPECT_EQ(rows[4]["value"], 2.5);
  EXPECT_EQ(evidence["doctor"]["capture_reason"], "shm_capture");
  EXPECT_EQ(evidence["doctor"]["live_bitrate_owner"], "doctor_available");
  EXPECT_EQ(evidence["deterministic_source_of_truth"]["primary_issue"], "network_jitter");

  // Missing media loss stays unavailable rather than zero; control loss is never relabeled media loss.
  auto missing = synthetic_doctor();
  (*row_of(missing, "packet_loss"))["value"] = nullptr;
  (*row_of(missing, "packet_loss"))["source"] = "unavailable";
  (*row_of(missing, "latency"))["value"] = std::numeric_limits<double>::infinity();
  (*row_of(missing, "frame_pacing"))["value"] = "12";
  const auto unavailable = nlohmann::json::parse(*dae::project_evidence(missing, {.capture_reason = "bogus reason 10.1.1.1"}));
  const auto &missing_rows = unavailable["doctor"]["evidence"];
  for (const auto *id : {"packet_loss", "latency", "frame_pacing", "bitrate"}) {
    SCOPED_TRACE(id);
    const auto found = std::find_if(missing_rows.begin(), missing_rows.end(), [&](const auto &entry) { return entry["id"] == id; });
    ASSERT_NE(found, missing_rows.end());
    EXPECT_TRUE((*found)["value"].is_null());
    EXPECT_EQ((*found)["source"], "unavailable");
    EXPECT_EQ((*found)["status"], "unknown");
  }
  EXPECT_EQ(unavailable["doctor"]["capture_reason"], "unknown");
  const auto control = std::find_if(missing_rows.begin(), missing_rows.end(), [](const auto &entry) { return entry["id"] == "control_channel_packet_loss"; });
  EXPECT_EQ((*control)["source"], "enet_control_channel");
}

// ---- Output bounds and redaction ----

TEST(DoctorAiExplanationSanitizer, BoundsAndPlainTextFailClosedInsteadOfTruncating) {
  const std::vector<std::string> none;
  ASSERT_TRUE(dae::sanitize_explanation(valid_explanation(), none));
  std::vector<std::pair<std::string, std::function<void(nlohmann::json &)>>> refusals {
    {"extra key", [](auto &e) { e["endpoint"] = "x"; }},
    {"missing key", [](auto &e) { e.erase("advanced_detail"); }},
    {"destructive", [](auto &e) { e["destructive_action_allowed"] = true; }},
    {"confidence grade", [](auto &e) { e["confidence"] = "certain"; }},
    {"evidence type", [](auto &e) { e["evidence"] = "one line"; }},
    {"evidence item type", [](auto &e) { e["evidence"] = nlohmann::json::array({1}); }},
    {"seven evidence", [](auto &e) { e["evidence"] = nlohmann::json::array({"a", "b", "c", "d", "e", "f", "g"}); }},
    {"four try_first", [](auto &e) { e["try_first"] = nlohmann::json::array({"a", "b", "c", "d"}); }},
    {"513 code points", [](auto &e) { e["likely_cause"] = std::string(513, 'x'); }},
    {"385 code points", [](auto &e) { e["evidence"] = nlohmann::json::array({std::string(385, 'x')}); }},
    {"1537 code points", [](auto &e) { e["advanced_detail"] = std::string(1537, 'x'); }},
    {"control character", [](auto &e) { e["likely_cause"] = std::string {"bell\x07"}; }},
    {"escape sequence", [](auto &e) { e["likely_cause"] = std::string {"\x1b[31mred"}; }},
    {"bidi override", [](auto &e) { e["likely_cause"] = "abc\u202Edef"; }},
    {"invalid utf-8", [](auto &e) { e["likely_cause"] = std::string {"\xff\xfe"}; }},
  };
  for (auto &[label, change] : refusals) {
    SCOPED_TRACE(label);
    auto explanation = valid_explanation();
    change(explanation);
    EXPECT_FALSE(dae::sanitize_explanation(explanation, none));
  }
  // Code points, not bytes: 512 two-byte characters are within likely_cause's bound.
  auto wide = valid_explanation();
  std::string accented;
  for (int i = 0; i < 512; ++i) accented += "\xc3\xa9";
  wide["likely_cause"] = accented;
  EXPECT_TRUE(dae::sanitize_explanation(wide, none));
  // Setting words and numbers stay prose: the result is never parsed into a control or value.
  auto advice = valid_explanation();
  advice["try_first"] = nlohmann::json::array({"Lower the bitrate to 15 Mbps or try 60 FPS."});
  const auto kept = dae::sanitize_explanation(advice, none);
  ASSERT_TRUE(kept);
  EXPECT_EQ((*kept)["try_first"][0], "Lower the bitrate to 15 Mbps or try 60 FPS.");
}

TEST(DoctorAiExplanationSanitizer, ReflectedIdentitiesAddressesPathsAndCredentialsAreRedacted) {
  const std::vector<std::string> identities {"Canary Owner Phone", "canary-app-session-9d1f", "Canary Secret Title"};
  auto explanation = valid_explanation();
  explanation["likely_cause"] = "Canary Owner Phone on 192.168.1.20 or fe80::1 lost frames playing Canary Secret Title.";
  explanation["evidence"] = nlohmann::json::array({
    "Logs at /" "home/fictional-user/.config/polaris/polaris.log and C:\\Users\\fictional-user\\AppData mention api_key=abc123.",
    "Authorization: Bearer abc123 was sent to https://user:pw@host.lan/x for session canary-app-session-9d1f.",
    "MAC 00:1a:2b:3c:4d:5e, email fictional-user@example.com, host gaming-pc.local, token sk-live-0123456789abcdef.",
  });
  explanation["advanced_detail"] = "Key 0123456789abcdef0123456789abcdef0123456789abcdef and 2001:db8:0:0:1:2:3:4.";
  const auto clean = dae::sanitize_explanation(explanation, identities);
  ASSERT_TRUE(clean);
  const auto text = clean->dump();
  for (const auto *leak : {"Canary Owner Phone", "Canary Secret Title", "canary-app-session-9d1f", "192.168.1.20", "fe80::1",
                           "/" "home/fictional-user", "C:\\\\Users", "abc123", "user:pw", "host.lan", "00:1a:2b", "fictional-user@example.com",
                           "gaming-pc.local", "sk-live", "0123456789abcdef0123", "2001:db8"}) {
    EXPECT_EQ(text.find(leak), std::string::npos) << leak;
  }
  EXPECT_NE(text.find("[redacted"), std::string::npos);
}

TEST(DoctorAiExplanationSanitizer, ReusesTheWebRedactionShapeVectors) {
  // From src_assets/common/assets/web/redaction-shape-cases.test.js: the secret must not survive.
  for (const auto *input : {
         "auth_token=\"abc123\"", "api_key='abc123'", "api_key: \"abc123\"", "token=[abc123]", "Warning: token=[abc123]",
         "api_key=a=b=c=abc123", "auth_token=abc123:extra", "api_key=https://x.test/?q=abc123", "api_key  =  abc123",
         "api_key\t=\tabc123", "api_key:abc123", "{\"api_key\": \"abc123\"}", "\"api_key\": abc123", "\"api_key\"=abc123",
         "{'api_key': 'abc123'}", "(api_key): abc123", "[api_key]: abc123", "{\"a\": {\"b\": {\"api_key\": \"abc123\"}}}",
         "{\"secret\": {\"inner\": \"abc123\"}}", "{\"tokens\": [\"abc123\", \"x\"]}", "keyboard=us auth_token=abc123",
         "keyName=capture_path client_secret=abc123", "first=ok auth_token=abc123 second=ok apikey=abc123",
         "[2026-08-17 20:31:02.123]: Info: config: api_key = abc123", "polaris[591466]: Warning: request failed apiKey=abc123",
         "Caused by: java.lang.RuntimeException: db-password=abc123", "GET /api/config?apikey=abc123 HTTP/1.1",
         "curl -H \"Authorization: Bearer abc123\" https://host/x", "set-cookie: session=abc123; Path=/",
         "msg=\"auth failed\" auth_token=abc123 status=401", "Info: start\nWarning: auth_token=abc123\nInfo: done",
       }) {
    SCOPED_TRACE(input);
    EXPECT_EQ(dae::redact_text(input, {}).find("abc123"), std::string::npos);
  }
}

TEST(DoctorAiExplanationResponse, FallbacksAreSmallAndCarryNoTextAndSuccessHasTheSixFields) {
  const dae::response_identity_t identity {
    .request_id = request_uuid(1), .app_session_id = "a", .session_generation = 7, .source_result_id = "r",
    .finding_fingerprint = std::string(64, 'b'), .controller_revision = 9, .snapshot_id = std::string(32, 'c'),
    .snapshot_age_ms = 3000,
  };
  for (const auto code : {dae::code_e::finding_changed, dae::code_e::provider_timeout, dae::code_e::rate_limited,
                          dae::code_e::host_ai_disabled, dae::code_e::invalid_explanation}) {
    const auto body = nlohmann::json::parse(dae::response_body(code, identity, 57000, std::nullopt, std::nullopt));
    EXPECT_EQ(body["status"], false);
    EXPECT_EQ(body["fallback"], true);
    EXPECT_EQ(body["changed"], false);
    EXPECT_TRUE(body["explanation"].is_null());
    EXPECT_EQ(body["expires_in_ms"], 0);
    EXPECT_EQ(body["authority"], "explanation_only");
    EXPECT_EQ(body["may_define_settings"], false);
    EXPECT_EQ(body["source"], (nlohmann::json {{"kind", "ai-explanation"}, {"mode", "host-configured"}, {"informational", true}}));
    EXPECT_EQ(body.size(), 19u);
  }
  const auto success = nlohmann::json::parse(dae::response_body(dae::code_e::explained, identity, 57000, std::nullopt, valid_explanation()));
  EXPECT_EQ(success["status"], true);
  EXPECT_EQ(success["code"], "explained");
  EXPECT_EQ(success["expires_in_ms"], 57000);
  EXPECT_EQ(success["explanation"].size(), 6u);
  EXPECT_EQ(dae::http_status(dae::code_e::explained), 200);
  EXPECT_EQ(dae::http_status(dae::code_e::provider_timeout), 504);
  EXPECT_EQ(dae::http_status(dae::code_e::invalid_explanation), 502);
  EXPECT_EQ(dae::http_status(dae::code_e::request_busy), 429);
}

// ---- The production store and worker with a fake scope at the boundary ----

namespace {
  struct fake_provider_t {
    std::mutex mutex;
    std::condition_variable changed;
    bool frozen = false;
    bool entered = false;
    int calls = 0;
    std::vector<std::string> evidence;
    std::function<dae::provider_outcome_t()> reply = [] { return dae::provider_outcome_t {dae::code_e::explained, valid_explanation()}; };

    dae::provider_t hook() {
      return [this](const dae::provider_call_t &call) {
        std::unique_lock lock(mutex);
        ++calls;
        evidence.push_back(call.evidence);
        entered = true;
        changed.notify_all();
        // A frozen provider waits for release or for the server's cancellation, within a test bound.
        const auto bound = std::chrono::steady_clock::now() + 10s;
        while (frozen && !call.cancel.load() && std::chrono::steady_clock::now() < bound) {
          changed.wait_for(lock, 10ms);
        }
        if (call.cancel.load()) return dae::provider_outcome_t {dae::code_e::provider_failed, std::nullopt};
        return reply();
      };
    }
    bool wait_entered() {
      std::unique_lock lock(mutex);
      return changed.wait_for(lock, 5s, [&] { return entered; });
    }
    void release() {
      std::lock_guard lock(mutex);
      frozen = false;
      changed.notify_all();
    }
    void freeze() {
      std::lock_guard lock(mutex);
      frozen = true;
      entered = false;
    }
    int call_count() {
      std::lock_guard lock(mutex);
      return calls;
    }
  };

  struct fake_clock_t {
    const std::chrono::steady_clock::time_point base = std::chrono::steady_clock::now();
    std::atomic<std::int64_t> offset_ms {0};
    dae::steady_clock_fn_t hook() {
      return [this] { return base + std::chrono::milliseconds(offset_ms.load()); };
    }
    void advance(std::chrono::milliseconds by) { offset_ms += by.count(); }
  };

  struct replies_t {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::pair<int, nlohmann::json>> list;
    dae::writer_t writer() {
      return [this](int status, const std::string &body) {
        std::lock_guard lock(mutex);
        list.emplace_back(status, nlohmann::json::parse(body));
        changed.notify_all();
      };
    }
    std::pair<int, nlohmann::json> wait(std::size_t count) {
      std::unique_lock lock(mutex);
      changed.wait_for(lock, 10s, [&] { return list.size() >= count; });
      return list.size() >= count ? list[count - 1] : std::pair<int, nlohmann::json> {0, nullptr};
    }
    std::size_t size() {
      std::lock_guard lock(mutex);
      return list.size();
    }
  };

  struct fake_world_t {
    std::mutex mutex;
    std::shared_ptr<const void> record = std::make_shared<int>(1);
    nlohmann::json doctor = synthetic_doctor();
    std::uint64_t controller_revision = 9;
    std::uint64_t generation = 7;
    bool owner = true;
    dae::scope_reader_t reader() {
      return [this] {
        std::lock_guard lock(mutex);
        dae::scope_view_t scope;
        scope.cert_record = record;
        scope.cert_uuid = "owner";
        scope.permission_view = true;
        scope.sole_active_owner = owner;
        scope.app_session_id = "synthetic-app-session";
        scope.session_generation = generation;
        scope.controller_revision = controller_revision;
        scope.doctor = doctor;
        scope.projection = {.capture_reason = "shm_capture", .observed_encoder_kbps = 17500};
        scope.identities = {"Canary Owner Phone"};
        return scope;
      };
    }
    dae::request_t request(int n) {
      const auto finding = dae::finding_of(synthetic_doctor());
      return {request_uuid(n), "synthetic-app-session", 7, finding.source_result_id, finding.fingerprint, 9};
    }
  };

  dae::config_view_t ready_config() {
    dae::config_view_t config;
    config.enabled = true;
    config.transport_ready = true;
    config.generation = 1;
    return config;
  }
}  // namespace

TEST(DoctorAiExplanationStore, OneInFlightNoQueueDuplicatesAndTwoAttemptsPerWindow) {
  fake_provider_t provider;
  fake_clock_t clock;
  fake_world_t world;
  replies_t replies;
  dae::service_t service {{.read_config = ready_config, .provider = provider.hook(), .clock = clock.hook(), .snapshot_id = {}}};

  provider.freeze();
  service.submit(world.request(1), world.reader(), replies.writer());
  ASSERT_TRUE(provider.wait_entered());
  EXPECT_TRUE(service.peek("owner").busy);
  service.submit(world.request(1), world.reader(), replies.writer());
  EXPECT_EQ(replies.wait(1).second["code"], "duplicate_request");
  service.submit(world.request(2), world.reader(), replies.writer());
  EXPECT_EQ(replies.wait(2).first, 429);
  EXPECT_EQ(replies.wait(2).second["code"], "request_busy");
  EXPECT_EQ(provider.call_count(), 1) << "no waiter attaches to the running attempt and nothing is queued";
  provider.release();
  const auto first = replies.wait(3);
  EXPECT_EQ(first.first, 200);
  EXPECT_EQ(first.second["request_id"], request_uuid(1));
  ASSERT_TRUE(first.second["snapshot_id"].is_string());
  EXPECT_EQ(first.second["snapshot_id"].get<std::string>().size(), 32u);

  for (int wait = 0; wait < 500 && service.peek("owner").busy; ++wait) std::this_thread::sleep_for(2ms);
  service.submit(world.request(1), world.reader(), replies.writer());
  EXPECT_EQ(replies.wait(4).second["code"], "duplicate_request") << "the same UUID stays refused for the window";
  service.submit(world.request(3), world.reader(), replies.writer());
  EXPECT_EQ(replies.wait(5).first, 200);
  for (int wait = 0; wait < 500 && service.peek("owner").busy; ++wait) std::this_thread::sleep_for(2ms);
  clock.advance(10s);
  EXPECT_TRUE(service.peek("owner").rate_limited);
  service.submit(world.request(4), world.reader(), replies.writer());
  const auto limited = replies.wait(6);
  EXPECT_EQ(limited.first, 429);
  EXPECT_EQ(limited.second["code"], "rate_limited");
  ASSERT_TRUE(limited.second["retry_after_ms"].is_number_integer());
  EXPECT_EQ(limited.second["retry_after_ms"], 50000);
  EXPECT_EQ(provider.call_count(), 2);
  clock.advance(50s);
  EXPECT_FALSE(service.peek("owner").rate_limited);
  service.submit(world.request(4), world.reader(), replies.writer());
  EXPECT_EQ(replies.wait(7).first, 200) << "the window, not the UUID, gates a new click";
  const auto counters = service.counters();
  EXPECT_EQ(counters.accepted_attempts, 3u);
  EXPECT_EQ(counters.provider_calls, 3u);
  service.shutdown();
  EXPECT_EQ(service.counters().completions, 3u);
  EXPECT_EQ(replies.size(), 7u) << "every submission was answered exactly once";
}

TEST(DoctorAiExplanationStore, PendingTextIsDroppedWhenTheCapturedContextChanges) {
  struct change_t { std::string label; std::function<void(fake_world_t &, fake_clock_t &)> apply; std::string code; int status; };
  const std::vector<change_t> changes {
    {"numeric drift only", [](auto &w, auto &) { std::lock_guard l(w.mutex); (*row_of(w.doctor, "latency"))["value"] = 77.0; }, "explained", 200},
    {"meaningful finding", [](auto &w, auto &) { std::lock_guard l(w.mutex); (*row_of(w.doctor, "packet_loss"))["status"] = "pass"; }, "finding_changed", 409},
    {"controller revision", [](auto &w, auto &) { std::lock_guard l(w.mutex); ++w.controller_revision; }, "scope_changed", 409},
    {"reconnect generation", [](auto &w, auto &) { std::lock_guard l(w.mutex); ++w.generation; }, "scope_changed", 409},
    {"record replaced", [](auto &w, auto &) { std::lock_guard l(w.mutex); w.record = std::make_shared<int>(2); }, "certificate_changed", 409},
    {"revoked", [](auto &w, auto &) { std::lock_guard l(w.mutex); w.record.reset(); }, "unauthorized", 401},
    {"ownership lost", [](auto &w, auto &) { std::lock_guard l(w.mutex); w.owner = false; }, "active_owner_required", 403},
    {"expiry", [](auto &, auto &c) { c.advance(61s); }, "snapshot_expired", 409},
  };
  for (const auto &change : changes) {
    SCOPED_TRACE(change.label);
    fake_provider_t provider;
    fake_clock_t clock;
    fake_world_t world;
    replies_t replies;
    dae::service_t service {{.read_config = ready_config, .provider = provider.hook(), .clock = clock.hook(), .snapshot_id = {}}};
    provider.freeze();
    service.submit(world.request(1), world.reader(), replies.writer());
    ASSERT_TRUE(provider.wait_entered());
    change.apply(world, clock);
    provider.release();
    const auto [status, body] = replies.wait(1);
    EXPECT_EQ(status, change.status);
    EXPECT_EQ(body["code"], change.code);
    EXPECT_EQ(body["explanation"].is_null(), change.status != 200);
    EXPECT_EQ(provider.call_count(), 1);
    if (change.status == 401) {
      EXPECT_EQ(body["request_id"], "") << "a revoked caller gets no identity echo";
    }
    if (change.status == 200) {
      // The captured reading, not the drifted one, went to the provider.
      EXPECT_NE(provider.evidence.front().find("\"value\":42.0"), std::string::npos);
    }
  }
}

TEST(DoctorAiExplanationStore, PublicationUsesTheValidatedCompletionReadAndNoLaterView) {
  // Read 1 admits, read 2 is the pre-dispatch recheck, read 3 validates the completion. A read 4 would
  // see a revoked caller: it must never happen, and nothing from it may shape the published text.
  for (const bool revoke_at_completion : {false, true}) {
    SCOPED_TRACE(revoke_at_completion ? "completion read revoked" : "completion read valid");
    fake_world_t world;
    std::atomic<int> reads {0};
    const auto base = world.reader();
    const dae::scope_reader_t reader = [&] {
      const int read = ++reads;
      auto scope = base();
      static const char *names[] = {"Admission Alpha", "Dispatch Bravo", "Completion Charlie", "Late Delta"};
      scope.identities = {names[std::min(read, 4) - 1]};
      if (read >= 4 || (revoke_at_completion && read == 3)) scope.cert_record.reset();
      return scope;
    };
    fake_provider_t provider;
    provider.reply = [] {
      auto e = valid_explanation();
      e["likely_cause"] = "Admission Alpha, Dispatch Bravo, Completion Charlie and Late Delta.";
      return dae::provider_outcome_t {dae::code_e::explained, e};
    };
    replies_t replies;
    dae::service_t service {{.read_config = ready_config, .provider = provider.hook(), .clock = {}, .snapshot_id = {}}};
    service.submit(world.request(1), reader, replies.writer());
    const auto [status, body] = replies.wait(1);
    service.shutdown();
    EXPECT_EQ(reads.load(), 3) << "admission, dispatch recheck and completion recheck only";
    if (revoke_at_completion) {
      EXPECT_EQ(status, 401);
      EXPECT_TRUE(body["explanation"].is_null());
      continue;
    }
    ASSERT_EQ(status, 200) << body.dump();
    const auto text = body["explanation"]["likely_cause"].get<std::string>();
    EXPECT_EQ(text.find("Admission Alpha"), std::string::npos) << "the captured identities stay redacted";
    EXPECT_EQ(text.find("Completion Charlie"), std::string::npos) << "the validated completion view redacts";
    EXPECT_NE(text.find("Late Delta"), std::string::npos) << "no later, unchecked view was read";
  }
}

TEST(DoctorAiExplanationStore, OptOutReconfigureStopAndFailuresAnswerOnceWithoutText) {
  {
    fake_provider_t provider;
    fake_world_t world;
    replies_t replies;
    std::atomic<bool> enabled {true};
    std::atomic<std::uint64_t> generation {1};
    dae::service_t service {{.read_config = [&] { auto c = ready_config(); c.enabled = enabled; c.generation = generation; return c; },
                             .provider = provider.hook(), .clock = {}, .snapshot_id = {}}};
    provider.freeze();
    service.submit(world.request(1), world.reader(), replies.writer());
    ASSERT_TRUE(provider.wait_entered());
    enabled = false;
    provider.release();
    EXPECT_EQ(replies.wait(1).second["code"], "host_ai_disabled");
    EXPECT_EQ(replies.wait(1).first, 503);
    for (int wait = 0; wait < 500 && service.peek("owner").busy; ++wait) std::this_thread::sleep_for(2ms);
    enabled = true;
    provider.freeze();
    service.submit(world.request(2), world.reader(), replies.writer());
    ASSERT_TRUE(provider.wait_entered());
    ++generation;
    provider.release();
    EXPECT_EQ(replies.wait(2).second["code"], "provider_not_ready") << "a reconfigured provider never releases old text";
  }
  {
    fake_provider_t provider;
    fake_world_t world;
    replies_t replies;
    dae::service_t service {{.read_config = ready_config, .provider = provider.hook(), .clock = {}, .snapshot_id = {}}};
    provider.freeze();
    service.submit(world.request(1), world.reader(), replies.writer());
    ASSERT_TRUE(provider.wait_entered());
    const auto started = std::chrono::steady_clock::now();
    service.shutdown();  // stop admission, signal cancellation, drain the one attempt, join
    EXPECT_LT(std::chrono::steady_clock::now() - started, 5s);
    EXPECT_EQ(replies.wait(1).second["code"], "server_stopping");
    EXPECT_EQ(service.counters().completions, 1u);
    service.submit(world.request(2), world.reader(), replies.writer());
    EXPECT_EQ(replies.wait(2).second["code"], "server_stopping");
    EXPECT_EQ(provider.call_count(), 1);
  }
  for (const auto &[label, outcome, code] : std::vector<std::tuple<std::string, std::function<dae::provider_outcome_t()>, std::string>> {
         {"timeout", [] { return dae::provider_outcome_t {dae::code_e::provider_timeout, std::nullopt}; }, "provider_timeout"},
         {"failure", [] { return dae::provider_outcome_t {dae::code_e::provider_failed, std::nullopt}; }, "provider_failed"},
         {"throws", []() -> dae::provider_outcome_t { throw std::runtime_error("provider said 10.9.9.9 secret"); }, "provider_failed"},
         {"oversized", [] {
            // Every field within its own code point cap, two UTF-8 bytes each: over 8 KiB in total.
            const auto wide = [](int points) { std::string text; for (int i = 0; i < points; ++i) text += "\xc3\xa9"; return text; };
            auto e = valid_explanation();
            e["likely_cause"] = wide(512);
            e["evidence"] = nlohmann::json::array();
            for (int i = 0; i < 6; ++i) e["evidence"].push_back(wide(384));
            e["try_first"] = nlohmann::json::array({wide(384), wide(384), wide(384)});
            e["advanced_detail"] = wide(1536);
            return dae::provider_outcome_t {dae::code_e::explained, e};
          }, "invalid_explanation"},
         {"unbounded field", [] { auto e = valid_explanation(); e["likely_cause"] = std::string(600, 'x'); return dae::provider_outcome_t {dae::code_e::explained, e}; }, "invalid_explanation"},
       }) {
    SCOPED_TRACE(label);
    fake_provider_t provider;
    provider.reply = outcome;
    fake_world_t world;
    replies_t replies;
    dae::service_t service {{.read_config = ready_config, .provider = provider.hook(), .clock = {}, .snapshot_id = {}}};
    service.submit(world.request(1), world.reader(), replies.writer());
    const auto [status, body] = replies.wait(1);
    EXPECT_EQ(body["code"], code);
    EXPECT_TRUE(body["explanation"].is_null());
    EXPECT_EQ(body.dump().find("10.9.9.9"), std::string::npos);
    EXPECT_LE(body.dump().size(), dae::k_max_response_bytes);
    EXPECT_GE(status, 500);
  }
}

TEST(DoctorAiExplanationStore, DescriptorReadsAllocateAndStartNothing) {
  fake_provider_t provider;
  fake_world_t world;
  std::atomic<int> ids {0};
  dae::service_t service {{.read_config = ready_config, .provider = provider.hook(), .clock = {},
                           .snapshot_id = [&] { ++ids; return dae::random_snapshot_id(); }}};
  const auto scope = world.reader()();
  nlohmann::json descriptor;
  for (int i = 0; i < 50; ++i) descriptor = dae::descriptor(scope, service.config(), service.peek("owner"));
  EXPECT_EQ(ids.load(), 0);
  EXPECT_EQ(provider.call_count(), 0);
  const auto counters = service.counters();
  EXPECT_EQ(counters.snapshots_allocated + counters.accepted_attempts + counters.provider_calls + counters.completions, 0u);
  EXPECT_EQ(descriptor["available"], true);
  EXPECT_EQ(descriptor["blocked_reason"], "");
  EXPECT_EQ(descriptor["finding_fingerprint"], dae::finding_of(synthetic_doctor()).fingerprint);
  EXPECT_EQ(descriptor["controller_revision"], 9);
  EXPECT_EQ(descriptor.size(), 8u);
  auto viewer = scope;
  viewer.sole_active_owner = false;
  const auto refused = dae::descriptor(viewer, ready_config(), {});
  EXPECT_EQ(refused["permitted"], false);
  EXPECT_EQ(refused["blocked_reason"], "active_owner_required");
  EXPECT_EQ(refused["finding_fingerprint"], "");
  EXPECT_EQ(refused["controller_revision"], 0);
  auto disabled = ready_config();
  disabled.enabled = false;
  EXPECT_EQ(dae::descriptor(scope, disabled, {})["blocked_reason"], "host_ai_disabled");
  EXPECT_EQ(dae::descriptor(scope, disabled, {})["permitted"], true) << "permission is independent of host opt-in";
}

// ---- Bounded provider transports (fake endpoint/CLI only) ----

TEST(DoctorAiExplanationTransport, ReadinessIsPureAndRefusesUnboundedModes) {
  ai_optimizer::config_t local;
  local.enabled = true;
  local.provider = "local";
  local.model = "fixture-model";
  local.auth_mode = "none";
  local.base_url = "http://127.0.0.1:9/v1";
  EXPECT_TRUE(ai_optimizer::paired_explanation_transport_ready(local, true));
  EXPECT_FALSE(ai_optimizer::paired_explanation_transport_ready(local, false)) << "a synchronous resolver cannot keep the deadline";
  auto codex = local;
  codex.provider = "openai";
  codex.auth_mode = "subscription";
  EXPECT_FALSE(ai_optimizer::paired_explanation_transport_ready(codex, true));
  auto anthropic_key = local;
  anthropic_key.provider = "anthropic";
  anthropic_key.auth_mode = "api_key";
  anthropic_key.api_key = "fixture-key";
  EXPECT_FALSE(ai_optimizer::paired_explanation_transport_ready(anthropic_key, true));
  auto claude = local;
  claude.provider = "anthropic";
  claude.auth_mode = "subscription";
  claude.timeout_ms = 5000;
  EXPECT_FALSE(ai_optimizer::paired_explanation_transport_ready(claude, true)) << "5 s cannot fit the sign-in check and inference";
  claude.timeout_ms = 6000;
#ifndef _WIN32
  EXPECT_TRUE(ai_optimizer::paired_explanation_transport_ready(claude, false));
#endif
  EXPECT_EQ(ai_optimizer::paired_explanation_deadline_ms(120000), 30000);
  EXPECT_EQ(ai_optimizer::paired_explanation_deadline_ms(0), 1000);
  std::atomic<bool> cancel {false};
  EXPECT_EQ(ai_optimizer::explain_paired_doctor(codex, "{}", cancel).status, ai_optimizer::paired_explanation_status_e::provider_not_ready);
}

namespace {
  // A localhost OpenAI-compatible endpoint whose answer the test chooses.
  struct fake_endpoint_t {
    SimpleWeb::Server<SimpleWeb::HTTP> server;
    std::thread thread;
    std::atomic<unsigned short> port {0};
    std::mutex mutex;
    std::string mode = "valid";
    std::vector<nlohmann::json> requests;

    fake_endpoint_t() {
      server.config.address = "127.0.0.1";
      server.config.port = 0;
      server.config.thread_pool_size = 2;
      server.resource["^/v1/chat/completions$"]["POST"] = [this](auto response, auto request) {
        std::string selected;
        {
          std::lock_guard lock(mutex);
          requests.push_back(nlohmann::json::parse(request->content.string(), nullptr, false));
          selected = mode;
        }
        const auto content = [](const nlohmann::json &explanation) {
          nlohmann::json message = nlohmann::json::object();
          message["content"] = explanation.dump();
          nlohmann::json choice = nlohmann::json::object();
          choice["message"] = message;
          nlohmann::json reply = nlohmann::json::object();
          reply["choices"] = nlohmann::json::array();
          reply["choices"].push_back(choice);
          return reply.dump();
        };
        if (selected == "slow") std::this_thread::sleep_for(2500ms);
        if (selected == "valid" || selected == "slow") {
          response->write(content(valid_explanation()));
        } else if (selected == "huge") {
          auto e = valid_explanation();
          e["advanced_detail"] = std::string(70000, 'h');
          response->write(content(e));
        } else if (selected == "extra") {
          auto e = valid_explanation();
          e["endpoint"] = "http://10.0.0.9/";
          response->write(content(e));
        } else if (selected == "destructive") {
          auto e = valid_explanation();
          e["destructive_action_allowed"] = true;
          response->write(content(e));
        } else if (selected == "not_json") {
          response->write("provider error from 10.0.0.9 with api_key=abc123");
        } else {
          response->write(SimpleWeb::StatusCode::server_error_internal_server_error, "provider error 10.0.0.9");
        }
      };
      thread = std::thread([this] { server.start([this](unsigned short assigned) { port = assigned; }); });
      for (int waited = 0; port == 0 && waited < 500; ++waited) std::this_thread::sleep_for(10ms);
    }
    ~fake_endpoint_t() {
      server.stop();
      if (thread.joinable()) thread.join();
    }
    ai_optimizer::config_t config(int timeout_ms) const {
      ai_optimizer::config_t c;
      c.enabled = true;
      c.provider = "local";
      c.model = "fixture-model";
      c.auth_mode = "none";
      c.base_url = "http://127.0.0.1:" + std::to_string(port.load()) + "/v1";
      c.timeout_ms = timeout_ms;
      return c;
    }
  };
}  // namespace

TEST(DoctorAiExplanationTransport, HttpOutputDeadlineAndShapeAreBounded) {
  if (!ai_optimizer::paired_explanation_http_resolver_bounded()) {
    // The same build refuses the HTTP transport for this route; covered by the readiness case.
    std::atomic<bool> cancel {false};
    fake_endpoint_t endpoint;
    EXPECT_EQ(ai_optimizer::explain_paired_doctor(endpoint.config(5000), "{}", cancel).status,
              ai_optimizer::paired_explanation_status_e::provider_not_ready);
    RecordProperty("http_resolver_bounded", "false");
    return;  // this build's refusal is the behavior under test; nothing else may run the transport
  }
  RecordProperty("http_resolver_bounded", "true");
  fake_endpoint_t endpoint;
  ASSERT_NE(endpoint.port.load(), 0);
  using status_e = ai_optimizer::paired_explanation_status_e;
  const auto evidence = *dae::project_evidence(synthetic_doctor(), {.capture_reason = "shm_capture", .observed_encoder_kbps = 17500});
  const auto run = [&](const std::string &mode, int timeout_ms) {
    {
      std::lock_guard lock(endpoint.mutex);
      endpoint.mode = mode;
    }
    std::atomic<bool> cancel {false};
    const auto started = std::chrono::steady_clock::now();
    auto result = ai_optimizer::explain_paired_doctor(endpoint.config(timeout_ms), evidence, cancel);
    return std::pair {result, std::chrono::steady_clock::now() - started};
  };
  const auto [valid, valid_time] = run("valid", 5000);
  ASSERT_EQ(valid.status, status_e::explained);
  EXPECT_TRUE(dae::sanitize_explanation(nlohmann::json::parse(*valid.explanation_json), {}));
  {
    std::lock_guard lock(endpoint.mutex);
    ASSERT_EQ(endpoint.requests.size(), 1u);
    const auto &sent = endpoint.requests.front();
    EXPECT_EQ(sent["max_tokens"], 600);
    EXPECT_EQ(sent["model"], "fixture-model");
    ASSERT_EQ(sent["messages"].size(), 2u);
    const auto user = sent["messages"][1]["content"].get<std::string>();
    EXPECT_NE(user.find(evidence), std::string::npos) << "the provider sees only the projected evidence";
    for (const auto &canary : k_canaries) EXPECT_FALSE(contains_ci(sent.dump(), canary)) << canary;
  }
  EXPECT_EQ(run("huge", 5000).first.status, status_e::provider_failed) << "a 64 KiB transport cap ends the transfer";
  EXPECT_EQ(run("extra", 5000).first.status, status_e::invalid_explanation);
  EXPECT_EQ(run("destructive", 5000).first.status, status_e::invalid_explanation);
  EXPECT_EQ(run("not_json", 5000).first.status, status_e::invalid_explanation);
  EXPECT_EQ(run("http_500", 5000).first.status, status_e::provider_failed);
  const auto [slow, slow_time] = run("slow", 1000);
  EXPECT_EQ(slow.status, status_e::provider_timeout);
  EXPECT_LT(slow_time, 2200ms) << "the configured 1 s deadline bounds the paired call";
  {
    std::lock_guard lock(endpoint.mutex);
    endpoint.mode = "slow";
  }
  std::atomic<bool> cancel {false};
  auto cancelled = std::async(std::launch::async, [&] {
    return ai_optimizer::explain_paired_doctor(endpoint.config(30000), evidence, cancel);
  });
  std::this_thread::sleep_for(200ms);
  const auto cancelled_at = std::chrono::steady_clock::now();
  cancel = true;
  EXPECT_EQ(cancelled.get().status, status_e::provider_failed);
  EXPECT_LT(std::chrono::steady_clock::now() - cancelled_at, 1500ms) << "server stop cancels the HTTP transfer";
}

#ifndef _WIN32
TEST(DoctorAiExplanationTransport, ClaudeCliRunsWithoutToolsWithinTheTotalDeadline) {
  auto name = (fs::temp_directory_path() / "paired-claude-XXXXXX").string();
  ASSERT_NE(mkdtemp(name.data()), nullptr);
  const fs::path root = name;
  const auto cli = root / "claude";
  {
    std::ofstream script(cli);
    script << R"PY(#!/usr/bin/python3
import json, pathlib, sys, time
root = pathlib.Path(__file__).parent
mode = (root / 'mode').read_text()
if sys.argv[1:] == ['--safe-mode', 'auth', 'status']:
    print(json.dumps({'loggedIn': mode != 'logged_out', 'authMethod': 'claude.ai'}))
    sys.exit(1 if mode == 'logged_out' else 0)
(root / 'args').write_text(json.dumps(sys.argv[1:]))
if mode == 'sleep':
    time.sleep(10)
if mode == 'huge':
    sys.stdout.write('x' * 200000)
    sys.exit(0)
explanation = {'likely_cause': 'Network pressure.', 'evidence': ['Loss was judged.'], 'try_first': ['Review Doctor.'],
               'advanced_detail': 'Plain text.', 'confidence': 'low', 'destructive_action_allowed': False}
print(json.dumps({'type': 'result', 'subtype': 'success', 'is_error': False, 'structured_output': explanation}))
)PY";
  }
  fs::permissions(cli, fs::perms::owner_all);
  const auto set_mode = [&](const char *mode) { std::ofstream(root / "mode") << mode; };
  ai_optimizer::config_t claude;
  claude.enabled = true;
  claude.provider = "anthropic";
  claude.model = "fixture-model";
  claude.auth_mode = "subscription";
  claude.timeout_ms = 6000;
  std::atomic<bool> cancel {false};
  const ai_optimizer::paired_explanation_overrides_t overrides {.claude_executable = cli.string()};
  using status_e = ai_optimizer::paired_explanation_status_e;

  set_mode("valid");
  const auto valid = ai_optimizer::explain_paired_doctor(claude, "{\"schema\":1}", cancel, overrides);
  ASSERT_EQ(valid.status, status_e::explained);
  const auto args = nlohmann::json::parse(std::ifstream(root / "args"));
  const auto tools = std::find(args.begin(), args.end(), "--tools");
  ASSERT_NE(tools, args.end());
  EXPECT_EQ(*std::next(tools), "") << "inference runs with no tools";
  EXPECT_NE(std::find(args.begin(), args.end(), "--strict-mcp-config"), args.end());

  set_mode("huge");
  EXPECT_EQ(ai_optimizer::explain_paired_doctor(claude, "{}", cancel, overrides).status, status_e::invalid_explanation);
  set_mode("logged_out");
  EXPECT_EQ(ai_optimizer::explain_paired_doctor(claude, "{}", cancel, overrides).status, status_e::provider_not_ready);
  set_mode("sleep");
  const auto started = std::chrono::steady_clock::now();
  EXPECT_EQ(ai_optimizer::explain_paired_doctor(claude, "{}", cancel, overrides).status, status_e::provider_timeout);
  EXPECT_LT(std::chrono::steady_clock::now() - started, 6500ms) << "sign-in check + inference stay within the 6 s deadline";
  std::error_code ec;
  fs::remove_all(root, ec);
}
#endif

// ---- The actual paired route (Linux: production HTTPS server, certificates and session state) ----

#if defined(__linux__)
  #include <Simple-Web-Server/client_https.hpp>
  #include <src/process.h>
  #include <src/rtsp.h>
  #include <src/uuid.h>

namespace {
  constexpr std::uint64_t k_generation = 61301;
  const std::string k_owner = "canary-owner-uuid";
  const std::string k_token = "canary-app-session-9d1f";

  class DoctorExplanationHttp: public testing::Test {
  protected:
    struct credentials_t { fs::path cert, key; std::string pem; };
    credentials_t credentials(const std::string &name) {
      const auto c = crypto::gen_creds(name, 2048);
      credentials_t result {directory / (name + ".crt"), directory / (name + ".key"), c.x509};
      std::ofstream(result.cert) << c.x509;
      std::ofstream(result.key) << c.pkey;
      fs::permissions(result.key, fs::perms::owner_read | fs::perms::owner_write);
      return result;
    }
    bool authorize(const credentials_t &c, const std::string &id, const std::string &name, crypto::PERM permission) {
      auto client = std::make_shared<crypto::named_cert_t>();
      client->name = name; client->uuid = id; client->cert = c.pem; client->enable_legacy_ordering = true;
      return nvhttp::add_authorized_client_for_tests(client, permission);
    }

    void SetUp() override {
      directory = fs::temp_directory_path() / ("polaris-doctor-ai-" + uuid_util::uuid_t::generate().string());
      ASSERT_TRUE(fs::create_directory(directory));
      fs::permissions(directory, fs::perms::owner_all);
      host = credentials("host"); owner = credentials("owner"); other = credentials("other");
      blind = credentials("blind"); unpaired = credentials("unpaired");
      old_state = config::nvhttp.file_state;
      old_fresh = config::sunshine.flags.test(config::flag::FRESH_STATE);
      old_ai = config::video.ai_optimizer;
      configured = true;
      config::sunshine.flags.set(config::flag::FRESH_STATE);
      config::nvhttp.file_state = (directory / "state.json").string();
      nvhttp::reset_pairing_state_for_tests();
      ASSERT_TRUE(authorize(owner, k_owner, "Canary Owner Phone", crypto::PERM::_all));
      ASSERT_TRUE(authorize(other, "canary-other-uuid", "Canary Other Tablet", crypto::PERM::_all));
      ASSERT_TRUE(authorize(blind, "canary-blind-uuid", "Canary Blind Device", crypto::PERM::_no));

      ai_optimizer::config_t ai;
      ai.enabled = true;
      ai.provider = "local";
      ai.model = "fixture-model";
      ai.auth_mode = "none";
      ai.base_url = "http://127.0.0.1:9/v1";
      ai.timeout_ms = 5000;
      ai_optimizer::reconfigure(ai);

      // One owned stream: RTSP session, launch owner, timing scope, telemetry and the controller session.
      adaptive_bitrate::set_enabled(false);
      launch = std::make_shared<rtsp_stream::launch_session_t>();
      launch->id = 61301; launch->unique_id = k_owner; launch->device_name = "Canary Owner Phone"; launch->session_token = k_token;
      rtsp_stream::add_session_for_tests(*launch, false);
      proc::ctx_t app; app.id = "613"; app.name = "Canary Secret Title"; app.uuid = "613-canary-title";
      proc::proc.set_active_launch_for_tests(app, launch);
      proc::proc.set_running_app_for_tests(app);
      stream_stats::update_stream_active(false);
      stream_stats::start_session_timing(k_owner, k_generation, k_token);
      stream_stats::update_stream_active(true, "Canary Owner Phone", "10.9.8.7");
      stream_stats::update_video_stats(60.0, 20000, 4.0, "hevc", 1920, 1080);
      stream_stats::update_network_stats(12.0, 0.0, 1000);
      doctor_actions::session_started(k_owner, k_generation, k_token, 20000);
    }

    void TearDown() override {
      route.reset();
      if (!configured) {
        if (!directory.empty()) { std::error_code ec; fs::remove_all(directory, ec); }
        return;
      }
      doctor_actions::session_ended(k_owner, k_generation);
      stream_stats::stop_session_timing(k_owner, k_generation);
      stream_stats::update_stream_active(false);
      rtsp_stream::set_cleanup_session_probe_for_tests([]() {});
      rtsp_stream::terminate_sessions();
      rtsp_stream::set_cleanup_session_probe_for_tests({});
      proc::proc.set_active_launch_for_tests(proc::ctx_t {}, nullptr);
      proc::proc.clear_running_app_for_tests();
      stream_stats::update_capture_metadata(platf::frame_metadata_t {});
      ai_optimizer::reconfigure(ai_optimizer::config_t {});
      config::video.ai_optimizer = old_ai;
      nvhttp::reset_pairing_state_for_tests();
      config::nvhttp.file_state = old_state;
      config::sunshine.flags.set(config::flag::FRESH_STATE, old_fresh);
      std::error_code ec; fs::remove_all(directory, ec);
    }

    void start(dae::provider_t provider_hook) {
      route = std::make_unique<nvhttp::doctor_explanation_route_for_tests_t>(
        host.cert.string(), host.key.string(),
        dae::service_t::hooks_t {.read_config = {}, .provider = std::move(provider_hook), .clock = clock.hook(), .snapshot_id = {}});
      ASSERT_NE(route->port(), 0);
    }

    struct reply_t { int status = 0; nlohmann::json body; std::string cache_control; };
    reply_t send(const credentials_t &c, const std::string &method, const std::string &path, const std::string &body = {},
                 const std::string &type = "application/json") {
      SimpleWeb::Client<SimpleWeb::HTTPS> client {"127.0.0.1:" + std::to_string(route->port()), false, c.cert.string(), c.key.string()};
      client.config.timeout = 15;
      SimpleWeb::CaseInsensitiveMultimap headers;
      if (!type.empty()) headers.emplace("Content-Type", type);
      const auto response = client.request(method, path, body, headers);
      reply_t reply;
      reply.status = std::stoi(response->status_code);
      reply.body = nlohmann::json::parse(response->content.string(), nullptr, false);
      if (const auto found = response->header.find("Cache-Control"); found != response->header.end()) reply.cache_control = found->second;
      return reply;
    }
    reply_t post(const credentials_t &c, const nlohmann::json &body) {
      return send(c, "POST", "/polaris/v1/doctor/explanation", body.dump());
    }
    nlohmann::json descriptor(const credentials_t &c) {
      return send(c, "GET", "/polaris/v1/doctor/explanation/descriptor", {}, "").body;
    }
    nlohmann::json request_from(const nlohmann::json &described, int n) {
      const auto &d = described["descriptor"];
      return {{"schema", 1}, {"request_id", request_uuid(n)}, {"app_session_id", k_token}, {"session_generation", k_generation},
              {"source_result_id", d["source_result_id"]}, {"finding_fingerprint", d["finding_fingerprint"]},
              {"controller_revision", d["controller_revision"]}};
    }

    /// Controller, configuration and action state that an explanation must never move.
    nlohmann::json host_state() {
      const auto doctor = adaptive_bitrate::get_doctor_state();
      const auto ai = ai_optimizer::paired_explanation_config();
      const auto stats = stream_stats::get_current();
      return {
        {"action_authority_revision", doctor.action_authority_revision}, {"live_bitrate_kbps", doctor.live_bitrate_kbps},
        {"base_bitrate_kbps", doctor.base_bitrate_kbps}, {"adaptive_enabled", adaptive_bitrate::is_enabled()},
        {"adaptive_target", stats.adaptive_target_bitrate_kbps}, {"ai_generation", ai.generation}, {"ai_enabled", ai.enabled},
        {"ai_model", ai.config.model}, {"ai_config_enabled", config::video.ai_optimizer.enabled},
        {"recovery_profiles", fs::exists(platf::appdata() / "recovery_profiles.json")},
      };
    }

    fake_clock_t clock;
    std::unique_ptr<nvhttp::doctor_explanation_route_for_tests_t> route;
    std::shared_ptr<rtsp_stream::launch_session_t> launch;
    credentials_t host, owner, other, blind, unpaired;
    fs::path directory;
    std::string old_state;
    decltype(config::video.ai_optimizer) old_ai;
    bool old_fresh = false;
    bool configured = false;
  };
}  // namespace

TEST_F(DoctorExplanationHttp, TheOwnerGetsOneBoundedExplanationAndNoHostStateMoves) {
  fake_provider_t provider;
  start(provider.hook());
  const auto described = descriptor(owner);
  ASSERT_EQ(described["descriptor"]["available"], true) << described.dump();
  EXPECT_EQ(described["descriptor"]["blocked_reason"], "");
  EXPECT_EQ(described["descriptor"]["enabled"], true);
  EXPECT_EQ(described["descriptor"]["permitted"], true);
  EXPECT_EQ(described["descriptor"]["source_result_id"], described["doctor"]["result_id"]);
  const auto before = host_state();
  const auto reply = post(owner, request_from(described, 1));
  ASSERT_EQ(reply.status, 200) << reply.body.dump();
  EXPECT_EQ(reply.cache_control, "no-store");
  EXPECT_EQ(reply.body["code"], "explained");
  EXPECT_EQ(reply.body["status"], true);
  EXPECT_EQ(reply.body["fallback"], false);
  EXPECT_EQ(reply.body["changed"], false);
  EXPECT_EQ(reply.body["request_id"], request_uuid(1));
  EXPECT_EQ(reply.body["app_session_id"], k_token);
  EXPECT_EQ(reply.body["session_generation"], k_generation);
  EXPECT_EQ(reply.body["finding_fingerprint"], described["descriptor"]["finding_fingerprint"]);
  EXPECT_EQ(reply.body["controller_revision"], described["descriptor"]["controller_revision"]);
  EXPECT_EQ(reply.body["snapshot_id"].get<std::string>().size(), 32u);
  EXPECT_GT(reply.body["expires_in_ms"].get<std::int64_t>(), 0);
  EXPECT_LE(reply.body["expires_in_ms"].get<std::int64_t>(), 60000);
  EXPECT_LE(reply.body.dump().size(), dae::k_max_response_bytes);
  EXPECT_EQ(reply.body.size(), 19u);
  EXPECT_EQ(provider.call_count(), 1);
  EXPECT_EQ(host_state(), before) << "controller, configuration and action state are unchanged";
  // The provider saw only the host projection: no device, app, session or address from this host.
  ASSERT_EQ(provider.evidence.size(), 1u);
  const auto &sent = provider.evidence.front();
  EXPECT_LE(sent.size(), dae::k_max_evidence_bytes);
  for (const auto *canary : {"Canary", k_owner.c_str(), k_token.c_str(), "10.9.8.7", "613-canary-title", "hevc"}) {
    EXPECT_FALSE(contains_ci(sent, canary)) << canary;
  }
  // Repeated status-equivalent reads afterwards start nothing.
  const auto counters = route->service().counters();
  for (int i = 0; i < 10; ++i) descriptor(owner);
  const auto after = route->service().counters();
  EXPECT_EQ(after.snapshots_allocated, counters.snapshots_allocated);
  EXPECT_EQ(after.provider_calls, counters.provider_calls);
  EXPECT_EQ(after.accepted_attempts, counters.accepted_attempts);
}

TEST_F(DoctorExplanationHttp, PermissionScopeAndClaimRefusalsNeverReachTheProvider) {
  fake_provider_t provider;
  start(provider.hook());
  const auto described = descriptor(owner);
  ASSERT_EQ(described["descriptor"]["available"], true) << described.dump();
  const auto before = host_state();

  try {
    const auto refused = post(unpaired, request_from(described, 1));
    EXPECT_EQ(refused.status, 401);
  } catch (const std::exception &) {
    // Refused during TLS verification instead; either way nothing was admitted.
  }
  const auto blind_reply = post(blind, request_from(described, 2));
  EXPECT_EQ(blind_reply.status, 403);
  EXPECT_EQ(blind_reply.body["code"], "permission_required");
  const auto other_reply = post(other, request_from(described, 3));
  EXPECT_EQ(other_reply.status, 403);
  EXPECT_EQ(other_reply.body["code"], "active_owner_required");
  EXPECT_EQ(descriptor(other)["descriptor"]["permitted"], false);
  EXPECT_EQ(descriptor(other)["descriptor"]["finding_fingerprint"], "");

  std::vector<std::tuple<std::string, const char *, nlohmann::json, std::string>> claims {
    {"other app session", "app_session_id", "another-session", "scope_changed"},
    {"other generation", "session_generation", k_generation + 1, "scope_changed"},
    {"other finding", "finding_fingerprint", std::string(64, '0'), "finding_changed"},
    {"other result id", "source_result_id", "doctor-v2-ok-none-gpu_native", "finding_changed"},
    {"other controller revision", "controller_revision", described["descriptor"]["controller_revision"].get<std::uint64_t>() + 1, "scope_changed"},
  };
  int n = 10;
  for (const auto &[label, key, value, code] : claims) {
    SCOPED_TRACE(label);
    auto body = request_from(described, ++n);
    body[key] = value;
    const auto reply = post(owner, body);
    EXPECT_EQ(reply.status, 409);
    EXPECT_EQ(reply.body["code"], code);
    EXPECT_TRUE(reply.body["explanation"].is_null());
  }

  // A second active stream (another device) removes exclusive ownership.
  rtsp_stream::launch_session_t second {};
  second.id = 61302; second.unique_id = "canary-other-uuid"; second.session_token = "other-session";
  rtsp_stream::add_session_for_tests(second, false);
  EXPECT_EQ(descriptor(owner)["descriptor"]["blocked_reason"], "active_owner_required");
  const auto shared = post(owner, request_from(described, 30));
  EXPECT_EQ(shared.status, 403);
  EXPECT_EQ(shared.body["code"], "active_owner_required");

  // Strict body admission on the actual route.
  EXPECT_EQ(send(owner, "POST", "/polaris/v1/doctor/explanation", request_from(described, 31).dump(), "text/plain").status, 415);
  EXPECT_EQ(send(owner, "POST", "/polaris/v1/doctor/explanation", std::string(2000, ' ')).status, 413);
  auto forged = request_from(described, 32);
  forged["provider"] = "openai";
  EXPECT_EQ(post(owner, forged).status, 400);
  EXPECT_EQ(send(owner, "POST", "/polaris/v1/doctor/explanation?model=x", request_from(described, 33).dump()).status, 400);

  EXPECT_EQ(provider.call_count(), 0);
  EXPECT_EQ(route->service().counters().accepted_attempts, 0u);
  EXPECT_EQ(host_state(), before);
}

TEST_F(DoctorExplanationHttp, HostOptInAndUnboundedTransportsAre503WithoutAFallbackProvider) {
  fake_provider_t provider;
  start(provider.hook());
  const auto described = descriptor(owner);
  ASSERT_EQ(described["descriptor"]["available"], true) << described.dump();
  ai_optimizer::set_enabled(false);
  EXPECT_EQ(descriptor(owner)["descriptor"]["blocked_reason"], "host_ai_disabled");
  EXPECT_EQ(descriptor(owner)["descriptor"]["permitted"], true);
  const auto disabled = post(owner, request_from(described, 1));
  EXPECT_EQ(disabled.status, 503);
  EXPECT_EQ(disabled.body["code"], "host_ai_disabled");
  ai_optimizer::config_t codex;
  codex.enabled = true;
  codex.provider = "openai";
  codex.model = "fixture-model";
  codex.auth_mode = "subscription";
  ai_optimizer::reconfigure(codex);
  EXPECT_EQ(descriptor(owner)["descriptor"]["blocked_reason"], "provider_not_ready");
  const auto codex_reply = post(owner, request_from(described, 2));
  EXPECT_EQ(codex_reply.status, 503);
  EXPECT_EQ(codex_reply.body["code"], "provider_not_ready");
  EXPECT_EQ(provider.call_count(), 0);
}

TEST_F(DoctorExplanationHttp, NumericDriftIsAllowedButAChangedFindingReconnectOrManualChangeDropsTheText) {
  fake_provider_t provider;
  start(provider.hook());
  const auto pending = [&](int n, const std::function<void()> &during) {
    clock.advance(61s);  // a fresh budget window for each click
    const auto described = descriptor(owner);
    EXPECT_EQ(described["descriptor"]["available"], true) << described.dump();
    provider.freeze();
    auto reply = std::async(std::launch::async, [&, body = request_from(described, n)] { return post(owner, body); });
    EXPECT_TRUE(provider.wait_entered());
    during();
    provider.release();
    return reply.get();
  };

  // Numeric drift only: same meaningful finding, so the captured reading is explained.
  const auto before_drift = descriptor(owner)["descriptor"]["finding_fingerprint"];
  const auto drift = pending(1, [&] {
    stream_stats::update_video_stats(60.0, 21000, 4.0, "hevc", 1920, 1080);
    EXPECT_EQ(descriptor(owner)["descriptor"]["finding_fingerprint"], before_drift) << descriptor(owner).dump();
  });
  EXPECT_EQ(drift.status, 200) << drift.body.dump();
  EXPECT_NE(provider.evidence.back().find("\"value\":20000"), std::string::npos) << "the as-of-POST reading went out";

  // A meaningful change of the current finding.
  const auto changed = pending(2, [&] {
    platf::frame_metadata_t metadata;
    metadata.transport = platf::frame_transport_e::shm;
    metadata.residency = platf::frame_residency_e::cpu;
    stream_stats::update_capture_metadata(metadata);
    EXPECT_NE(descriptor(owner)["descriptor"]["finding_fingerprint"], before_drift);
  });
  EXPECT_EQ(changed.status, 409) << changed.body.dump();
  EXPECT_EQ(changed.body["code"], "finding_changed");
  EXPECT_TRUE(changed.body["explanation"].is_null());

  // A manual live bitrate change moves the controller's authority revision.
  const auto before_manual = host_state();
  const auto manual = pending(3, [&] {
    EXPECT_TRUE(doctor_actions::set_owner_live_bitrate(k_owner, k_generation, k_token, 15000));
  });
  EXPECT_EQ(manual.status, 409) << manual.body.dump();
  EXPECT_EQ(manual.body["code"], "scope_changed");
  EXPECT_NE(host_state()["action_authority_revision"], before_manual["action_authority_revision"]) << "only the test's own manual change moved it";

  // Reconnect: the same app on a new stream generation.
  const auto reconnect = pending(4, [&] {
    doctor_actions::session_ended(k_owner, k_generation);
    stream_stats::stop_session_timing(k_owner, k_generation);
    stream_stats::start_session_timing(k_owner, k_generation + 1, k_token);
    doctor_actions::session_started(k_owner, k_generation + 1, k_token, 20000);
    EXPECT_TRUE(static_cast<bool>(doctor_actions::acquire_paired_global_control(k_owner, k_generation + 1, k_token)));
  });
  EXPECT_EQ(reconnect.status, 409) << reconnect.body.dump();
  EXPECT_EQ(reconnect.body["code"], "scope_changed");
  doctor_actions::session_ended(k_owner, k_generation + 1);
  stream_stats::stop_session_timing(k_owner, k_generation + 1);
  stream_stats::start_session_timing(k_owner, k_generation, k_token);
  doctor_actions::session_started(k_owner, k_generation, k_token, 20000);

  // Expiry while the provider is still running.
  const auto expired = pending(5, [&] { clock.advance(61s); });
  EXPECT_EQ(expired.status, 409);
  EXPECT_EQ(expired.body["code"], "snapshot_expired");
  EXPECT_EQ(provider.call_count(), 5);
}

TEST_F(DoctorExplanationHttp, RevocationRecordReplacementAndOptOutDuringTheAttemptReleaseNothing) {
  fake_provider_t provider;
  start(provider.hook());
  const auto pending = [&](int n, const std::function<void()> &during) {
    clock.advance(61s);
    const auto described = descriptor(owner);
    EXPECT_EQ(described["descriptor"]["available"], true) << described.dump();
    provider.freeze();
    auto reply = std::async(std::launch::async, [&, body = request_from(described, n)] { return post(owner, body); });
    EXPECT_TRUE(provider.wait_entered());
    during();
    provider.release();
    return reply.get();
  };
  const auto opt_out = pending(1, [] { ai_optimizer::set_enabled(false); });
  EXPECT_EQ(opt_out.status, 503);
  EXPECT_EQ(opt_out.body["code"], "host_ai_disabled");
  ai_optimizer::set_enabled(true);

  const auto replaced = pending(2, [&] {
    EXPECT_EQ(nvhttp::update_device_info_result(k_owner, "Canary Owner Phone", "", 0, {}, {}, crypto::PERM::_all,
                                                true, false, false), nvhttp::client_mutation_result_t::success);
  });
  EXPECT_EQ(replaced.status, 409) << replaced.body.dump();
  EXPECT_EQ(replaced.body["code"], "certificate_changed");

  const auto revoked = pending(3, [&] { EXPECT_TRUE(nvhttp::unpair_client(k_owner)); });
  EXPECT_EQ(revoked.status, 401) << revoked.body.dump();
  EXPECT_EQ(revoked.body["code"], "unauthorized");
  EXPECT_EQ(revoked.body["request_id"], "");
  EXPECT_TRUE(revoked.body["explanation"].is_null());
  EXPECT_EQ(provider.call_count(), 3);
}

TEST_F(DoctorExplanationHttp, ConcurrentClicksAndBudgetsHoldAcrossReconnectAndShutdownAnswersOnce) {
  fake_provider_t provider;
  start(provider.hook());
  const auto described = descriptor(owner);
  ASSERT_EQ(described["descriptor"]["available"], true) << described.dump();
  provider.freeze();
  auto first = std::async(std::launch::async, [&] { return post(owner, request_from(described, 1)); });
  ASSERT_TRUE(provider.wait_entered());
  EXPECT_EQ(descriptor(owner)["descriptor"]["blocked_reason"], "request_busy");
  const auto replay = post(owner, request_from(described, 1));
  EXPECT_EQ(replay.status, 409);
  EXPECT_EQ(replay.body["code"], "duplicate_request");
  const auto parallel = post(owner, request_from(described, 2));
  EXPECT_EQ(parallel.status, 429);
  EXPECT_EQ(parallel.body["code"], "request_busy");
  provider.release();
  EXPECT_EQ(first.get().status, 200);
  for (int wait = 0; wait < 500 && route->service().peek(k_owner).busy; ++wait) std::this_thread::sleep_for(2ms);
  EXPECT_EQ(post(owner, request_from(described, 3)).status, 200);
  for (int wait = 0; wait < 500 && route->service().peek(k_owner).busy; ++wait) std::this_thread::sleep_for(2ms);
  // Reconnect on a new generation does not restore the budget.
  stream_stats::stop_session_timing(k_owner, k_generation);
  stream_stats::start_session_timing(k_owner, k_generation + 7, k_token);
  doctor_actions::session_ended(k_owner, k_generation);
  doctor_actions::session_started(k_owner, k_generation + 7, k_token, 20000);
  auto reconnected = descriptor(owner);
  EXPECT_EQ(reconnected["descriptor"]["blocked_reason"], "rate_limited") << reconnected.dump();
  auto again = request_from(reconnected, 4);
  again["session_generation"] = k_generation + 7;
  const auto limited = post(owner, again);
  EXPECT_EQ(limited.status, 429);
  EXPECT_EQ(limited.body["code"], "rate_limited");
  EXPECT_GT(limited.body["retry_after_ms"].get<std::int64_t>(), 0);
  EXPECT_EQ(provider.call_count(), 2);
  doctor_actions::session_ended(k_owner, k_generation + 7);
  stream_stats::stop_session_timing(k_owner, k_generation + 7);
  stream_stats::start_session_timing(k_owner, k_generation, k_token);
  doctor_actions::session_started(k_owner, k_generation, k_token, 20000);

  // Shutdown while an attempt runs: admission stops, the attempt is cancelled and answered once.
  clock.advance(61s);
  const auto fresh = descriptor(owner);
  provider.freeze();
  auto pending = std::async(std::launch::async, [&] {
    try { return post(owner, request_from(fresh, 5)).status; } catch (const std::exception &) { return -1; }
  });
  ASSERT_TRUE(provider.wait_entered());
  const auto stopping = std::chrono::steady_clock::now();
  route.reset();
  EXPECT_LT(std::chrono::steady_clock::now() - stopping, 5s);
  const auto status = pending.get();
  EXPECT_TRUE(status == 503 || status == -1) << status;
  EXPECT_EQ(provider.call_count(), 3);
}
#endif
