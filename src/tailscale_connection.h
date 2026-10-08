/** Read-only, address-free Tailscale evidence. Never a media-quality or trust policy. */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <nlohmann/json.hpp>

namespace tailscale_connection {
  constexpr std::int64_t max_age_ms = 10000;
  constexpr std::size_t max_status_bytes = 512 * 1024;

  // control_connection requires both actual TLS endpoints; peer_status describes
  // only a daemon peer matched to a current stream's address, never its media route.
  nlohmann::json unknown(std::string_view scope, std::string_view reason,
                         std::string_view address_kind = "unknown");
  nlohmann::json from_status(std::string_view status, std::string_view remote_ip,
                             std::string_view local_ip, std::int64_t observed_at_ms,
                             std::string_view scope = "control_connection");
  std::optional<std::string> read_status_socket(std::string_view socket_path);
  nlohmann::json observe(std::string_view remote_ip, std::string_view local_ip = {},
                         std::string_view scope = "control_connection");
}  // namespace tailscale_connection
