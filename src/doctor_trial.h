/**
 * @file src/doctor_trial.h
 * @brief Private, owner/app-scoped Doctor v2 fresh-launch trial contract.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

#include <nlohmann/json.hpp>

namespace doctor_trial {
  inline constexpr std::int64_t lifetime_seconds = 24 * 60 * 60;

  struct effective_settings_t {
    std::string topology;
    int width = 0;
    int height = 0;
    int target_fps = 0;
    int bitrate_kbps = 0;
    std::string codec;
    bool hdr = false;
  };

  struct launch_override_t {
    std::string run_id;
    int target_fps = 0;
  };

  /** Derive and persist a one-dimension FPS proposal from complete v2 evidence. */
  nlohmann::json propose(const std::filesystem::path &path,
                         const std::string &owner_uuid,
                         const std::string &app_uuid,
                         const std::string &launch_instance_id,
                         std::uint64_t session_generation,
                         const nlohmann::json &doctor_status,
                         const effective_settings_t &settings,
                         std::int64_t now);

  /** Explicitly queue an exact proposal. */
  nlohmann::json confirm(const std::filesystem::path &path,
                         const std::string &owner_uuid,
                         const std::string &app_uuid,
                         const std::string &run_id,
                         std::int64_t now);

  nlohmann::json status(const std::filesystem::path &path,
                        const std::string &owner_uuid,
                        const std::string &app_uuid,
                        std::int64_t now);

  nlohmann::json cancel(const std::filesystem::path &path,
                        const std::string &owner_uuid,
                        const std::string &app_uuid,
                        const std::string &run_id,
                        std::int64_t now);

  /** Consume a queued proposal once on a fresh launch of the exact owner/app. */
  std::optional<launch_override_t> begin_launch(const std::filesystem::path &path,
                                                 const std::string &owner_uuid,
                                                 const std::string &app_uuid,
                                                 const std::string &launch_instance_id,
                                                 std::uint64_t launch_generation,
                                                 std::int64_t now);

  /** Advance collecting/terminal state from a full post-warmup v2 window. */
  nlohmann::json observe(const std::filesystem::path &path,
                         const std::string &owner_uuid,
                         const std::string &app_uuid,
                         const nlohmann::json &doctor_status,
                         const effective_settings_t &settings,
                         bool crashed,
                         std::int64_t now);

  std::int64_t now_epoch_seconds();
}  // namespace doctor_trial
