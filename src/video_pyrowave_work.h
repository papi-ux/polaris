/**
 * @file src/video_pyrowave_work.h
 * @brief Optional local PyroWave work/buffer observations; no control authority.
 *
 * One adapter belongs to one real wrapper incarnation and its owning thread.
 * The trusted wrapper must discard on convert entry and begin before EVERY
 * buffer-mutating backend call, including a retained-picture re-encode.
 * Completion means that exact call returned success with nonempty output.
 * Conversion has no PTS: the frame number is bound only at packet handoff.
 * Neither a caller-supplied success bool nor these metadata tests prove codec
 * or GPU completion; the production callsites are part of this contract.
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <utility>
#include <vector>

namespace video::pyrowave_work {
  enum class kind_e { imported, blank_primer, blank_hdr, packed, retained };
  enum class authority_e { unknown };
  enum class provenance_e { wrapper_call_success_nonempty };

  namespace detail {
    // Strongly owned structural identity. Its address is never exported as an
    // integer or borrowed lifetime ID, and no registry or trace source is used.
    struct incarnation_t final {};
    using incarnation_ptr = std::shared_ptr<const incarnation_t>;
  }

  class adapter_t;

  class work_ticket_t final {
  public:
    work_ticket_t() noexcept = default;
    work_ticket_t(const work_ticket_t &) = delete;
    work_ticket_t &operator=(const work_ticket_t &) = delete;
    work_ticket_t &operator=(work_ticket_t &&) = delete;
    work_ticket_t(work_ticket_t &&other) noexcept:
        incarnation_ {std::move(other.incarnation_)},
        sequence_ {std::exchange(other.sequence_, 0)},
        kind_ {other.kind_}, budget_ {other.budget_} {}

    bool available() const noexcept { return incarnation_ && sequence_ != 0; }

  private:
    friend class adapter_t;
    work_ticket_t(detail::incarnation_ptr incarnation, std::uint64_t sequence,
                  kind_e kind, std::size_t budget) noexcept:
        incarnation_ {std::move(incarnation)}, sequence_ {sequence},
        kind_ {kind}, budget_ {budget} {}

    detail::incarnation_ptr incarnation_;
    std::uint64_t sequence_ = 0;
    kind_e kind_ = kind_e::packed;
    std::size_t budget_ = 0;
  };

  class handoff_receipt_t final {
  public:
    // These accessors expose historical facts, never live permission.
    authority_e authority() const noexcept { return authority_e::unknown; }
    provenance_e provenance() const noexcept { return provenance_e::wrapper_call_success_nonempty; }
    kind_e work_kind() const noexcept { return kind_; }
    std::uint64_t local_work_sequence() const noexcept { return sequence_; }
    std::int64_t handoff_frame_index() const noexcept { return frame_; }
    std::size_t effective_budget_argument_bytes() const noexcept { return budget_; }
    std::size_t completed_bitstream_bytes() const noexcept { return completed_bytes_; }
    std::size_t generic_packet_copy_bytes() const noexcept { return packet_bytes_; }
    bool same_local_wrapper(const handoff_receipt_t &other) const noexcept {
      return incarnation_ == other.incarnation_;
    }

  private:
    friend class adapter_t;
    handoff_receipt_t(detail::incarnation_ptr incarnation, std::uint64_t sequence,
                      kind_e kind, std::size_t budget, std::size_t completed_bytes,
                      std::int64_t frame, std::size_t packet_bytes) noexcept:
        incarnation_ {std::move(incarnation)}, sequence_ {sequence}, kind_ {kind},
        budget_ {budget}, completed_bytes_ {completed_bytes}, frame_ {frame},
        packet_bytes_ {packet_bytes} {}

    // Keeping the cookie alive also prevents address reuse while historical
    // queued records or detached tickets still name that incarnation.
    const detail::incarnation_ptr incarnation_;
    const std::uint64_t sequence_;
    const kind_e kind_;
    const std::size_t budget_;
    const std::size_t completed_bytes_;
    const std::int64_t frame_;
    const std::size_t packet_bytes_;
  };

  // Inline immutable historical value: no per-handoff allocation. Its cookie
  // is the one shared allocation made for the actual wrapper incarnation.
  using receipt_value = std::optional<const handoff_receipt_t>;

  class adapter_t final {
  public:
    // A finite metadata identity allowance, independent of codec settings.
    // The default reaches UINT64_MAX exactly once, then stays unavailable.
    // Zero deliberately disables observations. This is not a policy knob.
    explicit adapter_t(std::uint64_t work_identity_limit = std::numeric_limits<std::uint64_t>::max()) noexcept:
        work_identity_limit_ {work_identity_limit} {
      if (work_identity_limit_ == 0) return;
      try {
        incarnation_ = std::make_shared<const detail::incarnation_t>();
      }
      catch (const std::bad_alloc &) {
        // Optional metadata allocation cannot fail construction/encoding.
      }
    }

    adapter_t(const adapter_t &) = delete;
    adapter_t(adapter_t &&) = delete;
    adapter_t &operator=(const adapter_t &) = delete;
    adapter_t &operator=(adapter_t &&) = delete;
    ~adapter_t() { retire(); }

    bool available() const noexcept { return static_cast<bool>(incarnation_); }

    // Clear ONLY observation state. In particular, this does not clear the
    // wrapper's converted flag, bitstream, retained picture, or backend state.
    void discard_buffer() noexcept {
      active_sequence_ = 0;
      completed_.reset();
    }

    void retire() noexcept {
      discard_buffer();
      incarnation_.reset();
    }

    work_ticket_t begin_work(kind_e kind, std::size_t effective_budget_argument_bytes) noexcept {
      discard_buffer();
      if (!incarnation_ || !valid_kind(kind) || effective_budget_argument_bytes == 0 ||
          last_work_sequence_ == work_identity_limit_) return {};
      // Strictly below the finite limit before increment: never wraps.
      active_sequence_ = ++last_work_sequence_;
      return {incarnation_, active_sequence_, kind, effective_budget_argument_bytes};
    }

    // Consumes even an invalid/foreign ticket. A failed, empty, overwritten,
    // moved-from or retired ticket never leaves an older observation usable.
    void complete_work(work_ticket_t ticket, bool backend_succeeded,
                       const std::vector<std::uint8_t> &buffer) noexcept {
      const bool exact = incarnation_ && ticket.incarnation_ == incarnation_ &&
                         ticket.sequence_ != 0 && ticket.sequence_ == active_sequence_;
      discard_buffer();
      if (!exact || !backend_succeeded || buffer.empty()) return;
      completed_.emplace(completed_t {ticket.sequence_, ticket.kind_, ticket.budget_,
                                     &buffer, buffer.data(), buffer.size(),
                                     ticket.kind_ == kind_e::retained});
    }

    // The cached prepare branch observes NO new work or setting. It promotes
    // only its own unconsumed conversion buffer, retaining that work's budget.
    void prepare_cached_buffer(const std::vector<std::uint8_t> &buffer) noexcept {
      if (!completed_ || completed_->ready || completed_->kind == kind_e::retained ||
          !matches(*completed_, buffer)) {
        discard_buffer();
        return;
      }
      completed_->ready = true;
    }

    // The wrapper computes the actual retained result in a separate statement
    // before obtaining the buffer reference, avoiding argument-order reliance.
    // Return that original bool unchanged, even for unavailable observations.
    bool complete_retained(work_ticket_t ticket, bool backend_succeeded,
                           const std::vector<std::uint8_t> &buffer) noexcept {
      complete_work(std::move(ticket), backend_succeeded, buffer);
      return backend_succeeded;
    }

    receipt_value take_for_packet(std::int64_t frame_index,
                                const std::vector<std::uint8_t> &encoded_buffer,
                                const std::vector<std::uint8_t> &packet_copy) noexcept {
      auto completed = std::exchange(completed_, std::nullopt);
      active_sequence_ = 0;
      // Reserve every representable handoff, even one with unavailable
      // metadata, so a duplicate/regressed frame cannot acquire fresh proof.
      if (!incarnation_ || frame_index < 0 ||
          (last_handoff_frame_ && frame_index <= *last_handoff_frame_)) return {};
      last_handoff_frame_ = frame_index;
      if (!completed || !completed->ready || !matches(*completed, encoded_buffer) ||
          packet_copy.size() != completed->bytes) return {};
      // Private constructor; no allocation or throwing operation at handoff.
      return receipt_value {handoff_receipt_t(incarnation_, completed->sequence,
        completed->kind, completed->budget, completed->bytes, frame_index, packet_copy.size())};
    }

  private:
    struct completed_t {
      std::uint64_t sequence;
      kind_e kind;
      std::size_t budget;
      // Equality guards inside the owning wrapper's work epoch only. These
      // pointers are never dereferenced or exported and are NOT lifetime IDs.
      const std::vector<std::uint8_t> *object;
      const std::uint8_t *data;
      std::size_t bytes;
      bool ready;
    };

    static bool valid_kind(kind_e kind) noexcept {
      switch (kind) {
        case kind_e::imported:
        case kind_e::blank_primer:
        case kind_e::blank_hdr:
        case kind_e::packed:
        case kind_e::retained: return true;
      }
      return false;
    }

    static bool matches(const completed_t &work, const std::vector<std::uint8_t> &buffer) noexcept {
      return work.object == &buffer && work.data == buffer.data() &&
             work.bytes != 0 && work.bytes == buffer.size();
    }

    detail::incarnation_ptr incarnation_;
    const std::uint64_t work_identity_limit_;
    std::uint64_t last_work_sequence_ = 0;
    std::uint64_t active_sequence_ = 0;
    std::optional<std::int64_t> last_handoff_frame_;
    std::optional<completed_t> completed_;
  };
}  // namespace video::pyrowave_work
