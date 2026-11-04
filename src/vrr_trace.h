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
#include "vrr_pacing_receipt.h"
#include "vrr_session_policy.h"

namespace vrr_trace {
  inline constexpr unsigned k_schema_major = 2;
  // Minor 5 adds passive native_signal_context/native_signal_sample companion rows;
  // minor 4 added native_admission/native_admission_sample rows (shapes unchanged).
  // minor 3 added historical packet records at dequeue; minor 2 added host_history_run.
  // Readers must explicitly support this minor; older reader predicates stay unchanged.
  inline constexpr unsigned k_schema_minor = 5;
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

  // Copied under stats_mutex with the event counter mutation. No trace queue lock is involved.
  struct pacing_observation_t {
    std::optional<std::uint64_t> accepted_sequence;
    std::optional<std::uint64_t> event_count_after;
  };
  void record_pacing_receipt(const vrr_pacing::receipt_t &receipt, bool accepted,
                            const pacing_observation_t &observation) noexcept;

  struct bitrate_observation_t {
    std::uint64_t scope_generation = 0;
    std::uint64_t trace_scope = 0;
    int requested_before_kbps = 0;
    std::uint64_t requested_before_revision = 0;
    int applied_before_kbps = 0;
    std::uint64_t applied_before_revision = 0;
  };
  void record_bitrate_update(event_t encoder_identity, const bitrate_observation_t &before,
                            int requested_kbps, std::uint64_t request_revision, int encoder_before_kbps,
                            const char *static_result, std::optional<int> backend_result) noexcept;

  enum class policy_source_e : unsigned {
    unavailable = 0, explicit_launch_request = 1, paired_client = 2, client_launch_request = 3,
    device_profile_v1 = 4, capability_validation = 5, composed_display_components = 6,
    runtime_policy = 7, explicit_doctor_trial = 8,
  };
  policy_source_e policy_source(std::string_view source) noexcept;
  struct session_policy_t {
    bool available = false;
    int requested_fps_milli = 0;
    int resolved_fps_milli = 0;
    policy_source_e display_mode_source = policy_source_e::unavailable;
    policy_source_e target_fps_source = policy_source_e::unavailable;
    policy_source_e width_source = policy_source_e::unavailable;
    policy_source_e height_source = policy_source_e::unavailable;
  };
  void record_session_policy(event_t encoder_identity, const session_policy_t &policy) noexcept;

  // The ended session's frozen variable-refresh request and pacing runtime, as values.
  struct host_history_vrr_t {
    std::uint64_t session_generation = 0;
    vrr_policy::requested_e requested = vrr_policy::requested_e::absent;
    vrr_policy::admission_e admission = vrr_policy::admission_e::launch;
    bool host_source_pacing = false;
    bool watch_or_input_only = false;
  };
  struct host_history_pacing_t {
    vrr_pacing::mode_e mode = vrr_pacing::mode_e::waiting;
    bool ended = false;
    bool authority_lost = false;
    std::uint64_t receipt_session_generation = 0;
    std::uint64_t receipt_capture_generation = 0;
    std::optional<std::uint64_t> accepted_watermark;
  };

  // One host pause: the retained ended session it read and what its history writer returned.
  // Values only, filled when the trace is on. Text is static: known_* results or literals.
  // Host process identity is the header's; this row reads no /proc and is not a write time.
  struct host_history_run_t {
    // stream::join's ending media session generation; 0 when the caller named none.
    std::uint64_t expected_session_generation = 0;
    // proc's capture generation. Resume keeps it, so it never names a media run.
    std::uint64_t capture_generation = 0;
    bool client_report_recorded = false;
    // Whether pause read stats.last_session at all, and the generation it held.
    bool snapshot_read = false;
    std::optional<std::uint64_t> ended_session_generation;
    std::optional<host_history_vrr_t> vrr;
    std::optional<host_history_pacing_t> pacing;
    // Empty is unknown.
    std::string_view codec;
    std::string_view encoder_backend;
    std::string_view start_outcome;
    session_policy_t launch_policy;
    // Why the history writer was not called; empty when it was.
    std::string_view writer_skip;
    bool writer_called = false;
    // The writer's own observation names, meaningful only when it was called.
    std::string_view writer_feedback;
    std::string_view writer_merge;
    std::string_view writer_write;
  };
  // Map a runtime string to the same static text, or empty when it is not a known value.
  std::string_view known_codec(std::string_view value) noexcept;
  std::string_view known_encoder_backend(std::string_view value) noexcept;
  std::string_view known_start_outcome(std::string_view value) noexcept;
  // Ended facts appear only when the snapshot's generation equals the expected one; a slot that
  // another session's end replaced, or none, leaves them null instead of guessing.
  event_t host_history_run_event(const host_history_run_t &run) noexcept;
  void record_host_history_run(const host_history_run_t &run) noexcept;

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
