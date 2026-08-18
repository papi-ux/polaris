/**
 * @file src/crash_report.h
 * @brief Run-outcome tracking and fatal-signal evidence capture.
 *
 * Polaris could not previously tell a user whether it had crashed. The log of a
 * run that died is preserved as the bounded backup log, but nothing recorded
 * that the run ended abnormally, so a support report could not distinguish a
 * segfault from an ordinary quit.
 *
 * This module records one small state document per run and, when a fatal signal
 * arrives, writes a backtrace from inside the signal handler. On the next start
 * the two are read back together and classified.
 */
#pragma once

// standard includes
#include <cstddef>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

// lib includes
#include <nlohmann/json.hpp>

namespace crash_report {
  /** @brief Schema version of the persisted run-state document. */
  inline constexpr int run_state_schema_version = 1;

  /** @brief First line of a crash evidence file, used to reject foreign files. */
  inline constexpr std::string_view crash_evidence_magic = "polaris-crash-v1";

  /** @brief Largest crash evidence file read back on the next start. */
  inline constexpr std::size_t max_crash_evidence_bytes = 64U * 1024U;

  /** @brief Largest run-state document read back on the next start. */
  inline constexpr std::size_t max_run_state_bytes = 8U * 1024U;

  /** @brief File name of the run-state document inside the state directory. */
  inline constexpr std::string_view run_state_file_name = "last_run.json";

  /** @brief File name of the crash evidence inside the state directory. */
  inline constexpr std::string_view crash_evidence_file_name = "last_crash.txt";

  /**
   * @brief What happened to the run before this one.
   */
  enum class outcome_e {
    unknown,  ///< No usable run-state was recorded, typically a first start.
    clean,    ///< The previous run recorded its own exit.
    crashed,  ///< The previous run died on a fatal signal and left evidence.
    unclean,  ///< The previous run never recorded an exit and left no evidence.
  };

  /**
   * @brief The persisted per-run document.
   */
  struct run_state_t {
    int schema_version = run_state_schema_version;
    std::string run_id;
    std::string version;
    long long pid = 0;
    std::string started_at;
    std::string log_path;
    std::string status;  ///< "running" while live, "clean" once an exit is recorded.
    std::string ended_at;
    std::string shutdown_reason;
    int exit_code = 0;
  };

  /**
   * @brief The classified result read back on the next start.
   */
  struct previous_run_t {
    outcome_e outcome = outcome_e::unknown;
    std::string version;
    std::string run_id;
    std::string started_at;
    std::string ended_at;
    std::string shutdown_reason;
    std::string log_path;
    long long pid = 0;
    int exit_code = 0;
    int signal_number = 0;
    std::string signal_name;
    std::string evidence;  ///< Raw crash evidence text; empty unless outcome is crashed.
  };

  /**
   * @brief Serialize a run-state document.
   */
  std::string serialize_run_state(const run_state_t &state);

  /**
   * @brief Parse a run-state document.
   * @return The parsed state, or nullopt when the payload is malformed or
   *         carries a schema version this build does not understand.
   */
  std::optional<run_state_t> parse_run_state(std::string_view payload);

  /**
   * @brief Decide what happened to the previous run.
   *
   * Crash evidence is only attributed to the previous run when its recorded
   * `run_id` matches. Evidence left by an older run is ignored rather than
   * reported against an unrelated one, which is what makes it safe to leave the
   * evidence file on disk instead of consuming it.
   *
   * @param state The previous run-state document, if one was readable.
   * @param crash_evidence Raw contents of the crash evidence file, or empty.
   */
  previous_run_t classify_previous_run(
    const std::optional<run_state_t> &state,
    std::string_view crash_evidence
  );

  /**
   * @brief Stable wire name for an outcome.
   */
  std::string_view describe_outcome(outcome_e outcome);

  /**
   * @brief Render a classified previous run for the diagnostics API.
   */
  nlohmann::json to_json(const previous_run_t &previous);

  /**
   * @brief Read the previous run's outcome, then record this run as running.
   *
   * Call once, after logging is initialized so the classification can be logged,
   * and before anything that can crash. The state directory is passed in rather
   * than resolved here so the module stays linkable without the platform layer.
   *
   * @param state_dir Directory holding the run-state and crash evidence files.
   * @param log_path Active log path recorded for this run.
   * @param version Polaris version recorded for this run.
   */
  void begin_run(
    const std::filesystem::path &state_dir,
    const std::string &log_path,
    const std::string &version
  );

  /**
   * @brief Record that this run is ending on its own terms.
   *
   * Must be reached by ordinary control flow. Deliberately not called from a
   * signal handler: the write path is not async-signal-safe, and a run killed
   * before it returns is genuinely an unclean exit.
   */
  void note_clean_exit(int exit_code, const char *shutdown_reason);

  /**
   * @brief Install fatal-signal handlers that write crash evidence.
   *
   * Requires begin_run() first, which is what preformats the buffers the
   * handler is allowed to touch. The handler restores the default disposition
   * and re-raises, so core dumps and `coredumpctl` keep working unchanged.
   */
  void install_fatal_handlers();

  /**
   * @brief The previous run as classified by begin_run().
   */
  const previous_run_t &previous_run();

#ifdef POLARIS_TESTS
  /**
   * @brief Reset process-wide state so tests can drive begin_run() repeatedly.
   */
  void reset_for_tests();
#endif
}  // namespace crash_report
