/**
 * @file src/doctor_actions.h
 * @brief Evidence-gated, reversible one-click Doctor actions shared by web and paired clients.
 */
#pragma once

#include <filesystem>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>

#include <nlohmann/json.hpp>

#include "stream_stats.h"

namespace doctor_actions {

  struct recovery_action_context_t {
    bool active_owner = false;
    bool host_tuning_allowed = false;
    bool caller_is_viewer = false;
    bool require_owner_scope = true;
    bool enforce_request_scope = false;
    std::string owner_uuid;
    std::string device_name;
    std::string app_uuid;
    std::string app_name;
    std::string launch_instance_id;
    std::uint64_t session_generation = 0;
    std::string effective_stream_display_mode;
    std::filesystem::path state_path;
    stream_stats::stats_t stats;
    nlohmann::json health;
  };

  /** Current telemetry must pass this guard before Doctor may reduce quality. */
  bool network_pressure_confirmed(const stream_stats::stats_t &stats);

  /** Paired-route authorization, including owner-scoped Undo after disconnect. */
  bool paired_route_allowed(std::string_view action_id,
                            std::string_view run_id,
                            bool active_owner_present,
                            bool caller_is_active_owner);

  /**
   * Sole-owner transaction guard for process-global paired-client controls.
   *
   * The held guard serializes the authorization decision and the caller's
   * mutation with stream generation handoff. A false guard owns no lock.
   */
  class paired_global_control_guard_t {
   public:
    paired_global_control_guard_t() = default;
    paired_global_control_guard_t(const paired_global_control_guard_t &) = delete;
    paired_global_control_guard_t &operator=(const paired_global_control_guard_t &) = delete;
    paired_global_control_guard_t(paired_global_control_guard_t &&) noexcept = default;
    paired_global_control_guard_t &operator=(paired_global_control_guard_t &&) noexcept = default;

    explicit operator bool() const noexcept { return authorized_; }
    bool set_adaptive_enabled(bool enabled);
    void release() noexcept {
      if (lock_.owns_lock()) lock_.unlock();
    }

   private:
    friend paired_global_control_guard_t acquire_paired_global_control(
      std::string_view owner_uuid,
      std::uint64_t session_generation,
      std::string_view launch_instance_id
    );

    paired_global_control_guard_t(std::unique_lock<std::mutex> lock,
                                  bool authorized) noexcept:
        lock_(std::move(lock)),
        authorized_(authorized) {
    }

    std::unique_lock<std::mutex> lock_;
    bool authorized_ = false;
  };

  paired_global_control_guard_t acquire_paired_global_control(
    std::string_view owner_uuid,
    std::uint64_t session_generation = 0,
    std::string_view launch_instance_id = {}
  );

  /** Atomically apply a live paired-client bitrate only for the sole owner. */
  bool set_owner_live_bitrate(std::string_view owner_uuid,
                              std::uint64_t session_generation,
                              std::string_view launch_instance_id,
                              int bitrate_kbps);

  /** Apply an authenticated host-admin adaptive toggle without inheriting Doctor's temporary target. */
  void set_adaptive_enabled(bool enabled);

  /** Clamp a proposed bitrate to one guarded reduction step. */
  int guarded_bitrate_target(int current_bitrate_kbps,
                             int requested_bitrate_kbps,
                             int minimum_bitrate_kbps);

  /** Clamp a quality retry to one 25% increase toward the paired target. */
  int guarded_quality_retry_target(int current_bitrate_kbps,
                                   int paired_target_bitrate_kbps);

  /** Execute recheck, apply, verify, or undo and return the Doctor action result contract. */
  nlohmann::json execute(const nlohmann::json &request);

  /** Execute with trusted current owner/app/telemetry for durable recovery actions. */
  nlohmann::json execute(const nlohmann::json &request,
                         const recovery_action_context_t &recovery_context);

  /**
   * Serialize global controller initialization with Doctor rollback and track
   * the exact sessions that can observe that process-global actuator.
   */
  void session_started(std::string_view owner_uuid,
                       std::uint64_t session_generation,
                       std::string_view launch_instance_id,
                       int base_bitrate_kbps);

  /** Roll back and retire a same-stream action when its authenticated session ends. */
  void session_ended(std::string_view owner_uuid, std::uint64_t session_generation);

#ifdef POLARIS_TESTS
  /** Compatibility helper for unit fixtures that do not model app-session tokens. */
  void session_started(std::string_view owner_uuid,
                       std::uint64_t session_generation,
                       int base_bitrate_kbps);

  /** Make the active receipt's post-change window due without sleeping in unit tests. */
  void make_verification_due_for_tests();

  /** Complete the active host-received evidence window without sleeping. */
  void make_verification_window_complete_for_tests();

  /** Run the active receipt's verification watchdog synchronously in unit tests. */
  void run_verification_watchdog_for_tests();
#endif

}  // namespace doctor_actions
