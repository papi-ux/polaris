/**
 * @file src/logging.cpp
 * @brief Definitions for logging related functions.
 */
// standard includes
#include <atomic>
#include <filesystem>
#include <iomanip>
#include <iostream>

// lib includes
#include <boost/core/null_deleter.hpp>
#include <boost/date_time/posix_time/posix_time.hpp>
#include <boost/format.hpp>
#include <boost/log/attributes/clock.hpp>
#include <boost/log/common.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/sinks/basic_sink_backend.hpp>
#include <boost/log/sinks.hpp>
#include <boost/log/sources/severity_logger.hpp>

// local includes
#include "bounded_log_file.h"
#include "logging.h"

// conditional includes
#ifdef __ANDROID__
  #include <android/log.h>
#else
  #include <display_device/logging.h>
#endif

extern "C" {
#include <libavutil/log.h>
}

using namespace std::literals;

namespace bl = boost::log;

boost::shared_ptr<text_sink> sink;
std::atomic_uint64_t active_log_file_generation {0};

namespace {
  using bounded_file_backend_base_t = boost::log::sinks::basic_formatted_sink_backend<
    char,
    boost::log::sinks::combine_requirements<
      boost::log::sinks::synchronized_feeding,
      boost::log::sinks::flushing
    >::type
  >;

  class bounded_file_backend_t: public bounded_file_backend_base_t {
  public:
    bounded_file_backend_t(
      const std::filesystem::path &active_path,
      const std::filesystem::path &backup_path,
      const std::uintmax_t max_bytes
    ):
        file_(active_path, backup_path, max_bytes, []() {
          active_log_file_generation.fetch_add(1, std::memory_order_release);
        }) {
    }

    void consume(const boost::log::record_view &, const string_type &formatted_message) {
      const auto result = file_.write_record(formatted_message);
      if (result == logging::bounded_log_write_result_e::rejected && !reported_failure_) {
        std::cerr << "Polaris stopped writing the active log because its bounded file backend failed." << std::endl;
        reported_failure_ = true;
      }
    }

    void flush() {
      file_.flush();
    }

    [[nodiscard]] bool good() const {
      return file_.good();
    }

    bool clear() {
      const auto cleared = file_.clear();
      if (cleared) {
        reported_failure_ = false;
      }
      return cleared;
    }

  private:
    logging::bounded_log_file_t file_;
    bool reported_failure_ = false;
  };

  using bounded_file_queue_t = boost::log::sinks::bounded_fifo_queue<
    async_log_queue_capacity,
    boost::log::sinks::block_on_overflow
  >;
  using bounded_file_sink_t = boost::log::sinks::asynchronous_sink<
    bounded_file_backend_t,
    bounded_file_queue_t
  >;
  boost::shared_ptr<bounded_file_sink_t> file_sink;
  std::unique_ptr<logging::bounded_log_owner_lock_t> runtime_log_owner_lock;
}  // namespace

bl::sources::severity_logger<int> verbose(0);  // Dominating output
bl::sources::severity_logger<int> debug(1);  // Follow what is happening
bl::sources::severity_logger<int> info(2);  // Should be informed about
bl::sources::severity_logger<int> warning(3);  // Strange events
bl::sources::severity_logger<int> error(4);  // Recoverable errors
bl::sources::severity_logger<int> fatal(5);  // Unrecoverable errors
#ifdef POLARIS_TESTS
bl::sources::severity_logger<int> tests(10);  // Automatic tests output
#endif

BOOST_LOG_ATTRIBUTE_KEYWORD(severity, "Severity", int)

namespace logging {
  deinit_t::~deinit_t() {
    deinit();
  }

  void deinit() {
    log_flush();
    if (file_sink) {
      bl::core::get()->remove_sink(file_sink);
      file_sink.reset();
    }
    runtime_log_owner_lock.reset();
    if (sink) {
      bl::core::get()->remove_sink(sink);
      sink.reset();
    }
  }

  void formatter(const boost::log::record_view &view, boost::log::formatting_ostream &os) {
    constexpr const char *message = "Message";
    constexpr const char *severity = "Severity";

    auto log_level = view.attribute_values()[severity].extract<int>().get();

    std::string_view log_type;
    switch (log_level) {
      case 0:
        log_type = "Verbose: "sv;
        break;
      case 1:
        log_type = "Debug: "sv;
        break;
      case 2:
        log_type = "Info: "sv;
        break;
      case 3:
        log_type = "Warning: "sv;
        break;
      case 4:
        log_type = "Error: "sv;
        break;
      case 5:
        log_type = "Fatal: "sv;
        break;
#ifdef POLARIS_TESTS
      case 10:
        log_type = "Tests: "sv;
        break;
#endif
    };

    // Print the RECORD's creation time, not "now". This formatter runs on the
    // asynchronous sink's consumer thread, so with wall-clock-at-format the
    // printed time is the FLUSH time — observed live 2026-08-10: the consumer
    // stalled behind journald backpressure for a whole streaming session and
    // then drained a 37,500-line backlog in one second, every line stamped
    // with the drain moment. That fabricated a convincing-but-fake "second
    // launch" transcript (a full RTSP handshake apparently completing in 1 ms)
    // and made the real teardown impossible to reconstruct from the journal.
    // The TimeStamp attribute is stamped at record creation by the global
    // local_clock registered in init(); the wall-clock fallback only covers
    // records logged before init() has run.
    boost::posix_time::ptime record_time;
    if (auto stamp = view.attribute_values()["TimeStamp"].extract<boost::posix_time::ptime>()) {
      record_time = stamp.get();
    } else {
      record_time = boost::posix_time::microsec_clock::local_time();
    }

    auto lt = boost::posix_time::to_tm(record_time);
    const auto ms = record_time.time_of_day().total_milliseconds() % 1000;

    os << "["sv << std::put_time(&lt, "%Y-%m-%d %H:%M:%S.") << boost::format("%03u") % ms << "]: "sv
       << log_type;

    // Cap any single record so one pathological message (e.g. a broken GL driver
    // returning a multi-megabyte info log) cannot flood the log file unbounded.
    constexpr std::size_t max_message_chars = 16 * 1024;
    auto message_value = view.attribute_values()[message].extract<std::string>();
    if (message_value && message_value.get().size() > max_message_chars) {
      const auto &full = message_value.get();
      os << std::string_view(full.data(), max_message_chars)
         << "\u2026 [truncated "sv << (full.size() - max_message_chars) << " chars]"sv;
    } else {
      os << message_value;
    }
  }
#ifdef __ANDROID__
  namespace sinks = boost::log::sinks;
  namespace expr = boost::log::expressions;

  void android_log(const std::string &message, int severity) {
    android_LogPriority android_priority;
    switch (severity) {
      case 0:
        android_priority = ANDROID_LOG_VERBOSE;
        break;
      case 1:
        android_priority = ANDROID_LOG_DEBUG;
        break;
      case 2:
        android_priority = ANDROID_LOG_INFO;
        break;
      case 3:
        android_priority = ANDROID_LOG_WARN;
        break;
      case 4:
        android_priority = ANDROID_LOG_ERROR;
        break;
      case 5:
        android_priority = ANDROID_LOG_FATAL;
        break;
      default:
        android_priority = ANDROID_LOG_UNKNOWN;
        break;
    }
    __android_log_print(android_priority, "Sunshine", "%s", message.c_str());
  }

  // custom sink backend for android
  struct android_sink_backend: public sinks::basic_sink_backend<sinks::concurrent_feeding> {
    void consume(const bl::record_view &rec) {
      int log_sev = rec[severity].get();
      const std::string log_msg = rec[expr::smessage].get();
      // log to android
      android_log(log_msg, log_sev);
    }
  };
#endif

  [[nodiscard]] std::unique_ptr<deinit_t> init(int min_log_level, const std::string &log_file) {
    if (sink || file_sink) {
      // Deinitialize the logging system before reinitializing it. This can probably only ever be hit in tests.
      deinit();
    }

    // Check if the log file exists and handle backup. An empty path requests
    // console-only logging for commands that must not initialize user state.
    std::string backup_log_file;
    if (!log_file.empty()) {
      backup_log_file = log_file + ".backup";
    }
    auto file_logging_ready = !log_file.empty();
    if (file_logging_ready) {
      // Single owner per config directory: a second Polaris reaching this init
      // while another process owns the runtime log must not inherit, rotate,
      // or truncate the owner's files out from under its live sink (#393).
      runtime_log_owner_lock = std::make_unique<bounded_log_owner_lock_t>(log_file + ".lock");
      if (!runtime_log_owner_lock->owned()) {
        std::cout << "Another Polaris process owns the runtime log; continuing with console-only logging." << std::endl;
        runtime_log_owner_lock.reset();
        file_logging_ready = false;
      }
    }
    if (file_logging_ready && !bounded_log_file_t::preserve_existing(
                                log_file,
                                backup_log_file,
                                runtime_log_max_bytes
                              )) {
      std::cout << "Failed to preserve a bounded backup of the prior log file." << std::endl;
      // Fail closed instead of truncating the only surviving copy below.
      file_logging_ready = false;
    }

#ifndef __ANDROID__
    setup_av_logging(min_log_level);
    setup_libdisplaydevice_logging(min_log_level);
#endif

    sink = boost::make_shared<text_sink>();

#ifndef POLARIS_TESTS
    boost::shared_ptr<std::ostream> stream {&std::cout, boost::null_deleter()};
    sink->locked_backend()->add_stream(stream);
#endif

    if (file_logging_ready) {
      auto backend = boost::make_shared<bounded_file_backend_t>(
        log_file,
        backup_log_file,
        runtime_log_max_bytes
      );
      if (backend->good()) {
        file_sink = boost::make_shared<bounded_file_sink_t>(backend);
        file_sink->set_filter(severity >= min_log_level);
        file_sink->set_formatter(&formatter);
        active_log_file_generation.fetch_add(1, std::memory_order_release);
      } else {
        std::cout << "Failed to open the bounded active log file." << std::endl;
      }
    }
    sink->set_filter(severity >= min_log_level);
    sink->set_formatter(&formatter);

    // Flush after each log record to ensure log file contents on disk isn't stale.
    // This is particularly important when running from a Windows service.
    sink->locked_backend()->auto_flush(true);

    // Stamp every record with its creation time. The formatter runs later, on
    // the asynchronous sink's consumer thread, and must print this attribute
    // rather than the wall clock — otherwise a stalled consumer rewrites
    // history with flush-time stamps (see the formatter comment). Idempotent
    // across re-init: the attribute survives deinit() and a duplicate add
    // would be rejected, so only add it when absent.
    if (bl::core::get()->get_global_attributes().count("TimeStamp") == 0) {
      bl::core::get()->add_global_attribute("TimeStamp", bl::attributes::local_clock());
    }

    bl::core::get()->add_sink(sink);
    if (file_sink) {
      bl::core::get()->add_sink(file_sink);
    }

#ifdef __ANDROID__
    auto android_sink = boost::make_shared<sinks::synchronous_sink<android_sink_backend>>();
    bl::core::get()->add_sink(android_sink);
#endif
    return std::make_unique<deinit_t>();
  }

#ifndef __ANDROID__
  void setup_av_logging(int min_log_level) {
    if (min_log_level >= 1) {
      av_log_set_level(AV_LOG_QUIET);
    } else {
      av_log_set_level(AV_LOG_DEBUG);
    }
    av_log_set_callback([](void *ptr, int level, const char *fmt, va_list vl) {
      static int print_prefix = 1;
      char buffer[1024];

      av_log_format_line(ptr, level, fmt, vl, buffer, sizeof(buffer), &print_prefix);
      if (level <= AV_LOG_ERROR) {
        // We print AV_LOG_FATAL at the error level. FFmpeg prints things as fatal that
        // are expected in some cases, such as lack of codec support or similar things.
        BOOST_LOG(error) << buffer;
      } else if (level <= AV_LOG_WARNING) {
        BOOST_LOG(warning) << buffer;
      } else if (level <= AV_LOG_INFO) {
        BOOST_LOG(info) << buffer;
      } else if (level <= AV_LOG_VERBOSE) {
        // AV_LOG_VERBOSE is less verbose than AV_LOG_DEBUG
        BOOST_LOG(debug) << buffer;
      } else {
        BOOST_LOG(verbose) << buffer;
      }
    });
  }

  void setup_libdisplaydevice_logging(int min_log_level) {
    constexpr int min_level {static_cast<int>(display_device::Logger::LogLevel::verbose)};
    constexpr int max_level {static_cast<int>(display_device::Logger::LogLevel::fatal)};
    const auto log_level {static_cast<display_device::Logger::LogLevel>(std::min(std::max(min_level, min_log_level), max_level))};

    display_device::Logger::get().setLogLevel(log_level);
    display_device::Logger::get().setCustomCallback([](const display_device::Logger::LogLevel level, const std::string &message) {
      switch (level) {
        case display_device::Logger::LogLevel::verbose:
          BOOST_LOG(verbose) << message;
          break;
        case display_device::Logger::LogLevel::debug:
          BOOST_LOG(debug) << message;
          break;
        case display_device::Logger::LogLevel::info:
          BOOST_LOG(info) << message;
          break;
        case display_device::Logger::LogLevel::warning:
          BOOST_LOG(warning) << message;
          break;
        case display_device::Logger::LogLevel::error:
          BOOST_LOG(error) << message;
          break;
        case display_device::Logger::LogLevel::fatal:
          BOOST_LOG(fatal) << message;
          break;
      }
    });
  }
#endif

  void log_flush() {
    if (sink) {
      sink->flush();
    }
    if (file_sink) {
      file_sink->flush();
    }
  }

  std::uint64_t log_file_generation() {
    return active_log_file_generation.load(std::memory_order_acquire);
  }

  bool clear_log_file() {
    if (!file_sink) {
      return false;
    }

    // Clear only depends on the file queue. The console consumer can be blocked
    // by journald or a full pipe and must not prevent log-file recovery.
    file_sink->flush();

    auto backend = file_sink->locked_backend();
    const auto reopened = backend->clear();
    if (reopened) {
      active_log_file_generation.fetch_add(1, std::memory_order_release);
    }
    return reopened;
  }

  void print_help(const char *name) {
    std::cout
      << "Usage: "sv << name << " [options] [/path/to/configuration_file] [--cmd]"sv << std::endl
      << "    Any configurable option can be overwritten with: \"name=value\""sv << std::endl
      << std::endl
      << "    Note: The configuration will be created if it doesn't exist."sv << std::endl
      << std::endl
      << "    --help                    | print help"sv << std::endl
      << "    --creds username password | set Web UI credentials; restart Polaris afterwards"sv << std::endl
#ifdef __linux__
      << "    --setup-host [--enable-kms] | apply Linux udev/modules setup explicitly"sv << std::endl
#endif
      << "    --version                 | print the version of sunshine"sv << std::endl
      << std::endl
      << "    flags"sv << std::endl
      << "        -0 | Read PIN from stdin"sv << std::endl
      << "        -1 | Do not load previously saved state and do retain any state after shutdown"sv << std::endl
      << "           | Effectively starting as if for the first time without overwriting any pairings with your devices"sv << std::endl
      << "        -2 | Force replacement of headers in video stream"sv << std::endl
      << "        -p | Enable/Disable UPnP"sv << std::endl
      << std::endl;
  }

  std::string bracket(const std::string &input) {
    return "["s + input + "]"s;
  }

  std::wstring bracket(const std::wstring &input) {
    return L"["s + input + L"]"s;
  }

}  // namespace logging
