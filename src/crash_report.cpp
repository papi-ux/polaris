/**
 * @file src/crash_report.cpp
 * @brief Definitions for run-outcome tracking and fatal-signal evidence capture.
 */
// standard includes
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <exception>

// local includes
#include "crash_report.h"
#include "logging.h"
#include "private_state_file.h"

#ifdef _WIN32
  #include <process.h>
#else
  #include <csignal>
  #include <fcntl.h>
  #include <sys/stat.h>
  #include <unistd.h>
#endif

#if defined(__APPLE__) || (defined(__linux__) && defined(__GLIBC__))
  #define POLARIS_CRASH_HAVE_BACKTRACE 1
  #include <execinfo.h>
#endif

using namespace std::literals;

namespace crash_report {
  namespace {
    // Everything the signal handler is allowed to read lives here, preformatted
    // while ordinary code is still running. A handler may not allocate, take a
    // lock, or call into the logging framework, so the only work left at crash
    // time is opening a file descriptor and writing bytes that already exist.
    constexpr std::size_t max_path_bytes = 4096;
    constexpr std::size_t max_header_bytes = 1024;
    constexpr std::size_t max_terminate_bytes = 512;
    constexpr int max_frames = 64;
    // Not SIGSTKSZ: current glibc defines it as a sysconf() call rather than a
    // constant expression, so it cannot size a static array.
    constexpr std::size_t alternate_stack_bytes = 64U * 1024U;

    char crash_path_buffer[max_path_bytes] = {};
    char crash_header_buffer[max_header_bytes] = {};
    std::size_t crash_header_length = 0;
    char terminate_message_buffer[max_terminate_bytes] = {};
    std::atomic_bool handlers_armed {false};

    previous_run_t previous_run_state;
    run_state_t current_run_state;
    std::filesystem::path current_state_path;
    bool run_begun = false;

    struct signal_name_entry_t {
      int number;
      const char *name;
    };

    // Read from signal context, so it must be a table of literals and nothing else.
    constexpr std::array<signal_name_entry_t, 6> signal_names {{
#ifndef _WIN32
      {SIGSEGV, "SIGSEGV"},
      {SIGABRT, "SIGABRT"},
      {SIGBUS, "SIGBUS"},
      {SIGFPE, "SIGFPE"},
      {SIGILL, "SIGILL"},
      {SIGSYS, "SIGSYS"},
#else
      {0, ""},
      {0, ""},
      {0, ""},
      {0, ""},
      {0, ""},
      {0, ""},
#endif
    }};

    const char *signal_name_for(int number) {
      for (const auto &entry : signal_names) {
        if (entry.number == number && entry.name[0] != '\0') {
          return entry.name;
        }
      }
      return "UNKNOWN";
    }

    std::string utc_timestamp_now() {
      const std::time_t now = std::time(nullptr);
      std::tm parts {};
#ifdef _WIN32
      if (gmtime_s(&parts, &now) != 0) {
        return {};
      }
#else
      if (gmtime_r(&now, &parts) == nullptr) {
        return {};
      }
#endif
      char buffer[32] = {};
      if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%dT%H:%M:%SZ", &parts) == 0) {
        return {};
      }
      return buffer;
    }

    void copy_into(char *destination, std::size_t capacity, std::string_view source) {
      const std::size_t length = std::min(source.size(), capacity - 1);
      std::memcpy(destination, source.data(), length);
      destination[length] = '\0';
    }

    /**
     * @brief Extract a single-line `label: value` field from crash evidence.
     */
    std::string evidence_field(std::string_view evidence, std::string_view label) {
      std::size_t cursor = 0;
      while (cursor < evidence.size()) {
        const auto line_end = evidence.find('\n', cursor);
        const auto line = evidence.substr(cursor, line_end == std::string_view::npos ? std::string_view::npos : line_end - cursor);
        if (line.size() > label.size() && line.compare(0, label.size(), label) == 0) {
          auto value = line.substr(label.size());
          while (!value.empty() && (value.front() == ' ' || value.front() == '\t')) {
            value.remove_prefix(1);
          }
          while (!value.empty() && (value.back() == '\r' || value.back() == ' ')) {
            value.remove_suffix(1);
          }
          return std::string {value};
        }
        if (line_end == std::string_view::npos) {
          break;
        }
        cursor = line_end + 1;
      }
      return {};
    }

#ifndef _WIN32
    // Async-signal-safe primitives. ::write is on the POSIX safe list; nothing
    // here allocates, formats through stdio, or touches a lock.
    void write_all(int descriptor, const char *data, std::size_t length) {
      std::size_t written = 0;
      while (written < length) {
        const auto result = ::write(descriptor, data + written, length - written);
        if (result <= 0) {
          if (result < 0 && errno == EINTR) {
            continue;
          }
          return;
        }
        written += static_cast<std::size_t>(result);
      }
    }

    void write_literal(int descriptor, const char *text) {
      write_all(descriptor, text, std::strlen(text));
    }

    void write_decimal(int descriptor, long long value) {
      char buffer[24];
      std::size_t index = sizeof(buffer);
      const bool negative = value < 0;
      unsigned long long magnitude = negative ? 0ULL - static_cast<unsigned long long>(value) : static_cast<unsigned long long>(value);
      do {
        buffer[--index] = static_cast<char>('0' + (magnitude % 10));
        magnitude /= 10;
      } while (magnitude != 0 && index > 0);
      if (negative && index > 0) {
        buffer[--index] = '-';
      }
      write_all(descriptor, buffer + index, sizeof(buffer) - index);
    }

    void write_hex_pointer(int descriptor, const void *pointer) {
      static constexpr char digits[] = "0123456789abcdef";
      auto value = reinterpret_cast<unsigned long long>(pointer);
      char buffer[2 + (sizeof(value) * 2)];
      buffer[0] = '0';
      buffer[1] = 'x';
      for (std::size_t index = 0; index < sizeof(value) * 2; ++index) {
        const auto shift = ((sizeof(value) * 2) - 1 - index) * 4;
        buffer[2 + index] = digits[(value >> shift) & 0xF];
      }
      write_all(descriptor, buffer, sizeof(buffer));
    }

    extern "C" void fatal_signal_handler(int signal_number, siginfo_t *info, void *) {
      // A fault inside the handler must not loop back into it.
      static volatile sig_atomic_t already_entered = 0;
      if (already_entered) {
        ::_exit(128 + signal_number);
      }
      already_entered = 1;

      const int descriptor = ::open(crash_path_buffer, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
      if (descriptor >= 0) {
        write_all(descriptor, crash_header_buffer, crash_header_length);

        write_literal(descriptor, "signal: ");
        write_decimal(descriptor, signal_number);
        write_literal(descriptor, " ");
        write_literal(descriptor, signal_name_for(signal_number));
        write_literal(descriptor, "\n");

        write_literal(descriptor, "address: ");
        write_hex_pointer(descriptor, info != nullptr ? info->si_addr : nullptr);
        write_literal(descriptor, "\n");

        if (terminate_message_buffer[0] != '\0') {
          write_literal(descriptor, "terminate: ");
          write_literal(descriptor, terminate_message_buffer);
          write_literal(descriptor, "\n");
        }

        write_literal(descriptor, "backtrace:\n");
  #ifdef POLARIS_CRASH_HAVE_BACKTRACE
        void *frames[max_frames];
        const int frame_count = ::backtrace(frames, max_frames);
        ::backtrace_symbols_fd(frames, frame_count, descriptor);
  #else
        write_literal(descriptor, "(backtrace unavailable in this build)\n");
  #endif

        ::fsync(descriptor);
        ::close(descriptor);
      }

      // Hand the signal back to the default disposition so the core dump still
      // happens and coredumpctl keeps producing a symbolised backtrace.
      struct sigaction restore {};
      restore.sa_handler = SIG_DFL;
      sigemptyset(&restore.sa_mask);
      restore.sa_flags = 0;
      ::sigaction(signal_number, &restore, nullptr);
      ::raise(signal_number);
    }
#endif

    void terminate_handler() {
      // Ordinary context, so this may allocate. It runs before abort() turns
      // into SIGABRT, and the message it leaves is picked up by that handler.
      const char *message = "uncaught exception";
      std::string detail;
      if (auto pending = std::current_exception()) {
        try {
          std::rethrow_exception(pending);
        } catch (const std::exception &error) {
          detail = error.what();
          message = detail.c_str();
        } catch (...) {
          message = "uncaught non-standard exception";
        }
      }
      copy_into(terminate_message_buffer, sizeof(terminate_message_buffer), message);
      std::abort();
    }

    run_state_t build_current_state(const std::string &log_path, const std::string &version) {
      run_state_t state;
      state.schema_version = run_state_schema_version;
      state.version = version;
      state.started_at = utc_timestamp_now();
#ifdef _WIN32
      state.pid = static_cast<long long>(::_getpid());
#else
      state.pid = static_cast<long long>(::getpid());
#endif
      state.log_path = log_path;
      state.status = "running";
      state.run_id = state.started_at + "-" + std::to_string(state.pid);
      return state;
    }

    void persist(const run_state_t &state) {
      if (current_state_path.empty()) {
        return;
      }
      const auto result = private_state_file::write_atomic(current_state_path, serialize_run_state(state));
      if (!result) {
        BOOST_LOG(warning) << "Could not persist the run-state document; crash detection will be degraded on the next start."sv;
      }
    }
  }  // namespace

  std::string serialize_run_state(const run_state_t &state) {
    nlohmann::json document;
    document["schema_version"] = state.schema_version;
    document["run_id"] = state.run_id;
    document["version"] = state.version;
    document["pid"] = state.pid;
    document["started_at"] = state.started_at;
    document["log_path"] = state.log_path;
    document["status"] = state.status;
    document["ended_at"] = state.ended_at;
    document["shutdown_reason"] = state.shutdown_reason;
    document["exit_code"] = state.exit_code;
    return document.dump();
  }

  std::optional<run_state_t> parse_run_state(std::string_view payload) {
    if (payload.empty()) {
      return std::nullopt;
    }

    const auto document = nlohmann::json::parse(payload, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
      return std::nullopt;
    }
    if (document.value("schema_version", 0) != run_state_schema_version) {
      return std::nullopt;
    }

    run_state_t state;
    state.schema_version = run_state_schema_version;
    state.run_id = document.value("run_id", std::string {});
    state.version = document.value("version", std::string {});
    state.pid = document.value("pid", 0LL);
    state.started_at = document.value("started_at", std::string {});
    state.log_path = document.value("log_path", std::string {});
    state.status = document.value("status", std::string {});
    state.ended_at = document.value("ended_at", std::string {});
    state.shutdown_reason = document.value("shutdown_reason", std::string {});
    state.exit_code = document.value("exit_code", 0);
    return state;
  }

  previous_run_t classify_previous_run(
    const std::optional<run_state_t> &state,
    std::string_view crash_evidence
  ) {
    previous_run_t previous;
    if (!state) {
      return previous;
    }

    previous.version = state->version;
    previous.run_id = state->run_id;
    previous.started_at = state->started_at;
    previous.ended_at = state->ended_at;
    previous.shutdown_reason = state->shutdown_reason;
    previous.log_path = state->log_path;
    previous.pid = state->pid;
    previous.exit_code = state->exit_code;

    if (state->status == "clean") {
      previous.outcome = outcome_e::clean;
      return previous;
    }

    // The run never recorded an exit. Evidence decides whether that was a fatal
    // signal or something that gave Polaris no chance to record anything, such
    // as the OOM killer, SIGKILL, or power loss.
    previous.outcome = outcome_e::unclean;

    const bool evidence_is_ours =
      crash_evidence.size() > crash_evidence_magic.size() &&
      crash_evidence.compare(0, crash_evidence_magic.size(), crash_evidence_magic) == 0;
    if (!evidence_is_ours) {
      return previous;
    }

    // Evidence from an older run must not be reported against this one.
    const auto evidence_run_id = evidence_field(crash_evidence, "run_id:");
    if (evidence_run_id.empty() || evidence_run_id != state->run_id) {
      return previous;
    }

    previous.outcome = outcome_e::crashed;
    previous.evidence = std::string {crash_evidence};

    const auto signal_field = evidence_field(crash_evidence, "signal:");
    if (!signal_field.empty()) {
      const auto separator = signal_field.find(' ');
      const auto number_text = signal_field.substr(0, separator);
      try {
        previous.signal_number = std::stoi(number_text);
      } catch (const std::exception &) {
        previous.signal_number = 0;
      }
      if (separator != std::string::npos) {
        previous.signal_name = signal_field.substr(separator + 1);
      }
    }

    return previous;
  }

  std::string_view describe_outcome(outcome_e outcome) {
    switch (outcome) {
      case outcome_e::clean:
        return "clean"sv;
      case outcome_e::crashed:
        return "crashed"sv;
      case outcome_e::unclean:
        return "unclean"sv;
      case outcome_e::unknown:
      default:
        return "unknown"sv;
    }
  }

  nlohmann::json to_json(const previous_run_t &previous) {
    nlohmann::json document;
    document["outcome"] = describe_outcome(previous.outcome);
    document["recorded"] = previous.outcome != outcome_e::unknown;
    document["version"] = previous.version;
    document["run_id"] = previous.run_id;
    document["started_at"] = previous.started_at;
    document["ended_at"] = previous.ended_at;
    document["shutdown_reason"] = previous.shutdown_reason;
    document["log_path"] = previous.log_path;
    document["pid"] = previous.pid;
    document["exit_code"] = previous.exit_code;
    document["signal_number"] = previous.signal_number;
    document["signal_name"] = previous.signal_name;
    document["evidence"] = previous.evidence;
    return document;
  }

  void begin_run(
    const std::filesystem::path &state_dir,
    const std::string &log_path,
    const std::string &version
  ) {
    if (run_begun) {
      return;
    }
    run_begun = true;

    std::error_code directory_error;
    std::filesystem::create_directories(state_dir, directory_error);

    current_state_path = state_dir / std::string {run_state_file_name};
    const auto crash_path = state_dir / std::string {crash_evidence_file_name};

    const auto stored = private_state_file::read_secure(current_state_path, max_run_state_bytes);
    const auto evidence = private_state_file::read_secure(crash_path, max_crash_evidence_bytes);
    previous_run_state = classify_previous_run(
      stored ? parse_run_state(stored.payload) : std::nullopt,
      evidence ? std::string_view {evidence.payload} : std::string_view {}
    );

    switch (previous_run_state.outcome) {
      case outcome_e::crashed:
        BOOST_LOG(error) << "The previous Polaris run crashed on "sv << previous_run_state.signal_name
                         << ". Crash evidence is available from the Troubleshooting screen."sv;
        break;
      case outcome_e::unclean:
        BOOST_LOG(warning) << "The previous Polaris run did not record an exit and left no crash evidence, "
                              "which usually means it was killed rather than that it faulted."sv;
        break;
      case outcome_e::clean:
      case outcome_e::unknown:
      default:
        break;
    }

    current_run_state = build_current_state(log_path, version);
    persist(current_run_state);

    // Preformat everything the signal handler may touch, while allocation is
    // still legal.
    copy_into(crash_path_buffer, sizeof(crash_path_buffer), crash_path.string());
    const auto header = "polaris-crash-v1\nrun_id: " + current_run_state.run_id +
                        "\nversion: " + current_run_state.version +
                        "\npid: " + std::to_string(current_run_state.pid) + "\n";
    copy_into(crash_header_buffer, sizeof(crash_header_buffer), header);
    crash_header_length = std::strlen(crash_header_buffer);
  }

  void note_clean_exit(int exit_code, const char *shutdown_reason) {
    if (!run_begun) {
      return;
    }
    current_run_state.status = "clean";
    current_run_state.ended_at = utc_timestamp_now();
    current_run_state.exit_code = exit_code;
    current_run_state.shutdown_reason = shutdown_reason != nullptr ? shutdown_reason : "";
    persist(current_run_state);
  }

  void install_fatal_handlers() {
    if (handlers_armed.exchange(true)) {
      return;
    }

    std::set_terminate(terminate_handler);

#ifndef _WIN32
  #ifdef POLARIS_CRASH_HAVE_BACKTRACE
    // backtrace() loads the unwinder lazily and that first call allocates, so
    // force it here rather than inside a handler that may not allocate.
    void *warmup[2];
    (void) ::backtrace(warmup, 2);
  #endif

    // A stack-overflow SIGSEGV cannot be handled on the overflowed stack.
    static std::array<char, alternate_stack_bytes> alternate_stack;
    stack_t signal_stack {};
    signal_stack.ss_sp = alternate_stack.data();
    signal_stack.ss_size = alternate_stack.size();
    signal_stack.ss_flags = 0;
    (void) ::sigaltstack(&signal_stack, nullptr);

    struct sigaction action {};
    action.sa_sigaction = fatal_signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO | SA_ONSTACK;

    for (const auto &entry : signal_names) {
      if (entry.number != 0) {
        ::sigaction(entry.number, &action, nullptr);
      }
    }
#endif
  }

  const previous_run_t &previous_run() {
    return previous_run_state;
  }

#ifdef POLARIS_TESTS
  void reset_for_tests() {
    previous_run_state = {};
    current_run_state = {};
    current_state_path.clear();
    run_begun = false;
    crash_path_buffer[0] = '\0';
    crash_header_buffer[0] = '\0';
    crash_header_length = 0;
    terminate_message_buffer[0] = '\0';
  }
#endif
}  // namespace crash_report
