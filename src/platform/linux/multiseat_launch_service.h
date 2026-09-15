/** @file src/platform/linux/multiseat_launch_service.h
 * @brief Bounded launch requests and reconciliation under one controller owner.
 */
#pragma once
#ifdef __linux__
#include "multiseat_controller_production.h"
#include "multiseat_profile_catalog.h"

#include <chrono>
#include <memory>
#include <string_view>

namespace multiseat {
  inline constexpr int profile_app_id = 1347244801;
  inline constexpr std::string_view profile_app_uuid = "706f6c61-7269-4373-8000-6d756c746973";

  struct profile_launch_result_t {
    int status = 503;
    std::string_view message = "The profile runtime is unavailable";
    [[nodiscard]] bool prepared() const { return status == 200; }
  };
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
    virtual std::vector<profile_activity_t> profile_activity() const { return {}; }
    virtual bool idle() const { return false; }
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
    std::function<profiles::change_result_t(const profiles::steam_create_request_t &)> create;
    std::function<profiles::change_result_t(const profiles::edit_request_t &)> edit;
    std::function<profiles::change_result_t(std::string_view, std::string_view, bool)> access;
  };
  struct profile_admin_snapshot_t {
    bool available = false, changing = false, failed = false;
    std::vector<profile_summary_t> profiles;
    bool creation_available = false, management_available = false;
    std::vector<std::string> desktop_clients;
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
  };
  struct profile_client_spaces_t {
    bool available = false, can_switch = false;
    std::string selected;
    std::vector<profile_client_space_t> spaces;
    bool desktop_allowed = false;
  };

  struct profile_library_snapshot_t {
    std::string id, name;
    spaces::library_t library;
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
    [[nodiscard]] profile_launch_result_t create_steam_profile(profiles::steam_create_request_t request);
    [[nodiscard]] profile_launch_result_t edit_profile(profiles::edit_request_t request);
    // Cancellation only marks launches. Docker and input teardown remain on the
    // owner thread. Empty tokens allow an authenticated owner to cancel itself.
    [[nodiscard]] bool cancel_client(std::string_view client, std::string_view token = {});
    [[nodiscard]] std::optional<std::string> session_token(std::string_view client) const;
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
