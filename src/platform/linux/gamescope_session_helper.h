/**
 * @file src/platform/linux/gamescope_session_helper.h
 * @brief Locate the polaris-gamescope-session launcher and check it against the copy this build ships.
 */
#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace platf::gamescope_session_helper {

  enum class bundle_match_t {
    unknown,  ///< nothing to compare against: no bundled copy, or no installed file
    exact,  ///< the installed file is byte-identical to the bundled module
    wrapped,  ///< the installed file embeds the bundled module verbatim (Nix, manual installer)
    mismatch,  ///< the installed file came from a different checkout
  };

  struct resolution_t {
    std::filesystem::path helper;  ///< launcher to run; empty when none is installed
    std::filesystem::path shadowed;  ///< a different copy PATH would have picked; empty when none
    std::filesystem::path bundled;  ///< module copy shipped with this build; empty when absent
    std::filesystem::path runtime_lib;  ///< sibling runtime library that was compared; empty when absent
    bundle_match_t session_match {bundle_match_t::unknown};
    bundle_match_t runtime_lib_match {bundle_match_t::unknown};
  };

  /**
   * @brief Pick the launcher for this Polaris binary.
   *
   * The copy beside the running executable wins over PATH. A distribution
   * package ships the binary and the launcher together, so they cannot drift
   * apart, while a copy under ~/.local/bin or /usr/local/bin left by an earlier
   * scripts/install run is never updated by the package manager and would
   * otherwise shadow the packaged one.
   *
   * @param executable_dir Directory of the running binary, when known.
   * @param path_candidate What a PATH lookup found, or empty.
   * @param bundled_session The module copy this build ships, or empty.
   * @param bundled_runtime_lib The runtime library copy this build ships, or empty.
   */
  resolution_t resolve(
    const std::optional<std::filesystem::path> &executable_dir,
    const std::filesystem::path &path_candidate,
    const std::filesystem::path &bundled_session,
    const std::filesystem::path &bundled_runtime_lib
  );

  /// Resolve using /proc/self/exe, PATH, and the bundled assets of this build.
  resolution_t resolve_default();

  bundle_match_t compare_with_bundle(std::string_view installed, std::string_view bundled);

  std::string_view describe(bundle_match_t match);

  /// One line naming the launcher in use and how it relates to the shipped module.
  std::string summary(const resolution_t &resolution);

  /// Warnings worth a log line: a shadowed copy, or a launcher from another checkout.
  std::vector<std::string> advisories(const resolution_t &resolution);

}  // namespace platf::gamescope_session_helper
