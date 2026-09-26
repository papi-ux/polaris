/** @file src/platform/linux/multiseat_launch_service.cpp
 * @brief One owner for profile launch preparation, cleanup and shutdown.
 */
#include "multiseat_launch_service.h"
#ifdef __linux__
#include "multiseat_container_host.h"
#include "multiseat_profile_network.h"
#include "spaces_host_admin.h"
#include "spaces_nvidia_libraries.h"
#include "spaces_runtime.h"
#include "spaces_runtime_move.h"
#include "spaces_setup.h"
#include "src/platform/common.h"
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
#include <sstream>
#include <thread>
#include <utility>

namespace multiseat {
  namespace {
    /** A launcher family by the name the catalog and the console use for it. */
    runtime_profile_e family_of(std::string_view name) {
      for (const auto profile : {runtime_profile_e::steam, runtime_profile_e::heroic, runtime_profile_e::lutris})
        if (runtime_profile_name(profile) == name) return profile;
      return runtime_profile_e::unknown;
    }

    /**
     * What a library calls the tile that opens the launcher itself. Steam's is
     * named for the interface it opens, since that is what a player sees.
     */
    std::string launcher_display_name(std::string_view family) {
      if (family == "steam") return "Steam Big Picture";
      if (family == "heroic") return "Heroic";
      if (family == "lutris") return "Lutris";
      return {};
    }

    // A refusal's words are views, so they are literals: a sentence built for the refusal would be
    // gone by the time the launch response copies it.
    std::string_view missing_game_action(std::string_view family) {
      if (family == "steam") return "Open Steam Big Picture in that Space, or refresh the library.";
      if (family == "heroic") return "Open Heroic in that Space, or refresh the library.";
      if (family == "lutris") return "Open Lutris in that Space, or refresh the library.";
      return "Open the Space's launcher, or refresh the library.";
    }

    std::mutex installed_mutex;
    std::shared_ptr<profile_launch_service_t> installed;
    // Worker lifecycle generations cannot be confused with host proc generations.
    std::atomic<std::uint64_t> next_generation {1ULL << 63};

    // The controller repeats the same report every 50 ms while nothing changes, so each distinct
    // summary is logged once, without the per-pass counters. Without these lines a Space change
    // that cannot close or rebuild the controller fails with nothing in the log.
    const char *shutdown_status_name(controller_shutdown_status_e status) {
      switch (status) {
        case controller_shutdown_status_e::closed: return "closed";
        case controller_shutdown_status_e::already_closed: return "already_closed";
        case controller_shutdown_status_e::workers_pending: return "workers_pending";
        case controller_shutdown_status_e::streams_pending: return "streams_pending";
        case controller_shutdown_status_e::input_cleanup_incomplete: return "input_cleanup_incomplete";
      }
      return "unknown";
    }
    void describe_worker(std::ostream &out, const coordinator_reconciliation_report_t &worker) {
      const auto &broker = worker.broker;
      out << "worker{admission_ready=" << worker.admission_ready
          << " startup_recovery_complete=" << worker.startup_recovery_complete
          << " authority_blocked=" << worker.authority_blocked
          << " authority_status=" << static_cast<int>(worker.authority_status)
          << " authority_entries=" << worker.authority_entries
          << " active_orphan_authorities=" << worker.active_orphan_authorities
          << " authority_failures=" << worker.authority_failures
          << " endpoint_failures=" << worker.endpoint_failures
          << " endpoint_shutdown_failures=" << worker.endpoint_shutdown_failures
          << " broker{admission_ready=" << broker.admission_ready
          << " inventory_authoritative=" << broker.inventory_authoritative
          << " backend_observation_failed=" << broker.backend_observation_failed;
      // Admission closes for the pass, and without this nothing said why.
      if (broker.backend_observation_failed) out << " backend_observation_error=[" << broker.backend_observation_error << ']';
      out
          << " current=" << broker.current_workers << " orphans=" << broker.orphan_workers
          << " missing=" << broker.missing_workers << " stuck=" << broker.stuck_workers
          << " protocol_errors=" << broker.protocol_errors
          << " readiness_rejections=" << broker.readiness_rejections << "}}";
    }
    std::string describe_reconcile(const controller_reconcile_result_t &result) {
      std::ostringstream out;
      out << "ready=" << result.ready() << " status=" << static_cast<int>(result.status) << ' ';
      describe_worker(out, result.worker);
      if (!result.input) {
        out << " input=none";
        return out.str();
      }
      const auto &input = result.input->report;
      out << " input{status=" << static_cast<int>(result.input->status)
          << " admission_ready=" << input.admission_ready
          << " inventory_authoritative=" << input.inventory_authoritative
          << " expected=" << input.expected << " current=" << input.current
          << " orphans=" << input.orphans << " missing=" << input.missing
          << " protocol_errors=" << input.protocol_errors
          << " backend_failures=" << input.backend_failures << '}';
      return out.str();
    }
    std::string describe_shutdown(const controller_shutdown_report_t &report) {
      std::ostringstream out;
      out << "status=" << shutdown_status_name(report.status) << " stop_requests=" << report.stop_requests;
      if (report.worker) {
        out << ' ';
        describe_worker(out, *report.worker);
      }
      if (report.input) {
        out << " input{status=" << static_cast<int>(report.input->status)
            << " released=" << report.input->released_allocations
            << " cleanup_failures=" << report.input->cleanup_failures << '}';
      }
      return out.str();
    }

    class production_profile_controller_t final : public profile_controller_t {
    public:
      explicit production_profile_controller_t(std::unique_ptr<controller_runtime_t> runtime) : runtime_(std::move(runtime)) {}
      bool routes_client(std::string_view client) const override { return runtime_->routes_client(client); }
      std::optional<std::string> profile_for_client(std::string_view client) const override {
        return runtime_->profile_for_client(client);
      }
      std::vector<profile_summary_t> profile_catalog() const override { return runtime_->profile_catalog(); }
      spaces::library_reader_t library_reader() const override { return runtime_->library_reader(); }
      std::vector<std::string> desktop_clients() const override { return runtime_->desktop_clients(); }
      std::vector<std::string> desktop_default_clients() const override { return runtime_->desktop_default_clients(); }
      std::vector<profile_activity_t> profile_activity() const override { return runtime_->profile_activity(); }
      bool idle() const override {
        return runtime_->seats() == 0 && runtime_->managed_workers() == 0 &&
          runtime_->input_allocations() == 0 && runtime_->tracked_launches() == 0;
      }
      std::optional<gpu_usage_t> capacity() const override { return runtime_->capacity(); }
      void reconcile() override {
        auto summary = describe_reconcile(runtime_->reconcile());
        if (summary == last_reconcile_) return;
        BOOST_LOG(info) << "Spaces controller reconcile: " << summary;
        last_reconcile_ = std::move(summary);
      }
      profile_begin_result_t begin(const std::shared_ptr<rtsp_stream::launch_session_t> &launch) override {
        auto admitted = runtime_->admit_authenticated_profile_launch(launch, {
          static_cast<std::uint32_t>(launch->width), static_cast<std::uint32_t>(launch->height),
          static_cast<std::uint32_t>(launch->fps), launch->enable_hdr,
        });
        if (!admitted.admitted()) {
          if (admitted.status == controller_profile_admission_status_e::rejected) {
            switch (admitted.admission.rejection) {
              case admission_rejection_e::profile_already_active:
                return {{409, "This Space is already being played on another device.", "space_in_use",
                  "Wait for that stream to end, or choose another Space."}, {}};
              case admission_rejection_e::client_already_active:
                return {{409, "This device already has a Space running.", "device_busy",
                  "End that stream before starting another."}, {}};
              case admission_rejection_e::seat_capacity_reached:
                return {{409, "Every Space slot on this host is in use.", "space_capacity",
                  "Wait for a Space to finish, or end its stream from Spaces in Polaris."}, {}};
              case admission_rejection_e::encoder_capacity_reached:
                return {{409, "The host's video encoder is fully used by other streams.", "encoder_capacity",
                  "Wait for a stream to end before opening this Space."}, {}};
              default:
                return {{409, "The host could not admit this Space launch.", "space_not_admitted",
                  "Open Spaces in Polaris and recheck Host Setup."}, {}};
            }
          }
          return {{503, "Polaris is still reconciling its Spaces after a change.", "spaces_reconciling",
            "Try again in a moment."}, {}};
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
        // Either refusal used to leave nothing anywhere: the client got the
        // default words and the log got none. Say which step, and with what.
        const auto bound = runtime_->bind_runtime(handle, compositor_e::gamescope, "profile-owned launch");
        if (bound != mutation_result_e::applied) {
          BOOST_LOG(warning) << "Space worker was not started: the runtime could not be bound (result "
                             << static_cast<int>(bound) << ')';
          return {};
        }
        const auto started = runtime_->start_seat(handle, plan);
        if (!started.started()) {
          BOOST_LOG(warning) << "Space worker was not started: seat start status "
                             << static_cast<int>(started.status) << ", worker result "
                             << (started.worker ? std::to_string(static_cast<int>(*started.worker)) : std::string("none"));
          return {};
        }
        return {{200, "Space worker is starting"}, handle};
      }
      profile_poll_e poll(const std::shared_ptr<rtsp_stream::launch_session_t> &launch,
                          const seat_handle_t &seat) override {
        const auto state = runtime_->seat_state(seat);
        if (!state || *state == seat_state_e::stopping) return profile_poll_e::failed;
        if (*state != seat_state_e::running || !runtime_->admission_ready()) return profile_poll_e::pending;
        return runtime_->select_authenticated_launch(launch, seat).selected() ?
          profile_poll_e::selected : profile_poll_e::failed;
      }
      bool shutdown() override {
        const auto report = runtime_->shutdown();
        if (report.closed()) return true;
        auto summary = describe_shutdown(report);
        if (summary != last_shutdown_) {
          BOOST_LOG(warning) << "Spaces controller did not close: " << summary;
          last_shutdown_ = std::move(summary);
        }
        return false;
      }
    private:
      std::unique_ptr<controller_runtime_t> runtime_;
      std::string last_reconcile_, last_shutdown_;
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

  /**
   * Give the controller the machine's NVIDIA userspace, for runtimes built
   * without driver libraries of their own. A host that cannot supply a
   * complete set simply gets no mounts: its Spaces then refuse at launch with
   * a Host Setup pointer rather than starting and rendering black.
   */
  void attach_host_driver_libraries(production_controller_options_t &options) {
    const auto &contract = spaces::trusted_nvidia_contract();
    const auto &catalog = spaces::trusted_runtimes();
    if (!contract || !catalog) return;
    options.host_driver_image = [](std::string_view image) {
      const auto &runtimes = spaces::trusted_runtimes();
      if (!runtimes) return false;
      container::local_host_t host;
      return spaces::image_borrows_host_driver(host, image, *runtimes, &spaces::image_runtime_cache());
    };
    const auto loaded = spaces::loaded_nvidia_driver();
    if (!loaded || loaded->empty()) return;
    container::local_host_t host;
    const auto facts = spaces::resolve_host_driver_libraries(*contract, host, *loaded, contract->minimum_driver);
    if (!facts.ready()) {
      BOOST_LOG(warning) << "Spaces: this PC's NVIDIA driver files are not ready ("sv << facts.code << ')';
      return;
    }
    const auto directory = platf::appdata() / "spaces-graphics" / facts.driver_version;
    if (!spaces::publish_vendor_files(facts, directory)) {
      BOOST_LOG(warning) << "Spaces: could not publish the graphics descriptions under "sv
                         << directory.string() << "; a Space cannot read them as they are"sv;
      return;
    }
    options.container.host_driver = {facts.driver_version, facts.contract,
      spaces::host_driver_mounts(*contract, facts, directory)};
    BOOST_LOG(info) << "Spaces: NVIDIA driver "sv << facts.driver_version << " supplies "sv
                    << options.container.host_driver.mounts.size() << " files to a host-driver runtime"sv;
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
      attach_host_driver_libraries(options);
      return options;
    } catch (...) { return std::nullopt; }
  }

  struct profile_launch_service_t::impl_t {
    struct admin_request_t {
      std::string profile, client;
      std::optional<bool> access;
      std::vector<std::string> paired_clients;
      bool with_desktop = false;
      std::optional<std::vector<std::string>> all_clients;  // set for select all and clear all
      std::optional<profiles::space_create_request_t> creation;
      std::optional<profiles::edit_request_t> edit;
      std::optional<profiles::runtime_move_t> move;
      std::string kept_volume, kept_network;  // set by the owner thread before the promise
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
    std::uint64_t controller_revision = 0;
    profile_admin_options_t admin;
    std::shared_ptr<admin_request_t> queued_admin;
    std::shared_ptr<admin_request_t> active_admin;
    bool reconfiguring = false, admin_failed = false, selection_failed = false;
    // Finished removals for good, newest last, so a retry after the record is
    // gone is confirmed instead of reported as an unknown Space.
    std::deque<profiles::edit_request_t> removed_for_good;
    std::map<std::string, std::string> selections;
    std::filesystem::path selection_path;

    static bool permitted(const profile_summary_t &profile, std::string_view client) {
      return !profile.archived &&
        (std::find(profile.clients.begin(), profile.clients.end(), client) != profile.clients.end() ||
         std::find(profile.access_clients.begin(), profile.access_clients.end(), client) != profile.access_clients.end());
    }
    // Why selected_for() has nothing for this device, in the words a client keys on.
    std::string unavailable_reason() const {
      if (!controller) return "controller_missing";
      if (stopping) return "stopping";
      if (reconfiguring) return "reconfiguring";
      if (admin_failed) return "admin_failed";
      if (selection_failed) return "selection_failed";
      return "no_space_assigned";
    }
    std::optional<std::string> selected_for(std::string_view client) const {
      if (!controller || stopping || reconfiguring || admin_failed || selection_failed) return std::nullopt;
      const auto desktops = controller->desktop_clients();
      const bool desktop = std::find(desktops.begin(), desktops.end(), client) != desktops.end();
      const auto saved = selections.find(std::string(client));
      if (saved != selections.end() && saved->second == "desktop" && desktop) return "desktop";
      if (saved != selections.end()) for (const auto &profile : controller->profile_catalog())
        if (profile.id == saved->second && permitted(profile, client)) return profile.id;
      // A Desktop default comes before the first Space the device may open.
      const auto desktop_defaults = controller->desktop_default_clients();
      if (desktop && std::find(desktop_defaults.begin(), desktop_defaults.end(), client) != desktop_defaults.end()) return "desktop";
      const auto assigned = controller->profile_for_client(client);
      return assigned ? assigned : desktop ? std::optional<std::string>{"desktop"} : std::nullopt;
    }
    // The image the device's selected Space launches; nothing for Desktop or no Space.
    std::optional<std::string> selected_image(std::string_view client) const {
      const auto selected = selected_for(client);
      if (!selected || *selected == "desktop") return std::nullopt;
      for (const auto &profile : controller->profile_catalog())
        if (profile.id == *selected) return profile.image;
      return std::nullopt;
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
    // The owner's "a device with a Space also gets Desktop" setting. It sits beside the catalog, as
    // the selections do, and not in it: the catalog's keys are read strictly, by an older Polaris
    // on the same host as well, and one more key there would cost that build every Space.
    bool desktop_by_default = false;
    std::filesystem::path settings_path;
    static bool decode_settings(std::string_view payload) {
      const auto value = nlohmann::json::parse(payload);
      exact_keys(value, {"schema", "desktop_by_default"});
      if (!value.at("schema").is_number_unsigned() || value.at("schema") != 1 || !value.at("desktop_by_default").is_boolean())
        throw std::invalid_argument("spaces settings schema");
      return value.at("desktop_by_default").get<bool>();
    }
    bool save_desktop_by_default(bool enabled) {
      if (!settings_path.empty()) {
        const auto payload = nlohmann::json{{"schema", 1}, {"desktop_by_default", enabled}}.dump();
        const auto result = private_state_file::update_atomic(settings_path, profiles::maximum_catalog_bytes,
          [&](const auto &) -> std::optional<std::string> { return payload; });
        if (result.status != private_state_file::write_status_e::committed) return false;
      }
      desktop_by_default = enabled;
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
    std::vector<std::weak_ptr<rtsp_stream::launch_session_t>> host_launches;
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
          // A launcher this PC already runs a Space for copies that Space's
          // runtime. The first Space of a launcher has none to copy, so it
          // takes the image from the admitted catalog entry for that family,
          // which must already be downloaded: this path never pulls.
          if (!request.family.empty()) {
            const auto &catalog = spaces::trusted_runtimes();
            // Read the catalog and let go of it: the lease this takes is the
            // same one the write below needs, so holding it here would refuse
            // every first Space of a launcher.
            bool have_one = false;
            {
              const auto existing = profiles::load(path);
              have_one = existing && std::any_of(existing->catalog.profiles.begin(),
                existing->catalog.profiles.end(), [&](const auto &entry) {
                  return runtime_profile_name(entry.storage.runtime_profile) == request.family && !entry.archived;
                });
            }
            if (!have_one && catalog) {
              const auto choice = spaces::choose_runtime(*catalog, spaces::loaded_nvidia_driver(), request.family);
              if (!choice.runtime)
                return profiles::change_result_t {.error = "This Polaris build has no gaming runtime for that launcher."};
              const auto facts = spaces::inspect_runtime(host, *catalog, spaces::loaded_nvidia_driver(), true,
                &spaces::runtime_inspection_cache(), request.family);
              if (facts.status != "ready")
                return profiles::change_result_t {.error = std::string(profiles::space_runtime_not_downloaded.message),
                  .refusal = profiles::space_runtime_not_downloaded};
              return profiles::create_first_space(path, {request.request_id, request.name},
                choice.runtime->config_digest, request.family, host);
            }
          }
          return profiles::create_space(path, request, host);
        };
      if (!admin.edit && !admin.catalog.empty())
        admin.edit = [path = admin.catalog](const auto &request) { return profiles::edit(path, request); };
      if (!admin.access && !admin.catalog.empty())
        admin.access = [path = admin.catalog](auto profile, auto client, bool allowed, const auto &paired, bool with_desktop) { return profile == "desktop" ? profiles::set_desktop_access(path, client, allowed, paired) : profiles::set_access(path, profile, client, allowed, paired, with_desktop); };
      if (!admin.access_for_all && !admin.catalog.empty())
        admin.access_for_all = [path = admin.catalog](auto profile, const auto &clients, bool allowed, const auto &paired, bool with_desktop) {
          return profiles::set_access_for_all(path, profile, clients, allowed, paired, with_desktop);
        };
      if (!admin.remove_for_good && !admin.catalog.empty())
        admin.remove_for_good = [path = admin.catalog](const auto &request, std::stop_token stop) {
          container::local_host_t host(stop);
          return profiles::remove_for_good(path, request, host);
        };
      if (!admin.move_runtime && !admin.catalog.empty())
        admin.move_runtime = [path = admin.catalog](const auto &move, std::stop_token stop) {
          container::local_host_t host(stop);
          return profiles::move_runtime(path, move, host);
        };
      if (!admin.runtime_matches_host && !admin.catalog.empty())
        admin.runtime_matches_host = [](std::string_view image) { return spaces::runtime_matches_loaded_driver(image); };
      if (!admin.catalog.empty()) {
        selection_path = admin.catalog; selection_path += ".selections";
        if (std::filesystem::exists(selection_path)) {
          const auto saved = private_state_file::read_secure(selection_path, profiles::maximum_catalog_bytes, false, false);
          if (!saved) throw std::invalid_argument("unsafe space selections");
          selections = decode_selections(saved.payload);
        }
        settings_path = admin.catalog; settings_path += ".settings";
        if (std::filesystem::exists(settings_path)) {
          const auto saved = private_state_file::read_secure(settings_path, profiles::maximum_catalog_bytes, false, false);
          if (!saved) throw std::invalid_argument("unsafe spaces settings");
          desktop_by_default = decode_settings(saved.payload);
        }
      }
      thread = std::jthread([this](std::stop_token stop) { run(stop); });
    }

    static profile_launch_result_t removal_response(const profiles::removal_result_t &removed) {
      using outcome_e = profiles::removal_outcome_e;
      switch (removed.outcome) {
        case outcome_e::removed:
          return {200, "Space removed for good"};
        case outcome_e::not_found:
          return {404, "Space not found.", "space_unknown", "Refresh Spaces."};
        case outcome_e::name_mismatch:
          return {409, "The name you typed is not this Space's name.", "space_name_mismatch",
            "Type the Space's name exactly as it is shown."};
        case outcome_e::last_space:
          return {409, "This is the only Space, so it can be archived but not removed for good.", "space_last",
            "Create another Space first, or archive this one."};
        case outcome_e::docker_unavailable:
          return {503, "Docker did not answer, so nothing was removed.", "docker_unavailable",
            "Check that Docker is running, then try again."};
        case outcome_e::storage_unverified:
          return {409, "This Space's games and saves are not in the storage Polaris made for it, so nothing was removed.",
            "space_storage_unverified", "Archive the Space instead, or open Doctor & Support."};
        case outcome_e::storage_in_use:
          return {409, "A container is still using this Space's games and saves, so nothing was removed.",
            "space_storage_in_use", "Wait a moment, then try again."};
        case outcome_e::storage_not_removed:
          return {503, "Docker did not confirm that this Space's games and saves were deleted. The Space is archived for now.",
            "space_storage_not_removed", "Remove it for good again from Archived Spaces to finish."};
        case outcome_e::record_not_removed:
          return {503, "This Space's games and saves were deleted, but Polaris could not take it off the list. The Space is archived for now.",
            "space_record_not_removed", "Remove it for good again from Archived Spaces to finish."};
        default:
          return {409, "The Space was not removed. Refresh before retrying.", "spaces_change_not_saved",
            "Refresh Spaces and try again."};
      }
    }

    static profile_launch_result_t move_response(const profiles::runtime_move_result_t &moved) {
      using outcome_e = profiles::runtime_move_outcome_e;
      switch (moved.outcome) {
        case outcome_e::moved:
          return {200, "Space moved to the new gaming runtime"};
        case outcome_e::already_moved:
          return {200, "The Space already uses that gaming runtime"};
        case outcome_e::not_found:
          return {404, "Space not found.", "space_unknown", "Refresh Spaces."};
        case outcome_e::space_changed:
          return space_runtime_changed_result;
        case outcome_e::profile_mismatch:
          return space_runtime_profile_mismatch_result;
        case outcome_e::media_contract_mismatch:
          return space_runtime_media_mismatch_result;
        case outcome_e::identity_mismatch:
          return space_runtime_identity_mismatch_result;
        case outcome_e::docker_unavailable:
          return {503, "Docker did not answer, so the Space was not moved.", "docker_unavailable",
            "Check that Docker is running, then try again."};
        case outcome_e::storage_unverified:
          return {409, "This Space's games and saves are not in the storage Polaris made for it, so it was not moved.",
            "space_storage_unverified", "Open Doctor & Support."};
        default:
          return {409, "The Space was not moved. Refresh before retrying.", "spaces_change_not_saved",
            "Refresh Spaces and try again."};
      }
    }

    static const char *move_outcome_name(profiles::runtime_move_outcome_e outcome) {
      using outcome_e = profiles::runtime_move_outcome_e;
      switch (outcome) {
        case outcome_e::moved: return "moved";
        case outcome_e::already_moved: return "already_moved";
        case outcome_e::invalid: return "invalid";
        case outcome_e::not_found: return "not_found";
        case outcome_e::space_changed: return "space_changed";
        case outcome_e::profile_mismatch: return "profile_mismatch";
        case outcome_e::media_contract_mismatch: return "media_contract_mismatch";
        case outcome_e::identity_mismatch: return "identity_mismatch";
        case outcome_e::docker_unavailable: return "docker_unavailable";
        case outcome_e::storage_unverified: return "storage_unverified";
        case outcome_e::not_saved: return "not_saved";
      }
      return "unknown";
    }

    void change_profiles(const std::shared_ptr<admin_request_t> &request, bool pending, std::stop_token stop) noexcept {
      profile_launch_result_t result {503, "Space settings could not be restored. Restart Polaris after reviewing the Spaces catalog.",
        "spaces_admin_failed", "Restart Polaris."};
      try {
        bool host_active;
        { std::lock_guard lock(mutex); host_active = std::any_of(host_launches.begin(), host_launches.end(),
            [](const auto &weak) { const auto p = weak.lock(); return p && !p->is_cancelled(); }); }
        if (pending || host_active || !controller || !controller->idle()) {
          { std::lock_guard lock(mutex); blocked_clients.clear(); }
          result = {409, "Stop every Space stream and wait for cleanup before changing Spaces.", "spaces_streaming", "End the running Space streams, then try again."};
        } else {
          {
            std::lock_guard lock(mutex);
            fallback_catalog = controller->profile_catalog();
            if (!request->client.empty()) blocked_clients.insert(request->client);
            if (request->all_clients) blocked_clients.insert(request->all_clients->begin(), request->all_clients->end());
          }
          if (!controller->shutdown()) {
            BOOST_LOG(error) << "A Space change could not close the Spaces controller, so Space changes stay unavailable until Polaris restarts";
            std::lock_guard lock(mutex);
            admin_failed = true;
          } else {
            // Closing proves that no stream owns this catalog. Destruction
            // releases the old global input owner before the replacement is built.
            { std::lock_guard lock(mutex); ++controller_revision; controller.reset(); admin_failed = true; }
            const bool removal = request->edit && request->edit->operation == profiles::edit_operation_e::remove_for_good;
            const bool moving = request->move.has_value();
            profiles::removal_result_t removed;
            profiles::runtime_move_result_t moved;
            profiles::change_result_t persisted;
            if (moving) {
              const auto &move = *request->move;
              moved = admin.move_runtime(move, stop);
              if (moved.outcome == profiles::runtime_move_outcome_e::moved) {
                BOOST_LOG(info) << "Space " << move.profile_id << " now launches runtime image " << move.to_image
                  << " instead of " << moved.previous_image << "; its home, network, name and devices are unchanged";
              } else if (moved.outcome == profiles::runtime_move_outcome_e::already_moved) {
                BOOST_LOG(info) << "Space " << move.profile_id << " already launches runtime image " << move.to_image;
              } else {
                BOOST_LOG(warning) << "Space " << move.profile_id << " was not moved to runtime image " << move.to_image
                  << ": " << move_outcome_name(moved.outcome);
              }
            } else if (removal) {
              removed = admin.remove_for_good(*request->edit, stop);
              request->kept_volume = removed.kept_volume;
              request->kept_network = removed.kept_network;
              if (!removed.kept_volume.empty() || !removed.kept_network.empty()) {
                BOOST_LOG(warning) << "Removing a Space for good kept Docker resources: volume="
                  << (removed.kept_volume.empty() ? "none" : removed.kept_volume)
                  << " network=" << (removed.kept_network.empty() ? "none" : removed.kept_network);
              }
            } else {
              persisted = request->all_clients ? admin.access_for_all(request->profile, *request->all_clients, *request->access, request->paired_clients, request->with_desktop) :
                request->access ? admin.access(request->profile, request->client, *request->access, request->paired_clients, request->with_desktop) :
                request->edit ? admin.edit(*request->edit) : request->creation ? admin.create(*request->creation) :
                admin.persist(request->profile, request->client);
            }
            if (request->creation && !persisted && !persisted.volume_name.empty()) {
              BOOST_LOG(error) << "Profile creation retained resources for inspection: volume=" << persisted.volume_name
                << " initializer=" << persisted.initializer_name << " network=" << persisted.network_name;
            }
            const auto saved = removal ? removed.status : moving ? moved.status : persisted.status;
            const bool save_confirmed = saved != private_state_file::write_status_e::durability_uncertain;
            if (!save_confirmed) BOOST_LOG(error) << "A Space change could not confirm its save, so Spaces stay closed until Polaris restarts";
            if (save_confirmed) {
              auto replacement = admin.reload();
              if (!replacement) BOOST_LOG(error) << "Spaces could not be reloaded after a change, so Spaces stay closed until Polaris restarts";
              if (replacement) {
                std::lock_guard lock(mutex);
                controller = std::move(replacement);
                admin_failed = false;
                fallback_catalog.clear(); blocked_clients.clear();
                if (moving) {
                  result = move_response(moved);
                } else if (removal) {
                  result = removal_response(removed);
                  if (removed) {
                    removed_for_good.push_back(*request->edit);
                    while (removed_for_good.size() > 32) removed_for_good.pop_front();
                  }
                } else if (!persisted && persisted.refusal) {
                  result = {409, persisted.refusal->message, persisted.refusal->code, persisted.refusal->action};
                } else result = persisted ? profile_launch_result_t {200, request->edit ? "Space change saved" : request->creation ? "Space created" : request->access ? "Space access saved" : "Default Space saved"} :
                  profile_launch_result_t {409, request->edit ? "The Space change was not saved. Refresh before retrying." : request->creation ?
                    "The Space was not created. Refresh before retrying; retained resources may need administrator review." :
                    request->access ? "The Space access change was not saved. Refresh before retrying." :
                    "The Default Space was not saved. Refresh before retrying.", "spaces_change_not_saved", "Refresh Spaces and try again."};
              }
            }
          }
        }
      } catch (...) {
        BOOST_LOG(error) << "A Space change failed unexpectedly, so Spaces stay closed until Polaris restarts";
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
          if (change) change->promise.set_value({503, "Spaces are shutting down.", "spaces_stopping"});
          for (const auto &request : pending) finish(request, {503, "Spaces are shutting down.", "spaces_stopping"});
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
          if (change) change_profiles(change, !pending.empty(), stop);
          bool reconciled = true;
          try {
            if (controller && !admin_failed) controller->reconcile();
            else reconciled = false;
          } catch (...) { reconciled = false; }
          for (auto it = pending.begin(); it != pending.end();) {
            const auto &request = *it;
            const auto launch = request->launch.lock();
            std::optional<profile_launch_result_t> done;
            if (!launch || launch->is_cancelled()) done = profile_launch_result_t {409, "The Space launch was cancelled.", "space_launch_cancelled"};
            else if (std::chrono::steady_clock::now() >= request->deadline) done = space_start_timeout_result;
            else if (reconciled) {
              try {
                if (!request->seat) {
                  auto started = controller->begin(launch);
                  request->seat = started.seat;
                  if (!request->seat) done = started.result;
                } else {
                  const auto state = controller->poll(launch, *request->seat);
                  if (state == profile_poll_e::selected) done = profile_launch_result_t {200, "Space ready"};
                  else if (state == profile_poll_e::failed) done = space_runtime_unavailable_result;
                }
              } catch (...) { done = space_runtime_unavailable_result; }
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
  bool profile_launch_service_t::track_host_launch(const std::shared_ptr<rtsp_stream::launch_session_t> &launch) {
    if (!launch) return false;
    std::lock_guard lock(impl_->mutex);
    if (impl_->blocked_clients.contains(launch->unique_id)) return false;
    if (!impl_->controller) return !impl_->admin_failed;
    const auto desktops = impl_->controller->desktop_clients();
    const bool relevant = impl_->controller->routes_client(launch->unique_id) ||
      std::find(desktops.begin(), desktops.end(), launch->unique_id) != desktops.end();
    if (!relevant) return true;
    if (impl_->selected_for(launch->unique_id) != std::optional<std::string>{"desktop"}) return false;
    std::erase_if(impl_->host_launches, [](const auto &weak) { const auto p = weak.lock(); return !p || p->is_cancelled(); });
    if (impl_->host_launches.size() >= 64) return false;
    impl_->host_launches.push_back(launch);
    return true;
  }

  bool profile_launch_service_t::routes_client(std::string_view client) const {
    std::lock_guard lock(impl_->mutex);
    if (impl_->blocked_clients.contains(std::string(client))) return true;
    if (impl_->controller) {
      if (impl_->selected_for(client) == std::optional<std::string>{"desktop"}) return false;
      return impl_->controller->routes_client(client);
    }
    for (const auto &profile : impl_->fallback_catalog)
      if (impl_t::permitted(profile, client)) return true;
    return false;
  }
  std::optional<std::string> profile_launch_service_t::profile_for_client(std::string_view client) const {
    std::lock_guard lock(impl_->mutex);
    const auto selected = impl_->selected_for(client);
    return selected == std::optional<std::string>{"desktop"} ? std::nullopt : selected;
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
                                                         std::string_view expected_profile, std::string_view target) {
    if (!launch || !routes_client(launch->unique_id))
      return {404, "No Space is assigned to this device.", "no_space_assigned", "Open Spaces in Polaris and set this device's Default Space."};
    launch->require_worker_connection();
    if (launch->is_cancelled() || launch->lifecycle_generation || launch->watch_only || launch->input_only ||
        launch->temporary_authorization || !(launch->perm & crypto::PERM::launch) ||
        launch->width <= 0 || launch->height <= 0 || launch->fps <= 0 || launch->fps % 1000 != 0 || launch->enable_hdr) {
      return {400, "A Space stream needs a new SDR session at a whole frame rate.", "space_stream_options",
        "Set Play Setup to Auto frame rate with HDR off."};
    }
    const auto host_activity = spaces::try_begin_host_activity();
    if (!host_activity) return spaces_host_setup_running_result;
    std::string target_name;
    if (!target.empty()) {
      const auto snapshot = library_for_client(launch->unique_id, expected_profile);
      if (!snapshot) return {409, "This Space's library is no longer available.", "space_library_unavailable", "Refresh the library."};
      if (target == snapshot->launcher_target) target_name = snapshot->launcher_name;
      else {
        const auto game = std::find_if(snapshot->library.games.begin(), snapshot->library.games.end(),
          [&](const auto &item) { return item.target == target; });
        if (!snapshot->library.available || game == snapshot->library.games.end())
          return {409, "This game is no longer installed in the selected Space.", "space_game_missing",
            missing_game_action(snapshot->family)};
        target_name = game->name;
      }
    }
    // A runtime built for another NVIDIA driver cannot start: refuse before any
    // worker exists, with the fix, instead of failing inside the container.
    std::string checked_image;
    if (impl_->admin.runtime_matches_host) {
      std::optional<std::string> image;
      {
        std::lock_guard lock(impl_->mutex);
        if (impl_->controller && !impl_->stopping && !impl_->reconfiguring && !impl_->admin_failed)
          image = impl_->selected_image(launch->unique_id);
      }
      if (image && !image->empty()) {
        if (!impl_->admin.runtime_matches_host(*image)) return space_runtime_driver_mismatch_result;
        checked_image = *image;
      }
    }
    auto request = std::make_shared<impl_t::request_t>();
    request->launch = launch;
    request->deadline = std::chrono::steady_clock::now() + impl_->timeout;
    auto future = request->promise.get_future();
    {
      std::lock_guard lock(impl_->mutex);
      if (impl_->stopping || impl_->reconfiguring || impl_->admin_failed || impl_->selection_failed || !impl_->controller ||
          impl_->tracked.size() >= 64 || launch->lifecycle_generation)
        return {503, "Spaces are temporarily unavailable on the host.", "spaces_unavailable",
          "Refresh the library. If this continues, restart Polaris."};
      if (!impl_->controller->routes_client(launch->unique_id) ||
          (!expected_profile.empty() && impl_->selected_for(launch->unique_id) !=
            std::optional<std::string>{expected_profile}))
        return {409, "The Space assignment changed.", "space_assignment_changed", "Refresh the library."};
      const auto selected = impl_->selected_for(launch->unique_id);
      if (!selected) return {409, "No Space is selected for this device.", "no_space_selected", "Choose a Space in the library."};
      // The runtime check read the image outside the lock; a Space changed since then is asked again.
      if (impl_->admin.runtime_matches_host && impl_->selected_image(launch->unique_id).value_or("") != checked_image)
        return {409, "The Space assignment changed.", "space_assignment_changed", "Refresh the library."};
      launch->worker_profile_key = *selected;
      launch->worker_library_target = std::string(target);
      launch->worker_library_name = std::move(target_name);
      const auto generation = next_generation.fetch_add(1);
      if (generation < (1ULL << 63) || generation == std::numeric_limits<std::uint64_t>::max()) return space_runtime_unavailable_result;
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
      return space_start_timeout_result;
    }
    return future.get();
  }

  std::optional<profile_library_snapshot_t> profile_launch_service_t::library_for_client(
    std::string_view client, std::string_view profile) const {
    spaces::library_reader_t reader;
    profile_library_snapshot_t result;
    std::uint64_t epoch = 0;
    {
      std::lock_guard lock(impl_->mutex);
      if (!impl_->controller || impl_->stopping || impl_->reconfiguring || impl_->admin_failed || impl_->selection_failed) return {};
      const auto catalog = impl_->controller->profile_catalog();
      const auto entry = std::find_if(catalog.begin(), catalog.end(), [&](const auto &p) {
        return p.id == profile && p.library_enabled && impl_t::permitted(p, client);
      });
      if (entry == catalog.end()) return {};
      result.id = entry->id; result.name = entry->name;
      result.family = entry->family;
      result.launcher_target = std::string(container::launcher_sentinel(family_of(entry->family)));
      result.launcher_name = launcher_display_name(entry->family);
      reader = impl_->controller->library_reader(); epoch = impl_->controller_revision;
    }
    // The reader owns a copy of the immutable storage catalog. No service lock
    // is held while Docker reads manifests, so active streams keep reconciling.
    result.library = reader ? reader(profile) : spaces::library_t{};
    {
      std::lock_guard lock(impl_->mutex);
      if (!impl_->controller || impl_->controller_revision != epoch || impl_->stopping || impl_->reconfiguring || impl_->admin_failed) return {};
      const auto catalog = impl_->controller->profile_catalog();
      if (std::none_of(catalog.begin(), catalog.end(), [&](const auto &p) {
        return p.id == profile && p.library_enabled && impl_t::permitted(p, client);
      })) return {};
    }
    return result;
  }

  profile_admin_snapshot_t profile_launch_service_t::admin_snapshot() const {
    std::lock_guard lock(impl_->mutex);
    auto activity = impl_->controller ? impl_->controller->profile_activity() : std::vector<profile_activity_t>{};
    for (const auto &weak : impl_->tracked) if (const auto launch = weak.lock(); launch && !launch->is_cancelled()) {
      if (std::none_of(activity.begin(), activity.end(), [&](const auto &item) { return item.client == launch->unique_id; }))
        activity.push_back({launch->worker_profile_key, launch->unique_id,
          launch->setup_state.load() == rtsp_stream::launch_session_t::setup_state_e::started ? "running" : "starting"});
    }
    return {static_cast<bool>(impl_->admin.reload && impl_->admin.persist), impl_->reconfiguring,
      impl_->admin_failed && !impl_->reconfiguring,
      impl_->controller ? impl_->controller->profile_catalog() : impl_->fallback_catalog,
      static_cast<bool>(impl_->admin.reload && impl_->admin.create),
      static_cast<bool>(impl_->admin.reload && impl_->admin.edit),
      impl_->controller ? impl_->controller->desktop_clients() : std::vector<std::string>{}, std::move(activity),
      impl_->controller ? impl_->controller->capacity() : std::optional<gpu_usage_t>{},
      static_cast<bool>(impl_->admin.reload && impl_->admin.remove_for_good),
      impl_->controller ? impl_->controller->desktop_default_clients() : std::vector<std::string>{},
      static_cast<bool>(impl_->admin.reload && impl_->admin.move_runtime), impl_->desktop_by_default};
  }

  bool profile_launch_service_t::desktop_by_default() const {
    std::lock_guard lock(impl_->mutex);
    return impl_->desktop_by_default;
  }

  profile_launch_result_t profile_launch_service_t::set_desktop_by_default(bool enabled) {
    std::lock_guard lock(impl_->mutex);
    try {
      if (!impl_->save_desktop_by_default(enabled))
        return {503, "The Desktop Access setting was not saved.", "spaces_setting_not_saved", "Refresh Spaces and try again."};
    } catch (...) { return {503, "The Desktop Access setting was not saved.", "spaces_setting_not_saved", "Refresh Spaces and try again."}; }
    return {200, "Desktop Access setting saved"};
  }

  profile_launch_result_t profile_launch_service_t::set_assignment(std::string profile, std::string client) {
    const auto host_activity = spaces::try_begin_host_activity();
    if (!host_activity) return spaces_host_setup_running_result;
    auto request = std::make_shared<impl_t::admin_request_t>();
    request->profile = std::move(profile); request->client = std::move(client);
    auto future = request->future;
    {
      std::lock_guard lock(impl_->mutex);
      if (!impl_->admin.reload || !impl_->admin.persist || !impl_->controller || impl_->admin_failed || impl_->stopping)
        return {503, "Space administration is unavailable.", "spaces_admin_unavailable", "Refresh Spaces. If this continues, restart Polaris."};
      if (request->client.empty() || request->client.size() > 256) return {400, "Invalid paired device.", "invalid_request"};
      const auto catalog = impl_->controller->profile_catalog();
      if (!request->profile.empty() && request->profile != profiles::desktop_profile_key && std::none_of(catalog.begin(), catalog.end(),
          [&](const auto &entry) { return entry.id == request->profile && !entry.archived; })) return {404, "Unknown Space.", "space_unknown", "Refresh Spaces."};
      if (impl_->reconfiguring || !impl_->queued.empty() || std::any_of(impl_->tracked.begin(), impl_->tracked.end(),
          [](const auto &weak) { const auto launch = weak.lock(); return launch && !launch->is_cancelled(); }))
        return {409, "Stop every Space stream and wait for cleanup before changing Spaces.", "spaces_streaming", "End the running Space streams, then try again."};
      impl_->reconfiguring = true;
      // Fence even a previously unassigned device before the owner thread
      // starts the catalog transaction. It must not fall through to host apps.
      impl_->blocked_clients.insert(request->client);
      impl_->queued_admin = request;
      impl_->active_admin = request;
    }
    impl_->wake.notify_all();
    if (future.wait_for(std::chrono::seconds(25)) != std::future_status::ready)
      return {202, "The Default Space is still being saved. Refresh before retrying.", "spaces_change_pending"};
    return future.get();
  }

  profile_client_spaces_t profile_launch_service_t::client_spaces(std::string_view client) const {
    std::lock_guard lock(impl_->mutex);
    profile_client_spaces_t result;
    const auto selected = impl_->selected_for(client);
    result.available = selected.has_value(); result.selected = selected.value_or("");
    if (!result.available) result.unavailable_reason = impl_->unavailable_reason();
    result.can_switch = result.available;
    const auto desktops = impl_->controller ? impl_->controller->desktop_clients() : std::vector<std::string>{};
    result.desktop_allowed = std::find(desktops.begin(), desktops.end(), client) != desktops.end();
    const auto desktop_defaults = impl_->controller ? impl_->controller->desktop_default_clients() : std::vector<std::string>{};
    if (result.desktop_allowed && std::find(desktop_defaults.begin(), desktop_defaults.end(), client) != desktop_defaults.end())
      result.default_space = "desktop";
    else result.default_space = impl_->controller ? impl_->controller->profile_for_client(client).value_or("") : "";
    result.capacity = impl_->controller ? impl_->controller->capacity() : std::optional<gpu_usage_t>{};
    auto activity = impl_->controller ? impl_->controller->profile_activity() : std::vector<profile_activity_t>{};
    for (const auto &weak : impl_->tracked) if (const auto launch = weak.lock(); launch && !launch->is_cancelled()) {
      if (std::none_of(activity.begin(), activity.end(), [&](const auto &item) { return item.client == launch->unique_id; }))
        activity.push_back({launch->worker_profile_key, launch->unique_id,
          launch->setup_state.load() == rtsp_stream::launch_session_t::setup_state_e::started ? "running" : "starting"});
    }
    for (const auto &item : activity) if (item.client == client && result.can_switch) {
      result.can_switch = false; result.switch_blocked_reason = "your_stream";
    }
    for (const auto &weak : impl_->host_launches) if (const auto launch = weak.lock();
        launch && launch->unique_id == client && !launch->is_cancelled() && result.can_switch) {
      result.can_switch = false; result.switch_blocked_reason = "desktop_stream";
    }
    // A full budget blocks every Space this device is not already playing in.
    const bool at_capacity = result.capacity &&
      (result.capacity->active_seats >= result.capacity->max_seats ||
       result.capacity->encoder_sessions >= result.capacity->max_encoder_sessions);
    const auto catalog = impl_->controller ? impl_->controller->profile_catalog() : impl_->fallback_catalog;
    for (const auto &profile : catalog) {
      if (!impl_t::permitted(profile, client)) continue;
      std::string state = result.available ? "ready" : "unavailable";
      for (const auto &item : activity) if (item.profile == profile.id) {
        state = item.client == client ? item.state : "in_use"; break;
      }
      profile_client_space_t space {profile.id, profile.name, state, selected == profile.id, profile.library_enabled};
      space.launcher = profile.family;
      if (state != "ready") space.blocked_reason = state;
      else if (at_capacity) space.blocked_reason = "at_capacity";
      else space.can_open = true;
      result.spaces.push_back(std::move(space));
    }
    return result;
  }

  profile_launch_result_t profile_launch_service_t::select_space(std::string_view client, std::string_view profile,
                                                                std::string_view previous) {
    const auto host_activity = spaces::try_begin_host_activity();
    if (!host_activity) return spaces_host_setup_running_result;
    std::lock_guard lock(impl_->mutex);
    const auto selected = impl_->selected_for(client);
    if (!selected) return {503, "Spaces are unavailable. Refresh and try again.", "spaces_unavailable"};
    if (client.empty() || client.size() > 128 || profile.empty() || profile.size() > 128 || previous.size() > 128)
      return {400, "Invalid Space selection.", "invalid_request"};
    const auto catalog = impl_->controller->profile_catalog();
    const auto desktops = impl_->controller->desktop_clients();
    const bool desktop = profile == "desktop" && std::find(desktops.begin(), desktops.end(), client) != desktops.end();
    if (!desktop && std::none_of(catalog.begin(), catalog.end(), [&](const auto &entry) {
          return entry.id == profile && impl_t::permitted(entry, client);
        })) return {404, "This Space is not available to this device.", "space_not_permitted",
          "Ask the host owner to allow it under Device Access in Polaris."};
    if (*selected != previous) return {409, "Your selected Space changed.", "selection_changed", "Refresh before choosing again."};
    for (const auto &weak : impl_->host_launches) if (const auto launch = weak.lock();
        launch && launch->unique_id == client && !launch->is_cancelled() && *selected != profile)
      return {409, "End your desktop stream before changing Space.", "desktop_stream_active"};
    // Re-selecting the same Space is harmless while it is active.
    if (*selected == profile) return {200, "Space selected"};
    for (const auto &weak : impl_->tracked) if (const auto launch = weak.lock();
      launch && launch->unique_id == client && !launch->is_cancelled())
      return {409, "End your stream before changing Space.", "your_stream_active"};
    for (const auto &activity : impl_->controller->profile_activity()) if (activity.client == client)
      return {409, "Wait for your Space to finish closing before changing Space.", "space_closing"};
    try {
      if (!impl_->save_selection(client, profile)) return {503, "The Space selection was not saved.", "selection_not_saved", "Refresh before retrying."};
    } catch (...) { return {503, "The Space selection was not saved.", "selection_not_saved", "Refresh before retrying."}; }
    return {200, "Space selected"};
  }

  profile_launch_result_t profile_launch_service_t::set_access(std::string profile, std::string client, bool allowed,
                                                               std::vector<std::string> paired_clients) {
    const auto host_activity = spaces::try_begin_host_activity();
    if (!host_activity) return spaces_host_setup_running_result;
    auto request = std::make_shared<impl_t::admin_request_t>();
    request->profile = std::move(profile); request->client = std::move(client); request->access = allowed;
    request->paired_clients = std::move(paired_clients);
    const auto future = request->future;
    {
      std::lock_guard lock(impl_->mutex);
      request->with_desktop = impl_->desktop_by_default;
      if (!impl_->admin.reload || !impl_->admin.access || !impl_->controller || impl_->admin_failed || impl_->stopping)
        return {503, "Space access changes are unavailable right now.", "spaces_admin_unavailable", "Refresh Spaces. If this continues, restart Polaris."};
      if (request->client.empty() || request->client.size() > 128 || request->profile.empty() || request->profile.size() > 128)
        return {400, "Invalid Space or paired device.", "invalid_request"};
      const auto catalog = impl_->controller->profile_catalog();
      if (request->profile != "desktop" && std::none_of(catalog.begin(), catalog.end(), [&](const auto &entry) { return entry.id == request->profile && !entry.archived; }))
        return {404, "Unknown Space.", "space_unknown", "Refresh Spaces."};
      if (impl_->reconfiguring || !impl_->queued.empty() || std::any_of(impl_->tracked.begin(), impl_->tracked.end(),
        [](const auto &weak) { const auto launch = weak.lock(); return launch && !launch->is_cancelled(); }))
        return {409, "Stop every Space stream and wait for cleanup before changing Spaces.", "spaces_streaming", "End the running Space streams, then try again."};
      impl_->reconfiguring = true; impl_->blocked_clients.insert(request->client);
      impl_->queued_admin = request; impl_->active_admin = request;
    }
    impl_->wake.notify_all();
    if (future.wait_for(std::chrono::seconds(25)) != std::future_status::ready)
      return {202, "Space access is still being saved. Refresh before retrying.", "spaces_change_pending"};
    return future.get();
  }

  profile_launch_result_t profile_launch_service_t::set_access_for_all(std::string profile, std::vector<std::string> clients,
    bool allowed, std::vector<std::string> paired_clients) {
    const auto host_activity = spaces::try_begin_host_activity();
    if (!host_activity) return spaces_host_setup_running_result;
    auto request = std::make_shared<impl_t::admin_request_t>();
    request->profile = std::move(profile); request->all_clients = std::move(clients); request->access = allowed;
    request->paired_clients = std::move(paired_clients);
    const auto future = request->future;
    {
      std::lock_guard lock(impl_->mutex);
      request->with_desktop = impl_->desktop_by_default;
      if (!impl_->admin.reload || !impl_->admin.access_for_all || !impl_->controller || impl_->admin_failed || impl_->stopping)
        return {503, "Space access changes are unavailable right now.", "spaces_admin_unavailable", "Refresh Spaces. If this continues, restart Polaris."};
      if (request->profile.empty() || request->profile.size() > 128 || request->all_clients->size() > 4096 ||
          std::any_of(request->all_clients->begin(), request->all_clients->end(), [](const auto &client) { return client.empty() || client.size() > 128; }))
        return {400, "Invalid Space or paired device.", "invalid_request"};
      const auto catalog = impl_->controller->profile_catalog();
      if (request->profile != "desktop" && std::none_of(catalog.begin(), catalog.end(), [&](const auto &entry) { return entry.id == request->profile && !entry.archived; }))
        return {404, "Unknown Space.", "space_unknown", "Refresh Spaces."};
      if (impl_->reconfiguring || !impl_->queued.empty() || std::any_of(impl_->tracked.begin(), impl_->tracked.end(),
        [](const auto &weak) { const auto launch = weak.lock(); return launch && !launch->is_cancelled(); }))
        return {409, "Stop every Space stream and wait for cleanup before changing Spaces.", "spaces_streaming", "End the running Space streams, then try again."};
      impl_->reconfiguring = true;
      impl_->blocked_clients.insert(request->all_clients->begin(), request->all_clients->end());
      impl_->queued_admin = request; impl_->active_admin = request;
    }
    impl_->wake.notify_all();
    if (future.wait_for(std::chrono::seconds(25)) != std::future_status::ready)
      return {202, "Space access is still being saved. Refresh before retrying.", "spaces_change_pending"};
    return future.get();
  }

  profile_launch_result_t profile_launch_service_t::create_space_profile(profiles::space_create_request_t creation) {
    if (!profiles::valid_space_create_request(creation)) return {400, "Enter a valid Space name and Steam setup.", "invalid_request"};
    const auto host_activity = spaces::try_begin_host_activity();
    if (!host_activity) return spaces_host_setup_running_result;
    std::shared_ptr<impl_t::admin_request_t> request;
    {
      std::lock_guard lock(impl_->mutex);
      if (impl_->stopping) return {503, "Spaces are shutting down.", "spaces_stopping"};
      if (impl_->active_admin && impl_->active_admin->creation &&
          impl_->active_admin->creation->request_id == creation.request_id) {
        if (*impl_->active_admin->creation != creation) return {409, "This creation request is already in use.", "creation_request_in_use"};
        request = impl_->active_admin;
      } else {
        if (!impl_->admin.reload || !impl_->admin.create || !impl_->controller || impl_->admin_failed)
          return {503, "Space creation is unavailable.", "spaces_admin_unavailable", "Refresh Spaces. If this continues, restart Polaris."};
        if (impl_->reconfiguring || !impl_->queued.empty() || std::any_of(impl_->tracked.begin(), impl_->tracked.end(),
            [](const auto &weak) { const auto launch = weak.lock(); return launch && !launch->is_cancelled(); }))
          return {409, "Stop every Space stream and wait for cleanup before changing Spaces.", "spaces_streaming", "End the running Space streams, then try again."};
        const auto catalog = impl_->controller->profile_catalog();
        // Either a launcher this PC already runs, or the Space to copy.
        if (creation.family.empty()) {
          if (std::none_of(catalog.begin(), catalog.end(), [&](const auto &entry) {
                return entry.id == creation.source_profile_id && !entry.family.empty();
              })) return {404, "Select an existing Space to base the new one on.", "space_source_unknown"};
        } else if (std::none_of(catalog.begin(), catalog.end(), [&](const auto &entry) {
              return entry.family == creation.family && !entry.archived;
            })) {
          // The first Space of a launcher has none to copy, so it needs a
          // runtime this build publishes for that family. Whether that runtime
          // is downloaded is answered where the Space is actually made.
          const auto &runtimes = spaces::trusted_runtimes();
          if (!runtimes || !spaces::choose_runtime(*runtimes, spaces::loaded_nvidia_driver(), creation.family).runtime)
            return {404, "This Polaris build has no gaming runtime for that launcher.", "space_family_unpublished"};
        }
        request = std::make_shared<impl_t::admin_request_t>();
        request->creation = std::move(creation);
        impl_->reconfiguring = true;
        impl_->active_admin = impl_->queued_admin = request;
      }
    }
    impl_->wake.notify_all();
    if (request->future.wait_for(impl_->timeout) != std::future_status::ready)
      return {202, "The Space is still being created. Refresh before retrying.", "spaces_change_pending"};
    return request->future.get();
  }

  profile_launch_result_t profile_launch_service_t::edit_profile(profiles::edit_request_t edit) {
    // Removing for good has its own entry point, its own checks and its own result.
    if (!profiles::valid_edit_request(edit) || edit.operation == profiles::edit_operation_e::remove_for_good)
      return {400, "Enter a valid Space name and operation.", "invalid_request"};
    const auto host_activity = spaces::try_begin_host_activity();
    if (!host_activity) return spaces_host_setup_running_result;
    std::shared_ptr<impl_t::admin_request_t> request;
    {
      std::lock_guard lock(impl_->mutex);
      if (impl_->stopping) return {503, "Spaces are shutting down.", "spaces_stopping"};
      if (impl_->active_admin && impl_->active_admin->edit && *impl_->active_admin->edit == edit) {
        request = impl_->active_admin;
      } else {
        if (!impl_->admin.reload || !impl_->admin.edit || !impl_->controller || impl_->admin_failed)
          return {503, "Space management is unavailable.", "spaces_admin_unavailable", "Refresh Spaces. If this continues, restart Polaris."};
        if (impl_->reconfiguring || !impl_->queued.empty() || std::any_of(impl_->tracked.begin(), impl_->tracked.end(),
            [](const auto &weak) { const auto launch = weak.lock(); return launch && !launch->is_cancelled(); }))
          return {409, "Stop every Space stream and wait for cleanup before changing Spaces.", "spaces_streaming", "End the running Space streams, then try again."};
        const auto catalog = impl_->controller->profile_catalog();
        if (std::none_of(catalog.begin(), catalog.end(), [&](const auto &entry) { return entry.id == edit.profile_id; }))
          return {404, "Space not found.", "space_unknown", "Refresh Spaces."};
        request = std::make_shared<impl_t::admin_request_t>();
        request->edit = std::move(edit);
        impl_->reconfiguring = true;
        impl_->active_admin = impl_->queued_admin = request;
      }
    }
    impl_->wake.notify_all();
    if (request->future.wait_for(impl_->timeout) != std::future_status::ready)
      return {202, "The Space change is still being saved. Refresh before retrying.", "spaces_change_pending"};
    return request->future.get();
  }

  profile_removal_result_t profile_launch_service_t::remove_space_for_good(profiles::edit_request_t removal) {
    if (removal.operation != profiles::edit_operation_e::remove_for_good || !profiles::valid_edit_request(removal))
      return {{400, "Type the Space's name to remove it for good.", "invalid_request"}};
    const auto host_activity = spaces::try_begin_host_activity();
    if (!host_activity) return {spaces_host_setup_running_result};
    std::shared_ptr<impl_t::admin_request_t> request;
    {
      std::lock_guard lock(impl_->mutex);
      if (impl_->stopping) return {{503, "Spaces are shutting down.", "spaces_stopping"}};
      if (impl_->active_admin && impl_->active_admin->edit && *impl_->active_admin->edit == removal) {
        request = impl_->active_admin;
      } else {
        for (const auto &done : impl_->removed_for_good) {
          if (done.request_id != removal.request_id) continue;
          if (done == removal) return {{200, "Space removed for good"}};
          return {{409, "This removal request was already used for another change.", "removal_request_in_use",
            "Close the dialog and try again."}};
        }
        if (!impl_->admin.reload || !impl_->admin.remove_for_good || !impl_->controller || impl_->admin_failed)
          return {{503, "Space management is unavailable.", "spaces_admin_unavailable", "Refresh Spaces. If this continues, restart Polaris."}};
        const auto catalog = impl_->controller->profile_catalog();
        const auto target = std::find_if(catalog.begin(), catalog.end(), [&](const auto &entry) { return entry.id == removal.profile_id; });
        if (target == catalog.end()) return {{404, "Space not found.", "space_unknown", "Refresh Spaces."}};
        // This Space's own stream first, in its own words, then the rule every change follows.
        const auto activity = impl_->controller->profile_activity();
        const bool target_active = std::any_of(activity.begin(), activity.end(), [&](const auto &item) { return item.profile == target->id; }) ||
          std::any_of(impl_->tracked.begin(), impl_->tracked.end(), [&](const auto &weak) {
            const auto launch = weak.lock();
            return launch && !launch->is_cancelled() && launch->worker_profile_key == target->id;
          });
        if (target_active)
          return {{409, "This Space is open on a device.", "space_active", "End that stream, then remove the Space for good."}};
        if (impl_->reconfiguring || !impl_->queued.empty() || std::any_of(impl_->tracked.begin(), impl_->tracked.end(),
            [](const auto &weak) { const auto launch = weak.lock(); return launch && !launch->is_cancelled(); }))
          return {{409, "Stop every Space stream and wait for cleanup before changing Spaces.", "spaces_streaming", "End the running Space streams, then try again."}};
        if (target->name != removal.confirm_name)
          return {{409, "The name you typed is not this Space's name.", "space_name_mismatch", "Type the Space's name exactly as it is shown."}};
        // A new Space copies an existing one of its own family, so the last
        // Space of a family stays even when another family still has one.
        if (!target->family.empty() && std::none_of(catalog.begin(), catalog.end(),
              [&](const auto &entry) { return entry.family == target->family && entry.id != target->id; }))
          return {{409, "This is the only Space, so it can be archived but not removed for good.", "space_last",
            "Create another Space first, or archive this one."}};
        request = std::make_shared<impl_t::admin_request_t>();
        request->edit = std::move(removal);
        impl_->reconfiguring = true;
        impl_->active_admin = impl_->queued_admin = request;
      }
    }
    impl_->wake.notify_all();
    if (request->future.wait_for(impl_->timeout) != std::future_status::ready)
      return {{202, "The Space is still being removed. Refresh before retrying.", "spaces_change_pending"}};
    auto result = request->future.get();
    return {result, request->kept_volume, request->kept_network};
  }

  profile_launch_result_t profile_launch_service_t::move_space_runtime(profiles::runtime_move_t move) {
    if (!profiles::valid_runtime_move(move)) return {400, "Choose a Space and the gaming runtime to move it to.", "invalid_request"};
    const auto host_activity = spaces::try_begin_host_activity();
    if (!host_activity) return spaces_host_setup_running_result;
    std::shared_ptr<impl_t::admin_request_t> request;
    {
      std::lock_guard lock(impl_->mutex);
      if (impl_->stopping) return {503, "Spaces are shutting down.", "spaces_stopping"};
      if (impl_->active_admin && impl_->active_admin->move && *impl_->active_admin->move == move) {
        request = impl_->active_admin;
      } else {
        // Another change closes the controller while it saves; say that rather than "unavailable".
        if (impl_->reconfiguring) return spaces_change_running_result;
        if (!impl_->admin.reload || !impl_->admin.move_runtime || !impl_->controller || impl_->admin_failed)
          return {503, "Space management is unavailable.", "spaces_admin_unavailable", "Refresh Spaces. If this continues, restart Polaris."};
        const auto catalog = impl_->controller->profile_catalog();
        if (std::none_of(catalog.begin(), catalog.end(), [&](const auto &entry) { return entry.id == move.profile_id; }))
          return {404, "Space not found.", "space_unknown", "Refresh Spaces."};
        const auto activity = impl_->controller->profile_activity();
        if (std::any_of(activity.begin(), activity.end(), [&](const auto &item) { return item.profile == move.profile_id; }) ||
            std::any_of(impl_->tracked.begin(), impl_->tracked.end(), [&](const auto &weak) {
              const auto launch = weak.lock();
              return launch && !launch->is_cancelled() && launch->worker_profile_key == move.profile_id;
            }))
          return space_open_for_move_result;
        if (!impl_->queued.empty() || std::any_of(impl_->tracked.begin(), impl_->tracked.end(),
            [](const auto &weak) { const auto launch = weak.lock(); return launch && !launch->is_cancelled(); }))
          return spaces_streaming_result;
        request = std::make_shared<impl_t::admin_request_t>();
        request->move = std::move(move);
        impl_->reconfiguring = true;
        impl_->active_admin = impl_->queued_admin = request;
      }
    }
    impl_->wake.notify_all();
    if (request->future.wait_for(impl_->timeout) != std::future_status::ready)
      return {202, "The Space is still being moved. Refresh before retrying.", "spaces_change_pending"};
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
  bool profile_launch_service_t::session_starting(std::string_view client) const {
    std::lock_guard lock(impl_->mutex);
    for (const auto &weak : impl_->tracked) {
      const auto launch = weak.lock();
      if (launch && !launch->is_cancelled() && launch->unique_id == client &&
          launch->setup_state.load() != rtsp_stream::launch_session_t::setup_state_e::started)
        return true;
    }
    return false;
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
        return {true, launch->session_token, launch->width, launch->height, launch->fps / 1000,
          launch->worker_library_target.empty() ? std::string(profile_app_uuid) :
            spaces::game_identity(launch->worker_profile_key, launch->worker_library_target), launch->worker_library_name};
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
