/** @file src/encoder_probe_reuse.h
 * Identity-bound, process-local encoder capability reuse. No persistent authority.
 */
#pragma once

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace video::probe_reuse {
  struct identity_t {
    std::string gpu;
    std::string driver;
    std::string topology;
    std::string settings;

    bool complete() const {
      return !gpu.empty() && !driver.empty() && !topology.empty() && !settings.empty();
    }
    bool operator==(const identity_t &) const = default;
  };

  // Selection/capability state and this record share encoder_state_mutex.
  // Capture failures invalidate atomically without attempting to upgrade its
  // reader lock. An invalidation also survives a deferred external state reset.
  class cache_t {
  public:
    void invalidate() { generation.fetch_add(1); }
    std::uint64_t begin_probe() { return generation.fetch_add(1) + 1; }
    std::uint64_t epoch() const { return generation.load(); }

    bool reusable(const std::optional<identity_t> &current,
                  std::string_view selected, std::string_view configured,
                  bool mandatory_live_probe) const {
      return !mandatory_live_probe && current && current->complete() && saved &&
             saved_epoch == epoch() && *saved == *current && !selected.empty() &&
             saved_encoder == selected && (configured.empty() || configured == selected);
    }

    void remember(const std::optional<identity_t> &before,
                  const std::optional<identity_t> &after,
                  std::string_view selected, std::uint64_t successful_epoch) {
      saved.reset();
      // Identity must stay stable across the real probe. Loading another driver
      // library or changing capture transport requires another real validation.
      if (before && after && before->complete() && *before == *after &&
          !selected.empty() && successful_epoch == epoch()) {
        saved = after;
        saved_encoder = selected;
        saved_epoch = successful_epoch;
      }
    }

  private:
    std::atomic<std::uint64_t> generation {0};
    std::optional<identity_t> saved;
    std::string saved_encoder;
    std::uint64_t saved_epoch = 0;
  };
}  // namespace video::probe_reuse
