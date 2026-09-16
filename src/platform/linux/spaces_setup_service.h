/** Host-owned, durable first-space preparation. Configuration takes effect at the next explicit restart. */
#pragma once
#ifdef __linux__
#include "spaces_runtime.h"
#include "multiseat_profile_catalog.h"
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>

namespace multiseat::spaces {
  struct setup_request_t {
    std::string operation, request_id, runtime_id, name, gpu_id;
  };
  [[nodiscard]] std::optional<setup_request_t> decode_setup_request(std::string_view payload);

  struct setup_operations_t {
    std::function<runtime_install_result_t(std::string_view, std::stop_token)> install;
    std::function<bool(const profiles::first_steam_request_t &, std::string_view, std::stop_token)> prepare;
    std::function<nlohmann::json(const runtime_t &)> graphics;
    std::function<bool(const profiles::first_steam_request_t &, const runtime_t &, std::string_view, std::stop_token)> activate;
  };

  class setup_service_t {
  public:
    setup_service_t(std::filesystem::path journal, std::vector<runtime_t> catalog,
      bool enabled, setup_operations_t operations);
    ~setup_service_t();
    [[nodiscard]] nlohmann::json snapshot() const;
    // 202 queued/cancelling, 200 idempotent completion, 409 conflict, 503 unavailable.
    int submit(const setup_request_t &request);
    void shutdown();

  private:
    struct record_t {
      setup_request_t request;
      std::string reference, image, state, code, gpu_id;
    };
    bool save_locked();
    void work();
    std::filesystem::path journal_;
    std::vector<runtime_t> catalog_;
    setup_operations_t operations_;
    std::shared_ptr<void> lease_;
    mutable std::mutex mutex_;
    std::mutex shutdown_mutex_;
    std::condition_variable changed_;
    std::optional<record_t> record_;
    bool enabled_ = false, fault_ = false, active_ = false, closing_ = false;
    std::stop_source cancellation_;
    std::thread worker_;
  };

  [[nodiscard]] std::shared_ptr<setup_service_t> make_setup_service(
    const std::filesystem::path &directory, bool enabled);
  bool install_setup_service(const std::shared_ptr<setup_service_t> &service);
  void uninstall_setup_service(const std::shared_ptr<setup_service_t> &service);
  [[nodiscard]] std::shared_ptr<setup_service_t> installed_setup_service();
}
#endif
