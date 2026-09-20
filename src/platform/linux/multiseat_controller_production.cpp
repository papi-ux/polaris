/**
 * @file src/platform/linux/multiseat_controller_production.cpp
 * @brief Default-off production dependency factory for multiseat control.
 */
#include "multiseat_controller_production.h"

#ifdef __linux__

  #include "multiseat_container_host.h"
  #include "multiseat_profile_catalog.h"
  #include "multiseat_profile_network.h"
  #include "src/uuid.h"

  #include <algorithm>
  #include <set>
  #include <unordered_set>
  #include <utility>

namespace multiseat {
  namespace {
    class owning_linux_kernel_probe_t final :
        public input::kernel_node_probe_t {
    public:
      owning_linux_kernel_probe_t() : probe_(io_) {
      }

      input::node_observation_t observe(
        const std::filesystem::path &path
      ) override {
        return probe_.observe(path);
      }

    private:
      input::posix_kernel_node_io_t io_;
      input::linux_kernel_node_probe_t probe_;
    };

    class runtime_input_manifest_source_t final :
        public container::input_manifest_source_t {
    public:
      runtime_input_manifest_source_t(
        input::moonlight_session_runtime_t &runtime,
        input::kernel_node_probe_t &probe
      ) :
          runtime_(runtime),
          probe_(probe) {
      }

      std::optional<input::allocation_t> allocation(
        const seat_handle_t &handle
      ) override {
        return runtime_.input_allocation(handle);
      }

      input::node_observation_t observe(
        const std::filesystem::path &path
      ) override {
        return probe_.observe(path);
      }

    private:
      input::moonlight_session_runtime_t &runtime_;
      input::kernel_node_probe_t &probe_;
    };

    struct production_worker_backend_dependencies_t {
      production_worker_backend_dependencies_t(
        std::unique_ptr<container::host_t> host_value,
        std::unique_ptr<input::kernel_node_probe_t> probe_value,
        input::moonlight_session_runtime_t &runtime
      ) :
          host(std::move(host_value)),
          probe(std::move(probe_value)),
          input_manifests(runtime, *probe) {
      }

      // Reverse destruction keeps the manifest ahead of its referenced probe.
      std::unique_ptr<container::host_t> host;
      std::unique_ptr<input::kernel_node_probe_t> probe;
      runtime_input_manifest_source_t input_manifests;
    };

    bool catalog_device_path(const std::filesystem::path &path) {
      if (path.empty() || !path.is_absolute() ||
          path.lexically_normal() != path) {
        return false;
      }
      const auto value = path.native();
      return value.starts_with("/dev/") &&
             value.find(',') == std::string::npos &&
             value.find(':') == std::string::npos &&
             value.find('\n') == std::string::npos &&
             value.find('\r') == std::string::npos;
    }

    bool valid_catalog_boundary(
      const production_controller_options_t &options
    ) {
      if (options.gpus.empty() || !options.container.gpus.empty()) {
        return false;
      }
      std::unordered_set<std::string> logical_ids;
      std::unordered_set<std::string> exclusive_device_paths;
      for (const auto &gpu : options.gpus) {
        if (gpu.logical_gpu_id.empty() ||
            !catalog_device_path(gpu.render_node) ||
            gpu.devices.empty() || gpu.devices.size() > 64 ||
            gpu.max_seats == 0 ||
            gpu.max_encoder_sessions == 0 ||
            !logical_ids.emplace(gpu.logical_gpu_id).second) {
          return false;
        }
        std::unordered_set<std::string> local_device_paths;
        bool render_node_present = false;
        for (const auto &device : gpu.devices) {
          const auto path = device.native();
          if (!catalog_device_path(device) ||
              !local_device_paths.emplace(path).second ||
              !exclusive_device_paths.emplace(path).second) {
            return false;
          }
          render_node_present = render_node_present || device == gpu.render_node;
        }
        if (!render_node_present) {
          return false;
        }
      }
      return true;
    }

    std::optional<std::vector<container::gpu_t>> admitted_container_gpus(
      const std::vector<production_controller_gpu_t> &gpus,
      container::host_t &host
    ) {
      using character_device_key_t =
        std::pair<std::uint32_t, std::uint32_t>;
      std::set<character_device_key_t> exclusive_devices;
      std::set<std::pair<std::uint64_t, std::uint64_t>> exclusive_inodes;
      std::vector<container::gpu_t> result;
      result.reserve(gpus.size());
      for (const auto &gpu : gpus) {
        std::set<character_device_key_t> local_devices;
        container::gpu_t admitted {
          .logical_gpu_id = gpu.logical_gpu_id,
          .render_node = gpu.render_node,
          .max_encoder_sessions = gpu.max_encoder_sessions,
        };
        admitted.devices.reserve(gpu.devices.size());
        for (const auto &device : gpu.devices) {
          const auto identity = host.read_write_character_device(device);
          if (!identity) {
            return std::nullopt;
          }
          const character_device_key_t key {
            identity->character_major,
            identity->character_minor,
          };
          if (!local_devices.emplace(key).second ||
              !exclusive_devices.emplace(key).second ||
              !exclusive_inodes.emplace(
                identity->filesystem_device,
                identity->inode
              ).second) {
            return std::nullopt;
          }
          admitted.devices.push_back({
            .path = device,
            .admitted_identity = *identity,
          });
        }
        result.push_back(std::move(admitted));
      }
      return result;
    }

    std::vector<gpu_capacity_t> registry_gpus(
      const std::vector<production_controller_gpu_t> &gpus
    ) {
      std::vector<gpu_capacity_t> result;
      result.reserve(gpus.size());
      for (const auto &gpu : gpus) {
        result.push_back({
          .logical_gpu_id = gpu.logical_gpu_id,
          .render_node = gpu.render_node,
          .max_seats = gpu.max_seats,
          .max_encoder_sessions = gpu.max_encoder_sessions,
        });
      }
      return result;
    }

    std::optional<std::string> production_controller_epoch() {
      try {
        return uuid_util::uuid_t::generate().string();
      } catch (...) {
        return std::nullopt;
      }
    }
  }  // namespace

  controller_runtime_create_result_t create_production_controller_runtime(
    production_controller_options_t options,
    production_controller_factories_t factories
  ) {
    if (!options.enabled) {
      return controller_runtime_t::create({}, {});
    }
    std::shared_ptr<void> catalog_lease;
    std::vector<profile_summary_t> catalog_summary;
    std::vector<std::string> desktop_clients, desktop_default_clients;
    std::optional<std::pair<std::uint32_t, std::uint32_t>> catalog_owner;
    if (!options.profile_catalog.empty()) {
      if (!options.container.profiles.empty() || !options.container.workloads.empty() ||
          !options.profile_routes.empty() || options.container.engine != container::engine_e::docker ||
          options.container.executable != "/usr/bin/docker" || options.container.runtime_executable != "/usr/bin/runc" ||
          options.container.daemon_socket != "/var/run/docker.sock") {
        return {.status = controller_runtime_create_status_e::invalid_dependencies};
      }
      auto loaded = profiles::load(options.profile_catalog);
      if (!loaded) return {.status = controller_runtime_create_status_e::invalid_dependencies};
      catalog_lease = std::move(loaded->lease);
      desktop_clients = loaded->catalog.desktop_clients;
      desktop_default_clients = loaded->catalog.desktop_default_clients;
      catalog_owner = {loaded->catalog.owner_uid, loaded->catalog.owner_gid};
      for (auto &entry : loaded->catalog.profiles) {
        entry.storage.steam_library_enabled = !entry.archived && entry.storage.runtime_profile == runtime_profile_e::steam;
        entry.storage.host_driver_libraries =
          options.host_driver_image && options.host_driver_image(entry.storage.image_reference);
        catalog_summary.push_back({entry.storage.profile_key, entry.name, entry.client_keys,
          entry.storage.runtime_profile == runtime_profile_e::steam &&
            container::supported_streaming_workload(entry.storage.runtime_profile, entry.workload), entry.archived, entry.access_clients, entry.storage.steam_library_enabled,
          entry.storage.image_reference});
        if (std::find(options.container.workloads.begin(), options.container.workloads.end(),
              entry.workload) == options.container.workloads.end()) {
          options.container.workloads.push_back(entry.workload);
        }
        if (!entry.client_keys.empty() || !entry.access_clients.empty()) {
          options.profile_routes.push_back({entry.storage.profile_key, std::move(entry.client_keys), entry.workload, std::move(entry.access_clients)});
        }
        options.container.profiles.push_back(std::move(entry.storage));
      }
    }
    if (options.container.profiles.empty() && options.profile_routes.empty()) {
      return controller_runtime_t::create({}, {});
    }
    if (!valid_catalog_boundary(options)) {
      return {
        .status = controller_runtime_create_status_e::invalid_dependencies,
      };
    }

    controller_runtime_options_t runtime_options {
      .enabled = true, .worker_media_enabled = options.container.media_enabled,
      .profile_catalog = std::move(catalog_summary),
      .desktop_clients = std::move(desktop_clients),
      .desktop_default_clients = std::move(desktop_default_clients),
    };
    // Resolve profile storage/image and workload from the same trusted catalog
    // the backend will enforce. No second GPU or image allowlist is accepted.
    for (const auto &route : options.profile_routes) {
      const auto &profiles = options.container.profiles;
      const auto profile = std::find_if(profiles.begin(), profiles.end(), [&](const auto &entry) {
        return entry.profile_key == route.profile_key;
      });
      if (profile == profiles.end() ||
          std::count_if(profiles.begin(), profiles.end(), [&](const auto &entry) {
            return entry.profile_key == route.profile_key;
          }) != 1 ||
          std::find(options.container.workloads.begin(), options.container.workloads.end(),
            route.workload) == options.container.workloads.end()) {
        return {.status = controller_runtime_create_status_e::invalid_dependencies};
      }
      controller_profile_route_t resolved {
        .profile_key = route.profile_key,
        .client_keys = route.client_keys,
        .runtime_profile = profile->runtime_profile,
        .workload = route.workload,
        .access_clients = route.access_clients,
        .library_enabled = profile->steam_library_enabled,
      };
      for (const auto &gpu : options.gpus) {
        resolved.logical_gpu_ids.push_back(gpu.logical_gpu_id);
      }
      runtime_options.profile_routes.push_back(std::move(resolved));
    }
    if (!options.profile_catalog.empty()) {
      runtime_options.library_reader = spaces::make_library_reader(options.container.profiles,
        factories.container_host ? factories.container_host : [] { return std::make_unique<container::local_host_t>(); });
    }
    return controller_runtime_t::create(
      std::move(runtime_options),
      [options = std::move(options), factories = std::move(factories),
       catalog_lease = std::move(catalog_lease), catalog_owner]()
        mutable -> std::optional<controller_runtime_dependencies_t> {
        const auto epoch = factories.controller_epoch ?
                             factories.controller_epoch() :
                             production_controller_epoch();
        if (!epoch) {
          return std::nullopt;
        }

        auto host = factories.container_host ?
                      factories.container_host() :
                      std::make_unique<container::local_host_t>();
        if (catalog_owner && (!host || host->effective_uid() != catalog_owner->first ||
                              host->effective_gid() != catalog_owner->second)) return std::nullopt;
        auto admitted_gpus = host ?
                               admitted_container_gpus(options.gpus, *host) :
                               std::nullopt;
        if (!host || !admitted_gpus) {
          return std::nullopt;
        }
        auto probe = factories.kernel_probe ?
                       factories.kernel_probe() :
                       std::make_unique<owning_linux_kernel_probe_t>();
        if (!probe) {
          return std::nullopt;
        }

        auto moonlight = factories.moonlight_runtime ?
                           factories.moonlight_runtime() :
                           input::create_production_moonlight_session_runtime({
                             .enabled = true,
                           });
        if (moonlight.status !=
              input::moonlight_runtime_create_status_e::ready_enabled ||
            !moonlight.runtime) {
          return std::nullopt;
        }

        auto registry = std::make_unique<registry_t>(
          *epoch,
          registry_gpus(options.gpus)
        );
        auto authority =
          std::make_unique<worker_ipc::authority_store_t>(
            options.container.ipc_root
          );
        if (authority->status() != worker_ipc::authority_status_e::applied) {
          return std::nullopt;
        }

        auto backend_dependencies =
          std::make_shared<production_worker_backend_dependencies_t>(
            std::move(host),
            std::move(probe),
            *moonlight.runtime
        );
        auto container_options = std::move(options.container);
        container_options.gpus = std::move(*admitted_gpus);
        auto worker_backend = std::make_unique<container::backend_t>(
          *backend_dependencies->host,
          backend_dependencies->input_manifests,
          std::move(container_options)
        );

        return controller_runtime_dependencies_t {
          .profile_catalog_lease = std::move(catalog_lease),
          .registry = std::move(registry),
          .worker_authority_store = std::move(authority),
          .moonlight_runtime = std::move(moonlight.runtime),
          .worker_backend_dependencies = std::move(backend_dependencies),
          .worker_backend = std::move(worker_backend),
          .recovered_input_expectations = {},
          .worker_options = options.worker,
          .now = {},
          .session_factory = std::move(factories.worker_session),
        };
      }
    );
  }

}  // namespace multiseat

#endif
