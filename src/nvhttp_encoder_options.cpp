/**
 * @file src/nvhttp_encoder_options.cpp
 * @brief The encoder backends a client may request, and when each may fall back.
 */
// macros
#define BOOST_BIND_GLOBAL_PLACEHOLDERS

// standard includes
#include <string>
#include <string_view>

// lib includes
#include <nlohmann/json.hpp>

// local includes
#include "generated/polaris_v1_contract.h"
#include "nvhttp.h"
#include "video.h"

namespace v1 = polaris::contract::v1;

namespace nvhttp {

  bool encoder_backend_fallback_allowed(
      std::string_view requested_backend,
      bool session_override) {
    if (session_override) {
      return requested_backend == "auto";
    }
    return requested_backend != "vulkan";
  }

  nlohmann::json encoder_backend_options_json() {
    nlohmann::json options = nlohmann::json::array();
    for (const auto &backend : video::selectable_encoder_backends()) {
      std::string label;
      std::string reason;
      if (backend == "auto") {
        label = "Auto";
        reason = "Let Polaris select and live-validate the best encoder for this launch.";
      } else if (backend == "nvenc") {
        label = "NVIDIA NVENC";
        reason = "Require the NVIDIA NVENC backend for this launch.";
      } else if (backend == "vaapi") {
        label = "VA-API";
        reason = "Require the Linux VA-API backend for this launch.";
      } else if (backend == "vulkan") {
        label = "Vulkan Video";
        reason = "Require the experimental Vulkan Video backend for this launch.";
      } else if (backend == "quicksync") {
        label = "Intel Quick Sync";
        reason = "Require the Intel Quick Sync backend for this launch.";
      } else if (backend == "amdvce") {
        label = "AMD AMF/VCE";
        reason = "Require the AMD AMF/VCE backend for this launch.";
      } else if (backend == "videotoolbox") {
        label = "VideoToolbox";
        reason = "Require the macOS VideoToolbox backend for this launch.";
      } else if (backend == "software") {
        label = "Software";
        reason = "Require CPU software encoding for this launch.";
      } else {
        label = backend;
        reason = "Require this encoder backend for this launch.";
      }
      options.push_back({
        {v1::client_settings_encoder_option::value, backend},
        {v1::client_settings_encoder_option::label, label},
        {v1::client_settings_encoder_option::available, true},
        {v1::client_settings_encoder_option::experimental, backend == "vulkan"},
        {v1::client_settings_encoder_option::fallback_allowed, encoder_backend_fallback_allowed(backend, true)},
        {v1::client_settings_encoder_option::runtime_validation, "launch"},
        {v1::client_settings_encoder_option::reason, reason}
      });
    }
    return options;
  }

}  // namespace nvhttp
