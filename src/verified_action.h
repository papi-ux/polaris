/**
 * @file src/verified_action.h
 * @brief Read-back confirmation for actions whose failure is otherwise silent.
 *
 * The bugs that cost the most here are not crashes. They are actions that
 * report success and do not happen: a compositor CLI that exits 0 on a rejected
 * mode set, a setup step that is skipped rather than run, a capture path that
 * is requested and quietly falls back. Nothing faults, the stream comes up, and
 * the only symptom is that the user's choice did not apply.
 *
 * Polaris already models one of these correctly. The Linux GPU profile records
 * `gpu_native_requested` alongside `gpu_native_succeeded`, and the
 * Troubleshooting screen reads the pair to tell the user that GPU-native
 * capture was asked for and did not happen. This generalizes that shape: ask
 * the system what it actually did, compare it against what was requested, and
 * make the mismatch loud and reportable instead of invisible.
 *
 * The rule this encodes is that a return code is not evidence. Both silent
 * failures found in the field had a truthful answer available from the system
 * itself; the code simply never asked for it.
 */
#pragma once

// standard includes
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// lib includes
#include <nlohmann/json.hpp>

namespace verified_action {
  /** @brief Most mismatches retained for the support bundle. */
  inline constexpr std::size_t max_retained_records = 32;

  /**
   * @brief One action that reported success and did not land.
   */
  struct record_t {
    std::string id;           ///< Stable identifier, e.g. "display.topology.mode".
    std::string description;  ///< What was being attempted, in a user's terms.
    std::string requested;    ///< What the caller asked for.
    std::string actual;       ///< What the system reported back afterwards.
    std::string observed_at;  ///< UTC ISO 8601 timestamp.
  };

  /**
   * @brief Confirm an action landed by comparing a read-back against the request.
   *
   * Call after performing the action, passing the value read back out of the
   * system rather than anything derived from the call's own return code.
   *
   * @param id Stable identifier for this check.
   * @param description What was being attempted.
   * @param requested The value that was asked for.
   * @param actual The value read back afterwards.
   * @return True when the action landed, false when it silently did not.
   */
  bool confirm(
    std::string_view id,
    std::string_view description,
    std::string_view requested,
    std::string_view actual
  );

  /**
   * @brief Confirm an action landed where there is no value to compare.
   *
   * For steps whose only observable is whether they ran at all. Prefer the
   * value-comparing overload wherever the system can be asked what it did:
   * "it ran" is a weaker claim than "it produced what was asked for".
   *
   * @param id Stable identifier for this check.
   * @param description What was being attempted.
   * @param landed Whether the read-back showed the action took effect.
   * @return The value of `landed`, so callers can branch on it.
   */
  bool confirm(std::string_view id, std::string_view description, bool landed);

  /**
   * @brief Mismatches recorded so far, oldest first.
   */
  std::vector<record_t> silent_failures();

  /**
   * @brief Render recorded mismatches for the doctor payload and support bundle.
   */
  nlohmann::json to_json();

  /**
   * @brief Drop all recorded mismatches.
   */
  void clear();
}  // namespace verified_action
