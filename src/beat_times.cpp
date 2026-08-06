/**
 * @file src/beat_times.cpp
 * @brief Completion estimates, read from a local dataset rather than a third party.
 */
#include "beat_times.h"

#include "logging.h"
#include "platform/common.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <set>
#include <vector>
#include <thread>

#include <curl/curl.h>
#include <nlohmann/json.hpp>

namespace beat_times {

  namespace {

    std::filesystem::path dataset_path() {
      return platf::appdata() / "beat_times.json";
    }

    constexpr const char *HLTB_ORIGIN = "https://howlongtobeat.com";
    constexpr const char *HLTB_INIT = "https://howlongtobeat.com/api/bleed/init";
    constexpr const char *HLTB_SEARCH = "https://howlongtobeat.com/api/bleed";
    // The site answers a browser; anything else it does not have to answer at all.
    constexpr const char *HLTB_UA =
      "Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) "
      "Chrome/120.0.0.0 Safari/537.36";

    struct session_t {
      std::string token;
      std::string key;
      std::string value;

      bool valid() const {
        return !token.empty() && !key.empty() && !value.empty();
      }
    };

    size_t append_body(void *chunk, size_t size, size_t count, void *into) {
      static_cast<std::string *>(into)->append(static_cast<char *>(chunk), size * count);
      return size * count;
    }

    /** One request, with the two things this endpoint insists on. */
    long perform(CURL *curl, std::string &body, const std::vector<std::string> &headers) {
      struct curl_slist *list = nullptr;
      for (const auto &header : headers) {
        list = curl_slist_append(list, header.c_str());
      }
      curl_easy_setopt(curl, CURLOPT_HTTPHEADER, list);
      curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, append_body);
      curl_easy_setopt(curl, CURLOPT_WRITEDATA, &body);
      curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 15000L);
      curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
      // Their CDN refuses HTTP/2 on this path and the request simply never completes.
      curl_easy_setopt(curl, CURLOPT_HTTP_VERSION, CURL_HTTP_VERSION_1_1);

      const auto code = curl_easy_perform(curl);
      long status = 0;
      curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
      curl_slist_free_all(list);
      return code == CURLE_OK ? status : 0;
    }

    std::optional<session_t> open_session() {
      CURL *curl = curl_easy_init();
      if (!curl) {
        return std::nullopt;
      }

      const auto url = std::string(HLTB_INIT) + "?t=" +
                       std::to_string(std::chrono::duration_cast<std::chrono::milliseconds>(
                                        std::chrono::system_clock::now().time_since_epoch()
                       )
                                        .count());
      curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
      curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);

      std::string body;
      const auto status = perform(curl, body, {
                                                std::string("Origin: ") + HLTB_ORIGIN,
                                                std::string("Referer: ") + HLTB_ORIGIN + "/",
                                                std::string("User-Agent: ") + HLTB_UA,
                                              });
      curl_easy_cleanup(curl);

      if (status != 200) {
        return std::nullopt;
      }

      try {
        const auto payload = nlohmann::json::parse(body);
        session_t session;
        session.token = payload.value("token", "");
        // Two separate fields, and it is their VALUES that matter: the one named like a
        // key holds the name to send, the one named like a value holds what to send. The
        // field names themselves are never transmitted.
        for (const auto &[field, value] : payload.items()) {
          if (!value.is_string()) {
            continue;
          }
          std::string lowered;
          std::transform(field.begin(), field.end(), std::back_inserter(lowered), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
          });
          if (session.key.empty() && lowered.find("key") != std::string::npos) {
            session.key = value.get<std::string>();
          } else if (session.value.empty() && lowered.find("val") != std::string::npos) {
            session.value = value.get<std::string>();
          }
        }
        return session.valid() ? std::optional<session_t> {session} : std::nullopt;
      } catch (...) {
        return std::nullopt;
      }
    }

    int64_t seconds_field(const nlohmann::json &entry, const char *key) {
      if (!entry.contains(key) || !entry[key].is_number()) {
        return 0;
      }
      const auto value = entry[key].get<double>();
      return value > 0 ? static_cast<int64_t>(value) : 0;
    }

  }  // namespace

  std::string normalise_name(std::string_view name) {
    std::string folded;
    folded.reserve(name.size());
    for (const unsigned char ch : name) {
      if (std::isalnum(ch)) {
        folded.push_back(static_cast<char>(std::tolower(ch)));
      }
    }
    return folded;
  }

  dataset_t parse(std::string_view json_payload) {
    dataset_t data;
    if (json_payload.empty()) {
      return data;
    }

    try {
      const auto payload = nlohmann::json::parse(json_payload);
      if (!payload.is_object()) {
        return data;
      }

      if (payload.contains("generated_at") && payload["generated_at"].is_number()) {
        data.generated_at = payload["generated_at"].get<int64_t>();
      }

      const auto games = payload.find("games");
      if (games == payload.end() || !games->is_array()) {
        return data;
      }

      for (const auto &entry : *games) {
        if (!entry.is_object()) {
          continue;
        }

        estimate_t estimate;
        estimate.matched_name = entry.value("name", "");
        estimate.url = entry.value("url", "");
        estimate.main_seconds = seconds_field(entry, "main_seconds");
        estimate.extras_seconds = seconds_field(entry, "extras_seconds");
        estimate.completionist_seconds = seconds_field(entry, "completionist_seconds");

        // A row with a name and no figures says nothing, and would draw an empty gauge.
        if (estimate.matched_name.empty() || !estimate.has_any()) {
          continue;
        }

        const auto appid = entry.value("steam_appid", "");
        if (!appid.empty()) {
          data.by_steam_appid.emplace(appid, estimate);
        }

        const auto key = normalise_name(estimate.matched_name);
        if (!key.empty()) {
          // First spelling wins: a later duplicate is ambiguous, and guessing between
          // two rows is how a wrong number gets presented as a fact.
          data.by_name.emplace(key, estimate);
        }
      }
    } catch (...) {
      return {};
    }

    return data;
  }

  const dataset_t &dataset() {
    static std::mutex guard;
    static dataset_t cached;
    static std::filesystem::file_time_type loaded_stamp {};
    static bool loaded = false;

    const std::lock_guard<std::mutex> lock {guard};

    std::error_code ec;
    const auto path = dataset_path();
    const auto stamp = std::filesystem::last_write_time(path, ec);
    if (ec) {
      // No dataset is a normal state, not a failure: the gauge simply has no estimates.
      if (loaded) {
        cached = {};
        loaded = false;
      }
      return cached;
    }

    if (loaded && stamp == loaded_stamp) {
      return cached;
    }

    std::ifstream file(path);
    if (!file.is_open()) {
      return cached;
    }

    const std::string payload {std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    cached = parse(payload);
    loaded_stamp = stamp;
    loaded = true;

    BOOST_LOG(info) << "Beat times: loaded "sv << cached.by_steam_appid.size()
                    << " entries by app id and "sv << cached.by_name.size() << " by name"sv;
    return cached;
  }

  std::optional<estimate_t> lookup(const dataset_t &data, std::string_view steam_appid, std::string_view name) {
    if (!steam_appid.empty()) {
      const auto by_id = data.by_steam_appid.find(std::string(steam_appid));
      if (by_id != data.by_steam_appid.end()) {
        return by_id->second;
      }
    }

    const auto key = normalise_name(name);
    if (key.empty()) {
      return std::nullopt;
    }

    const auto by_name = data.by_name.find(key);
    if (by_name == data.by_name.end()) {
      return std::nullopt;
    }
    return by_name->second;
  }

  std::string match_key(std::string_view name) {
    std::string folded;
    folded.reserve(name.size());
    bool pending_space = false;
    for (const unsigned char ch : name) {
      if (std::isalnum(ch)) {
        if (pending_space && !folded.empty()) {
          folded.push_back(' ');
        }
        pending_space = false;
        folded.push_back(static_cast<char>(std::tolower(ch)));
      } else {
        pending_space = true;
      }
    }
    return folded;
  }

  int edit_distance(std::string_view left, std::string_view right) {
    if (left == right) {
      return 0;
    }
    if (left.empty()) {
      return static_cast<int>(right.size());
    }
    if (right.empty()) {
      return static_cast<int>(left.size());
    }

    std::vector<int> previous(right.size() + 1);
    std::vector<int> current(right.size() + 1);
    for (size_t column = 0; column <= right.size(); ++column) {
      previous[column] = static_cast<int>(column);
    }

    for (size_t row = 1; row <= left.size(); ++row) {
      current[0] = static_cast<int>(row);
      for (size_t column = 1; column <= right.size(); ++column) {
        const int substitution = previous[column - 1] + (left[row - 1] == right[column - 1] ? 0 : 1);
        current[column] = std::min({current[column - 1] + 1, previous[column] + 1, substitution});
      }
      previous.swap(current);
    }
    return previous[right.size()];
  }

  bool is_acceptable_match(std::string_view query, std::string_view candidate, int distance) {
    if (query.empty() || candidate.empty()) {
      return false;
    }
    if (query == candidate) {
      return true;
    }
    // An edition or subtitle appended to the exact title is still the same game, and it
    // runs both ways: a launcher says "Control Ultimate Edition" where a catalogue says
    // "Control", and either can be the longer name.
    const auto extends = [](std::string_view longer, std::string_view shorter) {
      return longer.size() > shorter.size() && longer.rfind(shorter, 0) == 0 &&
             (longer[shorter.size()] == ' ' || longer[shorter.size()] == ':');
    };
    if (extends(candidate, query) || extends(query, candidate)) {
      return true;
    }

    // Deliberately no rule for the query merely appearing inside the candidate. A search
    // for Control really does return "3-D Ultra Radio Control Racers Deluxe", and
    // containment would accept it as confidently as the right answer.
    const int threshold = std::max<int>(3, static_cast<int>(query.size()) / 2);
    return distance <= threshold;
  }

  namespace {

    /** Ask the search endpoint, and pick the candidate worth believing. */
    std::optional<estimate_t> fetch_estimate(const std::string &name) {
      const auto session = open_session();
      if (!session) {
        return std::nullopt;
      }

      nlohmann::json terms = nlohmann::json::array();
      {
        std::string word;
        for (const char ch : match_key(name)) {
          if (ch == ' ') {
            if (!word.empty()) {
              terms.push_back(word);
              word.clear();
            }
          } else {
            word.push_back(ch);
          }
        }
        if (!word.empty()) {
          terms.push_back(word);
        }
      }
      if (terms.empty()) {
        return std::nullopt;
      }

      nlohmann::json body {
        {"searchType", "games"},
        {"searchTerms", terms},
        {"searchPage", 1},
        {"size", 20},
        {"searchOptions",
         {{"games",
           {{"userId", 0},
            {"platform", ""},
            {"sortCategory", "name"},
            {"rangeCategory", "main"},
            {"modifier", "hide_dlc"},
            {"rangeTime", {{"min", 0}, {"max", 0}}},
            {"rangeYear", {{"min", ""}, {"max", ""}}},
            {"gameplay", {{"perspective", ""}, {"flow", ""}, {"genre", ""}, {"difficulty", ""}}}}},
          {"users", nlohmann::json::object()},
          {"lists", nlohmann::json::object()},
          {"filter", ""},
          {"sort", 0},
          {"randomizer", 0}}},
      };
      // The pair goes in the body as well as the headers; one without the other is a 403.
      body[session->key] = session->value;

      CURL *curl = curl_easy_init();
      if (!curl) {
        return std::nullopt;
      }
      const auto payload = body.dump();
      curl_easy_setopt(curl, CURLOPT_URL, HLTB_SEARCH);
      curl_easy_setopt(curl, CURLOPT_POSTFIELDS, payload.c_str());

      std::string response;
      const auto status = perform(curl, response, {
                                                    "Content-Type: application/json",
                                                    std::string("Origin: ") + HLTB_ORIGIN,
                                                    std::string("Referer: ") + HLTB_ORIGIN + "/",
                                                    std::string("User-Agent: ") + HLTB_UA,
                                                    "x-auth-token: " + session->token,
                                                    "x-hp-key: " + session->key,
                                                    "x-hp-val: " + session->value,
                                                  });
      curl_easy_cleanup(curl);

      if (status != 200) {
        BOOST_LOG(info) << "Beat times: search returned "sv << status << " for '"sv << name << "'"sv;
        return std::nullopt;
      }

      try {
        const auto parsed = nlohmann::json::parse(response);
        const auto results = parsed.find("data");
        if (results == parsed.end() || !results->is_array() || results->empty()) {
          return std::nullopt;
        }

        const auto query = match_key(name);
        const nlohmann::json *best = nullptr;
        std::string best_name;
        int best_distance = std::numeric_limits<int>::max();

        for (const auto &candidate : *results) {
          if (!candidate.is_object()) {
            continue;
          }
          const auto candidate_name = match_key(candidate.value("game_name", ""));
          const auto distance = edit_distance(query, candidate_name);
          if (distance < best_distance) {
            best_distance = distance;
            best = &candidate;
            best_name = candidate_name;
          }
          if (distance == 0) {
            break;
          }
        }

        if (!best || !is_acceptable_match(query, best_name, best_distance)) {
          return std::nullopt;
        }

        estimate_t estimate;
        estimate.matched_name = best->value("game_name", "");
        estimate.main_seconds = seconds_field(*best, "comp_main");
        estimate.extras_seconds = seconds_field(*best, "comp_plus");
        estimate.completionist_seconds = seconds_field(*best, "comp_100");
        const auto game_id = best->value("game_id", 0);
        if (game_id > 0) {
          estimate.url = "https://howlongtobeat.com/game/" + std::to_string(game_id);
        }
        return estimate.has_any() ? std::optional<estimate_t> {estimate} : std::nullopt;
      } catch (...) {
        return std::nullopt;
      }
    }

    /** Fold one answer into the dataset file, keeping everything already there. */
    void remember(const std::string &steam_appid, const estimate_t &estimate) {
      const auto path = dataset_path();
      nlohmann::json document {{"version", 1}, {"games", nlohmann::json::array()}};

      std::ifstream existing(path);
      if (existing.is_open()) {
        try {
          const std::string payload {std::istreambuf_iterator<char>(existing),
                                     std::istreambuf_iterator<char>()};
          const auto parsed = nlohmann::json::parse(payload);
          if (parsed.is_object() && parsed.contains("games") && parsed["games"].is_array()) {
            document = parsed;
          }
        } catch (...) {
          // A file we cannot read is replaced rather than appended to; the alternative is
          // losing the answer we just spent a request on.
        }
      }
      existing.close();

      nlohmann::json entry {
        {"name", estimate.matched_name},
        {"main_seconds", estimate.main_seconds},
        {"extras_seconds", estimate.extras_seconds},
        {"completionist_seconds", estimate.completionist_seconds},
      };
      if (!steam_appid.empty()) {
        entry["steam_appid"] = steam_appid;
      }
      if (!estimate.url.empty()) {
        entry["url"] = estimate.url;
      }

      auto &games = document["games"];
      bool replaced = false;
      for (auto &existing_entry : games) {
        const auto same_id = !steam_appid.empty() &&
                             existing_entry.value("steam_appid", "") == steam_appid;
        const auto same_name = match_key(existing_entry.value("name", "")) ==
                               match_key(estimate.matched_name);
        if (same_id || same_name) {
          existing_entry = entry;
          replaced = true;
          break;
        }
      }
      if (!replaced) {
        games.push_back(entry);
      }
      document["generated_at"] = std::chrono::duration_cast<std::chrono::seconds>(
                                   std::chrono::system_clock::now().time_since_epoch()
      )
                                   .count();

      // Written beside and renamed, so a reader never sees half a file.
      const auto scratch = path.string() + ".tmp";
      {
        std::ofstream out(scratch, std::ios::trunc);
        if (!out.is_open()) {
          return;
        }
        out << document.dump(2) << "\n";
      }
      std::error_code ec;
      std::filesystem::rename(scratch, path, ec);
      if (ec) {
        std::filesystem::remove(scratch, ec);
      }
    }

    std::mutex queue_guard;
    std::condition_variable queue_signal;
    std::deque<std::pair<std::string, std::string>> pending;
    std::set<std::string> seen;
    std::atomic<bool> running {false};
    std::thread worker;

    void drain() {
      while (running.load()) {
        std::pair<std::string, std::string> job;
        {
          std::unique_lock<std::mutex> lock {queue_guard};
          queue_signal.wait(lock, [] {
            return !pending.empty() || !running.load();
          });
          if (!running.load()) {
            return;
          }
          job = pending.front();
          pending.pop_front();
        }

        if (const auto estimate = fetch_estimate(job.second)) {
          BOOST_LOG(info) << "Beat times: resolved '"sv << job.second << "' to '"sv
                          << estimate->matched_name << "'"sv;
          remember(job.first, *estimate);
        }

        // Their rate limit is undocumented, so the pace is deliberately unhurried; this
        // runs once per title for the life of the dataset.
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
      }
    }

  }  // namespace

  void request_lookup(const std::string &steam_appid, const std::string &name) {
    if (name.empty()) {
      return;
    }

    const std::lock_guard<std::mutex> lock {queue_guard};
    const auto marker = steam_appid.empty() ? match_key(name) : steam_appid;
    // Asked once per run, whether or not the answer was useful: a title the catalogue
    // does not have would otherwise be re-asked on every library request forever.
    if (!seen.insert(marker).second) {
      return;
    }

    pending.emplace_back(steam_appid, name);
    if (!running.exchange(true)) {
      worker = std::thread(drain);
    }
    queue_signal.notify_one();
  }

  void shutdown() {
    if (!running.exchange(false)) {
      return;
    }
    queue_signal.notify_all();
    if (worker.joinable()) {
      worker.join();
    }
  }

}  // namespace beat_times
