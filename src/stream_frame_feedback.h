/**
 * @file src/stream_frame_feedback.h
 * @brief Frame-submission feedback (#611): a bounded per-stream sender ledger and its interval join.
 *
 * Observational only. Nothing here is a controller, FEC, pacing, bitrate or Doctor input, and it
 * never changes what the sender sends or how. It answers one question for an interval of frames a
 * client says it submitted to its decoder: did every one of those frames leave this host's send
 * path, with a known API disposition, encoded under one exact encoder setting? Anything that cannot
 * be shown frame by frame is unavailable, with a reason. An unavailable interval is never loss,
 * never clean, and never a measurement of network capacity or achieved encoder rate.
 *
 * Two clocks stay apart: the sender's steady clock and the client's own snapshot clock. Neither is
 * compared with the other. The only time bound used is the existing client media coverage bound,
 * passed in by the owner.
 */
#pragma once

#include "video_bitrate_provenance.h"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace stream_frame_feedback {
  using clock = std::chrono::steady_clock;

  /// Diagnostic retention of the sender ledger. A retention limit only, never a policy threshold.
  inline constexpr std::size_t k_ledger_capacity = 4096;
  inline constexpr int k_version = 1;

  /// The immutable encoder-setting tuple a packet carries. Equality is the whole tuple: two
  /// applications at the same kbps are different settings.
  struct encoder_setting_t {
    std::uint64_t encoder_lifetime = 0;
    std::uint64_t stream_generation = 0;
    std::uint64_t application_sequence = 0;
    std::uint64_t origin_revision = 0;
    int configured_encoder_kbps = 0;
    video::bitrate_provenance::receipt_origin_e receipt_origin = video::bitrate_provenance::receipt_origin_e::unavailable;

    friend bool operator==(const encoder_setting_t &, const encoder_setting_t &) = default;
  };

  inline std::optional<encoder_setting_t> encoder_setting_of(
      const std::optional<const video::bitrate_provenance::record_t> &record) noexcept {
    if (!record) return std::nullopt;
    return encoder_setting_t {
      record->scope().encoder_lifetime,
      record->scope().stream_generation,
      record->application_sequence(),
      record->authority_revision(),
      record->configured_encoder_kbps(),
      record->origin(),
    };
  }

  /// A connection or launch nonce: exactly 32 lowercase hexadecimal characters, not all zero.
  inline bool valid_nonce(std::string_view text) noexcept {
    if (text.size() != 32) return false;
    bool nonzero = false;
    for (const char c : text) {
      if (!(c >= '0' && c <= '9') && !(c >= 'a' && c <= 'f')) return false;
      nonzero = nonzero || c != '0';
    }
    return nonzero;
  }

  inline const char *receipt_origin_name(video::bitrate_provenance::receipt_origin_e origin) noexcept {
    switch (origin) {
      case video::bitrate_provenance::receipt_origin_e::successful_open: return "successful_open";
      case video::bitrate_provenance::receipt_origin_e::backend_reconfiguration: return "backend_reconfiguration";
      default: return "unavailable";
    }
  }

  /// What became of one frame's send path. Only sent is a complete, unambiguous API disposition.
  enum class send_disposition_e : std::uint8_t {
    pending,  ///< Recorded at the acquired destination; its send path has not finished.
    sent,  ///< The whole path finished and every send API call for the frame returned success.
    send_failed,  ///< An individual send call returned failure.
    batch_fallback,  ///< A batch returned false: partial acceptance then fallback may omit or duplicate datagrams.
    cancelled,  ///< The destination was retired during or after the frame's sends.
    exception,  ///< The send path threw.
    incomplete,  ///< The path ended without reporting how.
    not_sent,  ///< The path ended before any send call.
  };

  /**
   * Bounded per-stream-generation ledger of frames at the sender's acquired destination.
   *
   * One writer (the broadcast thread) records each frame twice: begin() before its first send call
   * and complete() after its whole path ends. Readers join a frame range under the same mutex. The
   * work under the lock is constant per frame and the ledger never allocates after construction.
   */
  class sender_ledger_t {
  public:
    explicit sender_ledger_t(std::uint64_t stream_generation) noexcept:
        generation_ {stream_generation}, retired_ {stream_generation == 0} {}
    sender_ledger_t(const sender_ledger_t &) = delete;
    sender_ledger_t &operator=(const sender_ledger_t &) = delete;

    struct ticket_t {
      std::int64_t full_index = 0;
    };

    std::uint64_t stream_generation() const noexcept { return generation_; }

    /// Record a frame at its acquired destination, before any send call; @p begun_at is that entry
    /// boundary. The sender high-water moves first, even when no slot can be kept. An index the 32-bit
    /// wire field cannot carry, or one that does not increase, permanently makes this scope's joins
    /// unavailable: no unwrap is invented.
    std::optional<ticket_t> begin(std::int64_t full_index, const std::optional<encoder_setting_t> &setting,
                                  clock::time_point begun_at = clock::now()) noexcept {
      std::lock_guard lock {mutex_};
      const bool increasing = !high_water_ || full_index > *high_water_;
      if (increasing) high_water_ = full_index;
      if (retired_ || poison_) return std::nullopt;
      if (full_index < 0 || full_index > static_cast<std::int64_t>(std::numeric_limits<std::uint32_t>::max())) {
        poison_ = "frame_index_unrepresentable";
        return std::nullopt;
      }
      if (!increasing) {
        poison_ = "frame_index_not_increasing";
        return std::nullopt;
      }
      auto &slot = slots_[static_cast<std::size_t>(full_index) % k_ledger_capacity];
      slot = slot_t {};
      slot.used = true;
      slot.full_index = full_index;
      slot.wire_index = static_cast<std::uint32_t>(full_index);
      slot.setting_known = setting.has_value();
      if (setting) slot.setting = *setting;
      slot.disposition = send_disposition_e::pending;
      slot.begun_at = begun_at;
      return ticket_t {full_index};
    }

    /// Record how the frame's whole send path ended. A pending result is stored as incomplete.
    void complete(ticket_t ticket, send_disposition_e disposition, clock::time_point path_ended = clock::now()) noexcept {
      std::lock_guard lock {mutex_};
      if (retired_) return;
      auto &slot = slots_[static_cast<std::size_t>(ticket.full_index) % k_ledger_capacity];
      if (!slot.used || slot.full_index != ticket.full_index || slot.disposition != send_disposition_e::pending) return;
      slot.disposition = disposition == send_disposition_e::pending ? send_disposition_e::incomplete : disposition;
      slot.path_ended = path_ended;
    }

    /// Stream retirement is terminal: no later join is available.
    void retire() noexcept {
      std::lock_guard lock {mutex_};
      retired_ = true;
    }

    /// The highest full index the sender has begun recording in this scope, if any.
    std::optional<std::int64_t> high_water() const noexcept {
      std::lock_guard lock {mutex_};
      return high_water_;
    }

    struct join_t {
      bool available = false;
      const char *reason = "unavailable";
      std::optional<std::uint32_t> frame_index;  ///< The first frame that made the range unavailable.
      encoder_setting_t setting;
    };

    /// Inspect every frame in (from, to]. The range joins only when each of them is retained, was
    /// sent with a complete successful disposition, began (before its first send call) and ended its
    /// send path within @p bound of @p now on this host's clock, and carries the same exact encoder
    /// tuple of this stream generation.
    join_t join(std::uint32_t from_exclusive, std::uint32_t to_inclusive,
                clock::time_point now, clock::duration bound) const noexcept {
      std::lock_guard lock {mutex_};
      join_t out;
      const auto fail = [&out](const char *reason, std::optional<std::uint32_t> frame = std::nullopt) {
        out.reason = reason;
        out.frame_index = frame;
        return out;
      };
      if (retired_) return fail("scope_retired");
      if (poison_) return fail(poison_);
      if (to_inclusive <= from_exclusive) return fail("no_frames_in_interval");
      if (!high_water_ || static_cast<std::int64_t>(to_inclusive) > *high_water_) {
        return fail("cursor_beyond_sender_high_water");
      }
      if (static_cast<std::uint64_t>(to_inclusive) - from_exclusive > k_ledger_capacity) {
        return fail("range_exceeds_ledger", from_exclusive + 1);
      }
      for (std::uint64_t index = static_cast<std::uint64_t>(from_exclusive) + 1; index <= to_inclusive; ++index) {
        const auto frame = static_cast<std::uint32_t>(index);
        const auto &slot = slots_[index % k_ledger_capacity];
        if (!slot.used || slot.full_index < static_cast<std::int64_t>(index)) return fail("host_frame_missing", frame);
        if (slot.full_index > static_cast<std::int64_t>(index)) return fail("ledger_evicted", frame);
        if (!slot.setting_known) return fail("encoder_setting_missing", frame);
        if (slot.setting.stream_generation != generation_) return fail("encoder_setting_generation_mismatch", frame);
        switch (slot.disposition) {
          case send_disposition_e::sent: break;
          case send_disposition_e::pending: return fail("send_pending", frame);
          case send_disposition_e::send_failed: return fail("send_failed", frame);
          case send_disposition_e::batch_fallback: return fail("send_batch_fallback", frame);
          case send_disposition_e::cancelled: return fail("send_cancelled", frame);
          case send_disposition_e::exception: return fail("send_exception", frame);
          case send_disposition_e::incomplete: return fail("send_incomplete", frame);
          case send_disposition_e::not_sent: return fail("frame_not_sent", frame);
        }
        // Both boundaries: a fresh return alone cannot cover a send that entered long before it.
        if (now < slot.path_ended || slot.path_ended < slot.begun_at ||
            now - slot.begun_at > bound || now - slot.path_ended > bound) {
          return fail("host_send_stale", frame);
        }
        if (index == static_cast<std::uint64_t>(from_exclusive) + 1) {
          out.setting = slot.setting;
        } else if (!(slot.setting == out.setting)) {
          return fail("mixed_encoder_settings", frame);
        }
      }
      out.available = true;
      out.reason = "joined";
      return out;
    }

    /// Const inclusive access for native packet correlation, including frame zero. The historical
    /// exclusive join above and its callers keep their original behavior and locking.
    join_t join_inclusive(std::uint32_t first_inclusive, std::uint32_t to_inclusive,
                clock::time_point now, clock::duration bound, bool try_only = false) const noexcept {
      std::unique_lock lock {mutex_, std::defer_lock};
      if (try_only) {
        if (!lock.try_lock()) { join_t busy; busy.reason = "ledger_busy"; return busy; }
      } else lock.lock();
      return join_inclusive_locked(first_inclusive, to_inclusive, now, bound);
    }

    // Immediate downward publication only; retain sender retirement/setting exclusion through
    // the consumer. The consumer must not call this ledger or invoke an encoder/SDK operation.
    template<class Consume>
    bool with_join_inclusive(std::uint32_t first, std::uint32_t last, clock::time_point now,
                             clock::duration bound, Consume &&consume) const {
      std::unique_lock lock {mutex_, std::try_to_lock};
      if (!lock.owns_lock()) return false;
      const auto joined = join_inclusive_locked(first, last, now, bound);
      if (!joined.available) return false;
      consume(joined);
      return true;
    }

  private:
    join_t join_inclusive_locked(std::uint32_t first_inclusive, std::uint32_t to_inclusive,
                                clock::time_point now, clock::duration bound) const noexcept {
      join_t out;
      const auto fail = [&out](const char *reason, std::optional<std::uint32_t> frame = std::nullopt) {
        out.reason = reason;
        out.frame_index = frame;
        return out;
      };
      if (retired_) return fail("scope_retired");
      if (poison_) return fail(poison_);
      if (to_inclusive < first_inclusive) return fail("no_frames_in_interval");
      if (!high_water_ || static_cast<std::int64_t>(to_inclusive) > *high_water_) {
        return fail("cursor_beyond_sender_high_water");
      }
      if (static_cast<std::uint64_t>(to_inclusive) - first_inclusive + 1 > k_ledger_capacity) {
        return fail("range_exceeds_ledger", first_inclusive);
      }
      for (std::uint64_t index = static_cast<std::uint64_t>(first_inclusive); index <= to_inclusive; ++index) {
        const auto frame = static_cast<std::uint32_t>(index);
        const auto &slot = slots_[index % k_ledger_capacity];
        if (!slot.used || slot.full_index < static_cast<std::int64_t>(index)) return fail("host_frame_missing", frame);
        if (slot.full_index > static_cast<std::int64_t>(index)) return fail("ledger_evicted", frame);
        if (!slot.setting_known) return fail("encoder_setting_missing", frame);
        if (slot.setting.stream_generation != generation_) return fail("encoder_setting_generation_mismatch", frame);
        switch (slot.disposition) {
          case send_disposition_e::sent: break;
          case send_disposition_e::pending: return fail("send_pending", frame);
          case send_disposition_e::send_failed: return fail("send_failed", frame);
          case send_disposition_e::batch_fallback: return fail("send_batch_fallback", frame);
          case send_disposition_e::cancelled: return fail("send_cancelled", frame);
          case send_disposition_e::exception: return fail("send_exception", frame);
          case send_disposition_e::incomplete: return fail("send_incomplete", frame);
          case send_disposition_e::not_sent: return fail("frame_not_sent", frame);
        }
        // Both boundaries: a fresh return alone cannot cover a send that entered long before it.
        if (now < slot.path_ended || slot.path_ended < slot.begun_at ||
            now - slot.begun_at > bound || now - slot.path_ended > bound) {
          return fail("host_send_stale", frame);
        }
        if (index == static_cast<std::uint64_t>(first_inclusive)) {
          out.setting = slot.setting;
        } else if (!(slot.setting == out.setting)) {
          return fail("mixed_encoder_settings", frame);
        }
      }
      out.available = true;
      out.reason = "joined";
      return out;
    }

  private:
    struct slot_t {
      bool used = false;
      bool setting_known = false;
      send_disposition_e disposition = send_disposition_e::pending;
      std::uint32_t wire_index = 0;
      std::int64_t full_index = 0;
      encoder_setting_t setting;
      clock::time_point begun_at {};
      clock::time_point path_ended {};
    };

    const std::uint64_t generation_;
    mutable std::mutex mutex_;
    bool retired_;
    const char *poison_ = nullptr;
    std::optional<std::int64_t> high_water_;
    std::array<slot_t, k_ledger_capacity> slots_ {};
  };

  /**
   * One frame's record on the sender thread. Constructed at the acquired destination before any send
   * call; destroyed when that frame's iteration ends, however it ends. Reports only actual send API
   * return values; it reads no trace label and does nothing per datagram beyond a few flags.
   */
  class frame_send_record_t {
  public:
    frame_send_record_t(sender_ledger_t *ledger, std::int64_t full_index,
                        const std::optional<const video::bitrate_provenance::record_t> &setting) noexcept:
        ledger_ {ledger} {
      if (ledger_) ticket_ = ledger_->begin(full_index, encoder_setting_of(setting));
    }
    ~frame_send_record_t() {
      if (ledger_ && ticket_) ledger_->complete(*ticket_, disposition());
    }
    frame_send_record_t(const frame_send_record_t &) = delete;
    frame_send_record_t &operator=(const frame_send_record_t &) = delete;

    /// The return value of one send_batch() (@p batch) or one individual send() call.
    void send_returned(bool batch, bool succeeded) noexcept {
      ++send_calls_;
      if (!succeeded) {
        if (batch) batch_failed_ = true;
        else single_failed_ = true;
      }
    }
    /// The path ended deliberately before any send call.
    void not_sent() noexcept { not_sent_ = true; }
    void exception() noexcept { exception_ = true; }
    /// The whole frame path returned; @p cancelled is the destination's retirement state at that point.
    void path_finished(bool cancelled) noexcept {
      finished_ = true;
      cancelled_ = cancelled;
    }

    send_disposition_e disposition() const noexcept {
      if (exception_) return send_disposition_e::exception;
      if (cancelled_) return send_disposition_e::cancelled;
      if (!finished_) {
        return send_calls_ == 0 && not_sent_ ? send_disposition_e::not_sent : send_disposition_e::incomplete;
      }
      if (single_failed_) return send_disposition_e::send_failed;
      // A false batch is unknown even when every fallback send succeeded.
      if (batch_failed_) return send_disposition_e::batch_fallback;
      if (send_calls_ == 0) return send_disposition_e::not_sent;
      return send_disposition_e::sent;
    }

  private:
    sender_ledger_t *const ledger_;
    std::optional<sender_ledger_t::ticket_t> ticket_;
    std::uint64_t send_calls_ = 0;
    bool batch_failed_ = false;
    bool single_failed_ = false;
    bool not_sent_ = false;
    bool exception_ = false;
    bool cancelled_ = false;
    bool finished_ = false;
  };

  /// One validated native snapshot of a client's decoder-submission bridge (contract v1).
  struct snapshot_t {
    /// The client's successful launch/resume attempt nonce, separate from the native connection nonce.
    std::string launch_nonce;
    std::string connection_nonce;
    std::uint64_t native_epoch = 0;
    std::uint64_t snapshot_sequence = 0;
    std::uint32_t last_frame_index = 0;
    std::uint64_t decode_units_submitted_total = 0;
    std::uint64_t payload_bytes_total = 0;
    std::uint64_t monotonic_timestamp_ms = 0;
    std::uint64_t last_submission_timestamp_ms = 0;
  };

  /// The optional section of one telemetry request, as parsed.
  struct section_t {
    enum class state_e { absent, malformed, unsupported_version, valid };
    state_e state = state_e::absent;
    const char *reason = "absent";
    snapshot_t snapshot;
  };

  /// The host's answer for one section. Observational: never media loss, never controller evidence.
  struct receipt_t {
    bool available = false;
    const char *reason = "no_receipt";
    /// The snapshot was well formed; its validated identity is echoed.
    bool snapshot_echo = false;
    std::uint64_t native_epoch = 0;
    std::uint64_t snapshot_sequence = 0;
    std::optional<std::uint32_t> unavailable_frame_index;
    // Set only when available.
    std::uint32_t from_frame_exclusive = 0;
    std::uint32_t to_frame_inclusive = 0;
    std::uint64_t frames_emitted = 0;
    std::uint64_t decode_units_submitted = 0;
    std::uint64_t frames_absent_at_submission = 0;
    std::uint64_t payload_bytes = 0;
    std::uint64_t client_interval_ms = 0;
    encoder_setting_t encoder_setting;
    clock::time_point host_received_at {};
  };

  /**
   * Baseline, latest receipt and sender ledger of one exact stream scope (owner, app session and
   * stream generation, and the launch nonce that scope was admitted with, if any). The owner checks
   * that scope before every ingest and retires this object when the scope ends. Every valid,
   * coherent, source-bounded snapshot of that launch becomes the next baseline, joined or not; a
   * refused one leaves the newer baseline untouched.
   */
  class scope_feedback_t {
  public:
    scope_feedback_t(std::shared_ptr<sender_ledger_t> ledger, clock::duration coverage_bound,
                     std::string launch_nonce = {}) noexcept:
        ledger_ {std::move(ledger)}, bound_ {coverage_bound},
        launch_nonce_ {valid_nonce(launch_nonce) ? std::move(launch_nonce) : std::string {}} {}
    scope_feedback_t(const scope_feedback_t &) = delete;
    scope_feedback_t &operator=(const scope_feedback_t &) = delete;

    receipt_t ingest(const section_t &section, clock::time_point host_now) {
      std::lock_guard lock {mutex_};
      receipt_t receipt;
      receipt.host_received_at = host_now;
      const auto finish = [&](const char *reason) {
        receipt.reason = reason;
        latest_ = receipt;
        return receipt;
      };
      if (retired_) {
        receipt.reason = "scope_retired";
        return receipt;
      }
      if (section.state != section_t::state_e::valid) return finish(section.reason);
      const auto &next = section.snapshot;
      receipt.snapshot_echo = true;
      receipt.native_epoch = next.native_epoch;
      receipt.snapshot_sequence = next.snapshot_sequence;
      // Only a report from the exact launch this scope was admitted with can bind to it.
      if (launch_nonce_.empty()) return finish("launch_nonce_unavailable");
      if (next.launch_nonce != launch_nonce_) return finish("launch_nonce_mismatch");
      if (!ledger_) return finish("sender_ledger_unavailable");
      const auto high_water = ledger_->high_water();
      const bool source_bounded = high_water && static_cast<std::int64_t>(next.last_frame_index) <= *high_water;

      if (!baseline_ || next.native_epoch > baseline_->snapshot.native_epoch) {
        const char *reason = baseline_ ? "native_epoch_baseline" : "baseline";
        if (baseline_ && next.connection_nonce == baseline_->snapshot.connection_nonce) {
          return finish("connection_nonce_reused");
        }
        if (!source_bounded) return finish("cursor_beyond_sender_high_water");
        baseline_ = baseline_t {next, next.monotonic_timestamp_ms, host_now};
        return finish(reason);
      }
      // A copy: the consumed snapshot replaces the baseline below, before the join reads this one.
      const auto previous = baseline_->snapshot;
      if (next.native_epoch < previous.native_epoch) return finish("native_epoch_regressed");
      if (next.connection_nonce != previous.connection_nonce) return finish("connection_nonce_mismatch");
      if (next.snapshot_sequence <= previous.snapshot_sequence) return finish("snapshot_sequence_not_increasing");
      if (next.monotonic_timestamp_ms <= previous.monotonic_timestamp_ms) return finish("receiver_clock_not_increasing");
      if (next.last_frame_index < previous.last_frame_index ||
          next.decode_units_submitted_total < previous.decode_units_submitted_total ||
          next.payload_bytes_total < previous.payload_bytes_total ||
          next.last_submission_timestamp_ms < previous.last_submission_timestamp_ms) {
        return finish("counter_regressed");
      }
      const std::uint64_t emitted = next.last_frame_index - previous.last_frame_index;
      const std::uint64_t submitted = next.decode_units_submitted_total - previous.decode_units_submitted_total;
      const std::uint64_t bytes = next.payload_bytes_total - previous.payload_bytes_total;
      // Each counted unit has its own distinct frame number and a positive payload.
      const bool coherent = emitted == 0 ? submitted == 0 && bytes == 0 :
                                           submitted > 0 && submitted <= emitted && bytes >= submitted;
      if (!coherent) return finish("counters_incoherent");
      if (!source_bounded) return finish("cursor_beyond_sender_high_water");

      // Consumed: the next baseline whatever this interval's availability. An empty snapshot advances
      // the counters and sequence but never the evidence age the next interval is measured from.
      const auto anchor_client_ms = baseline_->anchor_client_ms;
      const auto anchor_arrival = baseline_->anchor_arrival;
      baseline_->snapshot = next;
      if (emitted == 0) return finish("no_frames_in_interval");
      baseline_->anchor_client_ms = next.monotonic_timestamp_ms;
      baseline_->anchor_arrival = host_now;

      // Freshness is gated from the anchor; the reported interval is the native snapshot delta.
      const auto bound_ms = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(bound_).count());
      const std::uint64_t client_interval_ms = next.monotonic_timestamp_ms - previous.monotonic_timestamp_ms;
      if (next.monotonic_timestamp_ms - anchor_client_ms > bound_ms) return finish("receiver_interval_exceeds_bound");
      if (host_now < anchor_arrival || host_now - anchor_arrival > bound_) return finish("host_arrival_interval_exceeds_bound");
      if (next.monotonic_timestamp_ms - next.last_submission_timestamp_ms > bound_ms) return finish("last_submission_stale");

      const auto join = ledger_->join(previous.last_frame_index, next.last_frame_index, host_now, bound_);
      if (!join.available) {
        receipt.unavailable_frame_index = join.frame_index;
        return finish(join.reason);
      }
      receipt.available = true;
      receipt.from_frame_exclusive = previous.last_frame_index;
      receipt.to_frame_inclusive = next.last_frame_index;
      receipt.frames_emitted = emitted;
      receipt.decode_units_submitted = submitted;
      receipt.frames_absent_at_submission = emitted - submitted;
      receipt.payload_bytes = bytes;
      receipt.client_interval_ms = client_interval_ms;
      receipt.encoder_setting = join.setting;
      return finish("joined");
    }

    std::optional<receipt_t> latest() const {
      std::lock_guard lock {mutex_};
      return latest_;
    }

    void retire() noexcept {
      std::lock_guard lock {mutex_};
      retired_ = true;
      if (ledger_) ledger_->retire();
    }

  private:
    struct baseline_t {
      snapshot_t snapshot;
      std::uint64_t anchor_client_ms = 0;
      clock::time_point anchor_arrival {};
    };

    mutable std::mutex mutex_;
    const std::shared_ptr<sender_ledger_t> ledger_;
    const clock::duration bound_;
    const std::string launch_nonce_;
    bool retired_ = false;
    std::optional<baseline_t> baseline_;
    std::optional<receipt_t> latest_;
  };
}  // namespace stream_frame_feedback
