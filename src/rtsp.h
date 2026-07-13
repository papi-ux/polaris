/**
 * @file src/rtsp.h
 * @brief Declarations for RTSP streaming.
 */
#pragma once

// standard includes
#include <atomic>
#include <functional>
#include <memory>
#include <list>
#include <vector>

// local includes
#include "crypto.h"
#include "thread_safe.h"

#ifdef _WIN32
  #include <windows.h>
#endif

// Resolve circular dependencies
namespace stream {
  struct session_t;
}

namespace rtsp_stream {
  constexpr auto RTSP_SETUP_PORT = 21;

  enum class session_role_e {
    none,
    viewer,
    controller,
  };

  struct session_snapshot_t {
    int active_sessions = 0;
    int pending_sessions = 0;
    int viewer_count = 0;
    session_role_e requester_role = session_role_e::none;
    std::vector<std::string> requester_session_tokens;
  };

  struct launch_session_t {
    uint32_t id;

    crypto::aes_t gcm_key;
    crypto::aes_t iv;

    std::string av_ping_payload;
    uint32_t control_connect_data;

    std::string device_name;
    std::string unique_id;
    std::string session_token;
    crypto::PERM perm;
    bool watch_only;

    enum class setup_state_e : std::uint8_t {
      pending,
      handoff,
      started,
      cancelled,
    };

    bool try_begin_setup_handoff() {
      auto expected = setup_state_e::pending;
      return setup_state.compare_exchange_strong(expected, setup_state_e::handoff);
    }

    bool commit_setup_start() {
      auto expected = setup_state_e::handoff;
      return setup_state.compare_exchange_strong(expected, setup_state_e::started);
    }

    bool cancel_for_timeout() {
      auto expected = setup_state.load();
      while (expected == setup_state_e::pending || expected == setup_state_e::handoff) {
        if (setup_state.compare_exchange_weak(expected, setup_state_e::cancelled)) {
          return true;
        }
      }
      return false;
    }

    void cancel() {
      setup_state.store(setup_state_e::cancelled);
    }

    bool is_pending() const {
      return setup_state.load() == setup_state_e::pending;
    }

    bool is_pending_or_handoff() const {
      const auto state = setup_state.load();
      return state == setup_state_e::pending || state == setup_state_e::handoff;
    }

    bool is_cancelled() const {
      return setup_state.load() == setup_state_e::cancelled;
    }

    // Moonlight sends SETUP and PLAY over separate TCP connections, so started sessions remain admissible.
    bool accepts_control_connection() const {
      return setup_state.load() != setup_state_e::cancelled;
    }

    bool input_only;
    bool host_audio;
    int requested_width;
    int requested_height;
    int requested_fps;
    int width;
    int height;
    int fps;
    int gcmap;
    int surround_info;
    std::string surround_params;
    bool enable_hdr;
    bool enable_sops;
    bool virtual_display;
    bool mirror_desktop = false;
    bool force_private_after_desktop_steam_shutdown = false;
    bool user_locked_display_mode;
    bool user_locked_virtual_display;
    uint32_t scale_factor;
    std::optional<int> paired_target_bitrate_kbps;
    std::optional<int> target_bitrate_kbps;
    std::optional<int> nvenc_tune;
    std::optional<std::string> preferred_codec;
    std::string optimization_source;
    std::string optimization_reasoning;
    std::string optimization_confidence;
    std::string optimization_cache_status;
    std::string optimization_normalization_reason;
    int optimization_recommendation_version = 0;
    std::string pacing_policy;

    std::atomic<setup_state_e> setup_state {setup_state_e::pending};
    std::optional<crypto::cipher::gcm_t> rtsp_cipher;
    std::string rtsp_url_scheme;
    uint32_t rtsp_iv_counter;

    std::list<crypto::command_entry_t> client_do_cmds;
    std::list<crypto::command_entry_t> client_undo_cmds;

  #ifdef _WIN32
    GUID display_guid{};
  #endif
  };

  bool launch_session_raise(std::shared_ptr<launch_session_t> launch_session);

  /**
   * @brief Clear state for the specified launch session.
   * @param launch_session_id The ID of the session to clear.
   */
  void launch_session_clear(uint32_t launch_session_id);

  /**
   * @brief Get the number of active sessions.
   * @return Count of active sessions.
   */
  int session_count();
  int viewer_count();

  std::shared_ptr<stream::session_t>
  find_session(const std::string_view& uuid);

  session_snapshot_t session_snapshot(const std::string_view& uuid);

#ifdef POLARIS_TESTS
  session_role_e merge_session_role_for_tests(session_role_e current, bool watch_only);
  void accumulate_session_snapshot_for_tests(
    session_snapshot_t &snapshot,
    bool requester_matches,
    bool watch_only
  );
  std::uint64_t launch_timer_generation_for_tests();
  bool expire_pending_launch_for_tests(uint32_t launch_session_id, std::uint64_t timer_generation);
  void reset_cleanup_call_count_for_tests();
  unsigned cleanup_call_count_for_tests();
  void set_cleanup_unlocked_probe_for_tests(std::function<void()> probe);
  void set_cleanup_session_probe_for_tests(std::function<void()> probe);
  void run_cleanup_for_tests();
  enum class setup_insert_result_e {
    started,
    cancelled,
    failed,
  };
  setup_insert_result_e run_setup_insert_for_tests(
    launch_session_t &launch_session,
    bool cancel_after_insert,
    int start_result
  );
  void add_session_for_tests(launch_session_t &launch_session, bool stopping);
#endif

  std::list<std::string>
  get_all_session_uuids();

  /**
   * @brief Terminates all running streaming sessions.
   */
  void terminate_sessions();

  /**
   * @brief Runs the RTSP server loop.
   */
  void start();
}  // namespace rtsp_stream
