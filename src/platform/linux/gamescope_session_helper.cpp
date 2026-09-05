/**
 * @file src/platform/linux/gamescope_session_helper.cpp
 * @brief Locate the polaris-gamescope-session launcher and check it against the copy this build ships.
 */

#include "gamescope_session_helper.h"

#ifdef __linux__

#include <array>
#include <fstream>
#include <iterator>
#include <system_error>
#include <unistd.h>

#include "executable_path.h"

namespace platf::gamescope_session_helper {

  namespace {
    namespace fs = std::filesystem;

    constexpr std::string_view launcher_name = "polaris-gamescope-session";
    constexpr std::string_view runtime_lib_name = "polaris-gamescope-runtime-lib.sh";

    std::string read_text(const fs::path &path) {
      std::ifstream input(path, std::ios::binary);
      if (!input) {
        return {};
      }
      return std::string(std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>());
    }

    bool same_file(const fs::path &a, const fs::path &b) {
      std::error_code ec;
      const bool equivalent = fs::equivalent(a, b, ec);
      return !ec && equivalent;
    }

    bool regular_file(const fs::path &path) {
      std::error_code ec;
      return !path.empty() && fs::is_regular_file(path, ec) && !ec;
    }

    std::optional<fs::path> running_executable() {
      std::array<char, 4096> buffer {};
      const auto len = readlink("/proc/self/exe", buffer.data(), buffer.size() - 1);
      if (len <= 0) {
        return std::nullopt;
      }
      buffer[len] = '\0';
      return fs::path(buffer.data());
    }

    /**
     * Mirror the host-asset lookup: the installed assets directory first, then
     * the assets copied beside a build-directory binary.
     */
    fs::path bundled_asset(const std::optional<fs::path> &executable_dir, std::string_view name) {
      const auto relative = fs::path("gamescope") / name;
      const auto installed = fs::path(POLARIS_ASSETS_DIR) / relative;
      if (regular_file(installed)) {
        return installed;
      }
      if (executable_dir) {
        const auto local_build = *executable_dir / "assets" / relative;
        if (regular_file(local_build)) {
          return local_build;
        }
      }
      return {};
    }
  }  // namespace

  bundle_match_t compare_with_bundle(std::string_view installed, std::string_view bundled) {
    if (installed.empty() || bundled.empty()) {
      return bundle_match_t::unknown;
    }
    if (installed == bundled) {
      return bundle_match_t::exact;
    }
    if (installed.find(bundled) != std::string_view::npos) {
      return bundle_match_t::wrapped;
    }
    return bundle_match_t::mismatch;
  }

  std::string_view describe(bundle_match_t match) {
    switch (match) {
      case bundle_match_t::exact:
        return "identical to the module this build ships";
      case bundle_match_t::wrapped:
        return "wraps the module this build ships";
      case bundle_match_t::mismatch:
        return "installed from a different checkout";
      case bundle_match_t::unknown:
        break;
    }
    return "not compared against a bundled copy";
  }

  std::string_view match_key(bundle_match_t match) {
    switch (match) {
      case bundle_match_t::exact:
        return "exact";
      case bundle_match_t::wrapped:
        return "wrapped";
      case bundle_match_t::mismatch:
        return "mismatch";
      case bundle_match_t::unknown:
        break;
    }
    return "unknown";
  }

  resolution_t resolve(
    const std::optional<fs::path> &executable_dir,
    const fs::path &path_candidate,
    const fs::path &bundled_session,
    const fs::path &bundled_runtime_lib
  ) {
    resolution_t resolution;

    if (executable_dir) {
      const auto beside = *executable_dir / launcher_name;
      if (linux_util::is_executable_file(beside.string())) {
        resolution.helper = beside;
      }
    }

    if (!path_candidate.empty() && linux_util::is_executable_file(path_candidate.string())) {
      if (resolution.helper.empty()) {
        resolution.helper = path_candidate;
      } else if (!same_file(resolution.helper, path_candidate)) {
        resolution.shadowed = path_candidate;
      }
    }

    if (resolution.helper.empty() || !regular_file(bundled_session)) {
      return resolution;
    }

    resolution.bundled = bundled_session;
    resolution.session_match = compare_with_bundle(read_text(resolution.helper), read_text(bundled_session));

    // The launcher sources its runtime library from its own directory. Nix
    // inlines the library into the wrapper instead, so a missing sibling is
    // not a finding.
    const auto sibling_lib = resolution.helper.parent_path() / runtime_lib_name;
    if (regular_file(sibling_lib) && regular_file(bundled_runtime_lib)) {
      resolution.runtime_lib = sibling_lib;
      resolution.runtime_lib_match = compare_with_bundle(read_text(sibling_lib), read_text(bundled_runtime_lib));
    }

    return resolution;
  }

  resolution_t resolve_default() {
    std::optional<fs::path> executable_dir;
    if (const auto exe = running_executable()) {
      executable_dir = exe->parent_path();
    }
    const fs::path path_candidate = linux_util::find_executable_in_path(launcher_name);
    return resolve(
      executable_dir,
      path_candidate,
      bundled_asset(executable_dir, "polaris-gamescope-session.sh"),
      bundled_asset(executable_dir, runtime_lib_name)
    );
  }

  std::string summary(const resolution_t &resolution) {
    if (resolution.helper.empty()) {
      return "gamescope_stream: no polaris-gamescope-session launcher beside this binary or on PATH";
    }
    std::string line = "gamescope_stream: polaris-gamescope-session [" + resolution.helper.string() + "] is " +
                       std::string(describe(resolution.session_match));
    if (!resolution.runtime_lib.empty()) {
      line += "; runtime library [" + resolution.runtime_lib.string() + "] is " +
              std::string(describe(resolution.runtime_lib_match));
    }
    return line;
  }

  std::vector<std::string> advisories(const resolution_t &resolution) {
    std::vector<std::string> lines;
    if (!resolution.shadowed.empty()) {
      lines.push_back(
        "gamescope_stream: using polaris-gamescope-session shipped beside this binary [" +
        resolution.helper.string() + "]; PATH would have picked [" + resolution.shadowed.string() +
        "], a separate install that package updates never touch. Remove it if it came from an older scripts/install run."
      );
    }
    if (resolution.session_match == bundle_match_t::mismatch) {
      lines.push_back(
        "gamescope_stream: polaris-gamescope-session [" + resolution.helper.string() +
        "] does not match the launcher this Polaris build ships [" + resolution.bundled.string() +
        "]; it was installed from a different checkout. Reinstall the gamescope helpers from this Polaris version before reporting gamescope_stream problems."
      );
    }
    if (resolution.runtime_lib_match == bundle_match_t::mismatch) {
      lines.push_back(
        "gamescope_stream: polaris-gamescope-runtime-lib.sh [" + resolution.runtime_lib.string() +
        "] does not match the library this Polaris build ships; the launcher beside it sources it. Reinstall both helpers together."
      );
    }
    return lines;
  }

}  // namespace platf::gamescope_session_helper

#endif  // __linux__
