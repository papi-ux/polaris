#include "spaces_runtime.h"
#ifdef __linux__
#include "spaces_runtime_catalog.h"
#include "multiseat_profile_catalog.h"
#include <algorithm>
#include <iostream>
#include <set>
#include <stdexcept>

namespace multiseat::spaces {
  namespace {
    using json = nlohmann::json;
    bool hex(std::string_view value, std::size_t size) {
      return value.size() == size && value.find_first_not_of("0123456789abcdef") == std::string_view::npos;
    }
    bool digest(std::string_view value) { return value.starts_with("sha256:") && hex(value.substr(7), 64); }
    bool valid(const runtime_t &r) {
      return !r.id.empty() && r.id.size() <= 64 && r.id.front() != '-' &&
        r.id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") == std::string::npos &&
        hex(r.source_revision, 40) && digest(r.registry_digest) && digest(r.config_digest) &&
        ((r.variant == "default" && r.nvidia_driver.empty()) ||
         (r.variant == "nvidia" && r.nvidia_driver.size() >= 3 && r.nvidia_driver.size() <= 32 &&
          r.nvidia_driver.front() != '.' && r.nvidia_driver.back() != '.' &&
          r.nvidia_driver.find_first_not_of("0123456789.") == std::string::npos &&
          r.nvidia_driver.find("..") == std::string::npos && r.nvidia_driver.find('.') != std::string::npos));
    }
    json strict_json(std::string_view text) {
      if (text.empty() || text.size() > 65536) throw std::invalid_argument("invalid metadata size");
      std::vector<std::set<std::string>> keys;
      return json::parse(text, [&](int depth, json::parse_event_t event, json &value) {
        if (depth > 8) throw std::invalid_argument("metadata nesting");
        if (event == json::parse_event_t::object_start) keys.emplace_back();
        if (event == json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)
          throw std::invalid_argument("duplicate metadata key");
        if (event == json::parse_event_t::object_end) keys.pop_back();
        return true;
      });
    }
    bool succeeded(const container::command_result_t &r) {
      return r.exit_status == 0 && !r.timed_out && !r.output_truncated;
    }
    bool local_engine(container::host_t &host) {
      if (!host.trusted_runtime_file("/usr/bin/docker") || !host.trusted_runtime_file("/usr/bin/runc")) return false;
      auto args = container::command_prefix({});
      args.insert(args.end(), {"info", "--format={{json .}}"});
      const auto result = host.run(args, std::chrono::seconds(3), 65536);
      if (!succeeded(result)) return false;
      try {
        const auto info = strict_json(result.output);
        if (info.at("OSType") != "linux" || !info.at("SecurityOptions").is_array()) return false;
        for (const auto &option : info.at("SecurityOptions"))
          if (!option.is_string() || option.get<std::string>().starts_with("name=rootless")) return false;
        const auto runtime = info.at("Runtimes").at("runc").at("path");
        return runtime == "runc" || runtime == "/usr/bin/runc";
      } catch (...) { return false; }
    }
  }

  std::string runtime_t::reference() const {
    return std::string {runtime_repository} + "@" + registry_digest;
  }

  bool runtime_t::matches_image_id(std::string_view image) const {
    return valid(*this) && (image == config_digest || image == registry_digest);
  }

  std::optional<std::vector<runtime_t>> decode_runtime_catalog(std::string_view payload) {
    try {
      const auto document = strict_json(payload);
      if (!document.is_object() || document.size() != 2 || !document.at("schema").is_number_unsigned() ||
          document.at("schema") != 1 || !document.at("runtimes").is_array() || document.at("runtimes").size() > 16)
        return std::nullopt;
      std::vector<runtime_t> runtimes;
      std::set<std::string> ids, references;
      for (const auto &entry : document.at("runtimes")) {
        if (!entry.is_object() || entry.size() != 11 || entry.at("profile") != "steam" ||
            entry.at("platform") != "linux/amd64" || !entry.at("media_contract").is_number_unsigned() ||
            entry.at("media_contract") != 1 || !entry.at("uid").is_number_unsigned() || entry.at("uid") != 1000 ||
            !entry.at("gid").is_number_unsigned() || entry.at("gid") != 1000) return std::nullopt;
        runtime_t r {entry.at("id"), entry.at("variant"), entry.at("source_revision"),
          entry.at("registry_digest"), entry.at("config_digest"), entry.at("nvidia_driver")};
        if (!valid(r) || !ids.insert(r.id).second || !references.insert(r.reference()).second) return std::nullopt;
        runtimes.emplace_back(std::move(r));
      }
      return runtimes;
    } catch (...) { return std::nullopt; }
  }

  const std::optional<std::vector<runtime_t>> &trusted_runtimes() {
    static const auto catalog = decode_runtime_catalog(runtime_catalog_json);
    return catalog;
  }

  std::optional<std::string> verified_runtime_image(const runtime_t &r, std::string_view inspection) {
    if (!valid(r)) return {};
    try {
      const auto images = strict_json(inspection);
      if (!images.is_array() || images.size() != 1) return {};
      const auto &image = images.front();
      if (!r.matches_image_id(image.at("Id").get<std::string>()) || image.at("Os") != "linux" || image.at("Architecture") != "amd64" ||
          !image.at("RepoDigests").is_array() ||
          std::find(image.at("RepoDigests").begin(), image.at("RepoDigests").end(), r.reference()) == image.at("RepoDigests").end())
        return {};
      if (image.at("Id") == r.registry_digest) {
        // The reviewed registry manifest already binds its config digest. Do
        // not accept an arbitrary descriptor, tag or multi-platform index.
        const auto &descriptor = image.at("Descriptor");
        if (descriptor.at("digest") != r.registry_digest ||
            !descriptor.at("size").is_number_unsigned() || descriptor.at("size") == 0 || descriptor.at("size") > 65536 ||
            (descriptor.at("mediaType") != "application/vnd.oci.image.manifest.v1+json" &&
             descriptor.at("mediaType") != "application/vnd.docker.distribution.manifest.v2+json")) return {};
      }
      const auto &config = image.at("Config"), &labels = config.at("Labels");
      if (labels.at("org.opencontainers.image.source") != "https://github.com/papi-ux/polaris" ||
          labels.at("org.opencontainers.image.revision") != r.source_revision ||
          labels.at("io.polaris.multiseat.profile") != "steam" ||
          labels.at("io.polaris.multiseat.architecture") != "linux/amd64" ||
          labels.at("io.polaris.multiseat.media-contract") != "1" ||
          config.at("Entrypoint") != json::array({"/usr/bin/polaris-seat-worker"}) ||
          config.at("Cmd") != json::array({"run"})) return {};
      for (const auto *key : {"Volumes", "ExposedPorts", "OnBuild"})
        if (config.contains(key) && !config.at(key).empty()) return {};
      if (labels.value("io.polaris.multiseat.nvidia.driver", "") != r.nvidia_driver) return {};
      return image.at("Id").get<std::string>();
    } catch (...) { return {}; }
  }

  bool matches_runtime_image(const runtime_t &runtime, std::string_view inspection) {
    return verified_runtime_image(runtime, inspection).has_value();
  }

  runtime_install_result_t install_runtime(container::host_t &host, std::string_view id,
    const std::vector<runtime_t> &catalog, std::stop_token stop) {
    const auto cancelled = [] { return runtime_install_result_t {false, "download_cancelled",
      "Setup stopped. Docker may keep verified download layers for a later retry.", {}}; };
    const auto found = std::find_if(catalog.begin(), catalog.end(), [&](const auto &r) { return r.id == id; });
    if (found == catalog.end() || !valid(*found))
      return {false, "runtime_not_published", "This Polaris build has no approved download for that runtime.", {}};
#if !defined(__x86_64__)
    return {false, "unsupported_platform", "This runtime requires a Linux x86-64 host.", {}};
#endif
    if (stop.stop_requested()) return cancelled();
    const bool engine_ready = local_engine(host);
    if (stop.stop_requested()) return cancelled();
    if (!engine_ready)
      return {false, "docker_unavailable", "Polaris needs access to the system Docker Engine and runc.", {}};
    const auto &runtime = *found;
    auto inspect = container::command_prefix({});
    inspect.insert(inspect.end(), {"image", "inspect", runtime.reference()});
    const auto before = host.run(inspect, std::chrono::seconds(5), 65536);
    if (stop.stop_requested()) return cancelled();
    if (succeeded(before)) {
      if (const auto image = verified_runtime_image(runtime, before.output))
        return {true, "runtime_ready", "The approved gaming runtime is available.", *image};
      return {false, "runtime_identity_mismatch", "The installed runtime does not match this Polaris build. No space was created.", {}};
    }
    auto pull = container::command_prefix({});
    pull.insert(pull.end(), {"image", "pull", "--quiet", "--platform=linux/amd64", runtime.reference()});
    // No mutable tags, credentials, remote Docker contexts or caller-provided
    // URLs. The daemon checks the registry digest and content-addressed layers.
    const auto downloaded = host.run(pull, std::chrono::minutes(30), 65536);
    if (stop.stop_requested()) return cancelled();
    if (!succeeded(downloaded))
      return {false, "download_incomplete", "The runtime download did not finish. Retry the same runtime to reuse verified layers.", {}};
    const auto after = host.run(inspect, std::chrono::seconds(5), 65536);
    if (stop.stop_requested()) return cancelled();
    const auto image = succeeded(after) ? verified_runtime_image(runtime, after.output) : std::nullopt;
    if (!image)
      return {false, "runtime_verification_failed", "The downloaded runtime could not be verified. No space was created.", {}};
    return {true, "runtime_ready", "The approved gaming runtime is available.", *image};
  }

  int runtime_command(int argc, char **argv) {
    const auto &catalog = trusted_runtimes();
    if (!catalog) {
      std::cerr << "The packaged Spaces runtime catalog is invalid.\n";
      return 1;
    }
    if (argc == 1 && std::string_view(argv[0]) == "list") {
      json entries = json::array();
      for (const auto &r : *catalog) entries.push_back({{"id", r.id}, {"variant", r.variant},
        {"platform", "linux/amd64"}, {"nvidia_driver", r.nvidia_driver}});
      std::cout << json({{"schema", 1}, {"runtimes", entries}}).dump() << '\n';
      return 0;
    }
    if (argc == 5 && std::string_view(argv[0]) == "create-first") {
      container::local_host_t host;
      const auto runtime = install_runtime(host, argv[2], *catalog);
      if (!runtime.ready) {
        std::cout << json({{"prepared", false}, {"activated", false},
          {"code", runtime.code}, {"message", runtime.message}}).dump() << '\n';
        return 1;
      }
      const auto result = profiles::create_first_steam(argv[1], {argv[3], argv[4]}, runtime.image, host);
      std::cout << json({{"prepared", static_cast<bool>(result)}, {"activated", false},
        {"profile_id", result.profile_key}, {"volume", result.volume_name},
        {"initializer", result.initializer_name}, {"network", result.network_name},
        {"message", result ? "The first Steam home is prepared. Controller configuration and device assignment are still required." : result.error}}).dump() << '\n';
      return result ? 0 : 1;
    }
    if (argc == 2 && std::string_view(argv[0]) == "install") {
      container::local_host_t host;
      const auto result = install_runtime(host, argv[1], *catalog);
      std::cout << json({{"ready", result.ready}, {"code", result.code},
        {"message", result.message}, {"image", result.image}}).dump() << '\n';
      return result.ready ? 0 : 1;
    }
    std::cerr << "Usage: polaris --spaces-runtime list | install RUNTIME_ID\n"
                 "       polaris --spaces-runtime create-first CATALOG RUNTIME_ID REQUEST_ID NAME\n";
    return 2;
  }
}
#endif
