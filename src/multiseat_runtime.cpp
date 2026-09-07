/**
 * @file src/multiseat_runtime.cpp
 * @brief Admission and lifecycle contract for independent Polaris seats.
 */
#include "multiseat_runtime.h"

#include <algorithm>
#include <stdexcept>
#include <string_view>

namespace multiseat {
  namespace {
    bool concrete_compositor(compositor_e compositor) {
      switch (compositor) {
        case compositor_e::gamescope:
        case compositor_e::sway:
        case compositor_e::labwc:
          return true;
        case compositor_e::automatic:
          return false;
      }
      return false;
    }

    bool concrete_runtime_profile(runtime_profile_e profile) {
      switch (profile) {
        case runtime_profile_e::gamescope:
        case runtime_profile_e::steam:
        case runtime_profile_e::heroic:
        case runtime_profile_e::lutris:
          return true;
        case runtime_profile_e::unknown:
          return false;
      }
      return false;
    }

    bool valid_display_mode(const seat_display_mode_t &mode) {
      constexpr std::uint32_t maximum_dimension = 16384;
      constexpr std::uint32_t minimum_refresh_millihz = 1000;
      constexpr std::uint32_t maximum_refresh_millihz = 1000000;
      return mode.width > 0 && mode.width <= maximum_dimension &&
             mode.height > 0 && mode.height <= maximum_dimension &&
             mode.refresh_millihz >= minimum_refresh_millihz &&
             mode.refresh_millihz <= maximum_refresh_millihz;
    }

    bool ascii_alphanumeric(char value) {
      return (value >= 'a' && value <= 'z') ||
             (value >= 'A' && value <= 'Z') ||
             (value >= '0' && value <= '9');
    }

    bool opaque_name_token(std::string_view value, std::size_t max_size = 128) {
      return !value.empty() &&
             value.size() <= max_size &&
             ascii_alphanumeric(value.front()) &&
             std::all_of(
               value.begin(),
               value.end(),
               [](char character) {
                 return ascii_alphanumeric(character) ||
                        character == '-' ||
                        character == '_' ||
                        character == '.';
               }
             );
    }

    bool valid_controller_epoch(std::string_view epoch) {
      return !epoch.empty() &&
             epoch.size() <= 64 &&
             ascii_alphanumeric(epoch.front()) &&
             std::all_of(
               epoch.begin(),
               epoch.end(),
               [](char value) {
                 return ascii_alphanumeric(value) || value == '-' || value == '_';
               }
             );
    }

    seat_resources_t resources_for(std::string_view controller_epoch, std::uint64_t generation) {
      const auto suffix = std::string {controller_epoch} + "-" + std::to_string(generation);
      return seat_resources_t {
        .worker_name = "polaris-worker-" + suffix,
        .runtime_namespace = "polaris-runtime-" + suffix,
        .capture_wayland_socket = "polaris-capture-" + suffix,
        .wayland_socket = "polaris-wayland-" + suffix,
        .audio_sink = "polaris-audio-" + suffix,
        .input_seat = "polaris-input-" + suffix,
      };
    }

    bool valid_request(const seat_request_t &request) {
      return !request.client_key.empty() &&
             !request.profile_key.empty() &&
             valid_workload_plan(request.workload) &&
             !request.logical_gpu_id.empty() &&
             concrete_runtime_profile(request.runtime_profile) &&
             workload_matches_runtime_profile(request.workload, request.runtime_profile) &&
             valid_data_plane(request.data_plane) &&
             valid_display_mode(request.display_mode) &&
             request.encoder_sessions > 0;
    }
  }  // namespace

  bool valid_workload_plan(const workload_plan_t &plan) {
    return plan.kind != workload_kind_e::unknown &&
           opaque_name_token(plan.target_id);
  }

  bool workload_matches_runtime_profile(
    const workload_plan_t &plan,
    runtime_profile_e profile
  ) {
    switch (profile) {
      case runtime_profile_e::gamescope:
        return plan.kind == workload_kind_e::gamescope;
      case runtime_profile_e::steam:
        return plan.kind == workload_kind_e::steam;
      case runtime_profile_e::heroic:
        return plan.kind == workload_kind_e::heroic;
      case runtime_profile_e::lutris:
        return plan.kind == workload_kind_e::lutris;
      case runtime_profile_e::unknown:
        return false;
    }
    return false;
  }

  bool valid_data_plane(const seat_data_plane_t &data_plane) {
    return data_plane.display_topology ==
             display_topology_e::capture_host_with_nested_compositor &&
           data_plane.media_pipeline ==
             media_pipeline_e::worker_local_capture_encode;
  }

  registry_t::registry_t(
    std::string controller_epoch,
    std::vector<gpu_capacity_t> gpus
  ) :
      controller_epoch_(std::move(controller_epoch)) {
    if (!valid_controller_epoch(controller_epoch_)) {
      throw std::invalid_argument {"multiseat controller epoch must be an opaque name token"};
    }
    if (gpus.empty()) {
      throw std::invalid_argument {"multiseat registry requires at least one GPU"};
    }

    for (auto &gpu : gpus) {
      if (gpu.logical_gpu_id.empty() ||
          gpu.render_node.empty() ||
          gpu.max_seats == 0 ||
          gpu.max_encoder_sessions == 0) {
        throw std::invalid_argument {"multiseat GPU capacity is incomplete"};
      }

      gpu_state_t state {
        .capacity = std::move(gpu),
        .slots = {},
        .encoder_sessions = 0,
      };
      state.slots.resize(state.capacity.max_seats);
      const auto key = state.capacity.logical_gpu_id;
      if (!gpus_.emplace(key, std::move(state)).second) {
        throw std::invalid_argument {"multiseat GPU ids must be unique"};
      }
    }
  }

  admission_result_t registry_t::admit(const seat_request_t &request) {
    if (!valid_request(request)) {
      return {
        .rejection = admission_rejection_e::invalid_request,
        .seat = std::nullopt,
      };
    }

    std::scoped_lock lock {mutex_};
    for (const auto &gpu_entry : gpus_) {
      const auto &gpu = gpu_entry.second;
      for (const auto &slot : gpu.slots) {
        if (!slot) {
          continue;
        }
        if (slot->snapshot.client_key == request.client_key) {
          return {
            .rejection = admission_rejection_e::client_already_active,
            .seat = std::nullopt,
          };
        }
        if (slot->snapshot.profile_key == request.profile_key) {
          return {
            .rejection = admission_rejection_e::profile_already_active,
            .seat = std::nullopt,
          };
        }
      }
    }

    const auto gpu_it = gpus_.find(request.logical_gpu_id);
    if (gpu_it == gpus_.end()) {
      return {
        .rejection = admission_rejection_e::unknown_gpu,
        .seat = std::nullopt,
      };
    }

    auto &gpu = gpu_it->second;
    const auto free_slot = std::find_if(
      gpu.slots.begin(),
      gpu.slots.end(),
      [](const auto &slot) {
        return !slot.has_value();
      }
    );
    if (free_slot == gpu.slots.end()) {
      return {
        .rejection = admission_rejection_e::seat_capacity_reached,
        .seat = std::nullopt,
      };
    }

    if (request.encoder_sessions > gpu.capacity.max_encoder_sessions ||
        gpu.encoder_sessions >
          gpu.capacity.max_encoder_sessions - request.encoder_sessions) {
      return {
        .rejection = admission_rejection_e::encoder_capacity_reached,
        .seat = std::nullopt,
      };
    }

    auto generation = next_generation_++;
    if (generation == 0) {
      generation = next_generation_++;
    }
    if (generation == 0) {
      throw std::overflow_error {"multiseat generation space exhausted"};
    }

    const auto slot_index = static_cast<std::uint32_t>(
      std::distance(gpu.slots.begin(), free_slot)
    );
    seat_snapshot_t snapshot {
      .handle = {
        .controller_epoch = controller_epoch_,
        .logical_gpu_id = request.logical_gpu_id,
        .slot = slot_index,
        .generation = generation,
      },
      .resources = resources_for(controller_epoch_, generation),
      .client_key = request.client_key,
      .profile_key = request.profile_key,
      .workload = request.workload,
      .render_node = gpu.capacity.render_node,
      .runtime_profile = request.runtime_profile,
      .data_plane = request.data_plane,
      .display_mode = request.display_mode,
      .requested_compositor = request.requested_compositor,
      .selected_compositor = compositor_e::automatic,
      .selection_reason = {},
      .state = seat_state_e::reserved,
      .encoder_sessions = request.encoder_sessions,
    };

    *free_slot = seat_record_t {snapshot};
    gpu.encoder_sessions += request.encoder_sessions;
    return {
      .rejection = admission_rejection_e::none,
      .seat = std::move(snapshot),
    };
  }

  mutation_result_e registry_t::bind_runtime(
    const seat_handle_t &handle,
    compositor_e selected,
    std::string selection_reason
  ) {
    std::scoped_lock lock {mutex_};
    mutation_result_e result;
    auto *seat = find_exact_locked(handle, result);
    if (!seat) {
      return result;
    }
    if (seat->snapshot.state != seat_state_e::reserved) {
      return mutation_result_e::invalid_state;
    }
    if (!concrete_compositor(selected) || selection_reason.empty()) {
      return mutation_result_e::invalid_selection;
    }
    if (seat->snapshot.requested_compositor != compositor_e::automatic &&
        seat->snapshot.requested_compositor != selected) {
      return mutation_result_e::invalid_selection;
    }

    seat->snapshot.selected_compositor = selected;
    seat->snapshot.selection_reason = std::move(selection_reason);
    return mutation_result_e::applied;
  }

  mutation_result_e registry_t::mark_starting(const seat_handle_t &handle) {
    std::scoped_lock lock {mutex_};
    mutation_result_e result;
    auto *seat = find_exact_locked(handle, result);
    if (!seat) {
      return result;
    }
    if (seat->snapshot.state != seat_state_e::reserved ||
        !concrete_compositor(seat->snapshot.selected_compositor)) {
      return mutation_result_e::invalid_state;
    }

    seat->snapshot.state = seat_state_e::starting;
    return mutation_result_e::applied;
  }

  mutation_result_e registry_t::mark_running(const seat_handle_t &handle) {
    std::scoped_lock lock {mutex_};
    mutation_result_e result;
    auto *seat = find_exact_locked(handle, result);
    if (!seat) {
      return result;
    }
    if (seat->snapshot.state != seat_state_e::starting) {
      return mutation_result_e::invalid_state;
    }

    seat->snapshot.state = seat_state_e::running;
    return mutation_result_e::applied;
  }

  mutation_result_e registry_t::begin_stop(const seat_handle_t &handle) {
    std::scoped_lock lock {mutex_};
    mutation_result_e result;
    auto *seat = find_exact_locked(handle, result);
    if (!seat) {
      return result;
    }
    if (seat->snapshot.state == seat_state_e::stopping) {
      return mutation_result_e::applied;
    }

    seat->snapshot.state = seat_state_e::stopping;
    return mutation_result_e::applied;
  }

  mutation_result_e registry_t::release(const seat_handle_t &handle) {
    std::scoped_lock lock {mutex_};
    mutation_result_e result;
    auto *seat = find_exact_locked(handle, result);
    if (!seat) {
      return result;
    }
    if (seat->snapshot.state != seat_state_e::stopping) {
      return mutation_result_e::invalid_state;
    }

    auto &gpu = gpus_.at(handle.logical_gpu_id);
    gpu.encoder_sessions -= seat->snapshot.encoder_sessions;
    gpu.slots.at(handle.slot).reset();
    return mutation_result_e::applied;
  }

  std::optional<seat_snapshot_t> registry_t::snapshot(const seat_handle_t &handle) const {
    std::scoped_lock lock {mutex_};
    mutation_result_e result;
    const auto *seat = find_exact_locked(handle, result);
    if (!seat) {
      return std::nullopt;
    }
    return seat->snapshot;
  }

  std::vector<seat_snapshot_t> registry_t::seats() const {
    std::scoped_lock lock {mutex_};
    std::vector<seat_snapshot_t> result;
    for (const auto &gpu_entry : gpus_) {
      const auto &gpu = gpu_entry.second;
      for (const auto &slot : gpu.slots) {
        if (slot) {
          result.push_back(slot->snapshot);
        }
      }
    }
    std::sort(
      result.begin(),
      result.end(),
      [](const auto &left, const auto &right) {
        return left.handle.generation < right.handle.generation;
      }
    );
    return result;
  }

  std::optional<gpu_usage_t> registry_t::gpu_usage(const std::string &logical_gpu_id) const {
    std::scoped_lock lock {mutex_};
    const auto gpu_it = gpus_.find(logical_gpu_id);
    if (gpu_it == gpus_.end()) {
      return std::nullopt;
    }

    const auto &gpu = gpu_it->second;
    return gpu_usage_t {
      .active_seats = static_cast<std::uint32_t>(std::count_if(
        gpu.slots.begin(),
        gpu.slots.end(),
        [](const auto &slot) {
          return slot.has_value();
        }
      )),
      .encoder_sessions = gpu.encoder_sessions,
      .max_seats = gpu.capacity.max_seats,
      .max_encoder_sessions = gpu.capacity.max_encoder_sessions,
    };
  }

  registry_t::seat_record_t *registry_t::find_exact_locked(
    const seat_handle_t &handle,
    mutation_result_e &result
  ) {
    if (handle.controller_epoch != controller_epoch_) {
      result = mutation_result_e::stale_controller;
      return nullptr;
    }
    const auto gpu_it = gpus_.find(handle.logical_gpu_id);
    if (gpu_it == gpus_.end() || handle.slot >= gpu_it->second.slots.size()) {
      result = mutation_result_e::not_found;
      return nullptr;
    }

    auto &slot = gpu_it->second.slots[handle.slot];
    if (!slot) {
      result = mutation_result_e::not_found;
      return nullptr;
    }
    if (slot->snapshot.handle.generation != handle.generation) {
      result = mutation_result_e::stale_generation;
      return nullptr;
    }

    result = mutation_result_e::applied;
    return &*slot;
  }

  const registry_t::seat_record_t *registry_t::find_exact_locked(
    const seat_handle_t &handle,
    mutation_result_e &result
  ) const {
    if (handle.controller_epoch != controller_epoch_) {
      result = mutation_result_e::stale_controller;
      return nullptr;
    }
    const auto gpu_it = gpus_.find(handle.logical_gpu_id);
    if (gpu_it == gpus_.end() || handle.slot >= gpu_it->second.slots.size()) {
      result = mutation_result_e::not_found;
      return nullptr;
    }

    const auto &slot = gpu_it->second.slots[handle.slot];
    if (!slot) {
      result = mutation_result_e::not_found;
      return nullptr;
    }
    if (slot->snapshot.handle.generation != handle.generation) {
      result = mutation_result_e::stale_generation;
      return nullptr;
    }

    result = mutation_result_e::applied;
    return &*slot;
  }

}  // namespace multiseat
