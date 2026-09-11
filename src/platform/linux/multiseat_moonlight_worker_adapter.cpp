/**
 * @file src/platform/linux/multiseat_moonlight_worker_adapter.cpp
 * @brief Authenticated worker-to-Moonlight launch selection adapter.
 */
#include "multiseat_moonlight_worker_adapter.h"

#ifdef __linux__

  #include "src/rtsp.h"
  #include "multiseat_worker_launch_connection.h"

namespace multiseat::input {
  namespace {
    bool exact_endpoint(const worker_ipc::controller_connection_t &connection,
      const authenticated_worker_seat_t &seat) {
      return connection.connected() && connection.identity() == worker_ipc::endpoint_identity_t {
        .controller_epoch = seat.handle.controller_epoch,
        .logical_gpu_id = seat.handle.logical_gpu_id,
        .slot = seat.handle.slot,
        .generation = seat.handle.generation,
        .worker_name = seat.worker_name,
      };
    }
  }


  worker_launch_connection_t::worker_launch_connection_t(
    std::shared_ptr<rtsp_stream::launch_session_t> launch,
    authenticated_worker_seat_t seat,
    worker_ipc::controller_connection_t connection
  ):
      launch_(std::move(launch)),
      launch_id_(launch_->id),
      lifecycle_generation_(*launch_->lifecycle_generation),
      seat_(std::move(seat)),
      connection_(std::move(connection)) {
  }

  bool worker_launch_connection_t::matches(
    std::uint32_t launch_id, std::uint64_t lifecycle_generation,
    std::string_view client_key, const seat_handle_t &handle
  ) const {
    return !retired_.load() && !launch_->is_cancelled() &&
           launch_id == launch_id_ && lifecycle_generation == lifecycle_generation_ &&
           launch_->id == launch_id_ && launch_->lifecycle_generation == lifecycle_generation_ &&
           client_key == seat_.client_key && launch_->unique_id == seat_.client_key &&
           handle == seat_.handle && exact_endpoint(connection_, seat_);
  }

  bool worker_launch_connection_t::try_claim(std::uint64_t stream_generation) {
    std::uint64_t unclaimed = 0;
    return stream_generation != 0 && registered_.load() && !retired_.load() &&
           stream_generation_.compare_exchange_strong(unclaimed, stream_generation) &&
           bound_to(stream_generation);
  }

  bool worker_launch_connection_t::try_register() {
    bool unregistered = false;
    return matches_selection(launch_id_, lifecycle_generation_, seat_.handle) &&
           registered_.compare_exchange_strong(unregistered, true);
  }

  bool worker_launch_connection_t::matches_launch(const rtsp_stream::launch_session_t &launch) const {
    return &launch == launch_.get() &&
           matches(launch.id, launch.lifecycle_generation.value_or(0), launch.unique_id, seat_.handle);
  }

  bool worker_launch_connection_t::matches_selection(std::uint32_t launch_id,
    std::uint64_t lifecycle_generation, const seat_handle_t &handle) const {
    return matches(launch_id, lifecycle_generation, seat_.client_key, handle);
  }

  bool worker_launch_connection_t::matches_stream(std::uint32_t launch_id,
    std::uint64_t lifecycle_generation, std::string_view client_key, const seat_handle_t &handle,
    const std::shared_ptr<const std::atomic_bool> &requirement) const {
    return requirement == launch_->worker_connection_requirement() &&
           matches(launch_id, lifecycle_generation, client_key, handle);
  }

  bool worker_launch_connection_t::bound_to(std::uint64_t stream_generation) const {
    return stream_generation != 0 && stream_generation_.load() == stream_generation &&
           matches(launch_id_, lifecycle_generation_, seat_.client_key, seat_.handle);
  }

  worker_ipc::controller_connection_t worker_launch_connection_t::stream_connection(
    const std::uint64_t stream_generation
  ) const {
    if (!registered_.load() || retired_.load() || !bound_to(stream_generation)) {
      return {};
    }
    return connection_;
  }

  void worker_launch_connection_t::retire() noexcept {
    retired_.store(true);
  }

  moonlight_worker_launch_adapter_t::moonlight_worker_launch_adapter_t(
    authenticated_worker_seat_authority_t &worker_authority
  ):
      worker_authority_(worker_authority) {
  }

  moonlight_worker_selection_result_t
  moonlight_worker_launch_adapter_t::select(
    const std::shared_ptr<rtsp_stream::launch_session_t> &launch,
    const seat_handle_t &handle
  ) {
    return select_impl(launch, handle, false);
  }

  moonlight_worker_selection_result_t
  moonlight_worker_launch_adapter_t::select_with_connection(
    const std::shared_ptr<rtsp_stream::launch_session_t> &launch,
    const seat_handle_t &handle
  ) {
    return select_impl(launch, handle, true);
  }

  moonlight_worker_selection_result_t
  moonlight_worker_launch_adapter_t::select_impl(
    const std::shared_ptr<rtsp_stream::launch_session_t> &launch,
    const seat_handle_t &handle,
    bool require_connection
  ) {
    moonlight_worker_selection_result_t result;
    if (!launch || !handle.valid() || launch->id == 0 ||
        !launch->lifecycle_generation ||
        *launch->lifecycle_generation == 0 ||
        launch->unique_id.empty() || !launch->is_pending()) {
      return result;
    }

    if (require_connection) launch->require_worker_connection();

    bool authority_matched = false;
    bool client_matched = false;
    const auto select_authorized = [&](const authenticated_worker_seat_t &seat,
                                      const worker_ipc::controller_connection_t *connection) {
      if (seat.handle != handle || seat.worker_name.empty() ||
          seat.input_seat.empty()) {
        return;
      }
      if (connection && !exact_endpoint(*connection, seat)) {
        return;
      }
      authority_matched = true;
      if (seat.client_key != launch->unique_id) {
        return;
      }
      client_matched = true;
      const auto controller_feedback =
        !!(launch->perm & crypto::PERM::input_controller);
      std::shared_ptr<worker_launch_connection_t> reserved;
      if (connection) {
        reserved.reset(new worker_launch_connection_t(launch, seat, *connection));
      }
      result.selection_status = select_authenticated_moonlight_launch(
        launch,
        seat.handle,
        seat.input_seat,
        controller_feedback,
        {.required = require_connection, .connection = std::move(reserved)}
      );
    };
    result.authority_status = require_connection ?
      worker_authority_.with_authenticated_worker_connection(handle,
        [&](const auto &seat, const auto &connection) { select_authorized(seat, &connection); }) :
      worker_authority_.with_authenticated_worker_seat(handle,
        [&](const auto &seat) { select_authorized(seat, nullptr); });

    if (result.authority_status !=
        worker_seat_authorization_status_e::applied) {
      result.status =
        moonlight_worker_selection_status_e::worker_not_authorized;
      return result;
    }
    if (!authority_matched) {
      result.status = moonlight_worker_selection_status_e::authority_mismatch;
      return result;
    }
    if (!client_matched) {
      result.status = moonlight_worker_selection_status_e::client_mismatch;
      return result;
    }
    if (!result.selection_status) {
      result.status = moonlight_worker_selection_status_e::runtime_unavailable;
      return result;
    }
    if (*result.selection_status !=
        moonlight_launch_selection_status_e::registered) {
      result.status = moonlight_worker_selection_status_e::selection_rejected;
      return result;
    }
    result.status = moonlight_worker_selection_status_e::selected;
    return result;
  }

}  // namespace multiseat::input

#endif
