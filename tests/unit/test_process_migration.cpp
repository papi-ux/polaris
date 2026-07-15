#include "../tests_common.h"
#include "../tests_paths.h"

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <src/adaptive_bitrate.h>
#include <src/ai_optimizer.h>
#include <src/file_handler.h>
#include <src/nvhttp.h>
#include <src/process.h>
#include <src/stream_stats.h>

namespace {
#ifdef __linux__
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
  EXPECT_NE(
    cancel_handler.find(
      "request_session_shutdown(\n"
      "      named_cert_p->uuid,\n"
      "      session_token,\n"
      "      true,\n"
      "      !session_token.empty()\n"
      "    )"
    ),
    std::string::npos
  );
  EXPECT_NE(
    stop_handler.find(
      "request_session_shutdown(\n"
      "        named_cert_p->uuid,\n"
      "        expected_token,\n"
      "        can_launch,\n"
      "        true\n"
      "      )"
    ),
    std::string::npos
  );
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
  const auto stop_cage = terminate.find("cage_display_router::stop()");
  ASSERT_NE(stop_guard, std::string::npos);
  ASSERT_NE(stop_cage, std::string::npos);
  EXPECT_LT(stop_guard, stop_cage);
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
  EXPECT_EQ(migrated_tree["version"], 8);
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
  EXPECT_EQ(migrated_tree["version"], 8);

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
  EXPECT_EQ(parsed_tree["version"], 8);

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
  EXPECT_EQ(migrated_tree["version"], 8);
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
#endif
