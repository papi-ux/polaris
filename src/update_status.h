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
#endif

}  // namespace update_status
