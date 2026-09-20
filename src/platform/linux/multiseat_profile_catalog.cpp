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
#include <sstream>
#include <stdexcept>
#include <thread>
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

    bool request_uuid(std::string_view request_id) {
      if (request_id.size() != 36) return false;
      for (std::size_t i = 0; i < request_id.size(); ++i) {
        const char c = request_id[i];
        if (i == 8 || i == 13 || i == 18 || i == 23) { if (c != '-') return false; }
        else if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
      }
      return true;
    }

    bool valid_new_steam(std::string_view request_id, std::string_view name) {
      if (name.empty() || name.size() > 128 ||
          name.front() == ' ' || name.back() == ' ' ||
          std::any_of(name.begin(), name.end(), [](unsigned char c) { return c < 32 || c == 127; })) return false;
      return request_uuid(request_id);
    }

    /**
     * A launcher family by name, without throwing: a family this build does not
     * carry reads as unknown and refuses, rather than aborting a transaction.
     */
    runtime_profile_e launcher_family(std::string_view value) {
      if (value == "steam") return runtime_profile_e::steam;
      if (value == "heroic") return runtime_profile_e::heroic;
      if (value == "lutris") return runtime_profile_e::lutris;
      return runtime_profile_e::unknown;
    }

    /**
     * The workload a Space opens when it is made: its launcher, never a title.
     * The target comes from the one grammar that defines a family's sentinel.
     */
    workload_plan_t launcher_workload(runtime_profile_e profile) {
      switch (profile) {
        case runtime_profile_e::steam: return {workload_kind_e::steam, std::string(container::launcher_sentinel(profile))};
        case runtime_profile_e::heroic: return {workload_kind_e::heroic, std::string(container::launcher_sentinel(profile))};
        case runtime_profile_e::lutris: return {workload_kind_e::lutris, std::string(container::launcher_sentinel(profile))};
        default: return {};
      }
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
      std::set<std::string> profiles, volumes, clients, desktops;
      if (catalog.desktop_clients.size() > 4096) return false;
      for (const auto &client : catalog.desktop_clients) if (!token(client) || !desktops.insert(client).second) return false;
      std::set<std::string> desktop_defaults;
      if (catalog.desktop_default_clients.size() > 4096) return false;
      for (const auto &client : catalog.desktop_default_clients)
        if (!token(client) || !desktop_defaults.insert(client).second) return false;
      std::size_t grants = 0;
      for (const auto &entry : catalog.profiles) {
        const auto &storage = entry.storage;
        if (!token(storage.profile_key) || storage.profile_key == "desktop" || !storage.opaque_volume_name.starts_with("pv-") ||
            !token(storage.opaque_volume_name) || !image_id(storage.image_reference) ||
            !profiles.emplace(storage.profile_key).second ||
            !volumes.emplace(storage.opaque_volume_name).second ||
            entry.name.empty() || entry.name.size() > 128 ||
            std::any_of(entry.name.begin(), entry.name.end(), [](unsigned char c) { return c < 32 || c == 127; }) ||
            (entry.archived && (!entry.client_keys.empty() || !entry.access_clients.empty())) ||
            entry.access_clients.size() > 4096 || entry.client_keys.size() > 4096 || !valid_workload_plan(entry.workload) ||
            !workload_matches_runtime_profile(entry.workload, storage.runtime_profile)) return false;
        std::set<std::string> access;
        for (const auto &client : entry.access_clients)
          if (!token(client) || !access.insert(client).second || ++grants > 65536) return false;
        for (const auto &client : entry.client_keys) {
          if (!token(client) || !clients.emplace(client).second || clients.size() > 65536) return false;
        }
      }
      // One Default Space per device: a Space or Desktop, never both.
      return std::none_of(desktop_defaults.begin(), desktop_defaults.end(), [&](const auto &client) { return clients.contains(client); });
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

    // Removal reads Docker's answers without throwing: every unanswered call is
    // a refusal or a kept resource, never a guess that something is gone.
    std::optional<std::string> docker_output(container::host_t &host, std::initializer_list<std::string> arguments,
                                             std::chrono::milliseconds timeout = std::chrono::seconds(30)) {
      auto argv = container::command_prefix({});
      argv.insert(argv.end(), arguments.begin(), arguments.end());
      const auto result = host.run(argv, timeout, maximum_catalog_bytes);
      if (result.exit_status != 0 || result.timed_out || result.output_truncated) return std::nullopt;
      return result.output;
    }

    // One JSON string per line, as --format={{json .Name}} prints. Nothing when
    // the output cannot prove which names exist.
    std::optional<std::set<std::string>> listed_names(const std::optional<std::string> &output) {
      if (!output) return std::nullopt;
      try {
        std::set<std::string> names;
        std::istringstream lines(*output);
        for (std::string line; std::getline(lines, line);) {
          if (line.empty()) continue;
          const auto value = json::parse(line);
          if (!value.is_string()) return std::nullopt;
          names.insert(value.get<std::string>());
        }
        return names;
      } catch (...) { return std::nullopt; }
    }

    bool local_docker_engine(container::host_t &host) {
      if (!host.trusted_runtime_file("/usr/bin/docker")) return false;
      try {
        const auto output = docker_output(host, {"info", "--format={{json .}}"});
        if (!output) return false;
        const auto info = json::parse(*output);
        if (info.at("OSType") != "linux" || !info.at("SecurityOptions").is_array()) return false;
        for (const auto &option : info.at("SecurityOptions"))
          if (!option.is_string() || option.get<std::string>().starts_with("name=rootless")) return false;
        return true;
      } catch (...) { return false; }
    }

    // Exactly the home provision() made for this Space: a local volume without
    // driver options (a bind device would point outside Docker's own storage),
    // labelled for this Space, mounted at its own directory in Docker's store.
    bool created_home(const json &values, const entry_t &entry) {
      try {
        if (!values.is_array() || values.size() != 1) return false;
        const auto &volume = values[0];
        const auto &name = entry.storage.opaque_volume_name;
        const auto &options = volume.at("Options");
        const auto &labels = volume.at("Labels");
        const std::filesystem::path mountpoint = volume.at("Mountpoint").get<std::string>();
        return volume.at("Name") == name && volume.at("Driver") == "local" && volume.at("Scope") == "local" &&
          (options.is_null() || (options.is_object() && options.empty())) &&
          labels.is_object() && labels.contains("io.polaris.multiseat.profile") &&
          labels.at("io.polaris.multiseat.profile") == entry.storage.profile_key &&
          mountpoint.is_absolute() && mountpoint.lexically_normal() == mountpoint &&
          mountpoint.native().ends_with("/volumes/" + name + "/_data");
      } catch (...) { return false; }
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
      if (root.value("schema", 0) == 5) keys(root, {"schema", "owner_uid", "owner_gid", "profiles", "desktop_clients", "desktop_default_clients"});
      else if (root.value("schema", 0) == 4) keys(root, {"schema", "owner_uid", "owner_gid", "profiles", "desktop_clients"});
      else keys(root, {"schema", "owner_uid", "owner_gid", "profiles"});
      if (!root.at("schema").is_number_unsigned() || root.at("schema") < 1 || root.at("schema") > 5 ||
          !root.at("owner_uid").is_number_unsigned() || !root.at("owner_gid").is_number_unsigned() ||
          root.at("owner_uid").get<std::uint64_t>() > 2147483647 ||
          root.at("owner_gid").get<std::uint64_t>() > 2147483647 ||
          !root.at("profiles").is_array() || root.at("profiles").size() > 4096) return std::nullopt;
      catalog_t catalog {root.at("owner_uid").get<std::uint32_t>(), root.at("owner_gid").get<std::uint32_t>(), {}};
      if (root.at("schema") >= 4) catalog.desktop_clients = root.at("desktop_clients").get<std::vector<std::string>>();
      if (root.at("schema") == 5) catalog.desktop_default_clients = root.at("desktop_default_clients").get<std::vector<std::string>>();
      for (const auto &value : root.at("profiles")) {
        if (root.at("schema") >= 3) keys(value, {"id", "name", "volume", "family", "image", "target", "clients", "archived", "access_clients"});
        else if (root.at("schema") == 2) keys(value, {"id", "name", "volume", "family", "image", "target", "clients", "archived"});
        else keys(value, {"id", "name", "volume", "family", "image", "target", "clients"});
        const auto [runtime, kind] = family(value.at("family").get<std::string>());
        if (!value.at("clients").is_array() || value.at("clients").size() > 4096) return std::nullopt;
        // Parse fields before publishing an entry. Invalid JSON must not leave a
        // partially constructed nested aggregate on an exception path.
        entry_t entry;
        entry.storage.profile_key = value.at("id").get<std::string>();
        entry.storage.opaque_volume_name = value.at("volume").get<std::string>();
        entry.storage.runtime_profile = runtime;
        entry.storage.image_reference = value.at("image").get<std::string>();
        entry.name = value.at("name").get<std::string>();
        entry.workload = {kind, value.at("target").get<std::string>()};
        entry.client_keys = value.at("clients").get<std::vector<std::string>>();
        if (root.at("schema") != 1) entry.archived = value.at("archived").get<bool>();
        if (root.at("schema") >= 3) entry.access_clients = value.at("access_clients").get<std::vector<std::string>>();
        catalog.profiles.push_back(std::move(entry));
      }
      return valid(catalog) ? std::optional {std::move(catalog)} : std::nullopt;
    } catch (const std::exception &) { return std::nullopt; }
  }

  std::string encode(const catalog_t &catalog) {
    if (!valid(catalog)) throw std::invalid_argument("invalid profile catalog");
    const bool archives = std::any_of(catalog.profiles.begin(), catalog.profiles.end(), [](const auto &entry) { return entry.archived; });
    // The oldest schema that holds the catalog, so a file without Desktop defaults stays readable by
    // builds from before schema 5.
    const bool desktop_defaults = !catalog.desktop_default_clients.empty();
    const bool desktops = desktop_defaults || !catalog.desktop_clients.empty();
    const bool access = desktops || std::any_of(catalog.profiles.begin(), catalog.profiles.end(), [](const auto &entry) { return !entry.access_clients.empty(); });
    json root {{"schema", desktop_defaults ? 5 : desktops ? 4 : access ? 3 : archives ? 2 : 1}, {"owner_uid", catalog.owner_uid}, {"owner_gid", catalog.owner_gid}, {"profiles", json::array()}};
    if (desktops) root["desktop_clients"] = catalog.desktop_clients;
    if (desktop_defaults) root["desktop_default_clients"] = catalog.desktop_default_clients;
    for (const auto &entry : catalog.profiles) {
      root["profiles"].push_back({{"id", entry.storage.profile_key}, {"name", entry.name},
        {"volume", entry.storage.opaque_volume_name}, {"family", family(entry.storage.runtime_profile)},
        {"image", entry.storage.image_reference}, {"target", entry.workload.target_id}, {"clients", entry.client_keys}});
      if (archives || access) root["profiles"].back()["archived"] = entry.archived;
      if (access) root["profiles"].back()["access_clients"] = entry.access_clients;
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
      if (target == catalog.profiles.end() || target->archived) { result.error = "Unknown or removed space."; return std::nullopt; }
      for (const auto &entry : catalog.profiles) {
        if (std::find(entry.client_keys.begin(), entry.client_keys.end(), client_key) != entry.client_keys.end()) {
          if (&entry == &*target) return encode(catalog);
          result.error = "Device already belongs to another profile. Unassign it first.";
          return std::nullopt;
        }
      }
      target->client_keys.emplace_back(client_key);
      std::erase(catalog.desktop_default_clients, client_key);
      return encode(catalog);
    });
  }

  change_result_t unassign(const std::filesystem::path &path, std::string_view client_key) {
    return change(path, [&](auto &catalog, auto &result) -> std::optional<std::string> {
      if (!token(client_key)) { result.error = "Invalid paired device identifier."; return std::nullopt; }
      for (auto &entry : catalog.profiles) {
        std::erase(entry.client_keys, client_key);
        std::erase(entry.access_clients, client_key);
      }
      std::erase(catalog.desktop_default_clients, client_key);
      return encode(catalog);
    });
  }

  change_result_t set_desktop_access(const std::filesystem::path &path, std::string_view client_key, bool allowed) {
    return change(path, [&](auto &catalog, auto &result) -> std::optional<std::string> {
      if (!token(client_key)) { result.error = "Invalid paired device identifier."; return std::nullopt; }
      std::erase(catalog.desktop_clients, client_key);
      if (allowed) catalog.desktop_clients.emplace_back(client_key);
      // A Default Space is a place the device may play, so Desktop stops being one with its access.
      else std::erase(catalog.desktop_default_clients, client_key);
      return encode(catalog);
    });
  }

  change_result_t set_assignment(const std::filesystem::path &path,
                               std::string_view profile_key, std::string_view client_key) {
    return change(path, [&](catalog_t &catalog, change_result_t &result) -> std::optional<std::string> {
      if (!token(client_key)) { result.error = "Invalid paired device identifier."; return std::nullopt; }
      const auto listed = [&](const std::vector<std::string> &list) {
        return std::find(list.begin(), list.end(), client_key) != list.end();
      };
      // Removing a device from every Space stays its own explicit request.
      if (profile_key.empty()) {
        for (auto &entry : catalog.profiles) {
          std::erase(entry.client_keys, client_key);
          std::erase(entry.access_clients, client_key);
        }
        std::erase(catalog.desktop_default_clients, client_key);
        return encode(catalog);
      }
      const bool desktop = profile_key == desktop_profile_key;
      auto target = std::find_if(catalog.profiles.begin(), catalog.profiles.end(), [&](const auto &entry) {
        return entry.storage.profile_key == profile_key;
      });
      if (!desktop && (target == catalog.profiles.end() || target->archived)) {
        result.error = "Unknown profile."; return std::nullopt;
      }
      const auto refuse = [&](const refusal_t &refusal) -> std::optional<std::string> {
        result.refusal = refusal;
        result.error = std::string(refusal.message);
        return std::nullopt;
      };
      if (desktop && !listed(catalog.desktop_clients)) {
        const bool any_space = std::any_of(catalog.profiles.begin(), catalog.profiles.end(), [&](const auto &entry) {
          return !entry.archived && (listed(entry.client_keys) || listed(entry.access_clients));
        });
        // A device without a Space already opens Desktop, so there is nothing to record.
        if (!any_space) return encode(catalog);
        return refuse(desktop_access_required);
      }
      if (!desktop && !listed(target->client_keys) && !listed(target->access_clients)) return refuse(space_access_required);
      // A default only says where the device opens first. Leaving a Space default keeps that
      // Space open to the device under Device Access.
      for (auto &entry : catalog.profiles) {
        if ((!desktop && &entry == &*target) || !listed(entry.client_keys)) continue;
        std::erase(entry.client_keys, client_key);
        if (!listed(entry.access_clients)) entry.access_clients.emplace_back(client_key);
      }
      std::erase(catalog.desktop_default_clients, client_key);
      if (desktop) catalog.desktop_default_clients.emplace_back(client_key);
      else if (!listed(target->client_keys)) target->client_keys.emplace_back(client_key);
      return encode(catalog);
    });
  }

  change_result_t set_access(const std::filesystem::path &path,
    std::string_view profile_key, std::string_view client_key, bool allowed) {
    if (!token(profile_key) || !token(client_key)) return {.error = "Invalid space or paired device."};
    return change(path, [&](catalog_t &catalog, change_result_t &result) -> std::optional<std::string> {
      auto target = std::find_if(catalog.profiles.begin(), catalog.profiles.end(),
        [&](const auto &entry) { return entry.storage.profile_key == profile_key && !entry.archived; });
      if (target == catalog.profiles.end()) { result.error = "Unknown or removed space."; return std::nullopt; }
      std::erase(target->access_clients, client_key);
      if (allowed) target->access_clients.emplace_back(client_key);
      // Unticking a Space is how a device leaves it, so it stops being that device's Default Space too.
      else std::erase(target->client_keys, client_key);
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

  bool valid_space_create_request(const space_create_request_t &request) {
    return valid_new_steam(request.request_id, request.name) && token(request.source_profile_id) &&
      request.request_id != request.source_profile_id;
  }

  std::optional<space_create_request_t> decode_space_create_request(std::string_view payload) {
    if (payload.empty() || payload.size() > 4096) return std::nullopt;
    try {
      std::set<std::string> names;
      const auto body = json::parse(payload, [&](int depth, json::parse_event_t event, json &value) {
        if (depth > 2) throw std::invalid_argument("creation nesting");
        if (event == json::parse_event_t::key && !names.insert(value.get<std::string>()).second)
          throw std::invalid_argument("duplicate creation field");
        return true;
      });
      keys(body, {"request_id", "source_profile_id", "name"});
      space_create_request_t request {body.at("request_id").get<std::string>(),
        body.at("source_profile_id").get<std::string>(), body.at("name").get<std::string>()};
      return valid_space_create_request(request) ? std::optional {std::move(request)} : std::nullopt;
    } catch (...) { return std::nullopt; }
  }

  change_result_t create_space(const std::filesystem::path &path,
                             const space_create_request_t &request, container::host_t &host) {
    if (!valid_space_create_request(request)) return {.error = "Invalid Space creation request."};
    return change(path, [&](auto &catalog, auto &result) -> std::optional<std::string> {
      if (catalog.owner_uid != host.effective_uid() || catalog.owner_gid != host.effective_gid() ||
          host.effective_uid() != 1000 || host.effective_gid() != 1000) {
        result.error = "Current Spaces runtime images require the catalog and service identity to be 1000:1000.";
        return std::nullopt;
      }
      const auto source = std::find_if(catalog.profiles.begin(), catalog.profiles.end(), [&](const auto &entry) {
        return entry.storage.profile_key == request.source_profile_id;
      });
      // A new Space is the same kind of Space as the one it is based on: the
      // family decides which launcher the image carries and which library is
      // read, so it is inherited rather than chosen on the wire.
      const auto workload = source == catalog.profiles.end() ?
        workload_plan_t {} : launcher_workload(source->storage.runtime_profile);
      if (source == catalog.profiles.end() || workload.kind == workload_kind_e::unknown ||
          !container::supported_streaming_workload(source->storage.runtime_profile, source->workload)) {
        result.error = "Select an existing configured Space."; return std::nullopt;
      }
      const entry_t entry {
        .storage = {request.request_id, "pv-" + request.request_id, source->storage.runtime_profile,
                    source->storage.image_reference},
        .name = request.name, .workload = workload, .client_keys = {},
      };
      const auto existing = std::find_if(catalog.profiles.begin(), catalog.profiles.end(), [&](const auto &value) {
        return value.storage.profile_key == request.request_id;
      });
      if (existing != catalog.profiles.end()) {
        if (existing->name != entry.name || existing->storage.image_reference != entry.storage.image_reference ||
            existing->storage.opaque_volume_name != entry.storage.opaque_volume_name ||
            existing->storage.runtime_profile != entry.storage.runtime_profile || existing->workload != entry.workload) {
          result.error = "This creation request already identifies a different profile."; return std::nullopt;
        }
        result.profile_key = request.request_id;
        return encode(catalog); // Durable confirmation, with no Docker operations or assignment changes.
      }
      catalog.profiles.push_back(entry);
      const auto payload = encode(catalog); // Validate the complete catalog before provisioning.
      result.profile_key = request.request_id;
      provision(host, entry, result);
      return payload;
    });
  }

  bool valid_first_space_request(const first_space_request_t &request) {
    return valid_new_steam(request.request_id, request.name);
  }

  change_result_t create_first_space(const std::filesystem::path &path,
    const first_space_request_t &request, std::string_view image, std::string_view profile,
    container::host_t &host) {
    const auto workload = launcher_workload(launcher_family(profile));
    if (!valid_new_steam(request.request_id, request.name) || !image_id(image) ||
        workload.kind == workload_kind_e::unknown)
      return {.error = "Invalid first-space request, runtime identity or launcher family."};
    if (host.effective_uid() != 1000 || host.effective_gid() != 1000)
      return {.error = "The current Spaces runtimes require service identity 1000:1000. Do not change your Linux user ID."};
    change_result_t result;
    try {
      result.status = private_state_file::update_atomic(path, maximum_catalog_bytes,
        [&](const auto &current) -> std::optional<std::string> {
          auto catalog = current.status == private_state_file::read_status_e::missing ?
            std::optional {catalog_t {1000, 1000, {}}} : current ? decode(current.payload) : std::nullopt;
          if (!catalog || catalog->owner_uid != 1000 || catalog->owner_gid != 1000) {
            result.error = "The private Spaces catalog is unsafe or belongs to another service account.";
            return std::nullopt;
          }
          const entry_t entry {
            .storage = {request.request_id, "pv-" + request.request_id, launcher_family(profile), std::string(image)},
            .name = request.name, .workload = workload, .client_keys = {},
          };
          const auto existing = std::find_if(catalog->profiles.begin(), catalog->profiles.end(), [&](const auto &value) {
            return value.storage.profile_key == request.request_id;
          });
          if (existing != catalog->profiles.end()) {
            if (existing->name != entry.name || existing->storage.image_reference != entry.storage.image_reference ||
                existing->storage.opaque_volume_name != entry.storage.opaque_volume_name ||
                existing->storage.runtime_profile != entry.storage.runtime_profile ||
                existing->workload != entry.workload) {
              result.error = "This creation request already identifies a different space.";
              return std::nullopt;
            }
            result.profile_key = request.request_id;
            return encode(*catalog);
          }
          // The first Space of a launcher family, not the first Space on the
          // host. A family's runtime carries its own launcher and its own
          // library, so a host with Steam Spaces still has no Heroic one to
          // copy, and copying across families would hand it the wrong launcher.
          if (std::any_of(catalog->profiles.begin(), catalog->profiles.end(), [&](const auto &value) {
                return value.storage.runtime_profile == entry.storage.runtime_profile;
              })) {
            result.error = "Spaces is already configured for this launcher. Add another space from the existing setup.";
            return std::nullopt;
          }
          catalog->profiles.push_back(entry);
          const auto payload = encode(*catalog);
          result.profile_key = request.request_id;
          provision(host, entry, result);
          return payload;
        }).status;
    } catch (const std::exception &) {
      result.error = "First-space creation failed. Retain any reported resources for inspection.";
    }
    if (result.status == status_e::durability_uncertain)
      result.error = "Catalog replacement occurred but durability is uncertain. Retry the same request to confirm it; retain the volume.";
    else if (!result && result.error.empty())
      result.error = "The Spaces catalog is busy, unsafe, or could not be saved. No controller was activated.";
    return result;
  }

  bool valid_edit_request(const edit_request_t &request) {
    if (!token(request.profile_id)) return false;
    if (request.operation == edit_operation_e::remove_for_good)
      return request.name.empty() && request_uuid(request.request_id) &&
        !request.confirm_name.empty() && request.confirm_name.size() <= 128 &&
        std::none_of(request.confirm_name.begin(), request.confirm_name.end(), [](unsigned char c) { return c < 32 || c == 127; });
    if (!request.confirm_name.empty() || !request.request_id.empty()) return false;
    if (request.operation == edit_operation_e::remove || request.operation == edit_operation_e::restore) return request.name.empty();
    return request.operation == edit_operation_e::rename && !request.name.empty() && request.name.size() <= 128 &&
      request.name.front() != ' ' && request.name.back() != ' ' &&
      std::none_of(request.name.begin(), request.name.end(), [](unsigned char c) { return c < 32 || c == 127; });
  }

  std::optional<edit_request_t> decode_edit_request(std::string_view payload) {
    if (payload.empty() || payload.size() > 4096) return std::nullopt;
    try {
      std::set<std::string> seen;
      const auto body = json::parse(payload, [&](int depth, json::parse_event_t event, json &value) {
        if (depth > 2) throw std::invalid_argument("space edit nesting");
        if (event == json::parse_event_t::key && !seen.emplace(value.get<std::string>()).second)
          throw std::invalid_argument("duplicate space edit field");
        return true;
      });
      const auto operation = body.at("operation").get<std::string>();
      edit_request_t request;
      if (operation == "rename") {
        keys(body, {"operation", "profile_id", "name"});
        request.name = body.at("name").get<std::string>();
      } else if (operation == "remove" || operation == "restore") {
        keys(body, {"operation", "profile_id"});
        request.operation = operation == "remove" ? edit_operation_e::remove : edit_operation_e::restore;
      } else if (operation == "delete") {
        // Deleting data is its own operation with its own required fields; no
        // extra field can turn an archive into a deletion.
        keys(body, {"operation", "profile_id", "confirm_name", "request_id"});
        request.operation = edit_operation_e::remove_for_good;
        request.confirm_name = body.at("confirm_name").get<std::string>();
        request.request_id = body.at("request_id").get<std::string>();
      } else return std::nullopt;
      request.profile_id = body.at("profile_id").get<std::string>();
      return valid_edit_request(request) ? std::optional {request} : std::nullopt;
    } catch (...) { return std::nullopt; }
  }

  change_result_t edit(const std::filesystem::path &path, const edit_request_t &request) {
    if (!valid_edit_request(request) || request.operation == edit_operation_e::remove_for_good) return {.error = "Invalid space change."};
    return change(path, [&](catalog_t &catalog, change_result_t &result) -> std::optional<std::string> {
      const auto entry = std::find_if(catalog.profiles.begin(), catalog.profiles.end(),
        [&](const auto &value) { return value.storage.profile_key == request.profile_id; });
      if (entry == catalog.profiles.end()) { result.error = "Space not found. Refresh spaces."; return std::nullopt; }
      result.profile_key = entry->storage.profile_key;
      if (request.operation == edit_operation_e::rename) entry->name = request.name;
      else {
        entry->archived = request.operation == edit_operation_e::remove;
        if (entry->archived) { entry->client_keys.clear(); entry->access_clients.clear(); }
        // An archived entry already has no routes. A repeated restore must
        // preserve assignments deliberately added after the first restore.
      }
      return encode(catalog);
    });
  }

  removal_result_t remove_for_good(const std::filesystem::path &path, const edit_request_t &request,
                                   container::host_t &host, std::chrono::milliseconds wait_for_users) {
    using outcome_e = removal_outcome_e;
    removal_result_t result;
    if (request.operation != edit_operation_e::remove_for_good || !valid_edit_request(request)) return result;
    std::string volume, profile_key;
    bool steam = false, home_present = false;
    // Every refusal that changes nothing is decided inside the transaction that
    // archives the Space, so the decision and the archive read the same catalog.
    const auto fenced = change(path, [&](catalog_t &catalog, change_result_t &) -> std::optional<std::string> {
      const auto entry = std::find_if(catalog.profiles.begin(), catalog.profiles.end(),
        [&](const auto &value) { return value.storage.profile_key == request.profile_id; });
      if (entry == catalog.profiles.end()) { result.outcome = outcome_e::not_found; return std::nullopt; }
      if (entry->name != request.confirm_name) { result.outcome = outcome_e::name_mismatch; return std::nullopt; }
      steam = entry->storage.runtime_profile == runtime_profile_e::steam;
      // New Steam Spaces copy an existing one's runtime, so the last one stays.
      if (steam && std::none_of(catalog.profiles.begin(), catalog.profiles.end(), [&](const auto &other) {
            return &other != &*entry && other.storage.runtime_profile == runtime_profile_e::steam;
          })) { result.outcome = outcome_e::last_space; return std::nullopt; }
      volume = entry->storage.opaque_volume_name;
      profile_key = entry->storage.profile_key;
      if (!local_docker_engine(host)) { result.outcome = outcome_e::docker_unavailable; return std::nullopt; }
      const auto volumes = listed_names(docker_output(host, {"volume", "ls", "--format={{json .Name}}"}));
      if (!volumes) { result.outcome = outcome_e::docker_unavailable; return std::nullopt; }
      home_present = volumes->contains(volume);
      if (home_present) {
        const auto inspected = docker_output(host, {"volume", "inspect", volume});
        if (!inspected) { result.outcome = outcome_e::docker_unavailable; return std::nullopt; }
        std::optional<json> values;
        try { values = json::parse(*inspected); } catch (...) {}
        if (!values || !created_home(*values, *entry)) {
          result.outcome = outcome_e::storage_unverified; result.kept_volume = volume; return std::nullopt;
        }
        // A library read can hold the home for a few seconds; wait it out.
        const auto deadline = std::chrono::steady_clock::now() + wait_for_users;
        for (;;) {
          const auto users = listed_names(docker_output(host, {"ps", "--all", "--filter=volume=" + volume, "--format={{json .ID}}"}));
          if (!users) { result.outcome = outcome_e::docker_unavailable; return std::nullopt; }
          if (users->empty()) break;
          if (std::chrono::steady_clock::now() >= deadline) {
            result.outcome = outcome_e::storage_in_use; result.kept_volume = volume; return std::nullopt;
          }
          std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
      }
      // Archived first: if Docker stops partway, the Space cannot be opened
      // with half a home, and a retry from Archived Spaces finishes the job.
      entry->archived = true;
      entry->client_keys.clear();
      entry->access_clients.clear();
      result.outcome = outcome_e::not_saved;
      return encode(catalog);
    });
    if (fenced.status == status_e::durability_uncertain) result.status = fenced.status;
    if (!fenced) {
      if (result.outcome == outcome_e::invalid) result.outcome = outcome_e::not_saved;
      return result;
    }
    result.archived = true;
    if (home_present) {
      // Docker deletes the home. Polaris never opens, walks or unlinks its files.
      (void) docker_output(host, {"volume", "rm", volume}, std::chrono::minutes(10));
      const auto volumes = listed_names(docker_output(host, {"volume", "ls", "--format={{json .Name}}"}));
      if (!volumes || volumes->contains(volume)) {
        result.outcome = outcome_e::storage_not_removed; result.kept_volume = volume;
        return result;
      }
    }
    if (steam) {
      // The network holds no player data. One Docker will not remove is named,
      // and a network whose identity is not the one Polaris made is left alone.
      const auto network = container::profile_network_name(profile_key);
      const auto networks = listed_names(docker_output(host, {"network", "ls", "--format={{json .Name}}"}));
      if (networks && networks->contains(network) && container::profile_network_id(host, profile_key, true))
        (void) docker_output(host, {"network", "rm", network});
      const auto remaining = networks && !networks->contains(network) ? networks :
        listed_names(docker_output(host, {"network", "ls", "--format={{json .Name}}"}));
      if (!remaining || remaining->contains(network)) result.kept_network = network;
    }
    const auto removed = change(path, [&](catalog_t &catalog, change_result_t &) -> std::optional<std::string> {
      const auto entry = std::find_if(catalog.profiles.begin(), catalog.profiles.end(),
        [&](const auto &value) { return value.storage.profile_key == request.profile_id; });
      if (entry != catalog.profiles.end()) {
        if (!entry->archived || entry->storage.opaque_volume_name != volume) return std::nullopt;
        catalog.profiles.erase(entry);
      }
      return encode(catalog);
    });
    if (removed.status == status_e::durability_uncertain) result.status = removed.status;
    result.outcome = removed ? outcome_e::removed : outcome_e::record_not_removed;
    return result;
  }

  bool valid_runtime_move(const runtime_move_t &move) {
    const auto contract = [](std::string_view value) {
      return !value.empty() && value.size() <= 16 && value.find_first_not_of("0123456789") == std::string_view::npos;
    };
    return token(move.profile_id) && image_id(move.from_image) && image_id(move.to_image) &&
      contract(move.from_media_contract) && contract(move.to_media_contract) &&
      move.to_profile != runtime_profile_e::unknown && move.to_uid != 0 && move.to_gid != 0;
  }

  runtime_move_result_t move_runtime(const std::filesystem::path &path, const runtime_move_t &move, container::host_t &host) {
    using outcome_e = runtime_move_outcome_e;
    runtime_move_result_t result;
    if (!valid_runtime_move(move)) return result;
    const auto saved = change(path, [&](catalog_t &catalog, change_result_t &) -> std::optional<std::string> {
      const auto entry = std::find_if(catalog.profiles.begin(), catalog.profiles.end(),
        [&](const auto &value) { return value.storage.profile_key == move.profile_id; });
      if (entry == catalog.profiles.end()) { result.outcome = outcome_e::not_found; return std::nullopt; }
      result.previous_image = entry->storage.image_reference;
      // A retry after the move landed: save the same catalog again, which
      // confirms a replacement whose durability was uncertain.
      if (entry->storage.image_reference == move.to_image) {
        result.outcome = outcome_e::already_moved;
        return encode(catalog);
      }
      if (entry->storage.image_reference != move.from_image) { result.outcome = outcome_e::space_changed; return std::nullopt; }
      if (entry->storage.runtime_profile != move.to_profile) { result.outcome = outcome_e::profile_mismatch; return std::nullopt; }
      if (move.from_media_contract != move.to_media_contract) { result.outcome = outcome_e::media_contract_mismatch; return std::nullopt; }
      if (catalog.owner_uid != move.to_uid || catalog.owner_gid != move.to_gid ||
          host.effective_uid() != catalog.owner_uid || host.effective_gid() != catalog.owner_gid) {
        result.outcome = outcome_e::identity_mismatch; return std::nullopt;
      }
      if (!local_docker_engine(host)) { result.outcome = outcome_e::docker_unavailable; return std::nullopt; }
      const auto &volume = entry->storage.opaque_volume_name;
      const auto volumes = listed_names(docker_output(host, {"volume", "ls", "--format={{json .Name}}"}));
      if (!volumes) { result.outcome = outcome_e::docker_unavailable; return std::nullopt; }
      if (!volumes->contains(volume)) { result.outcome = outcome_e::storage_unverified; return std::nullopt; }
      const auto inspected = docker_output(host, {"volume", "inspect", volume});
      if (!inspected) { result.outcome = outcome_e::docker_unavailable; return std::nullopt; }
      std::optional<json> values;
      try { values = json::parse(*inspected); } catch (...) {}
      if (!values || !created_home(*values, *entry)) { result.outcome = outcome_e::storage_unverified; return std::nullopt; }
      entry->storage.image_reference = move.to_image;
      result.outcome = outcome_e::moved;
      return encode(catalog);
    });
    result.status = saved.status;
    // A refusal is its own answer. A write that did not commit, or a catalog
    // that could not be read at all, is not_saved; with durability_uncertain
    // the replacement may have landed, and a retry confirms which.
    const bool writing = result.outcome == outcome_e::moved || result.outcome == outcome_e::already_moved ||
      result.outcome == outcome_e::invalid;
    if (!saved && writing) result.outcome = outcome_e::not_saved;
    return result;
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
