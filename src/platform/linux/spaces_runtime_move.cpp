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
#include <map>
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
      const std::optional<std::string> &host_driver, runtime_inspection_cache_t *cache,
      std::string_view profile) {
      const auto facts = inspect_runtime(host, catalog, host_driver, true, cache, profile);
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

  bool image_borrows_host_driver(container::host_t &host, std::string_view image,
    const std::vector<runtime_t> &catalog, image_runtime_cache_t *cache) {
    if (borrows_host_driver(image, catalog)) return true;
    // A Space stays on the build of its runtime it was made with after this build lists a newer
    // one. An older build of a borrowing runtime carries no driver of its own, so without the
    // machine's files its worker stopped at once ("libcuda.so.1 did not arrive") and the Space
    // only said that its runtime did not start.
    const auto identity = identify_image(host, image, catalog, cache);
    return identity.known && identity.nvidia_source == "host";
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

  bool offers_newer_runtime(const image_runtime_t &image, const runtime_choice_t &choice) {
    // A Space that borrows this PC's driver is never forced to move by a driver
    // update, which is the point of it. A driver update was also the only thing
    // that ever carried a fixed runtime to an existing Space. So a newer build
    // of the runtime a Space already uses is offered when this build carries
    // one: the same launcher and the same kind of graphics support, under
    // another identity. A change of kind is one of the other two reasons, or a
    // change of graphics card, and is not called an update here. The catalog
    // carries no order, so "newer" means "the one this build carries": after a
    // downgrade of Polaris the offer points at the older runtime that build was
    // made with, which is still the one it should run.
    if (!image.known || !choice.runtime || image.profile != choice.runtime->profile ||
        image.runtime_id == choice.runtime->id) return false;
    const auto &variant = choice.runtime->variant;
    if (image.nvidia_source == "host") return variant == "nvidia-host";
    if (!image.nvidia_driver.empty()) return variant == "nvidia" && choice.runtime->nvidia_driver == image.nvidia_driver;
    return variant == "default";
  }

  json describe_space_runtime(const image_runtime_t &image, const std::optional<std::string> &host_driver,
    const runtime_choice_t &choice, runtime_image_e target) {
    const bool mismatch = driver_mismatch(image, host_driver);
    const bool upgrade = !mismatch && offers_host_driver(image, choice);
    const bool updated = !mismatch && !upgrade && offers_newer_runtime(image, choice);
    json result {{"runtime_driver", image.known ? json(image.nvidia_driver) : json(nullptr)},
      {"runtime_id", image.runtime_id},
      {"host_driver", host_driver && !host_driver->empty() ? json(*host_driver) : json(nullptr)},
      {"runtime_mismatch", mismatch}, {"runtime_move", nullptr}};
    if (!mismatch && !upgrade && !updated) return result;
    if (!choice.runtime) {
      result["runtime_move"] = {{"available", false}, {"code", "runtime_not_published"}};
      return result;
    }
    result["runtime_move"] = {{"available", true}, {"runtime_id", choice.runtime->id},
      {"reason", mismatch ? "driver_mismatch" : upgrade ? "host_driver_available" : "runtime_updated"},
      {"nvidia_driver", choice.runtime->nvidia_driver}, {"installed", target == runtime_image_e::verified},
      {"code", target == runtime_image_e::verified ? "runtime_ready" : target == runtime_image_e::absent ? "not_downloaded" :
        target == runtime_image_e::mismatch ? "runtime_identity_mismatch" : "inspection_failed"}};
    return result;
  }

  json describe_space_runtimes(container::host_t &host, const std::vector<profile_summary_t> &profiles,
    const std::vector<runtime_t> &catalog, const std::optional<std::string> &host_driver,
    image_runtime_cache_t *images, runtime_inspection_cache_t *targets) {
    json result = json::object();
    // Each Space asks within its own launcher family, and one inspection per
    // family serves every Space that shares it.
    std::map<std::string, runtime_image_e, std::less<>> targets_seen;
    for (const auto &profile : profiles) {
      image_runtime_t image;
      if (const auto known = catalog_image_runtime(profile.image, catalog)) image = *known;
      // A Space whose image this build does not list is still identified by its
      // own labels, on any graphics. A catalog replaces a runtime's entry when
      // it is rebuilt, so that is exactly the Space a newer build is for, and
      // reading labels only on NVIDIA left an AMD or Intel Space never offered
      // one. Docker answers once for each such image and the answer is kept.
      else if (!profile.image.empty()) image = identify_image(host, profile.image, catalog, images);
      const auto family = image.profile.empty() ? std::string("steam") : image.profile;
      const auto choice = choose_runtime(catalog, host_driver, family);
      auto state = runtime_image_e::unverifiable;
      if (choice.runtime && (driver_mismatch(image, host_driver) || offers_host_driver(image, choice) ||
                             offers_newer_runtime(image, choice))) {
        const auto seen = targets_seen.find(family);
        if (seen == targets_seen.end())
          state = targets_seen.emplace(family, target_image(host, catalog, host_driver, targets, family)).first->second;
        else state = seen->second;
      }
      result[profile.id] = describe_space_runtime(image, host_driver, choice, state);
    }
    return result;
  }

  json describe_launchers(container::host_t &host, const std::vector<profile_summary_t> &profiles,
    const std::vector<runtime_t> &catalog, const std::optional<std::string> &host_driver,
    runtime_inspection_cache_t *targets) {
    json result = json::array();
    for (const std::string_view family : {"steam", "heroic", "lutris"}) {
      const bool running = std::any_of(profiles.begin(), profiles.end(), [&](const auto &profile) {
        return profile.family == family && !profile.archived;
      });
      // A launcher this PC already runs a Space for lends the next one its
      // image, so nothing is asked of the catalog or of Docker about it.
      if (running) {
        result.push_back({{"family", family}, {"has_space", true}, {"installed", true}, {"runtime_id", ""}});
        continue;
      }
      const auto choice = choose_runtime(catalog, host_driver, family);
      if (!choice.runtime) continue;
      const auto state = target_image(host, catalog, host_driver, targets, family);
      result.push_back({{"family", family}, {"has_space", false},
        {"installed", state == runtime_image_e::verified}, {"runtime_id", choice.runtime->id}});
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
    // A move is a repair, when the Space was built for another driver, an
    // upgrade onto a runtime that borrows this PC's driver and so survives the
    // next update, or an update onto the runtime this build now carries.
    if (!driver_mismatch(facts.image, facts.host_driver) && !offers_host_driver(facts.image, facts.choice) &&
        !offers_newer_runtime(facts.image, facts.choice))
      return refuse({409, "This Space's gaming runtime already matches the NVIDIA driver on this PC.",
        "space_runtime_current", "Refresh Spaces."});
    if (!target)
      return refuse({409, "This Polaris build has no gaming runtime for the NVIDIA driver this PC runs.",
        "space_runtime_not_published", "Update Polaris, or install an NVIDIA driver this build has a runtime for."});
    if (target->id != runtime_id) return refuse(space_runtime_changed_result);
    // The Space, the image it runs and the runtime it would move to must all be
    // the same launcher family, or the move would hand it another launcher.
    if (facts.image.profile != target->profile || facts.space->family != target->profile)
      return refuse(space_runtime_profile_mismatch_result);
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

  move_decision_t decide_create(const create_facts_t &facts) {
    const auto refuse = [](const profile_launch_result_t &result) { return move_decision_t {result, std::nullopt}; };
    if (!facts.admin_available)
      return refuse({503, "Space management is unavailable.", "spaces_admin_unavailable",
        "Refresh Spaces. If this continues, restart Polaris."});
    // What this build and this PC are, which waiting does not change.
    if (!facts.choice.runtime)
      return refuse({404, "This Polaris build has no gaming runtime for that launcher.", "space_family_unpublished",
        "Update Polaris, or choose a launcher it has a runtime for."});
    // What is happening right now, which clears on its own. A runtime is a few
    // gigabytes, so it is never pulled under a stream that needs the same disk and link.
    if (facts.host_setup_running) return refuse(spaces_host_setup_running_result);
    if (facts.setup_running)
      return refuse({409, "Spaces setup is still running.", "spaces_setup_running", "Try again when it finishes."});
    if (facts.changing) return refuse(spaces_change_running_result);
    if (facts.streaming) return refuse(spaces_streaming_result);
    return {{202, "Creating the Space"}, facts.choice.runtime};
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
      if (!space || !runtime || !profiles::valid_first_space_request({request.request_id, "Move"})) return std::nullopt;
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
    json result {{"kind", job.creation ? "create" : "move"},
      {"request_id", job.request.request_id}, {"profile_id", job.request.profile_id},
      {"runtime_id", job.target.id}, {"nvidia_driver", job.target.nvidia_driver}, {"state", job.state},
      {"code", job.code}, {"message", job.message}, {"action", job.action}};
    if (job.creation) {
      result["family"] = job.creation->family;
      result["name"] = job.creation->name;
    }
    return result;
  }

  move_answer_t move_service_t::submit_create(const profiles::space_create_request_t &request) {
    if (request.family.empty() || !profiles::valid_space_create_request(request))
      return {400, "space_request_invalid", "That is not a request for a launcher's first Space.", "Refresh Spaces and try again."};
    {
      std::lock_guard lock(mutex_);
      if (closing_) return {503, "spaces_stopping", "Spaces are shutting down.", ""};
      if (job_ && job_->request.request_id == request.request_id) {
        if (!job_->creation || *job_->creation != request)
          return {409, "move_request_in_use", "This request was already used for something else.", "Refresh Spaces and try again."};
        // Running or done answers with its job. One that failed starts again:
        // the identity names the Space, so the same request is the retry.
        if (job_->state != "failed") return {job_->status, job_->code, job_->message, job_->action};
      }
      if (active_ || deciding_)
        return {409, "space_move_running", "Polaris is already downloading or changing a gaming runtime.", "Wait for it to finish."};
      deciding_ = true;
    }
    create_facts_t facts;
    bool read = true;
    try { facts = operations_.create_facts(request); } catch (...) { read = false; }
    const auto decision = read ? decide_create(facts) :
      move_decision_t {{503, "Polaris could not read this PC's gaming runtimes.", "space_runtime_unknown",
        "Check that Docker is running, then refresh Spaces."}, std::nullopt};
    std::lock_guard lock(mutex_);
    deciding_ = false;
    if (closing_) return {503, "spaces_stopping", "Spaces are shutting down.", ""};
    if (decision.result.status != 202 || !decision.target) {
      BOOST_LOG(info) << "The first " << request.family << " Space was not started: " << decision.result.code;
      return answer(decision.result);
    }
    const auto &target = *decision.target;
    job_t job {{request.request_id, {}, target.id}, target, {}, request};
    job.state = job.code = facts.target == runtime_image_e::verified ? "creating" : "downloading";
    job.message = job.state == "creating" ? "Creating the Space." :
      "Downloading the gaming runtime. You can leave this page and come back.";
    job_ = std::move(job);
    active_ = true;
    BOOST_LOG(info) << "Creating the first " << request.family << " Space on runtime " << target.id << "; the runtime is "
                    << (facts.target == runtime_image_e::verified ? "already downloaded" : "downloaded first");
    changed_.notify_all();
    return {202, "", "Creating the Space", ""};
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
      "Downloading the gaming runtime. You can leave this page and come back.";
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
      // A create job leaves nothing behind when it stops short, and a move leaves the Space as it was.
      const bool creating = job_ && job_->creation;
      const std::string untouched = creating ? " The Space was not created." : " The Space was not changed.";
      const std::string again = creating ? "Create it again after Polaris starts." : "Move it again after Polaris starts.";
      const std::string stopped = std::string("Polaris stopped before the ") + (creating ? "Space was created." : "move finished.") + untouched;
      if (stop.stop_requested()) {
        if (active_) finish(503, "failed", "spaces_stopping", stopped, again);
        return;
      }
      const auto job = *job_;
      lock.unlock();
      runtime_install_result_t installed;
      try { installed = operations_.install(job.target, stop); }
      catch (...) { installed = {false, "setup_failed", "The runtime could not be checked. Retry to check again.", {}}; }
      lock.lock();
      if (stop.stop_requested()) {
        finish(503, "failed", "download_cancelled", stopped, again);
        return;
      }
      // Only an image the compiled catalog approves for this runtime is ever pinned.
      if (!installed.ready || !job.target.matches_image_id(installed.image)) {
        const auto code = installed.ready || installed.code.empty() ? std::string {"runtime_verification_failed"} : installed.code;
        const auto message = installed.ready || installed.message.empty() ?
          std::string {"The runtime could not be verified."} : installed.message;
        BOOST_LOG(warning) << (creating ? "The Space " + job.creation->name : "Space " + job.request.profile_id)
                           << " was not " << (creating ? "created" : "moved") << ": runtime " << job.target.id
                           << " is not ready (" << code << ')';
        finish(503, "failed", code, message + untouched, creating ? "Create the Space again." : "Try the move again.");
        continue;
      }
      if (creating) {
        job_->state = job_->code = "creating";
        job_->message = "Creating the Space.";
        const auto creation = *job_->creation;
        lock.unlock();
        profile_launch_result_t created {503, "Spaces are shutting down.", "spaces_stopping"};
        for (;;) {
          try { created = operations_.create(creation); }
          catch (...) { created = {503, "The Space could not be created.", "spaces_change_not_saved", "Refresh Spaces and try again."}; }
          if (created.status != 202 || stop.stop_requested()) break;
          for (auto waited = std::chrono::milliseconds::zero(); waited < retry_delay_ && !stop.stop_requested();
               waited += std::chrono::milliseconds(10))
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        lock.lock();
        if (created.status == 200) {
          BOOST_LOG(info) << "Created the first " << creation.family << " Space on runtime " << job.target.id;
          finish(200, "done", "space_created", "The Space was created.", "");
        } else {
          const std::string code = created.status == 202 ? "spaces_stopping" : std::string(created.code);
          BOOST_LOG(warning) << "The first " << creation.family << " Space was not created: "
                             << (code.empty() ? std::string {"no reason given"} : code);
          finish(created.status == 202 ? 503 : created.status, "failed", code.empty() ? std::string {"spaces_change_not_saved"} : code,
            std::string(created.message), std::string(created.action));
        }
        if (stop.stop_requested()) return;
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
          "The Space now uses its new gaming runtime. Its games, sign-in and saves are unchanged.", "");
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
        container::local_host_t host;
        facts.home_uid = static_cast<std::uint32_t>(host.effective_uid());
        facts.home_gid = static_cast<std::uint32_t>(host.effective_gid());
        if (facts.space) facts.image = identify_image(host, facts.space->image, catalog, &image_runtime_cache());
        // The Space's own family decides which runtimes are candidates, so the
        // image is read first.
        const auto family = facts.image.profile.empty() ? std::string("steam") : facts.image.profile;
        facts.choice = choose_runtime(catalog, facts.host_driver, family);
        if (facts.choice.runtime)
          facts.target = target_image(host, catalog, facts.host_driver, &runtime_inspection_cache(), family);
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
      .create_facts = [catalog](const profiles::space_create_request_t &request) {
        create_facts_t facts;
        const auto service = installed_profile_service();
        if (!service) return facts;
        const auto admin = service->admin_snapshot();
        facts.admin_available = admin.available && admin.creation_available;
        facts.changing = admin.changing;
        facts.streaming = !admin.activity.empty();
        if (const auto setup = installed_setup_service()) {
          const auto job = setup->snapshot().value("job", json {});
          const auto state = job.is_object() ? job.value("state", std::string {}) : std::string {};
          facts.setup_running = state == "downloading" || state == "preparing" || state == "configuring";
        }
        facts.host_setup_running = host_admin_running();
        const auto driver = loaded_nvidia_driver();
        facts.choice = choose_runtime(catalog, driver, request.family);
        if (facts.choice.runtime) {
          container::local_host_t host;
          facts.target = target_image(host, catalog, driver, &runtime_inspection_cache(), request.family);
        }
        return facts;
      },
      .create = [](const profiles::space_create_request_t &request) -> profile_launch_result_t {
        const auto service = installed_profile_service();
        if (!service) return {503, "Spaces are not running.", "spaces_admin_unavailable", "Restart Polaris."};
        return service->create_space_profile(request);
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
