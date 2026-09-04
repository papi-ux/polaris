/**
 * @file tests/unit/platform/test_labwc_startup_diagnostics.cpp
 * @brief Test bounded labwc startup-client evidence.
 */
#include "../../tests_common.h"

#ifdef __linux__

#include <src/platform/linux/labwc_startup_diagnostics.h>

#include <glib.h>

#include <atomic>
#include <array>
#include <cerrno>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using namespace std::literals;

TEST(LabwcStartupDiagnosticsTests, KeepsOnlyABoundedTail) {
  labwc_startup_diagnostics::collector_t collector {"generation-a"};
  const std::string prefix(
    labwc_startup_diagnostics::max_retained_stderr_bytes,
    'a'
  );
  collector.append_stderr(prefix);
  collector.append_stderr("\nthe-useful-tail");

  const auto snapshot = collector.snapshot("generation-a");
  ASSERT_TRUE(snapshot.has_value());
  EXPECT_EQ(snapshot->stderr_bytes_seen, prefix.size() + 16);
  EXPECT_TRUE(snapshot->stderr_truncated);
  EXPECT_TRUE(snapshot->stderr_summary.ends_with("the-useful-tail"));
  EXPECT_LE(snapshot->stderr_summary.size(), labwc_startup_diagnostics::max_log_summary_bytes);
}

TEST(LabwcStartupDiagnosticsTests, RefusesEvidenceFromAnotherGeneration) {
  labwc_startup_diagnostics::collector_t collector {"generation-a"};
  collector.append_stderr("one generation only");
  collector.record_shell_exit_status(7);

  EXPECT_FALSE(collector.snapshot("").has_value());
  EXPECT_FALSE(collector.snapshot("generation-b").has_value());
  ASSERT_TRUE(collector.snapshot("generation-a").has_value());
}

TEST(LabwcStartupDiagnosticsTests, RedactsAndFlattensUntrustedStderr) {
  const auto private_path = "/"s + "home/example-user/game/file";
  const auto redacted_private_path = "/"s + "home/[user]/game/file";
  const auto result = labwc_startup_diagnostics::redact_stderr_for_log(
    "\x1b[31merror\x1b[0m\n" + private_path + " " +
    "https://user:pass@example.test/run?access_token=abc "s +
    "Authorization: Bearer-secret password=hunter2 "
    "--api-key topsecret 192.168.1.20 peer=[2001:db8::42] user@example.test "
    "hostname=private-workstation "
    "0123456789012345678901234567890123456789"
  );

  EXPECT_EQ(result.text.find('\n'), std::string::npos);
  EXPECT_EQ(result.text.find("example-user"), std::string::npos);
  EXPECT_EQ(result.text.find("user:pass"), std::string::npos);
  EXPECT_EQ(result.text.find("access_token=abc"), std::string::npos);
  EXPECT_EQ(result.text.find("Bearer-secret"), std::string::npos);
  EXPECT_EQ(result.text.find("hunter2"), std::string::npos);
  EXPECT_EQ(result.text.find("topsecret"), std::string::npos);
  EXPECT_EQ(result.text.find("192.168.1.20"), std::string::npos);
  EXPECT_EQ(result.text.find("2001:db8::42"), std::string::npos);
  EXPECT_EQ(result.text.find("user@example.test"), std::string::npos);
  EXPECT_EQ(result.text.find("private-workstation"), std::string::npos);
  EXPECT_EQ(result.text.find("0123456789012345678901234567890123456789"), std::string::npos);
  EXPECT_NE(result.text.find("error"), std::string::npos);
  EXPECT_NE(result.text.find(redacted_private_path), std::string::npos);
}

TEST(LabwcStartupDiagnosticsTests, KeepsRedactedSummaryWithinPublishedBound) {
  std::string input;
  for (int count = 0; count < 700; ++count) {
    input += "x ";
  }

  const auto result = labwc_startup_diagnostics::redact_stderr_for_log(input);
  EXPECT_TRUE(result.truncated);
  EXPECT_TRUE(result.text.starts_with("[truncated-prefix]"));
  EXPECT_LE(result.text.size(), labwc_startup_diagnostics::max_log_summary_bytes);
}

TEST(LabwcStartupDiagnosticsTests, InstrumentedShellReportsStatusAndStderr) {
  std::array<int, 2> stderr_pipe {-1, -1};
  std::array<int, 2> status_pipe {-1, -1};
  ASSERT_EQ(pipe(stderr_pipe.data()), 0);
  ASSERT_EQ(pipe(status_pipe.data()), 0);

  const auto private_path = "/"s + "home/example-user/game";
  const auto redacted_private_path = "/"s + "home/[user]/game";
  const auto command = labwc_startup_diagnostics::make_labwc_startup_command(
    labwc_startup_diagnostics::instrument_shell_command(
      "printf '%s\\n' \"can't launch " + private_path + "\" >&2; exit 7",
      stderr_pipe[1],
      status_pipe[1]
    )
  );

  gint argc = 0;
  gchar **argv = nullptr;
  GError *parse_error = nullptr;
  ASSERT_TRUE(g_shell_parse_argv(command.c_str(), &argc, &argv, &parse_error));
  ASSERT_EQ(parse_error, nullptr);
  ASSERT_EQ(argc, 3);
  ASSERT_STREQ(argv[0], "bash");
  ASSERT_STREQ(argv[1], "-c");

  auto collector = std::make_shared<labwc_startup_diagnostics::collector_t>("generation-a");
  std::thread reader {
    labwc_startup_diagnostics::drain_pipes,
    stderr_pipe[0],
    status_pipe[0],
    collector
  };

  const auto child = fork();
  if (child < 0) {
    g_strfreev(argv);
    close(stderr_pipe[1]);
    close(status_pipe[1]);
    reader.join();
    FAIL() << "fork failed";
    return;
  }
  if (child == 0) {
    close(stderr_pipe[0]);
    close(status_pipe[0]);
    execvp(argv[0], argv);
    _exit(126);
  }

  g_strfreev(argv);
  close(stderr_pipe[1]);
  close(status_pipe[1]);
  int child_status = 0;
  const auto waited = waitpid(child, &child_status, 0);
  reader.join();

  ASSERT_EQ(waited, child);
  ASSERT_TRUE(WIFEXITED(child_status));
  EXPECT_EQ(WEXITSTATUS(child_status), 7);
  const auto snapshot = collector->snapshot("generation-a");
  ASSERT_TRUE(snapshot.has_value());
  ASSERT_EQ(snapshot->shell_exit_status, std::optional<int> {7});
  EXPECT_NE(snapshot->stderr_summary.find("can't launch"), std::string::npos);
  EXPECT_NE(snapshot->stderr_summary.find(redacted_private_path), std::string::npos);
}

TEST(LabwcStartupDiagnosticsTests, ContinuouslyDrainsNoisyStderr) {
  std::array<int, 2> stderr_pipe {-1, -1};
  std::array<int, 2> status_pipe {-1, -1};
  ASSERT_EQ(pipe(stderr_pipe.data()), 0);
  ASSERT_EQ(pipe(status_pipe.data()), 0);

  auto collector = std::make_shared<labwc_startup_diagnostics::collector_t>("generation-a");
  std::thread reader {
    labwc_startup_diagnostics::drain_pipes,
    stderr_pipe[0],
    status_pipe[0],
    collector
  };

  constexpr std::size_t chunk_count = 256;
  const std::string chunk(4096, 'x');
  std::atomic_bool write_succeeded {true};
  std::thread writer {[&]() {
    for (std::size_t count = 0; count < chunk_count && write_succeeded; ++count) {
      std::size_t written = 0;
      while (written < chunk.size()) {
        const auto result = write(
          stderr_pipe[1],
          chunk.data() + written,
          chunk.size() - written
        );
        if (result > 0) {
          written += static_cast<std::size_t>(result);
        } else if (result < 0 && errno == EINTR) {
          continue;
        } else {
          write_succeeded = false;
          break;
        }
      }
    }
    if (write(status_pipe[1], "23\n", 3) != 3) {
      write_succeeded = false;
    }
    close(stderr_pipe[1]);
    close(status_pipe[1]);
  }};

  writer.join();
  reader.join();

  ASSERT_TRUE(write_succeeded);
  const auto snapshot = collector->snapshot("generation-a");
  ASSERT_TRUE(snapshot.has_value());
  EXPECT_EQ(snapshot->shell_exit_status, std::optional<int> {23});
  EXPECT_EQ(snapshot->stderr_bytes_seen, chunk.size() * chunk_count);
  EXPECT_TRUE(snapshot->stderr_truncated);
  EXPECT_LE(snapshot->stderr_summary.size(), labwc_startup_diagnostics::max_log_summary_bytes);
}

TEST(LabwcStartupDiagnosticsTests, KeepsTheFirstValidStatus) {
  labwc_startup_diagnostics::collector_t collector {"generation-a"};
  collector.record_shell_exit_status(-1);
  collector.record_shell_exit_status(256);
  collector.record_shell_exit_status(9);
  collector.record_shell_exit_status(0);

  const auto snapshot = collector.snapshot("generation-a");
  ASSERT_TRUE(snapshot.has_value());
  EXPECT_EQ(snapshot->shell_exit_status, std::optional<int> {9});
}

TEST(LabwcStartupDiagnosticsTests, DescriptionIsStableAndOneLine) {
  const labwc_startup_diagnostics::snapshot_t snapshot {
    .shell_exit_status = 127,
    .stderr_bytes_seen = 9000,
    .stderr_truncated = true,
    .stderr_summary = "flatpak: app is not installed"
  };

  EXPECT_EQ(
    labwc_startup_diagnostics::describe_for_log(snapshot),
    "startup_client_exit_status=127 startup_client_stderr_bytes=9000 "
    "startup_client_stderr_truncated=true "
    "startup_client_stderr_tail=[flatpak: app is not installed]"
  );
}

#endif  // __linux__
