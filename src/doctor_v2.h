/**
 * @file src/doctor_v2.h
 * @brief Shadow-mode Doctor v2 evidence ingestion and classification.
 */
#pragma once

#include <nlohmann/json.hpp>

#include <string>

namespace doctor_v2 {

  bool shadow_enabled();
  bool trials_enabled();

  /** Ingest one paired-client raw monotonic sample. */
  nlohmann::json ingest(const std::string &owner_uuid,
                        const std::string &app_uuid,
                        const nlohmann::json &payload);

  /** Build a read-only v2 status from stored client samples and host evidence. */
  nlohmann::json status(const std::string &owner_uuid,
                        const std::string &app_uuid,
                        const nlohmann::json &host_evidence = nlohmann::json::object());

#ifdef POLARIS_TESTS
  void clear_for_tests();
#endif

}  // namespace doctor_v2
