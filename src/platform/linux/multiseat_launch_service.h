/** @file src/platform/linux/multiseat_launch_service.h
 * @brief Bounded launch requests and reconciliation under one controller owner.
 */
#pragma once
#ifdef __linux__
#include "multiseat_controller_production.h"
#include "multiseat_profile_catalog.h"

#include <chrono>
#include <memory>
#include <stop_token>
#include <string_view>

namespace multiseat {
  inline constexpr int profile_app_id = 1347244801;
  inline constexpr std::string_view profile_app_uuid = "706f6c61-7269-4373-8000-6d756c746973";

  // Every refusal names what happened (message), a stable snake_case code and
  // the one change that fixes it (action). The HTTP layer puts them on the
  // launch response through launch_failure on its own thread; results built on
  // the owner thread only carry the words and never touch launch_failure.
  struct profile_launch_result_t {
    int status = 503;
    std::string_view message = "The Space's runtime is unavailable.";
    std::string_view code;
    std::string_view action;
    [[nodiscard]] bool prepared() const { return status == 200; }
  };
  inline constexpr profile_launch_result_t space_runtime_unavailable_result {
    503, "The Space's runtime did not start.", "space_runtime_unavailable", "Open Spaces in Polaris and check Host Setup."};
  inline constexpr profile_launch_result_t space_start_timeout_result {
    504, "The Space did not start in time.", "space_start_timeout",
    "Try again. If it keeps happening, open Spaces in Polaris and check Host Setup."};
  // The Space's image carries the NVIDIA userspace for one driver, and the host
  // loaded another: NVML and NVENC inside it would fail against the kernel module.
  inline constexpr profile_launch_result_t space_runtime_driver_mismatch_result {
    409, "This Space's gaming runtime was made for a different NVIDIA driver than the host now runs.",
    "space_runtime_driver_mismatch",
    "Open Spaces in Polaris on the host and move the Space to the runtime for this driver. Its Steam sign-in and games stay."};
  // Moving a Space to another runtime refuses in these words, whether the decision is made before
  // the move starts or again inside the catalog transaction.
  inline constexpr profile_launch_result_t space_open_for_move_result {
    409, "This Space is open on a device.", "space_active", "End that stream, then move the Space."};
  inline constexpr profile_launch_result_t spaces_change_running_result {
    409, "Polaris is saving another change to Spaces.", "spaces_change_running", "Try again when it finishes."};
  inline constexpr profile_launch_result_t spaces_streaming_result {
    409, "Stop every Space stream and wait for cleanup before changing Spaces.", "spaces_streaming",
    "End the running Space streams, then try again."};
  inline constexpr profile_launch_result_t space_runtime_changed_result {
    409, "This Space's gaming runtime, or the runtime for this PC's driver, changed since Spaces was loaded.",
    "space_runtime_changed", "Refresh Spaces and try again."};
  inline constexpr profile_launch_result_t space_runtime_profile_mismatch_result {
    409, "The runtime for this driver runs a different kind of Space, so this Space's home would not carry over.",
    "space_runtime_profile_mismatch", "Keep this Space on its runtime, or make a new Space."};
  inline constexpr profile_launch_result_t space_runtime_media_mismatch_result {
    409, "The runtime for this driver streams in a way this Space's runtime does not, so Polaris will not move it.",
    "space_runtime_media_mismatch", "Update Polaris, then try again."};
  inline constexpr profile_launch_result_t space_runtime_identity_mismatch_result {
    409, "The runtime for this driver runs as a different Linux account than the one this Space's home belongs to.",
    "space_runtime_identity_mismatch", "Keep this Space on its runtime. Do not change your Linux user ID."};
  // While an administrator approves a change to this PC's setup from the Spaces page.
  inline constexpr profile_launch_result_t spaces_host_setup_running_result {
    409, "Polaris is changing this PC's Spaces setup.", "spaces_host_setup_running",
    "Try again when Host Setup in Polaris finishes."};
  struct profile_begin_result_t {
    profile_launch_result_t result;
    std::optional<seat_handle_t> seat;
  };
  enum class profile_poll_e { pending, selected, failed };

  // Injectable ownership boundary. Route and catalog queries read immutable
  // snapshots outside the owner thread. All resource operations run on it.
  class profile_controller_t {
  public:
    virtual ~profile_controller_t() = default;
    virtual bool routes_client(std::string_view client) const = 0;
    virtual std::optional<std::string> profile_for_client(std::string_view client) const { return std::nullopt; }
    virtual std::vector<profile_summary_t> profile_catalog() const { return {}; }
    virtual spaces::library_reader_t library_reader() const { return {}; }
    virtual std::vector<std::string> desktop_clients() const { return {}; }
    /// Devices whose Default Space is Desktop.
    virtual std::vector<std::string> desktop_default_clients() const { return {}; }
    virtual std::vector<profile_activity_t> profile_activity() const { return {}; }
    virtual bool idle() const { return false; }
    /// Seat and encoder usage against the trusted budget, when the controller can count it.
    virtual std::optional<gpu_usage_t> capacity() const { return std::nullopt; }
    virtual void reconcile() = 0;
    virtual profile_begin_result_t begin(const std::shared_ptr<rtsp_stream::launch_session_t> &launch) = 0;
    virtual profile_poll_e poll(const std::shared_ptr<rtsp_stream::launch_session_t> &launch,
                               const seat_handle_t &seat) = 0;
    virtual bool shutdown() = 0;
  };

  std::unique_ptr<profile_controller_t> make_profile_controller(std::unique_ptr<controller_runtime_t> runtime);
  [[nodiscard]] std::optional<production_controller_options_t> load_controller_options(const std::filesystem::path &path);

  struct profile_admin_options_t {
    std::filesystem::path catalog;
    std::function<std::unique_ptr<profile_controller_t>()> reload;
    std::function<profiles::change_result_t(std::string_view, std::string_view)> persist;
    std::function<profiles::change_result_t(const profiles::space_create_request_t &)> create;
    std::function<profiles::change_result_t(const profiles::edit_request_t &)> edit;
    std::function<profiles::change_result_t(std::string_view, std::string_view, bool)> access;
    // Deletes a Space's home through Docker; the stop token ends a long removal at shutdown.
    std::function<profiles::removal_result_t(const profiles::edit_request_t &, std::stop_token)> remove_for_good;
    // Points one Space at another runtime image; runs on the owner thread once the controller closed.
    std::function<profiles::runtime_move_result_t(const profiles::runtime_move_t &, std::stop_token)> move_runtime;
    // False only when a Space's image is proven to be built for an NVIDIA driver the host no longer
    // runs. Checked before a launch starts anything; an answer it cannot get lets the launch go on.
    std::function<bool(std::string_view image)> runtime_matches_host;
  };
  struct profile_admin_snapshot_t {
    bool available = false, changing = false, failed = false;
    std::vector<profile_summary_t> profiles;
    bool creation_available = false, management_available = false;
    std::vector<std::string> desktop_clients;
    std::vector<profile_activity_t> activity;
    std::optional<gpu_usage_t> capacity;
    bool removal_available = false;  ///< a Space can be removed for good, not only archived
    std::vector<std::string> desktop_default_clients;  ///< devices whose Default Space is Desktop
    bool runtime_move_available = false;  ///< a Space can be moved to another gaming runtime
  };
  struct profile_session_snapshot_t {
    bool active = false;
    std::string token;
    int width = 0, height = 0, fps = 0;
    std::string game_identity, game_name;
  };

  struct profile_client_space_t {
    std::string id, name, state;
    bool selected = false;
    bool library_enabled = false;
    bool can_open = false;  ///< this device could open it right now, the host's capacity included
    std::string blocked_reason;  ///< when not: unavailable | in_use | starting | running | stopping | at_capacity
  };
  // What a device may see, and why what it cannot do is off. Every reason is a
  // stable snake_case word a client can key copy on; the six state words stay.
  struct profile_client_spaces_t {
    bool available = false, can_switch = false;
    std::string selected;
    std::vector<profile_client_space_t> spaces;
    bool desktop_allowed = false;
    std::string unavailable_reason;  ///< controller_missing | stopping | reconfiguring | admin_failed | selection_failed | no_space_assigned
    std::string switch_blocked_reason;  ///< your_stream | desktop_stream
    std::string default_space;  ///< where the device opens first: a Space id, "desktop" for a Desktop default, empty when none
    std::optional<gpu_usage_t> capacity;
  };

  struct profile_library_snapshot_t {
    std::string id, name;
    spaces::library_t library;
  };
  // A removal for good says what it could not delete and where it still is.
  struct profile_removal_result_t {
    profile_launch_result_t result;
    std::string kept_volume, kept_network;
  };
  class profile_launch_service_t final {
  public:
    explicit profile_launch_service_t(std::unique_ptr<profile_controller_t> controller,
      std::chrono::milliseconds timeout = std::chrono::seconds(25), profile_admin_options_t admin = {});
    ~profile_launch_service_t();
    profile_launch_service_t(const profile_launch_service_t &) = delete;
    profile_launch_service_t &operator=(const profile_launch_service_t &) = delete;
    [[nodiscard]] bool routes_client(std::string_view client) const;
    [[nodiscard]] bool track_host_launch(const std::shared_ptr<rtsp_stream::launch_session_t> &launch);
    [[nodiscard]] std::optional<std::string> profile_for_client(std::string_view client) const;
    // Only the assigned profile name, read from one controller snapshot.
    [[nodiscard]] std::optional<std::string> profile_name_for_client(std::string_view client) const;
    [[nodiscard]] profile_launch_result_t prepare(const std::shared_ptr<rtsp_stream::launch_session_t> &launch,
      std::string_view expected_profile = {}, std::string_view target = {});
    [[nodiscard]] std::optional<profile_library_snapshot_t> library_for_client(
      std::string_view client, std::string_view profile) const;
    [[nodiscard]] profile_admin_snapshot_t admin_snapshot() const;
    [[nodiscard]] profile_launch_result_t set_assignment(std::string profile, std::string client);
    [[nodiscard]] profile_launch_result_t set_access(std::string profile, std::string client, bool allowed);
    [[nodiscard]] profile_client_spaces_t client_spaces(std::string_view client) const;
    [[nodiscard]] profile_launch_result_t select_space(std::string_view client, std::string_view profile,
      std::string_view previous);
    [[nodiscard]] profile_launch_result_t create_space_profile(profiles::space_create_request_t request);
    [[nodiscard]] profile_launch_result_t edit_profile(profiles::edit_request_t request);
    // Deletes a Space's games and saves and its record. Refused while that Space
    // or any Space stream is active, when the typed name is not the Space's name,
    // and for the last Steam Space. A finished request answers its own retry.
    [[nodiscard]] profile_removal_result_t remove_space_for_good(profiles::edit_request_t request);
    // Points one Space at another runtime image and keeps everything else it has. Refused while
    // that Space or any Space stream is active or another change is being saved. The same move
    // asked again joins the one running, and after it finished it is confirmed as already moved.
    [[nodiscard]] profile_launch_result_t move_space_runtime(profiles::runtime_move_t move);
    // Cancellation only marks launches. Docker and input teardown remain on the
    // owner thread. Empty tokens allow an authenticated owner to cancel itself.
    [[nodiscard]] bool cancel_client(std::string_view client, std::string_view token = {});
    [[nodiscard]] std::optional<std::string> session_token(std::string_view client) const;
    /// A launch of this device's is admitted but has not started streaming yet.
    [[nodiscard]] bool session_starting(std::string_view client) const;
    [[nodiscard]] profile_session_snapshot_t session_snapshot(std::string_view client) const;
    void stop_admission();
    [[nodiscard]] bool shutdown(std::chrono::milliseconds timeout);
  private:
    struct impl_t;
    std::unique_ptr<impl_t> impl_;
  };

  [[nodiscard]] bool install_profile_launch_service(const std::shared_ptr<profile_launch_service_t> &service);
  void uninstall_profile_launch_service(const std::shared_ptr<profile_launch_service_t> &service);
  [[nodiscard]] std::shared_ptr<profile_launch_service_t> profile_service_for(std::string_view client);
  [[nodiscard]] std::shared_ptr<profile_launch_service_t> installed_profile_service();
}  // namespace multiseat
#endif
