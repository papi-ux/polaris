/**
 * @file src/web_session_store.cpp
 * @brief Durable fail-closed Web UI session state.
 */
#include "web_session_store.h"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <nlohmann/json.hpp>

#include "logging.h"
#include "private_state_file.h"

namespace web_session_store {
  namespace {
    constexpr int SCHEMA_VERSION = 1;

    std::int64_t unix_seconds(std::chrono::system_clock::time_point time) {
      return std::chrono::duration_cast<std::chrono::seconds>(time.time_since_epoch()).count();
    }

    bool valid_policy(const policy_t &policy) {
      return policy.absolute_lifetime.count() > 0 && policy.idle_timeout.count() > 0 &&
             policy.touch_interval.count() >= 0 && policy.max_sessions > 0 && policy.max_file_bytes > 0;
    }

    std::string canonical_sha256_hex(std::string_view value) {
      if (value.size() != 64) {
        return {};
      }
      std::string canonical;
      canonical.reserve(value.size());
      for (const unsigned char character : value) {
        if (character >= '0' && character <= '9') {
          canonical.push_back(static_cast<char>(character));
        } else if (character >= 'a' && character <= 'f') {
          canonical.push_back(static_cast<char>(character));
        } else if (character >= 'A' && character <= 'F') {
          canonical.push_back(static_cast<char>(character - 'A' + 'a'));
        } else {
          return {};
        }
      }
      return canonical;
    }
  }  // namespace

  struct manager_t::impl_t {
    struct session_t {
      std::int64_t created_at = 0;
      std::int64_t last_seen_at = 0;
      std::chrono::steady_clock::time_point absolute_deadline;
      std::chrono::steady_clock::time_point idle_deadline;
      std::chrono::steady_clock::time_point last_persisted_monotonic;
    };

    impl_t(std::filesystem::path path, std::string credential_fingerprint, policy_t policy):
        path {std::move(path)},
        credential_fingerprint {canonical_sha256_hex(credential_fingerprint)},
        policy {policy} {
    }

    std::filesystem::path path;
    std::string credential_fingerprint;
    policy_t policy;
    mutable std::mutex mutex;
    std::unordered_map<std::string, session_t> sessions;
    bool authorization_state_available = true;

    bool configured() const {
      return !path.empty() && is_valid_sha256_hex(credential_fingerprint) && valid_policy(policy);
    }

    bool expired(const session_t &session, clock_snapshot_t now) const {
      const auto wall_now = unix_seconds(now.wall);
      return wall_now - session.created_at >= policy.absolute_lifetime.count() ||
             wall_now - session.last_seen_at >= policy.idle_timeout.count() ||
             now.monotonic >= session.absolute_deadline || now.monotonic >= session.idle_deadline;
    }

    bool ensure_parent() const {
      const auto parent = path.parent_path();
      if (parent.empty()) {
        return true;
      }
      std::error_code ec;
      std::filesystem::create_directories(parent, ec);
      if (ec) {
        BOOST_LOG(error) << "Couldn't create Web UI session state directory " << parent << ": " << ec.message();
        return false;
      }
      return true;
    }

    bool persist(
      const std::unordered_map<std::string, session_t> &state,
      std::string_view fingerprint
    ) const {
      if (!configured() || !is_valid_sha256_hex(fingerprint) || state.size() > policy.max_sessions || !ensure_parent()) {
        return false;
      }

      std::vector<std::pair<std::string, session_t>> ordered(state.begin(), state.end());
      std::sort(ordered.begin(), ordered.end(), [](const auto &lhs, const auto &rhs) {
        return lhs.first < rhs.first;
      });

      nlohmann::json root = {
        {"version", SCHEMA_VERSION},
        {"credential_fingerprint", fingerprint},
        {"sessions", nlohmann::json::array()},
      };
      for (const auto &[id, session] : ordered) {
        if (!is_valid_sha256_hex(id) || session.created_at < 0 || session.last_seen_at < session.created_at) {
          return false;
        }
        root["sessions"].push_back({
          {"id", id},
          {"created_at", session.created_at},
          {"last_seen_at", session.last_seen_at},
        });
      }

      const auto payload = root.dump();
      if (payload.size() > policy.max_file_bytes) {
        return false;
      }
      return private_state_file::write_atomic(path, payload);
    }

    bool prune(clock_snapshot_t now) {
      const auto previous_size = sessions.size();
      std::erase_if(sessions, [&](const auto &entry) {
        return expired(entry.second, now);
      });
      return sessions.size() != previous_size;
    }
  };

  manager_t::manager_t(std::filesystem::path path, std::string credential_fingerprint, policy_t policy):
      impl_ {std::make_unique<impl_t>(std::move(path), std::move(credential_fingerprint), policy)} {
  }

  manager_t::~manager_t() = default;

  load_status_e manager_t::load(clock_snapshot_t now) {
    if (!impl_->configured()) {
      std::lock_guard lock(impl_->mutex);
      impl_->authorization_state_available = false;
      return load_status_e::rejected;
    }

    const auto stored = private_state_file::read_secure(impl_->path, impl_->policy.max_file_bytes);
    if (stored.status == private_state_file::read_status_e::missing) {
      std::lock_guard lock(impl_->mutex);
      impl_->sessions.clear();
      impl_->authorization_state_available = true;
      return load_status_e::missing;
    }
    if (stored.status == private_state_file::read_status_e::rejected) {
      std::lock_guard lock(impl_->mutex);
      impl_->sessions.clear();
      impl_->authorization_state_available = true;
      return load_status_e::rejected;
    }
    if (stored.status != private_state_file::read_status_e::ok) {
      std::lock_guard lock(impl_->mutex);
      impl_->sessions.clear();
      impl_->authorization_state_available = false;
      return load_status_e::io_error;
    }

    try {
      {
        std::lock_guard lock(impl_->mutex);
        impl_->authorization_state_available = true;
      }
      const auto root = nlohmann::json::parse(stored.payload);
      if (!root.is_object() || root.size() != 3 ||
          !root.contains("version") || !root["version"].is_number_integer() ||
          root["version"].get<int>() != SCHEMA_VERSION ||
          !root.contains("credential_fingerprint") || !root["credential_fingerprint"].is_string() ||
          !root.contains("sessions") || !root["sessions"].is_array()) {
        std::lock_guard lock(impl_->mutex);
        impl_->sessions.clear();
        return load_status_e::rejected;
      }

      const auto fingerprint = root["credential_fingerprint"].get<std::string>();
      if (!is_valid_sha256_hex(fingerprint) || fingerprint != impl_->credential_fingerprint ||
          root["sessions"].size() > impl_->policy.max_sessions) {
        std::lock_guard lock(impl_->mutex);
        impl_->sessions.clear();
        return load_status_e::rejected;
      }

      const auto wall_now = unix_seconds(now.wall);
      std::unordered_map<std::string, impl_t::session_t> imported;
      std::unordered_set<std::string> ids;
      bool pruned = false;
      for (const auto &entry : root["sessions"]) {
        if (!entry.is_object() || entry.size() != 3 ||
            !entry.contains("id") || !entry["id"].is_string() ||
            !entry.contains("created_at") || !entry["created_at"].is_number_integer() ||
            !entry.contains("last_seen_at") || !entry["last_seen_at"].is_number_integer()) {
          std::lock_guard lock(impl_->mutex);
          impl_->sessions.clear();
          return load_status_e::rejected;
        }

        const auto id = entry["id"].get<std::string>();
        const auto created_at = entry["created_at"].get<std::int64_t>();
        const auto last_seen_at = entry["last_seen_at"].get<std::int64_t>();
        if (!is_valid_sha256_hex(id) || !ids.insert(id).second || created_at < 0 ||
            last_seen_at < created_at || created_at > wall_now || last_seen_at > wall_now) {
          std::lock_guard lock(impl_->mutex);
          impl_->sessions.clear();
          return load_status_e::rejected;
        }

        const auto absolute_age = wall_now - created_at;
        const auto idle_age = wall_now - last_seen_at;
        if (absolute_age >= impl_->policy.absolute_lifetime.count() ||
            idle_age >= impl_->policy.idle_timeout.count()) {
          pruned = true;
          continue;
        }

        const auto absolute_remaining = impl_->policy.absolute_lifetime - std::chrono::seconds {absolute_age};
        const auto idle_remaining = impl_->policy.idle_timeout - std::chrono::seconds {idle_age};
        imported.emplace(id, impl_t::session_t {
          .created_at = created_at,
          .last_seen_at = last_seen_at,
          .absolute_deadline = now.monotonic + absolute_remaining,
          .idle_deadline = now.monotonic + idle_remaining,
          .last_persisted_monotonic = now.monotonic,
        });
      }

      std::lock_guard lock(impl_->mutex);
      impl_->sessions = std::move(imported);
      if (pruned && !impl_->persist(impl_->sessions, impl_->credential_fingerprint)) {
        BOOST_LOG(error) << "Couldn't durably prune expired Web UI sessions from " << impl_->path;
        impl_->sessions.clear();
        impl_->authorization_state_available = false;
        return load_status_e::io_error;
      }
      impl_->authorization_state_available = true;
      return load_status_e::loaded;
    } catch (const std::exception &exception) {
      BOOST_LOG(warning) << "Rejected malformed Web UI session state " << impl_->path << ": " << exception.what();
      std::lock_guard lock(impl_->mutex);
      impl_->sessions.clear();
      return load_status_e::rejected;
    }
  }

  bool manager_t::create(std::string_view session_id, clock_snapshot_t now) {
    const auto id = canonical_sha256_hex(session_id);
    if (id.empty() || !impl_->configured()) {
      return false;
    }
    const auto wall_now = unix_seconds(now.wall);
    if (wall_now < 0) {
      return false;
    }

    std::lock_guard lock(impl_->mutex);
    if (std::any_of(impl_->sessions.begin(), impl_->sessions.end(), [&](const auto &entry) {
          return wall_now < entry.second.created_at || wall_now < entry.second.last_seen_at;
        })) {
      return false;
    }
    impl_->prune(now);
    auto staged = impl_->sessions;
    if (staged.contains(id)) {
      return false;
    }
    if (staged.size() >= impl_->policy.max_sessions) {
      const auto oldest = std::min_element(staged.begin(), staged.end(), [](const auto &lhs, const auto &rhs) {
        if (lhs.second.last_seen_at != rhs.second.last_seen_at) {
          return lhs.second.last_seen_at < rhs.second.last_seen_at;
        }
        if (lhs.second.created_at != rhs.second.created_at) {
          return lhs.second.created_at < rhs.second.created_at;
        }
        return lhs.first < rhs.first;
      });
      if (oldest == staged.end()) {
        return false;
      }
      staged.erase(oldest);
    }

    staged.emplace(id, impl_t::session_t {
      .created_at = wall_now,
      .last_seen_at = wall_now,
      .absolute_deadline = now.monotonic + impl_->policy.absolute_lifetime,
      .idle_deadline = now.monotonic + impl_->policy.idle_timeout,
      .last_persisted_monotonic = now.monotonic,
    });
    if (!impl_->persist(staged, impl_->credential_fingerprint)) {
      return false;
    }
    impl_->sessions = std::move(staged);
    impl_->authorization_state_available = true;
    return true;
  }

  validation_status_e manager_t::validate(std::string_view session_id, clock_snapshot_t now) {
    const auto id = canonical_sha256_hex(session_id);
    if (id.empty()) {
      return validation_status_e::invalid;
    }
    if (!impl_->configured()) {
      return validation_status_e::io_error;
    }

    std::lock_guard lock(impl_->mutex);
    if (!impl_->authorization_state_available) {
      return validation_status_e::io_error;
    }

    const auto pruned = impl_->prune(now);
    auto it = impl_->sessions.find(id);
    if (it == impl_->sessions.end()) {
      if (pruned && !impl_->persist(impl_->sessions, impl_->credential_fingerprint)) {
        BOOST_LOG(error) << "Couldn't durably prune expired Web UI sessions from " << impl_->path;
        impl_->authorization_state_available = false;
        return validation_status_e::io_error;
      }
      return validation_status_e::invalid;
    }

    const auto wall_now = unix_seconds(now.wall);
    if (wall_now < it->second.last_seen_at) {
      auto staged = impl_->sessions;
      staged.erase(id);
      if (!impl_->persist(staged, impl_->credential_fingerprint)) {
        BOOST_LOG(error) << "Couldn't durably invalidate a Web UI session after wall-clock rollback at " << impl_->path;
        return validation_status_e::io_error;
      }
      impl_->sessions = std::move(staged);
      return validation_status_e::invalid;
    }
    const auto previous = it->second;
    it->second.last_seen_at = wall_now;
    it->second.idle_deadline = now.monotonic + impl_->policy.idle_timeout;
    if (pruned || now.monotonic - it->second.last_persisted_monotonic >= impl_->policy.touch_interval) {
      if (impl_->persist(impl_->sessions, impl_->credential_fingerprint)) {
        it->second.last_persisted_monotonic = now.monotonic;
      } else {
        BOOST_LOG(error) << "Couldn't persist refreshed Web UI session activity to " << impl_->path;
        it->second = previous;
        return validation_status_e::io_error;
      }
    }
    return validation_status_e::valid;
  }

  bool manager_t::invalidate(std::string_view session_id) {
    const auto id = canonical_sha256_hex(session_id);
    if (id.empty() || !impl_->configured()) {
      return false;
    }
    std::lock_guard lock(impl_->mutex);
    if (!impl_->authorization_state_available) {
      return false;
    }
    if (!impl_->sessions.contains(id)) {
      return true;
    }
    auto staged = impl_->sessions;
    staged.erase(id);
    if (!impl_->persist(staged, impl_->credential_fingerprint)) {
      return false;
    }
    impl_->sessions = std::move(staged);
    impl_->authorization_state_available = true;
    return true;
  }

  bool manager_t::invalidate_all() {
    if (!impl_->configured()) {
      return false;
    }
    std::lock_guard lock(impl_->mutex);
    const std::unordered_map<std::string, impl_t::session_t> empty;
    if (!impl_->persist(empty, impl_->credential_fingerprint)) {
      return false;
    }
    impl_->sessions.clear();
    impl_->authorization_state_available = true;
    return true;
  }

  bool manager_t::rotate_credentials(std::string credential_fingerprint, clock_snapshot_t) {
    credential_fingerprint = canonical_sha256_hex(credential_fingerprint);
    if (credential_fingerprint.empty() || !valid_policy(impl_->policy)) {
      return false;
    }
    std::lock_guard lock(impl_->mutex);
    const std::unordered_map<std::string, impl_t::session_t> empty;
    const auto previous_fingerprint = impl_->credential_fingerprint;
    impl_->credential_fingerprint = credential_fingerprint;
    if (!impl_->persist(empty, credential_fingerprint)) {
      impl_->credential_fingerprint = previous_fingerprint;
      return false;
    }
    impl_->sessions.clear();
    impl_->authorization_state_available = true;
    return true;
  }

  bool manager_t::fingerprint_matches(std::string_view credential_fingerprint) const {
    const auto canonical = canonical_sha256_hex(credential_fingerprint);
    std::lock_guard lock(impl_->mutex);
    return !canonical.empty() && impl_->credential_fingerprint == canonical;
  }

  std::size_t manager_t::size() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->sessions.size();
  }

  bool is_valid_sha256_hex(std::string_view value) {
    return value.size() == 64 && std::all_of(value.begin(), value.end(), [](unsigned char character) {
      return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    });
  }
}  // namespace web_session_store
