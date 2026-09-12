/**
 * @file src/platform/linux/multiseat_profile_catalog.cpp
 * @brief Private profile catalog transactions and fresh Docker volume setup.
 */
#include "multiseat_profile_catalog.h"
#include "multiseat_profile_network.h"
#ifdef __linux__

#include "multiseat_container_host.h"
#include "src/utility.h"
#include "src/uuid.h"

#include <algorithm>
#include <iostream>
#include <nlohmann/json.hpp>
#include <set>
#include <stdexcept>
#include <unistd.h>

namespace multiseat::profiles {
  namespace {
    using json = nlohmann::json;
    using status_e = private_state_file::write_status_e;

    bool token(std::string_view value) {
      return !value.empty() && value.size() <= 128 &&
        std::all_of(value.begin(), value.end(), [](unsigned char c) {
          return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
            (c >= '0' && c <= '9') || c == '-' || c == '_';
        }) && value.front() != '-';
    }

    bool image_id(std::string_view value) {
      return value.starts_with("sha256:") && value.size() == 71 &&
        std::all_of(value.begin() + 7, value.end(), [](char c) {
          return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        });
    }

    std::string family(runtime_profile_e value) {
      switch (value) {
        case runtime_profile_e::gamescope: return "gamescope";
        case runtime_profile_e::steam: return "steam";
        case runtime_profile_e::heroic: return "heroic";
        case runtime_profile_e::lutris: return "lutris";
        default: throw std::invalid_argument("unknown profile family");
      }
    }

    std::pair<runtime_profile_e, workload_kind_e> family(std::string_view value) {
      if (value == "gamescope") return {runtime_profile_e::gamescope, workload_kind_e::gamescope};
      if (value == "steam") return {runtime_profile_e::steam, workload_kind_e::steam};
      if (value == "heroic") return {runtime_profile_e::heroic, workload_kind_e::heroic};
      if (value == "lutris") return {runtime_profile_e::lutris, workload_kind_e::lutris};
      throw std::invalid_argument("unknown profile family");
    }

    void keys(const json &object, std::initializer_list<const char *> expected) {
      if (!object.is_object() || object.size() != expected.size()) throw std::invalid_argument("catalog fields");
      for (const auto *key : expected) if (!object.contains(key)) throw std::invalid_argument("catalog field missing");
    }

    bool valid(const catalog_t &catalog) {
      if (catalog.owner_uid == 0 || catalog.owner_gid == 0 ||
          catalog.owner_uid > 2147483647 || catalog.owner_gid > 2147483647 ||
          catalog.profiles.size() > 4096) return false;
      std::set<std::string> profiles, volumes, clients;
      for (const auto &entry : catalog.profiles) {
        const auto &storage = entry.storage;
        if (!token(storage.profile_key) || !storage.opaque_volume_name.starts_with("pv-") ||
            !token(storage.opaque_volume_name) || !image_id(storage.image_reference) ||
            !profiles.emplace(storage.profile_key).second ||
            !volumes.emplace(storage.opaque_volume_name).second ||
            entry.name.empty() || entry.name.size() > 128 ||
            std::any_of(entry.name.begin(), entry.name.end(), [](unsigned char c) { return c < 32 || c == 127; }) ||
            entry.client_keys.size() > 4096 || !valid_workload_plan(entry.workload) ||
            !workload_matches_runtime_profile(entry.workload, storage.runtime_profile)) return false;
        for (const auto &client : entry.client_keys) {
          if (!token(client) || !clients.emplace(client).second || clients.size() > 65536) return false;
        }
      }
      return true;
    }

    template<class Edit>
    change_result_t change(const std::filesystem::path &path, Edit edit) {
      change_result_t result;
      try {
        result.status = private_state_file::update_atomic(path, maximum_catalog_bytes,
          [&](const auto &current) -> std::optional<std::string> {
            auto catalog = current ? decode(current.payload) : std::nullopt;
            if (!catalog) { result.error = "Initialize a valid private catalog first."; return std::nullopt; }
            return edit(*catalog, result);
          }).status;
      } catch (const std::exception &) {
        result.error = "Profile operation failed. Retain any reported provisioning resources for inspection.";
      }
      if (result.status == status_e::durability_uncertain) {
        result.error = "Catalog replacement occurred but durability is uncertain. Read it back before retrying; retain the volume.";
      } else if (!result && result.error.empty()) {
        result.error = "Catalog is busy, unsafe, or could not be saved. Stop its controller before editing.";
      }
      return result;
    }

    json docker(container::host_t &host, std::initializer_list<std::string> arguments,
                bool parse = true) {
      auto argv = container::command_prefix({});
      argv.insert(argv.end(), arguments.begin(), arguments.end());
      const auto result = host.run(argv, std::chrono::seconds(30), maximum_catalog_bytes);
      if (result.exit_status != 0 || result.timed_out || result.output_truncated) {
        throw std::runtime_error("Docker operation did not complete authoritatively");
      }
      return parse ? json::parse(result.output) : json(result.output);
    }

    void provision(container::host_t &host, const entry_t &entry, change_result_t &result) {
      // Current runtime images provide a real passwd entry only for 1000:1000.
      // Fail before creating storage on hosts needing a different image identity.
      if (host.effective_uid() != 1000 || host.effective_gid() != 1000 ||
          !host.trusted_runtime_file("/usr/bin/docker") ||
          !host.trusted_runtime_file("/usr/bin/runc")) {
        throw std::runtime_error("runtime identity or executable unavailable");
      }
      const auto info = docker(host, {"info", "--format={{json .}}"});
      if (info.at("OSType") != "linux" || !info.at("SecurityOptions").is_array() ||
          (info.at("Runtimes").at("runc").at("path") != "runc" &&
           info.at("Runtimes").at("runc").at("path") != "/usr/bin/runc")) {
        throw std::runtime_error("local Linux runc engine required");
      }
      for (const auto &option : info.at("SecurityOptions")) {
        if (!option.is_string() || option.get<std::string>().starts_with("name=rootless")) {
          throw std::runtime_error("rootless Docker is not admitted");
        }
      }
      const auto &volume = entry.storage.opaque_volume_name;
      const auto inventory = docker(host, {"volume", "ls", "--format={{json .Name}}"}, false).get<std::string>();
      // Successful bounded inventory is required to prove absence. A failed
      // inspect is not evidence that a name is available.
      if (inventory.find(volume) != std::string::npos) throw std::runtime_error("volume already exists");
      const auto images = docker(host, {"image", "inspect", entry.storage.image_reference});
      if (!images.is_array() || images.size() != 1 || images[0].at("Id") != entry.storage.image_reference ||
          images[0].at("Os") != "linux" ||
          images[0].at("Config").at("Labels").at("io.polaris.multiseat.profile") != family(entry.storage.runtime_profile) ||
          (images[0].at("Config").contains("Volumes") && !images[0].at("Config").at("Volumes").empty()) ||
          (images[0].at("Config").contains("ExposedPorts") && !images[0].at("Config").at("ExposedPorts").empty())) {
        throw std::runtime_error("image identity, implicit volumes, or exposed ports rejected");
      }
      result.volume_name = volume;
      result.initializer_name = "polaris-profile-init-" + entry.storage.profile_key;
      const auto label = "io.polaris.multiseat.profile=" + entry.storage.profile_key;
      docker(host, {"volume", "create", "--driver=local", "--label=" + label, volume}, false);
      const auto inspect_volume = [&] {
        const auto values = docker(host, {"volume", "inspect", volume});
        if (!values.is_array() || values.size() != 1) throw std::runtime_error("volume inspection count");
        const auto &value = values[0];
        if (value.at("Name") != volume || value.at("Driver") != "local" ||
            value.at("Scope") != "local" || !value.at("Options").empty() ||
            value.at("Labels").at("io.polaris.multiseat.profile") != entry.storage.profile_key) {
          throw std::runtime_error("fresh volume identity rejected");
        }
      };
      inspect_volume();
      // Fixed code, no shell, no recursion, and no existing home adoption. The
      // directory must still be empty and newly owned by root before changing it.
      const std::string initialize_code =
        "import os,pwd,stat\n"
        "assert pwd.getpwuid(1000).pw_gid == 1000\n"
        "fd=os.open('/profile',os.O_RDONLY|os.O_DIRECTORY|os.O_NOFOLLOW)\n"
        "s=os.fstat(fd)\n"
        "assert s.st_uid == 0 and s.st_gid == 0 and not os.listdir(fd)\n"
        "os.fchown(fd,1000,1000)\n"
        "os.fchmod(fd,0o700)\n"
        "os.fsync(fd)\n"
        "s=os.fstat(fd)\n"
        "assert (s.st_uid,s.st_gid,stat.S_IMODE(s.st_mode)) == (1000,1000,0o700)\n";
      docker(host, {"run", "--rm", "--name=" + result.initializer_name,
        "--label=" + label, "--pull=never", "--runtime=runc", "--network=none",
        "--userns=host", "--read-only", "--user=0:0", "--cap-drop=ALL",
        "--cap-add=CHOWN", "--cap-add=FOWNER", "--security-opt=no-new-privileges",
        "--pids-limit=32", "--memory=128m", "--cpus=1", "--no-healthcheck",
        "--mount=type=volume,src=" + volume + ",dst=/profile,volume-nocopy",
        "--entrypoint=/usr/bin/python3", entry.storage.image_reference, "-I", "-c", initialize_code}, false);
      inspect_volume();
      if (entry.storage.runtime_profile == runtime_profile_e::steam) {
        result.network_name = container::profile_network_name(entry.storage.profile_key);
        if (!container::create_profile_network(host, entry.storage.profile_key))
          throw std::runtime_error("Steam profile network could not be provisioned authoritatively");
      }
    }
  }  // namespace

  std::optional<catalog_t> decode(std::string_view payload) {
    if (payload.empty() || payload.size() > maximum_catalog_bytes) return std::nullopt;
    try {
      std::vector<std::set<std::string>> object_keys;
      const auto root = json::parse(payload, [&](int depth, json::parse_event_t event, json &value) {
        if (depth > 12) throw std::invalid_argument("catalog nesting");
        if (event == json::parse_event_t::object_start) object_keys.emplace_back();
        if (event == json::parse_event_t::key &&
            !object_keys.back().emplace(value.get<std::string>()).second) throw std::invalid_argument("duplicate catalog key");
        if (event == json::parse_event_t::object_end) object_keys.pop_back();
        return true;
      });
      keys(root, {"schema", "owner_uid", "owner_gid", "profiles"});
      if (!root.at("schema").is_number_unsigned() || root.at("schema") != 1 ||
          !root.at("owner_uid").is_number_unsigned() || !root.at("owner_gid").is_number_unsigned() ||
          root.at("owner_uid").get<std::uint64_t>() > 2147483647 ||
          root.at("owner_gid").get<std::uint64_t>() > 2147483647 ||
          !root.at("profiles").is_array() || root.at("profiles").size() > 4096) return std::nullopt;
      catalog_t catalog {root.at("owner_uid").get<std::uint32_t>(), root.at("owner_gid").get<std::uint32_t>(), {}};
      for (const auto &value : root.at("profiles")) {
        keys(value, {"id", "name", "volume", "family", "image", "target", "clients"});
        const auto [runtime, kind] = family(value.at("family").get<std::string>());
        if (!value.at("clients").is_array() || value.at("clients").size() > 4096) return std::nullopt;
        catalog.profiles.push_back({
          .storage = {value.at("id").get<std::string>(), value.at("volume").get<std::string>(),
                      runtime, value.at("image").get<std::string>()},
          .name = value.at("name").get<std::string>(),
          .workload = {kind, value.at("target").get<std::string>()},
          .client_keys = value.at("clients").get<std::vector<std::string>>(),
        });
      }
      return valid(catalog) ? std::optional {std::move(catalog)} : std::nullopt;
    } catch (const std::exception &) { return std::nullopt; }
  }

  std::string encode(const catalog_t &catalog) {
    if (!valid(catalog)) throw std::invalid_argument("invalid profile catalog");
    json root {{"schema", 1}, {"owner_uid", catalog.owner_uid}, {"owner_gid", catalog.owner_gid}, {"profiles", json::array()}};
    for (const auto &entry : catalog.profiles) {
      root["profiles"].push_back({{"id", entry.storage.profile_key}, {"name", entry.name},
        {"volume", entry.storage.opaque_volume_name}, {"family", family(entry.storage.runtime_profile)},
        {"image", entry.storage.image_reference}, {"target", entry.workload.target_id}, {"clients", entry.client_keys}});
    }
    auto payload = root.dump(2) + "\n";
    if (payload.size() > maximum_catalog_bytes) throw std::invalid_argument("catalog too large");
    return payload;
  }

  std::optional<loaded_catalog_t> load(const std::filesystem::path &path) {
    auto read = private_state_file::read_with_lease(path, maximum_catalog_bytes);
    if (!read.read) return std::nullopt;
    auto catalog = decode(read.read.payload);
    if (!catalog) return std::nullopt;
    return loaded_catalog_t {std::move(*catalog), std::move(read.lease)};
  }

  change_result_t initialize(const std::filesystem::path &path, std::uint32_t uid, std::uint32_t gid) {
    change_result_t result;
    try {
      const auto payload = encode({uid, gid, {}});
      result.status = private_state_file::update_atomic(path, maximum_catalog_bytes,
        [&](const auto &current) -> std::optional<std::string> {
          return current.status == private_state_file::read_status_e::missing ? std::optional {payload} : std::nullopt;
        }).status;
    } catch (const std::exception &) {}
    if (!result) result.error = result.status == status_e::durability_uncertain ?
      "Catalog replacement occurred but durability is uncertain. Read it back before retrying." :
      "Catalog already exists, is busy, or cannot be safely initialized.";
    return result;
  }

  change_result_t assign(const std::filesystem::path &path, std::string_view profile_key, std::string_view client_key) {
    return change(path, [&](auto &catalog, auto &result) -> std::optional<std::string> {
      if (!token(client_key)) { result.error = "Invalid paired device identifier."; return std::nullopt; }
      auto target = std::find_if(catalog.profiles.begin(), catalog.profiles.end(), [&](const auto &entry) {
        return entry.storage.profile_key == profile_key;
      });
      if (target == catalog.profiles.end()) { result.error = "Unknown profile."; return std::nullopt; }
      for (const auto &entry : catalog.profiles) {
        if (std::find(entry.client_keys.begin(), entry.client_keys.end(), client_key) != entry.client_keys.end()) {
          if (&entry == &*target) return encode(catalog);
          result.error = "Device already belongs to another profile. Unassign it first.";
          return std::nullopt;
        }
      }
      target->client_keys.emplace_back(client_key);
      return encode(catalog);
    });
  }

  change_result_t unassign(const std::filesystem::path &path, std::string_view client_key) {
    return change(path, [&](auto &catalog, auto &result) -> std::optional<std::string> {
      if (!token(client_key)) { result.error = "Invalid paired device identifier."; return std::nullopt; }
      for (auto &entry : catalog.profiles) std::erase(entry.client_keys, client_key);
      return encode(catalog);
    });
  }

  change_result_t set_assignment(const std::filesystem::path &path,
                               std::string_view profile_key, std::string_view client_key) {
    return change(path, [&](auto &catalog, auto &result) -> std::optional<std::string> {
      if (!token(client_key)) { result.error = "Invalid paired device identifier."; return std::nullopt; }
      auto target = std::find_if(catalog.profiles.begin(), catalog.profiles.end(), [&](const auto &entry) {
        return entry.storage.profile_key == profile_key;
      });
      if (!profile_key.empty() && target == catalog.profiles.end()) {
        result.error = "Unknown profile."; return std::nullopt;
      }
      for (auto &entry : catalog.profiles) std::erase(entry.client_keys, client_key);
      if (target != catalog.profiles.end()) target->client_keys.emplace_back(client_key);
      return encode(catalog);
    });
  }

  change_result_t create(const std::filesystem::path &path, std::string_view name,
                         std::string_view image, container::host_t &host, const workload_plan_t &workload) {
    return change(path, [&](auto &catalog, auto &result) -> std::optional<std::string> {
      const auto runtime_profile = workload.kind == workload_kind_e::gamescope ? runtime_profile_e::gamescope :
        workload.kind == workload_kind_e::steam ? runtime_profile_e::steam : runtime_profile_e::unknown;
      if (!container::supported_streaming_workload(runtime_profile, workload)) {
        result.error = "Unsupported profile workload. Steam requires Big Picture or a canonical positive game ID.";
        return std::nullopt;
      }
      if (catalog.owner_uid != host.effective_uid() || catalog.owner_gid != host.effective_gid()) {
        result.error = "Catalog owner does not match the runtime user."; return std::nullopt;
      }
      if (host.effective_uid() != 1000 || host.effective_gid() != 1000) {
        result.error = "Current runtime images require UID and GID 1000. A matching user identity image is needed on this host.";
        return std::nullopt;
      }
      entry_t entry {
        .storage = {uuid_util::uuid_t::generate().string(), {}, runtime_profile, std::string(image)},
        .name = std::string(name), .workload = workload, .client_keys = {},
      };
      entry.storage.opaque_volume_name = "pv-" + entry.storage.profile_key;
      catalog.profiles.push_back(entry);
      const auto payload = encode(catalog);  // Complete validation before Docker mutations.
      result.profile_key = entry.storage.profile_key;
      provision(host, entry, result);
      return payload;
    });
  }

  int command(int argc, char **argv) {
    if (argc < 2) {
      std::cerr << "Usage: polaris --multiseat-profiles init|list CATALOG\n"
                   "       polaris --multiseat-profiles create CATALOG NAME LOCAL_IMAGE_SHA256\n"
                   "       polaris --multiseat-profiles create-steam CATALOG NAME LOCAL_IMAGE_SHA256 [GAME_ID]\n"
                   "       polaris --multiseat-profiles assign CATALOG PROFILE_ID PAIRED_DEVICE_ID\n"
                   "       polaris --multiseat-profiles unassign CATALOG PAIRED_DEVICE_ID\n";
      return 2;
    }
    const std::string_view action = argv[0];
    const std::filesystem::path path = argv[1];
    if (!path.is_absolute() || path.lexically_normal() != path || ::geteuid() == 0) {
      std::cerr << "Use an absolute catalog path as the ordinary Polaris service user.\n";
      return 2;
    }
    change_result_t result;
    if (action == "init" && argc == 2) result = initialize(path, ::geteuid(), ::getegid());
    else if (action == "list" && argc == 2) {
      auto loaded = load(path);
      if (!loaded) { std::cerr << "Catalog is missing, invalid, unsafe, or in use.\n"; return 1; }
      std::cout << encode(loaded->catalog);
      return 0;
    } else if (action == "create" && argc == 4) {
      container::local_host_t host;
      result = create(path, argv[2], argv[3], host);
    } else if (action == "create-steam" && (argc == 4 || argc == 5)) {
      container::local_host_t host;
      result = create(path, argv[2], argv[3], host, {workload_kind_e::steam, argc == 5 ? argv[4] : "big-picture-v1"});
    } else if (action == "assign" && argc == 4) result = assign(path, argv[2], argv[3]);
    else if (action == "unassign" && argc == 3) result = unassign(path, argv[2]);
    else { std::cerr << "Unknown profile operation or argument count.\n"; return 2; }
    if (!result) std::cerr << result.error << '\n';
    if (!result.volume_name.empty()) {
      std::cout << "profile=" << result.profile_key << " volume=" << result.volume_name << '\n';
      if (!result) std::cerr << "Retained volume=" << result.volume_name
        << "; initializer may remain: " << result.initializer_name << '\n';
    }
    if (!result.network_name.empty()) std::cout << "network=" << result.network_name << '\n';
    return result ? 0 : 1;
  }
}  // namespace multiseat::profiles
#endif
