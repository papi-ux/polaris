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

  /**
   * Whether a probe on this route can carry an identity the reuse gate could ever match. Only
   * NVENC on the owned labwc private compositor, capturing through wlr, gives one today. Every
   * other route, Gamescope Stream and the portal included, probes fresh every time, and so does
   * every other encoder on labwc. current_probe_identity() asks this before providers or hardware.
   */
  constexpr bool route_carries_identity(
    bool private_compositor,
    std::string_view requested_capture,
    std::string_view encoder
  ) {
    return private_compositor && (requested_capture.empty() || requested_capture == "wlr") &&
           encoder == "nvenc";
  }

  /**
   * Whether an encoder probe must run live instead of reusing the last result. An encoder of last
   * resort always probes again, and so does Vulkan Video, configured or chosen, whatever the Auto
   * policy says. The policy's exact_live_probe_required is one input, and only Auto sets it, so
   * explicit Vulkan reads false there and still never reuses a probe (#635).
   */
  constexpr bool live_probe_mandatory(
    bool always_reprobe,
    bool policy_requires_live_probe,
    std::string_view configured_encoder,
    std::string_view encoder
  ) {
    return always_reprobe || policy_requires_live_probe || configured_encoder == "vulkan" ||
           encoder == "vulkan";
  }

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

  /**
   * What the encoder_auto line says the reuse gate will do with the next probe (#635).
   *
   * gate_reuses must be the gate's own answer: cache_t::reusable() asked, right after remember(),
   * with the identity this probe recorded and the inputs the next probe's gate uses. So the line
   * says the next probe is reused only where the gate would reuse it, and only while nothing the
   * gate keys on changes. Every other answer says why a fresh probe runs: the rule in
   * live_probe_mandatory(), a route with no identity at all, or a probe that left none to match.
   */
  constexpr std::string_view next_probe_reuse(
    bool gate_reuses,
    bool always_reprobe,
    bool policy_requires_live_probe,
    std::string_view configured_encoder,
    std::string_view encoder,
    bool route_has_identity
  ) {
    if (gate_reuses) {
      return "if_unchanged (the next probe reuses this one while the GPU, driver, capture generation and settings stay the same)";
    }
    if (configured_encoder == "vulkan" || encoder == "vulkan") {
      return "off (Vulkan Video probes fresh every time)";
    }
    if (always_reprobe) {
      return "off (an encoder of last resort probes fresh every time)";
    }
    if (policy_requires_live_probe) {
      return "off (this Auto policy asks for a fresh probe every time)";
    }
    if (!route_has_identity) {
      return "off (a fresh probe runs every time here: only NVENC on the labwc private compositor is ever reused)";
    }
    return "off (this probe left no identity the gate can match, so the next one runs fresh)";
  }
}  // namespace video::probe_reuse
