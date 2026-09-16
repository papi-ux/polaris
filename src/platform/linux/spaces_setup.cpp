#include "spaces_setup.h"
#ifdef __linux__
#include <algorithm>
#include <filesystem>
#include <fstream>

namespace multiseat::spaces {
  namespace {
    using json = nlohmann::json;
    bool driver_version(std::string_view value) {
      return value.size() >= 3 && value.size() <= 32 && value.front() != '.' && value.back() != '.' &&
        value.find_first_not_of("0123456789.") == std::string_view::npos &&
        value.find("..") == std::string_view::npos && value.find('.') != std::string_view::npos;
    }
    // The loaded NVIDIA kernel module names its version. Only a plain dotted
    // version is ever repeated to the reader.
    std::optional<std::string> loaded_nvidia_driver() {
      std::ifstream file("/sys/module/nvidia/version");
      if (!file) return std::nullopt;
      std::string version(33, '\0');
      file.read(version.data(), static_cast<std::streamsize>(version.size()));
      version.resize(static_cast<std::size_t>(file.gcount()));
      while (!version.empty() && (version.back() == '\n' || version.back() == ' ')) version.pop_back();
      return driver_version(version) ? version : std::string {};
    }
    std::string either(const std::vector<std::string> &drivers) {
      std::string text;
      for (std::size_t i = 0; i < drivers.size(); ++i)
        text += (i == 0 ? "" : i + 1 == drivers.size() ? " or " : ", ") + drivers[i];
      return text;
    }
    json describe_runtime(const runtime_facts_t &r) {
      const auto name = !r.runtime ? std::string {} : r.runtime->variant == "nvidia" ?
        "gaming runtime for NVIDIA driver " + r.runtime->nvidia_driver : std::string {"gaming runtime for AMD and Intel graphics"};
      std::string detail;
      if (r.status == "not_published") detail = "This Polaris build has no approved gaming runtime yet.";
      else if (r.code == "driver_mismatch")
        detail = "The gaming runtime in this Polaris build needs NVIDIA driver " + either(r.nvidia_drivers) + ". " +
          (r.host_nvidia_driver && !r.host_nvidia_driver->empty() ?
            "This PC runs " + *r.host_nvidia_driver + ". Install the matching driver, restart the PC, then recheck." :
            std::string {"Polaris could not read the NVIDIA driver version on this PC."});
      else if (r.code == "graphics_unsupported")
        detail = r.host_nvidia_driver ?
          std::string {"The gaming runtime in this Polaris build is for AMD and Intel graphics. This PC uses the NVIDIA driver, and no NVIDIA runtime is approved yet."} :
          "The gaming runtime in this Polaris build needs NVIDIA graphics with driver " + either(r.nvidia_drivers) +
            ". No NVIDIA driver is loaded on this PC.";
      else if (r.status == "ready") detail = "The " + name + " is downloaded and verified.";
      else if (r.code == "docker_unavailable") detail = "This PC needs the " + name + ". Polaris checks whether it is downloaded once Docker Engine is ready.";
      else if (r.status == "available") detail = "This PC needs the " + name + ". It is not downloaded yet, and the download is several gigabytes.";
      else if (r.code == "runtime_identity_mismatch")
        detail = "The gaming runtime on this PC does not match this Polaris build, so Spaces will not use it. Retry, and open Doctor & Support if it persists.";
      else detail = "Polaris could not verify the gaming runtime on this PC. Retry, and check Docker if it keeps failing.";
      json runtime {{"status", r.status}, {"code", r.code}};
      if (r.runtime) {
        runtime["id"] = r.runtime->id; runtime["variant"] = r.runtime->variant;
        runtime["nvidia_driver"] = r.runtime->nvidia_driver;
      }
      // A build without a runtime is nothing this PC can fix, so that row is
      // neutral. Downloading is the step while one fits and is not verified.
      return {{"id", "runtime"}, {"title", "Gaming runtime"},
        {"state", r.status == "ready" ? "ready" : r.status == "not_published" ? "not_configured" : "required"},
        {"detail", std::move(detail)},
        {"action", r.status == "available" ? "download_runtime" : r.status == "failed" ? "retry_runtime" : ""},
        {"doc_anchor", "#download-the-gaming-runtime"}, {"runtime", std::move(runtime)}};
    }
  }

  runtime_choice_t choose_runtime(const std::vector<runtime_t> &catalog, const std::optional<std::string> &nvidia_driver) {
    if (catalog.empty()) return {std::nullopt, "runtime_not_published"};
    const std::string_view variant = nvidia_driver ? "nvidia" : "default";
    for (const auto &runtime : catalog) {
      if (runtime.variant == variant && (!nvidia_driver || runtime.nvidia_driver == *nvidia_driver)) return {runtime, {}};
    }
    const bool other_driver = nvidia_driver &&
      std::any_of(catalog.begin(), catalog.end(), [](const auto &runtime) { return runtime.variant == "nvidia"; });
    return {std::nullopt, other_driver ? "driver_mismatch" : "graphics_unsupported"};
  }

  runtime_inspection_cache_t::runtime_inspection_cache_t(std::chrono::steady_clock::duration lifetime, now_t now) :
    lifetime_(lifetime), now_(now ? std::move(now) : now_t([] { return std::chrono::steady_clock::now(); })) {}

  runtime_image_e runtime_inspection_cache_t::remember(const std::string &reference, const std::function<runtime_image_e()> &inspect) {
    std::uint64_t generation;
    {
      std::lock_guard lock(mutex_);
      if (entry_ && entry_->reference == reference && now_() - entry_->checked < lifetime_) return entry_->image;
      generation = generation_;
    }
    const auto image = inspect();
    // Timeouts and unreadable replies are asked again next time. An answer
    // that raced a download is dropped too: the download may have changed it.
    if (image != runtime_image_e::unverifiable) {
      std::lock_guard lock(mutex_);
      if (generation == generation_) entry_ = entry_t {reference, image, now_()};
    }
    return image;
  }

  void runtime_inspection_cache_t::forget() {
    std::lock_guard lock(mutex_);
    entry_.reset();
    ++generation_;
  }

  runtime_inspection_cache_t &runtime_inspection_cache() {
    static runtime_inspection_cache_t cache;
    return cache;
  }

  runtime_facts_t inspect_runtime(container::host_t &host, const std::vector<runtime_t> &catalog,
    const std::optional<std::string> &nvidia_driver, bool engine_ready, runtime_inspection_cache_t *cache) {
    runtime_facts_t facts;
    facts.host_nvidia_driver = nvidia_driver;
    for (const auto &runtime : catalog) {
      if (runtime.variant == "nvidia" &&
          std::find(facts.nvidia_drivers.begin(), facts.nvidia_drivers.end(), runtime.nvidia_driver) == facts.nvidia_drivers.end())
        facts.nvidia_drivers.push_back(runtime.nvidia_driver);
    }
    auto choice = choose_runtime(catalog, nvidia_driver);
    if (!choice.runtime) {
      facts.status = choice.code == "runtime_not_published" ? "not_published" : "unsupported";
      facts.code = std::move(choice.code);
      return facts;
    }
    facts.runtime = std::move(choice.runtime);
    if (!engine_ready) {
      facts.status = "available"; facts.code = "docker_unavailable";
      return facts;
    }
    const auto &runtime = *facts.runtime;
    const auto inspect = [&] {
      auto argv = container::command_prefix({});
      argv.insert(argv.end(), {"image", "inspect", runtime.reference()});
      const auto result = host.run(argv, std::chrono::seconds(5), 65536);
      if (result.timed_out || result.output_truncated) return runtime_image_e::unverifiable;
      if (result.exit_status == 0) {
        if (verified_runtime_image(runtime, result.output)) return runtime_image_e::verified;
        // Docker described one image under the pinned reference, and it differs.
        const auto images = json::parse(result.output, nullptr, false);
        return images.is_array() && images.size() == 1 ? runtime_image_e::mismatch : runtime_image_e::unverifiable;
      }
      // Docker lists what it found, an empty list, and exits 1 when nothing
      // has this reference. The engine answered moments ago.
      std::string_view found = result.output;
      while (!found.empty() && (found.back() == '\n' || found.back() == ' ')) found.remove_suffix(1);
      return result.exit_status == 1 && found == "[]" ? runtime_image_e::absent : runtime_image_e::unverifiable;
    };
    const auto image = cache ? cache->remember(runtime.reference(), inspect) : inspect();
    facts.status = image == runtime_image_e::verified ? "ready" : image == runtime_image_e::absent ? "available" : "failed";
    facts.code = image == runtime_image_e::verified ? "runtime_ready" : image == runtime_image_e::absent ? "not_downloaded" :
      image == runtime_image_e::mismatch ? "runtime_identity_mismatch" : "inspection_failed";
    return facts;
  }

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
    // Not a host prerequisite: first-Space setup downloads the runtime too.
    checks.push_back(describe_runtime(f.runtime));
    checks.push_back({{"id", "spaces"}, {"title", "Spaces configuration"},
      {"state", f.controller_available ? "ready" : f.controller_enabled ? "required" : "not_configured"},
      {"detail", f.controller_available ? "The configured Spaces controller is available." :
        f.controller_enabled ? "The configured Spaces controller is unavailable. Review the setup diagnostics." :
        "No Space has been configured on this host yet."},
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
    static const std::vector<runtime_t> unpublished;
    const auto &catalog = trusted_runtimes();
    f.runtime = inspect_runtime(host, catalog ? *catalog : unpublished, loaded_nvidia_driver(),
      f.docker_cli && f.runc && f.daemon_replied && f.daemon_linux && !f.daemon_rootless && f.daemon_runc,
      &runtime_inspection_cache());
    return describe_setup(f);
  }
}
#endif
