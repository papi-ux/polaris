/**
 * @file src/doctor_ai_explanation.h
 * @brief One explicit paired request for an informational AI explanation of a captured Doctor reading (#613).
 *
 * The deterministic Doctor stays the source of truth. A paired owner asks once, with a POST that
 * names the reading it saw; the host checks every claim against its own current state, copies a
 * bounded, typed projection of that reading, and makes at most one attempt with the provider the
 * host owner configured. Nothing here changes a stream, a setting, the controller or a Doctor
 * action: the result is plain prose the client may show beside the deterministic card.
 *
 * Status reads only describe whether the request is possible (descriptor()); they allocate no
 * identity, capture nothing and start no work.
 */
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <nlohmann/json.hpp>

#include "ai_optimizer.h"

namespace doctor_ai_explanation {
  inline constexpr std::size_t k_max_request_bytes = 1024;
  inline constexpr std::size_t k_max_response_bytes = 8192;
  inline constexpr std::size_t k_max_evidence_bytes = 8192;
  inline constexpr std::chrono::milliseconds k_snapshot_lifetime {60000};
  inline constexpr std::chrono::milliseconds k_rate_window {60000};
  inline constexpr std::size_t k_attempts_per_window = 2;
  inline constexpr std::uint64_t k_max_safe_integer = 9007199254740991ULL;

  /** Every result the paired route can return; each has one fixed HTTP status. */
  enum class code_e {
    explained,
    invalid_request,
    body_too_large,
    unsupported_media_type,
    unauthorized,
    permission_required,
    active_owner_required,
    unsupported_scope,
    scope_changed,
    certificate_changed,
    finding_changed,
    snapshot_expired,
    duplicate_request,
    host_ai_disabled,
    provider_not_ready,
    provider_failed,
    invalid_explanation,
    provider_timeout,
    request_busy,
    rate_limited,
    server_stopping,
  };

  std::string_view code_name(code_e code);
  int http_status(code_e code);

  /** The meaningful current finding of one final served Doctor. */
  struct finding_t {
    /// False when a meaningful field is missing or not a short identifier: the request is then unavailable.
    bool supported = false;
    /// SHA-256 (lowercase hex) of the canonical meaningful fields; empty when unsupported.
    std::string fingerprint;
    /// The Doctor's categorical result_id as served (an extra equality fence, never a snapshot identity).
    std::string source_result_id;
  };

  /** Pure: derive the finding from the final served Doctor. Numeric values, ages and prose are excluded. */
  finding_t finding_of(const nlohmann::json &doctor);

  /** Host-side measurements that are not taken from the Doctor object itself. */
  struct projection_inputs_t {
    /// stream_stats capture_path_reason for the same reading.
    std::string capture_reason;
    /// The observed encoder rate (stats.bitrate_kbps), not a request or target.
    std::optional<std::int64_t> observed_encoder_kbps;
  };

  /**
   * Pure: the only evidence a provider may receive. Fixed enums, numbers and host templates; every
   * other Doctor/health field is dropped. Missing or invalid measurements stay null/unavailable.
   * Empty when the bounded projection cannot be built from this reading.
   */
  std::optional<std::string> project_evidence(const nlohmann::json &doctor, const projection_inputs_t &inputs);

  /** The comparison claims of one POST body. */
  struct request_t {
    std::string request_id;
    std::string app_session_id;
    std::uint64_t session_generation = 0;
    std::string source_result_id;
    std::string finding_fingerprint;
    std::uint64_t controller_revision = 0;
  };

  struct parse_result_t {
    code_e code = code_e::invalid_request;
    std::optional<request_t> request;
  };

  /**
   * Strict request admission before any host state is read: media type, no query, <= 1 KiB, one
   * top-level object with exactly the seven keys, no duplicates, no coercion, valid UTF-8 and the
   * contract's patterns and ranges. `body` may hold one byte more than the limit to signal oversize.
   */
  parse_result_t parse_request(std::string_view content_type, std::string_view query_string, std::string_view body);

  /**
   * Bound, check and redact the provider's six-field explanation. Rejects instead of truncating.
   * `identities` are the current private strings (device and app names, IDs) to scrub from prose.
   */
  std::optional<nlohmann::json> sanitize_explanation(const nlohmann::json &explanation,
                                                     const std::vector<std::string> &identities);

  /** Credential, address, private-path, token and known-identity redaction for one string. */
  std::string redact_text(std::string_view text, const std::vector<std::string> &identities);

  /** Unicode code points in valid UTF-8; empty when invalid. */
  std::optional<std::size_t> utf8_code_points(std::string_view text);

  /** The caller's current paired scope, read fresh each time with only short existing guards. */
  struct scope_view_t {
    /// The live certificate record the caller resolves to; null when unpaired or revoked.
    std::shared_ptr<const void> cert_record;
    std::string cert_uuid;
    bool permission_view = false;
    /// Worker/Spaces profile scope: never supported by this request.
    bool unsupported_scope = false;
    /// Stop/shutdown in progress, or the lifecycle snapshot could not be taken.
    bool stopping = false;
    /// Exclusive active owner of exactly one non-viewer stream with matching timing scope.
    bool sole_active_owner = false;
    std::string app_session_id;
    std::uint64_t session_generation = 0;
    /// adaptive_bitrate's existing action_authority_revision.
    std::uint64_t controller_revision = 0;
    /// The final served Doctor for this caller.
    nlohmann::json doctor = nlohmann::json::object();
    projection_inputs_t projection;
    /// Private strings to scrub from provider prose.
    std::vector<std::string> identities;
  };

  /** Host AI configuration as one consistent copy. Credentials never leave the process. */
  struct config_view_t {
    bool enabled = false;
    bool transport_ready = false;
    std::uint64_t generation = 0;
    ai_optimizer::config_t config;
  };

  /** ai_optimizer's current configuration as a config_view_t; probes nothing. */
  config_view_t current_config();

  /** Read-only budget state for the descriptor. */
  struct peek_t {
    bool busy = false;
    bool rate_limited = false;
  };

  /** Pure descriptor served at doctor.ai_explanation_request. */
  nlohmann::json descriptor(const scope_view_t &scope, const config_view_t &config, const peek_t &peek);

  /** What the provider boundary returns to the route. */
  struct provider_outcome_t {
    code_e code = code_e::provider_failed;
    /// Validated six-field explanation when code == explained.
    std::optional<nlohmann::json> explanation;
  };

  struct provider_call_t {
    const ai_optimizer::config_t &config;
    const std::string &evidence;
    const std::atomic<bool> &cancel;
  };

  using provider_t = std::function<provider_outcome_t(const provider_call_t &call)>;
  using scope_reader_t = std::function<scope_view_t()>;
  using steady_clock_fn_t = std::function<std::chrono::steady_clock::time_point()>;
  using writer_t = std::function<void(int http_status, const std::string &body)>;

  /** The production provider: ai_optimizer's bounded paired adapter. */
  provider_outcome_t default_provider(const provider_call_t &call);

  /** Build one response body (always <= 8 KiB for a fallback). */
  struct response_identity_t {
    std::string request_id;
    std::string app_session_id;
    std::uint64_t session_generation = 0;
    std::string source_result_id;
    std::string finding_fingerprint;
    std::uint64_t controller_revision = 0;
    std::string snapshot_id;
    std::optional<std::int64_t> snapshot_age_ms;
  };
  std::string response_body(code_e code,
                            const response_identity_t &identity,
                            std::int64_t expires_in_ms,
                            std::optional<std::int64_t> retry_after_ms,
                            const std::optional<nlohmann::json> &explanation);

  /** Counters a test can read to prove what did not happen. */
  struct counters_t {
    std::uint64_t snapshots_allocated = 0;
    std::uint64_t provider_calls = 0;
    std::uint64_t accepted_attempts = 0;
    std::uint64_t completions = 0;
  };

  /**
   * One owned worker and the request store, scoped to the paired HTTPS server's lifetime.
   * One in-flight request globally, zero queue, two accepted attempts per trailing 60 s globally
   * and per certificate, request-UUID tombstones for the same window only.
   */
  class service_t {
  public:
    struct hooks_t {
      std::function<config_view_t()> read_config;
      provider_t provider;
      steady_clock_fn_t clock;
      /// 32 lowercase hex from a CSPRNG; empty on failure.
      std::function<std::string()> snapshot_id;
    };

    explicit service_t(hooks_t hooks);
    ~service_t();
    service_t(const service_t &) = delete;
    service_t &operator=(const service_t &) = delete;

    /** Read-only budget peek; allocates and records nothing. */
    peek_t peek(std::string_view cert_uuid) const;

    /** Current host AI configuration as the route sees it. */
    config_view_t config() const;

    /**
     * Admit one parsed request. Refusals are written synchronously; an admitted request is written
     * exactly once from the worker after its single provider attempt and final recheck.
     */
    void submit(const request_t &request, const scope_reader_t &read_scope, writer_t write);

    /** Refuse new requests and signal cancellable transports. */
    void stop_accepting();

    /** stop_accepting(), then wait for the in-flight request (bounded by its deadline) and join. */
    void shutdown();

    counters_t counters() const;

  private:
    struct snapshot_t;
    void run();
    void finish(snapshot_t &snapshot, code_e code, const std::optional<nlohmann::json> &explanation);
    std::optional<code_e> recheck(const snapshot_t &snapshot);

    hooks_t _hooks;
    mutable std::mutex _mutex;
    std::condition_variable _changed;
    bool _accepting = true;
    bool _stop = false;
    bool _in_flight = false;
    std::unique_ptr<snapshot_t> _job;
    std::deque<std::chrono::steady_clock::time_point> _global_attempts;
    std::map<std::string, std::deque<std::chrono::steady_clock::time_point>, std::less<>> _cert_attempts;
    std::deque<std::pair<std::string, std::chrono::steady_clock::time_point>> _tombstones;
    std::atomic<bool> _cancel {false};
    counters_t _counters;
    std::thread _worker;
  };

  /** CSPRNG 128-bit identity as 32 lowercase hex; empty when the generator fails. */
  std::string random_snapshot_id();
}  // namespace doctor_ai_explanation
