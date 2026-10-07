/**
 * @file src/vrr_session_policy.h
 * @brief What a client asked for about variable refresh, and what this host actually does about it.
 *
 * A client that wants its frames on the game's own cadence says so with `clientVrrRequested` on
 * /launch and /resume. The design of that flag is credited to Nonary's moonlight-qt VRR work
 * (https://github.com/Nonary/moonlight-qt): Nonary's VRR client (v6.1.0-vrr18) sends
 * clientVrrRequested=1 only when VRR is requested. No code from it is used here.
 *
 * The request is only an input. Effective source-time pacing needs a capture route proven to belong to
 * the generation, frames carrying the compositor's presentation time, a clock proven to be the one
 * that time is in, and an encode loop that keeps it. Default policy alone never grants that claim;
 * the private opt-in encode loop reports its generation-bound runtime observations separately.
 */
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace vrr_policy {

  enum class requested_e {
    absent,
    off,
    on,
    malformed,
  };

  enum class admission_e {
    launch,
    resume,
  };

  /** What one compositor presentation time turned out to be. */
  enum class presentation_status_e {
    valid,
    missing,
    invalid_nsec,
    invalid_overflow,
    nonmonotonic,
  };

  inline constexpr std::array k_presentation_statuses {
    presentation_status_e::valid,
    presentation_status_e::missing,
    presentation_status_e::invalid_nsec,
    presentation_status_e::invalid_overflow,
    presentation_status_e::nonmonotonic,
  };

  inline constexpr std::string_view k_request_arg = "clientVrrRequested";

  inline std::string_view name(requested_e value) {
    switch (value) {
      case requested_e::off:
        return "false";
      case requested_e::on:
        return "true";
      case requested_e::malformed:
        return "malformed";
      case requested_e::absent:
        break;
    }
    return "absent";
  }

  inline std::string_view name(admission_e value) {
    return value == admission_e::resume ? "resume" : "launch";
  }

  inline std::string_view name(presentation_status_e value) {
    switch (value) {
      case presentation_status_e::missing:
        return "missing";
      case presentation_status_e::invalid_nsec:
        return "invalid_nsec";
      case presentation_status_e::invalid_overflow:
        return "invalid_overflow";
      case presentation_status_e::nonmonotonic:
        return "nonmonotonic";
      case presentation_status_e::valid:
        break;
    }
    return "valid";
  }

  /**
   * @brief Read the request from every value the query carried for the flag.
   *
   * Nonary's VRR client sends `1`. `0`, `1`, `true` and `false` are accepted in any case. Anything else, an
   * empty value or the flag given twice is malformed: a client that sends two answers has not given
   * one, and guessing would let a later default decide what it asked for.
   */
  inline requested_e parse_request(const std::vector<std::string_view> &values) {
    if (values.empty()) {
      return requested_e::absent;
    }
    if (values.size() != 1) {
      return requested_e::malformed;
    }
    std::string lowered;
    lowered.reserve(values.front().size());
    for (const auto ch : values.front()) {
      lowered.push_back(static_cast<char>(ch >= 'A' && ch <= 'Z' ? ch - 'A' + 'a' : ch));
    }
    if (lowered == "1" || lowered == "true") {
      return requested_e::on;
    }
    if (lowered == "0" || lowered == "false") {
      return requested_e::off;
    }
    return requested_e::malformed;
  }

  /** One stream generation's request, frozen when the stream session is allocated. */
  struct record_t {
    std::uint64_t session_generation = 0;
    requested_e requested = requested_e::absent;
    admission_e admission = admission_e::launch;
    bool watch_or_input_only = false;
    // Private host opt-in, snapshotted at authenticated admission.
    bool host_source_pacing = false;
  };

  struct pacing_intent_t {
    bool host_opt_in = false;
    requested_e requested = requested_e::absent;
    bool watch_or_input_only = false;

    constexpr bool eligible() const {
      return host_opt_in && requested == requested_e::on && !watch_or_input_only;
    }
  };

  /**
   * @brief Presentation-time availability as the host capture producer saw it, across every route and
   *        session it served. Not attributable to any one stream generation, and never cadence evidence.
   */
  struct host_presentation_counts_t {
    std::array<std::uint64_t, k_presentation_statuses.size()> counts {};

    void count(presentation_status_e status) {
      auto &slot = counts[static_cast<std::size_t>(status)];
      if (slot < UINT64_MAX) {
        ++slot;
      }
    }

    std::uint64_t operator[](presentation_status_e status) const {
      return counts[static_cast<std::size_t>(status)];
    }
  };

  /**
   * @brief Freeze one generation's request from its admission.
   *
   * A Watch or input-only session carries no request of its own: it never owns the capture cadence,
   * so whatever it sent is kept but its policy is always watch_or_input_only.
   */
  inline record_t make_record(std::uint64_t session_generation,
                              requested_e requested,
                              admission_e admission,
                              bool watch_only,
                              bool input_only) {
    record_t record;
    record.session_generation = session_generation;
    record.requested = requested;
    record.admission = admission;
    record.watch_or_input_only = watch_only || input_only;
    return record;
  }

  struct decision_t {
    bool effective = false;
    std::string_view pacing_mode = "fixed_grid";
    std::string_view fallback_reason;
  };

  /**
   * @brief The policy this host applies to one generation, with the first reason that applies.
   *
   * @param capture_pacing The pacing label of a capture route proven to belong to this generation:
   *        "source_driven" or "fixed_interval". Pass empty when no route is proven for it, which is
   *        every production caller today; the process-wide capture label must never stand in for it.
   */
  inline decision_t decide(const record_t &record, std::string_view capture_pacing) {
    decision_t decision;
    if (record.watch_or_input_only) {
      decision.fallback_reason = "watch_or_input_only";
    } else if (record.requested == requested_e::malformed) {
      decision.fallback_reason = "request_malformed";
    } else if (record.requested != requested_e::on) {
      decision.fallback_reason = "not_requested";
    } else if (capture_pacing != "source_driven" && capture_pacing != "fixed_interval") {
      decision.fallback_reason = "route_unreported";
    } else if (capture_pacing == "fixed_interval") {
      decision.fallback_reason = "route_fixed_interval";
    } else {
      // A source-driven route still has no verified presentation-clock namespace, steady-clock probe
      // or source-time encode loop. Until those gates exist this is the terminal reason.
      decision.fallback_reason = "clock_domain_unverified";
    }
    return decision;
  }

  /** The capture pacing label as the diagnostic object reports it. */
  inline std::string_view reported_capture_pacing(std::string_view capture_pacing) {
    return capture_pacing == "source_driven" || capture_pacing == "fixed_interval" ? capture_pacing : "unknown";
  }

}  // namespace vrr_policy
