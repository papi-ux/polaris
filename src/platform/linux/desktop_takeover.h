/**
 * @file src/platform/linux/desktop_takeover.h
 * @brief Recoverable Hyprland desktop takeover for a Polaris virtual output.
 */
#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace desktop_takeover {

  struct monitor_state_t {
    std::string name;
    bool dpms_on = true;
    // Geometry re-stated when the output returns to the layout: hl.monitor
    // re-enables from a rule named like the connector, so an output placed by
    // a desc: or catch-all rule would come back at defaults.
    int x = 0;
    int y = 0;
    std::string mode;  // "WxH@R", as Hyprland writes a mode
    double scale = 1.0;

    bool operator==(const monitor_state_t &) const = default;
  };

  struct workspace_state_t {
    std::int64_t id = 0;
    std::string name;
    std::string monitor;
    // Window count from Hyprland; defaults to non-empty so an unreported count
    // is moved off the target rather than left on an output Polaris destroys.
    int windows = 1;

    bool operator==(const workspace_state_t &) const = default;
  };

  struct state_t {
    int owner_pid = 0;
    bool active = false;
    std::string target_output;
    std::string fallback_monitor;
    std::vector<monitor_state_t> monitors;
    std::vector<workspace_state_t> workspaces;
  };

  struct begin_result_t {
    bool ready = false;
    std::optional<state_t> recovery_state;
    std::string error;

    explicit operator bool() const {
      return ready;
    }
  };

  /** Parse the bounded Hyprland monitor response needed by takeover. */
  std::optional<std::vector<monitor_state_t>> parse_monitors(std::string_view json);

  /** Parse workspace identity and placement from Hyprland. */
  std::optional<std::vector<workspace_state_t>> parse_workspaces(std::string_view json);

  /** Parse Polaris's durable takeover recovery document. */
  std::optional<state_t> parse_state(std::string_view json);

  /** Serialize one recovery document. */
  std::string serialize_state(const state_t &state);

  /** Only a valid inactive tombstone permits replacing an existing document. */
  bool recovery_document_allows_takeover(std::string_view json);

  /** Stable Hyprland selector for a regular or named special workspace. */
  std::optional<std::string> workspace_selector(const workspace_state_t &workspace);

  /**
   * Translate a classic `hyprctl dispatch` argument vector into the hl.dsp.*
   * dispatcher object expression a Hyprland with a Lua config evaluates, or
   * nullopt when takeover issues no such dispatch or any argument fails the
   * safe-token check.
   */
  std::optional<std::string> lua_dispatcher(const std::vector<std::string> &arguments);

  /**
   * Build the hl.monitor expression that puts an output on or off a Lua-config
   * Hyprland's layout, or nullopt when the output name fails the safe-token
   * check. A disabled output leaves the layout, which confines the pointer;
   * enabling re-states the recorded geometry so a desc: or catch-all rule
   * cannot pull the output back at defaults.
   */
  std::optional<std::string> lua_monitor_state(const monitor_state_t &monitor, bool enabled);

  /** True when every recorded workspace is on the takeover target. */
  bool takeover_layout_matches(
    const state_t &state,
    const std::vector<workspace_state_t> &current
  );

  /** True when recorded workspaces are restored and only empty unrecorded workspaces remain on the target. */
  bool restored_layout_matches(
    const state_t &state,
    const std::vector<workspace_state_t> &current
  );

  /** Host has a live Hyprland control socket and a backend that creates an output. */
  bool is_available();
  bool is_available_fresh();
  std::string unavailable_reason(bool fresh = false);

  /** Move live workspaces to @p target_output, then power off source monitors. */
  begin_result_t begin(std::string_view target_output);

  /** Restore monitor power and workspace placement from exact recorded state. */
  bool restore(state_t &state, std::string *error = nullptr);

  /** Restore an interrupted takeover before virtual-display stale cleanup. */
  bool cleanup_stale();

}  // namespace desktop_takeover
