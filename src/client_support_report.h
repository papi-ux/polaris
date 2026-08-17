/**
 * @file src/client_support_report.h
 * @brief Intake for support reports submitted by a paired client.
 *
 * A streaming bug has two halves and the host only ever saw one of them. The
 * host bundle knows the capture path, the encoder and the frame timings; the
 * client knows what the user actually saw and whether the app fell over. Asking
 * a user to produce both, from two devices, and keep them matched, is how a bug
 * report turns into a conversation instead of a fix.
 *
 * A paired client can post its half here, and the host carries it into the same
 * bundle. Reports are held in memory only: this is client-supplied content, and
 * writing it to disk on the host would turn a support feature into a way for a
 * paired device to leave files behind.
 */
#pragma once

// standard includes
#include <chrono>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

// lib includes
#include <nlohmann/json.hpp>

namespace client_support_report {
  /** @brief Largest submission accepted, before parsing it. */
  inline constexpr std::size_t max_report_bytes = 256U * 1024U;

  /** @brief How many clients' reports are retained. */
  inline constexpr std::size_t max_retained_reports = 8;

  /** @brief Longest field retained from a submission, to bound one client's share. */
  inline constexpr std::size_t max_field_bytes = 64U * 1024U;

  /** @brief Minimum gap between accepted submissions from one client. */
  inline constexpr std::chrono::seconds min_submission_interval {10};

  struct report_t {
    std::string client_id;
    std::string device;
    std::string nova_version;
    std::string android_release;
    std::string occurred_at;
    std::string received_at;
    std::string notes;
    std::string crash;
    std::string log_tail;
  };

  /**
   * @brief Outcomes of parsing a submission.
   *
   * Rate limiting is not here: it is decided before the body is read, so a
   * client that is submitting too fast never gets its payload parsed at all.
   */
  enum class accept_e {
    accepted,
    too_large,
    malformed,
  };

  struct parse_result_t {
    accept_e status = accept_e::malformed;
    report_t report;
    std::string error;
  };

  /**
   * @brief Validate and normalize a submission.
   *
   * Pure, so the size bound, the field bounds and the rejection reasons can be
   * tested without a paired device or a TLS session.
   *
   * @param body Raw request body.
   * @param client_id Identity the host derived from the client certificate, not
   *                  anything the body claims about itself.
   * @param received_at Host timestamp to record.
   */
  parse_result_t parse_submission(
    std::string_view body,
    std::string_view client_id,
    std::string_view received_at
  );

  /**
   * @brief Whether this client may submit again yet.
   *
   * Records the attempt when it allows one, so callers must only ask once per
   * submission.
   */
  bool rate_limit_allows(std::string_view client_id, std::chrono::steady_clock::time_point now);

  /**
   * @brief Retain a report, evicting the oldest once full.
   */
  void store(report_t report);

  /**
   * @brief Retained reports, oldest first.
   */
  std::vector<report_t> recent();

  /**
   * @brief Render retained reports for the diagnostics API and support bundle.
   *
   * Values are passed through as submitted. The client redacts before sending,
   * and the export layer redacts again on the way out, so the host does not
   * need a third copy of those rules; what it must not do is assume the client
   * got it right, which is why nothing here is treated as trusted text.
   */
  nlohmann::json to_json();

  /**
   * @brief Current host wall-clock time as UTC ISO 8601.
   */
  std::string utc_timestamp_now();

  void clear();
}  // namespace client_support_report
