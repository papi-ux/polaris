/**
 * @file src/verified_action.cpp
 * @brief Definitions for read-back confirmation of otherwise silent failures.
 */
// standard includes
#include <ctime>
#include <deque>
#include <mutex>

// local includes
#include "logging.h"
#include "verified_action.h"

using namespace std::literals;

namespace verified_action {
  namespace {
    std::mutex records_mutex;
    std::deque<record_t> records;

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

    void record_mismatch(record_t entry) {
      // One greppable prefix for every instance of this bug class, so a support
      // log can be searched for the whole category rather than for whichever
      // wording the individual call site happened to use.
      BOOST_LOG(error) << "Silent failure: "sv << entry.id << " reported success but did not land. Requested ["sv
                       << entry.requested << "], system reports ["sv << entry.actual << "]. "sv << entry.description;

      const std::lock_guard lock {records_mutex};
      records.push_back(std::move(entry));
      while (records.size() > max_retained_records) {
        records.pop_front();
      }
    }
  }  // namespace

  bool confirm(
    std::string_view id,
    std::string_view description,
    std::string_view requested,
    std::string_view actual
  ) {
    if (requested == actual) {
      return true;
    }

    record_mismatch({
      .id = std::string {id},
      .description = std::string {description},
      .requested = std::string {requested},
      .actual = std::string {actual},
      .observed_at = utc_timestamp_now(),
    });
    return false;
  }

  bool confirm(std::string_view id, std::string_view description, bool landed) {
    if (landed) {
      return true;
    }

    record_mismatch({
      .id = std::string {id},
      .description = std::string {description},
      .requested = "applied",
      .actual = "not applied",
      .observed_at = utc_timestamp_now(),
    });
    return false;
  }

  std::vector<record_t> silent_failures() {
    const std::lock_guard lock {records_mutex};
    return {records.begin(), records.end()};
  }

  nlohmann::json to_json() {
    auto output = nlohmann::json::array();
    for (const auto &entry : silent_failures()) {
      output.push_back({
        {"id", entry.id},
        {"description", entry.description},
        {"requested", entry.requested},
        {"actual", entry.actual},
        {"observed_at", entry.observed_at},
      });
    }
    return output;
  }

  void clear() {
    const std::lock_guard lock {records_mutex};
    records.clear();
  }
}  // namespace verified_action
