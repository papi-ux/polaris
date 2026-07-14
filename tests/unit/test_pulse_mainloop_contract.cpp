/**
 * @file tests/unit/test_pulse_mainloop_contract.cpp
 * @brief Contract tests for synchronized PulseAudio async-operation ownership.
 */

#include "../tests_common.h"

#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

namespace {
  std::string read_linux_audio_source() {
    const auto path = std::filesystem::path(POLARIS_SOURCE_DIR) / "src/platform/linux/audio.cpp";
    std::ifstream in(path);
    if (!in) {
      return {};
    }

    std::ostringstream out;
    out << in.rdbuf();
    return out.str();
  }

  std::size_t leading_spaces(const std::string &line) {
    return line.find_first_not_of(' ');
  }

  std::vector<std::string> operation_contract_violations(const std::string &source) {
    std::vector<std::string> lines;
    std::istringstream input {source};
    for (std::string line; std::getline(input, line);) {
      lines.emplace_back(std::move(line));
    }

    std::vector<std::string> violations;
    const std::regex operation_declaration {R"(^([ ]*)op_t[ ]+([A-Za-z_][A-Za-z0-9_]*)[ ]*\{)"};
    std::size_t operation_count = 0;

    for (std::size_t line_index = 0; line_index < lines.size(); ++line_index) {
      std::smatch match;
      if (!std::regex_search(lines[line_index], match, operation_declaration)) {
        continue;
      }

      ++operation_count;
      const auto indentation = match[1].str().size();
      const auto variable = match[2].str();

      std::size_t scope_begin = 0;
      for (auto previous = line_index; previous > 0; --previous) {
        const auto &line = lines[previous - 1];
        const auto previous_indentation = leading_spaces(line);
        if (previous_indentation != std::string::npos &&
            previous_indentation < indentation &&
            line.find('{') != std::string::npos) {
          scope_begin = previous;
          break;
        }
      }

      std::size_t scope_end = lines.size();
      for (auto following = line_index + 1; following < lines.size(); ++following) {
        const auto following_indentation = leading_spaces(lines[following]);
        if (following_indentation != std::string::npos && following_indentation < indentation) {
          scope_end = following;
          break;
        }
      }

      std::size_t lock_count = 0;
      for (auto candidate = scope_begin; candidate < line_index; ++candidate) {
        if (leading_spaces(lines[candidate]) == indentation &&
            lines[candidate].find("mainloop_lock_t ") != std::string::npos) {
          ++lock_count;
        }
      }
      if (lock_count != 1) {
        violations.emplace_back(
          "operation '" + variable + "' on line " + std::to_string(line_index + 1) +
          " must have exactly one preceding same-scope mainloop lock"
        );
      }

      const std::regex matching_wait {
        "wait_for_operation[ ]*\\([ ]*" + variable + "[ ]*\\)"
      };
      std::size_t wait_count = 0;
      for (auto candidate = line_index + 1; candidate < scope_end; ++candidate) {
        wait_count += std::distance(
          std::sregex_iterator(lines[candidate].begin(), lines[candidate].end(), matching_wait),
          std::sregex_iterator()
        );
      }
      if (wait_count != 1) {
        violations.emplace_back(
          "operation '" + variable + "' on line " + std::to_string(line_index + 1) +
          " must have exactly one matching same-scope wait"
        );
      }
    }

    if (operation_count == 0) {
      violations.emplace_back("no PulseAudio operations found");
    }

    const std::regex wait_recheck_loop {
      R"(while[ ]*\([ ]*pa_operation_get_state[ ]*\([ ]*op\.get\(\)[ ]*\)[ ]*==[ ]*PA_OPERATION_RUNNING[ ]*\))"
    };
    if (!std::regex_search(source, wait_recheck_loop)) {
      violations.emplace_back("wait_for_operation must recheck PA_OPERATION_RUNNING in a while loop");
    }

    return violations;
  }

  std::string replace_once(std::string source, const std::string &needle, const std::string &replacement) {
    const auto position = source.find(needle);
    if (position == std::string::npos || source.find(needle, position + needle.size()) != std::string::npos) {
      return {};
    }
    source.replace(position, needle.size(), replacement);
    return source;
  }
}

TEST(PulseAudioMainloopContractTest, SerializesContextAndOperationAccessWithThreadedMainloop) {
  const auto source = read_linux_audio_source();
  ASSERT_FALSE(source.empty());

  EXPECT_NE(source.find("pa_threaded_mainloop_new"), std::string::npos);
  EXPECT_NE(source.find("pa_threaded_mainloop_get_api"), std::string::npos);
  EXPECT_NE(source.find("pa_threaded_mainloop_lock"), std::string::npos);
  EXPECT_NE(source.find("pa_threaded_mainloop_unlock"), std::string::npos);
  EXPECT_NE(source.find("pa_threaded_mainloop_wait"), std::string::npos);
  EXPECT_NE(source.find("pa_threaded_mainloop_signal"), std::string::npos);
  EXPECT_NE(source.find("pa_operation_get_state"), std::string::npos);
  EXPECT_NE(source.find("PA_OPERATION_RUNNING"), std::string::npos);

  EXPECT_EQ(source.find("pa_mainloop_run"), std::string::npos);
  EXPECT_EQ(source.find("std::thread worker"), std::string::npos);

  const auto pa_namespace = source.find("namespace pa {");
  ASSERT_NE(pa_namespace, std::string::npos);
  const auto server_source = source.substr(pa_namespace);
  EXPECT_EQ(server_source.find("alarm->wait()"), std::string::npos);
  EXPECT_EQ(server_source.find("move_alarm->wait()"), std::string::npos);

  const auto violations = operation_contract_violations(server_source);
  EXPECT_TRUE(violations.empty()) << (violations.empty() ? "" : violations.front());
}

TEST(PulseAudioMainloopContractTest, RejectsOperationWithoutSameScopeLock) {
  const auto source = read_linux_audio_source();
  ASSERT_FALSE(source.empty());

  const auto mutant = replace_once(
    source,
    "      int load_null(const char *name, const std::uint8_t *channel_mapping, int channels) {\n"
    "        auto alarm = safe::make_alarm<int>();\n"
    "        mainloop_lock_t lock {loop.get()};\n\n"
    "        op_t op {",
    "      int load_null(const char *name, const std::uint8_t *channel_mapping, int channels) {\n"
    "        auto alarm = safe::make_alarm<int>();\n\n"
    "        op_t op {"
  );
  ASSERT_FALSE(mutant.empty());
  EXPECT_FALSE(operation_contract_violations(mutant).empty());
}

TEST(PulseAudioMainloopContractTest, RejectsOneShotOperationWait) {
  const auto source = read_linux_audio_source();
  ASSERT_FALSE(source.empty());

  const auto mutant = replace_once(
    source,
    "while (pa_operation_get_state(op.get()) == PA_OPERATION_RUNNING) {",
    "if (pa_operation_get_state(op.get()) == PA_OPERATION_RUNNING) {"
  );
  ASSERT_FALSE(mutant.empty());
  EXPECT_FALSE(operation_contract_violations(mutant).empty());
}

TEST(PulseAudioMainloopContractTest, RejectsDuplicateAndMissingOperationWaits) {
  const auto source = read_linux_audio_source();
  ASSERT_FALSE(source.empty());

  auto mutant = replace_once(
    source,
    "if (!wait_for_operation(op) || !alarm->status()) {\n"
    "          BOOST_LOG(error) << \"Null-sink load operation was cancelled\"sv;",
    "if (!wait_for_operation(op) || !wait_for_operation(op) || !alarm->status()) {\n"
    "          BOOST_LOG(error) << \"Null-sink load operation was cancelled\"sv;"
  );
  ASSERT_FALSE(mutant.empty());
  mutant = replace_once(
    std::move(mutant),
    "if (!wait_for_operation(op) || !alarm->status()) {\n"
    "          BOOST_LOG(error) << \"Null-sink unload operation was cancelled for index [\"sv << i << ']';",
    "if (!alarm->status()) {\n"
    "          BOOST_LOG(error) << \"Null-sink unload operation was cancelled for index [\"sv << i << ']';"
  );
  ASSERT_FALSE(mutant.empty());
  EXPECT_GE(operation_contract_violations(mutant).size(), 2u);
}
