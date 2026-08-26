/**
 * @file src/recovery_profile.h
 * @brief Durable, owner-and-game-scoped next-launch recovery profiles.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace recovery_profile {
  inline constexpr std::int64_t lifetime_seconds = 24 * 60 * 60;

  struct safe_profile_t {
    std::string stream_display_mode;
    int width = 0;
    int height = 0;
    int target_fps = 0;
    int target_bitrate_kbps = 0;
    std::string preferred_codec;
    bool hdr = false;
  };

  struct record_t {
    std::string run_id;
    std::string owner_uuid;
    std::string app_uuid;
    std::string source_result_id;
    std::string state;
    std::int64_t created_at = 0;
    std::int64_t expires_at = 0;
    std::string queued_launch_instance_id;
    std::uint64_t queued_session_generation = 0;
    bool optimizer_prepared = false;
    safe_profile_t profile;
  };

  struct observed_launch_t {
    bool streaming = false;
    std::string owner_uuid;
    std::string app_uuid;
    std::string stream_display_mode;
    int width = 0;
    int height = 0;
    int target_fps = 0;
    int bitrate_kbps = 0;
    std::string codec;
    bool hdr = false;
    std::string launch_instance_id;
    std::uint64_t session_generation = 0;
  };

  struct optimizer_constraints_t {
    int paired_width = 0;
    int paired_height = 0;
    std::vector<std::string> supported_codecs;
    bool hdr_supported = false;
    std::vector<std::string> allowed_stream_display_modes;
  };

  struct app_identity_t {
    std::string uuid;
    std::string id;
    std::string name;
  };

  /** Prefer exact UUID/id; accept a display name only when it is unambiguous. */
  std::optional<std::string> resolve_canonical_app_uuid(
    std::string_view query,
    const std::vector<app_identity_t> &apps
  );

  std::int64_t now_epoch_seconds();

  /** Queue a host-derived profile. Same key + result id is idempotent. */
  nlohmann::json queue(const std::filesystem::path &path,
                       const std::string &owner_uuid,
                       const std::string &app_uuid,
                       const std::string &source_result_id,
                       const safe_profile_t &profile,
                       const std::string &current_launch_instance_id,
                       std::uint64_t current_session_generation,
                       std::int64_t now = now_epoch_seconds());

  /** Return only the matching owner's matching game's record. */
  nlohmann::json status(const std::filesystem::path &path,
                        const std::string &owner_uuid,
                        const std::string &app_uuid,
                        std::int64_t now = now_epoch_seconds());

  /** Read a queued record without consuming it (optimizer previews use this). */
  std::optional<record_t> queued(const std::filesystem::path &path,
                                 const std::string &owner_uuid,
                                 const std::string &app_uuid,
                                 std::int64_t now = now_epoch_seconds());

  /**
   * Read a queued record for optimizer launch preflight and atomically bind
   * its resolution to the authenticated paired client's preserved mode.
   */
  std::optional<record_t> prepare_for_optimizer(const std::filesystem::path &path,
                                                const std::string &owner_uuid,
                                                const std::string &app_uuid,
                                                const optimizer_constraints_t &constraints,
                                                std::int64_t now = now_epoch_seconds());

  /** Return terminal and queued receipts visible to only one paired owner. */
  nlohmann::json statuses_for_owner(const std::filesystem::path &path,
                                    const std::string &owner_uuid,
                                    std::int64_t now = now_epoch_seconds());

  /** Remove only the exact matching unconsumed record. */
  nlohmann::json undo(const std::filesystem::path &path,
                      const std::string &owner_uuid,
                      const std::string &app_uuid,
                      const std::string &run_id,
                      std::int64_t now = now_epoch_seconds());

  /**
   * Remove an exact queued run. Paired routes provide owner_scope; the
   * authenticated host route may omit it while still requiring the run id.
   */
  nlohmann::json undo_run(const std::filesystem::path &path,
                          const std::string &run_id,
                          const std::optional<std::string> &owner_scope,
                          std::int64_t now = now_epoch_seconds());

  /** Consume exactly once after a trusted host-side effective-settings match. */
  nlohmann::json verify(const std::filesystem::path &path,
                        const std::string &owner_uuid,
                        const std::string &app_uuid,
                        const std::string &run_id,
                        const observed_launch_t &observed,
                        std::int64_t now = now_epoch_seconds());

  nlohmann::json profile_json(const safe_profile_t &profile);
  nlohmann::json receipt_json(const record_t &record);

  /** Overlay a queued profile after ordinary optimizer normalization. */
  nlohmann::json overlay_optimization(nlohmann::json normalized,
                                      const record_t &record);
}  // namespace recovery_profile
