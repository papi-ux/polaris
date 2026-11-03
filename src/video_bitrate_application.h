/** @file
 * Passive historical application bridge. Boundary calls are caller attestations,
 * not SDK execution or current broker permission. Serialize with backend work.
 */
#pragma once

#include "video_bitrate_provenance.h"

#include <atomic>
#include <utility>

namespace video::bitrate_application {
  struct origin_t {
    std::uint64_t revision = 0;
    int target_kbps = 0;
  };

  // Independent metadata identities, never trace IDs, addresses or broker IDs.
  // A seeded producer also permits finite public exhaustion guards; production
  // always uses the single default producer below, without reset or reuse.
  class identity_source_t {
  public:
    explicit identity_source_t(std::uint64_t last_issued = 0) noexcept: last_ {last_issued} {}
    std::uint64_t issue() noexcept {
      auto last = last_.load(std::memory_order_relaxed);
      while (last != std::numeric_limits<std::uint64_t>::max()) {
        if (last_.compare_exchange_weak(last, last + 1, std::memory_order_relaxed)) return last + 1;
      }
      return 0;
    }
  private:
    std::atomic<std::uint64_t> last_;
  };

  inline identity_source_t &process_identities() noexcept {
    static identity_source_t source;
    return source;
  }

  enum class backend_e { avcodec, native_nvenc };
  enum class submit_stage_e { avcodec_send_frame_success, nvenc_encode_picture_success };
  enum class output_stage_e { avcodec_packet_pts, nvenc_output_timestamp };

  struct avcodec_rate_t {
    std::int64_t bit_rate = 0;
    std::int64_t min_rate = 0;
    std::int64_t max_rate = 0;
    bool cbr_with_vbr = false;
  };

  inline std::optional<std::int64_t> checked_timestamp(std::uint64_t pts) noexcept {
    if (pts > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) return std::nullopt;
    return static_cast<std::int64_t>(pts);
  }

  class adapter_t {
    struct open_t {
      std::optional<origin_t> origin;
      std::uint64_t sequence;
      avcodec_rate_t before;
      avcodec_rate_t after;
    };
    struct native_open_t {
      std::optional<origin_t> origin;
      std::uint64_t sequence;
      std::uint64_t average_bps;
      bool cbr;
    };
    struct state_t {
      state_t(bitrate_provenance::scope_t scope, backend_e backend, std::uint64_t limit) noexcept:
          scope {scope}, ledger {scope}, backend {backend}, operation_limit {limit} {}
      const bitrate_provenance::scope_t scope;
      bitrate_provenance::ledger_t ledger;
      const backend_e backend;
      const std::uint64_t operation_limit;
      std::uint64_t operation_sequence = 0;
      bool retired = false;
      bool open_seen = false;
      std::optional<open_t> opening;
      std::optional<native_open_t> native_opening;

      std::uint64_t next_operation() noexcept {
        opening.reset();
        native_opening.reset();
        ledger.setting_unavailable();
        if (retired) return 0;
        if (operation_sequence == operation_limit ||
            operation_sequence == std::numeric_limits<std::uint64_t>::max()) {
          retired = true;
          ledger.retire();
          return 0;
        }
        return ++operation_sequence;
      }
      bool bind(std::optional<origin_t> origin, std::uint64_t sequence,
                std::uint64_t bps, bitrate_provenance::receipt_origin_e kind) noexcept {
        if (retired || !origin || origin->revision == 0 || origin->target_kbps <= 0 ||
            sequence == 0 || bps != static_cast<std::uint64_t>(origin->target_kbps) * 1000) {
          ledger.setting_unavailable();
          return false;
        }
        return ledger.observe_receipt({scope, sequence, origin->revision, origin->target_kbps, kind});
      }
    };

  public:
    // One-use observation of this synchronous native update invocation. Leaving
    // it unfinished (including every SDK/error early return) clears only future
    // setting evidence. No-op explicitly preserves an already proven setting.
    class update_observation_t {
    public:
      update_observation_t() noexcept = default;
      update_observation_t(const update_observation_t &) = delete;
      update_observation_t &operator=(const update_observation_t &) = delete;
      update_observation_t(update_observation_t &&) = delete;
      update_observation_t &operator=(update_observation_t &&) = delete;
      ~update_observation_t() noexcept {
        if (!finished_) if (auto state = state_.lock()) state->ledger.setting_unavailable();
      }
      void unchanged() noexcept { finished_ = true; }
      bool nvenc_reconfigure_success(std::uint64_t accepted_average_bps) noexcept {
        if (std::exchange(finished_, true)) return false;
        auto state = state_.lock();
        if (!state || state->backend != backend_e::native_nvenc) return false;
        const auto sequence = state->next_operation();
        return state->bind(origin_, sequence, accepted_average_bps,
                           bitrate_provenance::receipt_origin_e::backend_reconfiguration);
      }
    private:
      friend class adapter_t;
      update_observation_t(const std::shared_ptr<state_t> &state, std::optional<origin_t> origin) noexcept:
          state_ {state}, origin_ {origin} {}
      std::weak_ptr<state_t> state_;
      const std::optional<origin_t> origin_ {};
      bool finished_ = false;
    };

    // Local, nonthrowing submission sink. Only weak custody escapes the adapter;
    // destruction/recreation cannot leave a pointer into a replacement ledger.
    // Construction/copying weak handles and record attachment allocate nothing.
    class submission_sink_t {
    public:
      submission_sink_t() noexcept = default;
      submission_sink_t(const submission_sink_t &) = delete;
      submission_sink_t &operator=(const submission_sink_t &) = delete;
      submission_sink_t(submission_sink_t &&) = delete;
      submission_sink_t &operator=(submission_sink_t &&) = delete;
      ~submission_sink_t() noexcept {
        if (!finished_) if (auto state = state_.lock(); state && ticket_) {
          state->ledger.complete(*ticket_, bitrate_provenance::submission_result_e::failed_or_unproven);
        }
      }
      bool accepted(submit_stage_e stage) noexcept {
        if (std::exchange(finished_, true)) return false;
        auto state = state_.lock();
        if (!state || !ticket_) return false;
        const bool matching = (state->backend == backend_e::avcodec && stage == submit_stage_e::avcodec_send_frame_success) ||
                              (state->backend == backend_e::native_nvenc && stage == submit_stage_e::nvenc_encode_picture_success);
        return state->ledger.complete(*ticket_, matching ?
          bitrate_provenance::submission_result_e::submitted_under_captured_setting :
          bitrate_provenance::submission_result_e::failed_or_unproven);
      }
    private:
      friend class adapter_t;
      submission_sink_t(const std::shared_ptr<state_t> &state, std::optional<std::int64_t> pts) noexcept:
          state_ {state}, ticket_ {state ? state->ledger.capture(pts) : std::nullopt} {}
      std::weak_ptr<state_t> state_;
      std::optional<bitrate_provenance::submission_t> ticket_;
      bool finished_ = false;
    };

    // The adapter is the sole persistent strong owner of its state/ledger.
    // Sinks borrow weak handles; moving a wrapper transfers its unique_ptr to
    // this nonmovable adapter. Limits only contract metadata availability.
    explicit adapter_t(std::uint64_t generation, backend_e backend,
                       identity_source_t &identities = process_identities(),
                       std::uint64_t operation_limit = std::numeric_limits<std::uint64_t>::max()) noexcept {
      const auto lifetime = generation ? identities.issue() : 0;
      if (!lifetime) return;
      try {
        state_ = std::make_shared<state_t>(bitrate_provenance::scope_t {lifetime, generation}, backend, operation_limit);
      } catch (const std::bad_alloc &) {
        // Metadata allocation failure never escapes into codec admission.
      }
    }
    adapter_t(const adapter_t &) = delete;
    adapter_t &operator=(const adapter_t &) = delete;
    adapter_t(adapter_t &&) = delete;
    adapter_t &operator=(adapter_t &&) = delete;
    ~adapter_t() noexcept { retire(); }

    update_observation_t begin_native_update(std::optional<origin_t> origin) noexcept {
      return update_observation_t {state_, origin};
    }

    // Unattributed native opens stay unknown. Never borrow a later poll.
    void nvenc_initialize_success() noexcept {
      if (!state_ || state_->backend != backend_e::native_nvenc || state_->open_seen) return;
      state_->open_seen = true;
      state_->next_operation();
    }

    // Explicit supported path: the caller captured this origin BEFORE the
    // actual successful SDK initialization at the literal CBR average rate.
    // SDK success alone stages a candidate, not a usable frame interval.
    void nvenc_initialize_success(std::optional<origin_t> origin,
                                  std::uint64_t accepted_average_bps, bool cbr) noexcept {
      if (!state_ || state_->backend != backend_e::native_nvenc || state_->open_seen) return;
      state_->open_seen = true;
      const auto sequence = state_->next_operation();
      if (sequence) state_->native_opening = native_open_t {origin, sequence, accepted_average_bps, cbr};
    }

    bool publish_usable_nvenc_open() noexcept {
      if (!state_ || state_->backend != backend_e::native_nvenc) return false;
      const auto candidate = std::exchange(state_->native_opening, std::nullopt);
      if (!candidate) return false;
      if (!candidate->cbr) {
        state_->ledger.setting_unavailable();
        return false;
      }
      return state_->bind(candidate->origin, candidate->sequence, candidate->average_bps,
                          bitrate_provenance::receipt_origin_e::successful_open);
    }

    // Call once, immediately after this actual avcodec_open2 succeeds. Failed
    // retries produce no candidate. Keep literal pre/post rate representations.
    void avcodec_open_success(std::optional<origin_t> origin, avcodec_rate_t before,
                              avcodec_rate_t after) noexcept {
      if (!state_ || state_->backend != backend_e::avcodec || state_->open_seen) return;
      state_->open_seen = true;
      const auto sequence = state_->next_operation();
      if (sequence) state_->opening = open_t {origin, sequence, before, after};
    }

    // Second boundary: only the successfully constructed usable outer session
    // publishes. A staged acceptance cannot itself authorize frame metadata.
    bool publish_usable_avcodec_open() noexcept {
      if (!state_) return false;
      auto candidate = std::exchange(state_->opening, std::nullopt);
      if (!candidate) return false;
      const auto &a = candidate->before;
      const auto &b = candidate->after;
      if (a.cbr_with_vbr || b.cbr_with_vbr || a.bit_rate <= 0 ||
          a.bit_rate != a.min_rate || a.bit_rate != a.max_rate ||
          a.bit_rate != b.bit_rate || a.min_rate != b.min_rate || a.max_rate != b.max_rate) {
        state_->ledger.setting_unavailable();
        return false;
      }
      return state_->bind(candidate->origin, candidate->sequence, static_cast<std::uint64_t>(b.bit_rate),
                          bitrate_provenance::receipt_origin_e::successful_open);
    }

    submission_sink_t capture_before_submit(std::optional<std::int64_t> pts) noexcept {
      return submission_sink_t {state_, pts};
    }
    std::optional<bitrate_provenance::record_t> take_output(output_stage_e stage,
                                                         std::optional<std::int64_t> pts) noexcept {
      if (!state_) return std::nullopt;
      const bool matching = (state_->backend == backend_e::avcodec && stage == output_stage_e::avcodec_packet_pts) ||
                            (state_->backend == backend_e::native_nvenc && stage == output_stage_e::nvenc_output_timestamp);
      return matching ? state_->ledger.take(state_->scope, pts) : std::nullopt;
    }
    void setting_unavailable() noexcept {
      if (state_) {
        state_->opening.reset();
        state_->native_opening.reset();
        state_->ledger.setting_unavailable();
      }
    }
    void retire() noexcept {
      if (state_) {
        state_->retired = true;
        state_->opening.reset();
        state_->native_opening.reset();
        state_->ledger.retire();
      }
    }

  private:
    std::shared_ptr<state_t> state_;
  };
}  // namespace video::bitrate_application
