/**
 * @file tests/fuzz/fuzz_confighttp_json.cpp
 * @brief Exercise the production config/app validators without HTTP or persistence.
 */
#include "confighttp_validation.h"

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t *data, std::size_t size) {
  // A harness resource limit, not the HTTP server's request admission policy.
  constexpr std::size_t max_input_bytes = 64 * 1024;
  if (size > max_input_bytes) {
    return 0;
  }
  const char *begin = size ? reinterpret_cast<const char *>(data) : "";
  nlohmann::json payload;
  try {
    payload = nlohmann::json::parse(begin, begin + size);
  } catch (const nlohmann::json::parse_error &) {
    // Malformed JSON is an expected parser rejection. Validator exceptions remain visible.
    return 0;
  }

  std::string config_error;
  const bool config_valid = confighttp::validation::validate_config_payload(payload, config_error);
  std::string app_error;
  (void) confighttp::validation::validate_app_payload(payload, app_error);
  if (config_valid) {
    confighttp::validation::normalize_write_only_secret_payload(payload);
  }
  return 0;
}
