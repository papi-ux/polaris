/**
 * @file src/platform/linux/user_unit_override.h
 * @brief Which binary the polaris user service really runs, and which one is running now.
 *
 * The Bazzite DRM/KMS recipe points the user service at a writable copy of the
 * binary through a drop-in so the copy can hold CAP_SYS_ADMIN. Remove the copy
 * without its drop-in and the service execs a path that no longer exists;
 * update the package and the copy silently stays on the old version. Both read
 * as "Polaris is broken" from the console, and neither shows anywhere but
 * `systemctl --user cat polaris`. These helpers read the same facts so
 * --setup-host, the update status and the Doctor can say them.
 */
#pragma once

#include "executable_path.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>
#include <unistd.h>

namespace platf::user_unit {
  struct exec_override_t {
    std::filesystem::path drop_in;  ///< the drop-in that last assigned ExecStart; empty when none did
    std::string exec_start;  ///< the effective ExecStart value; empty when unset or reset to nothing
    std::filesystem::path binary;  ///< the command's first word when it is an absolute path
    bool binary_missing = false;  ///< the binary is an absolute path that is not an executable file

    bool active() const {
      return !exec_start.empty();
    }
  };

  inline std::string_view trim_view(std::string_view text) {
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r')) {
      text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r')) {
      text.remove_suffix(1);
    }
    return text;
  }

  /**
   * @brief The ExecStart a unit's drop-in directory leaves in force.
   *
   * systemd reads `*.conf` drop-ins in lexical order; within [Service], an empty
   * `ExecStart=` clears the list and a value appends to it. Polaris' unit is
   * Type=simple, so one value is all systemd accepts and the last one wins.
   */
  inline exec_override_t effective_exec_override(const std::filesystem::path &drop_in_dir) {
    exec_override_t out;
    std::error_code ec;
    if (!std::filesystem::is_directory(drop_in_dir, ec)) {
      return out;
    }

    std::vector<std::filesystem::path> files;
    for (const auto &entry : std::filesystem::directory_iterator(drop_in_dir, ec)) {
      if (entry.is_regular_file(ec) && entry.path().extension() == ".conf") {
        files.push_back(entry.path());
      }
    }
    std::sort(files.begin(), files.end());

    for (const auto &file : files) {
      std::ifstream in(file);
      std::string line;
      bool in_service = false;
      while (std::getline(in, line)) {
        const auto text = trim_view(line);
        if (text.empty() || text.front() == '#' || text.front() == ';') {
          continue;
        }
        if (text.front() == '[') {
          in_service = text == "[Service]";
          continue;
        }
        if (!in_service) {
          continue;
        }
        const auto equals = text.find('=');
        if (equals == std::string_view::npos || trim_view(text.substr(0, equals)) != "ExecStart") {
          continue;
        }
        out.drop_in = file;
        out.exec_start = std::string {trim_view(text.substr(equals + 1))};
      }
    }

    if (out.exec_start.empty()) {
      return out;
    }
    // systemd allows prefix characters on the command: "-" ignores failure,
    // "@" renames argv[0], "+", "!" and "!!" change privileges, ":" disables
    // specifier expansion.
    std::string_view command = out.exec_start;
    while (!command.empty() && std::string_view {"-@+!:"}.find(command.front()) != std::string_view::npos) {
      command.remove_prefix(1);
    }
    const auto end = command.find_first_of(" \t");
    const auto first = command.substr(0, end);
    if (!first.empty() && first.front() == '/') {
      out.binary = std::filesystem::path {std::string {first}};
      out.binary_missing = !linux_util::is_executable_file(out.binary.string());
    }
    return out;
  }

  /// The path of the executable this process was started from.
  inline std::optional<std::filesystem::path> running_executable() {
    std::array<char, 4096> path {};
    const auto len = readlink("/proc/self/exe", path.data(), path.size() - 1);
    if (len <= 0) {
      return std::nullopt;
    }
    path[len] = '\0';
    return std::filesystem::path(path.data());
  }

  struct running_binary_t {
    std::string path;  ///< canonical path of the running executable
    std::string packaged_path;  ///< the absolute path the package installs, when the build declares one
    std::optional<bool> matches_package;  ///< nullopt when the packaged path is unknown or not installed
  };

  /**
   * @brief Whether the running executable is the one the package installed.
   * @param running The running executable, from running_executable().
   * @param packaged The build's POLARIS_EXECUTABLE_PATH; a relative value means a non-packaged build.
   */
  inline running_binary_t describe_running_binary(const std::filesystem::path &running, std::string_view packaged) {
    running_binary_t out;
    std::error_code ec;
    auto canonical_running = std::filesystem::canonical(running, ec);
    if (ec) {
      canonical_running = running;
    }
    out.path = canonical_running.string();
    if (packaged.empty() || packaged.front() != '/') {
      return out;
    }
    out.packaged_path = std::string {packaged};
    const auto canonical_packaged = std::filesystem::canonical(out.packaged_path, ec);
    if (ec) {
      // The package is not installed here, so there is nothing to compare against.
      return out;
    }
    out.matches_package = canonical_packaged == canonical_running;
    return out;
  }

  /// The one path the Bazzite guide ever wrote a runtime copy to, and so the only one --setup-host replaces.
  inline constexpr std::string_view guide_runtime_copy = "/usr/local/bin/polaris-kms";

  enum class runtime_copy_e {
    none,  ///< the service does not run the guide's copy, or that path is not a plain file
    current,  ///< the copy holds the same bytes as the binary it is refreshed from
    stale,  ///< the copy holds another build, so the service still runs that one
  };

  /// Whether two files hold the same bytes. A file that cannot be read matches nothing.
  inline bool same_contents(const std::filesystem::path &lhs, const std::filesystem::path &rhs) {
    std::error_code ec;
    const auto lhs_size = std::filesystem::file_size(lhs, ec);
    if (ec) {
      return false;
    }
    const auto rhs_size = std::filesystem::file_size(rhs, ec);
    if (ec || lhs_size != rhs_size) {
      return false;
    }
    std::ifstream left(lhs, std::ios::binary);
    std::ifstream right(rhs, std::ios::binary);
    if (!left || !right) {
      return false;
    }
    std::vector<char> left_block(1 << 16);
    std::vector<char> right_block(1 << 16);
    while (left && right) {
      left.read(left_block.data(), static_cast<std::streamsize>(left_block.size()));
      right.read(right_block.data(), static_cast<std::streamsize>(right_block.size()));
      if (left.gcount() != right.gcount() || !std::equal(left_block.begin(), left_block.begin() + left.gcount(), right_block.begin())) {
        return false;
      }
    }
    return left.eof() && right.eof();
  }

  /**
   * @brief Whether the service runs the guide's runtime copy, and whether that copy fell behind.
   *
   * Until 1.4.5 the Bazzite guide made this copy during every install, so hosts
   * set up then run it whether or not they use DRM/KMS capture. It lives
   * outside the deployment and no package update touches it: rpm reports the
   * new version while the service keeps running the build the copy was made
   * from. Only the guide's own path counts. The drop-in belongs to the account,
   * and --setup-host runs as root, so a path read from it is never one to write.
   *
   * @param packaged_exe The binary the copy is refreshed from.
   */
  inline runtime_copy_e guide_runtime_copy_state(const exec_override_t &override, const std::filesystem::path &packaged_exe, const std::filesystem::path &guide_copy = std::filesystem::path {guide_runtime_copy}) {
    if (!override.active() || override.binary != guide_copy) {
      return runtime_copy_e::none;
    }
    std::error_code ec;
    if (!std::filesystem::is_regular_file(std::filesystem::symlink_status(guide_copy, ec)) || ec) {
      return runtime_copy_e::none;
    }
    return same_contents(packaged_exe, guide_copy) ? runtime_copy_e::current : runtime_copy_e::stale;
  }

  /**
   * @brief What has to go for a host to stop having DRM/KMS capture.
   *
   * Three things each keep it alive on their own: the capability on the packaged binary, the guide's
   * copy of the binary, which carries a capability of its own, and the drop-in that points the user
   * service at that copy. The order matters in one direction only. A copy removed while its drop-in
   * survives leaves a service that cannot exec, which systemd reports as status=203/EXEC and nothing
   * else explains, so the drop-in goes first.
   *
   * The plan is separate from carrying it out because carrying it out needs root, a child process
   * and somebody's home directory, and this is the part worth testing.
   */
  struct kms_teardown_t {
    std::filesystem::path drop_in;  ///< the drop-in to remove because it points the service at the copy; empty when none does
    bool remove_guide_copy = false;  ///< the guide's copy is there to remove, and its capability leaves with it
    bool clear_binary_capability = false;  ///< the packaged binary carries cap_sys_admin

    bool empty() const {
      return drop_in.empty() && !remove_guide_copy && !clear_binary_capability;
    }
  };

  inline kms_teardown_t kms_teardown_plan(
    const exec_override_t &override,
    bool binary_holds_capability,
    bool guide_copy_exists,
    const std::filesystem::path &guide_copy = std::filesystem::path {guide_runtime_copy}
  ) {
    kms_teardown_t plan;
    plan.clear_binary_capability = binary_holds_capability;
    plan.remove_guide_copy = guide_copy_exists;
    // Only a drop-in that points at the copy is this feature's to remove. Someone who pointed the
    // service at a build tree of their own is not running the KMS recipe, and their drop-in stays.
    if (override.active() && override.binary == guide_copy) {
      plan.drop_in = override.drop_in;
    }
    return plan;
  }

  /**
   * @brief What --setup-host should say about the account's service override, or nothing.
   * @param packaged_exe The binary running --setup-host, which is the one a copy should be refreshed from.
   * @param guide_copy The copy this --setup-host refreshes by itself; empty when it is not the packaged binary and will not.
   */
  inline std::string setup_host_advice(const exec_override_t &override, std::string_view user, const std::filesystem::path &packaged_exe, const std::filesystem::path &guide_copy = std::filesystem::path {guide_runtime_copy}) {
    if (!override.active() || override.binary.empty()) {
      return {};
    }
    const auto drop_in = override.drop_in.string();
    const auto binary = override.binary.string();
    const auto account = std::string {user};
    if (override.binary_missing) {
      return "The polaris user service for [" + account + "] is overridden by " + drop_in + " to run " + binary +
             ", which does not exist, so the service cannot start (systemd reports status=203/EXEC).\n"
             "Either run the packaged binary again:\n"
             "  rm " + drop_in + "\n"
             "  systemctl --user daemon-reload\n"
             "  systemctl --user restart polaris\n"
             "or restore the copy it points at:\n"
             "  sudo install -D -m 0755 " + packaged_exe.string() + " " + binary + "\n"
             "  sudo setcap cap_sys_admin+ep " + binary + "\n"
             "  systemctl --user restart polaris\n";
    }
    std::error_code ec;
    const auto override_target = std::filesystem::canonical(override.binary, ec);
    if (ec) {
      return {};
    }
    const auto packaged = std::filesystem::canonical(packaged_exe, ec);
    if (ec || override_target == packaged) {
      return {};
    }
    if (override.binary == guide_copy) {
      return "The polaris user service for [" + account + "] runs " + binary + " through " + drop_in +
             ", a copy outside the package. Package updates do not change it: after every update, run\n"
             "  sudo -H polaris --setup-host\n"
             "which refreshes the copy and its DRM/KMS capability, or remove the drop-in to run the packaged binary again.\n";
    }
    return "The polaris user service for [" + account + "] runs " + binary + " through " + drop_in +
           ", a copy outside the package. Package updates do not change it: after every update, refresh the copy\n"
           "  sudo install -D -m 0755 " + packaged_exe.string() + " " + binary + "\n"
           "  sudo setcap cap_sys_admin+ep " + binary + "\n"
           "or remove the drop-in to run the packaged binary again.\n";
  }
}  // namespace platf::user_unit
