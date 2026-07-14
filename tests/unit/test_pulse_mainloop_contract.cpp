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

  std::size_t count_occurrences(const std::string &text, const std::string &needle) {
    std::size_t count = 0;
    std::size_t pos = 0;
    while ((pos = text.find(needle, pos)) != std::string::npos) {
      ++count;
      pos += needle.size();
    }
    return count;
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

  const std::regex operation_local {R"(\bop_t\s+[A-Za-z_][A-Za-z0-9_]*\s*\{)"};
  const auto operation_locals = std::distance(
    std::sregex_iterator(server_source.begin(), server_source.end(), operation_local),
    std::sregex_iterator()
  );
  const auto operation_waits = count_occurrences(server_source, "wait_for_operation(") - 1;
  EXPECT_EQ(operation_locals, operation_waits);
}
