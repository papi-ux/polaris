/**
 * @file src/platform/linux/gamescope_process.h
 * @brief Exact-generation gamescope ownership and XWayland discovery helpers.
 */
#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

namespace stream_runtime::gamescope_process {

  struct marker_t {
    int pid = 0;
    std::uint64_t start_time = 0;
    std::string role;
    std::filesystem::path executable;

    bool operator==(const marker_t &) const = default;
  };

  struct lookup_paths_t {
    std::filesystem::path proc_root = "/proc";
    std::filesystem::path proc_net_unix = "/proc/net/unix";
    std::filesystem::path x11_socket_dir = "/tmp/.X11-unix";
  };

  std::optional<marker_t> read_marker(const std::filesystem::path &path);
  bool write_marker(const std::filesystem::path &path, const marker_t &marker);

  /** Capture PID generation after validating gamescope --backend headless argv. */
  std::optional<marker_t> marker_for_pid(
    int pid,
    std::string_view role,
    const lookup_paths_t &paths = {}
  );

  /** Validate an in-memory PID generation and gamescope --backend headless argv. */
  bool is_valid_headless_gamescope(
    const marker_t &marker,
    const lookup_paths_t &paths = {}
  );

  /** Validate PID generation, role, and gamescope --backend headless argv. */
  std::optional<marker_t> validated_marker(
    const std::filesystem::path &path,
    std::string_view expected_role = {},
    const lookup_paths_t &paths = {}
  );

  /** Test an exact argv entry only after validating the PID generation. */
  bool process_has_argument(
    const marker_t &marker,
    std::string_view argument,
    const lookup_paths_t &paths = {}
  );

  /** True only when marker is still valid and its process tree holds socket_path. */
  bool process_tree_owns_socket(
    const marker_t &marker,
    const std::filesystem::path &socket_path,
    const lookup_paths_t &paths = {}
  );

  /**
   * True when any process holds the unique /proc/net/unix inode for socket_path.
   * False when missing, filesystem-only residue, or no live holder.
   * Ambiguous duplicate pathname rows fail closed as "live" (not safe to unlink).
   */
  bool socket_has_live_holder(
    const std::filesystem::path &socket_path,
    const lookup_paths_t &paths = {}
  );

  /**
   * Unlink a crash-orphaned Wayland socket (and .lock) when no live holder exists.
   * Returns true when the path is gone afterward; false if a live holder (or
   * ambiguous pathname) still owns it.
   */
  bool remove_orphan_socket(
    const std::filesystem::path &socket_path,
    const lookup_paths_t &paths = {}
  );

  /** Lowest numbered X socket held by an Xwayland descendant of marker.pid. */
  std::optional<std::string> discover_owned_x11_display(
    const marker_t &marker,
    const lookup_paths_t &paths = {}
  );

}  // namespace stream_runtime::gamescope_process
