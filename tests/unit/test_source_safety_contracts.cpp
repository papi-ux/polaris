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

  const auto initial = source.find("const auto active_generation = capture_ctxs.front().config.capture_generation;");
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
  const auto exact_branch = reinit_body.find("if (!exact_display_name.empty())");
  const auto exact_reset = reinit_body.find("reset_display(", exact_branch);
  const auto identity = reinit_body.find("exact_display_name", exact_reset);
  const auto reinit_reject = reinit_body.find("return;", identity);
  const auto fallback_refresh = reinit_body.find("refresh_displays(", reinit_reject);
  ASSERT_NE(exact_branch, std::string::npos);
  ASSERT_NE(exact_reset, std::string::npos);
  ASSERT_NE(identity, std::string::npos);
  ASSERT_NE(reinit_reject, std::string::npos);
  ASSERT_NE(fallback_refresh, std::string::npos);
  EXPECT_LT(exact_branch, exact_reset);
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

TEST(SourceSafetyContracts, ExactOutputProvenanceLivesInLaunchAndCaptureGenerations) {
  std::ifstream input(fs::path {POLARIS_SOURCE_DIR} / "src/video.cpp");
  ASSERT_TRUE(input.is_open());
  std::ostringstream contents;
  contents << input.rdbuf();
  const auto source = contents.str();

  std::ifstream process_input(fs::path {POLARIS_SOURCE_DIR} / "src/process.cpp");
  std::ifstream process_header_input(fs::path {POLARIS_SOURCE_DIR} / "src/process.h");
  ASSERT_TRUE(process_input.is_open());
  ASSERT_TRUE(process_header_input.is_open());
  std::ostringstream process_contents;
  std::ostringstream process_header_contents;
  process_contents << process_input.rdbuf();
  process_header_contents << process_header_input.rdbuf();
  const auto process_source = process_contents.str();
  const auto process_header = process_header_contents.str();

  EXPECT_NE(process_header.find("capture_generation::identity_t capture_generation;"), std::string::npos);
  EXPECT_EQ(process_header.find("std::string exact_display_name;"), std::string::npos);
  const auto first_publish = process_source.find("this->capture_generation.exact_display_name = this->display_name;");
  const auto second_publish = process_source.find(
    "this->capture_generation.exact_display_name = this->display_name;",
    first_publish + 1
  );
  const auto clear = process_source.find("capture_generation = {};", second_publish);
  ASSERT_NE(first_publish, std::string::npos);
  ASSERT_NE(second_publish, std::string::npos);
  ASSERT_NE(clear, std::string::npos);

  const auto sync_ctx = source.find("struct sync_session_ctx_t");
  const auto sync_ctx_end = source.find("};", sync_ctx);
  const auto async_ctx = source.find("struct capture_ctx_t");
  const auto async_ctx_end = source.find("};", async_ctx);
  ASSERT_NE(sync_ctx, std::string::npos);
  ASSERT_NE(sync_ctx_end, std::string::npos);
  ASSERT_NE(async_ctx, std::string::npos);
  ASSERT_NE(async_ctx_end, std::string::npos);
  EXPECT_NE(source.substr(sync_ctx, sync_ctx_end - sync_ctx).find("config_t config;"), std::string::npos);
  EXPECT_NE(source.substr(async_ctx, async_ctx_end - async_ctx).find("config_t config;"), std::string::npos);
  EXPECT_EQ(source.substr(sync_ctx, sync_ctx_end - sync_ctx).find("std::string exact_display_name;"), std::string::npos);
  EXPECT_EQ(source.substr(async_ctx, async_ctx_end - async_ctx).find("std::string exact_display_name;"), std::string::npos);
  const auto capture_admission = source.find(
    "config.capture_generation = proc::proc.capture_generation;"
  );
  const auto async_entry = source.find("void capture_async(");
  const auto async_publish = source.find("ref->capture_ctx_queue->raise(capture_ctx_t", async_entry);
  const auto async_provenance = source.find("config,", async_publish);
  const auto async_publish_end = source.find("});", async_publish);
  const auto async_call = source.find("capture_async(", capture_admission);
  const auto async_call_provenance = source.find("config,", async_call);
  const auto async_call_end = source.find(");", async_call);
  const auto sync_publish = source.find("ref->encode_session_ctx_queue.raise(sync_session_ctx_t", capture_admission);
  const auto sync_provenance = source.find("config,", sync_publish);
  const auto sync_publish_end = source.find("});", sync_publish);
  ASSERT_NE(capture_admission, std::string::npos);
  ASSERT_NE(async_entry, std::string::npos);
  ASSERT_NE(async_publish, std::string::npos);
  ASSERT_NE(async_provenance, std::string::npos);
  ASSERT_NE(async_publish_end, std::string::npos);
  ASSERT_NE(async_call, std::string::npos);
  ASSERT_NE(async_call_provenance, std::string::npos);
  ASSERT_NE(async_call_end, std::string::npos);
  ASSERT_NE(sync_publish, std::string::npos);
  ASSERT_NE(sync_provenance, std::string::npos);
  ASSERT_NE(sync_publish_end, std::string::npos);
  EXPECT_LT(async_provenance, async_publish_end);
  EXPECT_LT(async_call_provenance, async_call_end);
  EXPECT_LT(sync_provenance, sync_publish_end);

  const auto async_begin = source.find("void captureThread(");
  const auto sync_begin = source.find("encode_e encode_run_sync(", async_begin);
  const auto sync_end = source.find("void captureThreadSync()", sync_begin);
  ASSERT_NE(async_begin, std::string::npos);
  ASSERT_NE(sync_begin, std::string::npos);
  ASSERT_NE(sync_end, std::string::npos);

  const auto async_body = source.substr(async_begin, sync_begin - async_begin);
  const auto async_exact = async_body.find("const auto active_generation = capture_ctxs.front().config.capture_generation;");
  const auto async_policy = async_body.find("display_switch_allowed_for_exact_capture(exact_display_name)");
  const auto async_reinit_loop = async_body.find("while (capture_ctx_queue->running())", async_policy);
  const auto async_exact_reopen = async_body.find("if (!exact_display_name.empty())", async_reinit_loop);
  const auto async_reset = async_body.find("reset_display(", async_exact_reopen);
  const auto async_identity = async_body.find("exact_display_name", async_reset);
  const auto async_generic_refresh = async_body.find("refresh_displays(", async_identity);
  ASSERT_NE(async_exact, std::string::npos);
  ASSERT_NE(async_policy, std::string::npos);
  ASSERT_NE(async_reinit_loop, std::string::npos);
  ASSERT_NE(async_exact_reopen, std::string::npos);
  ASSERT_NE(async_reset, std::string::npos);
  ASSERT_NE(async_identity, std::string::npos);
  ASSERT_NE(async_generic_refresh, std::string::npos);
  EXPECT_LT(async_exact, async_policy);
  EXPECT_LT(async_exact_reopen, async_reset);
  EXPECT_LT(async_reset, async_identity);
  EXPECT_LT(async_identity, async_generic_refresh);
  EXPECT_EQ(
    async_body.find("const auto exact_display_name = proc::proc.display_name"),
    std::string::npos
  );
  EXPECT_EQ(
    async_body.find("const auto exact_display_name = proc::proc.exact_display_name"),
    std::string::npos
  );
  EXPECT_EQ(
    async_body.find("!switch_display_event->peek() && !exact_display_name.empty()"),
    std::string::npos
  );

  const auto sync_body = source.substr(sync_begin, sync_end - sync_begin);
  const auto sync_exact = sync_body.find("const auto active_generation = synced_session_ctxs.front()->config.capture_generation;");
  const auto sync_exact_branch = sync_body.find("if (!exact_display_name.empty())", sync_exact);
  const auto sync_reset = sync_body.find("reset_display(", sync_exact_branch);
  const auto sync_identity = sync_body.find("exact_display_name", sync_reset);
  const auto sync_generic_refresh = sync_body.find("refresh_displays(", sync_identity);
  const auto sync_policy = sync_body.find("display_switch_allowed_for_exact_capture(exact_display_name)");
  ASSERT_NE(sync_exact, std::string::npos);
  ASSERT_NE(sync_exact_branch, std::string::npos);
  ASSERT_NE(sync_reset, std::string::npos);
  ASSERT_NE(sync_identity, std::string::npos);
  ASSERT_NE(sync_generic_refresh, std::string::npos);
  ASSERT_NE(sync_policy, std::string::npos);
  EXPECT_LT(sync_exact_branch, sync_reset);
  EXPECT_LT(sync_reset, sync_identity);
  EXPECT_LT(sync_identity, sync_generic_refresh);
  EXPECT_EQ(
    sync_body.find("const auto exact_display_name = proc::proc.display_name"),
    std::string::npos
  );
  EXPECT_EQ(
    sync_body.find("const auto exact_display_name = proc::proc.exact_display_name"),
    std::string::npos
  );
}

TEST(SourceSafetyContracts, CaptureGenerationMismatchIsRejectedBeforePublication) {
  std::ifstream input(fs::path {POLARIS_SOURCE_DIR} / "src/video.cpp");
  ASSERT_TRUE(input.is_open());
  std::ostringstream contents;
  contents << input.rdbuf();
  const auto source = contents.str();

  const auto async_begin = source.find("void captureThread(");
  const auto sync_begin = source.find("encode_e encode_run_sync(", async_begin);
  const auto sync_end = source.find("void captureThreadSync()", sync_begin);
  ASSERT_NE(async_begin, std::string::npos);
  ASSERT_NE(sync_begin, std::string::npos);
  ASSERT_NE(sync_end, std::string::npos);

  const auto async_body = source.substr(async_begin, sync_begin - async_begin);
  const auto async_match = async_body.find("capture_generations_match(");
  const auto async_reject = async_body.find("incoming_capture_ctx->images->stop()", async_match);
  const auto async_publish = async_body.find("capture_ctxs.emplace_back", async_reject);
  ASSERT_NE(async_match, std::string::npos);
  ASSERT_NE(async_reject, std::string::npos);
  ASSERT_NE(async_publish, std::string::npos);
  EXPECT_LT(async_match, async_reject);
  EXPECT_LT(async_reject, async_publish);

  const auto sync_body = source.substr(sync_begin, sync_end - sync_begin);
  const auto sync_match = sync_body.find("capture_generations_match(");
  const auto sync_reject = sync_body.find("incoming_sync_ctx->join_event->raise(true)", sync_match);
  const auto sync_publish = sync_body.find("synced_session_ctxs.emplace_back", sync_reject);
  ASSERT_NE(sync_match, std::string::npos);
  ASSERT_NE(sync_reject, std::string::npos);
  ASSERT_NE(sync_publish, std::string::npos);
  EXPECT_LT(sync_match, sync_reject);
  EXPECT_LT(sync_reject, sync_publish);
}

TEST(SourceSafetyContracts, CaptureGenerationIdentityIsOwnedFromLaunchThroughVideo) {
  const auto root = fs::path {POLARIS_SOURCE_DIR};
  std::ifstream identity_in(root / "src/capture_generation.h");
  std::ifstream process_header_in(root / "src/process.h");
  std::ifstream process_in(root / "src/process.cpp");
  std::ifstream video_header_in(root / "src/video.h");
  std::ifstream video_in(root / "src/video.cpp");
  ASSERT_TRUE(identity_in.is_open());
  ASSERT_TRUE(process_header_in.is_open());
  ASSERT_TRUE(process_in.is_open());
  ASSERT_TRUE(video_header_in.is_open());
  ASSERT_TRUE(video_in.is_open());

  std::ostringstream identity_out, process_header_out, process_out, video_header_out, video_out;
  identity_out << identity_in.rdbuf();
  process_header_out << process_header_in.rdbuf();
  process_out << process_in.rdbuf();
  video_header_out << video_header_in.rdbuf();
  video_out << video_in.rdbuf();
  const auto identity = identity_out.str();
  const auto process_header = process_header_out.str();
  const auto process = process_out.str();
  const auto video_header = video_header_out.str();
  const auto video = video_out.str();

  EXPECT_NE(identity.find("struct identity_t"), std::string::npos);
  for (const auto field : {"generation_id", "exact_display_name", "requested_output_name", "stream_mode",
                           "capture_backend", "private_runtime", "adapter_name",
                           "headless_mode", "use_cage_compositor"}) {
    EXPECT_NE(identity.find(field), std::string::npos) << field;
  }
  EXPECT_NE(process_header.find("capture_generation::identity_t capture_generation;"), std::string::npos);
  EXPECT_EQ(process_header.find("std::string exact_display_name;"), std::string::npos);
  EXPECT_NE(process.find("capture_generation.generation_id = _session_generation;"), std::string::npos);
  EXPECT_NE(process.find("capture_generation = capture_generation::identity_t {"), std::string::npos);
  EXPECT_NE(video_header.find("capture_generation::identity_t capture_generation;"), std::string::npos);
  EXPECT_NE(video.find("config.capture_generation = proc::proc.capture_generation;"), std::string::npos);
}

TEST(SourceSafetyContracts, PortalSourceSelectionAndPublicationUseOneCaptureGeneration) {
  const auto root = fs::path {POLARIS_SOURCE_DIR};
  std::ifstream portal_in(root / "src/platform/linux/portal_grab.cpp");
  std::ifstream kwin_header_in(root / "src/platform/linux/kwingrab.h");
  ASSERT_TRUE(portal_in.is_open());
  ASSERT_TRUE(kwin_header_in.is_open());
  std::ostringstream portal_out, kwin_header_out;
  portal_out << portal_in.rdbuf();
  kwin_header_out << kwin_header_in.rdbuf();
  const auto portal = portal_out.str();
  const auto kwin_header = kwin_header_out.str();

  const auto ensure = portal.find("static std::shared_ptr<pipewire_capture::capture_t> ensure_global_capture(");
  const auto display = portal.find("class portal_display_t", ensure);
  ASSERT_NE(ensure, std::string::npos);
  ASSERT_NE(display, std::string::npos);
  const auto ensure_body = portal.substr(ensure, display - ensure);
  EXPECT_NE(ensure_body.find("const capture_generation::identity_t &generation"), std::string::npos);
  EXPECT_NE(ensure_body.find("g_media.generation == generation"), std::string::npos);
  const auto first_publication = ensure_body.find("g_media.generation = generation");
  const auto second_publication = ensure_body.find("g_media.generation = generation", first_publication + 1);
  const auto third_publication = ensure_body.find("g_media.generation = generation", second_publication + 1);
  const auto fourth_publication = ensure_body.find("g_media.generation = generation", third_publication + 1);
  ASSERT_NE(first_publication, std::string::npos);
  ASSERT_NE(second_publication, std::string::npos);
  ASSERT_NE(third_publication, std::string::npos);
  EXPECT_EQ(fourth_publication, std::string::npos);
  EXPECT_NE(
    ensure_body.find("g_media.capture != capture || g_media.generation != generation"),
    std::string::npos
  );
  EXPECT_NE(ensure_body.find("ensure_session_unlocked(generation)"), std::string::npos);
  EXPECT_NE(ensure_body.find("kwingrab::prefer_for_generation(generation)"), std::string::npos);
  EXPECT_NE(ensure_body.find("kwingrab::require_for_generation(generation)"), std::string::npos);
  EXPECT_NE(ensure_body.find("start_output_session(generation.requested_output_name)"), std::string::npos);
  for (const auto forbidden : {"config::video.adapter_name", "config::video.output_name",
                               "config::video.capture", "config::video.linux_display"}) {
    EXPECT_EQ(ensure_body.find(forbidden), std::string::npos) << forbidden;
  }

  const auto init = portal.find("init(platf::mem_type_e", display);
  const auto capture = portal.find("capture(const push_captured_image_cb_t", init);
  ASSERT_NE(init, std::string::npos);
  ASSERT_NE(capture, std::string::npos);
  const auto init_body = portal.substr(init, capture - init);
  const auto exact_validation = init_body.find("display_name != generation_.exact_display_name");
  const auto requested_validation = init_body.find("generation_.requested_output_name != generation_.exact_display_name");
  const auto exact_reject = init_body.find("return -1;", requested_validation);
  const auto init_capture = init_body.find("ensure_global_capture(", exact_reject);
  ASSERT_NE(exact_validation, std::string::npos);
  ASSERT_NE(requested_validation, std::string::npos);
  ASSERT_NE(exact_reject, std::string::npos);
  ASSERT_NE(init_capture, std::string::npos);
  EXPECT_LT(exact_validation, requested_validation);
  EXPECT_LT(requested_validation, exact_reject);
  EXPECT_LT(exact_reject, init_capture);
  EXPECT_NE(init_body.find("generation_ = config.capture_generation;"), std::string::npos);
  EXPECT_NE(init_body.find("generation_"), std::string::npos);
  EXPECT_NE(kwin_header.find("prefer_for_generation(const capture_generation::identity_t &generation)"), std::string::npos);
  EXPECT_NE(kwin_header.find("require_for_generation(const capture_generation::identity_t &generation)"), std::string::npos);
}

TEST(SourceSafetyContracts, SwayVirtualOutputCreationIsRejectedBeforeMutation) {
  std::ifstream input(fs::path {POLARIS_SOURCE_DIR} / "src/platform/linux/virtual_display.cpp");
  ASSERT_TRUE(input.is_open());
  std::ostringstream contents;
  contents << input.rdbuf();
  const auto source = contents.str();

  const auto wayland = source.find("namespace wayland_wlr");
  const auto available = source.find("static bool is_available()", wayland);
  const auto create = source.find("static std::optional<vdisplay_t> create(", available);
  ASSERT_NE(wayland, std::string::npos);
  ASSERT_NE(available, std::string::npos);
  ASSERT_NE(create, std::string::npos);
  const auto availability_body = source.substr(available, create - available);
  const auto exact_capability = availability_body.find(
    "wayland_compositor_supports_exact_output_creation(compositor)"
  );
  const auto reject = availability_body.find("return false", exact_capability);
  ASSERT_NE(exact_capability, std::string::npos);
  ASSERT_NE(reject, std::string::npos);
  EXPECT_EQ(source.find("swaymsg -r create_output"), std::string::npos);
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
  const auto first_output_fallback = kwin.find("output = outputs_.begin()->first", reject);
  ASSERT_NE(requested, std::string::npos);
  ASSERT_NE(exact_policy, std::string::npos);
  ASSERT_NE(reject, std::string::npos);
  EXPECT_EQ(configured_fallback, std::string::npos);
  ASSERT_NE(first_output_fallback, std::string::npos);
  EXPECT_LT(exact_policy, reject);
  EXPECT_LT(reject, first_output_fallback);

  std::ifstream portal_input(fs::path {POLARIS_SOURCE_DIR} / "src/platform/linux/portal_grab.cpp");
  ASSERT_TRUE(portal_input.is_open());
  std::ostringstream portal_contents;
  portal_contents << portal_input.rdbuf();
  const auto portal = portal_contents.str();

  const auto compatible = portal.find("const auto compatible");
  const auto identity = portal.find("g_media.generation == generation", compatible);
  const auto reuse = portal.find("return g_media.capture;", identity);
  const auto no_wayland = portal.find("#ifndef POLARIS_BUILD_WAYLAND", reuse);
  const auto no_wayland_mode = portal.find("generation.stream_mode == \"host_virtual_display\"", no_wayland);
  const auto no_wayland_reject = portal.find("return nullptr;", no_wayland_mode);
  const auto wayland_guard = portal.find("#ifdef POLARIS_BUILD_WAYLAND", no_wayland_reject);
  const auto kwin_start = portal.find(
    "kwingrab::start_output_session(generation.requested_output_name)",
    wayland_guard
  );
  const auto required = portal.find("kwingrab::require_for_generation(generation)", kwin_start);
  const auto fail_closed = portal.find("return nullptr;", required);
  const auto generic_portal = portal.find("ensure_session_unlocked(generation)", kwin_start);
  ASSERT_NE(compatible, std::string::npos);
  ASSERT_NE(identity, std::string::npos);
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
  EXPECT_LT(identity, reuse);
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

  const auto wayland = source.find("namespace wayland_wlr");
  const auto create_entry = source.find("static std::optional<vdisplay_t> create(", wayland);
  const auto hyprland = source.find("if (compositor == \"hyprland\")", create_entry);
  const auto requested_name = source.find("std::string candidate = hyprland_output_name_for_pid(", hyprland);
  const auto create = source.find("hyprctl output create headless", requested_name);
  const auto ownership = source.find("hyprland_monitors_contain_output(", create);
  const auto reject = source.find("return std::nullopt;", ownership);
  const auto publish = source.find("display.backend = backend_e::WAYLAND_WLR", reject);
  ASSERT_NE(wayland, std::string::npos);
  ASSERT_NE(create_entry, std::string::npos);
  ASSERT_NE(hyprland, std::string::npos);
  ASSERT_NE(requested_name, std::string::npos);
  ASSERT_NE(create, std::string::npos);
  ASSERT_NE(ownership, std::string::npos);
  ASSERT_NE(reject, std::string::npos);
  ASSERT_NE(publish, std::string::npos);
  EXPECT_LT(requested_name, create);
  EXPECT_LT(create, ownership);
  EXPECT_LT(ownership, reject);
  EXPECT_LT(reject, publish);
  EXPECT_EQ(source.find("swaymsg -r create_output"), std::string::npos);
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
