/**
 * @file src/platform/linux/desktop_takeover.cpp
 * @brief Recoverable Hyprland desktop takeover for a Polaris virtual output.
 */

#include "desktop_takeover.h"

#include "misc.h"
#include "stream_path.h"
#include "virtual_display.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "src/private_state_file.h"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <nlohmann/json.hpp>
#include <set>
#include <thread>
#include <unistd.h>

using namespace std::literals;

namespace desktop_takeover {
  namespace {
    using json = nlohmann::json;
    constexpr std::size_t maximum_document_bytes = 1024 * 1024;
    constexpr auto helper_timeout = std::chrono::seconds {2};
    constexpr auto probe_cache_ttl = std::chrono::seconds {5};

    struct monitor_observation_t {
      monitor_state_t state;
      bool disabled = false;
      bool focused = false;
    };

    struct probe_cache_t {
      bool initialized = false;
      bool available = false;
      std::string reason;
      std::chrono::steady_clock::time_point observed_at {};
    };

    std::mutex probe_mutex;
    probe_cache_t probe_cache;

    std::filesystem::path state_path() {
      return platf::appdata() / "desktop_takeover_state.json";
    }

    void set_error(std::string *error, std::string value) {
      if (error) {
        *error = std::move(value);
      }
    }

    bool safe_token(std::string_view value) {
      if (value.empty()) {
        return false;
      }
      return std::none_of(value.begin(), value.end(), [](unsigned char ch) {
        return ch == '\0' || ch <= 0x20 || ch == 0x7f;
      });
    }

    bool owner_is_alive(int owner_pid) {
      if (owner_pid <= 0) {
        return false;
      }
      if (kill(owner_pid, 0) == 0) {
        return true;
      }
      return errno == EPERM;
    }

    std::optional<json> hyprctl_json(std::string_view command) {
      const auto result = platf::run_process_argv_capture(
        {"hyprctl", "-j", std::string {command}},
        helper_timeout,
        maximum_document_bytes
      );
      if (result.exit_status != 0 || result.timed_out || result.truncated) {
        return std::nullopt;
      }
      try {
        return json::parse(result.output);
      } catch (...) {
        return std::nullopt;
      }
    }

    std::optional<std::vector<monitor_observation_t>> observe_monitors() {
      const auto root = hyprctl_json("monitors");
      if (!root || !root->is_array()) {
        return std::nullopt;
      }
      std::vector<monitor_observation_t> monitors;
      for (const auto &item : *root) {
        if (!item.is_object() || !item.contains("name") || !item["name"].is_string()) {
          return std::nullopt;
        }
        monitor_observation_t monitor;
        monitor.state.name = item["name"].get<std::string>();
        if (!safe_token(monitor.state.name)) {
          return std::nullopt;
        }
        if (item.contains("dpmsStatus")) {
          if (!item["dpmsStatus"].is_boolean()) {
            return std::nullopt;
          }
          monitor.state.dpms_on = item["dpmsStatus"].get<bool>();
        }
        if (item.contains("disabled")) {
          if (!item["disabled"].is_boolean()) {
            return std::nullopt;
          }
          monitor.disabled = item["disabled"].get<bool>();
        }
        if (item.contains("focused")) {
          if (!item["focused"].is_boolean()) {
            return std::nullopt;
          }
          monitor.focused = item["focused"].get<bool>();
        }
        monitors.emplace_back(std::move(monitor));
      }
      return monitors;
    }

    std::optional<std::vector<workspace_state_t>> observe_workspaces() {
      const auto root = hyprctl_json("workspaces");
      if (!root) {
        return std::nullopt;
      }
      return parse_workspaces(root->dump());
    }

    bool dispatch(const std::vector<std::string> &arguments) {
      std::vector<std::string> argv {"hyprctl", "dispatch"};
      argv.insert(argv.end(), arguments.begin(), arguments.end());
      const auto result = platf::run_process_argv_capture(
        argv,
        helper_timeout,
        4096
      );
      return result.exit_status == 0 && !result.timed_out && !result.truncated;
    }

    bool set_dpms(std::string_view monitor, bool enabled) {
      if (!safe_token(monitor)) {
        return false;
      }
      return dispatch({"dpms", enabled ? "on" : "off", std::string {monitor}});
    }

    bool move_workspace(const workspace_state_t &workspace, std::string_view monitor) {
      const auto selector = workspace_selector(workspace);
      if (!selector || !safe_token(monitor)) {
        return false;
      }
      return dispatch({"moveworkspacetomonitor", *selector, std::string {monitor}});
    }

    bool monitor_dpms_matches(
        const std::vector<monitor_observation_t> &monitors,
        std::string_view name,
        bool expected) {
      const auto found = std::find_if(monitors.begin(), monitors.end(), [&](const auto &monitor) {
        return monitor.state.name == name;
      });
      return found != monitors.end() && found->state.dpms_on == expected;
    }

    bool persist(const state_t &state) {
      const bool written = static_cast<bool>(private_state_file::write_atomic(
        state_path(),
        serialize_state(state)
      ));
      if (written) {
        std::lock_guard lock {probe_mutex};
        probe_cache.initialized = false;
      }
      return written;
    }

    bool recovery_record_allows_takeover(std::string *reason = nullptr) {
      const auto stored = private_state_file::read_secure(
        state_path(),
        maximum_document_bytes
      );
      if (stored.status == private_state_file::read_status_e::missing) {
        return true;
      }
      if (stored.status != private_state_file::read_status_e::ok) {
        set_error(reason, "Desktop Takeover recovery state cannot be read safely.");
        return false;
      }
      if (!recovery_document_allows_takeover(stored.payload)) {
        set_error(reason, "Desktop Takeover recovery is still active or its state is malformed.");
        return false;
      }
      return true;
    }

    probe_cache_t probe_now(bool fresh_virtual_display_probe) {
      probe_cache_t result;
      result.initialized = true;
      result.observed_at = std::chrono::steady_clock::now();

      if (!stream_path::binary_on_path("hyprctl")) {
        result.reason = "Desktop Takeover requires hyprctl on PATH.";
        return result;
      }
      if (!recovery_record_allows_takeover(&result.reason)) {
        return result;
      }
      const auto backend = fresh_virtual_display_probe ?
                             virtual_display::detect_backend_fresh() :
                             virtual_display::detect_backend();
      if (backend != virtual_display::backend_e::EVDI &&
          backend != virtual_display::backend_e::WAYLAND_WLR) {
        result.reason = "Desktop Takeover requires a virtual-display backend that creates a new output.";
        return result;
      }
      const auto version = hyprctl_json("version");
      if (!version || !version->is_object()) {
        result.reason = "Polaris could not reach the active Hyprland control socket.";
        return result;
      }
      result.available = true;
      return result;
    }

    probe_cache_t availability(bool fresh) {
      std::lock_guard lock {probe_mutex};
      const auto now = std::chrono::steady_clock::now();
      if (!fresh && probe_cache.initialized &&
          now - probe_cache.observed_at < probe_cache_ttl) {
        return probe_cache;
      }
      probe_cache = probe_now(fresh);
      return probe_cache;
    }
  }  // namespace

  std::optional<std::vector<monitor_state_t>> parse_monitors(std::string_view document) {
    try {
      const auto root = json::parse(document);
      if (!root.is_array()) {
        return std::nullopt;
      }
      std::vector<monitor_state_t> monitors;
      for (const auto &item : root) {
        if (!item.is_object() || !item.contains("name") || !item["name"].is_string()) {
          return std::nullopt;
        }
        monitor_state_t monitor;
        monitor.name = item["name"].get<std::string>();
        if (!safe_token(monitor.name)) {
          return std::nullopt;
        }
        if (item.contains("dpmsStatus")) {
          if (!item["dpmsStatus"].is_boolean()) {
            return std::nullopt;
          }
          monitor.dpms_on = item["dpmsStatus"].get<bool>();
        }
        monitors.emplace_back(std::move(monitor));
      }
      return monitors;
    } catch (...) {
      return std::nullopt;
    }
  }

  std::optional<std::vector<workspace_state_t>> parse_workspaces(std::string_view document) {
    try {
      const auto root = json::parse(document);
      if (!root.is_array()) {
        return std::nullopt;
      }
      std::vector<workspace_state_t> workspaces;
      for (const auto &item : root) {
        if (!item.is_object() || !item.contains("id") || !item["id"].is_number_integer() ||
            !item.contains("name") || !item["name"].is_string() ||
            !item.contains("monitor") || !item["monitor"].is_string()) {
          return std::nullopt;
        }
        workspace_state_t workspace;
        workspace.id = item["id"].get<std::int64_t>();
        workspace.name = item["name"].get<std::string>();
        workspace.monitor = item["monitor"].get<std::string>();
        if (!safe_token(workspace.monitor) || !workspace_selector(workspace)) {
          return std::nullopt;
        }
        workspaces.emplace_back(std::move(workspace));
      }
      return workspaces;
    } catch (...) {
      return std::nullopt;
    }
  }

  std::optional<state_t> parse_state(std::string_view document) {
    try {
      const auto root = json::parse(document);
      if (!root.is_object() || root.value("version", 0) != 1 ||
          !root.contains("active") || !root["active"].is_boolean()) {
        return std::nullopt;
      }
      state_t state;
      state.active = root["active"].get<bool>();
      if (!state.active) {
        return state;
      }
      if (!root.contains("owner_pid") || !root["owner_pid"].is_number_integer() ||
          !root.contains("target_output") || !root["target_output"].is_string() ||
          !root.contains("fallback_monitor") || !root["fallback_monitor"].is_string() ||
          !root.contains("monitors") || !root["monitors"].is_array() ||
          !root.contains("workspaces") || !root["workspaces"].is_array()) {
        return std::nullopt;
      }
      state.owner_pid = root["owner_pid"].get<int>();
      state.target_output = root["target_output"].get<std::string>();
      state.fallback_monitor = root["fallback_monitor"].get<std::string>();
      if (!safe_token(state.target_output) || !safe_token(state.fallback_monitor)) {
        return std::nullopt;
      }
      for (const auto &item : root["monitors"]) {
        if (!item.is_object() || !item.contains("name") || !item["name"].is_string() ||
            !item.contains("dpms_on") || !item["dpms_on"].is_boolean()) {
          return std::nullopt;
        }
        monitor_state_t monitor {
          item["name"].get<std::string>(),
          item["dpms_on"].get<bool>(),
        };
        if (!safe_token(monitor.name)) {
          return std::nullopt;
        }
        state.monitors.emplace_back(std::move(monitor));
      }
      for (const auto &item : root["workspaces"]) {
        if (!item.is_object() || !item.contains("id") || !item["id"].is_number_integer() ||
            !item.contains("name") || !item["name"].is_string() ||
            !item.contains("monitor") || !item["monitor"].is_string()) {
          return std::nullopt;
        }
        workspace_state_t workspace {
          item["id"].get<std::int64_t>(),
          item["name"].get<std::string>(),
          item["monitor"].get<std::string>(),
        };
        if (!safe_token(workspace.monitor) || !workspace_selector(workspace)) {
          return std::nullopt;
        }
        state.workspaces.emplace_back(std::move(workspace));
      }
      if (state.monitors.empty() || state.workspaces.empty()) {
        return std::nullopt;
      }
      return state;
    } catch (...) {
      return std::nullopt;
    }
  }

  std::string serialize_state(const state_t &state) {
    json root = {
      {"version", 1},
      {"active", state.active},
      {"owner_pid", state.owner_pid},
      {"target_output", state.target_output},
      {"fallback_monitor", state.fallback_monitor},
      {"monitors", json::array()},
      {"workspaces", json::array()},
    };
    for (const auto &monitor : state.monitors) {
      root["monitors"].push_back({
        {"name", monitor.name},
        {"dpms_on", monitor.dpms_on},
      });
    }
    for (const auto &workspace : state.workspaces) {
      root["workspaces"].push_back({
        {"id", workspace.id},
        {"name", workspace.name},
        {"monitor", workspace.monitor},
      });
    }
    return root.dump();
  }

  bool recovery_document_allows_takeover(std::string_view document) {
    const auto state = parse_state(document);
    return state.has_value() && !state->active;
  }

  std::optional<std::string> workspace_selector(const workspace_state_t &workspace) {
    if (workspace.id > 0) {
      return std::to_string(workspace.id);
    }
    if (workspace.id < 0 && workspace.name.starts_with("special:") &&
        safe_token(workspace.name)) {
      return workspace.name;
    }
    return std::nullopt;
  }

  bool takeover_layout_matches(
      const state_t &state,
      const std::vector<workspace_state_t> &current) {
    return std::all_of(state.workspaces.begin(), state.workspaces.end(), [&](const auto &expected) {
      const auto found = std::find_if(current.begin(), current.end(), [&](const auto &workspace) {
        return workspace.id == expected.id && workspace.name == expected.name;
      });
      return found != current.end() && found->monitor == state.target_output;
    });
  }

  bool restored_layout_matches(
      const state_t &state,
      const std::vector<workspace_state_t> &current) {
    const bool recorded_restored = std::all_of(state.workspaces.begin(), state.workspaces.end(), [&](const auto &expected) {
      const auto found = std::find_if(current.begin(), current.end(), [&](const auto &workspace) {
        return workspace.id == expected.id && workspace.name == expected.name;
      });
      return found == current.end() || found->monitor == expected.monitor;
    });
    return recorded_restored && std::none_of(current.begin(), current.end(), [&](const auto &workspace) {
      return workspace.monitor == state.target_output;
    });
  }

  bool is_available() {
    return availability(false).available;
  }

  bool is_available_fresh() {
    return availability(true).available;
  }

  std::string unavailable_reason(bool fresh) {
    return availability(fresh).reason;
  }

  begin_result_t begin(std::string_view target_output) {
    begin_result_t result;
    if (!safe_token(target_output)) {
      result.error = "Desktop Takeover received an invalid virtual output name.";
      return result;
    }
    if (!is_available_fresh()) {
      result.error = unavailable_reason();
      return result;
    }

    // Recheck immediately before observing or persisting. Availability is a
    // UI hint and may have been cached; this is the authority that prevents a
    // failed prior recovery from ever being overwritten.
    if (!recovery_record_allows_takeover(&result.error)) {
      return result;
    }

    const auto observed_monitors = observe_monitors();
    const auto observed_workspaces = observe_workspaces();
    if (!observed_monitors || !observed_workspaces) {
      result.error = "Desktop Takeover could not read the current Hyprland layout.";
      return result;
    }
    const auto target = std::find_if(observed_monitors->begin(), observed_monitors->end(), [&](const auto &monitor) {
      return monitor.state.name == target_output && !monitor.disabled;
    });
    if (target == observed_monitors->end()) {
      result.error = "Desktop Takeover could not find the newly created virtual output in Hyprland.";
      return result;
    }

    state_t state;
    state.owner_pid = static_cast<int>(getpid());
    state.active = true;
    state.target_output = std::string {target_output};
    for (const auto &monitor : *observed_monitors) {
      if (monitor.state.name != target_output && !monitor.disabled && monitor.state.dpms_on) {
        state.monitors.push_back(monitor.state);
        if (state.fallback_monitor.empty() || monitor.focused) {
          state.fallback_monitor = monitor.state.name;
        }
      }
    }
    if (state.monitors.empty()) {
      result.error = "Desktop Takeover needs at least one active physical monitor to restore later.";
      return result;
    }
    const std::set<std::string> source_names = [&]() {
      std::set<std::string> names;
      for (const auto &monitor : state.monitors) {
        names.insert(monitor.name);
      }
      return names;
    }();
    for (const auto &workspace : *observed_workspaces) {
      if (source_names.contains(workspace.monitor)) {
        state.workspaces.push_back(workspace);
      }
    }
    if (state.workspaces.empty()) {
      result.error = "Desktop Takeover found no live workspace on the physical monitors.";
      return result;
    }
    if (!persist(state)) {
      result.error = "Desktop Takeover could not durably record the layout; no display changes were made.";
      return result;
    }
    result.recovery_state = state;

    const auto rollback = [&](std::string message) {
      result.error = std::move(message);
      std::string restore_error;
      if (!restore(state, &restore_error) && !restore_error.empty()) {
        result.error += " " + restore_error;
      }
      result.recovery_state = state;
      return result;
    };

    for (const auto &workspace : state.workspaces) {
      if (!move_workspace(workspace, state.target_output)) {
        return rollback("Desktop Takeover could not move every workspace; Polaris is restoring the prior layout.");
      }
    }
    const auto moved_workspaces = observe_workspaces();
    if (!moved_workspaces || !takeover_layout_matches(state, *moved_workspaces)) {
      return rollback("Desktop Takeover could not verify workspace placement; Polaris is restoring the prior layout.");
    }

    for (const auto &monitor : state.monitors) {
      if (monitor.dpms_on && !set_dpms(monitor.name, false)) {
        return rollback("Desktop Takeover could not power off every physical monitor; Polaris is restoring the prior layout.");
      }
    }
    const auto powered_monitors = observe_monitors();
    const bool powered_off = powered_monitors &&
      std::all_of(state.monitors.begin(), state.monitors.end(), [&](const auto &monitor) {
        return !monitor.dpms_on || monitor_dpms_matches(*powered_monitors, monitor.name, false);
    });
    if (!powered_off) {
      return rollback("Desktop Takeover could not verify monitor power state; Polaris is restoring the prior layout.");
    }

    BOOST_LOG(info) << "Desktop Takeover active on ["sv << state.target_output
                    << "] with "sv << state.workspaces.size() << " workspace(s) and "sv
                    << state.monitors.size() << " physical monitor(s) powered off"sv;
    result.ready = true;
    result.recovery_state = std::move(state);
    return result;
  }

  bool restore(state_t &state, std::string *error) {
    if (!state.active) {
      return true;
    }
    bool commands_succeeded = true;
    for (const auto &monitor : state.monitors) {
      if (monitor.dpms_on) {
        commands_succeeded = set_dpms(monitor.name, true) && commands_succeeded;
      }
    }

    auto current = observe_workspaces();
    if (!current) {
      set_error(error, "Desktop Takeover could not read workspaces while restoring.");
      return false;
    }
    for (const auto &workspace : state.workspaces) {
      const auto found = std::find_if(current->begin(), current->end(), [&](const auto &candidate) {
        return candidate.id == workspace.id && candidate.name == workspace.name;
      });
      if (found != current->end() && found->monitor != workspace.monitor) {
        commands_succeeded = move_workspace(workspace, workspace.monitor) && commands_succeeded;
      }
    }
    for (const auto &workspace : *current) {
      const bool recorded = std::any_of(state.workspaces.begin(), state.workspaces.end(), [&](const auto &original) {
        return original.id == workspace.id && original.name == workspace.name;
      });
      if (!recorded && workspace.monitor == state.target_output) {
        commands_succeeded = move_workspace(workspace, state.fallback_monitor) && commands_succeeded;
      }
    }

    // Hyprland dispatch success means accepted, not necessarily already
    // reflected in the next JSON read. Require two consecutive matching
    // observations so late placement cannot strand a workspace on an output
    // Polaris is about to destroy.
    bool restoration_stable = false;
    int consecutive_matches = 0;
    for (int attempt = 0; attempt < 20 && commands_succeeded; ++attempt) {
      const auto restored_monitors = observe_monitors();
      const auto restored_workspaces = observe_workspaces();
      const bool monitors_match = restored_monitors &&
        std::all_of(state.monitors.begin(), state.monitors.end(), [&](const auto &monitor) {
          return monitor_dpms_matches(*restored_monitors, monitor.name, monitor.dpms_on);
        });
      const bool workspaces_match = restored_workspaces &&
                                    restored_layout_matches(state, *restored_workspaces);
      if (monitors_match && workspaces_match) {
        if (++consecutive_matches >= 2) {
          restoration_stable = true;
          break;
        }
      } else {
        consecutive_matches = 0;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds {50});
    }
    if (!commands_succeeded || !restoration_stable) {
      set_error(error, "Desktop Takeover restoration could not be verified; recovery state was retained.");
      return false;
    }

    state.active = false;
    if (!persist(state)) {
      state.active = true;
      set_error(error, "Desktop Takeover restored the layout but could not retire its recovery record.");
      return false;
    }
    BOOST_LOG(info) << "Desktop Takeover layout restoration verified"sv;
    return true;
  }

  bool cleanup_stale() {
    const auto stored = private_state_file::read_secure(state_path(), maximum_document_bytes);
    if (stored.status == private_state_file::read_status_e::missing) {
      return true;
    }
    if (stored.status != private_state_file::read_status_e::ok) {
      BOOST_LOG(error) << "Desktop Takeover recovery state could not be read safely"sv;
      return false;
    }
    auto state = parse_state(stored.payload);
    if (!state) {
      BOOST_LOG(error) << "Desktop Takeover recovery state is malformed; refusing automatic display changes"sv;
      return false;
    }
    if (!state->active) {
      return true;
    }
    if (state->owner_pid != static_cast<int>(getpid()) && owner_is_alive(state->owner_pid)) {
      BOOST_LOG(warning) << "Desktop Takeover recovery belongs to a live Polaris process; leaving it untouched"sv;
      return false;
    }
    std::string restore_error;
    if (!restore(*state, &restore_error)) {
      BOOST_LOG(error) << "Desktop Takeover stale recovery failed: "sv << restore_error;
      return false;
    }
    BOOST_LOG(info) << "Desktop Takeover stale recovery completed before virtual-display cleanup"sv;
    return true;
  }

}  // namespace desktop_takeover
