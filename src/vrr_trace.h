/** @file src/vrr_trace.h
 * Private, passive host observations. Schema major 2; unrelated to the process v1 qualifiers.
 * Keys, event names and text values passed to an event MUST have static lifetime.
 */
#pragma once

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>

namespace vrr_trace {
  inline constexpr unsigned k_schema_major = 2;
  inline constexpr std::size_t k_fields = 40;
  inline constexpr std::size_t k_queue_capacity = 512;
  inline constexpr std::uint64_t k_event_budget = 2'000'000;
  inline constexpr std::uint64_t k_byte_budget = 256 * 1024 * 1024;
  inline constexpr auto k_writer_lifetime = std::chrono::minutes(30);

  struct field_t {
    enum class type_e { null, signed_integer, unsigned_integer, text, real };
    const char *key = nullptr;
    type_e type = type_e::null;
    std::int64_t i = 0;
    std::uint64_t u = 0;
    double d = 0;
    std::string_view text;
  };

  // Fixed-size values only. No heap, formatting, file access, waiting or live authority.
  struct event_t {
    explicit event_t(const char *kind = "invalid"): kind(kind) {}
    const char *kind;
    std::uint64_t order = 0;  // total queue-admission order; loss is counted separately in footer
    std::int64_t observed_steady_ns = -1;
    std::array<field_t, k_fields> fields {};
    std::size_t size = 0;
    bool overflow = false;
    event_t &u(const char *key, std::uint64_t value) noexcept;
    event_t &i(const char *key, std::int64_t value) noexcept;
    event_t &s(const char *key, std::string_view static_value) noexcept;
    event_t &d(const char *key, double value) noexcept;
    event_t &n(const char *key) noexcept;
    event_t &o(const char *key, std::optional<std::int64_t> value) noexcept;
  };

  enum class status_e { off, running, refused, stopped };
  struct limits_t {
    std::uint64_t events = k_event_budget;
    std::uint64_t bytes = k_byte_budget;
#ifdef POLARIS_TESTS
    // Deterministic writer scheduling/fault injection, absent from the shipped producer.
    const std::atomic<bool> *test_hold_writer = nullptr;
    bool test_write_failure = false;
    bool test_close_failure = false;
#endif
  };

  class recorder_t {
  public:
    // Setup/teardown only, never call from a frame or packet loop. Linux only; other OSes refuse.
    static std::unique_ptr<recorder_t> open(std::string_view absolute_path, limits_t limits = {}) noexcept;
    ~recorder_t();
    recorder_t(const recorder_t &) = delete;
    recorder_t &operator=(const recorder_t &) = delete;
    // Bounded atomic reservation. Contention/full/overflow/cap are counted; callers never wait.
    void record(event_t event) noexcept;
    void stop() noexcept;
    status_e status() const noexcept;
    int refusal_errno() const noexcept;
  private:
    recorder_t();
    struct impl_t;
    std::unique_ptr<impl_t> impl_;
  };

#ifdef POLARIS_TESTS
  // Isolate real producer calls in a local test recorder; never present in production builds.
  class test_recorder_scope_t {
  public:
    explicit test_recorder_scope_t(recorder_t *recorder) noexcept;
    ~test_recorder_scope_t();
  private:
    recorder_t *previous_;
  };
#endif

  // Read environment exactly once at a capture/encoder setup seam. Absent/empty means OFF.
  // POLARIS_VRR_TRACE_V2 is an absolute local collection path; no endpoint or config default.
  void start_from_environment() noexcept;
  bool enabled() noexcept;
  status_e status() noexcept;
  int refusal_errno() noexcept;
  void record(event_t event) noexcept;
  std::uint64_t next_id() noexcept;  // process-wide, never reused; 0 when off/exhausted
  std::optional<std::int64_t> ns(std::optional<std::chrono::steady_clock::time_point> at) noexcept;

  // One socket API invocation, not physical delivery. Failure/exception exposes unknown partial
  // progress; whole-range fallback is explicitly related by retry_of. Never owns packet buffers.
  class send_attempt_t {
  public:
    send_attempt_t(std::uint64_t packet, int block, std::int64_t first, std::uint64_t count,
                   const char *api, std::uint64_t retry_of = 0) noexcept;
    ~send_attempt_t();
    void finish(bool api_result) noexcept;
    std::uint64_t id() const noexcept { return id_; }
  private:
    std::uint64_t id_ = 0, packet_ = 0, count_ = 0, retry_of_ = 0;
    int block_ = 0;
    std::int64_t first_ = 0;
    const char *api_ = nullptr;
    bool finished_ = false;
  };

  // Portable bounded PTS join. Mirrors submission_times_t's ordered erase semantics without
  // mutating it. A legacy delayed packet's actual timing stays absent even if its identity joins.
  class submissions_t {
  public:
    explicit submissions_t(const char *static_table = "identity"): table_(static_table) {}
    static constexpr std::size_t k_capacity = 64;
    struct entry_t {
      bool used = false;
      std::int64_t pts = 0;
      std::uint64_t id = 0;
    };
    // Every removal gets an explicit event (capacity, overwrite, ordered take or retirement).
    void put(std::int64_t pts, std::uint64_t id, std::uint64_t encoder) noexcept;
    std::optional<entry_t> take(std::int64_t pts, std::uint64_t encoder, bool ordered_erase) noexcept;
    void clear(std::uint64_t encoder, const char *reason) noexcept;
  private:
    const char *table_;
    std::array<entry_t, k_capacity> entries_ {};
  };
}  // namespace vrr_trace
