/**
 * @file src/platform/linux/multiseat_worker_launch_authority.h
 * @brief Narrow authenticated-worker authority for launch adapters.
 */
#pragma once

#ifdef __linux__

  #include "src/multiseat_runtime.h"
  #include "multiseat_worker_client.h"

  #include <functional>
  #include <string>

namespace multiseat {

  /** Non-secret projection exposed only during one serialized authorization. */
  struct authenticated_worker_seat_t {
    seat_handle_t handle;
    std::string worker_name;
    std::string input_seat;
    std::string client_key;

    bool operator==(const authenticated_worker_seat_t &) const = default;
  };

  enum class worker_seat_authorization_status_e {
    applied,
    invalid_request,
    reconciliation_required,
    seat_not_found,
    seat_not_running,
    worker_not_managed,
    authority_rejected,
    endpoint_not_authenticated,
    action_failed,
  };

  using authenticated_worker_seat_action_t =
    std::function<void(const authenticated_worker_seat_t &)>;

  using authenticated_worker_connection_action_t = std::function<void(
    const authenticated_worker_seat_t &,
    const worker_ipc::controller_connection_t &
  )>;

  /**
   * Executes one action while an exact running worker remains serialized.
   *
   * The projection is evidence, not a bearer token. Implementations must
   * revalidate their private authority and authenticated transport before
   * invoking the action. The action must not re-enter the authority and must
   * not retain a reference to the projection after it returns.
   */
  class authenticated_worker_seat_authority_t {
  public:
    virtual ~authenticated_worker_seat_authority_t() = default;

    [[nodiscard]] virtual worker_seat_authorization_status_e
    with_authenticated_worker_seat(
      const seat_handle_t &handle,
      const authenticated_worker_seat_action_t &action
    ) = 0;

    /**
     * Supply an immutable lease while the same seat authorization is held.
     * Metadata-only authorities cannot grant transport access. The action may
     * retain a lease copy, but must not block on I/O or re-enter this authority.
     * A retained lease retires with its original connection; it does not grant
     * a launch claim, a stream generation, or network delivery permission.
     */
    [[nodiscard]] virtual worker_seat_authorization_status_e
    with_authenticated_worker_connection(
      const seat_handle_t &handle,
      const authenticated_worker_connection_action_t &action
    ) {
      return !handle.valid() || !action ?
        worker_seat_authorization_status_e::invalid_request :
        worker_seat_authorization_status_e::endpoint_not_authenticated;
    }
  };

}  // namespace multiseat

#endif
