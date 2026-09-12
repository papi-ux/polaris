/** @file src/platform/linux/multiseat_launch_service.h
 * @brief Bounded launch requests and reconciliation under one controller owner.
 */
#pragma once
#ifdef __linux__
#include "multiseat_controller_production.h"

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

  // Injectable ownership boundary. Only routes_client reads immutable state
  // outside the owner thread. All resource operations run on that one thread.
  class profile_controller_t {
  public:
    virtual ~profile_controller_t() = default;
    virtual bool routes_client(std::string_view client) const = 0;
    virtual void reconcile() = 0;
    virtual profile_begin_result_t begin(const std::shared_ptr<rtsp_stream::launch_session_t> &launch) = 0;
    virtual profile_poll_e poll(const std::shared_ptr<rtsp_stream::launch_session_t> &launch,
                               const seat_handle_t &seat) = 0;
    virtual bool shutdown() = 0;
  };

  std::unique_ptr<profile_controller_t> make_profile_controller(std::unique_ptr<controller_runtime_t> runtime);
  [[nodiscard]] std::optional<production_controller_options_t> load_controller_options(const std::filesystem::path &path);

  class profile_launch_service_t final {
  public:
    explicit profile_launch_service_t(std::unique_ptr<profile_controller_t> controller,
      std::chrono::milliseconds timeout = std::chrono::seconds(25));
    ~profile_launch_service_t();
    profile_launch_service_t(const profile_launch_service_t &) = delete;
    profile_launch_service_t &operator=(const profile_launch_service_t &) = delete;
    [[nodiscard]] bool routes_client(std::string_view client) const;
    [[nodiscard]] profile_launch_result_t prepare(const std::shared_ptr<rtsp_stream::launch_session_t> &launch);
    // Cancellation only marks launches. Docker and input teardown remain on the
    // owner thread. Empty tokens allow an authenticated owner to cancel itself.
    [[nodiscard]] bool cancel_client(std::string_view client, std::string_view token = {});
    [[nodiscard]] std::optional<std::string> session_token(std::string_view client) const;
    void stop_admission();
    [[nodiscard]] bool shutdown(std::chrono::milliseconds timeout);
  private:
    struct impl_t;
    std::unique_ptr<impl_t> impl_;
  };

  [[nodiscard]] bool install_profile_launch_service(const std::shared_ptr<profile_launch_service_t> &service);
  void uninstall_profile_launch_service(const std::shared_ptr<profile_launch_service_t> &service);
  [[nodiscard]] std::shared_ptr<profile_launch_service_t> profile_service_for(std::string_view client);
}  // namespace multiseat
#endif
