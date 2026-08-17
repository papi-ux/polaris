/**
 * @file src/client_support_report.cpp
 * @brief Definitions for paired-client support report intake.
 */
// standard includes
#include <algorithm>
#include <ctime>
#include <deque>
#include <mutex>
#include <unordered_map>

// local includes
#include "client_support_report.h"

using namespace std::literals;

namespace client_support_report {
  namespace {
    std::mutex state_mutex;
    std::deque<report_t> reports;
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> last_submission;

    /**
     * @brief Take a bounded string field, tolerating a wrong JSON type.
     *
     * A client can send anything, so a field that is not a string is treated as
     * absent rather than as a reason to reject the whole report: a usable report
     * with one odd field beats no report at all.
     */
    std::string bounded_field(const nlohmann::json &body, const char *name) {
      const auto found = body.find(name);
      if (found == body.end() || !found->is_string()) {
        return {};
      }
      auto value = found->get<std::string>();
      if (value.size() > max_field_bytes) {
        value.resize(max_field_bytes);
        value += "\n[truncated by host]";
      }
      return value;
    }
  }  // namespace

  std::string utc_timestamp_now() {
    const std::time_t now = std::time(nullptr);
    std::tm parts {};
#ifdef _WIN32
    if (gmtime_s(&parts, &now) != 0) {
      return {};
    }
#else
    if (gmtime_r(&now, &parts) == nullptr) {
      return {};
    }
#endif
    char buffer[32] = {};
    if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &parts) == 0) {
      return {};
    }
    return buffer;
  }

  parse_result_t parse_submission(
    std::string_view body,
    std::string_view client_id,
    std::string_view received_at
  ) {
    parse_result_t result;

    if (body.size() > max_report_bytes) {
      result.status = accept_e::too_large;
      result.error = "Report exceeds the accepted size.";
      return result;
    }

    const auto document = nlohmann::json::parse(body, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
      result.status = accept_e::malformed;
      result.error = "Report body must be a JSON object.";
      return result;
    }

    report_t report;
    // Identity comes from the verified certificate, never from the body. A
    // client must not be able to file a report as another client.
    report.client_id = std::string {client_id};
    report.received_at = std::string {received_at};
    report.device = bounded_field(document, "device");
    report.nova_version = bounded_field(document, "nova_version");
    report.android_release = bounded_field(document, "android_release");
    report.occurred_at = bounded_field(document, "occurred_at");
    report.notes = bounded_field(document, "notes");
    report.crash = bounded_field(document, "crash");
    report.log_tail = bounded_field(document, "log_tail");

    if (report.crash.empty() && report.log_tail.empty() && report.notes.empty()) {
      result.status = accept_e::malformed;
      result.error = "Report carries no crash, log, or notes.";
      return result;
    }

    result.status = accept_e::accepted;
    result.report = std::move(report);
    return result;
  }

  bool rate_limit_allows(std::string_view client_id, std::chrono::steady_clock::time_point now) {
    const std::lock_guard lock {state_mutex};
    const std::string key {client_id};
    const auto previous = last_submission.find(key);
    if (previous != last_submission.end() && now - previous->second < min_submission_interval) {
      return false;
    }
    last_submission[key] = now;
    return true;
  }

  void store(report_t report) {
    const std::lock_guard lock {state_mutex};
    // One client repeating itself must not push every other client's report out,
    // so a new report from a client replaces that client's previous one.
    const auto existing = std::find_if(reports.begin(), reports.end(), [&](const report_t &held) {
      return held.client_id == report.client_id;
    });
    if (existing != reports.end()) {
      reports.erase(existing);
    }
    reports.push_back(std::move(report));
    while (reports.size() > max_retained_reports) {
      reports.pop_front();
    }
  }

  std::vector<report_t> recent() {
    const std::lock_guard lock {state_mutex};
    return {reports.begin(), reports.end()};
  }

  nlohmann::json to_json() {
    auto output = nlohmann::json::array();
    for (const auto &report : recent()) {
      output.push_back({
        {"client_id", report.client_id},
        {"device", report.device},
        {"nova_version", report.nova_version},
        {"android_release", report.android_release},
        {"occurred_at", report.occurred_at},
        {"received_at", report.received_at},
        {"notes", report.notes},
        {"crash", report.crash},
        {"log_tail", report.log_tail},
      });
    }
    return output;
  }

  void clear() {
    const std::lock_guard lock {state_mutex};
    reports.clear();
    last_submission.clear();
  }
}  // namespace client_support_report
