/**
 * @file src/platform/linux/display_topology.cpp
 * @brief Host display topology for dongle swap + connector discovery.
 */

#include "display_topology.h"

#ifdef __linux__

  #include "src/config.h"
  #include "src/logging.h"
  #include "src/platform/common.h"
  #include "src/platform/linux/misc.h"
  #include "src/verified_action.h"
  #include "stream_display_policy.h"

  #include <algorithm>
  #include <array>
#include <cctype>
  #include <chrono>
  #include <cmath>
  #include <cstdio>
  #include <cstdlib>
  #include <filesystem>
  #include <fstream>
  #include <memory>
  #include <nlohmann/json.hpp>
  #include <string>
  #include <system_error>
  #include <thread>
  #include <vector>

using namespace std::literals;

namespace display_topology {
  namespace {

    namespace fs = std::filesystem;

    std::string read_sysfs_file(const fs::path &path) {
      std::ifstream in(path);
      if (!in) {
        return {};
      }
      std::string value;
      std::getline(in, value);
      while (!value.empty() && (value.back() == '\n' || value.back() == '\r' || value.back() == ' ')) {
        value.pop_back();
      }
      return value;
    }

    /**
     * @brief card1-HDMI-A-2 → HDMI-A-2 (kscreen-doctor / xrandr style).
     */
    std::string connector_name_from_drm(const std::string &drm_name) {
      // drm_name like "card0-DP-3" or "card1-HDMI-A-2"
      const auto dash = drm_name.find('-');
      if (dash == std::string::npos || dash + 1 >= drm_name.size()) {
        return drm_name;
      }
      return drm_name.substr(dash + 1);
    }

    bool name_looks_like_panel(const std::string &name) {
      // Internal laptop panels / eDP are almost never dongles.
      const auto lower = name;
      return lower.find("eDP") != std::string::npos ||
             lower.find("LVDS") != std::string::npos ||
             lower.find("DSI") != std::string::npos;
    }

  }  // namespace

  std::vector<output_info_t> list_outputs() {
    std::vector<output_info_t> outputs;
    std::error_code ec;
    const fs::path drm_root {"/sys/class/drm"};
    if (!fs::exists(drm_root, ec)) {
      return outputs;
    }

    for (const auto &entry : fs::directory_iterator(drm_root, ec)) {
      if (ec) {
        break;
      }
      const auto name = entry.path().filename().string();
      // Skip card nodes and writeback.
      if (name.find("card") != 0 || name.find('-') == std::string::npos) {
        continue;
      }
      if (name.find("Writeback") != std::string::npos) {
        continue;
      }
      const auto status_path = entry.path() / "status";
      if (!fs::exists(status_path)) {
        continue;
      }

      output_info_t info;
      info.drm_path = name;
      info.name = connector_name_from_drm(name);
      const auto status = read_sysfs_file(status_path);
      info.connected = (status == "connected");
      const auto enabled = read_sysfs_file(entry.path() / "enabled");
      info.enabled = (enabled == "enabled");
      // Dongle heuristic: connected external HDMI/DP that is not an internal panel.
      // Dummy plugs usually show as connected HDMI-A-* or DP-*.
      info.likely_dongle =
        info.connected &&
        !name_looks_like_panel(info.name) &&
        (info.name.find("HDMI") != std::string::npos ||
         info.name.find("DP-") != std::string::npos ||
         info.name.find("DisplayPort") != std::string::npos);
      outputs.push_back(std::move(info));
    }

    // Suggest streaming = best connected dongle-like HDMI/DP (prefer HDMI dummy plugs).
    // Suggest primary = panel/desktop output (may be disconnected — kscreen still knows the name).
    auto dongle_score = [](const output_info_t &o) {
      int s = 0;
      if (!o.likely_dongle && !(o.connected && !name_looks_like_panel(o.name))) {
        return -1;
      }
      if (o.connected) {
        s += 100;
      }
      if (o.enabled) {
        s += 10;
      }
      // Dummy plugs on lea are usually HDMI-A-*; prefer HDMI over DP for streaming.
      if (o.name.find("HDMI") != std::string::npos) {
        s += 5;
      }
      // Prefer higher connector index (HDMI-A-2 over HDMI-A-1) when both connected.
      for (char c : o.name) {
        if (c >= '0' && c <= '9') {
          s += (c - '0');
        }
      }
      return s;
    };

    std::string suggested_stream;
    int best_stream = -1;
    for (const auto &o : outputs) {
      const int sc = dongle_score(o);
      if (sc > best_stream) {
        best_stream = sc;
        suggested_stream = o.name;
      }
    }

    auto primary_score = [&](const output_info_t &o) {
      if (o.name.empty() || o.name == suggested_stream) {
        return -1;
      }
      int s = 0;
      if (name_looks_like_panel(o.name)) {
        s += 200;  // eDP/LVDS always primary candidates even if "disconnected" in sysfs
      }
      if (o.connected) {
        s += 100;
      }
      if (o.enabled) {
        s += 50;
      }
      // Prefer DP over HDMI for primary when not a panel (desktop GPU primary).
      if (o.name.find("DP-") != std::string::npos || o.name.find("DisplayPort") != std::string::npos) {
        s += 20;
      }
      if (!o.likely_dongle) {
        s += 5;
      }
      return s;
    };

    std::string suggested_primary;
    int best_primary = -1;
    for (const auto &o : outputs) {
      const int sc = primary_score(o);
      if (sc > best_primary) {
        best_primary = sc;
        suggested_primary = o.name;
      }
    }

    for (auto &o : outputs) {
      o.suggested_streaming = (!suggested_stream.empty() && o.name == suggested_stream);
      o.suggested_primary = (!suggested_primary.empty() && o.name == suggested_primary);
    }

    return outputs;
  }

  bool ensure_dongle_outputs_configured() {
    auto &cfg = config::video.linux_display;
    if (!cfg.streaming_output.empty() && !cfg.primary_output.empty() &&
        cfg.streaming_output != cfg.primary_output) {
      // Still enable auto-manage when both are already set (dongle path needs it).
      if (!cfg.auto_manage_displays) {
        cfg.auto_manage_displays = true;
        BOOST_LOG(info) << "display_topology: enabling auto_manage_displays for dongle path"sv;
      }
      return true;
    }

    const auto outputs = list_outputs();
    std::string stream;
    std::string primary;
    for (const auto &o : outputs) {
      if (o.suggested_streaming) {
        stream = o.name;
      }
      if (o.suggested_primary) {
        primary = o.name;
      }
    }

    // If streaming is set but primary is not (or vice versa), fill only the missing side.
    if (cfg.streaming_output.empty() && !stream.empty()) {
      cfg.streaming_output = stream;
      BOOST_LOG(info) << "display_topology: auto-selected streaming_output=["sv << stream << "]"sv;
    }
    if (cfg.primary_output.empty() && !primary.empty()) {
      // Avoid same-as-streaming if heuristic collided.
      if (primary == cfg.streaming_output || primary == stream) {
        for (const auto &o : outputs) {
          if (o.name != cfg.streaming_output && o.name != stream && !o.name.empty()) {
            primary = o.name;
            break;
          }
        }
      }
      if (primary != cfg.streaming_output) {
        cfg.primary_output = primary;
        BOOST_LOG(info) << "display_topology: auto-selected primary_output=["sv << primary << "]"sv;
      }
    }
    if (config::video.output_name.empty() && !cfg.streaming_output.empty()) {
      config::video.output_name = cfg.streaming_output;
    }

    const bool ok = !cfg.streaming_output.empty() && !cfg.primary_output.empty() &&
                    cfg.streaming_output != cfg.primary_output;
    if (ok && !cfg.auto_manage_displays) {
      cfg.auto_manage_displays = true;
      BOOST_LOG(info) << "display_topology: auto-enabling auto_manage_displays (dongle outputs ready)"sv;
    }
    if (!ok) {
      BOOST_LOG(warning) << "display_topology: dongle auto-detect incomplete stream=["sv
                         << cfg.streaming_output << "] primary=["sv << cfg.primary_output
                         << "] — set linux_streaming_output / linux_primary_output"sv;
    }
    return ok;
  }

  bool swap_makes_headless_primary(std::string_view swap_mode) {
    return swap_mode.empty() || swap_mode == "privacy";
  }

  bool should_manage_host_topology() {
    const auto &cfg = config::video.linux_display;
    if (!cfg.auto_manage_displays || cfg.streaming_output.empty()) {
      return false;
    }
    // Private labwc must never dim/rearrange the host desktop.
    if (cfg.use_cage_compositor) {
      return false;
    }
    // Private runtimes leave host topology alone. Dongle/legacy auto_manage both enable
    // host swap (same true fallthrough) — no separate mode branches needed.
    if (stream_display_policy::resolve_current().use_private_runtime) {
      return false;
    }
    return true;
  }

  bool output_present(const std::string &name) {
    if (name.empty()) {
      return false;
    }
    // Sysfs name match only (feeds a warning); enable still runs if absent.
    for (const auto &o : list_outputs()) {
      if (o.name == name) {
        return true;
      }
    }
    return false;
  }

  // Host ScreenCast restore token path (must match portal_grab token_path host branch).
  // Privacy blanking without this token leaves KDE waiting on a picker nobody can see.
  bool host_portal_restore_token_present() {
    std::vector<fs::path> candidates;
    if (!config::sunshine.config_file.empty()) {
      const auto slash = config::sunshine.config_file.rfind('/');
      if (slash != std::string::npos) {
        candidates.emplace_back(fs::path(config::sunshine.config_file.substr(0, slash)) /
                                "portal_restore_token_host.txt");
      }
    }
    // Always also check appdata() — matches where portal_grab persists tokens.
    candidates.emplace_back(platf::appdata() / "portal_restore_token_host.txt");

    for (const auto &path : candidates) {
      std::error_code ec;
      if (!fs::is_regular_file(path, ec)) {
        continue;
      }
      std::ifstream in(path);
      std::string token;
      std::getline(in, token);
      if (!token.empty()) {
        BOOST_LOG(info) << "display_topology: host restore token present at "sv << path.string();
        return true;
      }
    }
    BOOST_LOG(info) << "display_topology: no host restore token yet (portal bootstrap)"sv;
    return false;
  }

  int run_kscreen(std::vector<std::string> args) {
    // QT_QPA_PLATFORM=wayland avoids kscreen-doctor aborting under a tty agent
    // session; timeout guards against hangs on some hosts.
    std::vector<std::string> argv {
      "timeout", "8", "env", "QT_QPA_PLATFORM=wayland", "kscreen-doctor"
    };
    argv.insert(argv.end(), args.begin(), args.end());
    BOOST_LOG(info) << "display_topology: running kscreen-doctor with "sv << args.size() << " argument(s)"sv;
    return platf::run_process_argv(argv);
  }


  /**
   * @brief Capture stdout of a fixed command.
   */
  std::string exec_capture(const char *cmd) {
    auto pipe_closer = [](FILE *pipe) {
      if (pipe) {
        pclose(pipe);
      }
    };
    std::unique_ptr<FILE, decltype(pipe_closer)> pipe(popen(cmd, "r"), pipe_closer);
    if (!pipe) {
      return {};
    }

    std::array<char, 4096> buffer {};
    std::string result;
    while (fgets(buffer.data(), buffer.size(), pipe.get()) != nullptr) {
      result += buffer.data();
    }
    return result;
  }

  /**
   * @brief Ask kscreen what the display layout actually is right now.
   *
   * The argument list is fixed with no interpolation. This must not become a
   * place where a configured output name reaches a shell.
   */
  std::string read_kscreen_json() {
    return exec_capture("timeout 8 env QT_QPA_PLATFORM=wayland kscreen-doctor -j 2>/dev/null");
  }

  std::string current_output_mode(std::string_view kscreen_json, std::string_view output_name) {
    if (kscreen_json.empty() || output_name.empty()) {
      return {};
    }

    const auto document = nlohmann::json::parse(kscreen_json, nullptr, false);
    if (document.is_discarded() || !document.is_object()) {
      return {};
    }
    const auto outputs = document.find("outputs");
    if (outputs == document.end() || !outputs->is_array()) {
      return {};
    }

    for (const auto &output : *outputs) {
      if (!output.is_object() || output.value("name", std::string {}) != output_name) {
        continue;
      }

      const auto current_id = output.value("currentModeId", std::string {});
      const auto modes = output.find("modes");
      if (current_id.empty() || modes == output.end() || !modes->is_array()) {
        return {};
      }

      for (const auto &mode : *modes) {
        if (!mode.is_object() || mode.value("id", std::string {}) != current_id) {
          continue;
        }

        const auto size = mode.find("size");
        if (size == mode.end() || !size->is_object()) {
          return {};
        }
        const auto width = size->value("width", 0);
        const auto height = size->value("height", 0);
        if (width <= 0 || height <= 0) {
          return {};
        }

        const auto geometry = std::to_string(width) + "x" + std::to_string(height);
        const auto refresh = mode.value("refreshRate", 0.0);
        if (refresh <= 0.0) {
          return geometry;
        }
        return geometry + "@" + std::to_string(std::llround(refresh)) + "Hz";
      }

      return {};
    }

    return {};
  }

  std::string format_output_mode_arg(int width, int height, int refresh_hz) {
    if (width <= 0 || height <= 0) {
      return {};
    }

    if (refresh_hz <= 0) {
      return std::to_string(width) + "x" + std::to_string(height);
    }

    return std::to_string(width) + "x" + std::to_string(height) + "@" + std::to_string(refresh_hz) + "Hz";
  }

  bool apply_requested_display_mode(const std::string &output_name, int width, int height, int refresh_hz) {
    if (output_name.empty()) {
      return false;
    }

    const auto mode_arg = format_output_mode_arg(width, height, refresh_hz);
    if (mode_arg.empty()) {
      return false;
    }

    static constexpr auto check_id = "display_topology.output_mode";
    static constexpr auto check_description = "Set the streaming output to the mode the session asked for";

    const int rc = run_kscreen({"output." + output_name + ".mode." + mode_arg});
    if (rc != 0) {
      BOOST_LOG(warning) << "display_topology: failed to request mode ["sv << mode_arg
                         << "] on output ["sv << output_name << "] code="sv << rc;
      verified_action::confirm(check_id, check_description, mode_arg, "kscreen-doctor exited " + std::to_string(rc));
      return false;
    }

    // A zero exit means the request was accepted, not that the mode changed.
    // Ask the compositor what it is actually running instead of believing the
    // tool that just reported success.
    const auto actual = current_output_mode(read_kscreen_json(), output_name);
    if (actual.empty()) {
      BOOST_LOG(info) << "display_topology: requested mode ["sv << mode_arg << "] on output ["sv
                      << output_name << "]; could not read the mode back to confirm it"sv;
      // Not recorded as a silent failure: an unreadable read-back is unknown,
      // and claiming a mismatch we did not observe would be its own lie.
      return true;
    }

    // A request without a refresh rate says nothing about refresh, so compare
    // only what was actually asked for.
    const auto comparable = mode_arg.find('@') == std::string::npos ?
                              actual.substr(0, actual.find('@')) :
                              actual;
    if (!verified_action::confirm(check_id, check_description, mode_arg, comparable)) {
      return false;
    }

    BOOST_LOG(info) << "display_topology: mode ["sv << mode_arg << "] confirmed active on output ["sv
                    << output_name << "]"sv;
    return true;
  }

  /**
   * @brief Poll sysfs connector state with short backoff until predicate or timeout.
   * Replaces fixed sleep_for chains after kscreen-doctor (P1).
   */
  bool wait_output_state(
    const std::string &name,
    bool want_enabled,
    std::chrono::milliseconds budget,
    std::string_view what
  ) {
    if (name.empty()) {
      return false;
    }
    const auto start = std::chrono::steady_clock::now();
    const auto deadline = start + budget;
    auto backoff = std::chrono::milliseconds(50);
    const auto max_backoff = std::chrono::milliseconds(200);
    int polls = 0;
    while (std::chrono::steady_clock::now() < deadline) {
      ++polls;
      for (const auto &o : list_outputs()) {
        if (o.name != name) {
          continue;
        }
        // For enable: connected+enabled. For disable: !enabled (or disconnected).
        const bool ok = want_enabled ? (o.connected && o.enabled) : !o.enabled;
        if (ok) {
          const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - start
          );
          BOOST_LOG(info) << "display_topology: settle ok early for "sv << what
                          << " ["sv << name << "] want_enabled="sv << want_enabled
                          << " polls="sv << polls << " elapsed_ms="sv << elapsed.count();
          return true;
        }
        break;
      }
      std::this_thread::sleep_for(backoff);
      if (backoff < max_backoff) {
        backoff = std::min(max_backoff, backoff + std::chrono::milliseconds(50));
      }
    }
    BOOST_LOG(warning) << "display_topology: settle timeout for "sv << what
                       << " ["sv << name << "] want_enabled="sv << want_enabled
                       << " polls="sv << polls << " budget_ms="sv << budget.count();
    return false;
  }

  void prepare_for_stream(int width, int height, int refresh_hz) {
    if (config::video.linux_display.stream_mode == "headless_dongle" ||
        config::video.linux_display.stream_mode.empty()) {
      // This step reports whether it configured anything and that answer was
      // being dropped. A dongle setup that quietly did not run is invisible
      // until the stream comes up on the wrong output.
      verified_action::confirm(
        "display_topology.dongle_outputs_configured",
        "Configure the dongle outputs before the stream starts",
        ensure_dongle_outputs_configured()
      );
    }
    if (!should_manage_host_topology()) {
      return;
    }
    const auto &cfg = config::video.linux_display;
    const bool distinct = !cfg.primary_output.empty() && cfg.streaming_output != cfg.primary_output;
    const bool privacy = swap_makes_headless_primary(cfg.headless_swap_mode);

    if (privacy && !distinct && !cfg.primary_output.empty()) {
      BOOST_LOG(warning) << "display_topology: privacy swap needs distinct streaming_output and primary_output; enabling streaming output only"sv;
    }

    if (!output_present(cfg.streaming_output)) {
      BOOST_LOG(warning) << "display_topology: streaming output ["sv << cfg.streaming_output
                         << "] not found in sysfs; attempting kscreen enable anyway"sv;
    }

    // Staged prepare: enable dongle first and let KWin enumerate it before
    // blanking the desk. Atomic enable+disable races to "There are no outputs"
    // and KDE ScreenCast Start hangs on a placeholder 0x0 screen.
    int ret = run_kscreen({"output." + cfg.streaming_output + ".enable"});
    if (ret != 0) {
      BOOST_LOG(error) << "display_topology: enable streaming output failed code="sv << ret;
    }
    // Was fixed 1500ms sleep; poll enabled state with same overall budget.
    // The poll reads the truth out of sysfs and its verdict was being discarded,
    // so an output that never came up produced a warning and nothing else.
    verified_action::confirm(
      "display_topology.streaming_output_enabled",
      "Bring the streaming output up before capture starts",
      wait_output_state(cfg.streaming_output, true, std::chrono::milliseconds(1500), "enable_streaming")
    );
    if (width > 0 && height > 0) {
      apply_requested_display_mode(cfg.streaming_output, width, height, refresh_hz);
    }

    const bool portal_capture = config::video.capture.empty() ||
                                config::video.capture == "auto" ||
                                config::video.capture == "portal";
    // Host portal ScreenCast is 8-bit SDR. Leave the streaming output in SDR so
    // KWin dumps tone-mapped BGRx instead of an HDR compositor view.
    if (portal_capture) {
      ret = run_kscreen({
        "output." + cfg.streaming_output + ".hdr.disable",
        "output." + cfg.streaming_output + ".wcg.disable",
      });
      if (ret != 0) {
        BOOST_LOG(warning) << "display_topology: could not disable HDR/WCG on streaming output code="sv << ret;
      }
    }

    if (privacy && distinct) {
      ret = run_kscreen({"output." + cfg.streaming_output + ".priority.1"});
      if (ret != 0) {
        BOOST_LOG(error) << "display_topology: set streaming priority failed code="sv << ret;
      }
      // Priority has no cheap sysfs predicate; short settle poll for streaming still enabled.
      wait_output_state(cfg.streaming_output, true, std::chrono::milliseconds(500), "priority_streaming");

      // Portal capture on host KDE needs either a saved restore token (auto Start)
      // or a visible desk for the one-time ScreenCast picker. Blanking without a
      // token guarantees no video.
      const bool blank_primary = !portal_capture || host_portal_restore_token_present();
      if (blank_primary) {
        ret = run_kscreen({"output." + cfg.primary_output + ".disable"});
        if (ret != 0) {
          BOOST_LOG(error) << "display_topology: disable primary failed code="sv << ret
                           << " (KDE may refuse if streaming output is not yet active)"sv;
        }
        wait_output_state(cfg.primary_output, false, std::chrono::milliseconds(1500), "disable_primary");
      }
      else {
        BOOST_LOG(warning)
          << "display_topology: bootstrap — keeping primary ["sv << cfg.primary_output
          << "] enabled so host ScreenCast picker is visible; approve once to save "
             "portal_restore_token_host.txt, then privacy blanking activates"sv;
      }
    }
    else if (distinct) {
      ret = run_kscreen({
        "output." + cfg.primary_output + ".priority.1",
        "output." + cfg.streaming_output + ".priority.2",
      });
      if (ret != 0) {
        BOOST_LOG(error) << "display_topology: extended layout failed code="sv << ret;
      }
      wait_output_state(cfg.streaming_output, true, std::chrono::milliseconds(500), "extended_layout");
    }
  }

  void restore_after_stream() {
    if (!should_manage_host_topology()) {
      return;
    }
    const auto &cfg = config::video.linux_display;
    const bool distinct = !cfg.primary_output.empty() && cfg.streaming_output != cfg.primary_output;
    const bool privacy = swap_makes_headless_primary(cfg.headless_swap_mode);

    if (privacy && !distinct && !cfg.primary_output.empty()) {
      return;
    }

    std::vector<std::string> args;
    if (privacy && distinct) {
      args = {
        "output." + cfg.primary_output + ".enable",
        "output." + cfg.primary_output + ".priority.1",
        "output." + cfg.streaming_output + ".disable",
      };
    }
    else {
      if (distinct) {
        args.push_back("output." + cfg.primary_output + ".priority.1");
      }
      args.push_back("output." + cfg.streaming_output + ".priority.2");
      args.push_back("output." + cfg.streaming_output + ".disable");
    }

    const int ret = run_kscreen(args);
    if (ret != 0) {
      BOOST_LOG(error) << "display_topology: restore failed code="sv << ret;
    }
  }

}  // namespace display_topology

#endif
