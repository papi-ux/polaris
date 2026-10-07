/**
 * @file src/platform/linux/steamos_adapter.h
 * @brief #204: the production SteamOS session-switch adapter and session observer.
 *
 * Default off: nothing here runs unless an app opted in and the coordinator admitted its launch. The
 * adapter then runs exactly `/usr/bin/steamosctl switch-to-desktop-mode` or `switch-to-game-mode`,
 * as an owned child with a fixed environment naming this account's own user bus, and only after a
 * fresh support proof: the trusted executable, the one installed and audited steamos-manager
 * package owning it, and this account's own manager offering the switch. Exit 0 is reported as
 * "request sent"; the coordinator proves what happened from the session records.
 */
#pragma once

#include "src/owned_child.h"
#include "src/platform/linux/steamos_bus.h"
#include "src/prelaunch_transition.h"

#include <memory>
#include <mutex>

namespace prelaunch::steamos {
  /// The fixed production sources for this process's account.
  bus::sources_t production_sources();

  /// One fresh read of the host's sessions, bound and checked against `since` (see host_t::observe()).
  observation_t observe(const bus::sources_t &sources, const session_identity_t *since, const bus::budget_t &budget, bool prove_service = false);

  class steamos_adapter_t final: public adapter_t {
  public:
    explicit steamos_adapter_t(bus::sources_t sources = production_sources());
    ~steamos_adapter_t() override;

    bool implemented() const override;
    bool command_present() const override;
    verdict_t support(const wait_t &wait) override;
    /**
     * @brief Support for display, within the readiness budget.
     *
     * A positive answer is kept for at most k_readiness_cache while the boot, the command, the
     * package database and the exact desc/files of each steamos-manager record, the account's bus
     * socket and the manager's unique bus name are all unchanged. A refusal or an unknown (timeout)
     * answers for k_refusal_cache without any read, so one app list costs at most one budget, and
     * is never turned into a positive answer. Launches never use this: they call support().
     */
    verdict_t readiness_support(const wait_t &wait) override;
    bool start(direction_e direction) override;
    step_e poll() override;
    void abort() override;

    static constexpr std::chrono::seconds k_readiness_cache {5};
    static constexpr std::chrono::seconds k_refusal_cache {2};

  private:
    /// The facts a kept readiness answer rests on; empty when they could not be read.
    std::string fingerprint(const bus::budget_t &budget) const;

    bus::sources_t sources_;
    std::unique_ptr<owned_child::child_t> child_;

    std::mutex cache_mutex_;
    std::string cache_key_;
    verdict_t cache_verdict_;
    std::chrono::steady_clock::time_point cache_at_;
    verdict_t refusal_;
    std::chrono::steady_clock::time_point refusal_at_;
  };
}  // namespace prelaunch::steamos
