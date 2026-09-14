#include "spaces_setup_service.h"
#ifdef __linux__
#include "spaces_setup.h"
#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

namespace multiseat::spaces {
  namespace {
    using json = nlohmann::json;
    namespace psf = private_state_file;
    std::mutex installed_mutex;
    std::shared_ptr<setup_service_t> installed;
    const std::map<std::string, std::string> messages {
      {"downloading", "Checking and downloading the gaming runtime. You can leave this page and return later."},
      {"preparing", "Preparing your private Steam home. Wait for this step to finish."},
      {"prepared", "Your Steam home is prepared. Graphics configuration and device assignment still need to be completed before you can play."},
      {"cancelled", "Setup stopped. Docker may keep verified download layers for your next retry."},
      {"interrupted", "Polaris stopped before setup finished. Retry to check the saved progress and continue."},
      {"host_prerequisites", "Host setup needs attention. Recheck Docker, graphics and controller access before retrying."},
      {"docker_unavailable", "Polaris could not reach the system Docker Engine. Recheck host setup, then retry."},
      {"download_incomplete", "The download did not finish. Retry to reuse verified layers."},
      {"runtime_verification_failed", "The runtime could not be verified. No Steam home was created."},
      {"runtime_identity_mismatch", "The installed runtime does not match this Polaris build. No Steam home was created."},
      {"runtime_not_published", "This Polaris build has no approved download for this runtime."},
      {"unsupported_platform", "This runtime requires a Linux x86-64 host."},
      {"home_incomplete", "Steam home setup did not finish. Retry checks the saved home without replacing player data. If this persists, open Doctor & Support."},
      {"setup_failed", "Setup could not finish. Retry to check the saved progress."},
    };
    json strict(std::string_view payload) {
      if (payload.empty() || payload.size() > 4096) throw std::invalid_argument("setup size");
      std::set<std::string> keys;
      return json::parse(payload, [&](int depth, json::parse_event_t event, json &value) {
        if (depth > 1) throw std::invalid_argument("setup nesting");
        if (event == json::parse_event_t::key && !keys.insert(value.get<std::string>()).second)
          throw std::invalid_argument("duplicate setup field");
        return true;
      });
    }
    bool valid_request(const setup_request_t &r) {
      if (!profiles::valid_first_steam_request({r.request_id, r.operation == "cancel" ? "Cancel" : r.name})) return false;
      if (r.operation == "cancel") return r.runtime_id.empty() && r.name.empty();
      return r.operation == "start" && !r.runtime_id.empty() && r.runtime_id.size() <= 64 &&
        r.runtime_id.front() != '-' &&
        r.runtime_id.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-") == std::string::npos;
    }
    bool digest(std::string_view value) {
      return value.size() == 71 && value.starts_with("sha256:") &&
        value.substr(7).find_first_not_of("0123456789abcdef") == std::string_view::npos;
    }
    bool retryable(std::string_view state) {
      return state == "failed" || state == "interrupted" || state == "cancelled";
    }
    bool valid_stage(std::string_view state, const std::string &code) {
      if (!messages.contains(code)) return false;
      if (state == "failed")
        return code != "prepared" && code != "preparing" && code != "downloading" &&
          code != "interrupted" && code != "cancelled";
      return (state == "prepared" || state == "preparing" || state == "downloading" ||
        state == "interrupted" || state == "cancelled") && state == code;
    }
  }

  std::optional<setup_request_t> decode_setup_request(std::string_view payload) {
    try {
      const auto body = strict(payload);
      setup_request_t r;
      r.operation = body.at("operation").get<std::string>();
      r.request_id = body.at("request_id").get<std::string>();
      if (r.operation == "start" && body.size() == 4) {
        r.runtime_id = body.at("runtime_id").get<std::string>();
        r.name = body.at("name").get<std::string>();
      } else if (r.operation != "cancel" || body.size() != 2) return {};
      return valid_request(r) ? std::optional {r} : std::nullopt;
    } catch (...) { return {}; }
  }

  setup_service_t::setup_service_t(std::filesystem::path journal, std::vector<runtime_t> catalog,
    bool enabled, setup_operations_t operations) :
    journal_(std::move(journal)), catalog_(std::move(catalog)), operations_(std::move(operations)), enabled_(enabled) {
    // An empty packaged catalog cannot start a job or create setup state.
    if (!enabled_ || catalog_.empty()) return;
    try {
      const auto owner_path = std::filesystem::path(journal_.string() + ".owner");
      constexpr std::string_view owner = R"({"schema":1})";
      if (!psf::update_atomic(owner_path, 64, [&](const psf::read_result_t &existing) -> std::optional<std::string> {
            if (existing.status != psf::read_status_e::missing && (!existing || existing.payload != owner)) return {};
            return std::string(owner);
          })) { fault_ = true; return; }
      auto owned = psf::read_with_lease(owner_path, 64);
      if (!owned.read || owned.read.payload != owner || !owned.lease) { fault_ = true; return; }
      lease_ = std::move(owned.lease);
      const auto saved = psf::read_secure(journal_, 4096, false, false);
      if (saved) {
        const auto body = strict(saved.payload);
        if (body.size() != 9 || !body.at("schema").is_number_unsigned() || body.at("schema") != 1)
          throw std::invalid_argument("setup schema");
        record_t r {{body.at("operation"), body.at("request_id"), body.at("runtime_id"), body.at("name")},
          body.at("reference"), body.at("image"), body.at("state"), body.at("code")};
        const std::string prefix = "ghcr.io/papi-ux/polaris-worker-steam@";
        if (!valid_request(r.request) || r.request.operation != "start" ||
            !r.reference.starts_with(prefix) || !digest(r.reference.substr(prefix.size())) || !digest(r.image) ||
            !valid_stage(r.state, r.code))
          throw std::invalid_argument("setup record");
        record_ = std::move(r);
        if (record_->state == "downloading" || record_->state == "preparing") {
          record_->state = "interrupted"; record_->code = "interrupted";
          if (!save_locked()) return;
        }
      } else if (saved.status != psf::read_status_e::missing) { fault_ = true; return; }
      worker_ = std::thread([this] { work(); });
    } catch (...) { fault_ = true; }
  }

  setup_service_t::~setup_service_t() { shutdown(); }

  bool setup_service_t::save_locked() {
    const auto &r = *record_;
    try {
      const json body {{"schema", 1}, {"operation", "start"}, {"request_id", r.request.request_id},
        {"runtime_id", r.request.runtime_id}, {"name", r.request.name}, {"reference", r.reference},
        {"image", r.image}, {"state", r.state}, {"code", r.code}};
      if (psf::write_atomic(journal_, body.dump())) return true;
    } catch (...) {}
    // A rename with uncertain durability may have committed. Freeze this owner;
    // the next process reads the journal instead of overwriting uncertain state.
    fault_ = true;
    return false;
  }

  json setup_service_t::snapshot() const {
    std::lock_guard lock(mutex_);
    json runtimes = json::array();
    for (const auto &r : catalog_) runtimes.push_back({{"id", r.id}, {"variant", r.variant}, {"nvidia_driver", r.nvidia_driver}});
    json result {{"version", 1}, {"available", enabled_ && !fault_ && !closing_ && bool(lease_)},
      {"runtimes", runtimes}, {"job", nullptr},
      {"message", !enabled_ ? "Spaces already have local configuration. Manage your existing spaces below." :
        fault_ ? "Saved setup state could not be secured. Restart Polaris after saving your work. If this persists, open Doctor & Support." :
        catalog_.empty() ? "The verified gaming runtime is not published for this preview yet." : ""}};
    if (record_) {
      const auto &r = *record_;
      const bool approved = std::any_of(catalog_.begin(), catalog_.end(), [&](const auto &runtime) {
        return runtime.id == r.request.runtime_id && runtime.reference() == r.reference && runtime.config_digest == r.image;
      });
      result["job"] = {{"request_id", r.request.request_id}, {"runtime_id", r.request.runtime_id},
        {"name", r.request.name}, {"state", fault_ ? "recovery_required" : r.state},
        {"message", fault_ ? result["message"].get<std::string>() : !approved && retryable(r.state) ?
          "This build no longer offers the runtime saved for this setup. Existing player data is preserved." : messages.at(r.code)},
        {"can_retry", approved && !fault_ && !closing_ && !active_ && retryable(r.state)},
        {"can_cancel", !fault_ && !closing_ && active_ && r.state == "downloading" && !cancellation_.stop_requested()}};
    }
    return result;
  }

  int setup_service_t::submit(const setup_request_t &request) {
    if (!valid_request(request)) return 400;
    std::lock_guard lock(mutex_);
    if (!enabled_ || fault_ || closing_ || !lease_) return 503;
    if (request.operation == "cancel") {
      if (!record_ || record_->request.request_id != request.request_id) return 409;
      if (!active_) return 200;
      if (record_->state != "downloading") return 409;
      cancellation_.request_stop();
      return 202;
    }
    const auto found = std::find_if(catalog_.begin(), catalog_.end(), [&](const auto &r) { return r.id == request.runtime_id; });
    if (found == catalog_.end()) return 409;
    if (record_) {
      const auto &old = record_->request;
      if (old.request_id != request.request_id || old.name != request.name || old.runtime_id != request.runtime_id ||
          record_->reference != found->reference() || record_->image != found->config_digest) return 409;
      if (active_) return 202;
      if (record_->state == "prepared") return 200;
      if (!retryable(record_->state)) return 409;
    }
    record_ = record_t {request, found->reference(), found->config_digest, "downloading", "downloading"};
    if (!save_locked()) return 503;
    cancellation_ = std::stop_source {};
    active_ = true;
    changed_.notify_one();
    return 202;
  }

  void setup_service_t::work() {
    std::unique_lock lock(mutex_);
    for (;;) {
      changed_.wait(lock, [&] { return closing_ || active_; });
      if (closing_ && !active_) return;
      const auto request = record_->request;
      const auto stop = cancellation_.get_token();
      lock.unlock();
      runtime_install_result_t runtime;
      try { if (!stop.stop_requested()) runtime = operations_.install(request.runtime_id, stop); }
      catch (...) { runtime.code = "setup_failed"; }
      lock.lock();
      if (stop.stop_requested() || !runtime.ready || runtime.image != record_->image) {
        record_->state = stop.stop_requested() ? "cancelled" : "failed";
        record_->code = stop.stop_requested() ? "cancelled" :
          messages.contains(runtime.code) && !runtime.ready ? runtime.code : "runtime_verification_failed";
        save_locked();
      } else {
        // Fence UI cancellation before provisioning. Shutdown still interrupts
        // bounded host commands; uncertain resources are retained for recovery.
        record_->state = "preparing"; record_->code = "preparing";
        if (save_locked()) {
          lock.unlock();
          bool prepared = false;
          try { prepared = operations_.prepare({request.request_id, request.name}, runtime.image, stop); }
          catch (...) {}
          lock.lock();
          record_->state = prepared ? "prepared" : stop.stop_requested() ? "interrupted" : "failed";
          record_->code = prepared ? "prepared" : stop.stop_requested() ? "interrupted" : "home_incomplete";
          save_locked();
        }
      }
      active_ = false;
      if (closing_) return;
    }
  }

  void setup_service_t::shutdown() {
    std::lock_guard shutdown_lock(shutdown_mutex_);
    {
      std::lock_guard lock(mutex_);
      closing_ = true;
      cancellation_.request_stop();
      changed_.notify_one();
    }
    if (worker_.joinable()) worker_.join();
  }

  std::shared_ptr<setup_service_t> make_setup_service(const std::filesystem::path &directory, bool enabled) {
    const auto catalog = trusted_runtimes().value_or(std::vector<runtime_t> {});
    return std::make_shared<setup_service_t>(directory / "spaces-setup.json", catalog, enabled,
      setup_operations_t {
        .install = [catalog](std::string_view id, std::stop_token stop) {
          container::local_host_t host(stop);
          if (!inspect_setup(host, false, false).at("host_prerequisites_ready").get<bool>())
            return runtime_install_result_t {false, "host_prerequisites", {}, {}};
          return install_runtime(host, id, catalog, stop);
        },
        .prepare = [path = directory / "spaces-profiles.json"](const auto &request, std::string_view image, std::stop_token stop) {
          container::local_host_t host(stop);
          return static_cast<bool>(profiles::create_first_steam(path, request, image, host));
        },
      });
  }

  bool install_setup_service(const std::shared_ptr<setup_service_t> &service) {
    std::lock_guard lock(installed_mutex);
    if (!service || installed) return false;
    installed = service;
    return true;
  }
  void uninstall_setup_service(const std::shared_ptr<setup_service_t> &service) {
    std::lock_guard lock(installed_mutex);
    if (installed == service) installed.reset();
  }
  std::shared_ptr<setup_service_t> installed_setup_service() {
    std::lock_guard lock(installed_mutex);
    return installed;
  }
}
#endif
