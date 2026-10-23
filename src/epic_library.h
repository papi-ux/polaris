/** Read-only installed-title metadata for the official Epic Linux Preview. */
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace epic_library {
  struct identity_t {
    std::string namespace_id, item_id, artifact_id;
    bool operator==(const identity_t &) const = default;
  };
  struct installation_t {
    identity_t identity;
    std::string installation_id, name, launch_executable, version;
    std::filesystem::path install_dir, data_root;
    std::string platform;
  };
  struct snapshot_t {
    bool available = false;
    std::vector<installation_t> games;
    std::vector<std::string> warnings;
  };
  bool valid_identity(const identity_t &identity);
  struct control_request_t {
    std::string action, session_id, revision;
    bool confirmed = false;
  };
  std::optional<control_request_t> parse_control_request(std::string_view bytes);
  snapshot_t parse_installations(std::string_view bytes, const std::filesystem::path &data_root);
  snapshot_t read_installations(const std::filesystem::path &data_root);
  snapshot_t scan(const std::vector<std::filesystem::path> &homes);
  std::optional<installation_t> find(const snapshot_t &, const identity_t &);
  // A catalog is only a title/identity hint. It never authorizes launch or termination.
  bool matches_heroic_catalog(const identity_t &, std::string_view bytes, std::string *name = nullptr);
  std::optional<identity_t> heroic_identity(const std::string &artifact_id,
    const std::vector<std::filesystem::path> &homes, std::string *name = nullptr);
  std::string launch_uri(const identity_t &);
  std::string launch_command(const std::filesystem::path &launcher, const identity_t &);
}
