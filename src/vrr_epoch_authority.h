/**
 * @file src/vrr_epoch_authority.h
 * @brief Out-of-band live authority of one capture epoch, checked by encode threads at use.
 *
 * A capture epoch's proof is historical evidence and its frames carry only value facts. Whether a
 * frame may still be emitted at its mapped source time depends on the epoch being alive NOW: still
 * open, of the consumer's own generation, with the very proof the frame carries, under the very
 * lease that proof was collected against, and with that lease still current.
 *
 * The epoch owns its authority and its single lease snapshot. The registry holds only weak
 * authority references, and the authority refers weakly to that original lease. Nothing else
 * (images, receipts, proofs, statistics) ever retains the lease. A check never
 * asks a display or runtime for its latest lease: a reopened display or relaunched compositor
 * cannot stand in for the generation a frame came from.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <iterator>
#include <memory>
#include <mutex>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "vrr_clock_proof.h"
#include "vrr_compositor_identity.h"

namespace vrr_clock {

  /// One capture epoch's live authority. Constructed and owned only by that epoch.
  class epoch_authority_t {
  public:
    epoch_authority_t(std::uint64_t epoch_id,
                      std::uint64_t capture_generation_id,
                      std::shared_ptr<const proof_t> proof,
                      std::weak_ptr<const compositor_lease_t> lease):
        epoch_id {epoch_id},
        capture_generation_id {capture_generation_id},
        proof {std::move(proof)},
        lease {std::move(lease)} {
    }

    const std::uint64_t epoch_id;
    const std::uint64_t capture_generation_id;
    /// Held so the proof's address cannot be reused while this authority exists.
    const std::shared_ptr<const proof_t> proof;
    /// The exact snapshot the proof was collected against, owned by the capture epoch alone.
    const std::weak_ptr<const compositor_lease_t> lease;
    /// Cleared before the owning epoch is replaced or destroyed.
    std::atomic<bool> open {true};
  };

  enum class authority_e {
    current,
    no_epoch,
    closed,
    generation_mismatch,
    proof_mismatch,
    lease_not_current,
  };

  inline std::string_view name(authority_e value) {
    switch (value) {
      case authority_e::current:
        return "current";
      case authority_e::no_epoch:
        return "no_epoch";
      case authority_e::closed:
        return "closed";
      case authority_e::generation_mismatch:
        return "generation_mismatch";
      case authority_e::proof_mismatch:
        return "proof_mismatch";
      case authority_e::lease_not_current:
        return "lease_not_current";
    }
    return "invalid_authority";
  }

  namespace detail {
    struct authority_registry_t {
      std::mutex mutex;
      std::unordered_map<std::uint64_t, std::weak_ptr<epoch_authority_t>> entries;
    };

    /// The process's one registry (an inline function's static is a single object across units).
    inline authority_registry_t &authority_registry() {
      static authority_registry_t registry;
      return registry;
    }
  }  // namespace detail

  /**
   * @brief The capture epoch's ownership of its published authority. Move-only.
   *
   * Destruction, move-assignment over it and reset() all close and withdraw the authority first, so a
   * replaced or destroyed epoch can never answer current again, even to a check already holding it.
   */
  class authority_publication_t {
  public:
    authority_publication_t() = default;

    explicit authority_publication_t(std::shared_ptr<epoch_authority_t> authority):
        authority_ {std::move(authority)} {
      if (!authority_) return;
      auto &registry = detail::authority_registry();
      std::lock_guard lock {registry.mutex};
      for (auto entry = registry.entries.begin(); entry != registry.entries.end();) {
        entry = entry->second.expired() ? registry.entries.erase(entry) : std::next(entry);
      }
      registry.entries[authority_->epoch_id] = authority_;
    }

    authority_publication_t(const authority_publication_t &) = delete;
    authority_publication_t &operator=(const authority_publication_t &) = delete;

    authority_publication_t(authority_publication_t &&other) noexcept:
        authority_ {std::move(other.authority_)} {
    }

    authority_publication_t &operator=(authority_publication_t &&other) noexcept {
      if (this != &other) {
        reset();
        authority_ = std::move(other.authority_);
      }
      return *this;
    }

    ~authority_publication_t() {
      reset();
    }

    void reset() noexcept {
      if (!authority_) return;
      authority_->open.store(false);
      {
        auto &registry = detail::authority_registry();
        std::lock_guard lock {registry.mutex};
        const auto entry = registry.entries.find(authority_->epoch_id);
        if (entry != registry.entries.end() && entry->second.lock() == authority_) registry.entries.erase(entry);
      }
      authority_.reset();
    }

    bool published() const {
      return static_cast<bool>(authority_);
    }

  private:
    std::shared_ptr<epoch_authority_t> authority_;
  };

  /**
   * @brief Whether a frame of epoch_id may use its mapped source time now.
   *
   * frame_proof is the proof pointer the frame's provenance carries; it must be the epoch's own
   * object, not an equal copy. The lease check runs outside the registry lock.
   */
  inline authority_e check_epoch_authority(std::uint64_t epoch_id,
                                           std::uint64_t capture_generation_id,
                                           const proof_t *frame_proof) {
    std::shared_ptr<epoch_authority_t> authority;
    {
      auto &registry = detail::authority_registry();
      std::lock_guard lock {registry.mutex};
      const auto entry = registry.entries.find(epoch_id);
      if (entry == registry.entries.end()) return authority_e::no_epoch;
      authority = entry->second.lock();
    }
    if (!authority) return authority_e::no_epoch;
    if (!authority->open.load()) return authority_e::closed;
    if (authority->capture_generation_id != capture_generation_id) return authority_e::generation_mismatch;
    if (!frame_proof || authority->proof.get() != frame_proof) return authority_e::proof_mismatch;
    // Retain the original lease only for this check. An authority retained by a racing checker
    // cannot prolong the epoch's lease lifetime or adopt a replacement display's lease.
    const auto lease = authority->lease.lock();
    if (!lease || !lease->current()) return authority_e::lease_not_current;
    // Closed while the lease was being checked: still refused.
    return authority->open.load() ? authority_e::current : authority_e::closed;
  }

}  // namespace vrr_clock
