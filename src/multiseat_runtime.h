/**
 * @file src/multiseat_runtime.h
 * @brief Admission and lifecycle contract for independent Polaris seats.
 */
#pragma once

#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace multiseat {

  enum class compositor_e {
    automatic,
    gamescope,
    sway,
    labwc,
  };

  enum class seat_state_e {
    reserved,
    starting,
    running,
    stopping,
  };

  enum class admission_rejection_e {
    none,
    invalid_request,
    unknown_gpu,
    client_already_active,
    profile_already_active,
    seat_capacity_reached,
    encoder_capacity_reached,
  };

  enum class mutation_result_e {
    applied,
    not_found,
    stale_controller,
    stale_generation,
    invalid_state,
    invalid_selection,
  };

  struct gpu_capacity_t {
    std::string logical_gpu_id;
    std::string render_node;
    std::uint32_t max_seats = 1;
    std::uint32_t max_encoder_sessions = 1;
  };

  struct seat_request_t {
    std::string client_key;
    std::string profile_key;
    std::string workload_key;
    std::string logical_gpu_id;
    compositor_e requested_compositor = compositor_e::automatic;
    std::uint32_t encoder_sessions = 1;
  };

  /**
   * Exact authority for one occupied GPU slot.
   *
   * A slot may be reused after teardown, and a control plane may restart. The
   * controller epoch and generation make an old handle incapable of mutating
   * either replacement.
   */
  struct seat_handle_t {
    std::string controller_epoch;
    std::string logical_gpu_id;
    std::uint32_t slot = 0;
    std::uint64_t generation = 0;

    [[nodiscard]] bool valid() const {
      return !controller_epoch.empty() && !logical_gpu_id.empty() && generation != 0;
    }

    bool operator==(const seat_handle_t &) const = default;
  };

  /**
   * Names handed to a future worker/container backend.
   *
   * They intentionally contain no client, profile, application, host, or
   * account text. Public process lists and runtime paths must not become an
   * accidental identity channel.
   */
  struct seat_resources_t {
    std::string worker_name;
    std::string runtime_namespace;
    std::string wayland_socket;
    std::string audio_sink;
    std::string input_seat;

    bool operator==(const seat_resources_t &) const = default;
  };

  struct seat_snapshot_t {
    seat_handle_t handle;
    seat_resources_t resources;
    std::string client_key;
    std::string profile_key;
    std::string workload_key;
    std::string render_node;
    compositor_e requested_compositor = compositor_e::automatic;
    compositor_e selected_compositor = compositor_e::automatic;
    std::string selection_reason;
    seat_state_e state = seat_state_e::reserved;
    std::uint32_t encoder_sessions = 1;
  };

  struct admission_result_t {
    admission_rejection_e rejection = admission_rejection_e::invalid_request;
    std::optional<seat_snapshot_t> seat;

    [[nodiscard]] bool accepted() const {
      return rejection == admission_rejection_e::none && seat.has_value();
    }
  };

  struct gpu_usage_t {
    std::uint32_t active_seats = 0;
    std::uint32_t encoder_sessions = 0;
    std::uint32_t max_seats = 0;
    std::uint32_t max_encoder_sessions = 0;

    bool operator==(const gpu_usage_t &) const = default;
  };

  /**
   * Thread-safe seat admission and exact-generation lifecycle registry.
   *
   * This is deliberately independent of Docker, Podman, Gamescope, and the
   * current proc_t singleton. It defines the boundary those implementations
   * must obey before Polaris can safely run more than one workload.
   */
  class registry_t {
  public:
    explicit registry_t(
      std::string controller_epoch,
      std::vector<gpu_capacity_t> gpus
    );

    admission_result_t admit(const seat_request_t &request);

    mutation_result_e bind_runtime(
      const seat_handle_t &handle,
      compositor_e selected,
      std::string selection_reason
    );
    mutation_result_e mark_starting(const seat_handle_t &handle);
    mutation_result_e mark_running(const seat_handle_t &handle);
    mutation_result_e begin_stop(const seat_handle_t &handle);
    mutation_result_e release(const seat_handle_t &handle);

    [[nodiscard]] std::optional<seat_snapshot_t> snapshot(const seat_handle_t &handle) const;
    [[nodiscard]] std::vector<seat_snapshot_t> seats() const;
    [[nodiscard]] std::optional<gpu_usage_t> gpu_usage(const std::string &logical_gpu_id) const;

  private:
    struct seat_record_t {
      seat_snapshot_t snapshot;
    };

    struct gpu_state_t {
      gpu_capacity_t capacity;
      std::vector<std::optional<seat_record_t>> slots;
      std::uint32_t encoder_sessions = 0;
    };

    seat_record_t *find_exact_locked(
      const seat_handle_t &handle,
      mutation_result_e &result
    );
    const seat_record_t *find_exact_locked(
      const seat_handle_t &handle,
      mutation_result_e &result
    ) const;

    const std::string controller_epoch_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, gpu_state_t> gpus_;
    std::uint64_t next_generation_ = 1;
  };

}  // namespace multiseat
