/** @file
 * FRAME01 private proposal. Passive encoder-setting metadata; no production hooks.
 * All calls require external serialization with backend work and setting changes.
 * Caller evidence is mandatory: this library cannot prove backend application.
 */
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>

namespace video::bitrate_provenance {
  // Supplied by the real owner, independently of broker/trace IDs. Both fields
  // are nonzero. encoder_lifetime is never reused; stream_generation is the real
  // stream generation and may persist across encoder recreation. Exactly one
  // ledger belongs to each owned encoder lifetime; this type cannot prove it.
  struct scope_t {
    std::uint64_t encoder_lifetime = 0;
    std::uint64_t stream_generation = 0;

    friend bool operator==(scope_t a, scope_t b) noexcept {
      return a.encoder_lifetime == b.encoder_lifetime && a.stream_generation == b.stream_generation;
    }
  };

  enum class receipt_origin_e { unavailable, successful_open, backend_reconfiguration };

  // An explicit caller assertion, NOT a capability or proof minted by this type.
  // application_sequence advances only on a real successful backend operation;
  // authority_revision binds that operation to the exact accepted authority.
  // A nominal config, request/ACK, trace value, or no-op reaffirmation is not one.
  struct setting_receipt_t {
    scope_t scope;
    std::uint64_t application_sequence = 0;
    std::uint64_t authority_revision = 0;
    int configured_encoder_kbps = 0;
    receipt_origin_e origin = receipt_origin_e::unavailable;
  };

  class ledger_t;

  // Copyable identifier only. It grants neither backend nor frame admission.
  class submission_t {
  public:
    scope_t scope() const noexcept { return scope_; }
    std::uint64_t sequence() const noexcept { return sequence_; }
    std::int64_t pts() const noexcept { return pts_; }

  private:
    friend class ledger_t;
    // Bookkeeping identity only: never a backend receipt or encoder lifetime.
    struct cookie_t {};
    submission_t(scope_t scope, std::uint64_t sequence, std::int64_t pts,
                 const std::shared_ptr<const cookie_t> &owner) noexcept:
        scope_ {scope}, sequence_ {sequence}, pts_ {pts}, owner_ {owner} {}
    scope_t scope_;
    std::uint64_t sequence_;
    std::int64_t pts_;
    std::weak_ptr<const cookie_t> owner_;
  };

  // Immutable through its public API. A value may survive the ledger/encoder;
  // no API accepts it back as permission, a receipt, or a live submission.
  class record_t {
  public:
    scope_t scope() const noexcept { return receipt_.scope; }
    std::uint64_t submission_sequence() const noexcept { return submission_sequence_; }
    std::int64_t input_pts() const noexcept { return input_pts_; }
    std::uint64_t application_sequence() const noexcept { return receipt_.application_sequence; }
    std::uint64_t authority_revision() const noexcept { return receipt_.authority_revision; }
    int configured_encoder_kbps() const noexcept { return receipt_.configured_encoder_kbps; }
    receipt_origin_e origin() const noexcept { return receipt_.origin; }

  private:
    friend class ledger_t;
    record_t(setting_receipt_t receipt, std::uint64_t sequence, std::int64_t pts) noexcept:
        receipt_ {receipt}, submission_sequence_ {sequence}, input_pts_ {pts} {}
    setting_receipt_t receipt_;
    std::uint64_t submission_sequence_;
    std::int64_t input_pts_;
  };

  enum class submission_result_e {
    failed_or_unproven,
    // Caller attests success and that ALL rate-dependent work for this exact
    // submission used the captured setting, including convert/retained work.
    submitted_under_captured_setting
  };

  class ledger_t {
  public:
    static constexpr std::size_t capacity = 64;

    explicit ledger_t(scope_t scope) noexcept:
        scope_ {scope}, retired_ {scope.encoder_lifetime == 0 || scope.stream_generation == 0} {
      if (retired_) return;
      try {
        owner_ = std::make_shared<const submission_t::cookie_t>();
      } catch (const std::bad_alloc &) {
        retired_ = true;  // Metadata allocation must never fail codec admission.
      }
    }
    ledger_t(const ledger_t &) = delete;
    ledger_t &operator=(const ledger_t &) = delete;
    ledger_t(ledger_t &&) = delete;
    ledger_t &operator=(ledger_t &&) = delete;

    // Observe AFTER a successful backend operation and BEFORE subsequent work.
    // Any observation invalidates in-flight captures. Committed old frames keep
    // their values. Returning false means metadata unavailable, never codec error.
    bool observe_receipt(const setting_receipt_t &receipt) noexcept {
      setting_unavailable();
      if (retired_ || !(receipt.scope == scope_) || receipt.configured_encoder_kbps <= 0 ||
          receipt.application_sequence <= last_application_ || receipt.authority_revision <= last_authority_) {
        return false;
      }
      if (receipt.origin == receipt_origin_e::successful_open) {
        if (started_ || last_application_ != 0) return false;
      } else if (receipt.origin == receipt_origin_e::backend_reconfiguration) {
        // No-op acknowledgements cannot establish a new application edge. This
        // conservative API also leaves genuine same-kbps reconfiguration unknown.
        if (receipt.configured_encoder_kbps == last_kbps_) return false;
      } else {
        return false;
      }
      last_application_ = receipt.application_sequence;
      last_authority_ = receipt.authority_revision;
      last_kbps_ = receipt.configured_encoder_kbps;
      current_ = receipt;
      return true;
    }

    // Call when backend state or its authority binding cannot be established.
    // Never substitute config/request/ACK as a fallback. Old committed outputs
    // remain observable; uncommitted work no longer has a provable interval.
    void setting_unavailable() noexcept {
      current_.reset();
      for (auto &entry : entries_) {
        if (!entry.committed) entry.value.reset();
      }
    }

    // Call once for EVERY attempted input, before its first rate-dependent work,
    // even when metadata is unavailable/full. PTS is the exact encoder input PTS,
    // nonnegative and strictly increasing in this scope, not capture time or DTS.
    // Missing/duplicate/regressed PTS permanently retires this metadata scope:
    // otherwise an untracked ambiguous input might alias an older output.
    std::optional<submission_t> capture(std::optional<std::int64_t> pts) noexcept {
      if (retired_) return std::nullopt;
      started_ = true;
      if (!pts || *pts < 0 || (last_pts_ && *pts <= *last_pts_)) {
        retire();
        return std::nullopt;
      }
      last_pts_ = *pts;  // Also reserve unavailable/overflow PTS against reuse.
      if (!current_) return std::nullopt;
      for (auto &entry : entries_) {
        if (entry.value) continue;
        if (last_submission_ == std::numeric_limits<std::uint64_t>::max()) {
          retire();  // No wrapping identity or slot-reuse ABA.
          return std::nullopt;
        }
        ++last_submission_;
        entry.value = record_t {*current_, last_submission_, *pts};
        entry.committed = false;
        return submission_t {scope_, last_submission_, *pts, owner_};
      }
      return std::nullopt;  // Never evict or borrow the current setting at output.
    }

    // Call only after the exact backend submission/work succeeded, before any
    // output lookup. On failure/unproven interval, cancel metadata only. Copies
    // of a consumed/cancelled ticket cannot commit again or affect a reused slot.
    bool complete(const submission_t &submission, submission_result_e result) noexcept {
      if (retired_ || !(submission.scope() == scope_) || submission.owner_.lock() != owner_) return false;
      for (auto &entry : entries_) {
        if (!entry.value || entry.committed ||
            entry.value->submission_sequence() != submission.sequence() ||
            entry.value->input_pts() != submission.pts()) continue;
        if (result != submission_result_e::submitted_under_captured_setting) {
          entry.value.reset();
          return false;
        }
        entry.committed = true;
        return true;
      }
      return false;
    }

    // Lookup consumes ONLY the exact matching PTS in the exact live scope.
    // Pending output is unproven and cancelled; absent/mismatched output does
    // not disturb unrelated delayed records. One record per input/output unit;
    // multi-packet callers must carry the returned value, not call take twice.
    std::optional<record_t> take(scope_t output_scope, std::optional<std::int64_t> pts) noexcept {
      if (retired_ || !(output_scope == scope_) || !pts || *pts < 0) return std::nullopt;
      for (auto &entry : entries_) {
        if (!entry.value || entry.value->input_pts() != *pts) continue;
        auto result = entry.committed ? entry.value : std::nullopt;
        entry.value.reset();
        return result;
      }
      return std::nullopt;
    }

    // Encoder/stream retirement is terminal. Recreate with an independently
    // owned, never-reused scope; no reset/rebind or authority-cloning operation.
    void retire() noexcept {
      retired_ = true;
      owner_.reset();  // Detached handles expire even if this address is reused.
      current_.reset();
      for (auto &entry : entries_) entry.value.reset();
    }

  private:
    struct entry_t {
      std::optional<record_t> value;
      bool committed = false;
    };
    const scope_t scope_;
    // One allocation per ledger; weak tickets cannot keep live authority alive.
    // This cookie only fences local handles, never establishes real scope proof.
    std::shared_ptr<const submission_t::cookie_t> owner_;
    bool retired_;
    bool started_ = false;
    std::uint64_t last_application_ = 0;
    std::uint64_t last_authority_ = 0;
    int last_kbps_ = 0;
    std::uint64_t last_submission_ = 0;
    std::optional<std::int64_t> last_pts_;
    std::optional<setting_receipt_t> current_;
    std::array<entry_t, capacity> entries_ {};
  };
}  // namespace video::bitrate_provenance
