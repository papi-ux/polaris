#include "spaces_setup.h"
#ifdef __linux__
#include <filesystem>
#include <fstream>

namespace multiseat::spaces {
  nlohmann::json describe_setup(const setup_facts_t &f) {
    using json = nlohmann::json;
    json checks = json::array();
    // Every check names the guide section that fixes it, so a failing row is
    // never a dead end whatever the console decides to render.
    auto add = [&](const char *id, const char *title, bool ready, const char *detail,
                   const char *action, const char *anchor) {
      checks.push_back({{"id", id}, {"title", title}, {"state", ready ? "ready" : "required"},
        {"detail", detail}, {"action", action}, {"doc_anchor", anchor}});
    };
    add("docker", "Docker Engine", f.docker_cli && f.runc,
      f.docker_cli && f.runc ? "Docker and its container runtime are installed." :
      "Install Docker Engine on this PC to run separate gaming spaces.", "install_docker", "#prepare-docker-from-spaces");
    const bool engine = f.daemon_replied && f.daemon_linux && !f.daemon_rootless && f.daemon_runc;
    add("docker_access", "Polaris access to Docker", engine,
      engine ? "This Polaris service can reach the local Docker Engine." :
      !f.daemon_replied ? "Start Docker and allow the Polaris service account to use it. Recheck after signing out and back in." :
      f.daemon_rootless ? "This version requires the system Docker Engine. Rootless Docker is not supported for Spaces yet." :
      "Spaces needs a local Linux Docker Engine with runc.", "docker_access", "#prepare-docker-from-spaces");
    add("identity", "Gaming runtime account", f.uid == 1000 && f.gid == 1000,
      f.uid == 1000 && f.gid == 1000 ? "The current runtime supports this service account." :
      "The current preview runtime does not yet support this service account. Do not change your Linux user ID.",
      "", "#gaming-runtime-account");
    add("input", "Controller and input access", f.input_access,
      f.input_access ? "Polaris can create virtual input devices. Controls are checked again when a space starts." :
      "Complete Polaris host setup so streamed controls can reach a space.", "host_setup", "#controller-access");
    add("gpu", "Graphics device access", f.gpu_access,
      f.gpu_access ? "A graphics device is accessible. Hardware encoding is checked when the space starts." :
      "Polaris cannot access a graphics device. Check the driver and host permissions.", "host_setup", "#graphics-access");
    add("security", "Spaces security support", f.security.ready, f.security.detail.c_str(), f.security.code.c_str(),
      "#prepare-spaces-security-support");
    checks.push_back({{"id", "spaces"}, {"title", "Spaces configuration"},
      {"state", f.controller_available ? "ready" : f.controller_enabled ? "required" : "not_configured"},
      {"detail", f.controller_available ? "The configured Spaces controller is available." :
        f.controller_enabled ? "The configured Spaces controller is unavailable. Review the setup diagnostics." :
        "Host preparation comes first. No spaces have been configured on this host."},
      {"action", "configure_spaces"}, {"doc_anchor", "#prepare-your-first-space"}});
    return {{"version", 2}, {"distribution", f.distribution},
      {"immutable_host", f.immutable_host}, {"service_uid", f.uid},
      {"host_prerequisites_ready", f.docker_cli && f.runc && engine && f.uid == 1000 && f.gid == 1000 && f.input_access && f.gpu_access && f.security.ready},
      {"configured", f.controller_enabled}, {"available", f.controller_available},
      {"checks", std::move(checks)}};
  }

  nlohmann::json inspect_setup(container::host_t &host, bool enabled, bool available, const std::optional<security_facts_t> &security) {
    setup_facts_t f;
    f.security = describe_security(security ? *security : inspect_security(host));
    f.uid = host.effective_uid(); f.gid = host.effective_gid();
    f.controller_enabled = enabled; f.controller_available = available;
    std::error_code error;
    f.immutable_host = std::filesystem::exists("/run/ostree-booted", error);
    // If the host type cannot be determined, do not offer mutable package steps.
    if (error) f.immutable_host = true;
    std::ifstream release("/etc/os-release");
    std::string line;
    // Read a literal ID, never source distribution files as shell code.
    for (unsigned lines = 0; lines < 64 && std::getline(release, line); ++lines) {
      if (line.starts_with("ID=")) {
        auto id = line.substr(3);
        if (id.size() >= 2 && id.front() == '"' && id.back() == '"') id = id.substr(1, id.size() - 2);
        if (id == "fedora" || id == "arch" || id == "cachyos" || id == "ubuntu" || id == "bazzite" || id == "steamos")
          f.distribution = id;
      }
    }
    f.docker_cli = host.trusted_runtime_file("/usr/bin/docker");
    f.runc = host.trusted_runtime_file("/usr/bin/runc");
    if (f.docker_cli && f.runc) {
      auto argv = container::command_prefix({});
      argv.insert(argv.end(), {"info", "--format={{json .}}"});
      auto result = host.run(argv, std::chrono::seconds(3), 65536);
      if (!result.timed_out && !result.output_truncated && result.exit_status == 0) {
        try {
          const auto info = nlohmann::json::parse(result.output);
          if (info.is_object() && info.contains("SecurityOptions") && info["SecurityOptions"].is_array()) {
            f.daemon_replied = true; f.daemon_linux = info.value("OSType", "") == "linux";
            for (const auto &option : info["SecurityOptions"]) {
              if (!option.is_string()) { f.daemon_replied = false; break; }
              if (option.get<std::string>().starts_with("name=rootless")) f.daemon_rootless = true;
            }
            auto runtime = info.at("Runtimes").at("runc").value("path", "");
            f.daemon_runc = runtime == "runc" || runtime == "/usr/bin/runc";
          }
        } catch (...) { f.daemon_replied = false; }
      }
    }
    f.input_access = host.read_write_character_device("/dev/uinput").has_value() &&
      host.read_write_character_device("/dev/uhid").has_value();
    // Limit the read-only device probe. Seat admission still selects and
    // verifies exact physical devices; this does not allocate or open a GPU.
    for (unsigned i = 128; i < 192; ++i) {
      if (host.read_write_character_device("/dev/dri/renderD" + std::to_string(i))) {
        f.gpu_access = true; break;
      }
    }
    return describe_setup(f);
  }
}
#endif
