/**
 * @file src/web_session_store.h
 * @brief Durable fail-closed Web UI session state.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>

namespace web_session_store {
  struct clock_snapshot_t {
    std::chrono::system_clock::time_point wall;
    std::chrono::steady_clock::time_point monotonic;
  };

  struct policy_t {
    std::chrono::seconds absolute_lifetime;
    std::chrono::seconds idle_timeout;
    std::chrono::seconds touch_interval;
    std::size_t max_sessions;
    std::size_t max_file_bytes;
  };

  enum class load_status_e {
    loaded,
    missing,
    rejected,
    io_error,
  };

  enum class validation_status_e {
    valid,
    invalid,
    io_error,
  };

  enum class creation_status_e {
    created,
    credential_mismatch,
    io_error,
  };

  class manager_t {
  public:
    manager_t(std::filesystem::path path, std::string credential_fingerprint, policy_t policy);
    ~manager_t();

    manager_t(const manager_t &) = delete;
    manager_t &operator=(const manager_t &) = delete;

    load_status_e load(clock_snapshot_t now);
    bool create(std::string_view session_id, clock_snapshot_t now);
    creation_status_e create_for_fingerprint(
      std::string_view session_id,
      std::string_view expected_credential_fingerprint,
      clock_snapshot_t now
    );
    validation_status_e validate(std::string_view session_id, clock_snapshot_t now);
    bool invalidate(std::string_view session_id);
    bool invalidate_all();
    bool rotate_credentials(std::string credential_fingerprint, clock_snapshot_t now);
    bool fingerprint_matches(std::string_view credential_fingerprint) const;
    std::size_t size() const;

  private:
    struct impl_t;
    std::unique_ptr<impl_t> impl_;
  };

  bool is_valid_sha256_hex(std::string_view value);
}  // namespace web_session_store
