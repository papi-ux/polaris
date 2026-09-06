/**
 * @file src/platform/linux/multiseat_controller_production.cpp
 * @brief Default-off production dependency factory for multiseat control.
 */
#include "multiseat_controller_production.h"

#ifdef __linux__

  #include "multiseat_podman_host.h"
  #include "src/uuid.h"

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
        public podman::input_manifest_source_t {
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
        std::unique_ptr<podman::host_t> host_value,
        std::unique_ptr<input::kernel_node_probe_t> probe_value,
        input::moonlight_session_runtime_t &runtime
      ) :
          host(std::move(host_value)),
          probe(std::move(probe_value)),
          input_manifests(runtime, *probe) {
      }

      // Reverse destruction keeps the manifest ahead of its referenced probe.
      std::unique_ptr<podman::host_t> host;
      std::unique_ptr<input::kernel_node_probe_t> probe;
      runtime_input_manifest_source_t input_manifests;
    };

    bool valid_catalog_boundary(
      const production_controller_options_t &options
    ) {
      if (options.gpus.empty() || !options.podman.gpus.empty()) {
        return false;
      }
      std::unordered_set<std::string> logical_ids;
      for (const auto &gpu : options.gpus) {
        if (gpu.logical_gpu_id.empty() || gpu.render_node.empty() ||
            gpu.devices.empty() || gpu.max_seats == 0 ||
            gpu.max_encoder_sessions == 0 ||
            !logical_ids.emplace(gpu.logical_gpu_id).second) {
          return false;
        }
      }
      return true;
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

    std::vector<podman::gpu_t> podman_gpus(
      const std::vector<production_controller_gpu_t> &gpus
    ) {
      std::vector<podman::gpu_t> result;
      result.reserve(gpus.size());
      for (const auto &gpu : gpus) {
        result.push_back({
          .logical_gpu_id = gpu.logical_gpu_id,
          .render_node = gpu.render_node,
          .devices = gpu.devices,
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
    if (!valid_catalog_boundary(options)) {
      return {
        .status = controller_runtime_create_status_e::invalid_dependencies,
      };
    }

    return controller_runtime_t::create(
      {.enabled = true},
      [options = std::move(options), factories = std::move(factories)]()
        mutable -> std::optional<controller_runtime_dependencies_t> {
        const auto epoch = factories.controller_epoch ?
                             factories.controller_epoch() :
                             production_controller_epoch();
        if (!epoch) {
          return std::nullopt;
        }

        auto host = factories.podman_host ?
                      factories.podman_host() :
                      std::make_unique<podman::local_host_t>();
        auto probe = factories.kernel_probe ?
                       factories.kernel_probe() :
                       std::make_unique<owning_linux_kernel_probe_t>();
        if (!host || !probe) {
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
            options.podman.ipc_root
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
        auto podman_options = std::move(options.podman);
        podman_options.gpus = podman_gpus(options.gpus);
        auto worker_backend = std::make_unique<podman::backend_t>(
          *backend_dependencies->host,
          backend_dependencies->input_manifests,
          std::move(podman_options)
        );

        return controller_runtime_dependencies_t {
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
