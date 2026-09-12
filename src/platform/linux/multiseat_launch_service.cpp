/** @file src/platform/linux/multiseat_launch_service.cpp
 * @brief One owner for profile launch preparation, cleanup and shutdown.
 */
#include "multiseat_launch_service.h"
#ifdef __linux__
#include "src/private_state_file.h"
#include "src/rtsp.h"
#include "src/utility.h"
#include "src/uuid.h"

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <future>
#include <limits>
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>
#include <thread>

namespace multiseat {
  namespace {
    std::mutex installed_mutex;
    std::shared_ptr<profile_launch_service_t> installed;
    // Worker lifecycle generations cannot be confused with host proc generations.
    std::atomic<std::uint64_t> next_generation {1ULL << 63};

    class production_profile_controller_t final : public profile_controller_t {
    public:
      explicit production_profile_controller_t(std::unique_ptr<controller_runtime_t> runtime) : runtime_(std::move(runtime)) {}
      bool routes_client(std::string_view client) const override { return runtime_->routes_client(client); }
      void reconcile() override { (void) runtime_->reconcile(); }
      profile_begin_result_t begin(const std::shared_ptr<rtsp_stream::launch_session_t> &launch) override {
        auto admitted = runtime_->admit_authenticated_profile_launch(launch, {
          static_cast<std::uint32_t>(launch->width), static_cast<std::uint32_t>(launch->height),
          static_cast<std::uint32_t>(launch->fps), launch->enable_hdr,
        });
        if (!admitted.admitted()) {
          if (admitted.status == controller_profile_admission_status_e::rejected) {
            if (admitted.admission.rejection == admission_rejection_e::profile_already_active)
              return {{409, "This profile already has an active seat"}, {}};
            if (admitted.admission.rejection == admission_rejection_e::client_already_active)
              return {{409, "This device already has an active seat"}, {}};
            return {{409, "No seat or encoder capacity is available for this profile"}, {}};
          }
          return {{503, "Profile reconciliation has not completed"}, {}};
        }
        const auto handle = admitted.admission.seat->handle;
        const input::plan_t plan {
          .touch = !!(launch->perm & crypto::PERM::input_touch),
          .pen = !!(launch->perm & crypto::PERM::input_pen),
          .gamepad_slots = (launch->perm & crypto::PERM::input_controller) == crypto::PERM::_no ? 0U : 1U,
        };
        if (runtime_->bind_runtime(handle, compositor_e::gamescope, "profile-owned launch") != mutation_result_e::applied ||
            !runtime_->start_seat(handle, plan).started()) return {};
        return {{200, "Profile worker is starting"}, handle};
      }
      profile_poll_e poll(const std::shared_ptr<rtsp_stream::launch_session_t> &launch,
                          const seat_handle_t &seat) override {
        const auto state = runtime_->seat_state(seat);
        if (!state || *state == seat_state_e::stopping) return profile_poll_e::failed;
        if (*state != seat_state_e::running || !runtime_->admission_ready()) return profile_poll_e::pending;
        return runtime_->select_authenticated_launch(launch, seat).selected() ?
          profile_poll_e::selected : profile_poll_e::failed;
      }
      bool shutdown() override { return runtime_->shutdown().closed(); }
    private:
      std::unique_ptr<controller_runtime_t> runtime_;
    };

    bool path_value(const std::filesystem::path &path) {
      return path.is_absolute() && path.lexically_normal() == path &&
        path.native().find_first_of(",:\n\r") == std::string::npos;
    }
    void exact_keys(const nlohmann::json &value, std::initializer_list<const char *> keys) {
      if (!value.is_object() || value.size() != keys.size()) throw std::invalid_argument("controller fields");
      for (const auto *key : keys) if (!value.contains(key)) throw std::invalid_argument("controller field missing");
    }
  }  // namespace

  std::unique_ptr<profile_controller_t> make_profile_controller(std::unique_ptr<controller_runtime_t> runtime) {
    if (!runtime) return {};
    return std::make_unique<production_profile_controller_t>(std::move(runtime));
  }

  std::optional<production_controller_options_t> load_controller_options(const std::filesystem::path &path) {
    constexpr std::size_t bound = 64 * 1024;
    if (!path_value(path)) return std::nullopt;
    auto read = private_state_file::read_secure(path, bound, false, false);
    if (!read) return std::nullopt;
    try {
      using json = nlohmann::json;
      std::vector<std::set<std::string>> keys;
      const auto root = json::parse(read.payload, [&](int depth, json::parse_event_t event, json &value) {
        if (depth > 8) throw std::invalid_argument("controller nesting");
        if (event == json::parse_event_t::object_start) keys.emplace_back();
        if (event == json::parse_event_t::key && !keys.back().emplace(value.get<std::string>()).second)
          throw std::invalid_argument("duplicate controller field");
        if (event == json::parse_event_t::object_end) keys.pop_back();
        return true;
      });
      exact_keys(root, {"schema", "deployment_id", "profile_catalog", "ipc_root", "selinux_type", "gpus"});
      if (!root.at("schema").is_number_unsigned() || root.at("schema") != 1 ||
          !root.at("gpus").is_array() || root.at("gpus").size() > 16) return std::nullopt;
      production_controller_options_t options;
      options.enabled = true;
      options.profile_catalog = root.at("profile_catalog").get<std::string>();
      options.container.ipc_root = root.at("ipc_root").get<std::string>();
      options.container.deployment_id = root.at("deployment_id").get<std::string>();
      options.container.selinux_type = root.at("selinux_type").get<std::string>();
      options.container.media_enabled = true;
      if (!path_value(options.profile_catalog) || !path_value(options.container.ipc_root) ||
          options.container.deployment_id.empty() || options.container.deployment_id.size() > 64 ||
          options.container.deployment_id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos ||
          (!options.container.selinux_type.empty() && options.container.selinux_type != "polaris_nvidia_worker_t")) return std::nullopt;
      for (const auto &private_file : {path, options.profile_catalog}) {
        const auto relative = private_file.lexically_relative(options.container.ipc_root);
        if (!relative.empty() && *relative.begin() != "..") return std::nullopt;
      }
      std::set<std::string> ids, devices;
      for (const auto &gpu : root.at("gpus")) {
        exact_keys(gpu, {"id", "render_node", "devices", "max_seats", "max_encoder_sessions"});
        production_controller_gpu_t entry;
        entry.logical_gpu_id = gpu.at("id").get<std::string>();
        entry.render_node = gpu.at("render_node").get<std::string>();
        for (const auto *capacity : {"max_seats", "max_encoder_sessions"}) {
          if (!gpu.at(capacity).is_number_unsigned() || gpu.at(capacity) < 1 || gpu.at(capacity) > 16) return std::nullopt;
        }
        entry.max_seats = gpu.at("max_seats").get<std::uint32_t>();
        entry.max_encoder_sessions = gpu.at("max_encoder_sessions").get<std::uint32_t>();
        if (entry.logical_gpu_id.empty() || entry.logical_gpu_id.size() > 128 ||
            entry.logical_gpu_id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.") != std::string::npos ||
            !ids.emplace(entry.logical_gpu_id).second || !gpu.at("devices").is_array() ||
            gpu.at("devices").empty() || gpu.at("devices").size() > 64) return std::nullopt;
        for (const auto &device : gpu.at("devices")) {
          std::filesystem::path device_path = device.get<std::string>();
          if (!path_value(device_path) || !device_path.native().starts_with("/dev/") ||
              !devices.emplace(device_path.native()).second) return std::nullopt;
          entry.devices.push_back(std::move(device_path));
        }
        if (std::find(entry.devices.begin(), entry.devices.end(), entry.render_node) == entry.devices.end()) return std::nullopt;
        options.gpus.push_back(std::move(entry));
      }
      return options;
    } catch (...) { return std::nullopt; }
  }

  struct profile_launch_service_t::impl_t {
    struct request_t {
      std::weak_ptr<rtsp_stream::launch_session_t> launch;
      std::promise<profile_launch_result_t> promise;
      std::chrono::steady_clock::time_point deadline;
      std::optional<seat_handle_t> seat;
    };
    std::unique_ptr<profile_controller_t> controller;
    const std::chrono::milliseconds timeout;
    std::mutex mutex;
    std::condition_variable wake, closed;
    bool stopping = false, stopped = false;
    std::deque<std::shared_ptr<request_t>> queued;
    std::vector<std::weak_ptr<rtsp_stream::launch_session_t>> tracked;
    std::jthread thread;

    impl_t(std::unique_ptr<profile_controller_t> value, std::chrono::milliseconds timeout_value) :
        controller(std::move(value)), timeout(timeout_value) {
      if (!controller || timeout <= std::chrono::milliseconds::zero() || timeout > std::chrono::seconds(25))
        throw std::invalid_argument("profile service options");
      thread = std::jthread([this](std::stop_token stop) { run(stop); });
    }

    void run(std::stop_token stop) noexcept {
      std::deque<std::shared_ptr<request_t>> pending;
      auto finish = [](const auto &request, profile_launch_result_t result) {
        if (!result.prepared()) if (const auto launch = request->launch.lock()) launch->cancel();
        request->promise.set_value(result);
      };
      for (;;) {
        bool drain;
        {
          std::unique_lock lock(mutex);
          drain = stopping || stop.stop_requested();
          while (!queued.empty()) { pending.push_back(std::move(queued.front())); queued.pop_front(); }
          std::erase_if(tracked, [](const auto &weak) { const auto launch = weak.lock(); return !launch || launch->is_cancelled(); });
        }
        if (drain) {
          for (const auto &request : pending) finish(request, {503, "The profile controller is stopping"});
          pending.clear();
          bool complete = false;
          try { complete = controller->shutdown(); } catch (...) {}
          if (complete || stop.stop_requested()) {
            std::lock_guard lock(mutex);
            stopped = complete;
            closed.notify_all();
            return;
          }
        } else {
          bool reconciled = true;
          try { controller->reconcile(); } catch (...) { reconciled = false; }
          for (auto it = pending.begin(); it != pending.end();) {
            const auto &request = *it;
            const auto launch = request->launch.lock();
            std::optional<profile_launch_result_t> done;
            if (!launch || launch->is_cancelled()) done = profile_launch_result_t {409, "Profile launch was cancelled"};
            else if (std::chrono::steady_clock::now() >= request->deadline) done = profile_launch_result_t {504, "Profile startup timed out"};
            else if (reconciled) {
              try {
                if (!request->seat) {
                  auto started = controller->begin(launch);
                  request->seat = started.seat;
                  if (!request->seat) done = started.result;
                } else {
                  const auto state = controller->poll(launch, *request->seat);
                  if (state == profile_poll_e::selected) done = profile_launch_result_t {200, "Profile ready"};
                  else if (state == profile_poll_e::failed) done = profile_launch_result_t {};
                }
              } catch (...) { done = profile_launch_result_t {}; }
            }
            if (done) { finish(request, *done); it = pending.erase(it); }
            else ++it;
          }
        }
        std::unique_lock lock(mutex);
        wake.wait_for(lock, std::chrono::milliseconds(50), [&] { return !queued.empty() || stop.stop_requested(); });
      }
    }
  };

  profile_launch_service_t::profile_launch_service_t(std::unique_ptr<profile_controller_t> controller,
    std::chrono::milliseconds timeout) : impl_(std::make_unique<impl_t>(std::move(controller), timeout)) {}
  profile_launch_service_t::~profile_launch_service_t() {
    stop_admission();
    impl_->thread.request_stop();
    impl_->wake.notify_all();
    impl_->thread.join();
  }
  bool profile_launch_service_t::routes_client(std::string_view client) const { return impl_->controller->routes_client(client); }

  profile_launch_result_t profile_launch_service_t::prepare(const std::shared_ptr<rtsp_stream::launch_session_t> &launch) {
    if (!launch || !routes_client(launch->unique_id)) return {404, "No profile is assigned to this device"};
    launch->require_worker_connection();
    if (launch->is_cancelled() || launch->lifecycle_generation || launch->watch_only || launch->input_only ||
        launch->temporary_authorization || !(launch->perm & crypto::PERM::launch) ||
        launch->width <= 0 || launch->height <= 0 || launch->fps <= 0 || launch->fps % 1000 != 0 || launch->enable_hdr) {
      return {400, "Profile launch requires a new authorized SDR session with a whole frame rate"};
    }
    auto request = std::make_shared<impl_t::request_t>();
    request->launch = launch;
    request->deadline = std::chrono::steady_clock::now() + impl_->timeout;
    auto future = request->promise.get_future();
    {
      std::lock_guard lock(impl_->mutex);
      if (impl_->stopping || impl_->tracked.size() >= 64 || launch->lifecycle_generation)
        return {503, "Profile launch admission is unavailable"};
      const auto generation = next_generation.fetch_add(1);
      if (generation < (1ULL << 63) || generation == std::numeric_limits<std::uint64_t>::max()) return {};
      launch->lifecycle_generation = generation;
      launch->session_token = uuid_util::uuid_t::generate().string();
      launch->client_do_cmds.clear();
      launch->client_undo_cmds.clear();
      impl_->tracked.push_back(launch);
      impl_->queued.push_back(request);
    }
    impl_->wake.notify_all();
    if (future.wait_until(request->deadline) != std::future_status::ready) {
      launch->cancel();
      impl_->wake.notify_all();
      return {504, "Profile startup timed out"};
    }
    return future.get();
  }

  bool profile_launch_service_t::cancel_client(std::string_view client, std::string_view token) {
    bool matched = token.empty();
    std::lock_guard lock(impl_->mutex);
    for (const auto &weak : impl_->tracked) {
      const auto launch = weak.lock();
      if (launch && !launch->is_cancelled() && launch->unique_id == client && (token.empty() || launch->session_token == token)) {
        launch->cancel();
        matched = true;
      }
    }
    impl_->wake.notify_all();
    return matched;
  }
  void profile_launch_service_t::stop_admission() {
    std::lock_guard lock(impl_->mutex);
    impl_->stopping = true;
    for (const auto &weak : impl_->tracked) if (const auto launch = weak.lock()) launch->cancel();
    impl_->wake.notify_all();
  }
  std::optional<std::string> profile_launch_service_t::session_token(std::string_view client) const {
    std::lock_guard lock(impl_->mutex);
    for (const auto &weak : impl_->tracked) {
      const auto launch = weak.lock();
      if (launch && !launch->is_cancelled() && launch->unique_id == client &&
          launch->setup_state.load() == rtsp_stream::launch_session_t::setup_state_e::started)
        return launch->session_token;
    }
    return std::nullopt;
  }
  bool profile_launch_service_t::shutdown(std::chrono::milliseconds timeout) {
    stop_admission();
    std::unique_lock lock(impl_->mutex);
    return impl_->closed.wait_for(lock, timeout, [&] { return impl_->stopped; });
  }
  bool install_profile_launch_service(const std::shared_ptr<profile_launch_service_t> &service) {
    std::lock_guard lock(installed_mutex);
    if (!service || installed) return false;
    installed = service;
    return true;
  }
  void uninstall_profile_launch_service(const std::shared_ptr<profile_launch_service_t> &service) {
    std::lock_guard lock(installed_mutex);
    if (installed == service) installed.reset();
  }
  std::shared_ptr<profile_launch_service_t> profile_service_for(std::string_view client) {
    std::lock_guard lock(installed_mutex);
    return installed && installed->routes_client(client) ? installed : nullptr;
  }
}  // namespace multiseat
#endif
