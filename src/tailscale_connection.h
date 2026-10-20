/** Read-only, address-free Tailscale evidence. Never a media-quality or trust policy. */
#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <nlohmann/json.hpp>

namespace tailscale_connection {
  constexpr std::int64_t max_age_ms = 10000;
  constexpr std::size_t max_status_bytes = 512 * 1024;
  /// The fixed daemon socket. Never taken from a request.
  constexpr std::string_view default_socket_path = "/var/run/tailscale/tailscaled.sock";

  // control_connection requires both actual TLS endpoints; peer_status describes
  // only a daemon peer matched to a current stream's address, never its media route.
  nlohmann::json unknown(std::string_view scope, std::string_view reason,
                         std::string_view address_kind = "unknown");
  nlohmann::json from_status(std::string_view status, std::string_view remote_ip,
                             std::string_view local_ip, std::int64_t observed_at_ms,
                             std::string_view scope = "control_connection");
  std::optional<std::string> read_status_socket(std::string_view socket_path);

  /// The production read: the fixed socket on Linux, nothing elsewhere.
  std::optional<std::string> read_default_status();
  /// Evidence for one request from one daemon read; no read gives unknown.
  nlohmann::json evaluate(const std::optional<std::string> &status, std::int64_t observed_at_ms,
                          std::string_view remote_ip, std::string_view local_ip = {},
                          std::string_view scope = "control_connection");
  std::int64_t wall_clock_ms();

  /**
   * Daemon reads for one HTTPS server, off that server's threads.
   *
   * Handlers submit a waiter and return at once, so a slow or stalled daemon never holds the
   * server. One worker thread performs one daemon read at a time; every waiter queued when a read
   * starts shares that read, and requests arriving during it wait for the next one, so an answer
   * is never older than its request. The raw status is handed to the waiters and then dropped:
   * no peer inventory, address or identity outlives the read.
   *
   * Bounded: at most max_waiters requests are accepted and unanswered at once, waiting and in the
   * read under way together. A full or stopped collector refuses the waiter and the caller answers
   * unknown now. Every accepted waiter is called exactly once, never under the collector's lock. stop() answers every waiter that has not been answered with no
   * status (unknown) and joins the worker; the owner calls it before tearing its server down.
   */
  class status_collector_t {
  public:
    using reader_t = std::function<std::optional<std::string>()>;
    using wall_clock_t = std::function<std::int64_t()>;
    /// Called once, on the worker, with the shared read (or nothing) and its wall-clock time.
    using waiter_t = std::function<void(const std::optional<std::string> &status, std::int64_t observed_at_ms)>;
    static constexpr std::size_t max_waiters = 32;

    explicit status_collector_t(reader_t reader = read_default_status, wall_clock_t clock = wall_clock_ms);
    ~status_collector_t();
    status_collector_t(const status_collector_t &) = delete;
    status_collector_t &operator=(const status_collector_t &) = delete;

    /// False when stopped or full; the waiter is then never called.
    bool submit(waiter_t waiter);
    void stop();
    /// Daemon reads performed so far.
    std::size_t reads() const;
#ifdef POLARIS_TESTS
    /// Tests only: true once at least `count` accepted waiters are queued for a read that has not
    /// started, within `timeout`; false on timeout or once stopped. Never wakes the worker.
    bool wait_queued_for_tests(std::size_t count, std::chrono::milliseconds timeout);
#endif

  private:
    void run();

    reader_t reader;
    wall_clock_t clock;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<waiter_t> waiting;
    bool stopping = false;
    /// Waiters taken by the worker and not yet answered; counts toward max_waiters.
    std::size_t in_flight = 0;
    std::size_t read_count = 0;
#ifdef POLARIS_TESTS
    /// Separate from `wake`, so a test waiting on the queue never takes the worker's wakeup.
    std::condition_variable queued_for_tests;
#endif
    std::thread worker;
  };

#ifdef POLARIS_TESTS
  /**
   * Replace the daemon read of collectors built with the default reader, for tests that drive the
   * production routes. Never reachable from a request; reset with an empty function.
   */
  void set_default_status_reader_for_tests(status_collector_t::reader_t reader);
#endif
}  // namespace tailscale_connection
