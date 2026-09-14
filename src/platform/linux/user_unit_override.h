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

  /**
   * @brief What --setup-host should say about the account's service override, or nothing.
   * @param packaged_exe The binary running --setup-host, which is the one a copy should be refreshed from.
   */
  inline std::string setup_host_advice(const exec_override_t &override, std::string_view user, const std::filesystem::path &packaged_exe) {
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
    return "The polaris user service for [" + account + "] runs " + binary + " through " + drop_in +
           ", a copy outside the package. Package updates do not change it: after every update, refresh the copy\n"
           "  sudo install -D -m 0755 " + packaged_exe.string() + " " + binary + "\n"
           "  sudo setcap cap_sys_admin+ep " + binary + "\n"
           "or remove the drop-in to run the packaged binary again.\n";
  }
}  // namespace platf::user_unit
