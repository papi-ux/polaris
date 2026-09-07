/**
 * @file src/platform/linux/multiseat_podman_backend.h
 * @brief Rootless Podman worker backend for isolated multiseat workers.
 */
#pragma once

#ifdef __linux__

#include "src/multiseat_worker_broker.h"
#include "src/platform/linux/input/inputtino_multiseat_backend.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace multiseat::podman {

  struct command_result_t {
    int exit_status = 127;
    bool timed_out = false;
    bool output_truncated = false;
    std::string output;
  };

  struct character_device_identity_t {
    std::uint64_t filesystem_device = 0;
    std::uint64_t inode = 0;
    std::uint32_t character_major = 0;
    std::uint32_t character_minor = 0;

    bool operator==(const character_device_identity_t &) const = default;
  };

  /**
   * Injectable host boundary. The production implementation uses an explicit
   * argv vector and the existing bounded Linux process runner; tests never
   * invoke a container engine or inspect live devices.
   */
  class host_t {
  public:
    virtual ~host_t() = default;

    [[nodiscard]] virtual std::uint64_t effective_uid() const = 0;
    [[nodiscard]] virtual bool executable_file(const std::filesystem::path &path) const = 0;
    [[nodiscard]] virtual bool readable_directory(const std::filesystem::path &path) const = 0;
    [[nodiscard]] virtual bool private_read_write_directory(
      const std::filesystem::path &path
    ) const = 0;
    [[nodiscard]] virtual bool private_readable_file(
      const std::filesystem::path &path
    ) const = 0;
    [[nodiscard]] virtual std::optional<character_device_identity_t>
    read_write_character_device(
      const std::filesystem::path &path
    ) const = 0;
    virtual command_result_t run(
      const std::vector<std::string> &argv,
      std::chrono::milliseconds timeout,
      std::size_t max_output_bytes
    ) = 0;
  };

  /**
   * Trusted input-allocation boundary used immediately before Podman launch.
   *
   * The production-shaped adapter below reads allocations from the generation-
   * fenced input authority and reuses its kernel probe. Tests inject a fake, so
   * no input node or container engine is opened by this checkpoint.
   */
  class input_manifest_source_t {
  public:
    virtual ~input_manifest_source_t() = default;

    [[nodiscard]] virtual std::optional<input::allocation_t> allocation(
      const seat_handle_t &handle
    ) = 0;
    [[nodiscard]] virtual input::node_observation_t observe(
      const std::filesystem::path &path
    ) = 0;
  };

  class authority_input_manifest_source_t final : public input_manifest_source_t {
  public:
    authority_input_manifest_source_t(
      input::authority_t &authority,
      input::kernel_node_probe_t &probe
    );

    [[nodiscard]] std::optional<input::allocation_t> allocation(
      const seat_handle_t &handle
    ) override;
    [[nodiscard]] input::node_observation_t observe(
      const std::filesystem::path &path
    ) override;

  private:
    input::authority_t &authority_;
    input::kernel_node_probe_t &probe_;
  };

  /** Opaque generation fingerprint stored in the worker's inspected labels. */
  [[nodiscard]] std::optional<std::string> input_manifest_fingerprint(
    const input::allocation_t &allocation
  );

  struct gpu_device_t {
    std::filesystem::path path;
    character_device_identity_t admitted_identity;

    bool operator==(const gpu_device_t &) const = default;
  };

  struct gpu_t {
    std::string logical_gpu_id;
    std::filesystem::path render_node;
    /** Exact immutable path/identity pairs admitted by the trusted catalog. */
    std::vector<gpu_device_t> devices;
    std::uint32_t max_encoder_sessions = 1;
  };

  struct profile_t {
    std::string profile_key;
    std::string opaque_volume_name;
    runtime_profile_e runtime_profile = runtime_profile_e::unknown;
    std::string image_reference;
  };

  struct shared_game_mount_t {
    std::string mount_name;
    std::filesystem::path host_path;
  };

  struct options_t {
    std::filesystem::path executable {"/usr/bin/podman"};
    std::string deployment_id;
    std::filesystem::path worker_entrypoint {"/usr/bin/polaris-seat-worker"};
    std::filesystem::path ipc_root;
    std::vector<gpu_t> gpus;
    std::vector<profile_t> profiles;
    std::vector<workload_plan_t> workloads;
    std::vector<shared_game_mount_t> shared_game_mounts;
    std::chrono::milliseconds command_timeout {5000};
    std::size_t max_command_output_bytes = 1024 * 1024;
    std::size_t max_inventory_workers = 64;
    std::uint32_t pids_limit = 4096;
    std::uint64_t shared_memory_bytes = 1024ULL * 1024ULL * 1024ULL;
    std::uint64_t runtime_tmpfs_bytes = 64ULL * 1024ULL * 1024ULL;
    std::uint64_t temporary_tmpfs_bytes = 1024ULL * 1024ULL * 1024ULL;
    std::uint64_t log_size_bytes = 8ULL * 1024ULL * 1024ULL;
    std::chrono::milliseconds health_interval {2000};
    std::chrono::milliseconds health_timeout {1000};
    std::chrono::milliseconds health_start_period {30000};
    std::uint32_t health_retries = 15;
    std::uint32_t health_log_count = 5;
    std::uint32_t health_log_size = 1024;
  };

  /**
   * Rootless, shell-free Podman implementation of the worker backend.
   *
   * It never uses `--privileged`, host networking, a host PID/IPC/UTS
   * namespace, a wildcard device, or a raw Docker-compatible socket. Profile
   * homes are pre-created opaque named volumes; game payloads are read-only
   * bind mounts; all devices are explicit allowlisted character devices.
   */
  class backend_t final : public worker_backend_t {
  public:
    backend_t(
      host_t &host,
      input_manifest_source_t &input_manifests,
      options_t options
    );

    worker_command_result_e launch(const worker_launch_spec_t &spec) override;
    worker_command_result_e stop(
      const worker_identity_t &identity,
      worker_stop_mode_e mode
    ) override;
    std::vector<worker_observation_t> inventory() override;

  private:
    struct container_record_t {
      std::string container_id;
      worker_observation_t observation;
      std::string runtime_state;
      std::vector<std::pair<std::string, std::string>> labels;
      bool input_binding_authoritative = false;
    };

    struct runtime_device_binding_t {
      std::filesystem::path host_path;
      std::filesystem::path worker_path;
      character_device_identity_t identity;
    };

    [[nodiscard]] const gpu_t *gpu_for(const worker_launch_spec_t &spec) const;
    [[nodiscard]] const profile_t *profile_for(const std::string &profile_key) const;
    [[nodiscard]] bool workload_allowed(const workload_plan_t &workload) const;
    [[nodiscard]] bool base_host_ready() const;
    [[nodiscard]] bool gpu_catalog_current() const;
    /** True/false when the profile volume is present/absent; empty on error. */
    [[nodiscard]] std::optional<bool> profile_volume_exists(const profile_t &profile);
    [[nodiscard]] bool launch_host_ready(
      const worker_launch_spec_t &spec,
      const input::allocation_t &input_allocation
    ) const;
    [[nodiscard]] bool valid_spec(const worker_launch_spec_t &spec) const;
    [[nodiscard]] std::optional<input::allocation_t> input_allocation_for(
      const seat_handle_t &handle,
      std::string_view input_seat
    ) const;
    [[nodiscard]] bool input_allocation_current(
      const input::allocation_t &allocation
    ) const;
    [[nodiscard]] bool inspected_bindings_match(
      const std::vector<runtime_device_binding_t> &bindings,
      const gpu_t &gpu,
      const input::allocation_t &input_allocation
    ) const;
    [[nodiscard]] std::vector<std::string> launch_argv(
      const worker_launch_spec_t &spec,
      const gpu_t &gpu,
      const profile_t &profile,
      const input::allocation_t &input_allocation,
      std::string_view input_fingerprint
    ) const;
    [[nodiscard]] std::vector<container_record_t> inventory_records(
      bool require_input_authority = true
    );
    [[nodiscard]] bool record_matches_spec(
      const container_record_t &record,
      const worker_launch_spec_t &spec
    ) const;

    host_t &host_;
    input_manifest_source_t &input_manifests_;
    const options_t options_;
  };

}  // namespace multiseat::podman

#endif
