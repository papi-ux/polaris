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

    // Geometry is read the same way by the pure parser and the live observer,
    // so a recorded monitor carries exactly what was on screen — begin()
    // observes through observe_monitors(), and a record whose geometry was
    // never read would be re-stated as defaults on the way back.
    void read_monitor_geometry(const json &item, monitor_state_t &monitor) {
      if (item.contains("x") && item["x"].is_number_integer()) {
        monitor.x = item["x"].get<int>();
      }
      if (item.contains("y") && item["y"].is_number_integer()) {
        monitor.y = item["y"].get<int>();
      }
      if (item.contains("scale") && item["scale"].is_number()) {
        monitor.scale = item["scale"].get<double>();
        if (monitor.scale <= 0) {
          monitor.scale = 1.0;
        }
      }
      if (item.contains("width") && item["width"].is_number_integer() &&
          item.contains("height") && item["height"].is_number_integer()) {
        monitor.mode = std::to_string(item["width"].get<int>()) + "x" +
                       std::to_string(item["height"].get<int>());
        if (item.contains("refreshRate") && item["refreshRate"].is_number()) {
          const auto refresh = std::lround(item["refreshRate"].get<double>());
          if (refresh > 0) {
            monitor.mode += "@" + std::to_string(refresh);
          }
        }
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
        read_monitor_geometry(item, monitor.state);
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

    bool hyprctl_exits_zero(const std::vector<std::string> &argv) {
      const auto result = platf::run_process_argv_capture(
        argv,
        helper_timeout,
        4096
      );
      return result.exit_status == 0 && !result.timed_out && !result.truncated;
    }

    // hyprctl exits 0 having done nothing whenever a request meets the wrong
    // config parser: eval on a legacy config answers "eval is only supported
    // with the lua config manager" ("unknown request" before 0.55), and
    // keyword on a Lua config answers "keyword can't work with non-legacy
    // parsers" — both with rc=0. A trimmed "ok" reply is what tells a real
    // success from those, so both forms require it, and callers still verify
    // by observation.
    bool hyprctl_replies_ok(const std::vector<std::string> &argv) {
      const auto result = platf::run_process_argv_capture(
        argv,
        helper_timeout,
        4096,
        {},
        true
      );
      constexpr auto is_space = [](char ch) {
        return ch == ' ' || ch == '\n' || ch == '\r' || ch == '\t';
      };
      std::string_view reply {result.output};
      while (!reply.empty() && is_space(reply.front())) {
        reply.remove_prefix(1);
      }
      while (!reply.empty() && is_space(reply.back())) {
        reply.remove_suffix(1);
      }
      return result.exit_status == 0 && !result.timed_out && !result.truncated && reply == "ok";
    }

    // Classic `hyprctl dispatch A B` is evaluated by a Hyprland with a Lua
    // config as the Lua expression `hl.dispatch(A B)`, so every dispatch
    // fails with a Lua syntax error (rc=7) and takeover could neither begin
    // nor restore there. A dispatcher is an hl.dsp.* object run through
    // hl.dispatch, delivered by `hyprctl eval`. Older Hyprland rejects the
    // eval as an unknown dispatcher, so the classic form stays first and the
    // second hyprctl spawn is paid only where the first one already failed.
    // The retry needs Hyprland 0.56+: its hyprctl is the first that exits
    // nonzero on an error reply, so on 0.55 the classic form never reads as
    // failed and this fallback never engages.
    bool dispatch(const std::vector<std::string> &arguments) {
      std::vector<std::string> argv {"hyprctl", "dispatch"};
      argv.insert(argv.end(), arguments.begin(), arguments.end());
      if (hyprctl_exits_zero(argv)) {
        return true;
      }
      const auto dispatcher = lua_dispatcher(arguments);
      if (dispatcher.has_value() &&
          hyprctl_exits_zero({"hyprctl", "eval", "hl.dispatch(" + *dispatcher + ")"})) {
        return true;
      }
      std::string dispatch_line;
      for (const auto &argument : arguments) {
        if (!dispatch_line.empty()) {
          dispatch_line += ' ';
        }
        dispatch_line += argument;
      }
      BOOST_LOG(error) << "Desktop Takeover hyprctl dispatch ["sv << dispatch_line
                       << "] failed: classic form rejected and "
                       << (dispatcher.has_value()
                              ? "the `hyprctl eval` form exited nonzero too"sv
                              : "takeover has no hl.dsp.* translation to retry it with"sv);
      return false;
    }

    bool set_dpms(std::string_view monitor, bool enabled) {
      if (!safe_token(monitor)) {
        return false;
      }
      return dispatch({"dpms", enabled ? "on" : "off", std::string {monitor}});
    }

    bool set_monitor_enabled(const monitor_state_t &monitor, bool enabled) {
      if (!safe_token(monitor.name)) {
        return false;
      }
      const auto lua_state = lua_monitor_state(monitor, enabled);
      if (lua_state.has_value() &&
          hyprctl_replies_ok({"hyprctl", "eval", *lua_state})) {
        return true;
      }
      // The keyword form is the legacy-config spelling; it only runs after
      // eval failed to answer ok, which on a Lua config is every time.
      return hyprctl_replies_ok({"hyprctl", "keyword", "monitor",
                                 monitor.name + (enabled ? ",enable" : ",disable")});
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
        read_monitor_geometry(item, monitor);
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
        if (item.contains("windows") && item["windows"].is_number_integer()) {
          workspace.windows = item["windows"].get<int>();
        }
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
        // Geometry is optional so a record written before it was recorded
        // still reads back; enabling then re-states only what it knows.
        if (item.contains("x") && item["x"].is_number_integer()) {
          monitor.x = item["x"].get<int>();
        }
        if (item.contains("y") && item["y"].is_number_integer()) {
          monitor.y = item["y"].get<int>();
        }
        if (item.contains("mode") && item["mode"].is_string()) {
          monitor.mode = item["mode"].get<std::string>();
        }
        if (item.contains("scale") && item["scale"].is_number()) {
          monitor.scale = item["scale"].get<double>();
          if (monitor.scale <= 0) {
            monitor.scale = 1.0;
          }
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
      // A takeover of a desktop with no windowed workspaces records none,
      // and that record must survive a crash: recovery has monitors to power
      // back on even with nothing to place.
      if (state.monitors.empty()) {
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
        {"x", monitor.x},
        {"y", monitor.y},
        {"mode", monitor.mode},
        {"scale", monitor.scale},
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

  // Lua double-quoted string literal: the only two characters that can end
  // the string early or change what it means are backslash and double quote.
  std::string lua_quote(std::string_view value) {
    std::string quoted = "\"";
    for (const char ch : value) {
      if (ch == '\\' || ch == '"') {
        quoted += '\\';
      }
      quoted += ch;
    }
    quoted += '"';
    return quoted;
  }

  std::optional<std::string> lua_dispatcher(const std::vector<std::string> &arguments) {
    // The quoting below is only as trustworthy as its input is nameable:
    // refuse anything safe_token refuses, so a caller that skips its own
    // validation cannot smuggle control characters into the eval string.
    for (const auto &argument : arguments) {
      if (!safe_token(argument)) {
        return std::nullopt;
      }
    }

    // Only the dispatches takeover issues; a new one is a translation to write
    // deliberately, not a pattern to guess at.
    if (arguments.size() == 3 && arguments.front() == "dpms" &&
        (arguments[1] == "on" || arguments[1] == "off")) {
      // The table form is load-bearing: a bare string argument is not parsed
      // as `on <monitor>` but ignored, and the dispatcher then toggles every
      // monitor Polaris did not ask about.
      return "hl.dsp.dpms({ action = \"" + arguments[1] + "\", monitor = " +
             lua_quote(arguments[2]) + " })";
    }
    if (arguments.size() == 3 && arguments.front() == "moveworkspacetomonitor") {
      return "hl.dsp.workspace.move({ workspace = " + lua_quote(arguments[1]) +
             ", monitor = " + lua_quote(arguments[2]) + " })";
    }
    return std::nullopt;
  }

  std::optional<std::string> lua_monitor_state(const monitor_state_t &monitor, bool enabled) {
    if (!safe_token(monitor.name)) {
      return std::nullopt;
    }
    // Hyprland's position is two integers joined by an x; %g keeps a scale of
    // 2 an integer and 1.25 a decimal, the way a Lua number reads best.
    char scale_text[32];
    std::snprintf(scale_text, sizeof(scale_text), "%g", monitor.scale);
    std::string expression = "hl.monitor({ output = " + lua_quote(monitor.name) +
                             ", disabled = " + (enabled ? "false" : "true");
    // Geometry is re-stated only when it was actually observed: a record
    // without a mode came from a monitor that reported none, and re-stating
    // defaults would pull a desc: or catch-all-placed output off its rule.
    if (enabled && !monitor.mode.empty()) {
      expression += ", position = \"" + std::to_string(monitor.x) + "x" +
                    std::to_string(monitor.y) + "\"";
      expression += ", mode = " + lua_quote(monitor.mode);
      expression += ", scale = " + std::string {scale_text};
    }
    expression += " })";
    return expression;
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
    // Hyprland backfills a fresh empty workspace the moment the last one
    // leaves an output, so an unrecorded empty workspace on the target is not
    // a stuck restore — it is the placeholder that dies with the output when
    // the virtual display is torn down. Anything holding a window must move.
    const bool target_clear = std::all_of(current.begin(), current.end(), [&](const auto &workspace) {
      if (workspace.monitor != state.target_output) {
        return true;
      }
      const bool recorded = std::any_of(state.workspaces.begin(), state.workspaces.end(), [&](const auto &original) {
        return original.id == workspace.id && original.name == workspace.name;
      });
      return !recorded && workspace.windows == 0;
    });
    return recorded_restored && target_clear;
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
      // Every physical output leaves the layout for the session — lit or
      // already asleep — so the pointer has nowhere to escape to; the
      // recorded dpms state and geometry put each one back the way it was.
      if (monitor.state.name != target_output && !monitor.disabled) {
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
      // Hyprland deletes an empty workspace instead of moving it cross-
      // monitor, so a takeover that recorded one could never verify it onto
      // the target — right after a reboot, behind the lock screen, every
      // fresh workspace is empty and takeover refused to start. An empty
      // workspace carries nothing worth taking over or restoring: record and
      // move only workspaces that hold windows. A desktop with none recorded
      // still powers its monitors down and streams; restore has nothing to
      // place and verification already tolerates that.
      if (source_names.contains(workspace.monitor) && workspace.windows > 0) {
        state.workspaces.push_back(workspace);
      }
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
    // A single observation is not proof of placement: a dispatch Hyprland
    // accepted is not necessarily reflected in the next JSON read, and a
    // compositor busy with a fresh session can serve stale reads. Restore
    // settles the same way — require two consecutive matching observations
    // before the layout counts as placed.
    bool placement_verified = false;
    int consecutive_matches = 0;
    const auto placement_deadline = std::chrono::steady_clock::now() + std::chrono::seconds {4};
    for (int attempt = 0; attempt < 20 && std::chrono::steady_clock::now() < placement_deadline; ++attempt) {
      const auto observed = observe_workspaces();
      if (observed && takeover_layout_matches(state, *observed)) {
        if (++consecutive_matches >= 2) {
          placement_verified = true;
          break;
        }
      } else {
        consecutive_matches = 0;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds {50});
    }
    if (!placement_verified) {
      const auto observed = observe_workspaces();
      if (observed) {
        std::string observed_line;
        for (const auto &workspace : *observed) {
          if (!observed_line.empty()) {
            observed_line += ' ';
          }
          observed_line += workspace.name + "@" + workspace.monitor;
        }
        BOOST_LOG(error) << "Desktop Takeover placement never verified; observed ["sv << observed_line
                         << "] but every recorded workspace belongs on ["sv << state.target_output << "]"sv;
      } else {
        BOOST_LOG(error) << "Desktop Takeover placement never verified; Hyprland workspaces could not be read"sv;
      }
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

    // Power alone leaves the monitors in the layout: dpms is a panel state,
    // so the pointer can still travel onto a dark output and drag focus
    // there. Taking the outputs off the layout confines the pointer to the
    // streamed output for the whole session.
    for (const auto &monitor : state.monitors) {
      if (!set_monitor_enabled(monitor, false)) {
        return rollback("Desktop Takeover could not take every physical monitor off the layout; Polaris is restoring the prior layout.");
      }
    }
    bool off_canvas = false;
    int consecutive_absences = 0;
    const auto off_canvas_deadline = std::chrono::steady_clock::now() + std::chrono::seconds {4};
    for (int attempt = 0; attempt < 20 && std::chrono::steady_clock::now() < off_canvas_deadline; ++attempt) {
      const auto observed = observe_monitors();
      const bool absent = observed &&
        std::none_of(observed->begin(), observed->end(), [&](const auto &candidate) {
          return std::any_of(state.monitors.begin(), state.monitors.end(), [&](const auto &recorded) {
            return recorded.name == candidate.state.name;
          });
        });
      if (absent) {
        if (++consecutive_absences >= 2) {
          off_canvas = true;
          break;
        }
      } else {
        consecutive_absences = 0;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds {50});
    }
    if (!off_canvas) {
      return rollback("Desktop Takeover could not verify the physical monitors left the layout; Polaris is restoring the prior layout.");
    }

    BOOST_LOG(info) << "Desktop Takeover active on ["sv << state.target_output
                    << "] with "sv << state.workspaces.size() << " workspace(s) and "sv
                    << state.monitors.size() << " physical monitor(s) off the layout"sv;
    result.ready = true;
    result.recovery_state = std::move(state);
    return result;
  }

  bool restore(state_t &state, std::string *error) {
    if (!state.active) {
      return true;
    }
    bool commands_succeeded = true;
    // Takeover took the physical monitors off the layout to confine the
    // pointer; they come back before anything else. Hyprland applies a
    // monitor rule on its next frame, and a dpms naming an output it cannot
    // find hits every enabled one — dpms and the moves must wait until the
    // outputs are observably back, or a panel that was asleep could blank
    // the streamed output on its way in.
    for (const auto &monitor : state.monitors) {
      commands_succeeded = set_monitor_enabled(monitor, true) && commands_succeeded;
    }
    bool monitors_back = false;
    int consecutive_presences = 0;
    const auto return_deadline = std::chrono::steady_clock::now() + std::chrono::seconds {4};
    for (int attempt = 0; attempt < 20 && std::chrono::steady_clock::now() < return_deadline; ++attempt) {
      const auto observed = observe_monitors();
      const bool present = observed &&
        std::all_of(state.monitors.begin(), state.monitors.end(), [&](const auto &recorded) {
          return std::any_of(observed->begin(), observed->end(), [&](const auto &candidate) {
            return candidate.state.name == recorded.name;
          });
        });
      if (present) {
        if (++consecutive_presences >= 2) {
          monitors_back = true;
          break;
        }
      } else {
        consecutive_presences = 0;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds {50});
    }
    if (!monitors_back) {
      set_error(error, "Desktop Takeover re-enabled its monitors but they never returned to the layout.");
      return false;
    }
    for (const auto &monitor : state.monitors) {
      // Re-enabling an output can wake its panel, so power is set to the
      // recorded state rather than assumed.
      commands_succeeded = set_dpms(monitor.name, monitor.dpms_on) && commands_succeeded;
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
      // An empty unrecorded workspace on the target is not evicted: moving the
      // last workspace off an output makes Hyprland backfill a fresh one, so
      // restore would chase its own tail. Empty ones carry nothing and die
      // with the output during virtual-display teardown.
      if (!recorded && workspace.monitor == state.target_output && workspace.windows > 0) {
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
