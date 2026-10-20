#pragma once

#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

/** Diagnostic observations only. No value in this namespace authorizes a setting or pacing change. */
namespace game_mode_adaptive_sync {
  using steady_clock_t = std::chrono::steady_clock;

  struct property_t {
    bool present = false;
    bool cardinal = false;
    std::uint8_t format = 0;
    std::uint32_t count = 0;
    std::uint32_t bytes_after = 0;
    std::uint32_t value = 0;
  };

  struct snapshot_t {
    property_t capable;
    property_t feedback;
    bool torn = false;
  };

  struct generation_t {
    // Private process identity, intentionally excluded from the client JSON representation.
    std::int64_t pid = 0;
    std::uint64_t start_time = 0;
    std::uint64_t exe_device = 0;
    std::uint64_t exe_inode = 0;
    std::uint64_t exe_size = 0;
    std::int64_t exe_mtime_ns = 0;
    bool operator==(const generation_t &) const = default;
  };

  struct version_t {
    std::string producer_exe;
    std::map<std::string, std::string> os_release;
    std::optional<std::string> package;
    std::vector<std::string> operator_reports;

    bool captured() const {
      return !producer_exe.empty() && !os_release.empty();
    }
  };

  struct observation_t {
    bool route_bound = false;
    bool generation_checked = false;
    bool connector_bound = false;
    bool amdgpu = false;
    std::string reason = "producer_unbound";
    generation_t generation;
    version_t version;
    snapshot_t properties;
    steady_clock_t::time_point observed_at {};
    std::optional<steady_clock_t::time_point> generation_checked_at;
  };

  enum class last_state_e { active, inactive, inactive_unsupported };
  inline const char *name(last_state_e value) {
    switch (value) {
      case last_state_e::active: return "active";
      case last_state_e::inactive: return "inactive";
      case last_state_e::inactive_unsupported: return "inactive_unsupported";
    }
    return "unknown";
  }

  inline const char *snapshot_reason(const observation_t &value) {
    if (!value.route_bound || !value.generation_checked) return "producer_unbound";
    if (value.properties.torn) return "x_snapshot_torn";
    for (const auto &property : {value.properties.capable, value.properties.feedback}) {
      if (!property.present) return "property_absent";
      if (!property.cardinal || property.format != 32 || property.count != 1 || property.bytes_after != 0) return "property_malformed";
      if (property.value > 1) return "property_out_of_domain";
    }
    if (value.properties.feedback.value == 1 && value.properties.capable.value == 0) return "contradictory_properties";
    if (value.properties.capable.value == 0 && !value.connector_bound) return "connector_unbound";
    return "property_read_bracketed";
  }

  struct record_t {
    observation_t attempt;
    std::optional<observation_t> last_known;
    bool ended = false;
  };

  struct symptom_t {
    // Supplied by an independent symptom detector, never inferred from a VRR property or silence
    // on a source-driven capture of a static desktop.
    bool freeze = false;
    steady_clock_t::time_point onset {};
  };

  struct evaluation_t {
    const char *current_state = "unknown";
    const char *current_reason = "no_independent_freshness";
    std::optional<last_state_e> last_state;
    std::optional<std::int64_t> age_ms;
    bool same_generation = false;
    const char *advice = "none";
    const char *range_status = "unmapped";
  };

  inline evaluation_t evaluate(const record_t &record, steady_clock_t::time_point now, symptom_t symptom = {}) {
    evaluation_t result;
    if (symptom.freeze) result.advice = "generic_freeze";
    if (!record.last_known || now < record.last_known->observed_at) return result;
    const auto &last = *record.last_known;
    // Treat even synthetic or corrupted records as untrusted inputs to the evaluator.
    if (std::string_view(snapshot_reason(last)) != "property_read_bracketed") return result;
    result.last_state = last.properties.feedback.value == 1 ? last_state_e::active :
                        last.properties.capable.value == 0 ? last_state_e::inactive_unsupported : last_state_e::inactive;
    result.age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - last.observed_at).count();
    result.same_generation = !record.ended && record.attempt.generation_checked && record.attempt.route_bound &&
                             record.attempt.generation_checked_at && *record.attempt.generation_checked_at <= now &&
                             record.attempt.generation == last.generation;
    if (symptom.freeze && symptom.onset > steady_clock_t::time_point {} && symptom.onset <= now && result.same_generation &&
        *record.attempt.generation_checked_at >= symptom.onset && last.amdgpu && last.version.captured() &&
        result.last_state == last_state_e::active) {
      // A read before onset or within the freeze window is a correlation, never a current-state
      // confirmation. The version range remains unmapped, including operator fix claims.
      result.advice = "qualified_correlation";
    }
    return result;
  }

  /** The capture owns its writer; accepted images carry a reader through a freeze or disconnect. */
  class store_t {
  public:
    void publish(observation_t observation) {
      std::lock_guard lock(mutex_);
      if (record_.ended) return;
      if (std::string_view(snapshot_reason(observation)) == "property_read_bracketed") record_.last_known = observation;
      record_.attempt = std::move(observation);
    }
    void end() {
      std::lock_guard lock(mutex_);
      record_.ended = true;
    }
    record_t read() const {
      std::lock_guard lock(mutex_);
      return record_;
    }
    std::shared_ptr<const store_t> retired_snapshot() const {
      auto result = std::make_shared<store_t>();
      result->record_ = read();
      result->record_.ended = true;
      return result;
    }
  private:
    mutable std::mutex mutex_;
    record_t record_;
  };
}
