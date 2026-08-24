/**
 * @file tests/unit/test_source_safety_contracts.cpp
 * @brief Tree-wide invariants behind two bug classes this codebase has hit.
 *
 * Both were consistency failures rather than gaps in knowledge: the correct
 * helper already existed a few hundred lines away and one call site simply did
 * not reach for it. A review pass finds those once; a contract finds them every
 * time, including in code written later.
 *
 * These scan the whole of src/ rather than named files on purpose. A contract
 * that lists the files it knows about only ever covers the bugs already found.
 */
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

  namespace fs = std::filesystem;

  struct source_file_t {
    std::string relative_path;
    std::string text;
  };

  std::vector<source_file_t> all_sources() {
    std::vector<source_file_t> sources;
    const fs::path root = fs::path {POLARIS_SOURCE_DIR} / "src";
    for (const auto &entry : fs::recursive_directory_iterator(root)) {
      if (!entry.is_regular_file()) {
        continue;
      }
      const auto extension = entry.path().extension().string();
      if (extension != ".cpp" && extension != ".h") {
        continue;
      }
      std::ifstream input(entry.path());
      std::ostringstream contents;
      contents << input.rdbuf();
      sources.push_back({fs::relative(entry.path(), fs::path {POLARIS_SOURCE_DIR}).generic_string(), contents.str()});
    }
    return sources;
  }

  std::size_t line_of(const std::string &text, std::size_t offset) {
    return static_cast<std::size_t>(std::count(text.begin(), text.begin() + offset, '\n')) + 1;
  }

  /**
   * @brief Whether @p offset sits inside a double-quoted literal on its line.
   *
   * Log messages quote the names of the very things these contracts look for --
   * kmsgrab prints a `$(readlink -f ...)` setcap hint -- and a contract that
   * flags prose gets suppressed rather than obeyed.
   */
  bool inside_string_literal(const std::string &text, std::size_t offset) {
    const auto line_start = text.rfind('\n', offset);
    std::size_t cursor = line_start == std::string::npos ? 0 : line_start + 1;
    bool quoted = false;
    for (; cursor < offset; ++cursor) {
      if (text[cursor] == '\\') {
        ++cursor;
        continue;
      }
      if (text[cursor] == '"') {
        quoted = !quoted;
      }
    }
    return quoted;
  }

  /**
   * @brief Whether @p offset sits inside a line or block comment.
   *
   * Contracts scan source text rather than the compiler AST, so comments that
   * describe a guarded call must not be treated as executable code.
   */
  bool inside_comment(const std::string &text, std::size_t offset) {
    const auto line_start = text.rfind('\n', offset);
    const auto line_comment = text.find("//", line_start == std::string::npos ? 0 : line_start + 1);
    if (line_comment != std::string::npos && line_comment < offset) {
      return true;
    }
    const auto block_open = text.rfind("/*", offset);
    const auto block_close = text.rfind("*/", offset);
    return block_open != std::string::npos &&
           (block_close == std::string::npos || block_open > block_close);
  }

  /// Text between the parenthesis at @p open and its match, exclusive.
  std::string argument_expression(const std::string &text, std::size_t open) {
    std::size_t depth = 1;
    std::size_t cursor = open + 1;
    for (; cursor < text.size() && depth > 0; ++cursor) {
      if (text[cursor] == '(') {
        ++depth;
      } else if (text[cursor] == ')') {
        --depth;
      }
    }
    return text.substr(open + 1, cursor - open - 2);
  }

}  // namespace

TEST(SourceSafetyContracts, ShellCommandsBuiltByConcatenationAreEscaped) {
  // A config value reaching std::system unescaped turned "can edit settings"
  // into "can run commands". The fix was to escape; this keeps the next
  // concatenated shell call from being written without one.
  static const std::regex shell_call {R"((std::system|(^|[^\w_])popen)\s*\()"};

  std::vector<std::string> unescaped;
  for (const auto &source : all_sources()) {
    for (auto it = std::sregex_iterator(source.text.begin(), source.text.end(), shell_call);
         it != std::sregex_iterator();
         ++it) {
      const auto open = source.text.find('(', static_cast<std::size_t>(it->position()));
      const auto expression = argument_expression(source.text, open);

      // Only concatenation can smuggle a value in; a bare literal cannot.
      if (expression.find('+') == std::string::npos) {
        continue;
      }
      if (expression.find("shell_escape") != std::string::npos ||
          expression.find("shell_quote") != std::string::npos ||
          expression.find("url_escape") != std::string::npos) {
        continue;
      }

      // Some call sites escape where the string is built rather than where it
      // is run, which no source-level check can see. Those carry a marker
      // naming where the escaping happened, so adding one costs a written
      // justification at the site instead of a silent entry in a list here.
      const auto preamble_start = open < 400 ? 0 : open - 400;
      const auto preamble = source.text.substr(preamble_start, open - preamble_start);
      if (preamble.find("shell-escape-checked") != std::string::npos) {
        continue;
      }

      unescaped.push_back(source.relative_path + ":" + std::to_string(line_of(source.text, open)));
    }
  }

  // Every concatenated shell call escapes. A new one lands here rather than in
  // an audit, and the fix is to escape it -- not to extend this list.
  EXPECT_TRUE(unescaped.empty()) << [&] {
    std::ostringstream out;
    out << "concatenated shell call(s) without an escape helper:";
    for (const auto &site : unescaped) {
      out << "\n  " << site;
    }
    return out.str();
  }();
}

TEST(SourceSafetyContracts, PackageFortifyFilterFollowsTheGnuCompilerIdentity) {
  for (const auto *relative_path : {
         "packaging/linux/Arch/PKGBUILD",
         "packaging/linux/SteamOS/PKGBUILD"
       }) {
    std::ifstream input(fs::path {POLARIS_SOURCE_DIR} / relative_path);
    ASSERT_TRUE(input.is_open()) << relative_path;
    std::ostringstream contents;
    contents << input.rdbuf();
    const auto text = contents.str();

    EXPECT_NE(text.find("-dM -E -x c++ /dev/null"), std::string::npos) << relative_path;
    EXPECT_NE(text.find("#define __GNUC__"), std::string::npos) << relative_path;
    EXPECT_NE(text.find("#define __clang__"), std::string::npos) << relative_path;
    EXPECT_EQ(text.find("-dumpfullversion"), std::string::npos) << relative_path;
  }
}

TEST(SourceSafetyContracts, GamescopePreviewConsumesBestEffortRepaintResult) {
  std::ifstream input(fs::path {POLARIS_SOURCE_DIR} / "src/confighttp.cpp");
  ASSERT_TRUE(input.is_open());
  std::ostringstream contents;
  contents << input.rdbuf();
  const auto text = contents.str();
  const auto command = text.find("gamescopectl debug_force_repaint");
  ASSERT_NE(command, std::string::npos);
  const auto window_start = command > 300 ? command - 300 : 0;
  const auto window = text.substr(window_start, 700);

  EXPECT_NE(window.find("const int repaint_result = std::system("), std::string::npos);
  EXPECT_NE(window.find("if (repaint_result != 0)"), std::string::npos);
  EXPECT_NE(window.find("BOOST_LOG(debug)"), std::string::npos);
}

TEST(SourceSafetyContracts, DoctorSteamVdfMutationAndUndoAreReleaseReadOnly) {
  const auto read = [](const fs::path &path) {
    std::ifstream input(path);
    std::ostringstream out;
    out << input.rdbuf();
    return out.str();
  };
  const auto root = fs::path {POLARIS_SOURCE_DIR};
  const auto doctor = read(root / "src/doctor_actions.cpp");
  const auto stream = read(root / "src/stream_stats.cpp");

  const auto apply = doctor.find("if (action_id == \"disable_steam_input_xbox\"");
  const auto apply_guard = doctor.find("return steam_vdf_read_only_response()", apply);
  const auto shutdown = doctor.find("ensure_steam_client_quiescent_for_doctor()", apply);
  ASSERT_NE(apply, std::string::npos);
  ASSERT_NE(apply_guard, std::string::npos);
  ASSERT_NE(shutdown, std::string::npos);
  EXPECT_LT(apply_guard, shutdown);

  const auto undo = doctor.find("if (action_run.kind == action_kind_e::disable_steam_input_xbox)");
  const auto undo_guard = doctor.find("return steam_vdf_read_only_response()", undo);
  const auto restore = doctor.find("rewrite_steam_profile(edit.path", undo);
  ASSERT_NE(undo, std::string::npos);
  ASSERT_NE(undo_guard, std::string::npos);
  ASSERT_NE(restore, std::string::npos);
  EXPECT_LT(undo_guard, restore);

  const auto builder = stream.find("nlohmann::json doctor_safe_action(");
  const auto branch = stream.find("else if (primary_issue == \"steam_input_conflict\")", builder);
  const auto branch_end = stream.find("} else if", branch + 1);
  ASSERT_NE(builder, std::string::npos);
  ASSERT_NE(branch, std::string::npos);
  ASSERT_NE(branch_end, std::string::npos);
  const auto action = stream.substr(branch, branch_end - branch);
  EXPECT_NE(action.find("id = \"none\""), std::string::npos);
  EXPECT_NE(action.find("kind = \"manual_guidance\""), std::string::npos);
  EXPECT_EQ(action.find("disable_steam_input_xbox"), std::string::npos);
}

TEST(SourceSafetyContracts, UbuntuSnapshotInstallsMayDowngradeRunnerPackages) {
  std::ifstream input(fs::path {POLARIS_SOURCE_DIR} / ".github/workflows/build.yml");
  ASSERT_TRUE(input.good());
  std::ostringstream contents;
  contents << input.rdbuf();
  const auto workflow = contents.str();
  ASSERT_FALSE(workflow.empty());

  const auto count_exact = [](std::string_view haystack, std::string_view needle) {
    std::size_t count = 0;
    for (std::size_t offset = 0;
         (offset = haystack.find(needle, offset)) != std::string_view::npos;
         offset += needle.size()) {
      ++count;
    }
    return count;
  };

  constexpr std::string_view unprotected_snapshot_install = R"(sudo apt-get "${apt_options[@]}" install -y \)";
  constexpr std::string_view protected_snapshot_install = R"(sudo apt-get "${apt_options[@]}" install -y --allow-downgrades \)";

  constexpr std::string_view curl_snapshot_lookup = "madison libcurl4-openssl-dev";
  constexpr std::string_view curl_runtime_pin = R"("libcurl4t64=$curl_snapshot_version")";
  constexpr std::string_view curl_dev_pin = R"("libcurl4-openssl-dev=$curl_snapshot_version")";

  EXPECT_EQ(count_exact(workflow, unprotected_snapshot_install), 0u);
  EXPECT_EQ(count_exact(workflow, protected_snapshot_install), 2u);
  EXPECT_EQ(count_exact(workflow, curl_snapshot_lookup), 2u);
  EXPECT_EQ(count_exact(workflow, curl_runtime_pin), 2u);
  EXPECT_EQ(count_exact(workflow, curl_dev_pin), 2u);
}

TEST(SourceSafetyContracts, LegacyVirtualDisplayPromotionPrecedesCaptureReevaluationAndRestoresHostState) {
  std::ifstream input(fs::path {POLARIS_SOURCE_DIR} / "src/process.cpp");
  ASSERT_TRUE(input.is_open());
  std::ostringstream contents;
  contents << input.rdbuf();
  const auto source = contents.str();

  const auto execute = source.find("int proc_t::execute_impl(");
  const auto terminate = source.find("void proc_t::terminate_impl(", execute);
  ASSERT_NE(execute, std::string::npos);
  ASSERT_NE(terminate, std::string::npos);
  const auto launch = source.substr(execute, terminate - execute);

  const auto derive = launch.find("effective_session_selection_for_launch(");
  const auto apply = launch.find("stream_display_policy::apply_selection(session_mode");
  const auto reevaluate = launch.find("platf::reevaluate_capture_sources()", apply);
  ASSERT_NE(derive, std::string::npos);
  ASSERT_NE(apply, std::string::npos);
  ASSERT_NE(reevaluate, std::string::npos);
  EXPECT_LT(derive, apply);
  EXPECT_LT(apply, reevaluate);
  EXPECT_NE(launch.find("_app.virtual_display", derive), std::string::npos);
  EXPECT_NE(launch.find("launch_session->user_locked_virtual_display", derive), std::string::npos);
  EXPECT_EQ(
    launch.find("!(launch_session && launch_session->mirror_desktop)", derive),
    std::string::npos
  ) << "derived desktop_display must actually be applied for mirror sessions";

  const auto terminate_end = source.find("bool proc_t::reload_configuration_from_file", terminate);
  ASSERT_NE(terminate_end, std::string::npos);
  const auto teardown = source.substr(terminate, terminate_end - terminate);
  EXPECT_NE(teardown.find("linux_display.stream_mode = initial_stream_mode"), std::string::npos);
  EXPECT_NE(teardown.find("config::video.capture = initial_capture"), std::string::npos);
}

TEST(SourceSafetyContracts, LinuxVirtualDisplayCreationUsesOnlyTheEffectiveMode) {
  std::ifstream input(fs::path {POLARIS_SOURCE_DIR} / "src/process.cpp");
  ASSERT_TRUE(input.is_open());
  std::ostringstream contents;
  contents << input.rdbuf();
  const auto source = contents.str();

  const auto decision = source.find("const bool should_use_linux_virtual_display");
  const auto launch = source.find("if (", decision);
  ASSERT_NE(decision, std::string::npos);
  ASSERT_NE(launch, std::string::npos);
  const auto body = source.substr(decision, launch - decision);

  EXPECT_NE(body.find("display_policy.use_host_virtual_display"), std::string::npos);
  EXPECT_EQ(body.find("launch_session->virtual_display"), std::string::npos);
  EXPECT_EQ(body.find("_app.virtual_display"), std::string::npos);
}

TEST(SourceSafetyContracts, VirtualDisplayCreatedNameSurvivesMappingFailureBeforeCapture) {
  std::ifstream input(fs::path {POLARIS_SOURCE_DIR} / "src/process.cpp");
  ASSERT_TRUE(input.is_open());
  std::ostringstream contents;
  contents << input.rdbuf();
  const auto source = contents.str();

  const auto created = source.find("Virtual Display created: ");
  const auto terminate = source.find("void proc_t::terminate_impl(", created);
  ASSERT_NE(created, std::string::npos);
  ASSERT_NE(terminate, std::string::npos);
  const auto launch = source.substr(created, terminate - created);

  const auto map = launch.find("display_device::map_display_name(this->display_name)");
  const auto preserve = launch.find("capture_output_name_for_virtual_display(", map);
  ASSERT_NE(map, std::string::npos);
  ASSERT_NE(preserve, std::string::npos);
  EXPECT_LT(map, preserve);
  EXPECT_NE(launch.find("this->display_name", preserve), std::string::npos);
}

TEST(SourceSafetyContracts, ExactDisplayCaptureCannotFallBackDuringInitOrReinit) {
  std::ifstream input(fs::path {POLARIS_SOURCE_DIR} / "src/video.cpp");
  ASSERT_TRUE(input.is_open());
  std::ostringstream contents;
  contents << input.rdbuf();
  const auto source = contents.str();

  const auto initial = source.find("const auto requested_display_name = proc::proc.display_name;");
  const auto initial_end = source.find("display_wp = disp", initial);
  ASSERT_NE(initial, std::string::npos);
  ASSERT_NE(initial_end, std::string::npos);
  const auto initial_body = source.substr(initial, initial_end - initial);
  const auto initial_policy = initial_body.find("capture_fallback_allowed(");
  const auto initial_reject = initial_body.find("return;", initial_policy);
  const auto initial_refresh = initial_body.find("refresh_displays(");
  ASSERT_NE(initial_policy, std::string::npos);
  ASSERT_NE(initial_reject, std::string::npos);
  ASSERT_NE(initial_refresh, std::string::npos);
  EXPECT_LT(initial_policy, initial_reject);
  EXPECT_LT(initial_reject, initial_refresh);

  const auto reinit = source.find("while (capture_ctx_queue->running())", initial_end);
  const auto reinit_end = source.find("display_wp = disp", reinit);
  ASSERT_NE(reinit, std::string::npos);
  ASSERT_NE(reinit_end, std::string::npos);
  const auto reinit_body = source.substr(reinit, reinit_end - reinit);
  const auto exact_name = reinit_body.find("const auto exact_display_name = proc::proc.display_name");
  const auto exact_reset = reinit_body.find("reset_display(", exact_name);
  const auto identity = reinit_body.find("exact_display_name", exact_reset);
  const auto reinit_reject = reinit_body.find("return;", identity);
  const auto fallback_refresh = reinit_body.find("refresh_displays(", reinit_reject);
  ASSERT_NE(exact_name, std::string::npos);
  ASSERT_NE(exact_reset, std::string::npos);
  ASSERT_NE(identity, std::string::npos);
  ASSERT_NE(reinit_reject, std::string::npos);
  ASSERT_NE(fallback_refresh, std::string::npos);
  EXPECT_LT(exact_name, exact_reset);
  EXPECT_LT(exact_reset, identity);
  EXPECT_LT(identity, reinit_reject);
  EXPECT_LT(reinit_reject, fallback_refresh);

  const auto generic_refresh = source.find(
    "void refresh_displays(platf::mem_type_e dev_type, std::vector<std::string> &display_names, int &current_display_index)"
  );
  const auto capture_thread = source.find("void captureThread(", generic_refresh);
  ASSERT_NE(generic_refresh, std::string::npos);
  ASSERT_NE(capture_thread, std::string::npos);
  const auto wrapper = source.substr(generic_refresh, capture_thread - generic_refresh);
  EXPECT_NE(wrapper.find("if (!refresh_displays("), std::string::npos);
  EXPECT_NE(
    wrapper.find("current_display_index = display_names.empty() ? -1 : 0"),
    std::string::npos
  );
}

TEST(SourceSafetyContracts, RequiredKwinVirtualCaptureCannotFallThroughToAnotherOutputOrPortal) {
  std::ifstream kwin_input(fs::path {POLARIS_SOURCE_DIR} / "src/platform/linux/kwingrab.cpp");
  ASSERT_TRUE(kwin_input.is_open());
  std::ostringstream kwin_contents;
  kwin_contents << kwin_input.rdbuf();
  const auto kwin = kwin_contents.str();

  const auto requested = kwin.find("if (!output_name.empty())");
  const auto exact_policy = kwin.find("output_selection_can_fallback(output_name)", requested);
  const auto reject = kwin.find("return -1;", exact_policy);
  const auto configured_fallback = kwin.find("config::video.linux_display.streaming_output", requested);
  ASSERT_NE(requested, std::string::npos);
  ASSERT_NE(exact_policy, std::string::npos);
  ASSERT_NE(reject, std::string::npos);
  ASSERT_NE(configured_fallback, std::string::npos);
  EXPECT_LT(exact_policy, reject);
  EXPECT_LT(reject, configured_fallback);

  std::ifstream portal_input(fs::path {POLARIS_SOURCE_DIR} / "src/platform/linux/portal_grab.cpp");
  ASSERT_TRUE(portal_input.is_open());
  std::ostringstream portal_contents;
  portal_contents << portal_input.rdbuf();
  const auto portal = portal_contents.str();

  const auto compatible = portal.find("const auto compatible");
  const auto identity = portal.find("capture_identity_matches(", compatible);
  const auto identity_mode = portal.find("g_media.stream_mode", identity);
  const auto identity_output = portal.find("g_media.output_name", identity_mode);
  const auto requested_mode = portal.find("requested_stream_mode", identity_output);
  const auto requested_output = portal.find("requested_output_name", requested_mode);
  const auto reuse = portal.find("return g_media.capture;", requested_output);
  const auto no_wayland = portal.find("#ifndef POLARIS_BUILD_WAYLAND", reuse);
  const auto no_wayland_mode = portal.find("requested_stream_mode == \"host_virtual_display\"", no_wayland);
  const auto no_wayland_reject = portal.find("return nullptr;", no_wayland_mode);
  const auto wayland_guard = portal.find("#ifdef POLARIS_BUILD_WAYLAND", no_wayland_reject);
  const auto kwin_start = portal.find("kwingrab::start_output_session(", wayland_guard);
  const auto required = portal.find("kwingrab::require_for_current_stream_mode()", kwin_start);
  const auto fail_closed = portal.find("return nullptr;", required);
  const auto generic_portal = portal.find("ensure_session_unlocked()", kwin_start);
  ASSERT_NE(compatible, std::string::npos);
  ASSERT_NE(identity, std::string::npos);
  ASSERT_NE(identity_mode, std::string::npos);
  ASSERT_NE(identity_output, std::string::npos);
  ASSERT_NE(requested_mode, std::string::npos);
  ASSERT_NE(requested_output, std::string::npos);
  ASSERT_NE(reuse, std::string::npos);
  ASSERT_NE(no_wayland, std::string::npos);
  ASSERT_NE(no_wayland_mode, std::string::npos);
  ASSERT_NE(no_wayland_reject, std::string::npos);
  ASSERT_NE(wayland_guard, std::string::npos);
  ASSERT_NE(kwin_start, std::string::npos);
  ASSERT_NE(required, std::string::npos);
  ASSERT_NE(fail_closed, std::string::npos);
  ASSERT_NE(generic_portal, std::string::npos);
  EXPECT_LT(compatible, identity);
  EXPECT_LT(identity, identity_mode);
  EXPECT_LT(identity_mode, identity_output);
  EXPECT_LT(identity_output, requested_mode);
  EXPECT_LT(requested_mode, requested_output);
  EXPECT_LT(requested_output, reuse);
  EXPECT_LT(reuse, no_wayland);
  EXPECT_LT(no_wayland, no_wayland_mode);
  EXPECT_LT(no_wayland_mode, no_wayland_reject);
  EXPECT_LT(no_wayland_reject, wayland_guard);
  EXPECT_LT(wayland_guard, kwin_start);
  EXPECT_LT(required, fail_closed);
  EXPECT_LT(fail_closed, generic_portal);
}

TEST(SourceSafetyContracts, PortalSourceSelectionAndIdentityTransitionOwnOneGeneration) {
  std::ifstream portal_input(fs::path {POLARIS_SOURCE_DIR} / "src/platform/linux/portal_grab.cpp");
  ASSERT_TRUE(portal_input.is_open());
  std::ostringstream portal_contents;
  portal_contents << portal_input.rdbuf();
  const auto portal = portal_contents.str();

  EXPECT_EQ(portal.find("static bool ensure_global_session()"), std::string::npos);
  const auto init = portal.find("if (!cage_configured)");
  const auto init_capture = portal.find("ensure_global_capture(", init);
  ASSERT_NE(init, std::string::npos);
  ASSERT_NE(init_capture, std::string::npos);
  EXPECT_EQ(portal.substr(init, init_capture - init).find("ensure_global_session()"), std::string::npos);

  const auto capture_fallback = portal.find("// Fallback: source-owned portal/KWin capture");
  const auto capture_owner = portal.find("ensure_global_capture(", capture_fallback);
  ASSERT_NE(capture_fallback, std::string::npos);
  ASSERT_NE(capture_owner, std::string::npos);
  EXPECT_EQ(portal.substr(capture_fallback, capture_owner - capture_fallback).find("ensure_global_session()"), std::string::npos);

  const auto transition = portal.find("capture configuration changed");
  const auto retired_portal = portal.find("auto retired_portal = std::move(g_media.portal)", transition);
  const auto unlock = portal.find("lock.unlock()", transition);
  const auto destroy_portal = portal.find("retired_portal.reset()", unlock);
  const auto relock = portal.find("lock.lock()", destroy_portal);
  ASSERT_NE(transition, std::string::npos);
  ASSERT_NE(retired_portal, std::string::npos);
  ASSERT_NE(unlock, std::string::npos);
  ASSERT_NE(destroy_portal, std::string::npos);
  ASSERT_NE(relock, std::string::npos);
  EXPECT_LT(retired_portal, unlock);
  EXPECT_LT(unlock, destroy_portal);
  EXPECT_LT(destroy_portal, relock);
}

TEST(SourceSafetyContracts, VirtualDisplayCreationPublishesOnlyProvenOwnedConnectors) {
  std::ifstream input(fs::path {POLARIS_SOURCE_DIR} / "src/platform/linux/virtual_display.cpp");
  ASSERT_TRUE(input.is_open());
  std::ostringstream contents;
  contents << input.rdbuf();
  const auto source = contents.str();

  const auto evdi_proof = source.find("if (!evdi_output_name_is_proven(output_name))");
  const auto evdi_disconnect = source.find("fn_disconnect(handle)", evdi_proof);
  const auto evdi_close = source.find("fn_close(handle)", evdi_disconnect);
  const auto evdi_reject = source.find("return std::nullopt;", evdi_close);
  const auto evdi_publish = source.find("vdisplay_t display", evdi_reject);
  ASSERT_NE(evdi_proof, std::string::npos);
  ASSERT_NE(evdi_disconnect, std::string::npos);
  ASSERT_NE(evdi_close, std::string::npos);
  ASSERT_NE(evdi_reject, std::string::npos);
  ASSERT_NE(evdi_publish, std::string::npos);
  EXPECT_LT(evdi_proof, evdi_disconnect);
  EXPECT_LT(evdi_disconnect, evdi_close);
  EXPECT_LT(evdi_close, evdi_reject);
  EXPECT_LT(evdi_reject, evdi_publish);

  const auto sway = source.find("else if (compositor == \"sway\")");
  const auto before = source.find("sway_outputs_before", sway);
  const auto snapshot_gate = source.find("if (!with_valid_sway_before_snapshot(", before);
  const auto create = source.find("swaymsg -r create_output", snapshot_gate);
  const auto command_proof = source.find("sway_create_output_succeeded(", create);
  const auto ownership = source.find("sway_new_headless_output(", command_proof);
  const auto sway_reject = source.find("return std::nullopt;", ownership);
  const auto mode = source.find("std::string mode_str", sway_reject);
  ASSERT_NE(sway, std::string::npos);
  ASSERT_NE(before, std::string::npos);
  ASSERT_NE(snapshot_gate, std::string::npos);
  ASSERT_NE(create, std::string::npos);
  ASSERT_NE(command_proof, std::string::npos);
  ASSERT_NE(ownership, std::string::npos);
  ASSERT_NE(sway_reject, std::string::npos);
  ASSERT_NE(mode, std::string::npos);
  EXPECT_LT(before, snapshot_gate);
  EXPECT_LT(snapshot_gate, create);
  EXPECT_LT(create, command_proof);
  EXPECT_LT(command_proof, ownership);
  EXPECT_LT(ownership, sway_reject);
  EXPECT_LT(sway_reject, mode);
}

TEST(SourceSafetyContracts, VirtualDisplayCreateSerializesCleanupCreationAndPersistence) {
  std::ifstream input(fs::path {POLARIS_SOURCE_DIR} / "src/platform/linux/virtual_display.cpp");
  ASSERT_TRUE(input.is_open());
  std::ostringstream contents;
  contents << input.rdbuf();
  const auto source = contents.str();

  const auto mutex = source.find("static std::mutex creation_mutex");
  const auto cleanup_entry = source.find("bool cleanup_stale()");
  const auto cleanup_end = source.find("#ifdef POLARIS_TESTS", cleanup_entry);
  const auto cleanup_body = source.substr(cleanup_entry, cleanup_end - cleanup_entry);
  const auto cleanup_lock = cleanup_body.find("std::lock_guard creation_lock {creation_mutex}");
  const auto create = source.find("std::optional<vdisplay_t> create(int width, int height, int fps)", cleanup_entry);
  const auto create_end = source.find("void destroy(vdisplay_t &display)", create);
  const auto destroy_lock = source.find("std::lock_guard creation_lock {creation_mutex}", create_end);
  ASSERT_NE(mutex, std::string::npos);
  ASSERT_NE(cleanup_entry, std::string::npos);
  ASSERT_NE(cleanup_end, std::string::npos);
  ASSERT_NE(cleanup_lock, std::string::npos);
  ASSERT_NE(create, std::string::npos);
  ASSERT_NE(create_end, std::string::npos);
  ASSERT_NE(destroy_lock, std::string::npos);
  const auto body = source.substr(create, create_end - create);
  const auto lock = body.find("std::lock_guard creation_lock {creation_mutex}");
  const auto cleanup = body.find("cleanup_stale_unlocked()");
  const auto backend = body.find("detect_backend()");
  const auto persistence = body.find("record_persisted_display(");
  ASSERT_NE(lock, std::string::npos);
  ASSERT_NE(cleanup, std::string::npos);
  ASSERT_NE(backend, std::string::npos);
  ASSERT_NE(persistence, std::string::npos);
  EXPECT_LT(lock, cleanup);
  EXPECT_LT(cleanup, backend);
  EXPECT_LT(backend, persistence);

  std::ifstream process_input(fs::path {POLARIS_SOURCE_DIR} / "src/process.cpp");
  std::ifstream confighttp_input(fs::path {POLARIS_SOURCE_DIR} / "src/confighttp.cpp");
  ASSERT_TRUE(process_input.is_open());
  ASSERT_TRUE(confighttp_input.is_open());
  std::ostringstream process_contents;
  std::ostringstream confighttp_contents;
  process_contents << process_input.rdbuf();
  confighttp_contents << confighttp_input.rdbuf();
  EXPECT_NE(process_contents.str().find("virtual_display::create("), std::string::npos);
  EXPECT_NE(confighttp_contents.str().find("virtual_display::create("), std::string::npos);
}

TEST(SourceSafetyContracts, WlgrabRequestedOutputSelectionFailsClosed) {
  std::ifstream input(fs::path {POLARIS_SOURCE_DIR} / "src/platform/linux/wlgrab.cpp");
  ASSERT_TRUE(input.is_open());
  std::ostringstream contents;
  contents << input.rdbuf();
  const auto source = contents.str();
  const auto init = source.find("int init(platf::mem_type_e hwdevice_type");
  const auto next_method = source.find("std::shared_ptr<platf::img_t> alloc_img()", init);
  ASSERT_NE(init, std::string::npos);
  ASSERT_NE(next_method, std::string::npos);
  const auto body = source.substr(init, next_method - init);

  const auto select = body.find("wlgrab_capture_policy::select_monitor_index(");
  const auto reject = body.find("if (!monitor_index)", select);
  const auto refuse = body.find("refusing to capture another output", reject);
  const auto return_failure = body.find("return -1;", reject);
  const auto dereference = body.find("interface.monitors[*monitor_index]", select);
  ASSERT_NE(select, std::string::npos);
  ASSERT_NE(reject, std::string::npos);
  ASSERT_NE(refuse, std::string::npos);
  ASSERT_NE(return_failure, std::string::npos);
  ASSERT_NE(dereference, std::string::npos);
  EXPECT_LT(reject, return_failure);
  EXPECT_LT(return_failure, dereference);
  EXPECT_EQ(body.find("interface.monitors[0].get()"), std::string::npos);
}

TEST(SourceSafetyContracts, ReadlinkResultsAreCheckedBeforeUse) {
  // readlink reports failure as -1. Widening that into a size handed a
  // string_view a length of SIZE_MAX; the other seven call sites all checked.
  std::vector<std::string> unchecked;
  for (const auto &source : all_sources()) {
    std::size_t at = 0;
    while ((at = source.text.find("readlink", at)) != std::string::npos) {
      const auto open = source.text.find('(', at);
      if (open == std::string::npos) {
        break;
      }
      const auto next = at + std::string("readlink").size();
      // Only a real call has its parenthesis immediately after the name, and
      // only prose writes "readlink()" with nothing in it.
      if (source.text.compare(next, 1, "(") != 0 || source.text.compare(next, 2, "()") == 0 ||
          inside_string_literal(source.text, at) || inside_comment(source.text, at)) {
        at = next;
        continue;
      }

      const auto window = source.text.substr(open, 320);
      static const std::regex guard {R"(<\s*0|<=\s*0|>\s*0|!=\s*-1|==\s*-1)"};
      if (!std::regex_search(window, guard)) {
        unchecked.push_back(source.relative_path + ":" + std::to_string(line_of(source.text, at)));
      }
      at = next;
    }
  }

  EXPECT_TRUE(unchecked.empty()) << [&] {
    std::ostringstream out;
    out << "readlink call(s) whose result is not checked nearby:";
    for (const auto &site : unchecked) {
      out << "\n  " << site;
    }
    return out.str();
  }();
}
