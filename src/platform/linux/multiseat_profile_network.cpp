#include "multiseat_profile_network.h"
#include <cctype>
#ifdef __linux__
#include <charconv>
#include <nlohmann/json.hpp>
#include <sstream>

namespace multiseat::container {
  namespace {
    using json = nlohmann::json;
    constexpr auto label = "io.polaris.multiseat.profile";
    bool digest(std::string_view value) {
      return value.size() == 64 && value.find_first_not_of("0123456789abcdef") == std::string_view::npos;
    }
    std::optional<std::string> run(host_t &host, std::initializer_list<std::string> arguments) {
      auto argv = command_prefix({});
      argv.insert(argv.end(), arguments.begin(), arguments.end());
      const auto result = host.run(argv, std::chrono::seconds(30), 4 * 1024 * 1024);
      if (result.exit_status != 0 || result.timed_out || result.output_truncated) return {};
      return result.output;
    }
  }

  bool valid_steam_target(std::string_view target) {
    if (target == "big-picture-v1") return true;
    if (target.empty() || target.size() > 10 || target.front() == '0') return false;
    std::uint32_t value = 0;
    const auto [end, error] = std::from_chars(target.data(), target.data() + target.size(), value);
    return error == std::errc {} && end == target.data() + target.size() && value != 0 &&
      std::to_string(value) == target;
  }
  namespace {
    /** `<runner>.<appName>`, the shape Heroic's own stores use. */
    bool valid_heroic_target(std::string_view target) {
      const auto dot = target.find('.');
      if (dot == std::string_view::npos) return false;
      const auto runner = target.substr(0, dot), name = target.substr(dot + 1);
      if (runner != "epic" && runner != "gog" && runner != "amazon" && runner != "sideload") return false;
      if (name.empty() || name.size() > 64) return false;
      if (!std::isalnum(static_cast<unsigned char>(name.front()))) return false;
      return name.find_first_not_of(
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") == std::string_view::npos;
    }

    /** `id.<decimal>`, which is how Lutris numbers a game in its own database. */
    bool valid_lutris_target(std::string_view target) {
      if (!target.starts_with("id.")) return false;
      return valid_steam_target(target.substr(3));
    }
  }  // namespace

  std::string_view launcher_sentinel(runtime_profile_e profile) {
    switch (profile) {
      case runtime_profile_e::gamescope: return "input-pong-v1";
      case runtime_profile_e::steam: return "big-picture-v1";
      case runtime_profile_e::heroic:
      case runtime_profile_e::lutris: return "library-v1";
      default: return {};
    }
  }

  bool needs_profile_network(runtime_profile_e profile) {
    return profile == runtime_profile_e::steam || profile == runtime_profile_e::heroic ||
      profile == runtime_profile_e::lutris;
  }

  bool valid_launcher_target(runtime_profile_e profile, std::string_view target) {
    if (!target.empty() && target == launcher_sentinel(profile)) return true;
    switch (profile) {
      case runtime_profile_e::steam: return valid_steam_target(target);
      case runtime_profile_e::heroic: return valid_heroic_target(target);
      case runtime_profile_e::lutris: return valid_lutris_target(target);
      default: return false;
    }
  }

  bool supported_streaming_workload(runtime_profile_e profile, const workload_plan_t &workload) {
    return workload_matches_runtime_profile(workload, profile) &&
      valid_launcher_target(profile, workload.target_id);
  }
  std::string profile_network_name(std::string_view profile_key) {
    if (profile_key.empty() || profile_key.size() > 128 ||
        profile_key.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string_view::npos)
      return {};
    return "pn-" + std::string(profile_key);
  }
  std::optional<std::string> profile_network_id(host_t &host, std::string_view profile_key, bool require_empty,
    std::string_view only_container) {
    const auto name = profile_network_name(profile_key);
    if (name.empty()) return {};
    const auto output = run(host, {"network", "inspect", name});
    if (!output) return {};
    try {
      const auto values = json::parse(*output);
      if (!values.is_array() || values.size() != 1) return {};
      const auto &network = values[0];
      const auto &options = network.at("Options");
      const auto &labels = network.at("Labels");
      const auto &ipam_options = network.at("IPAM").at("Options");
      const auto id = network.at("Id").get<std::string>();
      if (!digest(id) || network.at("Name") != name || network.at("Driver") != "bridge" ||
          network.at("Scope") != "local" || network.at("Internal") != false ||
          network.at("Attachable") != false || network.at("Ingress") != false || network.at("EnableIPv6") != false ||
          !labels.is_object() || labels.size() != 1 || labels.at(label) != profile_key ||
          options != json {{"com.docker.network.bridge.enable_icc", "false"},
                           {"com.docker.network.bridge.enable_ip_masquerade", "true"}} ||
          network.at("IPAM").at("Driver") != "default" ||
          !(ipam_options.is_null() || (ipam_options.is_object() && ipam_options.empty())) ||
          !network.at("Containers").is_object() || (require_empty && !network.at("Containers").empty())) return {};
      if (!only_container.empty() && (network.at("Containers").size() != 1 || !network.at("Containers").contains(only_container))) return {};
      return id;
    } catch (...) { return {}; }
  }
  bool create_profile_network(host_t &host, std::string_view profile_key) {
    const auto name = profile_network_name(profile_key);
    if (name.empty()) return false;
    const auto inventory = run(host, {"network", "ls", "--format={{json .Name}}"});
    if (!inventory) return false;
    try {
      std::istringstream lines(*inventory);
      std::string line;
      while (std::getline(lines, line)) {
        if (line.empty()) continue;
        const auto existing = json::parse(line);
        if (!existing.is_string() || existing == name) return false;
      }
    } catch (...) { return false; }
    auto created = run(host, {"network", "create", "--driver=bridge",
      "--label=" + std::string(label) + "=" + std::string(profile_key),
      "--opt=com.docker.network.bridge.enable_icc=false",
      "--opt=com.docker.network.bridge.enable_ip_masquerade=true", name});
    if (!created) return false;
    if (!created->empty() && created->back() == '\n') created->pop_back();
    if (!digest(*created)) return false;
    const auto inspected = profile_network_id(host, profile_key, true);
    return inspected && *inspected == *created;
  }
}
#endif
