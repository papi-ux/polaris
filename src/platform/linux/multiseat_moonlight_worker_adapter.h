/**
 * @file src/platform/linux/multiseat_moonlight_worker_adapter.h
 * @brief Authenticated worker-to-Moonlight launch selection adapter.
 */
#pragma once

#ifdef __linux__

  #include "multiseat_moonlight_runtime.h"
  #include "multiseat_worker_launch_authority.h"

  #include <memory>
  #include <optional>

namespace rtsp_stream {
  struct launch_session_t;
}

namespace multiseat::input {

  enum class moonlight_worker_selection_status_e {
    selected,
    invalid_launch,
    worker_not_authorized,
    authority_mismatch,
    client_mismatch,
    runtime_unavailable,
    selection_rejected,
  };

  struct moonlight_worker_selection_result_t {
    moonlight_worker_selection_status_e status =
      moonlight_worker_selection_status_e::invalid_launch;
    worker_seat_authorization_status_e authority_status =
      worker_seat_authorization_status_e::invalid_request;
    std::optional<moonlight_launch_selection_status_e> selection_status;

    [[nodiscard]] bool selected() const {
      return status == moonlight_worker_selection_status_e::selected &&
             authority_status == worker_seat_authorization_status_e::applied &&
             selection_status ==
               moonlight_launch_selection_status_e::registered;
    }
  };

  /**
   * Converts exact worker authority into one process-local Moonlight selection.
   *
   * A seat handle alone is never authority. The worker source revalidates the
   * exact running generation while serialized, and the paired-client UUID on
   * the retained authenticated launch must match the seat admission record.
   * Controller feedback is derived from that launch's permissions.
   */
  class moonlight_worker_launch_adapter_t final {
  public:
    explicit moonlight_worker_launch_adapter_t(
      authenticated_worker_seat_authority_t &worker_authority
    );

    [[nodiscard]] moonlight_worker_selection_result_t select(
      const std::shared_ptr<rtsp_stream::launch_session_t> &launch,
      const seat_handle_t &handle
    );

  private:
    authenticated_worker_seat_authority_t &worker_authority_;
  };

}  // namespace multiseat::input

#endif
