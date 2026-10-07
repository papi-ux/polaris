/**
 * @file src/platform/linux/steamos_adapter.cpp
 * @brief #204: the production SteamOS session-switch adapter and session observer.
 */
#include "steamos_adapter.h"

#include <algorithm>

#include <sys/stat.h>
#include <unistd.h>

namespace prelaunch::steamos {
  bus::sources_t production_sources() {
    bus::sources_t sources;
    sources.uid = static_cast<std::uint32_t>(::getuid());
    return sources;
  }

  observation_t observe(const bus::sources_t &sources, const session_identity_t *since, const bus::budget_t &budget, bool prove_service) {
    observation_t out;
    snapshot_t snapshot;
    out.read = bus::read_snapshot(sources, budget, snapshot, prove_service);
    if (!out.read.ok()) return out;
    out.game_mode = game_mode_of(snapshot.game_mode);
    if (prove_service) out.service = polaris_independent_of_session(snapshot);
    session_identity_t bound;
    out.bind = bind_session(snapshot, bound);
    if (out.bind.ok()) out.session = bound;
    if (since) out.retired = retired(snapshot, *since);
    return out;
  }

  steamos_adapter_t::steamos_adapter_t(bus::sources_t sources):
      sources_ {std::move(sources)} {}

  steamos_adapter_t::~steamos_adapter_t() {
    // A child still running when the adapter goes is stopped and reaped, never left behind.
    if (child_) child_->abort();
  }

  bool steamos_adapter_t::implemented() const {
    return true;
  }

  bool steamos_adapter_t::command_present() const {
    return executable_trusted(sources_.steamosctl, sources_.trusted_owner_uid, sources_.trusted_root).ok();
  }

  verdict_t steamos_adapter_t::support(const wait_t &wait) {
    return bus::probe_support(sources_, {wait.deadline, wait.cancelled});
  }

  std::string steamos_adapter_t::fingerprint(const bus::budget_t &budget) const {
    std::string key;
    const auto add_stat = [&key](const std::filesystem::path &path) {
      struct stat st {};
      if (::lstat(path.c_str(), &st) != 0) return false;
      key += std::to_string(st.st_dev) + ':' + std::to_string(st.st_ino) + ':' + std::to_string(st.st_mode) + ':' +
             std::to_string(st.st_uid) + ':' + std::to_string(st.st_size) + ':' + std::to_string(st.st_mtim.tv_sec) + '.' +
             std::to_string(st.st_mtim.tv_nsec) + ':' + std::to_string(st.st_ctim.tv_sec) + '.' + std::to_string(st.st_ctim.tv_nsec) + '|';
      return true;
    };
    const auto boot = read_text_limited(sources_.boot_id, 64);
    if (!boot) return {};
    key += *boot + '|';
    // The command, the package database (an install or removal touches the directory), and the
    // exact desc and files of every record that could be steamos-manager's, so an in-place edit of
    // a record changes the key.
    if (!add_stat(sources_.steamosctl) || !add_stat(sources_.pacman_local)) return {};
    std::vector<std::filesystem::path> records;
    std::error_code ec;
    const std::string prefix = std::string {k_package} + "-";
    for (std::filesystem::directory_iterator it {sources_.pacman_local, ec}, end; !ec && it != end; it.increment(ec)) {
      if (it->path().filename().string().starts_with(prefix)) records.push_back(it->path());
    }
    if (ec) return {};
    std::sort(records.begin(), records.end());
    for (const auto &record : records) {
      key += record.filename().string() + '|';
      if (!add_stat(record) || !add_stat(record / "desc") || !add_stat(record / "files")) return {};
    }
    if (sources_.user.address.empty()) {
      if (!add_stat(sources_.runtime_root / std::to_string(sources_.uid) / "bus")) return {};
    } else {
      key += sources_.user.address + '|';
    }
    // A restarted or replaced manager has a new unique name: unique names are never reused on a bus.
    std::string owner;
    if (!bus::manager_owner(sources_, budget, owner)) return {};
    return key + owner;
  }

  verdict_t steamos_adapter_t::readiness_support(const wait_t &wait) {
    const bus::budget_t budget {wait.deadline, wait.cancelled};
    // The executable is checked every time: it is cheap and local.
    if (auto trusted = executable_trusted(sources_.steamosctl, sources_.trusted_owner_uid, sources_.trusted_root); !trusted.ok()) return trusted;
    {
      // A refusal or an unknown (a stalled bus) answers the rest of an app list at once, briefly; it
      // is never a positive answer and never makes a later positive one.
      std::lock_guard lock {cache_mutex_};
      if (!refusal_.ok() && std::chrono::steady_clock::now() - refusal_at_ < k_refusal_cache) return refusal_;
    }
    const auto key = fingerprint(budget);
    {
      std::lock_guard lock {cache_mutex_};
      if (!key.empty() && key == cache_key_ && std::chrono::steady_clock::now() - cache_at_ < k_readiness_cache) return cache_verdict_;
    }
    auto verdict = key.empty() && budget.stop() ?
                     verdict_t::fail(budget.expired() ? "prelaunch_session_read_timeout" : "prelaunch_cancelled",
                       "Whether this host supports leaving Game Mode could not be read in time.") :
                     bus::probe_support(sources_, budget);
    std::lock_guard lock {cache_mutex_};
    if (verdict.ok()) {
      // A positive answer is kept only under the full key it was proved with.
      if (!key.empty()) {
        cache_key_ = key;
        cache_verdict_ = verdict;
        cache_at_ = std::chrono::steady_clock::now();
      }
      refusal_ = {};
    } else {
      cache_key_.clear();
      refusal_ = verdict;
      refusal_at_ = std::chrono::steady_clock::now();
    }
    return verdict;
  }

  bool steamos_adapter_t::start(direction_e direction) {
    if (child_ && child_->state() == owned_child::state_e::running) return false;
    auto argv = command_argv(direction == direction_e::to_desktop ? switch_e::to_desktop : switch_e::to_game_mode);
    auto env = command_env(sources_.uid);
    // Fixtures point the command and its bus elsewhere; production leaves both at their fixed values.
    argv.front() = sources_.steamosctl.string();
    if (!sources_.user.address.empty()) {
      for (auto &entry : env) {
        if (entry.starts_with("DBUS_SESSION_BUS_ADDRESS=")) entry = "DBUS_SESSION_BUS_ADDRESS=" + sources_.user.address;
      }
    }
    child_ = std::make_unique<owned_child::child_t>();
    return child_->start(argv, env);
  }

  step_e steamos_adapter_t::poll() {
    if (!child_) return step_e::failed;
    switch (child_->poll()) {
      case owned_child::state_e::running: return step_e::running;
      case owned_child::state_e::exited: return child_->exit_code() == 0 ? step_e::succeeded : step_e::failed;
      default: return step_e::failed;
    }
  }

  void steamos_adapter_t::abort() {
    if (child_) child_->abort();
  }
}  // namespace prelaunch::steamos
