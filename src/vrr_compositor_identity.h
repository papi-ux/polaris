/**
 * @file src/vrr_compositor_identity.h
 * @brief Match one connected presentation peer to a retained, owned compositor generation.
 *
 * Diagnostic authority only. A match does not enable VRR or source-time pacing. The runtime owns
 * the lease and its kernel descriptors; the collector brackets its peer/clock reads with current()
 * so a retired or dead generation cannot qualify a new capture epoch. These process facts stay
 * private and are never a public diagnostics payload.
 */
#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <utility>

namespace vrr_clock {

  struct compositor_process_t {
    std::int64_t pid = 0;
    std::uint32_t uid = 0;
    std::uint64_t starttime = 0;
    bool operator==(const compositor_process_t &) const = default;
  };

  /** Immutable expected process, backed by the runtime's retained generation ownership. */
  class compositor_lease_t {
  public:
    explicit compositor_lease_t(compositor_process_t expected): process(expected) {}
    virtual ~compositor_lease_t() = default;

    const compositor_process_t process;

    /**
     * True only while this exact generation remains published, its compositor pidfd is alive, and
     * its pinned Wayland socket still names the expected object. Unsupported reads fail closed.
     * Implementations must be safe for capture readers racing runtime retirement/replacement.
     */
    [[nodiscard]] virtual bool current() const = 0;
  };

  struct compositor_match_t {
    bool verified = false;
    std::string_view reason = "no_authoritative_compositor_identity";
  };

  /**
   * Bracket a collector's actual peer reads, rather than accepting a detached caller boolean. A
   * shared lease pins ownership across those reads; current() must still refuse a retired lease.
   */
  class compositor_identity_check_t {
  public:
    explicit compositor_identity_check_t(std::shared_ptr<const compositor_lease_t> lease):
        lease_(std::move(lease)), current_before_(lease_ && lease_->current()) {}

    [[nodiscard]] compositor_match_t verify(
      std::int64_t peer_pid,
      std::optional<std::uint32_t> peer_uid,
      std::optional<std::uint64_t> starttime_before,
      std::optional<std::uint64_t> starttime_after
    ) const {
      if (!lease_) return {};
      if (!current_before_) return {false, "compositor_generation_unavailable"};
      if (!lease_->current()) return {false, "compositor_generation_changed"};
      const auto &expected = lease_->process;
      if (expected.pid <= 1 || expected.starttime == 0) return {false, "compositor_identity_invalid"};
      if (peer_pid != expected.pid) return {false, "compositor_peer_pid_mismatch"};
      if (!peer_uid || *peer_uid != expected.uid) return {false, "compositor_peer_uid_mismatch"};
      if (!starttime_before || !starttime_after) return {false, "compositor_peer_starttime_unavailable"};
      if (*starttime_before != expected.starttime || *starttime_after != expected.starttime) {
        return {false, "compositor_peer_starttime_mismatch"};
      }
      return {true, "authoritative_compositor_peer_matched"};
    }

  private:
    std::shared_ptr<const compositor_lease_t> lease_;
    bool current_before_ = false;
  };

}  // namespace vrr_clock
