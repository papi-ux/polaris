/** Read-only installed-title metadata for the official Epic Linux Preview. */
#pragma once

#include <filesystem>
#include <functional>
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
    bool operator==(const installation_t &) const = default;
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
  // Exact available metadata content, independent of discovery order. Not a wire identifier.
  bool same_snapshot(const snapshot_t &, const snapshot_t &);
  std::optional<installation_t> find(const snapshot_t &, const identity_t &);
  // A catalog is only a title/identity hint. It never authorizes launch or termination.
  bool matches_heroic_catalog(const identity_t &, std::string_view bytes, std::string *name = nullptr);
  std::optional<identity_t> heroic_identity(const std::string &artifact_id,
    const std::vector<std::filesystem::path> &homes, std::string *name = nullptr);
  std::string launch_uri(const identity_t &);
  std::string launch_command(const std::filesystem::path &launcher, const identity_t &);
  /**
   * The startup client, for bash, that starts a retained AppImage under headless gamepad isolation.
   * Its runtime cannot mount inside the sandbox, which has no /dev/fuse and ignores setuid
   * fusermount, so it mounts outside first, in Polaris's own mount namespace, where the launcher's
   * executable stays statable by its host path. Only AppRun enters the sandbox, wrapped by isolate,
   * and only it ever gets the request. The mount stays up until End: a launcher can return while
   * its children still run from it.
   */
  std::string appimage_stage_command(const std::filesystem::path &image, const std::filesystem::path &install_dir,
    const identity_t &, const std::function<std::string(const std::string &)> &isolate);
}
