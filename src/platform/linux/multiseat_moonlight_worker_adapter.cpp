/**
 * @file src/platform/linux/multiseat_moonlight_worker_adapter.cpp
 * @brief Authenticated worker-to-Moonlight launch selection adapter.
 */
#include "multiseat_moonlight_worker_adapter.h"

#ifdef __linux__

  #include "src/rtsp.h"

namespace multiseat::input {

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
    moonlight_worker_selection_result_t result;
    if (!launch || !handle.valid() || launch->id == 0 ||
        !launch->lifecycle_generation ||
        *launch->lifecycle_generation == 0 ||
        launch->unique_id.empty() || !launch->is_pending()) {
      return result;
    }

    bool authority_matched = false;
    bool client_matched = false;
    result.authority_status =
      worker_authority_.with_authenticated_worker_seat(
        handle,
        [&](const authenticated_worker_seat_t &seat) {
          if (seat.handle != handle || seat.worker_name.empty() ||
              seat.input_seat.empty()) {
            return;
          }
          authority_matched = true;
          if (seat.client_key != launch->unique_id) {
            return;
          }
          client_matched = true;
          const auto controller_feedback =
            !!(launch->perm & crypto::PERM::input_controller);
          result.selection_status = select_authenticated_moonlight_launch(
            launch,
            seat.handle,
            seat.input_seat,
            controller_feedback
          );
        }
      );

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
