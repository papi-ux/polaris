/**
 * @file src/platform/linux/spaces_runtime_move.cpp
 * @brief Moving a Space to the gaming runtime for the NVIDIA driver this PC now runs.
 */
#include "spaces_runtime_move.h"
#ifdef __linux__
#include "spaces_host_admin.h"
#include "spaces_setup_service.h"
#include "src/logging.h"

#include <algorithm>
#include <set>

namespace multiseat::spaces {
  namespace {
    using json = nlohmann::json;
    std::mutex installed_mutex;
    std::shared_ptr<move_service_t> installed;

    bool image_id(std::string_view value) {
      return value.size() == 71 && value.starts_with("sha256:") &&
        value.substr(7).find_first_not_of("0123456789abcdef") == std::string_view::npos;
    }
    bool label_word(std::string_view value) {
      return !value.empty() && value.size() <= 64 &&
        value.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") == std::string_view::npos;
    }
    bool contract(std::string_view value) {
      return !value.empty() && value.size() <= 16 && value.find_first_not_of("0123456789") == std::string_view::npos;
    }
    runtime_profile_e profile_kind(std::string_view value) {
      if (value == "steam") return runtime_profile_e::steam;
      if (value == "gamescope") return runtime_profile_e::gamescope;
      if (value == "heroic") return runtime_profile_e::heroic;
      if (value == "lutris") return runtime_profile_e::lutris;
      return runtime_profile_e::unknown;
    }
    json strict(std::string_view payload, int max_depth) {
      std::vector<std::set<std::string>> keys;
      return json::parse(payload, [&](int depth, json::parse_event_t event, json &value) {
        if (depth > max_depth) throw std::invalid_argument("nesting");
        if (event == json::parse_event_t::object_start) keys.emplace_back();
        if (event == json::parse_event_t::key && !keys.back().insert(value.get<std::string>()).second)
          throw std::invalid_argument("duplicate key");
        if (event == json::parse_event_t::object_end) keys.pop_back();
        return true;
      });
    }
    // What Docker holds for the runtime this PC's driver needs, read through the
    // setup page's cache so the two pages ask Docker once between them.
    runtime_image_e target_image(container::host_t &host, const std::vector<runtime_t> &catalog,
      const std::optional<std::string> &host_driver, runtime_inspection_cache_t *cache) {
      const auto facts = inspect_runtime(host, catalog, host_driver, true, cache);
      if (facts.status == "ready") return runtime_image_e::verified;
      if (facts.code == "not_downloaded") return runtime_image_e::absent;
      if (facts.code == "runtime_identity_mismatch") return runtime_image_e::mismatch;
      return runtime_image_e::unverifiable;
    }
    move_answer_t answer(const profile_launch_result_t &result) {
      return {result.status, std::string(result.code), std::string(result.message), std::string(result.action)};
    }
    std::string driver_text(const std::string &driver) { return driver.empty() ? "none" : driver; }
  }  // namespace

  std::optional<image_runtime_t> catalog_image_runtime(std::string_view image, const std::vector<runtime_t> &catalog) {
    const auto found = std::find_if(catalog.begin(), catalog.end(), [&](const auto &r) { return r.matches_image_id(image); });
    if (found == catalog.end()) return std::nullopt;
    return image_runtime_t {true, found->id, found->profile, found->media_contract, found->nvidia_driver,
      found->variant == "nvidia-host" ? "host" : ""};
  }

  std::optional<image_runtime_t> labeled_image_runtime(std::string_view image, std::string_view inspection) {
    if (!image_id(image) || inspection.empty() || inspection.size() > 65536) return std::nullopt;
    try {
      const auto images = strict(inspection, 12);
      if (!images.is_array() || images.size() != 1 || images[0].at("Id") != image) return std::nullopt;
      const auto &labels = images[0].at("Config").at("Labels");
      if (!labels.is_object()) return std::nullopt;
      image_runtime_t result {true, {}, labels.at("io.polaris.multiseat.profile").get<std::string>(),
        labels.at("io.polaris.multiseat.media-contract").get<std::string>(),
        labels.value("io.polaris.multiseat.nvidia.driver", std::string {}),
        labels.value("io.polaris.multiseat.nvidia.source", std::string {})};
      // A label that is present but not what Polaris writes is not repeated as a driver or a kind.
      if (!label_word(result.profile) || !contract(result.media_contract) ||
          (!result.nvidia_source.empty() && result.nvidia_source != "host") ||
          (!result.nvidia_driver.empty() && !nvidia_driver_version(result.nvidia_driver))) return std::nullopt;
      return result;
    } catch (...) { return std::nullopt; }
  }

  std::optional<image_runtime_t> image_runtime_cache_t::find(const std::string &image) const {
    std::lock_guard lock(mutex_);
    const auto found = entries_.find(image);
    return found == entries_.end() ? std::nullopt : std::optional {found->second};
  }

  void image_runtime_cache_t::remember(const std::string &image, const image_runtime_t &runtime) {
    std::lock_guard lock(mutex_);
    // A host keeps a handful of runtime images; this bound only stops a runaway.
    if (entries_.size() >= 64 && !entries_.contains(image)) entries_.erase(entries_.begin());
    entries_[image] = runtime;
  }

  image_runtime_cache_t &image_runtime_cache() {
    static image_runtime_cache_t cache;
    return cache;
  }

  image_runtime_t identify_image(container::host_t &host, std::string_view image,
    const std::vector<runtime_t> &catalog, image_runtime_cache_t *cache) {
    if (const auto known = catalog_image_runtime(image, catalog)) return *known;
    if (!image_id(image)) return {};
    const std::string key(image);
    if (cache) {
      if (const auto hit = cache->find(key)) return *hit;
    }
    auto argv = container::command_prefix({});
    argv.insert(argv.end(), {"image", "inspect", key});
    const auto result = host.run(argv, std::chrono::seconds(5), 65536);
    if (result.exit_status != 0 || result.timed_out || result.output_truncated) return {};
    const auto labeled = labeled_image_runtime(image, result.output);
    if (!labeled) return {};
    if (cache) cache->remember(key, *labeled);
    return *labeled;
  }

  bool driver_mismatch(const image_runtime_t &image, const std::optional<std::string> &host_driver) {
    // An image that borrows the machine's driver cannot be built for another
    // one, so it never mismatches.
    return image.known && image.nvidia_source != "host" && !image.nvidia_driver.empty() &&
      host_driver && !host_driver->empty() && image.nvidia_driver != *host_driver;
  }

  bool offers_host_driver(const image_runtime_t &image, const runtime_choice_t &choice) {
    // A Space still on an image built for one driver can move to one that
    // borrows this PC's, which is an upgrade rather than a repair: it keeps
    // working across the next driver update instead of stranding again.
    return image.known && image.nvidia_source != "host" && !image.nvidia_driver.empty() &&
      choice.runtime && choice.runtime->variant == "nvidia-host";
  }

  json describe_space_runtime(const image_runtime_t &image, const std::optional<std::string> &host_driver,
    const runtime_choice_t &choice, runtime_image_e target) {
    const bool mismatch = driver_mismatch(image, host_driver);
    const bool upgrade = !mismatch && offers_host_driver(image, choice);
    json result {{"runtime_driver", image.known ? json(image.nvidia_driver) : json(nullptr)},
      {"runtime_id", image.runtime_id},
      {"host_driver", host_driver && !host_driver->empty() ? json(*host_driver) : json(nullptr)},
      {"runtime_mismatch", mismatch}, {"runtime_move", nullptr}};
    if (!mismatch && !upgrade) return result;
    if (!choice.runtime) {
      result["runtime_move"] = {{"available", false}, {"code", "runtime_not_published"}};
      return result;
    }
    result["runtime_move"] = {{"available", true}, {"runtime_id", choice.runtime->id},
      {"reason", upgrade ? "host_driver_available" : "driver_mismatch"},
      {"nvidia_driver", choice.runtime->nvidia_driver}, {"installed", target == runtime_image_e::verified},
      {"code", target == runtime_image_e::verified ? "runtime_ready" : target == runtime_image_e::absent ? "not_downloaded" :
        target == runtime_image_e::mismatch ? "runtime_identity_mismatch" : "inspection_failed"}};
    return result;
  }

  json describe_space_runtimes(container::host_t &host, const std::vector<profile_summary_t> &profiles,
    const std::vector<runtime_t> &catalog, const std::optional<std::string> &host_driver,
    image_runtime_cache_t *images, runtime_inspection_cache_t *targets) {
    json result = json::object();
    // Only a host that runs a readable NVIDIA driver can disagree with a runtime.
    const bool nvidia = host_driver && !host_driver->empty();
    const auto choice = choose_runtime(catalog, host_driver);
    std::optional<runtime_image_e> target;
    for (const auto &profile : profiles) {
      image_runtime_t image;
      if (const auto known = catalog_image_runtime(profile.image, catalog)) image = *known;
      else if (nvidia && !profile.image.empty()) image = identify_image(host, profile.image, catalog, images);
      // A Space whose image this build does not list is still identified by its
      // own labels, so an upgrade can be offered rather than a dead card.
      auto state = runtime_image_e::unverifiable;
      if (choice.runtime && (driver_mismatch(image, host_driver) || offers_host_driver(image, choice))) {
        if (!target) target = target_image(host, catalog, host_driver, targets);
        state = *target;
      }
      result[profile.id] = describe_space_runtime(image, host_driver, choice, state);
    }
    return result;
  }

  bool runtime_matches_loaded_driver(std::string_view image) {
    const auto host_driver = loaded_nvidia_driver();
    if (!host_driver || host_driver->empty()) return true;
    static const std::vector<runtime_t> unpublished;
    const auto &catalog = trusted_runtimes();
    container::local_host_t host;
    const auto identity = identify_image(host, image, catalog ? *catalog : unpublished, &image_runtime_cache());
    if (!driver_mismatch(identity, host_driver)) return true;
    BOOST_LOG(warning) << "Refused a Space launch: its runtime image " << image << " was built for NVIDIA driver "
                       << identity.nvidia_driver << " and this PC runs " << *host_driver
                       << ". Move the Space to the runtime for this driver in Spaces.";
    return false;
  }

  move_decision_t decide_move(const move_facts_t &facts, std::string_view runtime_id) {
    const auto refuse = [](const profile_launch_result_t &result) { return move_decision_t {result, std::nullopt}; };
    if (!facts.admin_available)
      return refuse({503, "Space management is unavailable.", "spaces_admin_unavailable",
        "Refresh Spaces. If this continues, restart Polaris."});
    if (!facts.space) return refuse({404, "Space not found.", "space_unknown", "Refresh Spaces."});
    // What the Space and this PC are, which waiting does not change.
    if (!facts.image.known)
      return refuse({503, "Polaris could not read which NVIDIA driver this Space's gaming runtime was made for.",
        "space_runtime_unknown", "Check that Docker is running, then refresh Spaces."});
    const auto &target = facts.choice.runtime;
    // Asked again after it finished: the Space already launches the runtime for this driver.
    if (target && target->matches_image_id(facts.space->image)) return {{200, "The Space already uses the runtime for this driver"}, target};
    // A move is either a repair, when the Space was built for another driver,
    // or an upgrade onto a runtime that borrows this PC's driver and therefore
    // survives the next update.
    if (!driver_mismatch(facts.image, facts.host_driver) && !offers_host_driver(facts.image, facts.choice))
      return refuse({409, "This Space's gaming runtime already matches the NVIDIA driver on this PC.",
        "space_runtime_current", "Refresh Spaces."});
    if (!target)
      return refuse({409, "This Polaris build has no gaming runtime for the NVIDIA driver this PC runs.",
        "space_runtime_not_published", "Update Polaris, or install an NVIDIA driver this build has a runtime for."});
    if (target->id != runtime_id) return refuse(space_runtime_changed_result);
    if (facts.image.profile != target->profile || !facts.space->steam) return refuse(space_runtime_profile_mismatch_result);
    if (facts.image.media_contract != target->media_contract) return refuse(space_runtime_media_mismatch_result);
    if (facts.home_uid != target->uid || facts.home_gid != target->gid) return refuse(space_runtime_identity_mismatch_result);
    // What is happening right now, which clears on its own.
    if (facts.host_setup_running) return refuse(spaces_host_setup_running_result);
    if (facts.setup_running)
      return refuse({409, "Spaces setup is still running.", "spaces_setup_running", "Try again when it finishes."});
    if (facts.changing) return refuse(spaces_change_running_result);
    if (facts.space_active) return refuse(space_open_for_move_result);
    if (facts.streaming) return refuse(spaces_streaming_result);
    return {{202, "Moving the Space"}, target};
  }

  std::optional<move_request_t> decode_move_request(std::string_view payload) {
    if (payload.empty() || payload.size() > 4096) return std::nullopt;
    try {
      const auto body = strict(payload, 1);
      if (!body.is_object() || body.size() != 3) return std::nullopt;
      move_request_t request {body.at("request_id").get<std::string>(), body.at("profile_id").get<std::string>(),
        body.at("runtime_id").get<std::string>()};
      const bool space = !request.profile_id.empty() && request.profile_id.size() <= 128 && request.profile_id.front() != '-' &&
        request.profile_id.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_") == std::string::npos;
      const bool runtime = !request.runtime_id.empty() && request.runtime_id.size() <= 64 && request.runtime_id.front() != '-' &&
        request.runtime_id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") == std::string::npos;
      // The request identity is a lowercase UUID, as every other Spaces job takes.
      if (!space || !runtime || !profiles::valid_first_steam_request({request.request_id, "Move"})) return std::nullopt;
      return request;
    } catch (...) { return std::nullopt; }
  }

  move_service_t::move_service_t(move_operations_t operations, std::chrono::milliseconds retry_delay) :
    operations_(std::move(operations)), retry_delay_(retry_delay) {
    worker_ = std::jthread([this](std::stop_token stop) { work(stop); });
  }

  move_service_t::~move_service_t() { shutdown(); }

  void move_service_t::shutdown() {
    {
      std::lock_guard lock(mutex_);
      closing_ = true;
    }
    worker_.request_stop();
    changed_.notify_all();
    if (worker_.joinable()) worker_.join();
  }

  bool move_service_t::active() const {
    std::lock_guard lock(mutex_);
    return active_;
  }

  json move_service_t::snapshot() const {
    std::lock_guard lock(mutex_);
    if (!job_) return nullptr;
    const auto &job = *job_;
    return {{"request_id", job.request.request_id}, {"profile_id", job.request.profile_id},
      {"runtime_id", job.target.id}, {"nvidia_driver", job.target.nvidia_driver}, {"state", job.state},
      {"code", job.code}, {"message", job.message}, {"action", job.action}};
  }

  move_answer_t move_service_t::submit(const move_request_t &request) {
    {
      std::lock_guard lock(mutex_);
      if (closing_) return {503, "spaces_stopping", "Spaces are shutting down.", ""};
      if (job_ && job_->request.request_id == request.request_id) {
        if (job_->request != request)
          return {409, "move_request_in_use", "This move request was already used for another Space.", "Refresh Spaces and try again."};
        // The same request answers with its job, running or finished.
        return {job_->status, job_->code, job_->message, job_->action};
      }
      if (active_ || deciding_)
        return {409, "space_move_running", "Polaris is already moving a Space to another runtime.", "Wait for that move to finish."};
      deciding_ = true;
    }
    // Docker and the other services are asked without this lock, so the page keeps reading the job.
    move_facts_t facts;
    bool read = true;
    try { facts = operations_.facts(request); } catch (...) { read = false; }
    const auto decision = read ? decide_move(facts, request.runtime_id) :
      move_decision_t {{503, "Polaris could not read this Space's gaming runtime.", "space_runtime_unknown",
        "Check that Docker is running, then refresh Spaces."}, std::nullopt};
    std::lock_guard lock(mutex_);
    deciding_ = false;
    if (closing_) return {503, "spaces_stopping", "Spaces are shutting down.", ""};
    if (decision.result.status != 202 || !decision.target) {
      if (!decision.result.code.empty())
        BOOST_LOG(info) << "Space " << request.profile_id << " was not moved to runtime " << request.runtime_id << ": "
                        << decision.result.code;
      return answer(decision.result);
    }
    const auto &target = *decision.target;
    job_t job {request, target, profiles::runtime_move_t {request.profile_id, facts.space->image, facts.image.media_contract,
      {}, target.media_contract, profile_kind(target.profile), target.uid, target.gid}};
    job.state = job.code = facts.target == runtime_image_e::verified ? "moving" : "downloading";
    job.message = job.state == "moving" ? "Moving the Space to the new gaming runtime." :
      "Downloading the gaming runtime for this driver. You can leave this page and come back.";
    job_ = std::move(job);
    active_ = true;
    BOOST_LOG(info) << "Moving Space " << request.profile_id << " from runtime image " << facts.space->image
                    << " (NVIDIA driver " << driver_text(facts.image.nvidia_driver) << ") to runtime " << target.id
                    << " for driver " << target.nvidia_driver << "; the runtime is "
                    << (facts.target == runtime_image_e::verified ? "already downloaded" : "downloaded first");
    changed_.notify_all();
    return {202, "", "Moving the Space", ""};
  }

  void move_service_t::finish(int status, std::string state, std::string code, std::string message, std::string action) {
    job_->status = status;
    job_->state = std::move(state);
    job_->code = std::move(code);
    job_->message = std::move(message);
    job_->action = std::move(action);
    active_ = false;
  }

  void move_service_t::work(std::stop_token stop) {
    std::unique_lock lock(mutex_);
    for (;;) {
      changed_.wait(lock, stop, [&] { return active_; });
      if (stop.stop_requested()) {
        if (active_) finish(503, "failed", "spaces_stopping", "Polaris stopped before the move finished. The Space was not changed.", "Move it again after Polaris starts.");
        return;
      }
      const auto job = *job_;
      lock.unlock();
      runtime_install_result_t installed;
      try { installed = operations_.install(job.target, stop); }
      catch (...) { installed = {false, "setup_failed", "The runtime could not be checked. Retry to check again.", {}}; }
      lock.lock();
      if (stop.stop_requested()) {
        finish(503, "failed", "download_cancelled", "Polaris stopped before the move finished. The Space was not changed.", "Move it again after Polaris starts.");
        return;
      }
      // Only an image the compiled catalog approves for this runtime is ever pinned.
      if (!installed.ready || !job.target.matches_image_id(installed.image)) {
        const auto code = installed.ready || installed.code.empty() ? std::string {"runtime_verification_failed"} : installed.code;
        const auto message = installed.ready || installed.message.empty() ?
          std::string {"The runtime could not be verified, so the Space was not moved."} : installed.message;
        BOOST_LOG(warning) << "Space " << job.request.profile_id << " was not moved: runtime " << job.target.id << " is not ready (" << code << ')';
        finish(503, "failed", code, message + " The Space was not changed.", "Try the move again.");
        continue;
      }
      job_->move.to_image = installed.image;
      job_->state = job_->code = "moving";
      job_->message = "Moving the Space to the new gaming runtime.";
      const auto move = job_->move;
      lock.unlock();
      // The Spaces owner answers 202 while its change is still saving; asking again joins it.
      profile_launch_result_t moved {503, "Spaces are shutting down.", "spaces_stopping"};
      for (;;) {
        try { moved = operations_.move(move); }
        catch (...) { moved = {503, "The Space could not be moved.", "spaces_change_not_saved", "Refresh Spaces and try again."}; }
        if (moved.status != 202 || stop.stop_requested()) break;
        for (auto waited = std::chrono::milliseconds::zero(); waited < retry_delay_ && !stop.stop_requested();
             waited += std::chrono::milliseconds(10))
          std::this_thread::sleep_for(std::chrono::milliseconds(10));
      }
      lock.lock();
      if (moved.status == 200) {
        BOOST_LOG(info) << "Space " << move.profile_id << " moved to runtime " << job.target.id << " (image " << move.to_image
                        << ") for NVIDIA driver " << job.target.nvidia_driver;
        finish(200, "done", "space_runtime_moved",
          "The Space now uses the gaming runtime for this driver. Its Steam sign-in and games are unchanged.", "");
      } else {
        const std::string code = moved.status == 202 ? "spaces_stopping" : std::string(moved.code);
        BOOST_LOG(warning) << "Space " << move.profile_id << " was not moved to runtime " << job.target.id << ": "
                           << (code.empty() ? std::string {"no reason given"} : code);
        finish(moved.status == 202 ? 503 : moved.status, "failed", code.empty() ? std::string {"spaces_change_not_saved"} : code,
          std::string(moved.message), std::string(moved.action));
      }
      if (stop.stop_requested()) return;
    }
  }

  std::shared_ptr<move_service_t> make_move_service() {
    const auto catalog = trusted_runtimes().value_or(std::vector<runtime_t> {});
    return std::make_shared<move_service_t>(move_operations_t {
      .facts = [catalog](const move_request_t &request) {
        move_facts_t facts;
        const auto service = installed_profile_service();
        if (!service) return facts;
        const auto admin = service->admin_snapshot();
        facts.admin_available = admin.available && admin.runtime_move_available && !admin.failed;
        const auto found = std::find_if(admin.profiles.begin(), admin.profiles.end(),
          [&](const auto &profile) { return profile.id == request.profile_id; });
        if (found != admin.profiles.end()) facts.space = *found;
        facts.changing = admin.changing;
        facts.streaming = !admin.activity.empty();
        facts.space_active = std::any_of(admin.activity.begin(), admin.activity.end(),
          [&](const auto &item) { return item.profile == request.profile_id; });
        if (const auto setup = installed_setup_service()) {
          const auto job = setup->snapshot().value("job", json {});
          const auto state = job.is_object() ? job.value("state", std::string {}) : std::string {};
          facts.setup_running = state == "downloading" || state == "preparing" || state == "configuring";
        }
        facts.host_setup_running = host_admin_running();
        facts.host_driver = loaded_nvidia_driver();
        facts.choice = choose_runtime(catalog, facts.host_driver);
        container::local_host_t host;
        facts.home_uid = static_cast<std::uint32_t>(host.effective_uid());
        facts.home_gid = static_cast<std::uint32_t>(host.effective_gid());
        if (facts.space) facts.image = identify_image(host, facts.space->image, catalog, &image_runtime_cache());
        if (facts.choice.runtime) facts.target = target_image(host, catalog, facts.host_driver, &runtime_inspection_cache());
        return facts;
      },
      .install = [catalog](const runtime_t &target, std::stop_token stop) {
        container::local_host_t host(stop);
        auto result = install_runtime(host, target.id, catalog, stop);
        // A pull changes what Docker holds; the setup and Spaces pages ask again.
        runtime_inspection_cache().forget();
        return result;
      },
      .move = [](const profiles::runtime_move_t &move) -> profile_launch_result_t {
        const auto service = installed_profile_service();
        if (!service) return {503, "Spaces are not running.", "spaces_admin_unavailable", "Restart Polaris."};
        return service->move_space_runtime(move);
      },
    });
  }

  bool install_move_service(const std::shared_ptr<move_service_t> &service) {
    std::lock_guard lock(installed_mutex);
    if (!service || installed) return false;
    installed = service;
    return true;
  }
  void uninstall_move_service(const std::shared_ptr<move_service_t> &service) {
    std::lock_guard lock(installed_mutex);
    if (installed == service) installed.reset();
  }
  std::shared_ptr<move_service_t> installed_move_service() {
    std::lock_guard lock(installed_mutex);
    return installed;
  }
}  // namespace multiseat::spaces
#endif
