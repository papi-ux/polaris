/** Read-only installed-title metadata for the official Epic Linux Preview. */
#pragma once

#include <cstdint>
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
  // Host-side installed deployment, never a host-native /app executable or ownership grant.
  // This slice supports the default user and system installations; named installations
  // and paths exposed only through portals remain unavailable.
  struct flatpak_deployment_t {
    std::string scope, installation, arch, branch, commit;
    std::filesystem::path root, directory, app_home;
    std::string metadata, image_stamp;
    bool operator==(const flatpak_deployment_t &) const = default;
  };
  struct flatpak_document_t {
    std::string id, bus_id, service;
    std::uint64_t uid {0}, service_pid {0};
    std::filesystem::path mount_point, origin, metadata_dir;
    std::vector<std::string> permissions;
    bool operator==(const flatpak_document_t &) const = default;
  };
  using flatpak_document_reader_t = std::function<std::optional<flatpak_document_t>(const std::string &, const std::filesystem::path &)>;
  struct installation_t {
    identity_t identity;
    std::string installation_id, name, launch_executable, version;
    std::filesystem::path install_dir, data_root;
    std::string platform;
    std::optional<flatpak_deployment_t> flatpak;
    std::optional<flatpak_document_t> document;
    bool operator==(const installation_t &) const = default;
  };
  struct snapshot_t {
    bool available = false;
    std::vector<installation_t> games;
    std::vector<std::string> warnings;
    // Also identifies an empty Flatpak-only scan; it cannot create a native setup receipt.
    bool flatpak_only = false;
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
  // Explicit default-system root for inert fixtures; production uses /var/lib/flatpak.
  snapshot_t scan(const std::vector<std::filesystem::path> &homes, const std::filesystem::path &system_flatpak_root);
  snapshot_t scan(const std::vector<std::filesystem::path> &homes, const std::filesystem::path &system_flatpak_root,
                  const flatpak_document_reader_t &);
  // Bounded, read-only Documents Lookup/Info/GetMountPoint against a retained bus owner.
  std::optional<flatpak_document_t> read_flatpak_document(const std::string &, const std::filesystem::path &games);
  bool verify_flatpak_deployment(const flatpak_deployment_t &);
  // Pure strict projection of Flatpak's nonsecret info file. No process authority.
  std::optional<std::string> flatpak_instance_id(std::string_view info, const flatpak_deployment_t &);
  std::string flatpak_launch_command(const flatpak_deployment_t &, const identity_t &);
  // Read-only same-path prerequisite. The caller must independently prove the live
  // desktop process/root; this helper cannot select a process or grant ownership.
  struct flatpak_path_file_t {
    std::uint64_t device {0}, inode {0}, uid {0}, mode {0}, size {0};
    std::int64_t modified_ns {0}, changed_ns {0};
    bool operator==(const flatpak_path_file_t &) const = default;
  };
  struct flatpak_path_reader_t {
    std::uint64_t owner {0};
    std::function<std::optional<flatpak_path_file_t>(const std::filesystem::path &)> nofollow;
    std::function<std::optional<std::filesystem::path>(const std::filesystem::path &)> canonical;
    std::function<std::optional<std::string>(const std::filesystem::path &, std::size_t)> prefix;
    flatpak_document_reader_t document;
    // Only the read-only FUSE property user.document-portal.host-path is queried.
    std::function<std::optional<std::filesystem::path>(const std::filesystem::path &)> portal_host_path;
    std::function<std::optional<std::filesystem::path>(const std::filesystem::path &)> link;
    std::string host_mountinfo, sandbox_mountinfo;
  };
  // Exact private comparison material, not a wire identifier or a readiness claim.
  std::optional<std::string> flatpak_game_path_binding(const installation_t &, const std::filesystem::path &process_root,
                                                     const flatpak_path_reader_t &);
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
