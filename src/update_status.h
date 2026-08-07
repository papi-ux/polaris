/**
 * @file src/update_status.h
 * @brief Host update awareness helpers for the web Update Center.
 */
#pragma once

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace update_status {

  struct distro_info_t {
    std::string id;
    std::string id_like;
    std::string version_id;
    std::string pretty_name;
  };

  distro_info_t parse_os_release(std::string_view content);
  distro_info_t detect_host_distro();
  std::string package_family(const distro_info_t &distro);
  std::string recommended_release_asset(const distro_info_t &distro);

  /**
   * @brief Whether an ini file enables a repository section named `polaris`.
   *
   * Answers the question the Update Center actually needs: can this host take
   * an upgrade from its package manager? That is a property of the repository
   * being configured, not of where the installed package originally came from
   * -- a package installed by hand upgrades from the repository just fine once
   * the repository exists.
   */
  bool repository_section_enabled(std::string_view ini_contents);
  bool host_repository_configured(const distro_info_t &distro);
  std::string repository_upgrade_command(const distro_info_t &distro, bool ostree_host);
  nlohmann::json distro_json(const distro_info_t &distro);
  nlohmann::json host_update_status();

#ifdef POLARIS_TESTS
  inline distro_info_t parse_os_release_for_tests(std::string_view content) {
    return parse_os_release(content);
  }

  inline std::string package_family_for_tests(const distro_info_t &distro) {
    return package_family(distro);
  }

  inline std::string recommended_release_asset_for_tests(const distro_info_t &distro) {
    return recommended_release_asset(distro);
  }

  inline bool repository_section_enabled_for_tests(std::string_view ini_contents) {
    return repository_section_enabled(ini_contents);
  }

  inline std::string repository_upgrade_command_for_tests(const distro_info_t &distro, bool ostree_host) {
    return repository_upgrade_command(distro, ostree_host);
  }
#endif

}  // namespace update_status
