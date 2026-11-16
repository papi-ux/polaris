/**
 * @file src/live_tuning_fec.h
 * @brief The encoder/parity transaction around Live Tuning's F0-unit policy.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

#include "stream_bitrate.h"

namespace live_tuning_fec {

  enum class selection_basis_e { unavailable, configured_baseline, encoder_confirmed_trial };

  inline const char *selection_basis_name(selection_basis_e basis) noexcept {
    switch (basis) {
      case selection_basis_e::configured_baseline: return "configured_baseline";
      case selection_basis_e::encoder_confirmed_trial: return "encoder_confirmed_trial";
      default: return "unavailable";
    }
  }

  /** Nominal allocation and passive identity from the same publication read.
   * Baseline has configured scope only; fallback/invalid frames have no identity.
   * Neither selection nor confirmation proves constructed or sent packet bytes. */
  struct selection_t {
    std::pair<int, int> allocation;
    selection_basis_e basis = selection_basis_e::unavailable;
    std::optional<std::uint64_t> generation;
    std::optional<std::uint64_t> authority_revision;
    std::optional<std::int64_t> from_frame;
  };

  // libstdc++ 15 deprecates the shared_ptr overloads of std::atomic_load and
  // std::atomic_store from C++20 on, in favour of std::atomic<std::shared_ptr<T>>, and
  // the Arch and SteamOS packages build with -Werror. The overloads are still correct
  // for published_, so the warning is silenced for this class only.
  #pragma GCC diagnostic push
  #pragma GCC diagnostic ignored "-Wdeprecated-declarations"

  /**
   * One controller-owned transaction. Writers serialize with the controller's
   * mutex. The packetizer only calls selection_for_frame(), which acquires
   * one immutable publication per frame, without taking that mutex. Atomic
   * shared_ptr publication is coherent; it is not claimed to be lock-free.
   */
  class actuator_t {
  public:
    actuator_t() = default;
    actuator_t(const actuator_t &) = delete;
    actuator_t &operator=(const actuator_t &) = delete;

    enum class phase_e { fixed, lowering_rate, raising_rate };
    enum class confirmation_e { none, published, restored, refused, stale };

    struct publication_t {
      std::uint64_t generation;
      int configured;
      int percentage;
      std::int64_t from_frame;
      std::uint64_t authority_revision;
    };

    struct pending_t {
      std::uint64_t generation;
      std::uint64_t authority_revision;
      int encoder_kbps;
      int percentage;
      // Allocated before asking the encoder to change. It is filled once at
      // the encoder's frame boundary and never mutated after publication.
      std::shared_ptr<publication_t> publication;
    };

    bool configure(std::uint64_t generation, const stream_bitrate::request_t &request) {
      reset();
      if (!generation || request.split_kbps <= 0 || request.audio_kbps < 0 ||
          request.fec_percentage < 0 || request.fec_percentage >= 50 ||
          request.encoder_kbps <= 0 ||
          stream_bitrate::encoder_kbps_for_wire(request.split_kbps,
            request.fec_percentage, request.audio_kbps) != request.encoder_kbps) {
        return false;
      }
      auto baseline = std::make_shared<const publication_t>(publication_t {
        generation, request.fec_percentage, request.fec_percentage, 0, 0
      });
      generation_ = generation;
      request_kbps_ = request.split_kbps;
      audio_kbps_ = request.audio_kbps;
      configured_ = percentage_ = request.fec_percentage;
      baseline_ = std::move(baseline);
      std::atomic_store_explicit(&published_, baseline_, std::memory_order_release);
      return true;
    }

    void reset() {
      std::atomic_store_explicit(&published_, std::shared_ptr<const publication_t> {}, std::memory_order_release);
      baseline_.reset();
      pending_.reset();
      generation_ = 0;
      request_kbps_ = 0;
      audio_kbps_ = configured_ = percentage_ = 0;
      phase_ = phase_e::fixed;
    }

    bool configured() const { return generation_ != 0; }
    std::uint64_t generation() const { return generation_; }
    int configured_percentage() const { return configured_; }
    int percentage() const { return percentage_; }
    phase_e phase() const { return phase_; }
    bool pending() const { return pending_.has_value(); }
    std::optional<int> pending_percentage() const {
      return pending_ ? std::optional<int> {pending_->percentage} : std::nullopt;
    }

    /** Map a policy target once. At F0 the map is exactly the identity. This
     * is the handshake's nominal ledger, not a measurement of packet bytes.
     * Refuse a target outside the immutable request, even after rounding. */
    std::optional<int> encoder_kbps(int policy_kbps, int percentage) const {
      if (!configured() || policy_kbps <= 0 || percentage < configured_ || percentage > 50) {
        return std::nullopt;
      }
      const auto wire = stream_bitrate::wire_kbps_for_encoder(policy_kbps, configured_, audio_kbps_);
      if (wire > request_kbps_) {
        return std::nullopt;
      }
      if (percentage == configured_) {
        return policy_kbps;
      }
      const auto encoder = stream_bitrate::encoder_kbps_for_wire(wire, percentage, audio_kbps_);
      if (encoder <= 0 || encoder > std::numeric_limits<int>::max()) {
        return std::nullopt;
      }
      return static_cast<int>(encoder);
    }

    /** Prepare the exact authority revision the caller will publish for this
     * request. Raising parity leaves the packetizer unchanged until confirm.
     * Returning to F0 publishes the lower parity BEFORE the caller may ask
     * the encoder for a higher bitrate. Only F0 -> trial -> F0 is admitted. */
    std::optional<int> request(int policy_kbps, int wanted_percentage,
                              int floor_kbps, bool eligible,
                              std::uint64_t next_authority_revision) {
      if (!configured() || pending_ || !next_authority_revision ||
          wanted_percentage == percentage_) {
        return std::nullopt;
      }
      const auto encoder = encoder_kbps(policy_kbps, wanted_percentage);
      if (!encoder || (wanted_percentage != configured_ && *encoder < floor_kbps)) {
        return std::nullopt;
      }
      if (wanted_percentage == configured_) {
        restore();
        pending_ = pending_t {generation_, next_authority_revision, *encoder, configured_, {}};
        phase_ = phase_e::raising_rate;
      } else {
        if (!eligible || percentage_ != configured_) {
          return std::nullopt;
        }
        auto publication = std::make_shared<publication_t>(publication_t {
          generation_, configured_, wanted_percentage, 0, 0
        });
        pending_ = pending_t {generation_, next_authority_revision, *encoder,
          wanted_percentage, std::move(publication)};
        phase_ = phase_e::lowering_rate;
      }
      return encoder;
    }

    /** Caller has already checked its exact encoder request/revision and the
     * backend's actual result. An old authority/ABA never confirms a new trial.
     * A positive frame boundary names the NEXT frame using this encoder rate.
     * Refusal cancels only an unpublished raise; confirmed parity stays put. */
    confirmation_e confirm(std::uint64_t generation, std::uint64_t authority_revision,
                           int actual_encoder_kbps, std::int64_t next_frame,
                           bool in_place, bool fresh) {
      if (!pending_) {
        return confirmation_e::none;
      }
      if (pending_->generation != generation ||
          pending_->authority_revision != authority_revision) {
        pending_.reset();
        phase_ = phase_e::fixed;
        return confirmation_e::stale;
      }
      if (pending_->encoder_kbps != actual_encoder_kbps) {
        return confirmation_e::none;
      }
      if (phase_ == phase_e::raising_rate) {
        pending_.reset();
        phase_ = phase_e::fixed;
        return confirmation_e::restored;
      }
      if (!in_place || !fresh || next_frame <= 0) {
        pending_.reset();
        phase_ = phase_e::fixed;
        return confirmation_e::refused;
      }
      auto publication = std::move(pending_->publication);
      publication->from_frame = next_frame;
      publication->authority_revision = pending_->authority_revision;
      percentage_ = pending_->percentage;
      pending_.reset();
      phase_ = phase_e::fixed;
      std::atomic_store_explicit(&published_,
        std::shared_ptr<const publication_t> {std::move(publication)}, std::memory_order_release);
      return confirmation_e::published;
    }

    /** Publish the preallocated baseline before an outside writer changes the
     * bitrate. There is no allocation, SDK call, wait or speculative ACK here. */
    void restore() {
      std::atomic_store_explicit(&published_, baseline_, std::memory_order_release);
      pending_.reset();
      percentage_ = configured_;
      phase_ = phase_e::fixed;
    }

    selection_t selection_for_frame(std::uint64_t generation, std::int64_t frame,
                                    int fallback) const {
      const auto publication = std::atomic_load_explicit(&published_, std::memory_order_acquire);
      selection_t selection {};
      selection.allocation = {fallback, fallback};
      if (!publication || publication->generation != generation) {
        return selection;
      }
      selection.allocation = {publication->configured,
        frame >= publication->from_frame ? publication->percentage : publication->configured};
      // Preserve legacy allocation for every frame argument, including zero
      // and invalid PTS. Such frames cannot acquire a publication identity.
      if (frame <= 0) {
        return selection;
      }
      selection.generation = publication->generation;
      selection.basis = selection_basis_e::configured_baseline;
      if (publication->from_frame > 0 && frame >= publication->from_frame) {
        selection.basis = selection_basis_e::encoder_confirmed_trial;
        selection.authority_revision = publication->authority_revision;
        selection.from_frame = publication->from_frame;
      }
      return selection;
    }

    std::pair<int, int> allocation_for_frame(std::uint64_t generation, std::int64_t frame,
                                            int fallback) const {
      return selection_for_frame(generation, frame, fallback).allocation;
    }

    int percentage_for_frame(std::uint64_t generation, std::int64_t frame, int fallback) const {
      return allocation_for_frame(generation, frame, fallback).second;
    }

  private:
    std::uint64_t generation_ = 0;
    std::int64_t request_kbps_ = 0;
    int audio_kbps_ = 0;
    int configured_ = 0;
    int percentage_ = 0;
    phase_e phase_ = phase_e::fixed;
    std::optional<pending_t> pending_;
    std::shared_ptr<const publication_t> baseline_;
    std::shared_ptr<const publication_t> published_;
  };

  #pragma GCC diagnostic pop

}  // namespace live_tuning_fec
