#include "../tests_common.h"
#include "../tests_paths.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <thread>
#include <src/adaptive_bitrate.h>
#include <src/ai_optimizer.h>
#include <src/file_handler.h>
#include <src/nvhttp.h>
#include <src/process.h>
#ifdef __linux__
  #include <src/platform/linux/cage_display_router.h>
#endif
#include <src/stream_stats.h>

#ifdef __linux__
  #include <csignal>
  #include <pthread.h>
  #include <sys/prctl.h>
  #include <sys/wait.h>
  #include <unistd.h>
#endif

namespace {
#ifdef __linux__
  volatile std::sig_atomic_t sigusr1_interruptions = 0;

  void record_sigusr1_interrupt(int) {
    sigusr1_interruptions = sigusr1_interruptions + 1;
  }

  struct linux_child_guard_t {
    explicit linux_child_guard_t(pid_t child_pid): pid(child_pid) {}
    linux_child_guard_t(const linux_child_guard_t &) = delete;
    linux_child_guard_t &operator=(const linux_child_guard_t &) = delete;

    ~linux_child_guard_t() {
      terminate_and_reap();
    }

    pid_t wait(int *status, int options) {
      if (pid <= 0 || reaped) {
        errno = ECHILD;
        return -1;
      }
      pid_t result;
      do {
        result = waitpid(pid, status, options);
      } while (result < 0 && errno == EINTR);
      if (result == pid || (result < 0 && errno == ECHILD)) {
        reaped = true;
      }
      return result;
    }

    pid_t terminate_and_reap(int *status_out = nullptr) {
      if (pid <= 0 || reaped) {
        errno = ECHILD;
        return -1;
      }
      int local_status = 0;
      int *status = status_out != nullptr ? status_out : &local_status;
      auto observed = wait(status, WNOHANG);
      if (observed == 0) {
        (void) kill(pid, SIGKILL);
        observed = wait(status, 0);
      }
      return observed;
    }

    pid_t pid = -1;
    bool reaped = false;
  };

  struct sigusr1_interrupt_guard_t {
    sigusr1_interrupt_guard_t() {
      struct sigaction action {};
      action.sa_handler = record_sigusr1_interrupt;
      sigemptyset(&action.sa_mask);
      action.sa_flags = 0;
      if (sigaction(SIGUSR1, &action, &previous_action) != 0) {
        return;
      }

      sigemptyset(&signal_set);
      sigaddset(&signal_set, SIGUSR1);
      if (pthread_sigmask(SIG_UNBLOCK, &signal_set, &previous_mask) != 0) {
        (void) sigaction(SIGUSR1, &previous_action, nullptr);
        return;
      }
      installed = true;
    }

    ~sigusr1_interrupt_guard_t() {
      (void) restore();
    }

    bool restore() {
      if (!installed) {
        return true;
      }
      if (pthread_sigmask(SIG_BLOCK, &signal_set, nullptr) != 0) {
        return false;
      }
      const timespec no_wait {};
      for (;;) {
        int result = -1;
        if (forced_drain_interruptions > 0) {
          --forced_drain_interruptions;
          errno = EINTR;
        } else {
          result = sigtimedwait(&signal_set, nullptr, &no_wait);
        }
        if (result == SIGUSR1 || (result < 0 && errno == EINTR)) {
          continue;
        }
        if (result < 0 && errno == EAGAIN) {
          break;
        }
        return false;
      }
      if (sigaction(SIGUSR1, &previous_action, nullptr) != 0) {
        return false;
      }
      if (pthread_sigmask(SIG_SETMASK, &previous_mask, nullptr) != 0) {
        return false;
      }
      installed = false;
      return true;
    }

    bool installed = false;
    int forced_drain_interruptions = 1;
    sigset_t signal_set {};
    sigset_t previous_mask {};
    struct sigaction previous_action {};
  };

  struct linux_cage_compositor_guard_t {
    linux_cage_compositor_guard_t():
        adapter_name(config::video.adapter_name),
        auto_manage_displays(config::video.linux_display.auto_manage_displays),
        use_cage_compositor(config::video.linux_display.use_cage_compositor),
        prefer_gpu_native_capture(config::video.linux_display.prefer_gpu_native_capture) {}

    ~linux_cage_compositor_guard_t() {
      config::video.adapter_name = adapter_name;
      config::video.linux_display.auto_manage_displays = auto_manage_displays;
      config::video.linux_display.use_cage_compositor = use_cage_compositor;
      config::video.linux_display.prefer_gpu_native_capture = prefer_gpu_native_capture;
    }

    std::string adapter_name;
    bool auto_manage_displays;
    bool use_cage_compositor;
    bool prefer_gpu_native_capture;
  };
#endif

  struct auto_quality_guard_t {
    bool ai_enabled;
    bool adaptive_enabled;

    auto_quality_guard_t():
        ai_enabled(ai_optimizer::is_enabled()),
        adaptive_enabled(adaptive_bitrate::is_enabled()) {}

    ~auto_quality_guard_t() {
      ai_optimizer::set_enabled(ai_enabled);
      adaptive_bitrate::set_enabled(adaptive_enabled);
    }
  };

  constexpr const char *expected_steam_shutdown_command() {
#ifdef __linux__
    return "setsid -f steam -shutdown";
#else
    return "setsid steam -shutdown";
#endif
  }

  std::string read_source_file_for_contract(const char *relative_path) {
    const auto path = std::filesystem::path(POLARIS_SOURCE_DIR) / relative_path;
    std::ifstream in(path);
    if (!in) {
      return {};
    }
    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
  }

  /**
   * @brief Collapse whitespace runs so a call's arguments can be matched without
   *        pinning its indentation.
   *
   * These contract tests assert which arguments a call is given, not how the
   * call is wrapped. Matching raw source text makes them fail on reformatting or
   * on the extra indent that comes from nesting the call one block deeper, which
   * says nothing about the contract.
   */
  std::string collapse_whitespace(std::string_view source) {
    std::string collapsed;
    collapsed.reserve(source.size());
    bool in_space = false;
    for (const char ch : source) {
      if (std::isspace(static_cast<unsigned char>(ch))) {
        in_space = true;
        continue;
      }
      if (in_space && !collapsed.empty()) {
        collapsed.push_back(' ');
      }
      in_space = false;
      collapsed.push_back(ch);
    }
    return collapsed;
  }

}  // namespace

TEST(ProcessRuntimeConfigTests, PolarisV1SessionStopContractIsAdvertisedAndRouted) {
  const auto source = read_source_file_for_contract("src/nvhttp.cpp");
  ASSERT_FALSE(source.empty());

  const auto status_start = source.find("auto polarisSessionStatus");
  const auto status_end = source.find("auto polarisStreamPolicy", status_start);
  const auto stop_start = source.find("auto polarisSessionStop");
  const auto stop_end = source.find("// Client session report", stop_start);
  const auto cancel_start = source.find("void cancel(");
  const auto cancel_end = source.find("void appasset(", cancel_start);
  ASSERT_NE(status_start, std::string::npos);
  ASSERT_NE(status_end, std::string::npos);
  ASSERT_NE(stop_start, std::string::npos);
  ASSERT_NE(stop_end, std::string::npos);
  ASSERT_NE(cancel_start, std::string::npos);
  ASSERT_NE(cancel_end, std::string::npos);

  const auto status_handler = source.substr(status_start, status_end - status_start);
  const auto stop_handler = source.substr(stop_start, stop_end - stop_start);
  const auto cancel_handler = source.substr(cancel_start, cancel_end - cancel_start);

  EXPECT_NE(source.find("session_stop_v1"), std::string::npos);
  EXPECT_NE(source.find("/polaris/v1/session/stop"), std::string::npos);
  EXPECT_NE(status_handler.find("PERM::launch"), std::string::npos);
  EXPECT_NE(status_handler.find("get_session_status_view("), std::string::npos);
  EXPECT_NE(status_handler.find("auto status_view = proc::proc.get_session_status_view("), std::string::npos);
  EXPECT_NE(status_handler.find("const auto &status_snapshot = status_view.snapshot"), std::string::npos);
  const auto status_view_claim = status_handler.find("get_session_status_view(");
  const auto status_stats_read = status_handler.find("stream_stats::get_current()");
  ASSERT_NE(status_view_claim, std::string::npos);
  ASSERT_NE(status_stats_read, std::string::npos);
  EXPECT_LT(status_view_claim, status_stats_read);
  for (const auto loose_read : {
         "proc::proc.get_session_owner_unique_id()",
         "proc::proc.get_session_owner_device_name()",
         "rtsp_stream::viewer_count()",
         "proc::proc.session_allows_client_commands()",
         "proc::proc.get_last_run_app_name()",
         "proc::proc.get_running_app_uuid()",
         "proc::proc.current_app_has_mangohud()",
         "proc::proc.session_uses_virtual_display()",
         "proc::proc.session_display_mode_is_explicit()"
       }) {
    EXPECT_EQ(status_handler.find(loose_read), std::string::npos);
  }
  const auto captured_display_helper_start = source.find(
    "std::string effective_stream_display_mode_selection(\n      const stream_stats::stats_t &stats,"
  );
  const auto captured_display_helper_end = source.find(
    "std::string effective_stream_display_mode_selection(const stream_stats::stats_t &stats)",
    captured_display_helper_start
  );
  ASSERT_NE(captured_display_helper_start, std::string::npos);
  ASSERT_NE(captured_display_helper_end, std::string::npos);
  const auto captured_display_helper = source.substr(
    captured_display_helper_start,
    captured_display_helper_end - captured_display_helper_start
  );
  EXPECT_EQ(captured_display_helper.find("proc::proc.session_uses_virtual_display()"), std::string::npos);
  const auto captured_display_call = status_handler.find("effective_stream_display_mode_selection(");
  const auto captured_display_call_end = status_handler.find(");", captured_display_call);
  const auto captured_virtual_display = status_handler.find(
    "status_snapshot.virtual_display",
    captured_display_call
  );
  ASSERT_NE(captured_display_call, std::string::npos);
  ASSERT_NE(captured_display_call_end, std::string::npos);
  ASSERT_NE(captured_virtual_display, std::string::npos);
  EXPECT_LT(captured_display_call, captured_virtual_display);
  EXPECT_LT(captured_virtual_display, captured_display_call_end);
  EXPECT_NE(status_handler.find("session_stop_outcome_t::allowed"), std::string::npos);
  EXPECT_NE(stop_handler.find("PERM::launch"), std::string::npos);
  EXPECT_NE(stop_handler.find("request_session_shutdown("), std::string::npos);
  EXPECT_NE(stop_handler.find("session_stop_outcome_t::no_active_session"), std::string::npos);
  EXPECT_EQ(stop_handler.find("session_token_matches_value"), std::string::npos);
  EXPECT_EQ(stop_handler.find("request_active_session_shutdown"), std::string::npos);
  EXPECT_EQ(cancel_handler.find("session_token_matches_request"), std::string::npos);
  EXPECT_EQ(cancel_handler.find("request_active_session_shutdown"), std::string::npos);
  // The paired certificate is the owner identity, so an owner cancel must not be
  // held to a sessiontoken match — clients routinely send a stale one. Everyone
  // else still needs the token to match when they supplied one.
  EXPECT_NE(
    collapse_whitespace(cancel_handler)
      .find("request_session_shutdown( named_cert_p->uuid, session_token, true, !session_token.empty() && !is_owner )"),
    std::string::npos
  );
  EXPECT_NE(
    collapse_whitespace(stop_handler)
      .find("request_session_shutdown( named_cert_p->uuid, expected_token, can_launch, true )"),
    std::string::npos
  );
}

TEST(ProcessRuntimeConfigTests, NestedSessionPrepReceivesCredentialAndFailsLaunchClosed) {
  const auto source = read_source_file_for_contract("src/process.cpp");
  ASSERT_FALSE(source.empty());
  const auto execute_start = source.find("int proc_t::execute_impl(");
  const auto terminate_start = source.find("void proc_t::terminate_impl(", execute_start);
  ASSERT_NE(execute_start, std::string::npos);
  ASSERT_NE(terminate_start, std::string::npos);
  const auto body = source.substr(execute_start, terminate_start - execute_start);
  const auto credential = body.find("set_child_only_session_env_var(");
  const auto prep_loop = body.find("for (; _app_prep_it != std::end(_app.prep_cmds)");
  ASSERT_NE(credential, std::string::npos);
  ASSERT_NE(prep_loop, std::string::npos);
  EXPECT_LT(credential, prep_loop);
  EXPECT_NE(body.find("critical nested gamescope prep command failed"), std::string::npos);
  EXPECT_NE(body.find("return 503;", prep_loop), std::string::npos);
  const auto app_env_loop = body.find("for (const auto &[key, val] : _app.env_vars)", prep_loop);
  ASSERT_NE(app_env_loop, std::string::npos);
  const auto reserved_filter = body.find("is_reserved_session_env_key(key)", app_env_loop);
  ASSERT_NE(reserved_filter, std::string::npos);
  const auto credential_reassert = body.find("set_child_only_session_env_var(", credential + 1);
  ASSERT_NE(credential_reassert, std::string::npos);
  EXPECT_LT(app_env_loop, reserved_filter);
  EXPECT_LT(reserved_filter, credential_reassert);
  EXPECT_NE(body.find("platf::unset_env(\"POLARIS_SESSION_INSTANCE_ID\")", app_env_loop), std::string::npos);
}

TEST(ProcessRuntimeConfigTests, SessionLifecycleGateOwnsLaunchRaiseAndTeardownWithoutCrossLockingRtsp) {
  const auto header = read_source_file_for_contract("src/process.h");
  const auto source = read_source_file_for_contract("src/process.cpp");
  const auto rtsp_source = read_source_file_for_contract("src/rtsp.cpp");
  const auto rtsp_header = read_source_file_for_contract("src/rtsp.h");
  const auto nvhttp = read_source_file_for_contract("src/nvhttp.cpp");
  ASSERT_FALSE(header.empty());
  ASSERT_FALSE(source.empty());
  ASSERT_FALSE(rtsp_source.empty());
  ASSERT_FALSE(rtsp_header.empty());
  ASSERT_FALSE(nvhttp.empty());

  EXPECT_NE(header.find("_session_lifecycle_gate"), std::string::npos);
  EXPECT_NE(header.find("execute_and_raise"), std::string::npos);
  EXPECT_NE(header.find("raise_session_for_admitted_launch"), std::string::npos);
  EXPECT_NE(header.find("capture_launch_generation"), std::string::npos);
  EXPECT_NE(source.find("session_snapshot({}).pending_sessions > 0"), std::string::npos);

  const auto execute_start = source.find("int proc_t::execute_impl(");
  const auto execute_end = source.find("int proc_t::running()", execute_start);
  ASSERT_NE(execute_start, std::string::npos);
  ASSERT_NE(execute_end, std::string::npos);
  const auto execute_impl = source.substr(execute_start, execute_end - execute_start);
  EXPECT_EQ(source.find("session_count_without_process_lock"), std::string::npos);
  EXPECT_EQ(execute_impl.find("lifecycle_lock.unlock()"), std::string::npos);
  EXPECT_NE(execute_impl.find("no_active_sessions_at_launch"), std::string::npos);
  EXPECT_EQ(execute_impl.find("rtsp_stream::session_count()"), std::string::npos);
  const auto function_source_between = [](const std::string &text, std::string_view begin, std::string_view end) {
    const auto start = text.find(begin);
    if (start == std::string::npos) {
      return std::string {};
    }
    const auto finish = text.find(end, start + begin.size());
    if (finish == std::string::npos) {
      return std::string {};
    }
    return text.substr(start, finish - start);
  };
  const auto begin_stop_body = function_source_between(
    source,
    "bool session_lifecycle_gate_t::begin_stop()",
    "void session_lifecycle_gate_t::finish_stop(bool"
  );
  const auto snapshot_wait = begin_stop_body.find("_state != state_e::snapshotting");
  const auto stop_publish = begin_stop_body.find("_stop_waiting = true");
  ASSERT_NE(snapshot_wait, std::string::npos);
  ASSERT_NE(stop_publish, std::string::npos);
  EXPECT_LT(snapshot_wait, stop_publish);
  const auto resume_body = function_source_between(source, "void proc_t::resume()", "void proc_t::pause()");
  const auto pause_body = function_source_between(source, "void proc_t::pause()", "void proc_t::terminate(");
  const auto report_writer = function_source_between(source, "void proc_t::mark_client_session_report_recorded", "bool proc_t::client_session_report_recorded");
  const auto report_reader = function_source_between(source, "bool proc_t::client_session_report_recorded", "bool proc_t::session_display_mode_is_explicit");
  const auto display_reader = function_source_between(source, "bool proc_t::session_display_mode_is_explicit", "bool proc_t::current_app_has_mangohud");
  const auto mangohud_reader = function_source_between(source, "bool proc_t::current_app_has_mangohud", "void proc_t::set_app_mangohud_configured");
  EXPECT_NE(resume_body.find("session_lifecycle_sync()"), std::string::npos);
  EXPECT_NE(pause_body.find("session_lifecycle_sync()"), std::string::npos);
  EXPECT_NE(report_writer.find("session_lifecycle_sync()"), std::string::npos);
  EXPECT_NE(report_reader.find("session_lifecycle_sync()"), std::string::npos);
  EXPECT_NE(display_reader.find("session_lifecycle_sync()"), std::string::npos);
  EXPECT_NE(mangohud_reader.find("session_lifecycle_sync()"), std::string::npos);
  const auto guarded_reader = [&](std::string_view begin, std::string_view end) {
    const auto body = function_source_between(source, begin, end);
    EXPECT_FALSE(body.empty());
    const auto sync = body.find("auto &sync = session_lifecycle_sync()");
    const auto lock = body.find("std::lock_guard<std::recursive_mutex> lifecycle_lock(sync.mutex)");
    ASSERT_NE(sync, std::string::npos);
    ASSERT_NE(lock, std::string::npos);
    EXPECT_LT(sync, lock);
  };
  guarded_reader("void proc_t::resume()", "void proc_t::pause()");
  guarded_reader("void proc_t::mark_client_session_report_recorded", "bool proc_t::client_session_report_recorded");
  guarded_reader("bool proc_t::client_session_report_recorded", "bool proc_t::session_display_mode_is_explicit");
  guarded_reader("bool proc_t::session_display_mode_is_explicit", "bool proc_t::current_app_has_mangohud");
  guarded_reader("bool proc_t::current_app_has_mangohud", "void proc_t::set_app_mangohud_configured");
  guarded_reader("void proc_t::set_app_mangohud_configured", "void proc_t::set_app_steam_launch_mode_configured");
  guarded_reader("void proc_t::set_app_steam_launch_mode_configured", "proc_t::session_lifecycle_sync_t &proc_t::session_lifecycle_sync");
  guarded_reader("std::string proc_t::get_last_run_app_name", "std::string proc_t::get_running_app_uuid");
  guarded_reader("std::string proc_t::get_running_app_uuid", "std::string proc_t::get_session_token");
  guarded_reader("std::string proc_t::get_session_token", "std::string proc_t::get_session_owner_unique_id");
  guarded_reader("std::string proc_t::get_session_owner_unique_id", "std::string proc_t::get_session_owner_device_name");
  guarded_reader("std::string proc_t::get_session_owner_device_name", "bool proc_t::is_session_owner");
  guarded_reader("bool proc_t::is_session_owner", "bool proc_t::session_uses_virtual_display");
  guarded_reader("bool proc_t::session_uses_virtual_display", "bool proc_t::session_allows_client_commands");
  guarded_reader("bool proc_t::session_allows_client_commands", "void proc_t::mark_client_session_report_recorded");
  EXPECT_NE(pause_body.find("std::unique_lock<std::recursive_mutex> lifecycle_lock(sync.mutex)"), std::string::npos);

  const auto status_snapshot = function_source_between(
    source,
    "session_status_view_t proc_t::get_session_status_view(",
    "session_stop_snapshot_t proc_t::get_session_stop_snapshot("
  );
  const auto snapshot_guard = function_source_between(
    source,
    "session_snapshot_guard_t::session_snapshot_guard_t(session_lifecycle_gate_t &gate)",
    "session_snapshot_guard_t::session_snapshot_guard_t(session_snapshot_guard_t &&other)"
  );
  const auto status_claim = status_snapshot.find("session_snapshot_guard_t::try_acquire");
  const auto status_latch = status_snapshot.find("const bool stop_observed = !guard.owns_snapshot()");
  const auto status_rtsp = status_snapshot.find("rtsp_stream::session_snapshot(unique_id)");
  const auto status_sync = status_snapshot.find("session_lifecycle_sync()");
  ASSERT_NE(status_claim, std::string::npos);
  ASSERT_NE(status_latch, std::string::npos);
  ASSERT_NE(status_rtsp, std::string::npos);
  ASSERT_NE(status_sync, std::string::npos);
  EXPECT_LT(status_claim, status_latch);
  EXPECT_LT(status_latch, status_rtsp);
  EXPECT_LT(status_rtsp, status_sync);
  EXPECT_NE(status_snapshot.find("rtsp_snapshot,\n      stop_observed"), std::string::npos);
  EXPECT_EQ(status_snapshot.find("stop_in_progress()"), std::string::npos);
  EXPECT_NE(snapshot_guard.find("begin_snapshot()"), std::string::npos);
  EXPECT_NE(snapshot_guard.find("finish_snapshot()"), std::string::npos);
  EXPECT_NE(header.find("session_snapshot_guard_t guard;"), std::string::npos);
  EXPECT_NE(status_snapshot.find("snapshot.viewer_count = rtsp_snapshot.viewer_count"), std::string::npos);
  const auto stop_snapshot_locked = function_source_between(
    source,
    "session_stop_snapshot_t proc_t::get_session_stop_snapshot_locked(",
    "session_status_view_t proc_t::get_session_status_view("
  );
  EXPECT_NE(stop_snapshot_locked.find("session_stop_token_matches("), std::string::npos);
  EXPECT_NE(stop_snapshot_locked.find("snapshot.stop_in_progress = stop_in_progress"), std::string::npos);
  EXPECT_EQ(stop_snapshot_locked.find("_session_lifecycle_gate->stop_in_progress()"), std::string::npos);
  const auto rtsp_snapshot_body = function_source_between(
    rtsp_source,
    "session_snapshot_t session_snapshot(const std::string_view& uuid)",
    "std::list<std::string>"
  );
  EXPECT_NE(rtsp_snapshot_body.find("accumulate_session_snapshot("), std::string::npos);
  EXPECT_NE(rtsp_snapshot_body.find("pending->is_pending_or_handoff()"), std::string::npos);
  EXPECT_NE(rtsp_snapshot_body.find("stream::session::state(*slot) == stream::session::state_e::STOPPING"), std::string::npos);
  EXPECT_EQ(rtsp_snapshot_body.find("break;"), std::string::npos);
  const auto rtsp_clear_body = function_source_between(
    rtsp_source,
    "void clear(bool all = true)",
    "/**\n     * @brief Removes the provided session"
  );
  const auto clear_lock = rtsp_clear_body.find("_session_slots.lock()");
  const auto clear_unlock = rtsp_clear_body.find("}\n#ifdef POLARIS_TESTS", clear_lock);
  const auto clear_join = rtsp_clear_body.find("stream::session::join", clear_unlock);
  ASSERT_NE(clear_lock, std::string::npos);
  ASSERT_NE(clear_unlock, std::string::npos);
  ASSERT_NE(clear_join, std::string::npos);
  EXPECT_LT(clear_lock, clear_unlock);
  EXPECT_LT(clear_unlock, clear_join);
  const auto rtsp_server_instance = rtsp_source.find("rtsp_server_t server {}");
  const auto public_rtsp_snapshot_start = rtsp_source.find(
    "session_snapshot_t session_snapshot(const std::string_view& uuid)",
    rtsp_server_instance
  );
  const auto public_rtsp_snapshot_end = rtsp_source.find("#ifdef POLARIS_TESTS", public_rtsp_snapshot_start);
  ASSERT_NE(rtsp_server_instance, std::string::npos);
  ASSERT_NE(public_rtsp_snapshot_start, std::string::npos);
  ASSERT_NE(public_rtsp_snapshot_end, std::string::npos);
  const auto public_rtsp_snapshot = rtsp_source.substr(
    public_rtsp_snapshot_start,
    public_rtsp_snapshot_end - public_rtsp_snapshot_start
  );
  EXPECT_EQ(public_rtsp_snapshot.find("server.clear(false)"), std::string::npos);
  EXPECT_NE(status_snapshot.find("stop_observed"), std::string::npos);

  const auto stop_snapshot_adapter = function_source_between(
    source,
    "session_stop_snapshot_t proc_t::get_session_stop_snapshot(",
    "session_stop_result_t proc_t::request_session_shutdown("
  );
  EXPECT_NE(stop_snapshot_adapter.find("get_session_status_view(unique_id, can_launch)"), std::string::npos);
  EXPECT_EQ(execute_impl.find("terminate();"), std::string::npos);

  const auto stop_start = source.find("proc_t::request_session_shutdown(");
  const auto stop_end = source.find("bool proc_t::session_shutdown_requested()", stop_start);
  ASSERT_NE(stop_start, std::string::npos);
  ASSERT_NE(stop_end, std::string::npos);
  const auto stop_impl = source.substr(stop_start, stop_end - stop_start);
  const auto claim = stop_impl.find("begin_stop()");
  const auto rtsp_snapshot = stop_impl.find("rtsp_stream::session_snapshot(unique_id)");
  const auto rtsp_terminate = stop_impl.find("rtsp_stream::terminate_sessions()");
  const auto process_terminate = stop_impl.find("terminate_impl(");
  const auto stop_commit = stop_impl.find("stop_committed = true");
  const auto release = stop_impl.rfind("finish_stop(stop_committed)");
  ASSERT_NE(claim, std::string::npos);
  ASSERT_NE(rtsp_snapshot, std::string::npos);
  ASSERT_NE(rtsp_terminate, std::string::npos);
  ASSERT_NE(process_terminate, std::string::npos);
  ASSERT_NE(stop_commit, std::string::npos);
  ASSERT_NE(release, std::string::npos);
  EXPECT_LT(claim, rtsp_snapshot);
  EXPECT_LT(rtsp_snapshot, rtsp_terminate);
  EXPECT_LT(rtsp_terminate, process_terminate);
  EXPECT_LT(process_terminate, stop_commit);
  EXPECT_LT(stop_commit, release);
  EXPECT_NE(stop_impl.find("bool stop_committed = false"), std::string::npos);

  const auto lifecycle_handoff = function_source_between(
    source,
    "bool session_lifecycle_gate_t::transition_launch_to_stop()",
    "void session_lifecycle_gate_t::begin_snapshot()"
  );
  EXPECT_NE(header.find("bool _launch_to_stop_handoff = false"), std::string::npos);
  EXPECT_NE(header.find("bool _last_stop_committed = false"), std::string::npos);
  EXPECT_NE(header.find("void finish_stop(bool committed = true)"), std::string::npos);
  EXPECT_NE(lifecycle_handoff.find("_launch_to_stop_handoff = true"), std::string::npos);
  EXPECT_NE(lifecycle_handoff.find("if (_last_stop_committed)"), std::string::npos);
  EXPECT_NE(lifecycle_handoff.find("_state = state_e::stopping"), std::string::npos);
  EXPECT_NE(lifecycle_handoff.find("return true"), std::string::npos);

  for (const auto &body : {
         function_source_between(source, "void session_lifecycle_gate_t::begin_launch()", "std::optional<std::uint64_t> session_lifecycle_gate_t::capture_launch_generation"),
         function_source_between(source, "std::optional<std::uint64_t> session_lifecycle_gate_t::capture_launch_generation", "bool session_lifecycle_gate_t::try_begin_rtsp_launch()"),
         function_source_between(source, "bool session_lifecycle_gate_t::try_begin_rtsp_launch()", "bool session_lifecycle_gate_t::try_begin_rtsp_launch(std::uint64_t"),
         function_source_between(source, "void session_lifecycle_gate_t::begin_snapshot()", "void session_lifecycle_gate_t::finish_snapshot()")
       }) {
    EXPECT_NE(body.find("_launch_to_stop_handoff"), std::string::npos);
  }
  EXPECT_NE(stop_impl.find("[this, &stop_committed]"), std::string::npos);

  const auto terminate_start = source.find("void proc_t::terminate_impl(");
  const auto terminate_end = source.find("bool proc_t::reload_configuration_from_file", terminate_start);
  ASSERT_NE(terminate_start, std::string::npos);
  ASSERT_NE(terminate_end, std::string::npos);
  const auto terminate_impl = source.substr(terminate_start, terminate_end - terminate_start);
  EXPECT_EQ(terminate_impl.find("finish_stop()"), std::string::npos);
  EXPECT_EQ(terminate_impl.find("_session_shutdown_requested"), std::string::npos);

  const auto launch_start = nvhttp.find("void launch(bool &host_audio");
  const auto resume_start = nvhttp.find("void resume(bool &host_audio", launch_start);
  const auto resume_end = nvhttp.find("void cancel(", resume_start);
  ASSERT_NE(launch_start, std::string::npos);
  ASSERT_NE(resume_start, std::string::npos);
  ASSERT_NE(resume_end, std::string::npos);
  const auto launch_handler = nvhttp.substr(launch_start, resume_start - launch_start);
  const auto resume_handler = nvhttp.substr(resume_start, resume_end - resume_start);

  const auto launch_generation = launch_handler.find("capture_session_launch_generation()");
  const auto launch_admission = launch_handler.find("try_begin_session_launch(*launch_generation)");
  const auto launch_running = launch_handler.find("proc::proc.running()");
  ASSERT_NE(launch_generation, std::string::npos);
  ASSERT_NE(launch_admission, std::string::npos);
  ASSERT_NE(launch_running, std::string::npos);
  EXPECT_LT(launch_generation, launch_admission);
  EXPECT_LT(launch_admission, launch_running);
  const auto launch_rejection = launch_handler.substr(launch_admission, launch_running - launch_admission);
  EXPECT_NE(launch_rejection.find("status_code\", 409"), std::string::npos);
  EXPECT_NE(launch_rejection.find("return;"), std::string::npos);

  const auto resume_generation = resume_handler.find("capture_session_launch_generation()");
  const auto resume_admission = resume_handler.find("try_begin_session_launch(*launch_generation)");
  const auto resume_running = resume_handler.find("proc::proc.running()");
  ASSERT_NE(resume_generation, std::string::npos);
  ASSERT_NE(resume_admission, std::string::npos);
  ASSERT_NE(resume_running, std::string::npos);
  EXPECT_LT(resume_generation, resume_admission);
  EXPECT_LT(resume_admission, resume_running);
  const auto resume_rejection = resume_handler.substr(resume_admission, resume_running - resume_admission);
  EXPECT_NE(resume_rejection.find("status_code\", 409"), std::string::npos);
  EXPECT_NE(resume_rejection.find("return;"), std::string::npos);

  const auto count_occurrences = [](const std::string &text, std::string_view needle) {
    std::size_t count = 0;
    for (auto pos = text.find(needle); pos != std::string::npos; pos = text.find(needle, pos + needle.size())) {
      ++count;
    }
    return count;
  };
  EXPECT_EQ(count_occurrences(launch_handler, "finish_session_launch()"), 1);
  EXPECT_EQ(count_occurrences(resume_handler, "finish_session_launch()"), 1);
  EXPECT_NE(launch_handler.find("terminate_from_admitted_launch()"), std::string::npos);

  EXPECT_EQ(launch_handler.find("rtsp_stream::launch_session_raise("), std::string::npos);
  EXPECT_NE(
    launch_handler.find("launch_input_only_and_raise(launch_session)"),
    std::string::npos
  );
  EXPECT_NE(
    launch_handler.find("execute_and_raise(*app_iter, launch_session)"),
    std::string::npos
  );
  EXPECT_NE(
    launch_handler.find("raise_session_for_admitted_launch(launch_session)"),
    std::string::npos
  );
  const auto launch_gate = launch_handler.find("raise_session_for_admitted_launch(");
  const auto launch_raise_guard = launch_handler.find("if (!launch_session_raised && !proc::proc.raise_session_for_admitted_launch(launch_session))");
  const auto launch_success = launch_handler.rfind("tree.put(\"root.<xmlattr>.status_code\", 200)");
  ASSERT_NE(launch_raise_guard, std::string::npos);
  EXPECT_NE(launch_handler.substr(launch_raise_guard, launch_success - launch_raise_guard).find("status_code\", 409"), std::string::npos);
  EXPECT_LT(launch_gate, launch_success);

  EXPECT_EQ(resume_handler.find("rtsp_stream::launch_session_raise("), std::string::npos);
  EXPECT_NE(
    resume_handler.find("raise_session_for_admitted_launch(launch_session)"),
    std::string::npos
  );
  const auto resume_gate = resume_handler.find("raise_session_for_admitted_launch(");
  const auto resume_raise_guard = resume_handler.find("if (!proc::proc.raise_session_for_admitted_launch(launch_session))");
  const auto resume_success = resume_handler.find("tree.put(\"root.<xmlattr>.status_code\", 200)");
  ASSERT_NE(resume_raise_guard, std::string::npos);
  EXPECT_NE(resume_handler.substr(resume_raise_guard, resume_success - resume_raise_guard).find("status_code\", 409"), std::string::npos);
  EXPECT_LT(resume_gate, resume_success);

  const auto rtsp = read_source_file_for_contract("src/rtsp.cpp");
  ASSERT_FALSE(rtsp.empty());
  EXPECT_NE(rtsp.find("snapshot.pending_sessions = 1"), std::string::npos);
  EXPECT_NE(rtsp.find("server.cancel_pending_session()"), std::string::npos);
  const auto handoff_start = rtsp.find("insert_start_result_e insert_and_start_if_not_cancelled(");
  const auto handoff_end = rtsp.find("void iterate()", handoff_start);
  ASSERT_NE(handoff_start, std::string::npos);
  ASSERT_NE(handoff_end, std::string::npos);
  const auto handoff = rtsp.substr(handoff_start, handoff_end - handoff_start);
  const auto handoff_claim = handoff.find("try_begin_setup_handoff()");
  const auto handoff_lock = handoff.find("_session_slots.lock()");
  const auto handoff_insert = handoff.find("_session_slots->emplace(session)");
  const auto handoff_commit = handoff.find("commit_setup_start()");
  const auto handoff_start_stream = handoff.find("stream::session::start(*session, remote_address)");
  ASSERT_NE(handoff_claim, std::string::npos);
  ASSERT_NE(handoff_lock, std::string::npos);
  ASSERT_NE(handoff_insert, std::string::npos);
  ASSERT_NE(handoff_commit, std::string::npos);
  ASSERT_NE(handoff_start_stream, std::string::npos);
  EXPECT_LT(handoff_claim, handoff_lock);
  EXPECT_LT(handoff_lock, handoff_insert);
  EXPECT_LT(handoff_insert, handoff_commit);
  EXPECT_LT(handoff_commit, handoff_start_stream);
  EXPECT_EQ(handoff.find("_launch_timer_mutex"), std::string::npos);
  EXPECT_NE(rtsp.find("insert_and_start_if_not_cancelled(stream_session, session"), std::string::npos);
  EXPECT_NE(rtsp.find("pending->cancel_for_timeout()"), std::string::npos);
  EXPECT_NE(rtsp_header.find("expected == setup_state_e::pending || expected == setup_state_e::handoff"), std::string::npos);
  EXPECT_NE(rtsp.find("_raised_timer_generation"), std::string::npos);
  EXPECT_NE(rtsp.find("std::mutex _launch_timer_mutex"), std::string::npos);
  EXPECT_NE(rtsp.find("std::lock_guard timer_lock(_launch_timer_mutex)"), std::string::npos);
  EXPECT_NE(rtsp.find("launch_event.pop_if("), std::string::npos);
  const auto cancel_pending_start = rtsp.find("void cancel_pending_session()");
  const auto cancel_pending_end = rtsp.find("/**", cancel_pending_start);
  ASSERT_NE(cancel_pending_start, std::string::npos);
  ASSERT_NE(cancel_pending_end, std::string::npos);
  const auto cancel_pending = rtsp.substr(cancel_pending_start, cancel_pending_end - cancel_pending_start);
  EXPECT_NE(cancel_pending.find("std::lock_guard timer_lock(_launch_timer_mutex)"), std::string::npos);
  EXPECT_NE(cancel_pending.find("raised_timer.cancel()"), std::string::npos);
  EXPECT_NE(cancel_pending.find("++_raised_timer_generation"), std::string::npos);
  const auto timer_callback_start = rtsp.find("raised_timer.async_wait(");
  const auto timer_callback_end = rtsp.find("return true;", timer_callback_start);
  ASSERT_NE(timer_callback_start, std::string::npos);
  ASSERT_NE(timer_callback_end, std::string::npos);
  const auto timer_callback = rtsp.substr(timer_callback_start, timer_callback_end - timer_callback_start);
  EXPECT_NE(timer_callback.find("expire_pending_launch(launch_session_id, timer_generation)"), std::string::npos);
  const auto timer_expiry_start = rtsp.find("bool expire_pending_launch(");
  const auto timer_expiry_end = rtsp.find("std::uint64_t launch_timer_generation()", timer_expiry_start);
  ASSERT_NE(timer_expiry_start, std::string::npos);
  ASSERT_NE(timer_expiry_end, std::string::npos);
  const auto timer_expiry = rtsp.substr(timer_expiry_start, timer_expiry_end - timer_expiry_start);
  EXPECT_NE(timer_expiry.find("std::lock_guard timer_lock(_launch_timer_mutex)"), std::string::npos);
  EXPECT_NE(timer_expiry.find("_raised_timer_generation.load() != timer_generation"), std::string::npos);
  EXPECT_NE(timer_expiry.find("launch_event.pop_if("), std::string::npos);
  EXPECT_NE(source.find("rtsp_snapshot.active_sessions + rtsp_snapshot.pending_sessions"), std::string::npos);
}

TEST(ProcessRuntimeConfigTests, RefreshPreservesLifecycleSynchronizationObjects) {
  const auto source = read_source_file_for_contract("src/process.cpp");
  const auto refresh_start = source.find("void refresh(const std::string &file_name, bool needs_terminate)");
  const auto refresh_end = source.find("}  // namespace proc", refresh_start);
  ASSERT_NE(refresh_start, std::string::npos);
  ASSERT_NE(refresh_end, std::string::npos);
  const auto refresh_body = source.substr(refresh_start, refresh_end - refresh_start);
  EXPECT_NE(refresh_body.find("proc.reload_configuration_from_file(file_name)"), std::string::npos);
  EXPECT_EQ(refresh_body.find("proc = std::move(*proc_opt)"), std::string::npos);

  const auto terminate_start = source.find("void proc_t::terminate_impl(bool immediate, bool needs_refresh)");
  const auto terminate_end = source.find("bool proc_t::reload_configuration_from_file", terminate_start);
  ASSERT_NE(terminate_start, std::string::npos);
  ASSERT_NE(terminate_end, std::string::npos);
  const auto terminate_body = source.substr(terminate_start, terminate_end - terminate_start);
  EXPECT_NE(terminate_body.find("reload_configuration_from_file(config::stream.file_apps)"), std::string::npos);
  EXPECT_EQ(terminate_body.find("refresh(config::stream.file_apps, false)"), std::string::npos);

  const auto reload_start = source.find("void proc_t::reload_configuration(proc_t &&parsed)");
  const auto reload_end = source.find("#if defined(POLARIS_TESTS)", reload_start);
  ASSERT_NE(reload_start, std::string::npos);
  ASSERT_NE(reload_end, std::string::npos);
  const auto reload_body = source.substr(reload_start, reload_end - reload_start);
  EXPECT_NE(reload_body.find("std::lock_guard<std::recursive_mutex> lifecycle_lock(sync.mutex)"), std::string::npos);
  EXPECT_NE(reload_body.find("_env = std::move(parsed._env)"), std::string::npos);
  EXPECT_NE(reload_body.find("_apps = std::move(parsed._apps)"), std::string::npos);
  EXPECT_EQ(reload_body.find("_session_lifecycle_gate"), std::string::npos);
  EXPECT_EQ(reload_body.find("_session_lifecycle_sync"), std::string::npos);

  const auto get_apps_start = source.find("std::vector<ctx_t> proc_t::get_apps() const");
  const auto get_apps_end = source.find("std::string proc_t::get_app_image", get_apps_start);
  ASSERT_NE(get_apps_start, std::string::npos);
  ASSERT_NE(get_apps_end, std::string::npos);
  const auto get_apps_body = source.substr(get_apps_start, get_apps_end - get_apps_start);
  EXPECT_NE(get_apps_body.find("std::lock_guard<std::recursive_mutex> lifecycle_lock(sync.mutex)"), std::string::npos);
  EXPECT_NE(get_apps_body.find("return _apps"), std::string::npos);
}

TEST(ProcessRuntimeConfigTests, DeviceDbBitrateStaysOutWhenAutoQualityOffAndMaxBitrateUnlocked) {
  const auto resolved = proc::resolve_device_db_launch_bitrate_for_tests(
    0,
    std::optional<int> {},
    false,
    "Steam Deck OLED",
    "Steam Big Picture"
  );

  EXPECT_FALSE(resolved.has_value());
}

TEST(ProcessRuntimeConfigTests, DeviceDbBitrateCanSeedAutoQualityWhenEnabled) {
  const auto resolved = proc::resolve_device_db_launch_bitrate_for_tests(
    0,
    std::optional<int> {},
    true,
    "Steam Deck OLED",
    "Steam Big Picture"
  );

  ASSERT_TRUE(resolved.has_value());
  EXPECT_EQ(*resolved, 25000);
}

TEST(ProcessRuntimeConfigTests, PairedClientBitrateWinsEvenWhenAutoQualityOff) {
  const auto resolved = proc::resolve_device_db_launch_bitrate_for_tests(
    0,
    std::optional<int> {45000},
    false,
    "Steam Deck OLED",
    "Steam Big Picture"
  );

  ASSERT_TRUE(resolved.has_value());
  EXPECT_EQ(*resolved, 45000);
}

TEST(ProcessRuntimeConfigTests, ManualMaxBitrateLocksOutDeviceDbProfile) {
  const auto resolved = proc::resolve_device_db_launch_bitrate_for_tests(
    50000,
    std::optional<int> {},
    true,
    "Steam Deck OLED",
    "Steam Big Picture"
  );

  EXPECT_FALSE(resolved.has_value());
}

TEST(ProcessRuntimeConfigTests, MissionControlPolicyDoesNotUseDeviceDbBitrateWhenAutoQualityOffAndClientBitrateUnknown) {
  auto_quality_guard_t guard;
  ai_optimizer::set_enabled(false);
  adaptive_bitrate::set_enabled(false);

  crypto::named_cert_t client {};
  client.name = "Steam Deck OLED";
  client.uuid = "issue-147-client";

  stream_stats::stats_t stats {};
  stats.width = 1280;
  stats.height = 800;
  stats.requested_client_fps = 90.0;
  stats.codec = "hevc";

  const auto policy = nvhttp::build_stream_policy_json_for_tests(
    client,
    stats,
    nlohmann::json::object()
  );

  EXPECT_EQ(policy.value("target_bitrate_kbps", -1), 0);
  EXPECT_EQ(policy.value("target_bitrate_source", std::string {}), "client_request");
}


TEST(ProcessRuntimeConfigTests, StreamPolicySurfacesHdrDowngradeEffectiveMode) {
  crypto::named_cert_t client {};
  client.name = "Nova Client";

  stream_stats::stats_t stats {};
  stats.dynamic_range = 1;
  stats.display_hdr = false;
  stats.hdr_metadata_available = false;
  stats.stream_hdr_enabled = false;
  stats.color_coding = "SDR (Rec. 709)";

  const auto policy = nvhttp::build_stream_policy_json_for_tests(
    client,
    stats,
    nlohmann::json::object()
  );

  EXPECT_TRUE(policy.value("hdr_requested", false));
  EXPECT_FALSE(policy.value("hdr_active", true));
  EXPECT_EQ(policy.value("hdr_effective_mode", std::string {}), "sdr_10bit");
  EXPECT_EQ(policy.value("hdr_downgrade_reason", std::string {}), "display_not_hdr");
  EXPECT_NE(policy.dump().find("10-bit SDR, not HDR"), std::string::npos);
  EXPECT_NE(policy.dump().find("hdr_downgraded"), std::string::npos);
}

TEST(ProcessRuntimeConfigTests, SessionHealthFlagsHdrSourceMissingAsActionableIssue) {
  stream_stats::stats_t stats {};
  stats.dynamic_range = 1;
  stats.display_hdr = false;
  stats.hdr_metadata_available = false;
  stats.stream_hdr_enabled = false;
  stats.color_coding = "SDR (Rec. 709)";

  const auto health = nvhttp::build_session_health_json_for_tests(
    stats,
    false,
    "Nova Client",
    "Steam Big Picture"
  );

  EXPECT_EQ(health.value("primary_issue", std::string {}), "hdr_downgraded");
  EXPECT_EQ(health.value("grade", std::string {}), "watch");
  EXPECT_EQ(health.value("limiting_factor", std::string {}), "hdr");
  EXPECT_EQ(health.value("hdr_effective_mode", std::string {}), "sdr_10bit");
  EXPECT_EQ(health.value("hdr_downgrade_reason", std::string {}), "display_not_hdr");
  EXPECT_NE(health.dump().find("10-bit SDR, not HDR"), std::string::npos);
  EXPECT_NE(health.dump().find("hdr_downgraded"), std::string::npos);
}

TEST(ProcessRuntimeConfigTests, SessionHealthFlagsHeadlessHdrUnavailableSeparately) {
  stream_stats::stats_t stats {};
  stats.dynamic_range = 1;
  stats.runtime_effective_headless = true;
  stats.display_hdr = false;
  stats.hdr_metadata_available = false;
  stats.stream_hdr_enabled = false;
  stats.color_coding = "SDR (Rec. 709)";

  const auto health = nvhttp::build_session_health_json_for_tests(
    stats,
    false,
    "Nova Client",
    "Steam Big Picture"
  );

  EXPECT_EQ(health.value("primary_issue", std::string {}), "hdr_downgraded");
  EXPECT_EQ(health.value("hdr_effective_mode", std::string {}), "sdr_10bit");
  EXPECT_EQ(health.value("hdr_downgrade_reason", std::string {}), "headless_hdr_unavailable");
  EXPECT_NE(health.dump().find("Private Stream"), std::string::npos);
  EXPECT_NE(health.dump().find("physical or virtual HDR-capable display path"), std::string::npos);
}

#ifdef __linux__
TEST(ProcessRuntimeConfigTests, MissionControlPolicyIncludesLinuxGpuProfileForVaapiCaptureTruth) {
  linux_cage_compositor_guard_t linux_guard;
  config::video.adapter_name = "/dev/dri/renderD128";
  config::video.linux_display.use_cage_compositor = true;
  config::video.linux_display.prefer_gpu_native_capture = true;

  crypto::named_cert_t client {};
  client.name = "Steam Deck OLED";
  client.uuid = "amd-vaapi-client";

  stream_stats::stats_t stats {};
  stats.runtime_effective_headless = true;
  stats.capture_transport = platf::frame_transport_e::shm;
  stats.capture_residency = platf::frame_residency_e::cpu;
  stats.capture_format = platf::frame_format_e::bgra8;
  stats.capture_device = "/dev/dri/renderD128";
  stats.encode_target_device = "vaapi";
  stats.encode_target_residency = platf::frame_residency_e::gpu;
  stats.encode_target_format = platf::frame_format_e::nv12;

  const auto policy = nvhttp::build_stream_policy_json_for_tests(
    client,
    stats,
    nlohmann::json::object()
  );

  ASSERT_TRUE(policy.contains("linux_gpu_profile"));
  const auto &profile = policy.at("linux_gpu_profile");
  EXPECT_EQ(profile.at("encoder_api"), "vaapi");
  EXPECT_EQ(profile.at("encoder_adapter"), "/dev/dri/renderD128");
  EXPECT_EQ(profile.at("capture_device"), "/dev/dri/renderD128");
  EXPECT_TRUE(profile.at("adapter_matches_capture_device"));
  EXPECT_TRUE(profile.at("gpu_native_requested"));
  EXPECT_FALSE(profile.at("gpu_native_succeeded"));
}
#endif

TEST(ProcessRuntimeConfigTests, InitialTerminateDoesNotResetAdaptiveBitrateMax) {
#ifdef __linux__
  linux_cage_compositor_guard_t linux_guard;
  config::video.linux_display.auto_manage_displays = false;
  config::video.linux_display.use_cage_compositor = false;
#endif
  const auto original_max = config::video.adaptive_bitrate.max_bitrate_kbps;
  config::video.adaptive_bitrate.max_bitrate_kbps = 100000;

  proc::proc_t process {boost::process::v1::environment {}, {}};
  process.terminate(false, false);

  EXPECT_EQ(100000, config::video.adaptive_bitrate.max_bitrate_kbps);
  config::video.adaptive_bitrate.max_bitrate_kbps = original_max;
}

#ifdef __linux__
TEST(ProcessRuntimeConfigTests, DirectHeadlessCageSuppressesInheritedMangoHud) {
  proc::ctx_t app {};
  app.name = "Portal";
  app.source = "steam";
  app.steam_appid = "400";
  app.detached = {"setsid steam steam://rungameid/400"};

  EXPECT_FALSE(proc::cage_mangohud_allowed_for_session_for_tests(app, true, true));

  app.env_vars["MANGOHUD"] = "1";
  EXPECT_TRUE(proc::cage_mangohud_allowed_for_session_for_tests(app, true, true));
}

TEST(ProcessRuntimeConfigTests, SteamBigPictureNeverAllowsCageMangoHud) {
  proc::ctx_t app {};
  app.name = "Steam Big Picture";
  app.detached = {"setsid steam -gamepadui"};
  app.env_vars["MANGOHUD"] = "1";

  EXPECT_FALSE(proc::cage_mangohud_allowed_for_session_for_tests(app, true, true));
}

TEST(ProcessRuntimeConfigTests, SteamBigPictureInputGuardIsScopedToPrivateCompatibilitySessions) {
  proc::ctx_t big_picture {};
  big_picture.name = "Steam Big Picture";
  big_picture.detached = {"setsid steam -gamepadui"};

  EXPECT_TRUE(proc::steam_big_picture_input_guard_enabled_for_tests(big_picture, true, false));
  EXPECT_FALSE(proc::steam_big_picture_input_guard_enabled_for_tests(big_picture, false, false));
  EXPECT_FALSE(proc::steam_big_picture_input_guard_enabled_for_tests(big_picture, true, true));

  proc::ctx_t compatibility_game {};
  compatibility_game.name = "MOUSE";
  compatibility_game.source = "steam";
  compatibility_game.steam_appid = "2416450";
  compatibility_game.steam_launch_mode = "big-picture";
  compatibility_game.detached = {
    "setsid steam -gamepadui",
    "setsid steam steam://rungameid/2416450",
  };
  EXPECT_TRUE(proc::steam_big_picture_input_guard_enabled_for_tests(compatibility_game, true, false));

  compatibility_game.steam_launch_mode = "direct";
  compatibility_game.detached = {"setsid steam steam://rungameid/2416450"};
  EXPECT_FALSE(proc::steam_big_picture_input_guard_enabled_for_tests(compatibility_game, true, false));
}

TEST(ProcessRuntimeConfigTests, SteamBigPictureInputGuardUsesAppHomeForLogPath) {
  proc::ctx_t app {};
  app.env_vars["HOME"] = "/tmp/polaris-app-home";
  boost::process::v1::environment host_env;
  host_env["HOME"] = "/tmp/polaris-host-home";

  EXPECT_EQ(
    proc::steam_big_picture_log_path_for_tests(app, host_env),
    "/tmp/polaris-app-home/.local/share/Steam/logs/gameprocess_log.txt"
  );
}

TEST(ProcessRuntimeConfigTests, SteamBigPictureInputGuardSnapshotsIdentityAndEofFromOneDescriptor) {
  EXPECT_TRUE(proc::steam_big_picture_atomic_snapshot_survives_path_replacement_for_tests());
}

TEST(ProcessRuntimeConfigTests, SteamBigPictureInputGuardBindsWatcherStreamToPinnedDescriptor) {
  EXPECT_TRUE(proc::steam_big_picture_pinned_stream_survives_path_replacement_for_tests());
}

TEST(ProcessRuntimeConfigTests, SteamBigPictureInputGuardParsesOnlyPositiveGameLifecycleRecords) {
  const auto started = proc::parse_steam_game_process_event_for_tests(
    "[2026-07-14 16:23:37] AppID 2416450 adding PID 938289 as a tracked process \"SteamLaunch AppId=2416450 -- steam-launch-wrapper --\""
  );
  EXPECT_EQ(started.kind, proc::steam_game_process_event_kind_e::started);
  EXPECT_EQ(started.appid, "2416450");

  const auto stopped = proc::parse_steam_game_process_event_for_tests(
    "[2026-07-14 16:27:10] Remove 2416450 from running list"
  );
  EXPECT_EQ(stopped.kind, proc::steam_game_process_event_kind_e::stopped);
  EXPECT_EQ(stopped.appid, "2416450");

  const auto helper = proc::parse_steam_game_process_event_for_tests(
    "[2026-07-14 16:23:38] AppID 250820 adding PID 938300 as a tracked process"
  );
  EXPECT_EQ(helper.kind, proc::steam_game_process_event_kind_e::none);

  const auto missing_wrapper = proc::parse_steam_game_process_event_for_tests(
    "[2026-07-14 16:23:37] AppID 2416450 adding PID 938289 as a tracked process \"SteamLaunch AppId=2416450 -- MOUSE.exe\""
  );
  EXPECT_EQ(missing_wrapper.kind, proc::steam_game_process_event_kind_e::none);

  const auto mismatched = proc::parse_steam_game_process_event_for_tests(
    "[2026-07-14 16:23:37] AppID 2416450 adding PID 938289 as a tracked process \"SteamLaunch AppId=999 -- steam-launch-wrapper --\""
  );
  EXPECT_EQ(mismatched.kind, proc::steam_game_process_event_kind_e::none);

  const auto embedded_remove = proc::parse_steam_game_process_event_for_tests(
    "[2026-07-14 16:23:37] AppID 999 adding PID 123 as a tracked process \"--title Remove 2416450 from running list\""
  );
  EXPECT_EQ(embedded_remove.kind, proc::steam_game_process_event_kind_e::none);
}

TEST(ProcessRuntimeConfigTests, SteamBigPictureInputGuardClosesOnceAndReopensAfterLastGame) {
  std::unordered_set<std::string> active_appids;
  const std::string start_mouse =
    "AppID 2416450 adding PID 938289 as a tracked process \"SteamLaunch AppId=2416450 -- steam-launch-wrapper --\"";
  const std::string start_second =
    "AppID 883710 adding PID 938400 as a tracked process \"SteamLaunch AppId=883710 -- steam-launch-wrapper --\"";

  auto transition = proc::apply_steam_big_picture_guard_event_for_tests(active_appids, start_mouse);
  EXPECT_TRUE(transition.close_big_picture);
  EXPECT_FALSE(transition.open_big_picture);
  EXPECT_EQ(transition.active_games, 1u);

  transition = proc::apply_steam_big_picture_guard_event_for_tests(active_appids, start_mouse);
  EXPECT_FALSE(transition.close_big_picture);
  EXPECT_FALSE(transition.open_big_picture);
  EXPECT_EQ(transition.active_games, 1u);

  transition = proc::apply_steam_big_picture_guard_event_for_tests(active_appids, start_second);
  EXPECT_FALSE(transition.close_big_picture);
  EXPECT_FALSE(transition.open_big_picture);
  EXPECT_EQ(transition.active_games, 2u);

  transition = proc::apply_steam_big_picture_guard_event_for_tests(active_appids, "Remove 2416450 from running list");
  EXPECT_FALSE(transition.close_big_picture);
  EXPECT_FALSE(transition.open_big_picture);
  EXPECT_EQ(transition.active_games, 1u);

  transition = proc::apply_steam_big_picture_guard_event_for_tests(active_appids, "Remove 883710 from running list");
  EXPECT_FALSE(transition.close_big_picture);
  EXPECT_TRUE(transition.open_big_picture);
  EXPECT_EQ(transition.active_games, 0u);

  transition = proc::apply_steam_big_picture_guard_event_for_tests(active_appids, "Remove 999 from running list");
  EXPECT_FALSE(transition.close_big_picture);
  EXPECT_FALSE(transition.open_big_picture);
}

TEST(ProcessRuntimeConfigTests, SteamBigPictureInputGuardTailsOnlyNewLifecycleRecords) {
  const auto actions = proc::run_steam_big_picture_guard_file_scenario_for_tests(
    proc::steam_big_picture_guard_file_scenario_e::appended_lifecycle
  );
  EXPECT_EQ(actions, (std::vector<std::string> {"close/bigpicture", "open/bigpicture"}));
}

TEST(ProcessRuntimeConfigTests, SteamBigPictureInputGuardRetriesFailedCloseDispatch) {
  const auto actions = proc::run_steam_big_picture_guard_file_scenario_for_tests(
    proc::steam_big_picture_guard_file_scenario_e::close_dispatch_retry
  );
  EXPECT_EQ(
    actions,
    (std::vector<std::string> {"close/bigpicture", "close/bigpicture"})
  );
}

TEST(ProcessRuntimeConfigTests, SteamBigPictureInputGuardRetriesFailedOpenDispatch) {
  const auto actions = proc::run_steam_big_picture_guard_file_scenario_for_tests(
    proc::steam_big_picture_guard_file_scenario_e::open_dispatch_retry
  );
  EXPECT_EQ(
    actions,
    (std::vector<std::string> {"close/bigpicture", "open/bigpicture", "open/bigpicture"})
  );
}

TEST(ProcessRuntimeConfigTests, SteamBigPictureInputGuardDoesNotReopenBeforeIsolatedSteamCleanup) {
  const auto actions = proc::run_steam_big_picture_guard_file_scenario_for_tests(
    proc::steam_big_picture_guard_file_scenario_e::teardown_while_closed
  );
  EXPECT_EQ(actions, (std::vector<std::string> {"close/bigpicture"}));
}

TEST(ProcessRuntimeConfigTests, SteamBigPictureInputGuardRestoresAfterLogTruncation) {
  const auto actions = proc::run_steam_big_picture_guard_file_scenario_for_tests(
    proc::steam_big_picture_guard_file_scenario_e::truncation_while_closed
  );
  EXPECT_EQ(actions, (std::vector<std::string> {"close/bigpicture", "open/bigpicture"}));
}

TEST(ProcessRuntimeConfigTests, SteamBigPictureInputGuardDisablesAfterLogReplacementWithoutReplayingStaleRecords) {
  const auto actions = proc::run_steam_big_picture_guard_file_scenario_for_tests(
    proc::steam_big_picture_guard_file_scenario_e::replacement_with_stale_records_while_closed
  );
  EXPECT_EQ(actions, (std::vector<std::string> {"close/bigpicture", "open/bigpicture"}));
}

TEST(ProcessRuntimeConfigTests, SteamBigPictureInputGuardDisablesIfLogChangesBeforeWatcherStarts) {
  const auto actions = proc::run_steam_big_picture_guard_file_scenario_for_tests(
    proc::steam_big_picture_guard_file_scenario_e::replacement_before_watcher_start
  );
  EXPECT_TRUE(actions.empty());
}

TEST(ProcessRuntimeConfigTests, PreCageSteamTerminationRequiresImmutableCageAndExactSessionOwnership) {
  proc::ctx_t steam_app {};
  steam_app.source = "steam";
  steam_app.steam_appid = "2416450";
  steam_app.detached = {"setsid steam steam://rungameid/2416450"};

  EXPECT_TRUE(proc::should_terminate_session_owned_steam_before_cage_stop_for_tests(
    steam_app, true, true, true, false, true
  ));
  // Unowned desktop Steam must not block terminating session-owned Steam.
  EXPECT_TRUE(proc::should_terminate_session_owned_steam_before_cage_stop_for_tests(
    steam_app, true, true, true, true, true
  ));
  EXPECT_FALSE(proc::should_terminate_session_owned_steam_before_cage_stop_for_tests(
    steam_app, true, true, true, false, false
  ));
  EXPECT_FALSE(proc::should_terminate_session_owned_steam_before_cage_stop_for_tests(
    steam_app, false, true, true, false, true
  ));
  EXPECT_FALSE(proc::should_terminate_session_owned_steam_before_cage_stop_for_tests(
    steam_app, true, false, true, false, true
  ));
  EXPECT_FALSE(proc::should_terminate_session_owned_steam_before_cage_stop_for_tests(
    steam_app, true, true, false, false, true
  ));

  proc::ctx_t non_steam_app {};
  non_steam_app.name = "Native Game";
  non_steam_app.cmd = "/usr/bin/native-game";
  EXPECT_FALSE(proc::should_terminate_session_owned_steam_before_cage_stop_for_tests(
    non_steam_app, true, true, true, false, true
  ));
}

TEST(ProcessRuntimeConfigTests, IsolatedSessionOwnershipRequiresExactNulDelimitedGeneration) {
  const std::string token = "private-generation-42";
  const std::string exact = std::string("HOME=/tmp") + char(0) +
                            "POLARIS_SESSION_INSTANCE_ID=" + token + char(0) +
                            "DISPLAY=:9" + char(0);
  EXPECT_TRUE(proc::isolated_session_environ_matches_for_tests(exact, token));
  EXPECT_FALSE(proc::isolated_session_environ_matches_for_tests(exact, "other-generation"));
  EXPECT_FALSE(proc::isolated_session_environ_matches_for_tests(
    "POLARIS_SESSION_INSTANCE_ID=" + token + "-suffix" + char(0), token
  ));
  EXPECT_FALSE(proc::isolated_session_environ_matches_for_tests(
    "NOT_POLARIS_SESSION_INSTANCE_ID=" + token + char(0), token
  ));
  EXPECT_FALSE(proc::isolated_session_environ_matches_for_tests(
    "PAYLOAD=POLARIS_SESSION_INSTANCE_ID=" + token + char(0), token
  ));
  EXPECT_FALSE(proc::isolated_session_environ_matches_for_tests(
    "POLARIS_SESSION_INSTANCE_ID=" + token, token
  ));
  EXPECT_FALSE(proc::isolated_session_environ_matches_for_tests("", token));
  EXPECT_FALSE(proc::isolated_session_environ_matches_for_tests(exact, ""));
  EXPECT_FALSE(proc::isolated_session_environ_matches_for_tests(
    std::string("POLARIS_SESSION_INSTANCE_ID=") + char(0), ""
  ));
  EXPECT_TRUE(proc::session_instance_environment_is_child_only_for_tests(token));
}

TEST(ProcessRuntimeConfigTests, ProcStartTimeParserHandlesParenthesizedNames) {
  std::ostringstream stat;
  stat << "123 (steam helper (private)) S";
  for (int field = 4; field <= 21; ++field) {
    stat << " 0";
  }
  stat << " 4242";
  EXPECT_EQ(proc::proc_start_time_from_stat_for_tests(stat.str()), std::optional<std::uint64_t> {4242});
  EXPECT_EQ(proc::proc_start_time_from_stat_for_tests("malformed"), std::nullopt);
}

TEST(ProcessRuntimeConfigTests, SteamPidfdCaptureRejectsStartTimeMismatch) {
  EXPECT_TRUE(proc::steam_pidfd_capture_identity_matches_for_tests(4242, 4242));
  EXPECT_FALSE(proc::steam_pidfd_capture_identity_matches_for_tests(4242, 4243));
  EXPECT_FALSE(proc::steam_pidfd_capture_identity_matches_for_tests(std::nullopt, 4242));
  EXPECT_FALSE(proc::steam_pidfd_capture_identity_matches_for_tests(4242, std::nullopt));
}

TEST(ProcessRuntimeConfigTests, GamescopeAttachedClientPidReuseFailsClosedBeforePidfdSignal) {
  int ready_pipe[2] {-1, -1};
  ASSERT_EQ(pipe(ready_pipe), 0);
  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    if (setsid() < 0) _exit(124);
    close(ready_pipe[0]);
    if (dup2(ready_pipe[1], 3) < 0) {
      _exit(126);
    }
    close(ready_pipe[1]);
    execl(
      "/usr/bin/env",
      "env",
      "GAMESCOPE_WAYLAND_DISPLAY=gamescope-0",
      "STEAM_COMPAT_APP_ID=4242",
      "POLARIS_SESSION_INSTANCE_ID=gamescope-attached-test",
      "/bin/sh",
      "-c",
      "printf x >&3; exec /usr/bin/tail -f /dev/null",
      static_cast<char *>(nullptr)
    );
    _exit(127);
  }
  linux_child_guard_t child_guard {child};
  close(ready_pipe[1]);
  char ready = 0;
  ASSERT_EQ(read(ready_pipe[0], &ready, 1), 1);
  close(ready_pipe[0]);

  EXPECT_FALSE(proc::terminate_gamescope_attached_clients_for_tests("4242", child));
  EXPECT_EQ(kill(child, 0), 0) << "PID-reuse simulation signalled the captured process";

  EXPECT_FALSE(proc::terminate_gamescope_attached_clients_for_tests("4242", -1, child));
  EXPECT_EQ(kill(child, 0), 0) << "unreadable live candidate allowed partial pidfd signaling";

  EXPECT_TRUE(proc::terminate_gamescope_attached_clients_for_tests("4242"));
  int status = 0;
  ASSERT_EQ(child_guard.wait(&status, 0), child);
  EXPECT_TRUE(WIFSIGNALED(status));
}

TEST(ProcessRuntimeConfigTests, GamescopeAttachedUnreadableLiveProcessFailsClosed) {
  int ready_pipe[2] {-1, -1};
  ASSERT_EQ(pipe(ready_pipe), 0);
  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    if (setsid() < 0) _exit(124);
    close(ready_pipe[0]);
    setenv("GAMESCOPE_WAYLAND_DISPLAY", "gamescope-0", 1);
    setenv("STEAM_COMPAT_APP_ID", "4242", 1);
    setenv("POLARIS_SESSION_INSTANCE_ID", "gamescope-attached-test", 1);
    if (prctl(PR_SET_NAME, "game-client", 0, 0, 0) != 0) _exit(125);
    if (prctl(PR_SET_DUMPABLE, 0) != 0) _exit(126);
    const char ready = 'x';
    (void) write(ready_pipe[1], &ready, 1);
    close(ready_pipe[1]);
    for (;;) pause();
  }
  linux_child_guard_t child_guard {child};
  close(ready_pipe[1]);
  char ready = 0;
  ASSERT_EQ(read(ready_pipe[0], &ready, 1), 1);
  close(ready_pipe[0]);
  EXPECT_FALSE(proc::terminate_gamescope_attached_clients_for_tests("4242"));
  EXPECT_EQ(kill(child, 0), 0);
}

TEST(ProcessRuntimeConfigTests, GamescopeAttachedCleanupDrainsStrippedExactGenerationDescendant) {
  int ready_pipe[2] {-1, -1};
  ASSERT_EQ(pipe(ready_pipe), 0);
  setenv("GAMESCOPE_WAYLAND_DISPLAY", "gamescope-0", 1);
  setenv("STEAM_COMPAT_APP_ID", "4242", 1);
  setenv("POLARIS_SESSION_INSTANCE_ID", "gamescope-attached-test", 1);
  const pid_t root = fork();
  ASSERT_GE(root, 0);
  if (root == 0) {
    if (setsid() < 0) _exit(124);
    close(ready_pipe[0]);
    const std::string fd = std::to_string(ready_pipe[1]);
    const std::string command =
      "trap '' TERM; env -u GAMESCOPE_WAYLAND_DISPLAY -u STEAM_COMPAT_APP_ID "
      "-u POLARIS_SESSION_INSTANCE_ID /bin/sh -c 'trap \"\" TERM; while :; do sleep 1; done' & "
      "printf '%s\\n' \"$!\" >&" + fd + "; while :; do sleep 1; done";
    execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char *>(nullptr));
    _exit(127);
  }
  unsetenv("GAMESCOPE_WAYLAND_DISPLAY");
  unsetenv("STEAM_COMPAT_APP_ID");
  unsetenv("POLARIS_SESSION_INSTANCE_ID");
  close(ready_pipe[1]);
  char descendant_text[32] {};
  const auto count = read(ready_pipe[0], descendant_text, sizeof(descendant_text) - 1);
  ASSERT_GT(count, 0);
  close(ready_pipe[0]);
  const pid_t descendant = static_cast<pid_t>(std::strtol(descendant_text, nullptr, 10));
  ASSERT_GT(descendant, 1);
  const bool terminated = proc::terminate_gamescope_attached_clients_for_tests("4242");
  if (!terminated) {
    (void) kill(root, SIGKILL);
    (void) kill(descendant, SIGKILL);
  }
  EXPECT_TRUE(terminated);
  EXPECT_EQ(waitpid(root, nullptr, 0), root);
  errno = 0;
  EXPECT_EQ(kill(descendant, 0), -1);
  EXPECT_EQ(errno, ESRCH);
}

TEST(ProcessRuntimeConfigTests, GamescopeAttachedCleanupDrainsReparentedPrivateGroupDescendant) {
  int ready_pipe[2] {-1, -1};
  ASSERT_EQ(pipe(ready_pipe), 0);
  setenv("POLARIS_SESSION_INSTANCE_ID", "gamescope-attached-test", 1);
  const pid_t root = fork();
  ASSERT_GE(root, 0);
  if (root == 0) {
    close(ready_pipe[0]);
    if (setsid() < 0 || dup2(ready_pipe[1], 3) < 0) _exit(124);
    close(ready_pipe[1]);
    const char *program =
      "import os,signal,time\n"
      "middle=os.fork()\n"
      "if middle==0:\n"
      " descendant=os.fork()\n"
      " if descendant==0:\n"
      "  time.sleep(0.05)\n"
      "  os.setpgid(0,0)\n"
      "  os.write(3, str(os.getpid()).encode()+b'\\n')\n"
      "  os.environ.clear()\n"
      "  signal.signal(signal.SIGTERM, signal.SIG_IGN)\n"
      "  while True: time.sleep(1)\n"
      " os._exit(0)\n"
      "os.waitpid(middle,0)\n"
      "signal.pause()\n";
    execl(
      "/usr/bin/env", "env",
      "POLARIS_SESSION_INSTANCE_ID=gamescope-attached-test",
      "/usr/bin/python3", "-c", program,
      static_cast<char *>(nullptr)
    );
    _exit(127);
  }
  close(ready_pipe[1]);
  char descendant_text[32] {};
  ASSERT_GT(read(ready_pipe[0], descendant_text, sizeof(descendant_text) - 1), 0);
  close(ready_pipe[0]);
  const pid_t descendant = static_cast<pid_t>(std::strtol(descendant_text, nullptr, 10));
  ASSERT_GT(descendant, 1);
  const bool terminated = proc::terminate_gamescope_attached_clients_for_tests("4242");
  if (!terminated) {
    (void) kill(-root, SIGKILL);
  }
  EXPECT_TRUE(terminated);
  EXPECT_EQ(waitpid(root, nullptr, 0), root);
  errno = 0;
  EXPECT_EQ(kill(descendant, 0), -1);
  EXPECT_EQ(errno, ESRCH);
}

TEST(ProcessRuntimeConfigTests, GamescopeAttachedCleanupRejectsUnownedSameAppProcess) {
  int ready_pipe[2] {-1, -1};
  ASSERT_EQ(pipe(ready_pipe), 0);
  const pid_t child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    close(ready_pipe[0]);
    setenv("GAMESCOPE_WAYLAND_DISPLAY", "gamescope-0", 1);
    setenv("STEAM_COMPAT_APP_ID", "4242", 1);
    setenv("POLARIS_SESSION_INSTANCE_ID", "other-generation", 1);
    const char ready = 'x';
    (void) write(ready_pipe[1], &ready, 1);
    close(ready_pipe[1]);
    execl("/bin/bash", "bash", "-c", "exec -a AppId=4242 /usr/bin/tail -f /dev/null", static_cast<char *>(nullptr));
    _exit(127);
  }
  linux_child_guard_t child_guard {child};
  close(ready_pipe[1]);
  char ready = 0;
  ASSERT_EQ(read(ready_pipe[0], &ready, 1), 1);
  close(ready_pipe[0]);

  bool observed = false;
  for (int attempt = 0; attempt < 100; ++attempt) {
    std::ifstream cmdline("/proc/" + std::to_string(child) + "/cmdline", std::ios::binary);
    const std::string bytes {
      std::istreambuf_iterator<char> {cmdline}, std::istreambuf_iterator<char> {}
    };
    if (bytes.find("AppId=4242") != std::string::npos) {
      observed = true;
      break;
    }
    usleep(10000);
  }
  ASSERT_TRUE(observed);
  // Nothing carries the current session credential: cleanup is already done
  // (game quit before cancel). That is success — do not fail closed and brick
  // later teardown. Foreign generations that only share AppId / attach env must
  // still be left alone.
  EXPECT_TRUE(proc::terminate_gamescope_attached_clients_for_tests("4242"));
  EXPECT_EQ(kill(child, 0), 0) << "other-generation gamescope client was signalled";
}

TEST(ProcessRuntimeConfigTests, SteamShutdownOwnershipRejectsUnmarkedActiveClients) {
  const std::string running_status = "Name:\tsteam\nState:\tS (sleeping)\n";
  const std::string zombie_status = "Name:\tsteam\nState:\tZ (zombie)\n";
  const std::string token = "private-generation-42";
  const std::string marker = "POLARIS_SESSION_INSTANCE_ID=" + token + char(0);

  EXPECT_TRUE(proc::steam_shutdown_process_is_unowned_active_for_tests(
    "steamwebhelper", "/opt/steam/steamwebhelper", "/opt/steam/steamwebhelper", running_status, "", token
  ));
  EXPECT_FALSE(proc::steam_shutdown_process_is_unowned_active_for_tests(
    "steam", "/opt/steam/ubuntu12_32/steam", "/opt/steam/ubuntu12_32/steam", running_status, marker, token
  ));
  EXPECT_TRUE(proc::steam_shutdown_process_is_unowned_active_for_tests(
    "steam", "/opt/steam/ubuntu12_32/steam", "/opt/steam/ubuntu12_32/steam", running_status,
    "POLARIS_SESSION_INSTANCE_ID=" + token + "-stale" + char(0), token
  ));
  EXPECT_FALSE(proc::steam_shutdown_process_is_unowned_active_for_tests(
    "steam", "/opt/steam/ubuntu12_32/steam", "/opt/steam/ubuntu12_32/steam", zombie_status, "", token
  ));
}

TEST(ProcessRuntimeConfigTests, PidfdSteamTerminationSignalsOnlyCapturedChild) {
  int ready_pipe[2] {-1, -1};
  ASSERT_EQ(pipe(ready_pipe), 0);
  const auto child = fork();
  ASSERT_NE(child, -1);
  if (child == 0) {
    close(ready_pipe[0]);
    std::signal(SIGTERM, SIG_DFL);
    const char ready = '1';
    (void) write(ready_pipe[1], &ready, 1);
    for (;;) pause();
  }
  linux_child_guard_t child_guard {child};

  close(ready_pipe[1]);
  char ready = 0;
  ASSERT_EQ(read(ready_pipe[0], &ready, 1), 1);
  close(ready_pipe[0]);
  const bool terminated = proc::terminate_pid_with_pidfd_for_tests(
    child, std::chrono::milliseconds(500), std::chrono::milliseconds(500)
  );
  if (!terminated) {
    child_guard.terminate_and_reap();
  }
  int status = 0;
  ASSERT_EQ(child_guard.wait(&status, 0), child);
  EXPECT_TRUE(terminated);
  ASSERT_TRUE(WIFSIGNALED(status));
  EXPECT_EQ(WTERMSIG(status), SIGTERM);
}

TEST(ProcessRuntimeConfigTests, PidfdSteamTerminationBoundsGracefulWaitAndKillsSurvivor) {
  int ready_pipe[2] {-1, -1};
  ASSERT_EQ(pipe(ready_pipe), 0);
  const auto child = fork();
  ASSERT_NE(child, -1);
  if (child == 0) {
    close(ready_pipe[0]);
    std::signal(SIGTERM, SIG_IGN);
    const char ready = '1';
    (void) write(ready_pipe[1], &ready, 1);
    for (;;) pause();
  }
  linux_child_guard_t child_guard {child};

  close(ready_pipe[1]);
  char ready = 0;
  ASSERT_EQ(read(ready_pipe[0], &ready, 1), 1);
  close(ready_pipe[0]);
  const auto started = std::chrono::steady_clock::now();
  const bool terminated = proc::terminate_pid_with_pidfd_for_tests(
    child, std::chrono::milliseconds(100), std::chrono::milliseconds(500)
  );
  const auto elapsed = std::chrono::steady_clock::now() - started;
  if (!terminated) {
    child_guard.terminate_and_reap();
  }
  int status = 0;
  ASSERT_EQ(child_guard.wait(&status, 0), child);
  EXPECT_TRUE(terminated);
  EXPECT_LT(elapsed, std::chrono::seconds(2));
  ASSERT_TRUE(WIFSIGNALED(status));
  EXPECT_EQ(WTERMSIG(status), SIGKILL);
}

TEST(ProcessRuntimeConfigTests, PidfdSteamTerminationDeadlineSurvivesRepeatedEintr) {
#ifdef __linux__
  int ready_pipe[2] {-1, -1};
  ASSERT_EQ(pipe(ready_pipe), 0);
  const auto child = fork();
  ASSERT_NE(child, -1);
  if (child == 0) {
    close(ready_pipe[0]);
    std::signal(SIGTERM, SIG_IGN);
    const char ready = '1';
    (void) write(ready_pipe[1], &ready, 1);
    for (;;) pause();
  }
  linux_child_guard_t child_guard {child};

  close(ready_pipe[1]);
  char ready = 0;
  ASSERT_EQ(read(ready_pipe[0], &ready, 1), 1);
  close(ready_pipe[0]);

  sigusr1_interruptions = 0;
  sigusr1_interrupt_guard_t signal_guard;
  ASSERT_TRUE(signal_guard.installed);
  const auto test_thread = pthread_self();
  std::atomic<bool> sender_ready {false};
  std::atomic<bool> call_active {false};
  std::atomic<bool> production_wait_entered {false};
  std::atomic<int> signals_sent_while_call_active {0};
  std::jthread interrupter([
    test_thread,
    &sender_ready,
    &call_active,
    &production_wait_entered,
    &signals_sent_while_call_active
  ](std::stop_token stop_token) {
    sender_ready.store(true, std::memory_order_release);
    while (!stop_token.stop_requested() && !production_wait_entered.load(std::memory_order_acquire)) {
      std::this_thread::yield();
    }
    const auto stop_at = std::chrono::steady_clock::now() + std::chrono::milliseconds(350);
    while (!stop_token.stop_requested() &&
           call_active.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < stop_at) {
      if (pthread_kill(test_thread, SIGUSR1) == 0 && call_active.load(std::memory_order_acquire)) {
        signals_sent_while_call_active.fetch_add(1, std::memory_order_relaxed);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
  });
  while (!sender_ready.load(std::memory_order_acquire)) {
    std::this_thread::yield();
  }

  const auto handled_before_call = sigusr1_interruptions;
  call_active.store(true, std::memory_order_release);
  const auto started = std::chrono::steady_clock::now();
  const bool terminated = proc::terminate_pid_with_pidfd_after_wait_entry_for_tests(
    child,
    std::chrono::milliseconds(100),
    std::chrono::milliseconds(500),
    production_wait_entered
  );
  const auto elapsed = std::chrono::steady_clock::now() - started;
  const auto handled_at_return = sigusr1_interruptions;
  call_active.store(false, std::memory_order_release);
  interrupter.request_stop();
  interrupter.join();
  EXPECT_TRUE(production_wait_entered.load(std::memory_order_acquire));
  EXPECT_GT(signals_sent_while_call_active.load(std::memory_order_relaxed), 0);
  EXPECT_GT(handled_at_return, handled_before_call)
    << "SIGUSR1 must be handled after production enters pidfd wait and before it returns";
  EXPECT_TRUE(signal_guard.restore())
    << "signal disposition, mask, and pending state must restore explicitly";

  if (!terminated) {
    child_guard.terminate_and_reap();
  }
  int status = 0;
  ASSERT_EQ(child_guard.wait(&status, 0), child);
  EXPECT_TRUE(terminated);
  EXPECT_LT(elapsed, std::chrono::milliseconds(250));
  ASSERT_TRUE(WIFSIGNALED(status));
  EXPECT_EQ(WTERMSIG(status), SIGKILL);
#else
  GTEST_SKIP() << "Linux-only pidfd interruption deadline";
#endif
}

TEST(ProcessRuntimeConfigTests, PidfdSteamTerminationDeadlineSurvivesZeroPollEintr) {
#ifdef __linux__
  int ready_pipe[2] {-1, -1};
  ASSERT_EQ(pipe(ready_pipe), 0);
  const auto child = fork();
  ASSERT_NE(child, -1);
  if (child == 0) {
    close(ready_pipe[0]);
    std::signal(SIGTERM, SIG_IGN);
    const char ready = '1';
    (void) write(ready_pipe[1], &ready, 1);
    for (;;) pause();
  }
  linux_child_guard_t child_guard {child};

  close(ready_pipe[1]);
  char ready = 0;
  ASSERT_EQ(read(ready_pipe[0], &ready, 1), 1);
  close(ready_pipe[0]);

  const auto started = std::chrono::steady_clock::now();
  const bool terminated = proc::terminate_pid_with_forced_zero_poll_eintr_for_tests(
    child,
    std::chrono::milliseconds(50),
    std::chrono::milliseconds(50),
    80,
    std::chrono::milliseconds(5)
  );
  const auto elapsed = std::chrono::steady_clock::now() - started;

  int status = 0;
  const auto reaped = terminated ? child_guard.wait(&status, 0) : child_guard.terminate_and_reap(&status);
  ASSERT_EQ(reaped, child);
  EXPECT_FALSE(terminated);
  EXPECT_LT(elapsed, std::chrono::milliseconds(200));
  ASSERT_TRUE(WIFSIGNALED(status));
  EXPECT_EQ(WTERMSIG(status), SIGKILL);
#else
  GTEST_SKIP() << "Linux-only pidfd zero-poll interruption deadline";
#endif
}

TEST(ProcessRuntimeConfigTests, ProductionPreCageSteamTeardownUsesImmutableExactGeneration) {
#ifdef __linux__
  linux_cage_compositor_guard_t config_guard;
  const std::string token = "production-wiring-generation";
  proc::ctx_t steam_app {};
  steam_app.name = "Steam Big Picture";
  steam_app.cmd = "steam -gamepadui";

  auto spawn_steam_like = [&](bool marked, const char *argv0 = "steam") {
    const auto child = fork();
    EXPECT_GE(child, 0);
    if (child == 0) {
      if (marked) {
        setenv("POLARIS_SESSION_INSTANCE_ID", token.c_str(), 1);
      } else {
        unsetenv("POLARIS_SESSION_INSTANCE_ID");
      }
      execl("/bin/sleep", argv0, "60", nullptr);
      _exit(127);
    }
    return child;
  };
  auto wait_for_token = [&](pid_t pid) {
    const auto expected = std::string("POLARIS_SESSION_INSTANCE_ID=") + token;
    for (int attempt = 0; attempt < 40; ++attempt) {
      std::ifstream environ("/proc/" + std::to_string(pid) + "/environ", std::ios::binary);
      std::ostringstream bytes;
      bytes << environ.rdbuf();
      if (bytes.str().find(expected) != std::string::npos) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return false;
  };
  auto owned = spawn_steam_like(true);
  linux_child_guard_t owned_guard {owned};
  ASSERT_GT(owned, 0);
  ASSERT_TRUE(wait_for_token(owned));
  config::video.linux_display.use_cage_compositor = false;
  EXPECT_TRUE(proc::terminate_session_owned_steam_before_cage_stop_for_tests(
    steam_app, true, token
  ));
  int status = 0;
  const auto owned_wait = owned_guard.wait(&status, WNOHANG);
  EXPECT_EQ(owned_wait, owned);
  if (owned_wait == 0) {
    EXPECT_EQ(owned_guard.terminate_and_reap(&status), owned);
  }

  auto faulted_owned = spawn_steam_like(true);
  linux_child_guard_t faulted_guard {faulted_owned};
  auto faulted_control = spawn_steam_like(true, "steamwebhelper");
  linux_child_guard_t faulted_control_guard {faulted_control};
  ASSERT_GT(faulted_owned, 0);
  ASSERT_GT(faulted_control, 0);
  ASSERT_TRUE(wait_for_token(faulted_owned));
  ASSERT_TRUE(wait_for_token(faulted_control));
  EXPECT_FALSE(proc::terminate_session_owned_steam_with_forced_capture_failure_for_tests(
    steam_app, true, token, faulted_owned
  ));
  EXPECT_EQ(faulted_guard.wait(&status, WNOHANG), 0)
    << "pre-cage ownership capture failure must preserve the faulted Steam process";
  EXPECT_EQ(faulted_control_guard.wait(&status, WNOHANG), 0)
    << "incomplete ownership capture must preserve other captured Steam processes";
  EXPECT_FALSE(proc::terminate_session_owned_steam_with_pidfd_open_error_for_tests(
    steam_app, true, token, faulted_owned
  ));
  EXPECT_EQ(faulted_guard.wait(&status, WNOHANG), 0)
    << "non-ESRCH pidfd_open failure must preserve the target Steam process";
  EXPECT_EQ(faulted_control_guard.wait(&status, WNOHANG), 0)
    << "non-ESRCH pidfd_open failure must preserve every captured Steam process";
  EXPECT_FALSE(proc::terminate_session_owned_steam_with_post_capture_enumeration_error_for_tests(
    steam_app, true, token, faulted_owned
  ));
  EXPECT_EQ(faulted_guard.wait(&status, WNOHANG), 0)
    << "post-capture proc enumeration failure must not authorize pre-cage signaling";
  EXPECT_EQ(faulted_control_guard.wait(&status, WNOHANG), 0)
    << "partial enumeration must preserve every captured Steam process";
  EXPECT_FALSE(proc::terminate_session_owned_steam_with_reused_pid_for_tests(
    steam_app, true, token, faulted_owned
  ));
  EXPECT_EQ(faulted_guard.wait(&status, WNOHANG), 0)
    << "an active Steam replacement at a reused PID must keep ownership capture incomplete";
  EXPECT_EQ(faulted_control_guard.wait(&status, WNOHANG), 0)
    << "reused-PID ambiguity must preserve other captured Steam processes";
  EXPECT_TRUE(proc::terminate_session_owned_steam_before_cage_stop_for_tests(
    steam_app, true, token
  ));
  const auto faulted_wait = faulted_guard.wait(&status, WNOHANG);
  EXPECT_EQ(faulted_wait, faulted_owned);
  if (faulted_wait == 0) {
    EXPECT_EQ(faulted_guard.terminate_and_reap(&status), faulted_owned);
  }
  const auto control_wait = faulted_control_guard.wait(&status, WNOHANG);
  EXPECT_EQ(control_wait, faulted_control);
  if (control_wait == 0) {
    EXPECT_EQ(faulted_control_guard.terminate_and_reap(&status), faulted_control);
  }

  auto mirror_owned = spawn_steam_like(true);
  linux_child_guard_t mirror_guard {mirror_owned};
  ASSERT_GT(mirror_owned, 0);
  ASSERT_TRUE(wait_for_token(mirror_owned));
  config::video.linux_display.use_cage_compositor = true;
  EXPECT_FALSE(proc::terminate_session_owned_steam_before_cage_stop_for_tests(
    steam_app, false, token
  ));
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  EXPECT_EQ(mirror_guard.wait(&status, WNOHANG), 0);
#else
  GTEST_SKIP() << "Linux-only immutable private Steam teardown wiring";
#endif
}

TEST(ProcessRuntimeConfigTests, ProductionPreCageSteamTeardownDrainsHelpersBeforeFallbackSignals) {
#ifdef __linux__
  linux_cage_compositor_guard_t config_guard;
  const std::string token = "steam-root-first-generation";
  proc::ctx_t steam_app {};
  steam_app.name = "MOUSE: P.I. For Hire";
  steam_app.source = "steam";
  steam_app.steam_appid = "2416450";

  int ready_pipe[2] {-1, -1};
  int event_pipe[2] {-1, -1};
  int helper_pid_pipe[2] {-1, -1};
  ASSERT_EQ(pipe(ready_pipe), 0);
  ASSERT_EQ(pipe(event_pipe), 0);
  ASSERT_EQ(pipe(helper_pid_pipe), 0);

  const auto root = fork();
  ASSERT_NE(root, -1);
  if (root == 0) {
    close(ready_pipe[0]);
    close(event_pipe[0]);
    close(helper_pid_pipe[0]);
    setenv("POLARIS_SESSION_INSTANCE_ID", token.c_str(), 1);

    const auto helper_script =
      "import os, signal, time\n"
      "event_fd = " + std::to_string(event_pipe[1]) + "\n"
      "ready_fd = " + std::to_string(ready_pipe[1]) + "\n"
      "def on_term(signum, frame):\n"
      "    os.write(event_fd, b'H')\n"
      "def on_drain(signum, frame):\n"
      "    time.sleep(1.0)\n"
      "    os.write(event_fd, b'G')\n"
      "    raise SystemExit(0)\n"
      "signal.signal(signal.SIGTERM, on_term)\n"
      "signal.signal(signal.SIGUSR1, on_drain)\n"
      "os.write(ready_fd, b'H')\n"
      "while True:\n"
      "    signal.pause()\n";
    setenv("POLARIS_TEST_STEAM_HELPER_SCRIPT", helper_script.c_str(), 1);
    const auto root_script =
      std::string {"exec -a steamwebhelper python3 -c \"$POLARIS_TEST_STEAM_HELPER_SCRIPT\" & "} +
      "helper=$!; printf '%s\\n' \"$helper\" >&" + std::to_string(helper_pid_pipe[1]) + "; " +
      "trap 'printf R >&" + std::to_string(event_pipe[1]) +
      "; kill -USR1 \"$helper\"; exit 0' TERM; " +
      "printf R >&" + std::to_string(ready_pipe[1]) + "; " +
      "wait \"$helper\"";
    execl("/bin/bash", "/tmp/ubuntu12_32/steam", "-c", root_script.c_str(), nullptr);
    _exit(120);
  }
  linux_child_guard_t root_guard {root};

  close(ready_pipe[1]);
  close(event_pipe[1]);
  close(helper_pid_pipe[1]);
  std::string helper_pid_text;
  for (;;) {
    char digit = 0;
    ASSERT_EQ(read(helper_pid_pipe[0], &digit, 1), 1);
    if (digit == '\n') {
      break;
    }
    helper_pid_text.push_back(digit);
  }
  close(helper_pid_pipe[0]);
  const auto helper = static_cast<pid_t>(std::stol(helper_pid_text));
  ASSERT_GT(helper, 0);

  std::string ready_events;
  while (ready_events.size() < 2) {
    char event = 0;
    ASSERT_EQ(read(ready_pipe[0], &event, 1), 1);
    ready_events.push_back(event);
  }
  close(ready_pipe[0]);
  EXPECT_NE(ready_events.find('R'), std::string::npos);
  EXPECT_NE(ready_events.find('H'), std::string::npos);

  config::video.linux_display.use_cage_compositor = false;
  const bool terminated = proc::terminate_session_owned_steam_before_cage_stop_for_tests(
    steam_app, true, token
  );
  if (!terminated) {
    (void) kill(root, SIGKILL);
    (void) kill(helper, SIGKILL);
    int ignored_status = 0;
    (void) root_guard.wait(&ignored_status, 0);
    close(event_pipe[0]);
    FAIL() << "production pre-cage Steam ownership capture unexpectedly refused the test graph";
    return;
  }

  int root_status = 0;
  ASSERT_EQ(root_guard.wait(&root_status, 0), root);
  ASSERT_TRUE(WIFEXITED(root_status));
  EXPECT_EQ(WEXITSTATUS(root_status), 0);

  std::string shutdown_events;
  for (;;) {
    char buffer[16];
    const auto bytes = read(event_pipe[0], buffer, sizeof(buffer));
    if (bytes > 0) {
      shutdown_events.append(buffer, static_cast<std::size_t>(bytes));
      continue;
    }
    ASSERT_EQ(bytes, 0);
    break;
  }
  close(event_pipe[0]);

  EXPECT_NE(shutdown_events.find('R'), std::string::npos)
    << "the Steam client root must receive the initial graceful signal";
  EXPECT_NE(shutdown_events.find('G'), std::string::npos)
    << "the Steam helper must drain through the client root";
  EXPECT_EQ(shutdown_events.find('H'), std::string::npos)
    << "Steam helpers must not receive fallback SIGTERM before the client root can drain them";
#else
  GTEST_SKIP() << "Linux-only private Steam process-graph teardown ordering";
#endif
}

TEST(ProcessRuntimeConfigTests, ProductionPreCageSteamTeardownTreatsSteamScriptAsClientRoot) {
#ifdef __linux__
  linux_cage_compositor_guard_t config_guard;
  const std::string token = "steam-script-root-generation";
  proc::ctx_t steam_app {};
  steam_app.name = "Steam Big Picture";
  steam_app.source = "steam";

  const auto root = fork();
  ASSERT_NE(root, -1);
  if (root == 0) {
    setenv("POLARIS_SESSION_INSTANCE_ID", token.c_str(), 1);
    execl("/bin/sleep", "/tmp/steam.sh", "60", nullptr);
    _exit(127);
  }
  linux_child_guard_t root_guard {root};

  const auto expected = std::string("POLARIS_SESSION_INSTANCE_ID=") + token;
  bool ready = false;
  for (int attempt = 0; attempt < 40; ++attempt) {
    std::ifstream environ("/proc/" + std::to_string(root) + "/environ", std::ios::binary);
    std::ostringstream bytes;
    bytes << environ.rdbuf();
    if (bytes.str().find(expected) != std::string::npos) {
      ready = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  ASSERT_TRUE(ready);

  config::video.linux_display.use_cage_compositor = false;
  EXPECT_TRUE(proc::terminate_session_owned_steam_before_cage_stop_for_tests(
    steam_app, true, token
  ));

  int status = 0;
  const auto waited = root_guard.wait(&status, WNOHANG);
  EXPECT_EQ(waited, root);
  if (waited == 0) {
    EXPECT_EQ(root_guard.terminate_and_reap(&status), root);
  }
#else
  GTEST_SKIP() << "Linux-only private Steam script root classification";
#endif
}

TEST(ProcessRuntimeConfigTests, ProductionPreCageSteamTeardownPrefersNativeClientOverSteamLauncher) {
#ifdef __linux__
  linux_cage_compositor_guard_t config_guard;
  const std::string token = "native-steam-root-priority-generation";
  proc::ctx_t steam_app {};
  steam_app.name = "MOUSE: P.I. For Hire";
  steam_app.source = "steam";
  steam_app.steam_appid = "2416450";

  auto spawn_owned = [&](const char *argv0) {
    const auto child = fork();
    EXPECT_NE(child, -1);
    if (child == 0) {
      setenv("POLARIS_SESSION_INSTANCE_ID", token.c_str(), 1);
      execl("/bin/sleep", argv0, "60", nullptr);
      _exit(127);
    }
    return child;
  };

  const auto native_root = spawn_owned("/tmp/ubuntu12_32/steam");
  linux_child_guard_t native_guard {native_root};
  const auto launcher = spawn_owned("steam");
  linux_child_guard_t launcher_guard {launcher};
  ASSERT_GT(native_root, 0);
  ASSERT_GT(launcher, 0);

  const auto expected = std::string("POLARIS_SESSION_INSTANCE_ID=") + token;
  auto wait_for_token = [&](pid_t pid) {
    for (int attempt = 0; attempt < 40; ++attempt) {
      std::ifstream environ("/proc/" + std::to_string(pid) + "/environ", std::ios::binary);
      std::ostringstream bytes;
      bytes << environ.rdbuf();
      if (bytes.str().find(expected) != std::string::npos) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return false;
  };
  ASSERT_TRUE(wait_for_token(native_root));
  ASSERT_TRUE(wait_for_token(launcher));

  config::video.linux_display.use_cage_compositor = false;
  EXPECT_TRUE(proc::terminate_session_owned_steam_before_cage_stop_for_tests(
    steam_app, true, token
  ));

  int status = 0;
  const auto native_wait = native_guard.wait(&status, WNOHANG);
  EXPECT_EQ(native_wait, native_root);
  if (native_wait == 0) {
    EXPECT_EQ(native_guard.terminate_and_reap(&status), native_root);
  }
  const auto launcher_wait = launcher_guard.wait(&status, WNOHANG);
  EXPECT_EQ(launcher_wait, launcher);
  if (launcher_wait == 0) {
    EXPECT_EQ(launcher_guard.terminate_and_reap(&status), launcher);
  }
#else
  GTEST_SKIP() << "Linux-only native Steam root priority";
#endif
}

TEST(ProcessRuntimeConfigTests, ProductionPreCageSteamTeardownSelectsTopLevelLauncherFromLauncherTree) {
#ifdef __linux__
  linux_cage_compositor_guard_t config_guard;
  const std::string token = "steam-launcher-tree-generation";
  proc::ctx_t steam_app {};
  steam_app.name = "Steam Big Picture";
  steam_app.source = "steam";

  int child_pid_pipe[2] {-1, -1};
  ASSERT_EQ(pipe(child_pid_pipe), 0);
  const auto launcher_root = fork();
  ASSERT_NE(launcher_root, -1);
  if (launcher_root == 0) {
    close(child_pid_pipe[0]);
    setenv("POLARIS_SESSION_INSTANCE_ID", token.c_str(), 1);
    const auto launcher_child = fork();
    if (launcher_child < 0) {
      _exit(120);
    }
    if (launcher_child == 0) {
      close(child_pid_pipe[1]);
      execl("/bin/sleep", "steam", "60", nullptr);
      _exit(121);
    }
    (void) write(child_pid_pipe[1], &launcher_child, sizeof(launcher_child));
    close(child_pid_pipe[1]);
    execl("/bin/sleep", "steam", "60", nullptr);
    _exit(122);
  }
  linux_child_guard_t root_guard {launcher_root};

  close(child_pid_pipe[1]);
  pid_t launcher_child = -1;
  ASSERT_EQ(read(child_pid_pipe[0], &launcher_child, sizeof(launcher_child)), sizeof(launcher_child));
  close(child_pid_pipe[0]);
  ASSERT_GT(launcher_child, 0);

  const auto expected = std::string("POLARIS_SESSION_INSTANCE_ID=") + token;
  auto wait_for_token = [&](pid_t pid) {
    for (int attempt = 0; attempt < 40; ++attempt) {
      std::ifstream environ("/proc/" + std::to_string(pid) + "/environ", std::ios::binary);
      std::ostringstream bytes;
      bytes << environ.rdbuf();
      if (bytes.str().find(expected) != std::string::npos) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return false;
  };
  ASSERT_TRUE(wait_for_token(launcher_root));
  ASSERT_TRUE(wait_for_token(launcher_child));

  config::video.linux_display.use_cage_compositor = false;
  const bool terminated = proc::terminate_session_owned_steam_before_cage_stop_for_tests(
    steam_app, true, token
  );
  if (!terminated) {
    (void) kill(launcher_child, SIGKILL);
  }
  EXPECT_TRUE(terminated);

  int status = 0;
  const auto root_wait = root_guard.wait(&status, WNOHANG);
  EXPECT_EQ(root_wait, launcher_root);
  if (root_wait == 0) {
    EXPECT_EQ(root_guard.terminate_and_reap(&status), launcher_root);
  }

  bool child_exited = false;
  for (int attempt = 0; attempt < 40; ++attempt) {
    std::ifstream status_file("/proc/" + std::to_string(launcher_child) + "/status");
    std::ostringstream status_bytes;
    status_bytes << status_file.rdbuf();
    if (!status_file || status_bytes.str().find("State:\tZ") != std::string::npos) {
      child_exited = true;
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  if (!child_exited) {
    (void) kill(launcher_child, SIGKILL);
  }
  EXPECT_TRUE(child_exited);
#else
  GTEST_SKIP() << "Linux-only Steam launcher tree root selection";
#endif
}

TEST(ProcessRuntimeConfigTests, ProductionPreCageSteamTeardownTerminatesOnlyTheExactGenerationSteam) {
#ifdef __linux__
  linux_cage_compositor_guard_t config_guard;
  const std::string token = "mixed-production-wiring-generation";
  proc::ctx_t steam_app {};
  steam_app.name = "Steam Big Picture";
  steam_app.cmd = "steam -gamepadui";

  auto spawn_steam_like = [&](bool marked) {
    const auto child = fork();
    EXPECT_GE(child, 0);
    if (child == 0) {
      if (marked) {
        setenv("POLARIS_SESSION_INSTANCE_ID", token.c_str(), 1);
      } else {
        unsetenv("POLARIS_SESSION_INSTANCE_ID");
      }
      execl("/bin/sleep", "steam", "60", nullptr);
      _exit(127);
    }
    return child;
  };
  auto wait_for_steam_like = [&](pid_t pid, bool marked) {
    const auto expected = std::string("POLARIS_SESSION_INSTANCE_ID=") + token;
    for (int attempt = 0; attempt < 40; ++attempt) {
      std::ifstream cmdline_file("/proc/" + std::to_string(pid) + "/cmdline", std::ios::binary);
      const std::string cmdline(
        (std::istreambuf_iterator<char>(cmdline_file)),
        std::istreambuf_iterator<char>()
      );
      std::ifstream environ_file("/proc/" + std::to_string(pid) + "/environ", std::ios::binary);
      const std::string environ(
        (std::istreambuf_iterator<char>(environ_file)),
        std::istreambuf_iterator<char>()
      );
      const bool marker_matches = environ.find(expected) != std::string::npos;
      if (cmdline.starts_with(std::string("steam\0", 6)) && marker_matches == marked) {
        return true;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    return false;
  };
  const auto owned = spawn_steam_like(true);
  linux_child_guard_t owned_guard {owned};
  const auto unowned = spawn_steam_like(false);
  linux_child_guard_t unowned_guard {unowned};
  ASSERT_GT(owned, 0);
  ASSERT_GT(unowned, 0);
  ASSERT_TRUE(wait_for_steam_like(owned, true));
  ASSERT_TRUE(wait_for_steam_like(unowned, false));

  // Desktop Steam still running does not block the teardown: exact-generation
  // ownership exists precisely so the session-owned client can be terminated
  // while the user's own Steam is left alone. The call still reports false,
  // because survivor cleanup stops as soon as it sees a client it does not own,
  // and the caller falls back to cage cleanup.
  EXPECT_FALSE(proc::terminate_session_owned_steam_before_cage_stop_for_tests(
    steam_app, true, token
  ));

  int owned_status = 0;
  pid_t owned_wait = 0;
  for (int attempt = 0; attempt < 40 && owned_wait == 0; ++attempt) {
    owned_wait = owned_guard.wait(&owned_status, WNOHANG);
    if (owned_wait == 0) {
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
  }
  EXPECT_EQ(owned_wait, owned)
    << "the exact-generation Steam process must be terminated even under mixed ownership";

  int unowned_status = 0;
  const auto unowned_wait = unowned_guard.wait(&unowned_status, WNOHANG);
  EXPECT_EQ(unowned_wait, 0)
    << "the unowned desktop Steam process must never be terminated";

#else
  GTEST_SKIP() << "Linux-only mixed private/desktop Steam ownership";
#endif
}

TEST(ProcessRuntimeConfigTests, GamescopeCountsAsPrivateFamilyLaunch) {
#ifdef __linux__
  // labwc private stream: headless + cage.
  EXPECT_TRUE(proc::streaming_launch_requests_private_family(true, true, "headless_stream", ""));
  // Gamescope reaches its cage through the mode/runtime signal, not the cage
  // config flag, so it must still be classified private-family or its desktop
  // Steam drain/refuse policy never runs.
  EXPECT_TRUE(proc::streaming_launch_requests_private_family(false, false, "gamescope_stream", ""));
  EXPECT_TRUE(proc::streaming_launch_requests_private_family(false, false, "", "gamescope"));
  // Genuine desktop/mirror launches stay non-private.
  EXPECT_FALSE(proc::streaming_launch_requests_private_family(false, false, "desktop_display", ""));
  EXPECT_FALSE(proc::streaming_launch_requests_private_family(false, false, "host_virtual_display", ""));
#endif
}

TEST(ProcessRuntimeConfigTests, UnreadableEnvironLatchSparesPrivilegedDescendants) {
#ifdef __linux__
  // Steam's setuid sandbox helpers: EACCES on environ, root real uid,
  // descended from polaris. Unreadable AND unsignalable — they die with the
  // captured same-uid ancestors, so they must not wedge the teardown into
  // "retaining immutable cage generation" and 503s until restart.
  EXPECT_FALSE(proc::unreadable_environ_latches_capture_for_tests(EACCES, uid_t {0}, uid_t {1000}, true));
  EXPECT_FALSE(proc::unreadable_environ_latches_capture_for_tests(EACCES, uid_t {0}, uid_t {1000}, std::nullopt));
  // A same-uid process hiding its environ stays conservative: it may be a
  // session member the cleanup would otherwise leave running.
  EXPECT_TRUE(proc::unreadable_environ_latches_capture_for_tests(EACCES, uid_t {1000}, uid_t {1000}, true));
  // Unknown uid keeps the latch too.
  EXPECT_TRUE(proc::unreadable_environ_latches_capture_for_tests(EACCES, std::nullopt, uid_t {1000}, true));
  // Non-EACCES read failures on descendants keep the latch regardless of uid.
  EXPECT_TRUE(proc::unreadable_environ_latches_capture_for_tests(EIO, uid_t {0}, uid_t {1000}, true));
  // Non-descendants never latched and still do not.
  EXPECT_FALSE(proc::unreadable_environ_latches_capture_for_tests(EACCES, uid_t {1000}, uid_t {1000}, false));
#endif
}

TEST(ProcessRuntimeConfigTests, IsolatedSessionCleanupPolicyRetainsIncompleteCageGeneration) {
#ifdef __linux__
  EXPECT_TRUE(proc::isolated_session_generation_blocks_launch_for_tests(true, true));
  EXPECT_TRUE(proc::isolated_session_generation_blocks_launch_for_tests(true, false));
  EXPECT_TRUE(proc::isolated_session_generation_blocks_launch_for_tests(false, true));
  EXPECT_FALSE(proc::isolated_session_generation_blocks_launch_for_tests(false, false));

  EXPECT_FALSE(proc::isolated_session_cleanup_resets_router_for_tests(true, true, false));
  EXPECT_FALSE(proc::isolated_session_cleanup_clears_state_for_tests(true, true, false));
  EXPECT_FALSE(proc::isolated_session_cleanup_resets_router_for_tests(true, false, true));
  EXPECT_FALSE(proc::isolated_session_cleanup_clears_state_for_tests(true, false, true));

  EXPECT_TRUE(proc::isolated_session_cleanup_resets_router_for_tests(true, true, true));
  EXPECT_TRUE(proc::isolated_session_cleanup_clears_state_for_tests(true, true, true));

  // Mirror/non-cage launches still receive a generation token, but no cage state
  // is owned; teardown must clear that token without touching the cage router.
  EXPECT_FALSE(proc::isolated_session_cleanup_resets_router_for_tests(false, true, true));
  EXPECT_TRUE(proc::isolated_session_cleanup_clears_state_for_tests(false, true, true));

  EXPECT_FALSE(proc::isolated_session_uses_legacy_group_termination_for_tests(true, false));
  EXPECT_TRUE(proc::isolated_session_uses_legacy_group_termination_for_tests(false, false));
  EXPECT_FALSE(proc::isolated_session_uses_legacy_group_termination_for_tests(false, true));
  EXPECT_TRUE(proc::isolated_session_detaches_legacy_handles_for_tests(true));
  EXPECT_FALSE(proc::isolated_session_detaches_legacy_handles_for_tests(false));
#else
  GTEST_SKIP() << "Linux-only isolated session generation policy";
#endif
}

TEST(ProcessRuntimeConfigTests, ExternalCageRouterResetDoesNotSignalLiveProcess) {
#ifdef __linux__
  const auto child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    execl("/bin/sleep", "sleep", "60", nullptr);
    _exit(127);
  }
  linux_child_guard_t child_guard {child};

  cage_display_router::reset_after_external_stop_for_tests(child);
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  int status = 0;
  EXPECT_EQ(child_guard.wait(&status, WNOHANG), 0)
    << "router reset must not signal or reap a live externally-owned process";
#else
  GTEST_SKIP() << "Linux-only cage router signal safety";
#endif
}

TEST(ProcessRuntimeConfigTests, ExactGenerationCleanupLeavesUnownedControlProcessAlive) {
  const std::string token = "test-private-generation-4242";
  const auto spawn_sleep = [&](bool owned) {
    const auto child = fork();
    if (child == 0) {
      if (owned) {
        setenv("POLARIS_SESSION_INSTANCE_ID", token.c_str(), 1);
      } else {
        unsetenv("POLARIS_SESSION_INSTANCE_ID");
      }
      execl("/bin/sleep", "sleep", "60", static_cast<char *>(nullptr));
      _exit(127);
    }
    return child;
  };

  const auto owned = spawn_sleep(true);
  linux_child_guard_t owned_guard {owned};
  const auto control = spawn_sleep(false);
  linux_child_guard_t control_guard {control};
  if (owned <= 0 || control <= 0) {
    FAIL() << "failed to spawn exact-generation cleanup test children";
    return;
  }

  const auto owned_environ = "/proc/" + std::to_string(owned) + "/environ";
  bool token_visible = false;
  for (int i = 0; i < 40 && !token_visible; ++i) {
    std::ifstream in(owned_environ, std::ios::binary);
    const std::string environ((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    token_visible = environ.find("POLARIS_SESSION_INSTANCE_ID=" + token) != std::string::npos;
    if (!token_visible) {
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
  }
  if (!token_visible) {
    FAIL() << "exact generation token did not become visible in child environ";
    return;
  }

  EXPECT_TRUE(proc::terminate_exact_generation_processes_for_tests(token));

  int owned_status = 0;
  EXPECT_EQ(owned_guard.wait(&owned_status, 0), owned);
  EXPECT_TRUE(WIFSIGNALED(owned_status));

  int control_status = 0;
  EXPECT_EQ(control_guard.wait(&control_status, WNOHANG), 0)
    << "unowned control process must remain alive";
  EXPECT_EQ(control_guard.terminate_and_reap(&control_status), control);
}

TEST(ProcessRuntimeConfigTests, ExactGenerationCaptureFailureRetainsGenerationAndLeavesChildUnsignaled) {
#ifdef __linux__
  const std::string token = "forced-incomplete-generation-capture";
  const auto child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    setenv("POLARIS_SESSION_INSTANCE_ID", token.c_str(), 1);
    execl("/bin/sleep", "sleep", "60", nullptr);
    _exit(127);
  }
  linux_child_guard_t child_guard {child};

  const auto expected = std::string("POLARIS_SESSION_INSTANCE_ID=") + token;
  bool token_visible = false;
  for (int attempt = 0; attempt < 40 && !token_visible; ++attempt) {
    std::ifstream environ_file("/proc/" + std::to_string(child) + "/environ", std::ios::binary);
    const std::string environ(
      (std::istreambuf_iterator<char>(environ_file)),
      std::istreambuf_iterator<char>()
    );
    token_visible = environ.find(expected) != std::string::npos;
    if (!token_visible) {
      std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
  }
  ASSERT_TRUE(token_visible);

  EXPECT_TRUE(proc::isolated_session_capture_failure_retains_generation_for_tests(token, child));
  int status = 0;
  EXPECT_EQ(child_guard.wait(&status, WNOHANG), 0)
    << "an incompletely captured exact-generation child must not be signaled";

  EXPECT_TRUE(proc::exact_generation_pidfd_open_error_fails_closed_for_tests(token, child));
  EXPECT_EQ(child_guard.wait(&status, WNOHANG), 0)
    << "non-ESRCH pidfd_open failure must not authorize exact-generation signaling";

  EXPECT_TRUE(proc::exact_generation_post_capture_enumeration_error_fails_closed_for_tests(token, child));
  EXPECT_EQ(child_guard.wait(&status, WNOHANG), 0)
    << "a post-capture proc enumeration error must fail before signaling captured children";

  EXPECT_TRUE(proc::exact_generation_reused_pid_fails_closed_for_tests(token, child));
  EXPECT_EQ(child_guard.wait(&status, WNOHANG), 0)
    << "a reused exact-generation PID must keep capture incomplete without signaling the replacement";

  EXPECT_TRUE(proc::terminate_exact_generation_processes_for_tests(token));
  EXPECT_EQ(child_guard.wait(&status, 0), child);
  EXPECT_TRUE(WIFSIGNALED(status));
#else
  GTEST_SKIP() << "Linux-only exact-generation capture fault propagation";
#endif
}

TEST(ProcessRuntimeConfigTests, ExactGenerationUnreadableEnvironFailsClosedAndLeavesChildUnsignaled) {
#ifdef __linux__
  const std::string token = "unreadable-exact-generation-environ";
  int ready_pipe[2] {-1, -1};
  ASSERT_EQ(pipe(ready_pipe), 0);
  const auto child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    close(ready_pipe[0]);
    setenv("POLARIS_SESSION_INSTANCE_ID", token.c_str(), 1);
    if (prctl(PR_SET_DUMPABLE, 0) != 0) {
      _exit(126);
    }
    const char ready = '1';
    (void) write(ready_pipe[1], &ready, 1);
    close(ready_pipe[1]);
    for (;;) pause();
  }
  linux_child_guard_t child_guard {child};

  close(ready_pipe[1]);
  char ready = 0;
  ASSERT_EQ(read(ready_pipe[0], &ready, 1), 1);
  close(ready_pipe[0]);

  EXPECT_FALSE(proc::terminate_exact_generation_processes_for_tests(token));
  EXPECT_TRUE(proc::exact_generation_unknown_ancestry_fails_closed_for_tests(token, child));
  int status = 0;
  EXPECT_EQ(child_guard.wait(&status, WNOHANG), 0)
    << "an unreadable same-UID process must not be silently excluded or signaled";
#else
  GTEST_SKIP() << "Linux-only unreadable exact-generation environ handling";
#endif
}

TEST(ProcessRuntimeConfigTests, ExactGenerationProcEnumerationErrorFailsClosedAndLeavesChildUnsignaled) {
#ifdef __linux__
  const std::string token = "private-steam-enumeration-error-test";
  int ready_pipe[2] {-1, -1};
  ASSERT_EQ(pipe(ready_pipe), 0);
  const auto child = fork();
  ASSERT_NE(child, -1);
  if (child == 0) {
    close(ready_pipe[0]);
    setenv("POLARIS_SESSION_INSTANCE_ID", token.c_str(), 1);
    const char ready = '1';
    (void) write(ready_pipe[1], &ready, 1);
    close(ready_pipe[1]);
    for (;;) pause();
  }
  linux_child_guard_t child_guard {child};

  close(ready_pipe[1]);
  char ready = 0;
  ASSERT_EQ(read(ready_pipe[0], &ready, 1), 1);
  close(ready_pipe[0]);

  EXPECT_TRUE(proc::exact_generation_proc_enumeration_error_fails_closed_for_tests(token));
  int status = 0;
  EXPECT_EQ(child_guard.wait(&status, WNOHANG), 0)
    << "enumeration failure must never authorize signals";
#else
  GTEST_SKIP() << "Linux-only exact-generation /proc enumeration handling";
#endif
}

TEST(ProcessRuntimeConfigTests, SessionOwnedSteamUsesExactGenerationPidfdsBeforeImmutableCageCleanup) {
  const auto source = read_source_file_for_contract("src/process.cpp");
  const auto cage_source = read_source_file_for_contract("src/platform/linux/cage_display_router.cpp");
  ASSERT_FALSE(source.empty());
  ASSERT_FALSE(cage_source.empty());

  const auto implementation_start = source.find("bool terminate_session_owned_steam_before_cage_stop_impl(");
  const auto implementation_end = source.find("bool is_active_desktop_steam_client_process(", implementation_start);
  const auto helper_start = source.find("bool proc_t::terminate_session_owned_steam_before_cage_stop(");
  const auto terminate_start = source.find("void proc_t::terminate_impl(");
  const auto terminate_end = source.find("bool proc_t::reload_configuration_from_file", terminate_start);
  const auto execute_start = source.find("int proc_t::execute_impl(");
  const auto execute_end = source.find("int proc_t::running()", execute_start);
  ASSERT_NE(implementation_start, std::string::npos);
  ASSERT_NE(implementation_end, std::string::npos);
  ASSERT_NE(helper_start, std::string::npos);
  ASSERT_NE(terminate_start, std::string::npos);
  ASSERT_NE(terminate_end, std::string::npos);
  ASSERT_NE(execute_start, std::string::npos);
  ASSERT_NE(execute_end, std::string::npos);
  ASSERT_LT(helper_start, terminate_start);

  const auto implementation = source.substr(
    implementation_start, implementation_end - implementation_start
  );
  const auto helper = source.substr(helper_start, terminate_start - helper_start);
  EXPECT_NE(helper.find("terminate_session_owned_steam_before_cage_stop_impl("), std::string::npos);
  EXPECT_NE(helper.find("_app"), std::string::npos);
  EXPECT_NE(helper.find("_session_used_cage_compositor"), std::string::npos);
  EXPECT_NE(helper.find("_session_instance_id"), std::string::npos);
  const auto ownership_scan = implementation.find("steam_client_ownership_snapshot(session_instance_id)");
  const auto immutable_cage_gate = implementation.find("session_owned_cage");
  const auto generation_gate = implementation.find("session_instance_id.empty()");
  const auto capture_gate = implementation.find("ownership.capture_complete");
  const auto mixed_ownership_gate = implementation.find("ownership.unowned.empty()");
  const auto pidfd_termination = implementation.find("terminate_pidfds(");
  ASSERT_NE(ownership_scan, std::string::npos);
  ASSERT_NE(immutable_cage_gate, std::string::npos);
  ASSERT_NE(generation_gate, std::string::npos);
  ASSERT_NE(capture_gate, std::string::npos);
  ASSERT_NE(mixed_ownership_gate, std::string::npos);
  ASSERT_NE(pidfd_termination, std::string::npos);
  EXPECT_LT(ownership_scan, capture_gate);
  EXPECT_LT(capture_gate, pidfd_termination);
  EXPECT_LT(mixed_ownership_gate, pidfd_termination);
  EXPECT_EQ(implementation.find("config::video.linux_display.use_cage_compositor"), std::string::npos);
  EXPECT_EQ(implementation.find("cage_display_router::is_running()"), std::string::npos);
  EXPECT_EQ(implementation.find("platf::run_command("), std::string::npos);
  EXPECT_EQ(implementation.find("_env"), std::string::npos);
  EXPECT_EQ(implementation.find("child.wait"), std::string::npos);
  EXPECT_EQ(implementation.find("steam -shutdown"), std::string::npos);

  EXPECT_NE(source.find("SYS_pidfd_open"), std::string::npos);
  EXPECT_NE(source.find("SYS_pidfd_send_signal"), std::string::npos);
  EXPECT_NE(source.find("poll("), std::string::npos);
  EXPECT_NE(source.find("proc_environ_contains_exact_entry("), std::string::npos);
  EXPECT_NE(
    source.find("return final_snapshot.capture_complete && final_snapshot.owned.empty();"),
    std::string::npos
  );
  EXPECT_EQ(source.find("proc_environ_contains_isolated_session_marker("), std::string::npos);

  const auto execute = source.substr(execute_start, execute_end - execute_start);
  const auto refuse_stale_generation = execute.find(
    "refusing launch while an incompletely cleaned isolated session generation remains"
  );
  const auto generate_instance = execute.find("_session_instance_id = generate_session_token()");
  const auto inject_instance = execute.find("set_child_only_session_env_var(");
  const auto start_cage = execute.find("start_cage_with_runtime_fallback(");
  const auto remember_cage = execute.find("_session_used_cage_compositor = true", start_cage);
  ASSERT_NE(refuse_stale_generation, std::string::npos);
  EXPECT_NE(
    execute.find("isolated_session_generation_blocks_launch("),
    std::string::npos
  );
  ASSERT_NE(generate_instance, std::string::npos);
  ASSERT_NE(inject_instance, std::string::npos);
  ASSERT_NE(start_cage, std::string::npos);
  ASSERT_NE(remember_cage, std::string::npos);
  EXPECT_LT(refuse_stale_generation, generate_instance);
  EXPECT_LT(generate_instance, inject_instance);
  EXPECT_LT(inject_instance, start_cage);
  EXPECT_LT(start_cage, remember_cage);
  EXPECT_EQ(execute.find("terminate_isolated_session_processes(\"before launching"), std::string::npos);
  const auto cage_child_start = cage_source.find("if (pid == 0)");
  const auto cage_child_end = cage_source.find("set_labwc_process_environment(headless)", cage_child_start);
  ASSERT_NE(cage_child_start, std::string::npos);
  ASSERT_NE(cage_child_end, std::string::npos);
  const auto cage_child = cage_source.substr(cage_child_start, cage_child_end - cage_child_start);
  EXPECT_NE(
    cage_child.find("\n        setenv(\"POLARIS_SESSION_INSTANCE_ID\""),
    std::string::npos
  );
  const auto fail_guard_start = execute.find("auto fg = util::fail_guard(");
  const auto fail_guard_end = execute.find("if (!app.gamepad.empty()", fail_guard_start);
  ASSERT_NE(fail_guard_start, std::string::npos);
  ASSERT_NE(fail_guard_end, std::string::npos);
  const auto fail_guard = execute.substr(fail_guard_start, fail_guard_end - fail_guard_start);
  EXPECT_EQ(fail_guard.find("cage_display_router::stop();"), std::string::npos)
    << "execute failure cleanup must not bypass exact-generation pidfd cleanup";

  const auto terminate = source.substr(terminate_start, terminate_end - terminate_start);
  const auto terminate_private_steam = terminate.find("terminate_session_owned_steam_before_cage_stop(");
  const auto terminate_attached = terminate.find("terminate_gamescope_attached_session_clients(");
  const auto attached_failure_barrier = terminate.find("refusing all later teardown because exact Gamescope ancestry closure was incomplete");
  const auto terminate_generation = terminate.find("terminate_isolated_session_generation();");
  const auto terminate_main = terminate.find("terminate_process_group(");
  const auto legacy_group_gate = terminate.find(
    "isolated_session_uses_legacy_group_termination("
  );
  const auto legacy_detach_gate = terminate.find(
    "isolated_session_detaches_legacy_handles("
  );
  const auto detach_legacy_child = terminate.find("_process.detach();");
  const auto detach_legacy_group = terminate.find("_process_group.detach();");
  const auto clear_legacy_child = terminate.find("_process = boost::process::v1::child();");
  const auto immutable_undo_guard = terminate.find("_session_used_cage_compositor", clear_legacy_child);
  const auto finish_generation = terminate.find("finish_isolated_session_generation_cleanup();");
  ASSERT_NE(terminate_private_steam, std::string::npos);
  ASSERT_NE(terminate_attached, std::string::npos);
  ASSERT_NE(attached_failure_barrier, std::string::npos);
  ASSERT_NE(terminate_generation, std::string::npos);
  ASSERT_NE(terminate_main, std::string::npos);
  ASSERT_NE(legacy_group_gate, std::string::npos);
  ASSERT_NE(legacy_detach_gate, std::string::npos);
  ASSERT_NE(detach_legacy_child, std::string::npos);
  ASSERT_NE(detach_legacy_group, std::string::npos);
  ASSERT_NE(clear_legacy_child, std::string::npos);
  ASSERT_NE(immutable_undo_guard, std::string::npos);
  ASSERT_NE(finish_generation, std::string::npos);
  EXPECT_LT(terminate_attached, attached_failure_barrier);
  EXPECT_LT(attached_failure_barrier, terminate_private_steam);
  EXPECT_LT(terminate_private_steam, terminate_generation);
  EXPECT_NE(source.find("_session_used_gamescope_runtime = gamescope_stream_session;"), std::string::npos);
  EXPECT_NE(terminate.find("_session_used_gamescope_runtime"), std::string::npos);
  EXPECT_NE(source.find("const bool prior_cleanup_complete = _exact_generation_cleanup_complete;"), std::string::npos);
  EXPECT_NE(source.find("prior_cleanup_complete && isolated_cleanup_complete"), std::string::npos);
  EXPECT_LT(terminate_generation, terminate_main);
  EXPECT_LT(terminate_generation, legacy_group_gate);
  EXPECT_LT(legacy_group_gate, legacy_detach_gate);
  EXPECT_LT(legacy_detach_gate, detach_legacy_child);
  EXPECT_LT(detach_legacy_child, detach_legacy_group);
  EXPECT_LT(detach_legacy_group, clear_legacy_child);
  EXPECT_LT(clear_legacy_child, immutable_undo_guard);
  EXPECT_LT(immutable_undo_guard, finish_generation);
  // terminate_impl may narrow captured completeness as later teardown stages
  // report, but it must never widen it: assigning a literal, or anything that is
  // not the flag ANDed with a fresh result, would let an incomplete generation
  // present itself as clean.
  for (std::size_t pos = terminate.find("_exact_generation_cleanup_complete =");
       pos != std::string::npos;
       pos = terminate.find("_exact_generation_cleanup_complete =", pos + 1)) {
    const auto statement_end = terminate.find(';', pos);
    ASSERT_NE(statement_end, std::string::npos);
    const auto assignment = collapse_whitespace(terminate.substr(pos, statement_end - pos));
    EXPECT_TRUE(assignment.starts_with("_exact_generation_cleanup_complete = _exact_generation_cleanup_complete &&"))
      << "terminate_impl must only narrow captured completeness, found: " << assignment;
  }
  EXPECT_EQ(terminate.find("_exact_generation_cleanup_complete = true"), std::string::npos)
    << "terminate_impl must not be able to replace captured completeness with a literal";

  const auto generation_cleanup_start = source.find("void proc_t::terminate_isolated_session_generation()");
  const auto generation_finish_start = source.find("void proc_t::finish_isolated_session_generation_cleanup()");
  const auto generation_finish_end = source.find("#endif", generation_finish_start);
  ASSERT_NE(generation_cleanup_start, std::string::npos);
  ASSERT_NE(generation_finish_start, std::string::npos);
  ASSERT_NE(generation_finish_end, std::string::npos);
  const auto generation_cleanup = source.substr(
    generation_cleanup_start,
    generation_finish_start - generation_cleanup_start
  );
  const auto generation_finish = source.substr(
    generation_finish_start,
    generation_finish_end - generation_finish_start
  );
  const auto cleanup_result = generation_cleanup.find(
    "const bool isolated_cleanup_complete = terminate_isolated_session_processes("
  );
  const auto cleanup_success_gate = generation_cleanup.find("isolated_session_cleanup_resets_router(");
  // The router reset goes through the labwc stream-runtime facade rather than
  // naming cage_display_router directly.
  const auto reset_cage = generation_cleanup.find("stream_runtime::labwc::reset_after_external_stop()");
  const auto retain_generation = generation_cleanup.find(
    "retaining immutable cage generation because exact-generation cleanup was incomplete"
  );
  const auto cleanup_clear_gate = generation_finish.find("isolated_session_cleanup_clears_state(");
  const auto clear_generation = generation_finish.find("_session_instance_id.clear()");
  ASSERT_NE(cleanup_result, std::string::npos);
  ASSERT_NE(cleanup_success_gate, std::string::npos);
  ASSERT_NE(reset_cage, std::string::npos);
  ASSERT_NE(retain_generation, std::string::npos);
  ASSERT_NE(cleanup_clear_gate, std::string::npos);
  ASSERT_NE(clear_generation, std::string::npos);
  EXPECT_LT(cleanup_result, cleanup_success_gate);
  EXPECT_LT(cleanup_success_gate, reset_cage);
  EXPECT_LT(reset_cage, retain_generation);
  EXPECT_LT(cleanup_clear_gate, clear_generation);
  EXPECT_EQ(terminate.find("config::video.linux_display.use_cage_compositor"), std::string::npos);
  EXPECT_EQ(terminate.find("cage_display_router::stop()"), std::string::npos);
  EXPECT_EQ(terminate.find("terminate_steam_app_processes("), std::string::npos);
  EXPECT_EQ(terminate.find("settle_isolated_browser_stream_steam_cleanup("), std::string::npos);

  const auto reset_start = cage_source.find("void reset_after_external_stop()");
  const auto reset_end = cage_source.find("bool is_running()", reset_start);
  ASSERT_NE(reset_start, std::string::npos);
  ASSERT_NE(reset_end, std::string::npos);
  const auto reset = cage_source.substr(reset_start, reset_end - reset_start);
  EXPECT_NE(reset.find("waitpid("), std::string::npos);
  EXPECT_NE(reset.find("WNOHANG"), std::string::npos);
  EXPECT_EQ(reset.find("kill("), std::string::npos);
  EXPECT_EQ(reset.find("SIGTERM"), std::string::npos);
  EXPECT_EQ(reset.find("SIGKILL"), std::string::npos);
}


TEST(ProcessRuntimeConfigTests, SteamBigPictureInputGuardIsStartedAndStoppedInsideProcessLifetime) {
  const auto source = read_source_file_for_contract("src/process.cpp");
  ASSERT_FALSE(source.empty());

  const auto execute_start = source.find("int proc_t::execute_impl(");
  const auto execute_end = source.find("int proc_t::running()", execute_start);
  const auto terminate_start = source.find("void proc_t::terminate_impl(");
  const auto terminate_end = source.find("bool proc_t::reload_configuration_from_file", terminate_start);
  ASSERT_NE(execute_start, std::string::npos);
  ASSERT_NE(execute_end, std::string::npos);
  ASSERT_NE(terminate_start, std::string::npos);
  ASSERT_NE(terminate_end, std::string::npos);

  const auto execute = source.substr(execute_start, execute_end - execute_start);
  const auto terminate = source.substr(terminate_start, terminate_end - terminate_start);
  const auto snapshot_guard = execute.find("snapshot_steam_big_picture_input_guard(");
  const auto first_prep_launch = execute.find("platf::run_command(cmd.elevated, true, cmd.do_cmd");
  const auto failed_launch_guard = execute.find("stop_guard_on_failed_launch = util::fail_guard");
  const auto failed_launch_stop = execute.find("stop_steam_big_picture_input_guard();", failed_launch_guard);
  const auto first_cage_launch = execute.find("start_cage_with_runtime_fallback(game_cmd)");
  const auto first_main_launch = execute.find("_process = platf::run_command(");
  const auto start_guard = execute.find("start_steam_big_picture_input_guard(");
  const auto launch_committed = execute.find("stop_guard_on_failed_launch.disable()");
  ASSERT_NE(snapshot_guard, std::string::npos);
  ASSERT_NE(first_prep_launch, std::string::npos);
  ASSERT_NE(failed_launch_guard, std::string::npos);
  ASSERT_NE(failed_launch_stop, std::string::npos);
  ASSERT_NE(first_cage_launch, std::string::npos);
  ASSERT_NE(first_main_launch, std::string::npos);
  ASSERT_NE(start_guard, std::string::npos);
  ASSERT_NE(launch_committed, std::string::npos);
  EXPECT_LT(snapshot_guard, first_prep_launch);
  EXPECT_LT(snapshot_guard, start_guard);
  EXPECT_LT(failed_launch_guard, failed_launch_stop);
  EXPECT_LT(failed_launch_stop, start_guard);
  EXPECT_LT(start_guard, first_cage_launch);
  EXPECT_LT(start_guard, first_main_launch);
  EXPECT_LT(first_main_launch, launch_committed);
  const auto stop_guard = terminate.find("stop_steam_big_picture_input_guard()");
  const auto cleanup_generation = terminate.find("terminate_isolated_session_generation();");
  const auto finish_generation = terminate.find("finish_isolated_session_generation_cleanup();");
  ASSERT_NE(stop_guard, std::string::npos);
  ASSERT_NE(cleanup_generation, std::string::npos);
  ASSERT_NE(finish_generation, std::string::npos);
  EXPECT_LT(stop_guard, cleanup_generation);
  EXPECT_LT(cleanup_generation, finish_generation);
}

TEST(ProcessRuntimeConfigTests, HeadlessCageSteamBigPictureSkipsHostShutdownUndo) {
  proc::ctx_t app {};
  app.name = "Steam Big Picture";
  app.detached = {"setsid steam -gamepadui"};

  proc::cmd_t shutdown_undo {
    std::string {},
    std::string {"setsid -f steam -shutdown"},
    false
  };

  EXPECT_TRUE(proc::should_skip_steam_shutdown_undo_after_cage_cleanup_for_tests(app, shutdown_undo, true));
  EXPECT_FALSE(proc::should_skip_steam_shutdown_undo_after_cage_cleanup_for_tests(app, shutdown_undo, false));

  proc::ctx_t unclassified_app {};
  unclassified_app.name = "Custom Cage App";
  unclassified_app.cmd = "/usr/bin/custom-launcher";
  EXPECT_TRUE(proc::should_skip_steam_shutdown_undo_after_cage_cleanup_for_tests(
    unclassified_app, shutdown_undo, true
  )) << "immutable cage ownership plus an explicit Steam shutdown undo must fail closed";
}


TEST(ProcessRuntimeConfigTests, DesktopSteamDetectorRecognizesSteamWebHelper) {
  EXPECT_TRUE(proc::desktop_steam_client_process_for_tests(
    "steamwebhelper",
    "/opt/steam-test/Steam/ubuntu12_64/steamwebhelper",
    std::string("/opt/steam-test/Steam/ubuntu12_64/steamwebhelper") + char(0) + "-type=zygote"
  ));
}

TEST(ProcessRuntimeConfigTests, DesktopSteamDetectorIgnoresTransientShutdownClient) {
  EXPECT_FALSE(proc::desktop_steam_client_process_for_tests(
    "steam",
    "/usr/bin/steam",
    std::string("steam") + char(0) + "-shutdown"
  ));
}

TEST(ProcessRuntimeConfigTests, DesktopSteamDetectorIgnoresZombieShutdownRemnant) {
  EXPECT_FALSE(proc::desktop_steam_client_process_for_tests(
    "steam",
    "",
    "",
    "Name:\tsteam\nState:\tZ (zombie)\nPPid:\t275560\n"
  ));
}

TEST(ProcessRuntimeConfigTests, DesktopSteamDetectorFailsClosedOnProcOpenError) {
#ifdef __linux__
  EXPECT_TRUE(proc::desktop_steam_proc_open_error_fails_closed_for_tests());
#else
  GTEST_SKIP() << "Linux-only desktop Steam proc scanner";
#endif
}

TEST(ProcessRuntimeConfigTests, DesktopSteamDetectorFailsClosedOnProcEnumerationError) {
#ifdef __linux__
  EXPECT_TRUE(proc::desktop_steam_proc_enumeration_error_fails_closed_for_tests());
#else
  GTEST_SKIP() << "Linux-only desktop Steam proc scanner";
#endif
}

TEST(ProcessRuntimeConfigTests, DesktopSteamDetectorFailsClosedOnProcReadError) {
#ifdef __linux__
  const auto child = fork();
  ASSERT_GE(child, 0);
  if (child == 0) {
    execl("/bin/sleep", "sleep", "60", nullptr);
    _exit(127);
  }
  linux_child_guard_t child_guard {child};
  for (int attempt = 0; attempt < 40; ++attempt) {
    std::ifstream status("/proc/" + std::to_string(child) + "/status");
    if (status.good()) {
      break;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
  }
  EXPECT_TRUE(proc::desktop_steam_proc_read_error_fails_closed_for_tests(child));
#else
  GTEST_SKIP() << "Linux-only desktop Steam proc scanner";
#endif
}

TEST(ProcessRuntimeConfigTests, DesktopSteamActiveOffersExplicitForcePrivateShutdown) {
  const auto policy = proc::resolve_desktop_launch_safety_policy_for_tests(
    true,
    false,
    true,
    true,
    true,
    false
  );

  EXPECT_TRUE(policy.desktopSteamActive);
  EXPECT_FALSE(policy.canLaunchPrivateStream);
  EXPECT_TRUE(policy.canForceCloseDesktopSteamForPrivateStream);
  EXPECT_EQ(policy.recommendedAction, "refuse_private_stream");
}

TEST(ProcessRuntimeConfigTests, ExplicitForcePrivateAfterDesktopSteamShutdownAllowsPrivateLaunch) {
  const auto policy = proc::resolve_desktop_launch_safety_policy_for_tests(
    true,
    false,
    true,
    true,
    false,
    true
  );

  EXPECT_TRUE(policy.canLaunchPrivateStream);
  EXPECT_EQ(policy.recommendedAction, "force_private_stream_after_desktop_steam_shutdown");
}

TEST(ProcessRuntimeConfigTests, ForcePrivateFlagWithoutPrivateStreamDoesNotEscalatePolicy) {
  const auto policy = proc::resolve_desktop_launch_safety_policy_for_tests(
    false,
    false,
    true,
    true,
    false,
    true
  );

  EXPECT_TRUE(policy.desktopSteamActive);
  EXPECT_FALSE(policy.canLaunchPrivateStream);
  EXPECT_EQ(policy.recommendedAction, "launch_desktop_stream");
}

TEST(ProcessRuntimeConfigTests, ExplicitMirrorBeatsContradictoryForcePrivateFlag) {
  const auto policy = proc::resolve_desktop_launch_safety_policy_for_tests(
    true,
    true,
    true,
    true,
    false,
    true
  );

  EXPECT_TRUE(policy.desktopSteamActive);
  EXPECT_EQ(policy.recommendedAction, "mirror_desktop");
}

TEST(ProcessRuntimeConfigTests, DesktopSteamActiveRefusesUnsafePrivateSteamLaunch) {
  const auto policy = proc::resolve_desktop_launch_safety_policy_for_tests(
    true,
    false,
    true,
    true,
    false
  );

  EXPECT_TRUE(policy.desktopSteamActive);
  EXPECT_TRUE(policy.physicalDisplayRisk);
  EXPECT_FALSE(policy.canLaunchPrivateStream);
  EXPECT_TRUE(policy.canMirrorDesktop);
  EXPECT_EQ(policy.recommendedAction, "refuse_private_stream");

  const auto contract = proc::desktop_launch_safety_policy_to_json(policy);
  EXPECT_TRUE(contract.at("desktopSteamActive"));
  EXPECT_TRUE(contract.at("physicalDisplayRisk"));
  EXPECT_FALSE(contract.at("canLaunchPrivateStream"));
  EXPECT_TRUE(contract.at("canMirrorDesktop"));
  EXPECT_EQ(contract.at("recommendedAction"), "refuse_private_stream");
}

TEST(ProcessRuntimeConfigTests, ActiveDesktopGameRefusesUnsafePrivateLaunch) {
  const auto policy = proc::resolve_desktop_launch_safety_policy_for_tests(
    true,
    false,
    false,
    false,
    true
  );

  EXPECT_FALSE(policy.desktopSteamActive);
  EXPECT_TRUE(policy.physicalDisplayRisk);
  EXPECT_FALSE(policy.canLaunchPrivateStream);
  EXPECT_TRUE(policy.canMirrorDesktop);
  EXPECT_EQ(policy.recommendedAction, "refuse_private_stream");
}

TEST(ProcessRuntimeConfigTests, ExplicitMirrorDesktopReportsPhysicalDisplayRisk) {
  const auto policy = proc::resolve_desktop_launch_safety_policy_for_tests(
    true,
    true,
    true,
    true,
    false
  );

  EXPECT_TRUE(policy.desktopSteamActive);
  EXPECT_TRUE(policy.physicalDisplayRisk);
  EXPECT_FALSE(policy.canLaunchPrivateStream);
  EXPECT_TRUE(policy.canMirrorDesktop);
  EXPECT_EQ(policy.recommendedAction, "mirror_desktop");
}

TEST(ProcessRuntimeConfigTests, NvHttpStreamingLaunchPolicyRefusesUnsafePrivateSteamLaunch) {
  nvhttp::args_t args;

  const auto policy = nvhttp::resolve_streaming_launch_safety_policy_for_tests(
    args,
    true,
    true,
    true,
    false
  );

  EXPECT_TRUE(policy.desktopSteamActive);
  EXPECT_TRUE(policy.physicalDisplayRisk);
  EXPECT_FALSE(policy.canLaunchPrivateStream);
  EXPECT_TRUE(policy.canMirrorDesktop);
  EXPECT_EQ(policy.recommendedAction, "refuse_private_stream");
}

TEST(ProcessRuntimeConfigTests, NvHttpStreamingLaunchPolicyAcceptsExplicitMirrorDesktopQueryParam) {
  nvhttp::args_t args;
  args.emplace("launchMode", "mirror_desktop");

  const auto policy = nvhttp::resolve_streaming_launch_safety_policy_for_tests(
    args,
    true,
    true,
    true,
    false
  );

  EXPECT_TRUE(policy.desktopSteamActive);
  EXPECT_TRUE(policy.physicalDisplayRisk);
  EXPECT_FALSE(policy.canLaunchPrivateStream);
  EXPECT_TRUE(policy.canMirrorDesktop);
  EXPECT_EQ(policy.recommendedAction, "mirror_desktop");
}

TEST(ProcessRuntimeConfigTests, ClearPrivateSteamLaunchIsAllowed) {
  const auto policy = proc::resolve_desktop_launch_safety_policy_for_tests(
    true,
    false,
    true,
    false,
    false
  );

  EXPECT_FALSE(policy.desktopSteamActive);
  EXPECT_FALSE(policy.physicalDisplayRisk);
  EXPECT_TRUE(policy.canLaunchPrivateStream);
  EXPECT_TRUE(policy.canMirrorDesktop);
  EXPECT_EQ(policy.recommendedAction, "launch_private_stream");
}
#endif

TEST(ProcessMigrationTests, ParseRepairsMalformedLegacyAppsJson) {
  const auto file_path = test_paths::root() / "legacy_apps_migration.json";

  const nlohmann::json legacy_apps = {
    {"version", 1},
    {"apps", {
      {
        {"name", "Legacy App"},
        {"uuid", 12345},
        {"allow-client-commands", "yes"},
        {"exclude-global-state-cmd", nlohmann::json::array({"off"})},
        {"auto-detach", nlohmann::json::array({"ON"})},
        {"wait-all", "YES"},
        {"terminate-on-pause", "true"},
        {"virtual-display", nlohmann::json::array({"on"})},
        {"last-launched", "1710000000"},
        {"exit-timeout", "bad"},
        {"scale-factor", "125"},
        {"prep-cmd", {{
          {"do", "echo hello"},
          {"elevated", nlohmann::json::array({"TRUE"})}
        }}},
        {"detached", {"echo ready"}}
      }
    }}
  };

  ASSERT_EQ(file_handler::write_file(file_path.string().c_str(), legacy_apps.dump(2)), 0);

  auto parsed_proc = proc::parse(file_path.string());
  ASSERT_TRUE(parsed_proc.has_value());

  const auto migrated_tree = nlohmann::json::parse(file_handler::read_file(file_path.string().c_str()));
  ASSERT_TRUE(migrated_tree.contains("version"));
  EXPECT_EQ(migrated_tree["version"], 9);
  ASSERT_TRUE(migrated_tree.contains("apps"));
  ASSERT_TRUE(migrated_tree["apps"].is_array());
  ASSERT_EQ(migrated_tree["apps"].size(), 1);

  const auto &migrated_app = migrated_tree["apps"][0];
  EXPECT_TRUE(migrated_app["uuid"].is_string());
  EXPECT_TRUE(migrated_app["allow-client-commands"].is_boolean());
  EXPECT_TRUE(migrated_app["exclude-global-state-cmd"].is_boolean());
  EXPECT_TRUE(migrated_app["auto-detach"].is_boolean());
  EXPECT_TRUE(migrated_app["wait-all"].is_boolean());
  EXPECT_TRUE(migrated_app["terminate-on-pause"].is_boolean());
  EXPECT_TRUE(migrated_app["virtual-display"].is_boolean());
  EXPECT_TRUE(migrated_app["last-launched"].is_number_integer());
  EXPECT_TRUE(migrated_app["exit-timeout"].is_number_integer());
  EXPECT_TRUE(migrated_app["scale-factor"].is_number_integer());
  EXPECT_TRUE(migrated_app["prep-cmd"][0]["elevated"].is_boolean());
  EXPECT_EQ(migrated_app["exit-timeout"], 5);
  EXPECT_EQ(migrated_app["scale-factor"], 125);

  const auto &apps = parsed_proc->get_apps();
  const auto migrated_ctx = std::find_if(apps.begin(), apps.end(), [](const auto &app) {
    return app.name == "Legacy App";
  });

  ASSERT_NE(migrated_ctx, apps.end());
  EXPECT_FALSE(migrated_ctx->uuid.empty());
  EXPECT_TRUE(migrated_ctx->allow_client_commands);
  EXPECT_TRUE(migrated_ctx->auto_detach);
  EXPECT_TRUE(migrated_ctx->wait_all);
  EXPECT_TRUE(migrated_ctx->virtual_display);
  EXPECT_TRUE(migrated_ctx->terminate_on_pause);
  EXPECT_EQ(migrated_ctx->scale_factor, 125);
  EXPECT_EQ(migrated_ctx->exit_timeout, std::chrono::seconds(5));
  EXPECT_EQ(migrated_ctx->last_launched, 1710000000);

  std::filesystem::remove(file_path);
}

TEST(ProcessMigrationTests, ParseNormalizesSteamBigPictureLaunchAndAddsCleanupUndo) {
#ifdef __linux__
  linux_cage_compositor_guard_t guard;
  config::video.linux_display.use_cage_compositor = false;
#endif

  const auto file_path = test_paths::root() / "steam_big_picture_normalization.json";

  const nlohmann::json apps = {
    {"version", 2},
    {"apps", {
      {
        {"name", "Steam Big Picture"},
        {"uuid", "steam-big-picture-test"},
        {"cmd", ""},
        {"detached", {"setsid steam -gamepadui"}},
        {"prep-cmd", nlohmann::json::array()},
        {"env", {
          {"MANGOHUD", "1"},
          {"MANGOHUD_CONFIG", "fps_limit=60"}
        }},
        {"image-path", "./assets/steam.png"}
      }
    }}
  };

  ASSERT_EQ(file_handler::write_file(file_path.string().c_str(), apps.dump(2)), 0);

  auto parsed_proc = proc::parse(file_path.string());
  ASSERT_TRUE(parsed_proc.has_value());

  const auto &parsed_apps = parsed_proc->get_apps();
  const auto steam_ctx = std::find_if(parsed_apps.begin(), parsed_apps.end(), [](const auto &app) {
    return app.name == "Steam Big Picture";
  });

  ASSERT_NE(steam_ctx, parsed_apps.end());
  ASSERT_EQ(steam_ctx->detached.size(), 1);
  EXPECT_EQ(steam_ctx->detached.front(), "setsid steam -gamepadui");
  EXPECT_TRUE(steam_ctx->cmd.empty());
  ASSERT_FALSE(steam_ctx->prep_cmds.empty());
  EXPECT_EQ(steam_ctx->prep_cmds.back().undo_cmd, expected_steam_shutdown_command());
  EXPECT_TRUE(steam_ctx->env_vars.empty());

  std::filesystem::remove(file_path);
}

#ifdef __linux__
TEST(ProcessMigrationTests, ParseDoesNotAddSteamBigPicturePrelaunchShutdownForLinuxCage) {
  linux_cage_compositor_guard_t guard;
  config::video.linux_display.use_cage_compositor = true;

  const auto file_path = test_paths::root() / "steam_big_picture_cage_prelaunch_shutdown.json";

  const nlohmann::json apps = {
    {"version", 2},
    {"apps", {
      {
        {"name", "Steam Big Picture"},
        {"uuid", "steam-big-picture-cage-prelaunch"},
        {"cmd", ""},
        {"detached", {"setsid steam -gamepadui"}},
        {"prep-cmd", nlohmann::json::array()},
        {"image-path", "./assets/steam.png"}
      }
    }}
  };

  ASSERT_EQ(file_handler::write_file(file_path.string().c_str(), apps.dump(2)), 0);

  auto parsed_proc = proc::parse(file_path.string());
  ASSERT_TRUE(parsed_proc.has_value());

  const auto &parsed_apps = parsed_proc->get_apps();
  const auto steam_ctx = std::find_if(parsed_apps.begin(), parsed_apps.end(), [](const auto &app) {
    return app.name == "Steam Big Picture";
  });

  ASSERT_NE(steam_ctx, parsed_apps.end());
  ASSERT_EQ(steam_ctx->prep_cmds.size(), 1);
  EXPECT_TRUE(steam_ctx->prep_cmds.front().do_cmd.empty());
  EXPECT_EQ(steam_ctx->prep_cmds.front().undo_cmd, expected_steam_shutdown_command());

  std::filesystem::remove(file_path);
}
#endif

TEST(ProcessMigrationTests, ParseStripsSteamBigPictureMangoHudEvenWithExistingCleanupUndo) {
#ifdef __linux__
  linux_cage_compositor_guard_t guard;
  config::video.linux_display.use_cage_compositor = false;
#endif

  const auto file_path = test_paths::root() / "steam_big_picture_existing_cleanup.json";

  const nlohmann::json apps = {
    {"version", 2},
    {"apps", {
      {
        {"name", "Steam Big Picture"},
        {"uuid", "steam-big-picture-existing-cleanup"},
        {"cmd", ""},
        {"detached", {"setsid steam steam://open/bigpicture"}},
        {"prep-cmd", {{
          {"undo", "setsid steam steam://close/bigpicture"}
        }}},
        {"env", {
          {"MANGOHUD", "1"},
          {"MANGOHUD_DLSYM", "1"},
          {"MANGOHUD_CONFIG", "fps_limit=60"}
        }},
        {"image-path", "./assets/steam.png"}
      }
    }}
  };

  ASSERT_EQ(file_handler::write_file(file_path.string().c_str(), apps.dump(2)), 0);

  auto parsed_proc = proc::parse(file_path.string());
  ASSERT_TRUE(parsed_proc.has_value());

  const auto &parsed_apps = parsed_proc->get_apps();
  const auto steam_ctx = std::find_if(parsed_apps.begin(), parsed_apps.end(), [](const auto &app) {
    return app.name == "Steam Big Picture";
  });

  ASSERT_NE(steam_ctx, parsed_apps.end());
  ASSERT_EQ(steam_ctx->prep_cmds.size(), 1);
  EXPECT_EQ(steam_ctx->prep_cmds.front().undo_cmd, expected_steam_shutdown_command());
  EXPECT_TRUE(steam_ctx->env_vars.empty());

  std::filesystem::remove(file_path);
}

TEST(ProcessMigrationTests, ParseNormalizesSteamLibraryLaunchAndAddsShutdownUndo) {
#ifdef __linux__
  linux_cage_compositor_guard_t guard;
  config::video.linux_display.use_cage_compositor = false;
#endif

  const auto file_path = test_paths::root() / "steam_library_launch_normalization.json";

  const nlohmann::json apps = {
    {"version", 2},
    {"apps", {
      {
        {"name", "Indiana Jones and the Great Circle"},
        {"uuid", "steam-library-normalization-test"},
        {"cmd", ""},
        {"detached", {"setsid steam steam://rungameid/2677660"}},
        {"prep-cmd", nlohmann::json::array()},
        {"source", "steam"},
        {"steam-appid", "2677660"},
        {"image-path", "./assets/indiana.png"}
      }
    }}
  };

  ASSERT_EQ(file_handler::write_file(file_path.string().c_str(), apps.dump(2)), 0);

  auto parsed_proc = proc::parse(file_path.string());
  ASSERT_TRUE(parsed_proc.has_value());

  const auto &parsed_apps = parsed_proc->get_apps();
  const auto steam_ctx = std::find_if(parsed_apps.begin(), parsed_apps.end(), [](const auto &app) {
    return app.name == "Indiana Jones and the Great Circle";
  });

  ASSERT_NE(steam_ctx, parsed_apps.end());
  ASSERT_EQ(steam_ctx->detached.size(), 1);
  EXPECT_EQ(steam_ctx->detached.front(), "setsid steam steam://rungameid/2677660");
  ASSERT_FALSE(steam_ctx->prep_cmds.empty());
  EXPECT_EQ(steam_ctx->prep_cmds.back().undo_cmd, expected_steam_shutdown_command());
  EXPECT_EQ(steam_ctx->steam_appid, "2677660");
  EXPECT_EQ(steam_ctx->source, "steam");

  const auto migrated_tree = nlohmann::json::parse(file_handler::read_file(file_path.string().c_str()));
  EXPECT_EQ(migrated_tree["version"], 9);

  std::filesystem::remove(file_path);
}

TEST(ProcessMigrationTests, ParseNormalizesCurrentSteamLibraryLaunchWithoutBigPicture) {
#ifdef __linux__
  linux_cage_compositor_guard_t guard;
  config::video.linux_display.use_cage_compositor = false;
#endif

  const auto file_path = test_paths::root() / "steam_library_current_launch_normalization.json";

  const nlohmann::json apps = {
    {"version", 8},
    {"apps", {
      {
        {"name", "Resident Evil 2"},
        {"uuid", "steam-library-current-normalization-test"},
        {"cmd", ""},
        {"detached", {
          "setsid steam -gamepadui",
          "setsid bash -lc \"sleep 6; steam steam://rungameid/883710 >/dev/null 2>&1 || true; sleep 4; exec steam -applaunch 883710 >/dev/null 2>&1 || true\""
        }},
        {"prep-cmd", {{
          {"undo", "setsid steam -shutdown"}
        }}},
        {"source", "steam"},
        {"steam-appid", "883710"},
        {"image-path", "./assets/resident-evil-2.png"}
      }
    }}
  };

  ASSERT_EQ(file_handler::write_file(file_path.string().c_str(), apps.dump(2)), 0);

  auto parsed_proc = proc::parse(file_path.string());
  ASSERT_TRUE(parsed_proc.has_value());

  const auto &parsed_apps = parsed_proc->get_apps();
  const auto steam_ctx = std::find_if(parsed_apps.begin(), parsed_apps.end(), [](const auto &app) {
    return app.name == "Resident Evil 2";
  });

  ASSERT_NE(steam_ctx, parsed_apps.end());
  ASSERT_EQ(steam_ctx->detached.size(), 1);
  EXPECT_EQ(steam_ctx->detached.front(), "setsid steam steam://rungameid/883710");
  ASSERT_EQ(steam_ctx->prep_cmds.size(), 1);
  EXPECT_EQ(steam_ctx->prep_cmds.front().undo_cmd, expected_steam_shutdown_command());

  const auto parsed_tree = nlohmann::json::parse(file_handler::read_file(file_path.string().c_str()));
  EXPECT_EQ(parsed_tree["version"], 9);

  std::filesystem::remove(file_path);
}

TEST(ProcessMigrationTests, ParseDefaultsSteamLibraryLaunchModeToDirect) {
#ifdef __linux__
  linux_cage_compositor_guard_t guard;
  config::video.linux_display.use_cage_compositor = false;
#endif

  const auto file_path = test_paths::root() / "steam_library_default_direct_launch_mode.json";

  const nlohmann::json apps = {
    {"version", 8},
    {"apps", {
      {
        {"name", "Portal"},
        {"uuid", "steam-library-default-direct-launch-mode"},
        {"cmd", ""},
        {"detached", {
          "setsid steam -gamepadui",
          "setsid bash -lc \"sleep 6; steam steam://rungameid/400 >/dev/null 2>&1 || true; sleep 4; exec steam -applaunch 400 >/dev/null 2>&1 || true\""
        }},
        {"prep-cmd", {{
          {"undo", "setsid steam -shutdown"}
        }}},
        {"source", "steam"},
        {"steam-appid", "400"},
        {"image-path", "./assets/portal.png"}
      }
    }}
  };

  ASSERT_EQ(file_handler::write_file(file_path.string().c_str(), apps.dump(2)), 0);

  auto parsed_proc = proc::parse(file_path.string());
  ASSERT_TRUE(parsed_proc.has_value());

  const auto &parsed_apps = parsed_proc->get_apps();
  const auto steam_ctx = std::find_if(parsed_apps.begin(), parsed_apps.end(), [](const auto &app) {
    return app.name == "Portal";
  });

  ASSERT_NE(steam_ctx, parsed_apps.end());
  EXPECT_EQ(steam_ctx->steam_launch_mode, "direct");
  ASSERT_EQ(steam_ctx->detached.size(), 1);
  EXPECT_EQ(steam_ctx->detached.front(), "setsid steam steam://rungameid/400");

  std::filesystem::remove(file_path);
}

TEST(ProcessMigrationTests, ParseHonorsExplicitSteamLibraryBigPictureLaunchMode) {
#ifdef __linux__
  linux_cage_compositor_guard_t guard;
  config::video.linux_display.use_cage_compositor = false;
#endif

  const auto file_path = test_paths::root() / "steam_library_big_picture_launch_mode.json";

  const nlohmann::json apps = {
    {"version", 8},
    {"apps", {
      {
        {"name", "Resident Evil 2"},
        {"uuid", "steam-library-big-picture-launch-mode"},
        {"cmd", ""},
        {"detached", {"setsid steam steam://rungameid/883710"}},
        {"prep-cmd", {{
          {"undo", "setsid steam -shutdown"}
        }}},
        {"source", "steam"},
        {"steam-appid", "883710"},
        {"steam-launch-mode", "big-picture"},
        {"image-path", "./assets/resident-evil-2.png"}
      }
    }}
  };

  ASSERT_EQ(file_handler::write_file(file_path.string().c_str(), apps.dump(2)), 0);

  auto parsed_proc = proc::parse(file_path.string());
  ASSERT_TRUE(parsed_proc.has_value());

  const auto &parsed_apps = parsed_proc->get_apps();
  const auto steam_ctx = std::find_if(parsed_apps.begin(), parsed_apps.end(), [](const auto &app) {
    return app.name == "Resident Evil 2";
  });

  ASSERT_NE(steam_ctx, parsed_apps.end());
  EXPECT_EQ(steam_ctx->steam_launch_mode, "big-picture");
  ASSERT_EQ(steam_ctx->detached.size(), 2);
  EXPECT_EQ(steam_ctx->detached[0], "setsid steam -gamepadui");
  EXPECT_EQ(
    steam_ctx->detached[1],
    "setsid bash -lc \"sleep 6; steam steam://rungameid/883710 >/dev/null 2>&1 || true; sleep 4; exec steam -applaunch 883710 >/dev/null 2>&1 || true\""
  );

  std::filesystem::remove(file_path);
}

#ifdef __linux__
TEST(ProcessMigrationTests, ParseDoesNotAddSteamLibraryPrelaunchShutdownForLinuxCage) {
  linux_cage_compositor_guard_t guard;
  config::video.linux_display.use_cage_compositor = true;

  const auto file_path = test_paths::root() / "steam_library_cage_prelaunch_shutdown.json";

  const nlohmann::json apps = {
    {"version", 2},
    {"apps", {
      {
        {"name", "Indiana Jones and the Great Circle"},
        {"uuid", "steam-library-cage-prelaunch"},
        {"cmd", ""},
        {"detached", {"setsid steam steam://rungameid/2677660"}},
        {"prep-cmd", nlohmann::json::array()},
        {"source", "steam"},
        {"steam-appid", "2677660"},
        {"image-path", "./assets/indiana.png"}
      }
    }}
  };

  ASSERT_EQ(file_handler::write_file(file_path.string().c_str(), apps.dump(2)), 0);

  auto parsed_proc = proc::parse(file_path.string());
  ASSERT_TRUE(parsed_proc.has_value());

  const auto &parsed_apps = parsed_proc->get_apps();
  const auto steam_ctx = std::find_if(parsed_apps.begin(), parsed_apps.end(), [](const auto &app) {
    return app.name == "Indiana Jones and the Great Circle";
  });

  ASSERT_NE(steam_ctx, parsed_apps.end());
  ASSERT_EQ(steam_ctx->detached.size(), 1);
  EXPECT_EQ(steam_ctx->detached.front(), "setsid steam steam://rungameid/2677660");
  ASSERT_EQ(steam_ctx->prep_cmds.size(), 1);
  EXPECT_TRUE(steam_ctx->prep_cmds.front().do_cmd.empty());
  EXPECT_EQ(steam_ctx->prep_cmds.front().undo_cmd, expected_steam_shutdown_command());

  std::filesystem::remove(file_path);
}

TEST(ProcessMigrationTests, ParsePreservesLutrisImportMetadataAndSource) {
  const auto file_path = test_paths::root() / "lutris_import_metadata.json";

  const nlohmann::json apps = {
    {"version", 8},
    {"apps", {{
      {"name", "Lutris Game"},
      {"uuid", "a4a9a18a-3898-4b34-9e3d-21a7a9647712"},
      {"source", "lutris"},
      {"lutris-slug", "lutris-game"},
      {"lutris-runner", "wine"},
      {"detached", {"setsid lutris lutris:rungame/lutris-game"}},
      {"gamepad", "ds5"},
      {"game-category", "cinematic"},
      {"auto-detach", true},
      {"wait-all", true},
      {"exit-timeout", 5}
    }}}
  };

  ASSERT_EQ(file_handler::write_file(file_path.string().c_str(), apps.dump(2)), 0);

  auto parsed_proc = proc::parse(file_path.string());
  ASSERT_TRUE(parsed_proc.has_value());

  const auto migrated_tree = nlohmann::json::parse(file_handler::read_file(file_path.string().c_str()));
  ASSERT_TRUE(migrated_tree.contains("apps"));
  ASSERT_EQ(migrated_tree["apps"].size(), 1);
  const auto &migrated_app = migrated_tree["apps"][0];
  EXPECT_EQ(migrated_app["source"], "lutris");
  EXPECT_EQ(migrated_app["lutris-slug"], "lutris-game");
  EXPECT_EQ(migrated_app["lutris-runner"], "wine");

  const auto &parsed_apps = parsed_proc->get_apps();
  const auto lutris_ctx = std::find_if(parsed_apps.begin(), parsed_apps.end(), [](const auto &app) {
    return app.name == "Lutris Game";
  });

  ASSERT_NE(lutris_ctx, parsed_apps.end());
  EXPECT_EQ(lutris_ctx->source, "lutris");
  EXPECT_EQ(lutris_ctx->gamepad, "ds5");
  EXPECT_EQ(lutris_ctx->game_category, "cinematic");
  ASSERT_EQ(lutris_ctx->detached.size(), 1);
  EXPECT_EQ(lutris_ctx->detached[0], "setsid lutris lutris:rungame/lutris-game");
}

TEST(ProcessMigrationTests, ParseAddsLutrisLauncherWhenLutrisGamesExist) {
  const auto file_path = test_paths::root() / "lutris_launcher_migration.json";

  const nlohmann::json apps = {
    {"version", 7},
    {"apps", {{
      {"name", "Black Myth: Wukong"},
      {"uuid", "45cb1d9d-90d3-6023-0800-457901181759"},
      {"source", "lutris"},
      {"lutris-slug", "black-myth-wukong"},
      {"detached", {"setsid lutris lutris:rungame/black-myth-wukong"}},
      {"auto-detach", true},
      {"wait-all", true},
      {"exit-timeout", 5}
    }}}
  };

  ASSERT_EQ(file_handler::write_file(file_path.string().c_str(), apps.dump(2)), 0);

  auto parsed_proc = proc::parse(file_path.string());
  ASSERT_TRUE(parsed_proc.has_value());

  const auto migrated_tree = nlohmann::json::parse(file_handler::read_file(file_path.string().c_str()));
  EXPECT_EQ(migrated_tree["version"], 9);
  ASSERT_TRUE(migrated_tree.contains("apps"));

  const auto &migrated_apps = migrated_tree["apps"];
  const auto lutris_app = std::find_if(migrated_apps.begin(), migrated_apps.end(), [](const auto &app) {
    return app.value("name", "") == "Lutris";
  });

  ASSERT_NE(lutris_app, migrated_apps.end());
  EXPECT_EQ((*lutris_app)["source"], "lutris");
  EXPECT_EQ((*lutris_app)["image-path"], "lutris.png");
  ASSERT_TRUE((*lutris_app).contains("detached"));
  ASSERT_EQ((*lutris_app)["detached"].size(), 1);
  EXPECT_EQ((*lutris_app)["detached"][0], "setsid lutris");

  const auto &parsed_apps = parsed_proc->get_apps();
  const auto parsed_lutris = std::find_if(parsed_apps.begin(), parsed_apps.end(), [](const auto &app) {
    return app.name == "Lutris";
  });

  ASSERT_NE(parsed_lutris, parsed_apps.end());
  ASSERT_EQ(parsed_lutris->detached.size(), 1);
  EXPECT_EQ(parsed_lutris->detached[0], "setsid lutris");

  std::filesystem::remove(file_path);
}

// SB-5: polaris-gamescope-session hardwire unwraps to mode-neutral steam-appid launches.
// Big Picture may keep the session helper; library games must not.
TEST(ProcessMigrationTests, ParseUnwrapsPolarisHdrSessionLibraryHardwire) {
  const auto file_path = test_paths::root() / "sb5_unwrap_hdr_session.json";

  const nlohmann::json apps = {
    {"version", 8},
    {"apps", {
      {
        {"name", "ARMORED CORE VI"},
        {"uuid", "5D6418F1-9D9A-DC0B-FE8A-8A70B7DA88CB"},
        {"cmd", "/nix/store/fake-polaris-gamescope-session/bin/polaris-gamescope-session wait"},
        {"auto-detach", false},
        {"wait-all", true},
        {"source", "polaris-gamescope-session"},
        {"prep-cmd", {{
          {"do", "/nix/store/fake-polaris-gamescope-session/bin/polaris-gamescope-session start 1888160"},
          {"undo", "/nix/store/fake-polaris-gamescope-session/bin/polaris-gamescope-session stop"}
        }}}
      },
      {
        {"name", "Steam Big Picture"},
        {"uuid", "282A8EBA-DA08-6226-D08E-AEB756AEEF04"},
        {"cmd", "/nix/store/fake-polaris-gamescope-session/bin/polaris-gamescope-session wait"},
        {"auto-detach", false},
        {"prep-cmd", {{
          {"do", "/nix/store/fake-polaris-gamescope-session/bin/polaris-gamescope-session start"},
          {"undo", "/nix/store/fake-polaris-gamescope-session/bin/polaris-gamescope-session stop"}
        }}}
      }
    }}
  };

  ASSERT_EQ(file_handler::write_file(file_path.string().c_str(), apps.dump(2)), 0);

  auto parsed_proc = proc::parse(file_path.string());
  ASSERT_TRUE(parsed_proc.has_value());

  const auto migrated_tree = nlohmann::json::parse(file_handler::read_file(file_path.string().c_str()));
  EXPECT_EQ(migrated_tree["version"], 9);

  const auto &migrated_apps = migrated_tree["apps"];
  const auto lib_app = std::find_if(migrated_apps.begin(), migrated_apps.end(), [](const auto &app) {
    return app.value("name", "") == "ARMORED CORE VI";
  });
  ASSERT_NE(lib_app, migrated_apps.end());
  EXPECT_EQ((*lib_app)["source"], "steam");
  EXPECT_EQ((*lib_app)["steam-appid"], "1888160");
  ASSERT_TRUE((*lib_app).contains("detached"));
  ASSERT_EQ((*lib_app)["detached"].size(), 1);
  // No setsid: the prefix is carried over from the command being replaced, and
  // the polaris-gamescope-session wrapper this fixture unwraps has none.
  EXPECT_EQ((*lib_app)["detached"][0], "steam steam://rungameid/1888160");
  EXPECT_TRUE((*lib_app).value("cmd", "").empty() || (*lib_app)["cmd"] == "");

  const auto bp_app = std::find_if(migrated_apps.begin(), migrated_apps.end(), [](const auto &app) {
    return app.value("name", "") == "Steam Big Picture";
  });
  ASSERT_NE(bp_app, migrated_apps.end());
  // Optional BP entry may keep polaris-gamescope-session shell.
  EXPECT_NE(bp_app->value("cmd", "").find("polaris-gamescope-session"), std::string::npos);

  const auto &parsed_apps = parsed_proc->get_apps();
  const auto lib_ctx = std::find_if(parsed_apps.begin(), parsed_apps.end(), [](const auto &app) {
    return app.name == "ARMORED CORE VI";
  });
  ASSERT_NE(lib_ctx, parsed_apps.end());
  EXPECT_EQ(lib_ctx->steam_appid, "1888160");
  EXPECT_EQ(lib_ctx->source, "steam");
  ASSERT_EQ(lib_ctx->detached.size(), 1);
  EXPECT_EQ(lib_ctx->detached.front(), "steam steam://rungameid/1888160");
  EXPECT_TRUE(lib_ctx->cmd.empty());
  // No polaris-gamescope-session left in prep for library game.
  EXPECT_TRUE(std::none_of(lib_ctx->prep_cmds.begin(), lib_ctx->prep_cmds.end(), [](const auto &cmd) {
    return cmd.do_cmd.find("polaris-gamescope-session") != std::string::npos ||
           cmd.undo_cmd.find("polaris-gamescope-session") != std::string::npos;
  }));

  std::filesystem::remove(file_path);
}
#endif
