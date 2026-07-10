/**
 * @file src/update_status.cpp
 * @brief Host update awareness helpers for the web Update Center.
 */
#include "update_status.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace update_status {
  namespace {
    std::string trim(std::string value) {
      const auto not_space = [](unsigned char ch) {
        return !std::isspace(ch);
      };
      value.erase(value.begin(), std::find_if(value.begin(), value.end(), not_space));
      value.erase(std::find_if(value.rbegin(), value.rend(), not_space).base(), value.end());
      return value;
    }

    std::string unquote(std::string value) {
      value = trim(std::move(value));
      if (value.size() >= 2 &&
          ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\''))) {
        value = value.substr(1, value.size() - 2);
      }
      return value;
    }

    std::string lower_copy(std::string value) {
      std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
      });
      return value;
    }

    std::vector<std::string> distro_tokens(const distro_info_t &distro) {
      std::vector<std::string> tokens;
      if (!distro.id.empty()) {
        tokens.push_back(lower_copy(distro.id));
      }

      std::istringstream input(distro.id_like);
      std::string token;
      while (input >> token) {
        tokens.push_back(lower_copy(token));
      }
      return tokens;
    }

    bool has_token(const std::vector<std::string> &tokens, std::string_view value) {
      return std::find(tokens.begin(), tokens.end(), value) != tokens.end();
    }
  }  // namespace

  distro_info_t parse_os_release(std::string_view content) {
    distro_info_t distro;
    std::istringstream input {std::string(content)};
    std::string line;

    while (std::getline(input, line)) {
      line = trim(std::move(line));
      if (line.empty() || line.front() == '#') {
        continue;
      }

      const auto sep = line.find('=');
      if (sep == std::string::npos) {
        continue;
      }

      auto key = lower_copy(trim(line.substr(0, sep)));
      auto value = unquote(line.substr(sep + 1));

      if (key == "id") {
        distro.id = lower_copy(std::move(value));
      } else if (key == "id_like") {
        distro.id_like = lower_copy(std::move(value));
      } else if (key == "version_id") {
        distro.version_id = std::move(value);
      } else if (key == "pretty_name") {
        distro.pretty_name = std::move(value);
      } else if (key == "name" && distro.pretty_name.empty()) {
        distro.pretty_name = std::move(value);
      }
    }

    return distro;
  }

  distro_info_t detect_host_distro() {
#ifdef __linux__
    std::ifstream os_release {"/etc/os-release"};
    if (!os_release) {
      return {};
    }

    std::ostringstream buffer;
    buffer << os_release.rdbuf();
    return parse_os_release(buffer.str());
#else
    return {};
#endif
  }

  std::string package_family(const distro_info_t &distro) {
    const auto tokens = distro_tokens(distro);
    if (has_token(tokens, "arch") || has_token(tokens, "cachyos") ||
        has_token(tokens, "manjaro") || has_token(tokens, "endeavouros")) {
      return "arch";
    }

    if (has_token(tokens, "fedora") || has_token(tokens, "bazzite") ||
        has_token(tokens, "rhel") || has_token(tokens, "centos")) {
      return "fedora";
    }

    if (has_token(tokens, "ubuntu")) {
      return "ubuntu";
    }

    return {};
  }

  std::string recommended_release_asset(const distro_info_t &distro) {
    const auto family = package_family(distro);
    if (family == "arch") {
      return "Polaris-arch-x86_64.pkg.tar.zst";
    }

    if (family == "fedora") {
      if (distro.version_id.empty()) {
        return {};
      }
      return "Polaris-fedora" + distro.version_id + "-x86_64.rpm";
    }

    if (family == "ubuntu" && distro.version_id.rfind("24.04", 0) == 0) {
      return "Polaris-ubuntu24.04-x86_64.deb";
    }

    return {};
  }

  nlohmann::json distro_json(const distro_info_t &distro) {
    return {
      {"id", distro.id},
      {"id_like", distro.id_like},
      {"version_id", distro.version_id},
      {"pretty_name", distro.pretty_name},
    };
  }

  nlohmann::json host_update_status() {
    const auto distro = detect_host_distro();
    return {
      {"status", true},
      {"platform", POLARIS_PLATFORM},
      {"version", PROJECT_VERSION},
      {"manual_install_only", true},
      {"auto_install_supported", false},
      {"auto_install_enabled", false},
      {"distro", distro_json(distro)},
      {"package_family", package_family(distro)},
      {"recommended_asset_name", recommended_release_asset(distro)},
    };
  }

}  // namespace update_status
