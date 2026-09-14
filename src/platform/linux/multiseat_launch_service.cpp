/** @file src/platform/linux/multiseat_launch_service.cpp
 * @brief One owner for profile launch preparation, cleanup and shutdown.
 */
#include "multiseat_launch_service.h"
#ifdef __linux__
#include "multiseat_container_host.h"
#include "src/logging.h"
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
#include <map>
#include <nlohmann/json.hpp>
#include <set>
#include <thread>
#include <utility>

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
      std::optional<std::string> profile_for_client(std::string_view client) const override {
        return runtime_->profile_for_client(client);
      }
      std::vector<profile_summary_t> profile_catalog() const override { return runtime_->profile_catalog(); }
      std::vector<profile_activity_t> profile_activity() const override { return runtime_->profile_activity(); }
      bool idle() const override {
        return runtime_->seats() == 0 && runtime_->managed_workers() == 0 &&
          runtime_->input_allocations() == 0 && runtime_->tracked_launches() == 0;
      }
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
          // The image-owned compositor currently consumes keyboard and mouse
          // descriptors only. Permissions do not establish provider support.
          // RTSP omits native pen/touch so clients can use mouse emulation.
          .touch = false,
          .pen = false,
          .gamepad_slots = (launch->perm & crypto::PERM::input_controller) == crypto::PERM::_no ? 0U : 1U,
          .steam_input = admitted.admission.seat->runtime_profile == runtime_profile_e::steam &&
            (launch->perm & crypto::PERM::input_controller) != crypto::PERM::_no,
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
    struct admin_request_t {
      std::string profile, client;
      std::optional<bool> access;
      std::optional<profiles::steam_create_request_t> creation;
      std::optional<profiles::edit_request_t> edit;
      std::promise<profile_launch_result_t> promise;
      std::shared_future<profile_launch_result_t> future = promise.get_future().share();
    };
    struct request_t {
      std::weak_ptr<rtsp_stream::launch_session_t> launch;
      std::promise<profile_launch_result_t> promise;
      std::chrono::steady_clock::time_point deadline;
      std::optional<seat_handle_t> seat;
    };
    std::unique_ptr<profile_controller_t> controller;
    profile_admin_options_t admin;
    std::shared_ptr<admin_request_t> queued_admin;
    std::shared_ptr<admin_request_t> active_admin;
    bool reconfiguring = false, admin_failed = false, selection_failed = false;
    std::map<std::string, std::string> selections;
    std::filesystem::path selection_path;

    static bool permitted(const profile_summary_t &profile, std::string_view client) {
      return !profile.archived &&
        (std::find(profile.clients.begin(), profile.clients.end(), client) != profile.clients.end() ||
         std::find(profile.access_clients.begin(), profile.access_clients.end(), client) != profile.access_clients.end());
    }
    std::optional<std::string> selected_for(std::string_view client) const {
      if (!controller || stopping || reconfiguring || admin_failed || selection_failed) return std::nullopt;
      const auto saved = selections.find(std::string(client));
      if (saved != selections.end()) for (const auto &profile : controller->profile_catalog())
        if (profile.id == saved->second && permitted(profile, client)) return profile.id;
      return controller->profile_for_client(client);
    }
    static std::map<std::string, std::string> decode_selections(std::string_view payload) {
      using json = nlohmann::json;
      std::vector<std::set<std::string>> keys;
      const auto value = json::parse(payload, [&](int depth, json::parse_event_t event, json &item) {
        if (depth > 4) throw std::invalid_argument("selection nesting");
        if (event == json::parse_event_t::object_start) keys.emplace_back();
        if (event == json::parse_event_t::key && !keys.back().insert(item.get<std::string>()).second)
          throw std::invalid_argument("duplicate selection field");
        if (event == json::parse_event_t::object_end) keys.pop_back();
        return true;
      });
      exact_keys(value, {"schema", "selections"});
      if (!value.at("schema").is_number_unsigned() || value.at("schema") != 1 || !value.at("selections").is_object() || value.at("selections").size() > 65536)
        throw std::invalid_argument("selection schema");
      auto result = value.at("selections").get<std::map<std::string, std::string>>();
      for (const auto &[client, profile] : result)
        for (const auto *id : {&client, &profile})
          if (id->empty() || id->size() > 128 || id->find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") != std::string::npos)
            throw std::invalid_argument("selection identity");
      return result;
    }
    bool save_selection(std::string_view client, std::string_view profile) {
      auto next = selections; next[std::string(client)] = profile;
      if (next.size() > 65536) return false;
      if (!selection_path.empty()) {
        const auto result = private_state_file::update_atomic(selection_path, profiles::maximum_catalog_bytes,
          [&](const auto &current) -> std::optional<std::string> {
            if (current && decode_selections(current.payload) != selections) return std::nullopt;
            if (!current && !selections.empty()) return std::nullopt;
            const auto payload = nlohmann::json{{"schema", 1}, {"selections", next}}.dump();
            return payload.size() <= profiles::maximum_catalog_bytes ? std::optional{payload} : std::nullopt;
          });
        if (result.status == private_state_file::write_status_e::durability_uncertain) selection_failed = true;
        if (result.status != private_state_file::write_status_e::committed) return false;
      }
      selections = std::move(next);
      return true;
    }
    std::vector<profile_summary_t> fallback_catalog;
    std::set<std::string> blocked_clients;
    const std::chrono::milliseconds timeout;
    std::mutex mutex;
    std::condition_variable wake, closed;
    bool stopping = false, stopped = false;
    std::deque<std::shared_ptr<request_t>> queued;
    std::vector<std::weak_ptr<rtsp_stream::launch_session_t>> tracked;
    std::jthread thread;

    impl_t(std::unique_ptr<profile_controller_t> value, std::chrono::milliseconds timeout_value,
           profile_admin_options_t admin_value) :
        controller(std::move(value)), admin(std::move(admin_value)), timeout(timeout_value) {
      if (!controller || timeout <= std::chrono::milliseconds::zero() || timeout > std::chrono::seconds(25))
        throw std::invalid_argument("profile service options");
      if (!admin.persist && !admin.catalog.empty())
        admin.persist = [path = admin.catalog](auto profile, auto client) { return profiles::set_assignment(path, profile, client); };
      if (!admin.create && !admin.catalog.empty())
        admin.create = [path = admin.catalog](const auto &request) {
          container::local_host_t host;
          return profiles::create_steam(path, request, host);
        };
      if (!admin.edit && !admin.catalog.empty())
        admin.edit = [path = admin.catalog](const auto &request) { return profiles::edit(path, request); };
      if (!admin.access && !admin.catalog.empty())
        admin.access = [path = admin.catalog](auto profile, auto client, bool allowed) { return profiles::set_access(path, profile, client, allowed); };
      if (!admin.catalog.empty()) {
        selection_path = admin.catalog; selection_path += ".selections";
        if (std::filesystem::exists(selection_path)) {
          const auto saved = private_state_file::read_secure(selection_path, profiles::maximum_catalog_bytes, false, false);
          if (!saved) throw std::invalid_argument("unsafe space selections");
          selections = decode_selections(saved.payload);
        }
      }
      thread = std::jthread([this](std::stop_token stop) { run(stop); });
    }

    void change_profiles(const std::shared_ptr<admin_request_t> &request, bool pending) noexcept {
      profile_launch_result_t result {503, "Profile configuration could not be restored. Restart Polaris after reviewing the catalog."};
      try {
        if (pending || !controller || !controller->idle()) {
          { std::lock_guard lock(mutex); blocked_clients.clear(); }
          result = {409, request->edit ? "Stop space streams and wait for cleanup before renaming or removing a space" : request->creation ? "Stop profile sessions and wait for cleanup before creating a profile" :
            "Stop profile sessions and wait for cleanup before changing assignments"};
        } else {
          {
            std::lock_guard lock(mutex);
            fallback_catalog = controller->profile_catalog();
            if (!request->client.empty()) blocked_clients.insert(request->client);
          }
          if (!controller->shutdown()) {
            std::lock_guard lock(mutex);
            admin_failed = true;
          } else {
            // Closing proves that no stream owns this catalog. Destruction
            // releases the old global input owner before the replacement is built.
            { std::lock_guard lock(mutex); controller.reset(); admin_failed = true; }
            const auto persisted = request->access ? admin.access(request->profile, request->client, *request->access) :
              request->edit ? admin.edit(*request->edit) : request->creation ? admin.create(*request->creation) :
              admin.persist(request->profile, request->client);
            if (request->creation && !persisted && !persisted.volume_name.empty()) {
              BOOST_LOG(error) << "Profile creation retained resources for inspection: volume=" << persisted.volume_name
                << " initializer=" << persisted.initializer_name << " network=" << persisted.network_name;
            }
            if (persisted.status != private_state_file::write_status_e::durability_uncertain) {
              auto replacement = admin.reload();
              if (replacement) {
                std::lock_guard lock(mutex);
                controller = std::move(replacement);
                admin_failed = false;
                fallback_catalog.clear(); blocked_clients.clear();
                result = persisted ? profile_launch_result_t {200, request->edit ? "Space change saved" : request->creation ? "Steam profile created" : "Profile assignment saved"} :
                  profile_launch_result_t {409, request->edit ? "Space change was not saved; refresh before retrying" : request->creation ?
                    "Profile was not created. Refresh before retrying; retained provisioning resources may need administrator review." :
                    "Assignment was not saved; refresh before retrying"};
              }
            }
          }
        }
      } catch (...) {
        std::lock_guard lock(mutex);
        admin_failed = true;
      }
      { std::lock_guard lock(mutex); reconfiguring = false; active_admin.reset(); }
      request->promise.set_value(result);
    }

    void run(std::stop_token stop) noexcept {
      std::deque<std::shared_ptr<request_t>> pending;
      auto finish = [](const auto &request, profile_launch_result_t result) {
        if (!result.prepared()) if (const auto launch = request->launch.lock()) launch->cancel();
        request->promise.set_value(result);
      };
      for (;;) {
        bool drain;
        std::shared_ptr<admin_request_t> change;
        {
          std::unique_lock lock(mutex);
          drain = stopping || stop.stop_requested();
          change = std::exchange(queued_admin, {});
          while (!queued.empty()) { pending.push_back(std::move(queued.front())); queued.pop_front(); }
          std::erase_if(tracked, [](const auto &weak) { const auto launch = weak.lock(); return !launch || launch->is_cancelled(); });
        }
        if (drain) {
          if (change) change->promise.set_value({503, "The profile controller is stopping"});
          for (const auto &request : pending) finish(request, {503, "The profile controller is stopping"});
          pending.clear();
          bool complete = false;
          try { complete = !controller || controller->shutdown(); } catch (...) {}
          if (complete || stop.stop_requested()) {
            std::lock_guard lock(mutex);
            stopped = complete;
            closed.notify_all();
            return;
          }
        } else {
          if (change) change_profiles(change, !pending.empty());
          bool reconciled = true;
          try {
            if (controller && !admin_failed) controller->reconcile();
            else reconciled = false;
          } catch (...) { reconciled = false; }
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
        wake.wait_for(lock, std::chrono::milliseconds(50), [&] { return !queued.empty() || queued_admin || stop.stop_requested(); });
      }
    }
  };

  profile_launch_service_t::profile_launch_service_t(std::unique_ptr<profile_controller_t> controller,
    std::chrono::milliseconds timeout, profile_admin_options_t admin) :
      impl_(std::make_unique<impl_t>(std::move(controller), timeout, std::move(admin))) {}
  profile_launch_service_t::~profile_launch_service_t() {
    stop_admission();
    impl_->thread.request_stop();
    impl_->wake.notify_all();
    impl_->thread.join();
  }
  bool profile_launch_service_t::routes_client(std::string_view client) const {
    std::lock_guard lock(impl_->mutex);
    if (impl_->blocked_clients.contains(std::string(client))) return true;
    if (impl_->controller) return impl_->controller->routes_client(client);
    for (const auto &profile : impl_->fallback_catalog)
      if (impl_t::permitted(profile, client)) return true;
    return false;
  }
  std::optional<std::string> profile_launch_service_t::profile_for_client(std::string_view client) const {
    std::lock_guard lock(impl_->mutex);
    return impl_->selected_for(client);
  }

  std::optional<std::string> profile_launch_service_t::profile_name_for_client(std::string_view client) const {
    std::lock_guard lock(impl_->mutex);
    if (!impl_->controller || impl_->stopping || impl_->admin_failed || impl_->reconfiguring) return std::nullopt;
    const auto id = impl_->selected_for(client);
    if (!id) return std::nullopt;
    for (const auto &profile : impl_->controller->profile_catalog())
      if (profile.id == *id && !profile.name.empty()) return profile.name;
    return std::nullopt;
  }

  profile_launch_result_t profile_launch_service_t::prepare(const std::shared_ptr<rtsp_stream::launch_session_t> &launch,
                                                         std::string_view expected_profile) {
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
      if (impl_->stopping || impl_->reconfiguring || impl_->admin_failed || impl_->selection_failed || !impl_->controller ||
          impl_->tracked.size() >= 64 || launch->lifecycle_generation)
        return {503, "Profile launch admission is unavailable"};
      if (!impl_->controller->routes_client(launch->unique_id) ||
          (!expected_profile.empty() && impl_->selected_for(launch->unique_id) !=
            std::optional<std::string>{expected_profile}))
        return {409, "The profile assignment changed; refresh the library"};
      const auto selected = impl_->selected_for(launch->unique_id);
      if (!selected) return {409, "Choose an available space before launching"};
      launch->worker_profile_key = *selected;
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

  profile_admin_snapshot_t profile_launch_service_t::admin_snapshot() const {
    std::lock_guard lock(impl_->mutex);
    return {static_cast<bool>(impl_->admin.reload && impl_->admin.persist), impl_->reconfiguring,
      impl_->admin_failed && !impl_->reconfiguring,
      impl_->controller ? impl_->controller->profile_catalog() : impl_->fallback_catalog,
      static_cast<bool>(impl_->admin.reload && impl_->admin.create),
      static_cast<bool>(impl_->admin.reload && impl_->admin.edit)};
  }

  profile_launch_result_t profile_launch_service_t::set_assignment(std::string profile, std::string client) {
    auto request = std::make_shared<impl_t::admin_request_t>();
    request->profile = std::move(profile); request->client = std::move(client);
    auto future = request->future;
    {
      std::lock_guard lock(impl_->mutex);
      if (!impl_->admin.reload || !impl_->admin.persist || !impl_->controller || impl_->admin_failed || impl_->stopping)
        return {503, "Profile administration is unavailable"};
      if (request->client.empty() || request->client.size() > 256) return {400, "Invalid paired device"};
      const auto catalog = impl_->controller->profile_catalog();
      if (!request->profile.empty() && std::none_of(catalog.begin(), catalog.end(),
          [&](const auto &entry) { return entry.id == request->profile && !entry.archived; })) return {404, "Unknown profile"};
      if (impl_->reconfiguring || !impl_->queued.empty() || std::any_of(impl_->tracked.begin(), impl_->tracked.end(),
          [](const auto &weak) { const auto launch = weak.lock(); return launch && !launch->is_cancelled(); }))
        return {409, "Stop profile sessions and wait for cleanup before changing assignments"};
      impl_->reconfiguring = true;
      // Fence even a previously unassigned device before the owner thread
      // starts the catalog transaction. It must not fall through to host apps.
      impl_->blocked_clients.insert(request->client);
      impl_->queued_admin = request;
      impl_->active_admin = request;
    }
    impl_->wake.notify_all();
    if (future.wait_for(std::chrono::seconds(25)) != std::future_status::ready)
      return {202, "The assignment change is still running; refresh before retrying"};
    return future.get();
  }

  profile_client_spaces_t profile_launch_service_t::client_spaces(std::string_view client) const {
    std::lock_guard lock(impl_->mutex);
    profile_client_spaces_t result;
    const auto selected = impl_->selected_for(client);
    result.available = selected.has_value(); result.selected = selected.value_or("");
    result.can_switch = result.available;
    auto activity = impl_->controller ? impl_->controller->profile_activity() : std::vector<profile_activity_t>{};
    for (const auto &weak : impl_->tracked) if (const auto launch = weak.lock(); launch && !launch->is_cancelled()) {
      if (std::none_of(activity.begin(), activity.end(), [&](const auto &item) { return item.client == launch->unique_id; }))
        activity.push_back({launch->worker_profile_key, launch->unique_id,
          launch->setup_state.load() == rtsp_stream::launch_session_t::setup_state_e::started ? "running" : "starting"});
    }
    for (const auto &item : activity) if (item.client == client) result.can_switch = false;
    const auto catalog = impl_->controller ? impl_->controller->profile_catalog() : impl_->fallback_catalog;
    for (const auto &profile : catalog) {
      if (!impl_t::permitted(profile, client)) continue;
      std::string state = result.available ? "ready" : "unavailable";
      for (const auto &item : activity) if (item.profile == profile.id) {
        state = item.client == client ? item.state : "in_use"; break;
      }
      result.spaces.push_back({profile.id, profile.name, state, selected == profile.id});
    }
    return result;
  }

  profile_launch_result_t profile_launch_service_t::select_space(std::string_view client, std::string_view profile,
                                                                std::string_view previous) {
    std::lock_guard lock(impl_->mutex);
    const auto selected = impl_->selected_for(client);
    if (!selected) return {503, "Spaces are unavailable. Refresh and try again."};
    if (client.empty() || client.size() > 128 || profile.empty() || profile.size() > 128 || previous.size() > 128)
      return {400, "Invalid space selection"};
    const auto catalog = impl_->controller->profile_catalog();
    if (std::none_of(catalog.begin(), catalog.end(), [&](const auto &entry) {
          return entry.id == profile && impl_t::permitted(entry, client);
        })) return {404, "This space is not available to this device"};
    if (*selected != previous) return {409, "Your selected space changed. Refresh before choosing again."};
    // Re-selecting the same Space is harmless while it is active.
    if (*selected == profile) return {200, "Space selected"};
    for (const auto &weak : impl_->tracked) if (const auto launch = weak.lock();
      launch && launch->unique_id == client && !launch->is_cancelled())
      return {409, "End your stream before switching spaces"};
    for (const auto &activity : impl_->controller->profile_activity()) if (activity.client == client)
      return {409, "Wait for your space to finish closing before switching"};
    try {
      if (!impl_->save_selection(client, profile)) return {503, "Space selection was not saved. Refresh before retrying."};
    } catch (...) { return {503, "Space selection was not saved. Refresh before retrying."}; }
    return {200, "Space selected"};
  }

  profile_launch_result_t profile_launch_service_t::set_access(std::string profile, std::string client, bool allowed) {
    auto request = std::make_shared<impl_t::admin_request_t>();
    request->profile = std::move(profile); request->client = std::move(client); request->access = allowed;
    const auto future = request->future;
    {
      std::lock_guard lock(impl_->mutex);
      if (!impl_->admin.reload || !impl_->admin.access || !impl_->controller || impl_->admin_failed || impl_->stopping)
        return {503, "Space access administration is unavailable"};
      if (request->client.empty() || request->client.size() > 128 || request->profile.empty() || request->profile.size() > 128)
        return {400, "Invalid space or paired device"};
      const auto catalog = impl_->controller->profile_catalog();
      if (std::none_of(catalog.begin(), catalog.end(), [&](const auto &entry) { return entry.id == request->profile && !entry.archived; }))
        return {404, "Unknown space"};
      if (impl_->reconfiguring || !impl_->queued.empty() || std::any_of(impl_->tracked.begin(), impl_->tracked.end(),
        [](const auto &weak) { const auto launch = weak.lock(); return launch && !launch->is_cancelled(); }))
        return {409, "Stop space streams and wait for cleanup before changing access"};
      impl_->reconfiguring = true; impl_->blocked_clients.insert(request->client);
      impl_->queued_admin = request; impl_->active_admin = request;
    }
    impl_->wake.notify_all();
    if (future.wait_for(std::chrono::seconds(25)) != std::future_status::ready)
      return {202, "Space access is still being saved. Refresh before retrying."};
    return future.get();
  }

  profile_launch_result_t profile_launch_service_t::create_steam_profile(profiles::steam_create_request_t creation) {
    if (!profiles::valid_steam_create_request(creation)) return {400, "Enter a valid profile name and Steam setup"};
    std::shared_ptr<impl_t::admin_request_t> request;
    {
      std::lock_guard lock(impl_->mutex);
      if (impl_->stopping) return {503, "The profile controller is stopping"};
      if (impl_->active_admin && impl_->active_admin->creation &&
          impl_->active_admin->creation->request_id == creation.request_id) {
        if (*impl_->active_admin->creation != creation) return {409, "This creation request is already in use"};
        request = impl_->active_admin;
      } else {
        if (!impl_->admin.reload || !impl_->admin.create || !impl_->controller || impl_->admin_failed)
          return {503, "Profile creation is unavailable"};
        if (impl_->reconfiguring || !impl_->queued.empty() || std::any_of(impl_->tracked.begin(), impl_->tracked.end(),
            [](const auto &weak) { const auto launch = weak.lock(); return launch && !launch->is_cancelled(); }))
          return {409, "Stop profile sessions and wait for cleanup before creating a profile"};
        const auto catalog = impl_->controller->profile_catalog();
        if (std::none_of(catalog.begin(), catalog.end(), [&](const auto &entry) {
              return entry.id == creation.source_profile_id && entry.steam;
            })) return {404, "Select an existing configured Steam profile"};
        request = std::make_shared<impl_t::admin_request_t>();
        request->creation = std::move(creation);
        impl_->reconfiguring = true;
        impl_->active_admin = impl_->queued_admin = request;
      }
    }
    impl_->wake.notify_all();
    if (request->future.wait_for(impl_->timeout) != std::future_status::ready)
      return {202, "The profile is still being created; refresh before retrying"};
    return request->future.get();
  }

  profile_launch_result_t profile_launch_service_t::edit_profile(profiles::edit_request_t edit) {
    if (!profiles::valid_edit_request(edit)) return {400, "Enter a valid space name and operation"};
    std::shared_ptr<impl_t::admin_request_t> request;
    {
      std::lock_guard lock(impl_->mutex);
      if (impl_->stopping) return {503, "The space controller is stopping"};
      if (impl_->active_admin && impl_->active_admin->edit && *impl_->active_admin->edit == edit) {
        request = impl_->active_admin;
      } else {
        if (!impl_->admin.reload || !impl_->admin.edit || !impl_->controller || impl_->admin_failed)
          return {503, "Space management is unavailable"};
        if (impl_->reconfiguring || !impl_->queued.empty() || std::any_of(impl_->tracked.begin(), impl_->tracked.end(),
            [](const auto &weak) { const auto launch = weak.lock(); return launch && !launch->is_cancelled(); }))
          return {409, "Stop space streams and wait for cleanup before renaming or removing a space"};
        const auto catalog = impl_->controller->profile_catalog();
        if (std::none_of(catalog.begin(), catalog.end(), [&](const auto &entry) { return entry.id == edit.profile_id; }))
          return {404, "Space not found. Refresh spaces"};
        request = std::make_shared<impl_t::admin_request_t>();
        request->edit = std::move(edit);
        impl_->reconfiguring = true;
        impl_->active_admin = impl_->queued_admin = request;
      }
    }
    impl_->wake.notify_all();
    if (request->future.wait_for(impl_->timeout) != std::future_status::ready)
      return {202, "The space change is still running; refresh before retrying"};
    return request->future.get();
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
  profile_session_snapshot_t profile_launch_service_t::session_snapshot(std::string_view client) const {
    std::lock_guard lock(impl_->mutex);
    for (const auto &weak : impl_->tracked) {
      const auto launch = weak.lock();
      if (launch && !launch->is_cancelled() && launch->unique_id == client &&
          launch->setup_state.load() == rtsp_stream::launch_session_t::setup_state_e::started)
        return {true, launch->session_token, launch->width, launch->height, launch->fps / 1000};
    }
    return {};
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
  std::shared_ptr<profile_launch_service_t> installed_profile_service() {
    std::lock_guard lock(installed_mutex);
    return installed;
  }
}  // namespace multiseat
#endif
