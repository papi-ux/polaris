/**
 * @file tests/unit/test_crash_report.cpp
 * @brief Test run-outcome classification and its persisted contract.
 */
#include <filesystem>
#include <gtest/gtest.h>
#include <src/crash_report.h>
#include <src/logging.h>
#include <string>

#ifndef _WIN32
  #include <csignal>
  #include <sys/resource.h>
  #include <sys/wait.h>
  #include <unistd.h>
#endif

using namespace std::literals;

namespace {
  namespace fs = std::filesystem;

  crash_report::run_state_t running_state() {
    crash_report::run_state_t state;
    state.run_id = "2026-08-17T10:00:00Z-4242";
    state.version = "1.3.11";
    state.pid = 4242;
    state.started_at = "2026-08-17T10:00:00Z";
    state.log_path = "/var/log/polaris/polaris.log";
    state.status = "running";
    return state;
  }

  std::string evidence_for(const std::string &run_id, const std::string &signal_line = "signal: 11 SIGSEGV") {
    return std::string {crash_report::crash_evidence_magic} +
           "\nrun_id: " + run_id +
           "\nversion: 1.3.11\npid: 4242\n" + signal_line +
           "\naddress: 0x0000000000000000\nbacktrace:\npolaris(+0x1234)\n";
  }

  class CrashReportStateDir: public ::testing::Test {
  protected:
    void SetUp() override {
      root_ = fs::temp_directory_path() / ("polaris-crash-report-" + std::to_string(counter_++));
      fs::remove_all(root_);
      crash_report::reset_for_tests();
    }

    void TearDown() override {
      crash_report::reset_for_tests();
      std::error_code error;
      fs::remove_all(root_, error);
    }

    fs::path root_;

  private:
    static int counter_;
  };

  int CrashReportStateDir::counter_ = 0;
}  // namespace

TEST(CrashReportRunStateTests, SerializesAndParsesRoundTrip) {
  auto state = running_state();
  state.status = "clean";
  state.ended_at = "2026-08-17T11:00:00Z";
  state.shutdown_reason = "SIGTERM received";
  state.exit_code = 3;

  const auto parsed = crash_report::parse_run_state(crash_report::serialize_run_state(state));

  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->run_id, state.run_id);
  EXPECT_EQ(parsed->version, state.version);
  EXPECT_EQ(parsed->pid, state.pid);
  EXPECT_EQ(parsed->started_at, state.started_at);
  EXPECT_EQ(parsed->log_path, state.log_path);
  EXPECT_EQ(parsed->status, state.status);
  EXPECT_EQ(parsed->ended_at, state.ended_at);
  EXPECT_EQ(parsed->shutdown_reason, state.shutdown_reason);
  EXPECT_EQ(parsed->exit_code, state.exit_code);
}

TEST(CrashReportRunStateTests, RejectsUnusablePayloads) {
  EXPECT_FALSE(crash_report::parse_run_state(""sv).has_value());
  EXPECT_FALSE(crash_report::parse_run_state("not json"sv).has_value());
  EXPECT_FALSE(crash_report::parse_run_state("[]"sv).has_value());
  // A document from a schema this build does not understand must not be read
  // field by field on the assumption the names still mean the same thing.
  EXPECT_FALSE(crash_report::parse_run_state(R"({"schema_version":99,"status":"clean"})"sv).has_value());
}

TEST(CrashReportClassifyTests, ReportsUnknownWithoutAPreviousRun) {
  const auto previous = crash_report::classify_previous_run(std::nullopt, ""sv);

  EXPECT_EQ(previous.outcome, crash_report::outcome_e::unknown);
  EXPECT_TRUE(previous.evidence.empty());
}

TEST(CrashReportClassifyTests, ReportsCleanWhenTheRunRecordedItsExit) {
  auto state = running_state();
  state.status = "clean";
  state.shutdown_reason = "SIGINT received";
  state.exit_code = 0;

  const auto previous = crash_report::classify_previous_run(state, ""sv);

  EXPECT_EQ(previous.outcome, crash_report::outcome_e::clean);
  EXPECT_EQ(previous.shutdown_reason, "SIGINT received");
}

TEST(CrashReportClassifyTests, ReportsUncleanWhenNoExitAndNoEvidence) {
  // SIGKILL, the OOM killer, and power loss all land here: the run never got to
  // record an exit and never got to write evidence either.
  const auto previous = crash_report::classify_previous_run(running_state(), ""sv);

  EXPECT_EQ(previous.outcome, crash_report::outcome_e::unclean);
  EXPECT_EQ(previous.signal_number, 0);
  EXPECT_TRUE(previous.evidence.empty());
}

TEST(CrashReportClassifyTests, ReportsCrashedWithTheSignalItDiedOn) {
  const auto state = running_state();

  const auto previous = crash_report::classify_previous_run(state, evidence_for(state.run_id));

  EXPECT_EQ(previous.outcome, crash_report::outcome_e::crashed);
  EXPECT_EQ(previous.signal_number, 11);
  EXPECT_EQ(previous.signal_name, "SIGSEGV");
  EXPECT_NE(previous.evidence.find("backtrace:"), std::string::npos);
  EXPECT_EQ(previous.log_path, state.log_path);
}

TEST(CrashReportClassifyTests, IgnoresEvidenceLeftByAnEarlierRun) {
  // The evidence file is deliberately not consumed on read, so a crash from
  // three runs ago is still on disk. Attributing it to an unrelated run would
  // report a crash that did not happen.
  const auto previous = crash_report::classify_previous_run(
    running_state(),
    evidence_for("2026-08-01T09:00:00Z-1111")
  );

  EXPECT_EQ(previous.outcome, crash_report::outcome_e::unclean);
  EXPECT_TRUE(previous.evidence.empty());
  EXPECT_EQ(previous.signal_number, 0);
}

TEST(CrashReportClassifyTests, IgnoresEvidenceThatIsNotOurs) {
  const auto state = running_state();
  const auto foreign = "some other tool's crash file\nrun_id: " + state.run_id + "\n";

  const auto previous = crash_report::classify_previous_run(state, foreign);

  EXPECT_EQ(previous.outcome, crash_report::outcome_e::unclean);
  EXPECT_TRUE(previous.evidence.empty());
}

TEST(CrashReportClassifyTests, SurvivesEvidenceWithoutAParsableSignal) {
  const auto state = running_state();

  const auto previous = crash_report::classify_previous_run(state, evidence_for(state.run_id, "signal: not-a-number"));

  EXPECT_EQ(previous.outcome, crash_report::outcome_e::crashed);
  EXPECT_EQ(previous.signal_number, 0);
}

TEST(CrashReportClassifyTests, DescribesOutcomesWithStableWireNames) {
  EXPECT_EQ(crash_report::describe_outcome(crash_report::outcome_e::unknown), "unknown"sv);
  EXPECT_EQ(crash_report::describe_outcome(crash_report::outcome_e::clean), "clean"sv);
  EXPECT_EQ(crash_report::describe_outcome(crash_report::outcome_e::crashed), "crashed"sv);
  EXPECT_EQ(crash_report::describe_outcome(crash_report::outcome_e::unclean), "unclean"sv);
}

TEST(CrashReportClassifyTests, RendersTheDiagnosticsPayload) {
  const auto state = running_state();

  const auto document = crash_report::to_json(crash_report::classify_previous_run(state, evidence_for(state.run_id)));

  EXPECT_EQ(document["outcome"], "crashed");
  EXPECT_TRUE(document["recorded"].get<bool>());
  EXPECT_EQ(document["signal_name"], "SIGSEGV");
  EXPECT_EQ(document["version"], "1.3.11");
  EXPECT_FALSE(document["evidence"].get<std::string>().empty());
}

TEST(CrashReportClassifyTests, MarksAnAbsentPreviousRunAsNotRecorded) {
  const auto document = crash_report::to_json(crash_report::classify_previous_run(std::nullopt, ""sv));

  EXPECT_EQ(document["outcome"], "unknown");
  EXPECT_FALSE(document["recorded"].get<bool>());
}

TEST_F(CrashReportStateDir, FirstRunHasNoPreviousOutcome) {
  crash_report::begin_run(root_, (root_ / "polaris.log").string(), "1.3.11");

  EXPECT_EQ(crash_report::previous_run().outcome, crash_report::outcome_e::unknown);
  EXPECT_TRUE(fs::exists(root_ / std::string {crash_report::run_state_file_name}));
}

TEST_F(CrashReportStateDir, ARecordedExitIsReadBackAsClean) {
  crash_report::begin_run(root_, (root_ / "polaris.log").string(), "1.3.11");
  crash_report::note_clean_exit(0, "SIGTERM received");

  crash_report::reset_for_tests();
  crash_report::begin_run(root_, (root_ / "polaris.log").string(), "1.3.11");

  const auto &previous = crash_report::previous_run();
  EXPECT_EQ(previous.outcome, crash_report::outcome_e::clean);
  EXPECT_EQ(previous.shutdown_reason, "SIGTERM received");
  EXPECT_EQ(previous.version, "1.3.11");
}

TEST_F(CrashReportStateDir, AMissingExitIsReadBackAsUnclean) {
  crash_report::begin_run(root_, (root_ / "polaris.log").string(), "1.3.11");
  // No note_clean_exit(): this is what a killed process leaves behind.

  crash_report::reset_for_tests();
  crash_report::begin_run(root_, (root_ / "polaris.log").string(), "1.3.11");

  EXPECT_EQ(crash_report::previous_run().outcome, crash_report::outcome_e::unclean);
}

#ifndef _WIN32
TEST_F(CrashReportStateDir, AFatalSignalLeavesEvidenceTheNextRunReadsBack) {
  // The handler cannot be exercised in-process: it re-raises so the default
  // disposition still dumps core, which would take the test runner with it.
  // A forked child dies for real, exactly as Polaris would, and the parent then
  // plays the part of the next start.
  const auto child = ::fork();
  ASSERT_NE(child, -1);

  if (child == 0) {
    // Do not litter the developer's machine or CI with core files just to prove
    // the handler ran.
    const rlimit no_core {0, 0};
    (void) ::setrlimit(RLIMIT_CORE, &no_core);

    crash_report::begin_run(root_, (root_ / "polaris.log").string(), "1.3.11");
    crash_report::install_fatal_handlers();
    std::raise(SIGSEGV);
    ::_exit(0);  // Only reached if the handler swallowed the signal, which is a bug.
  }

  int status = 0;
  ASSERT_EQ(::waitpid(child, &status, 0), child);
  // Re-raising rather than exiting is what keeps coredumpctl working, so assert
  // the child really did die of the original signal.
  ASSERT_TRUE(WIFSIGNALED(status)) << "child exited instead of dying on the signal";
  EXPECT_EQ(WTERMSIG(status), SIGSEGV);

  crash_report::reset_for_tests();
  crash_report::begin_run(root_, (root_ / "polaris.log").string(), "1.3.11");

  const auto &previous = crash_report::previous_run();
  EXPECT_EQ(previous.outcome, crash_report::outcome_e::crashed);
  EXPECT_EQ(previous.signal_number, SIGSEGV);
  EXPECT_EQ(previous.signal_name, "SIGSEGV");
  EXPECT_EQ(previous.version, "1.3.11");
  EXPECT_NE(previous.evidence.find("backtrace:"), std::string::npos);
}
#endif

TEST(BackupLogPathTests, NamesTheBackupBesideTheActiveLog) {
  EXPECT_EQ(logging::backup_log_path("/var/log/polaris.log"), "/var/log/polaris.log.backup");
}

TEST(BackupLogPathTests, HasNoBackupWhenLoggingIsConsoleOnly) {
  EXPECT_TRUE(logging::backup_log_path("").empty());
}
