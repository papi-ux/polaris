/**
 * @file src/process.h
 * @brief Declarations for the startup and shutdown of the apps started by a streaming Session.
 */
#pragma once

#ifndef __kernel_entry
  #define __kernel_entry
#endif

#ifndef BOOST_PROCESS_VERSION
  #define BOOST_PROCESS_VERSION 1
#endif

// standard includes
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <unordered_set>
#include <unordered_map>

// lib includes
#include <boost/process/v1/child.hpp>
#include <boost/process/v1/group.hpp>
#include <boost/process/v1/environment.hpp>
#include <boost/process/v1/search_path.hpp>
#include <boost/property_tree/ptree.hpp>
#include <nlohmann/json.hpp>

// local includes
#include "config.h"
#include "audio.h"
#include "platform/common.h"
#include "rtsp.h"
#include "utility.h"

#ifdef _WIN32
  #include "platform/windows/virtual_display.h"
#endif

#define VIRTUAL_DISPLAY_UUID "8902CB19-674A-403D-A587-41B092E900BA"
#define FALLBACK_DESKTOP_UUID "EAAC6159-089A-46A9-9E24-6436885F6610"
#define REMOTE_INPUT_UUID "8CB5C136-DA67-4F99-B4A1-F9CD35005CF4"
#define TERMINATE_APP_UUID "E16CBE1B-295D-4632-9A76-EC4180C857D3"

namespace proc {
  using file_t = util::safe_ptr_v2<FILE, int, fclose>;

#ifdef __linux__
  struct steam_big_picture_guard_runtime_t;
  struct steam_big_picture_guard_snapshot_t;
#endif

#ifdef _WIN32
  extern VDISPLAY::DRIVER_STATUS vDisplayDriverStatus;
#endif

  inline constexpr std::string_view STEAM_LAUNCH_MODE_DIRECT = "direct";
  inline constexpr std::string_view STEAM_LAUNCH_MODE_BIG_PICTURE = "big-picture";

  std::string normalize_steam_launch_mode(std::string mode);
  bool is_valid_steam_launch_mode(std::string_view mode);
  bool steam_launch_mode_is_big_picture(std::string_view mode);

#if defined(POLARIS_TESTS)
  std::optional<int> resolve_device_db_launch_bitrate_for_tests(
    int configured_max_bitrate,
    const std::optional<int> &paired_target_bitrate_kbps,
    bool ai_auto_quality_enabled,
    const std::string &device_name,
    const std::string &app_name
  );
#endif

#if defined(__linux__)
  struct desktop_launch_safety_policy_t {
    bool desktopSteamActive = false;
    bool physicalDisplayRisk = false;
    bool canLaunchPrivateStream = true;
    bool canMirrorDesktop = false;
    bool canForceCloseDesktopSteamForPrivateStream = false;
    std::string recommendedAction;
    std::string privateStreamUnavailableReason;
    std::string forcePrivateStreamLabel;
  };

  desktop_launch_safety_policy_t resolve_desktop_launch_safety_policy(
    bool private_stream_requested,
    bool mirror_desktop_explicit,
    bool force_private_after_desktop_steam_shutdown,
    const struct ctx_t &app,
    bool desktop_steam_active,
    bool active_desktop_game
  );

  nlohmann::json desktop_launch_safety_policy_to_json(const desktop_launch_safety_policy_t &policy);
  bool desktop_steam_client_active();
  bool request_desktop_steam_shutdown_for_private_stream();
#endif

#if defined(POLARIS_TESTS) && defined(__linux__)

  enum class steam_game_process_event_kind_e {
    none,
    started,
    stopped,
  };

  struct steam_game_process_event_t {
    steam_game_process_event_kind_e kind = steam_game_process_event_kind_e::none;
    std::string appid;
  };

  struct steam_big_picture_guard_transition_t {
    bool close_big_picture = false;
    bool open_big_picture = false;
    std::size_t active_games = 0;
  };

  enum class steam_big_picture_guard_file_scenario_e {
    appended_lifecycle,
    close_dispatch_retry,
    open_dispatch_retry,
    teardown_while_closed,
    truncation_while_closed,
  };

  bool steam_big_picture_input_guard_enabled_for_tests(
    const struct ctx_t &app,
    bool use_cage_compositor,
    bool mirror_desktop
  );
  steam_game_process_event_t parse_steam_game_process_event_for_tests(std::string_view line);
  steam_big_picture_guard_transition_t apply_steam_big_picture_guard_event_for_tests(
    std::unordered_set<std::string> &active_appids,
    std::string_view line
  );
  std::vector<std::string> run_steam_big_picture_guard_file_scenario_for_tests(
    steam_big_picture_guard_file_scenario_e scenario
  );

  desktop_launch_safety_policy_t resolve_desktop_launch_safety_policy_for_tests(
    bool private_stream_requested,
    bool mirror_desktop_explicit,
    bool app_uses_steam,
    bool desktop_steam_active,
    bool active_desktop_game,
    bool force_private_after_desktop_steam_shutdown = false
  );

  bool desktop_steam_client_process_for_tests(std::string_view comm,
                                               std::string_view argv0_path,
                                               std::string_view cmdline,
                                               std::string_view status = {});

  bool cage_mangohud_allowed_for_session_for_tests(const struct ctx_t &app,
                                                   bool use_cage_compositor,
                                                   bool requested_headless);
  bool should_skip_steam_shutdown_undo_after_cage_cleanup_for_tests(const struct ctx_t &app,
                                                                   const config::prep_cmd_t &cmd,
                                                                   bool use_cage_compositor);
#endif

  typedef config::prep_cmd_t cmd_t;

  /**
   * pre_cmds -- guaranteed to be executed unless any of the commands fail.
   * detached -- commands detached from Sunshine
   * cmd -- Runs indefinitely until:
   *    No session is running and a different set of commands it to be executed
   *    Command exits
   * working_dir -- the process working directory. This is required for some games to run properly.
   * cmd_output --
   *    empty    -- The output of the commands are appended to the output of sunshine
   *    "null"   -- The output of the commands are discarded
   *    filename -- The output of the commands are appended to filename
   */
  struct ctx_t {
    std::vector<cmd_t> prep_cmds;
    std::vector<cmd_t> state_cmds;

    /**
     * Some applications, such as Steam, either exit quickly, or keep running indefinitely.
     *
     * Apps that launch normal child processes and terminate will be handled by the process
     * grouping logic (wait_all). However, apps that launch child processes indirectly or
     * into another process group (such as UWP apps) can only be handled by the auto-detach
     * heuristic which catches processes that exit 0 very quickly, but we won't have proper
     * process tracking for those.
     *
     * For cases where users just want to kick off a background process and never manage the
     * lifetime of that process, they can use detached commands for that.
     */
    std::vector<std::string> detached;

    std::string idx;
    std::string uuid;
    std::string name;
    std::string cmd;
    std::string working_dir;
    std::string output;
    std::string image_path;
    std::string id;
    std::string gamepad;
    std::string steam_appid;
    std::string steam_launch_mode = std::string {STEAM_LAUNCH_MODE_DIRECT};
    std::string game_category;  // "fast_action", "cinematic", "desktop", "vr", or ""
    std::string source;         // "steam", "lutris", "heroic", or "manual"
    std::vector<std::string> genres;
    std::map<std::string, std::string> env_vars;  // per-app environment variables
    int64_t last_launched = 0;  // unix timestamp (seconds since epoch)
    bool elevated;
    bool auto_detach;
    bool wait_all;
    bool virtual_display;
    bool virtual_display_primary;
    bool use_app_identity;
    bool per_client_app_identity;
    bool allow_client_commands;
    bool terminate_on_pause;
    int  scale_factor;
    std::chrono::seconds exit_timeout;
  };

  enum class session_stop_outcome_t {
    allowed,
    no_active_session,
    permission_denied,
    stop_in_progress,
    viewer_forbidden,
    uncontrolled_stream,
    other_owner,
    token_mismatch,
    session_changed,
  };

  struct session_stop_snapshot_t {
    session_stop_outcome_t outcome = session_stop_outcome_t::no_active_session;
    int running_app_id = 0;
    int active_sessions = 0;
    bool had_running_app = false;
    bool owned_by_client = false;
    rtsp_stream::session_role_e requester_role = rtsp_stream::session_role_e::none;
    bool stop_in_progress = false;
    bool token_available = false;
    std::string session_token;
    std::string game;
  };

  struct session_stop_result_t {
    session_stop_snapshot_t snapshot;
    bool stopped = false;
  };

  struct session_status_snapshot_t {
    session_stop_snapshot_t stop;
    int viewer_count = 0;
    std::string owner_unique_id;
    std::string owner_device_name;
    std::string game;
    std::string game_uuid;
    bool client_commands_enabled = false;
    bool mangohud_configured = false;
    bool virtual_display = false;
    bool display_mode_explicit = false;
  };

  session_stop_outcome_t evaluate_session_stop_request(
    bool can_launch,
    bool has_running_app,
    int active_sessions,
    bool owned_by_client,
    rtsp_stream::session_role_e requester_role,
    bool stop_in_progress,
    bool token_matches
  );
  bool session_token_matches_active(std::string_view expected_token, std::string_view active_token);
  bool session_stop_token_matches(
    bool require_exact_token,
    std::string_view expected_token,
    std::string_view active_token,
    const std::vector<std::string> &requester_rtsp_tokens
  );

  class session_lifecycle_gate_t {
  public:
    void begin_launch();
    std::optional<std::uint64_t> capture_launch_generation() const;
    bool try_begin_rtsp_launch();
    bool try_begin_rtsp_launch(std::uint64_t expected_generation);
    bool begin_stop();
    bool transition_launch_to_stop();
    void begin_snapshot();
    bool try_begin_snapshot();
    void finish_snapshot();
    void finish_launch();
    void finish_stop(bool committed = true);
    bool stop_in_progress() const;

  private:
    enum class state_e {
      idle,
      launching,
      snapshotting,
      stopping,
    };

    mutable std::mutex _mutex;
    mutable std::condition_variable _changed;
    state_e _state = state_e::idle;
    bool _stop_waiting = false;
    bool _launch_to_stop_handoff = false;
    bool _last_stop_committed = false;
    std::uint64_t _generation = 1;
  };

  class session_snapshot_guard_t {
  public:
    explicit session_snapshot_guard_t(session_lifecycle_gate_t &gate);
    static session_snapshot_guard_t try_acquire(session_lifecycle_gate_t &gate);
    ~session_snapshot_guard_t();
    bool owns_snapshot() const;
    session_snapshot_guard_t(const session_snapshot_guard_t &) = delete;
    session_snapshot_guard_t &operator=(const session_snapshot_guard_t &) = delete;
    session_snapshot_guard_t(session_snapshot_guard_t &&other) noexcept;
    session_snapshot_guard_t &operator=(session_snapshot_guard_t &&) = delete;

  private:
    session_snapshot_guard_t() = default;
    session_lifecycle_gate_t *_gate = nullptr;
  };

  struct session_status_view_t {
    session_snapshot_guard_t guard;
    session_status_snapshot_t snapshot;
  };

  class proc_t {
  public:
    KITTY_DEFAULT_CONSTR_MOVE_THROW(proc_t)

    std::string display_name;
    std::string initial_display;
    std::string mode_changed_display;
    bool initial_hdr = false;
    bool virtual_display = false;
    bool allow_client_commands = false;
    int initial_color_range = 0;
    int initial_nvenc_tune = 0;
    int initial_max_bitrate = 0;
    int initial_adaptive_max_bitrate = 0;
    bool initial_video_config_saved = false;

    proc_t(
      boost::process::v1::environment &&env,
      std::vector<ctx_t> &&apps
    ):
        _env(std::move(env)),
        _apps(std::move(apps)) {
    }

    void launch_input_only(std::shared_ptr<rtsp_stream::launch_session_t> launch_session);
    bool launch_input_only_and_raise(std::shared_ptr<rtsp_stream::launch_session_t> launch_session);

    int execute(const ctx_t& _app, std::shared_ptr<rtsp_stream::launch_session_t> launch_session);
    int execute_and_raise(const ctx_t& _app, std::shared_ptr<rtsp_stream::launch_session_t> launch_session);
    bool raise_session_for_admitted_launch(std::shared_ptr<rtsp_stream::launch_session_t> launch_session);
    std::optional<std::uint64_t> capture_session_launch_generation() const;
    bool try_begin_session_launch(std::uint64_t expected_generation);
    void finish_session_launch();

    /**
     * @return `_app_id` if a process is running, otherwise returns `0`
     */
    int running();

    ~proc_t();

    std::vector<ctx_t> get_apps() const;
    std::string get_app_image(int app_id);
    std::string get_last_run_app_name();
    std::string get_running_app_uuid();
    std::string get_session_token();
    std::string get_session_owner_unique_id();
    std::string get_session_owner_device_name();
    bool is_session_owner(const std::string &unique_id);
    bool session_uses_virtual_display();
    bool session_allows_client_commands();
    void mark_client_session_report_recorded(const std::string &unique_id);
    bool client_session_report_recorded() const;
    bool session_display_mode_is_explicit() const;
    bool current_app_has_mangohud() const;
    void set_app_mangohud_configured(const std::string &uuid, bool enabled);
    void set_app_steam_launch_mode_configured(const std::string &uuid, const std::string &mode);
    bool session_shutdown_requested() const;
    session_status_view_t get_session_status_view(const std::string &unique_id, bool can_launch);
    session_stop_snapshot_t get_session_stop_snapshot(const std::string &unique_id, bool can_launch);
    session_stop_result_t request_session_shutdown(
      const std::string &unique_id,
      std::string_view expected_token,
      bool can_launch,
      bool require_exact_token
    );
    boost::process::v1::environment get_env();
    void resume();
    void pause();
    void terminate(bool immediate = false, bool needs_refresh = true);
    void terminate_from_admitted_launch();
    bool reload_configuration_from_file(const std::string &file_name);
    void reload_configuration(proc_t &&parsed);
#if defined(POLARIS_TESTS)
    std::pair<const void *, const void *> session_lifecycle_identity_for_tests() const;
    void with_session_lifecycle_lock_for_tests(const std::function<void()> &callback);
    bool begin_session_stop_for_tests();
    void finish_session_stop_for_tests(bool committed);
#endif

  private:
    struct session_lifecycle_sync_t {
      std::recursive_mutex mutex;
    };

    void launch_input_only_impl(std::shared_ptr<rtsp_stream::launch_session_t> launch_session);
    int execute_impl(
      const ctx_t& app,
      std::shared_ptr<rtsp_stream::launch_session_t> launch_session,
      bool no_active_sessions_at_launch
    );
    void terminate_impl(bool immediate, bool needs_refresh);
#ifdef __linux__
    std::shared_ptr<const steam_big_picture_guard_snapshot_t> snapshot_steam_big_picture_input_guard(
      bool use_cage_compositor,
      bool mirror_desktop
    ) const;
    void start_steam_big_picture_input_guard(
      const boost::process::v1::environment &launch_env,
      std::shared_ptr<const steam_big_picture_guard_snapshot_t> snapshot
    );
    void stop_steam_big_picture_input_guard();
#endif
    session_lifecycle_sync_t &session_lifecycle_sync() const;
    session_stop_snapshot_t get_session_stop_snapshot_locked(
      const std::string &unique_id,
      bool can_launch,
      std::string_view expected_token,
      bool require_exact_token,
      const rtsp_stream::session_snapshot_t &rtsp_snapshot,
      bool stop_in_progress
    );

    int _app_id = 0;
    std::string _app_name;

    boost::process::v1::environment _env;

    std::shared_ptr<rtsp_stream::launch_session_t> _launch_session;
    std::shared_ptr<config::input_t> _saved_input_config;
    audio::audio_ctx_ref_t _audio_context;

    std::vector<ctx_t> _apps;
    ctx_t _app;
    std::chrono::steady_clock::time_point _app_launch_time;

    // If no command associated with _app_id, yet it's still running
    bool placebo {};

    boost::process::v1::child _process;
    boost::process::v1::group _process_group;

    file_t _pipe;
    std::unordered_set<std::string> _session_env_keys;
#ifdef __linux__
    std::shared_ptr<steam_big_picture_guard_runtime_t> _steam_big_picture_guard;
#endif
    std::vector<cmd_t>::const_iterator _app_prep_it;
    std::vector<cmd_t>::const_iterator _app_prep_begin;
    std::shared_ptr<session_lifecycle_gate_t> _session_lifecycle_gate {std::make_shared<session_lifecycle_gate_t>()};
    std::shared_ptr<session_lifecycle_sync_t> _session_lifecycle_sync {std::make_shared<session_lifecycle_sync_t>()};
    std::uint64_t _session_generation = 0;
    bool _client_session_report_recorded = false;
    std::chrono::steady_clock::time_point _client_session_report_recorded_at {};
    std::string _client_session_report_recorded_unique_id;
  };

  boost::filesystem::path
  find_working_directory(const std::string &cmd, const boost::process::v1::environment &env);

  /**
   * @brief Calculate a stable id based on name and image data
   * @return Tuple of id calculated without index (for use if no collision) and one with.
   */
  std::tuple<std::string, std::string> calculate_app_id(const std::string &app_name, std::string app_image_path, int index);

  std::string validate_app_image_path(std::string app_image_path);
  void refresh(const std::string &file_name, bool needs_terminate = true);
  void migrate_apps(nlohmann::json* fileTree_p, nlohmann::json* inputTree_p);
  std::optional<proc::proc_t> parse(const std::string &file_name);

  /**
   * @brief Initialize proc functions
   * @return Unique pointer to `deinit_t` to manage cleanup
   */
  std::unique_ptr<platf::deinit_t> init();

  /**
   * @brief Terminates all child processes in a process group.
   * @param proc The child process itself.
   * @param group The group of all children in the process tree.
   * @param exit_timeout The timeout to wait for the process group to gracefully exit.
   */
  void terminate_process_group(boost::process::v1::child &proc, boost::process::v1::group &group, std::chrono::seconds exit_timeout);

  extern proc_t proc;

  extern int input_only_app_id;
  extern std::string input_only_app_id_str;
  extern int terminate_app_id;
  extern std::string terminate_app_id_str;
}  // namespace proc

#ifdef BOOST_PROCESS_VERSION
  #undef BOOST_PROCESS_VERSION
#endif
