/**
 * @file src/platform/linux/session_launch_linux.cpp
 * @brief Private-runtime session start helpers (extract from process.cpp).
 */

#include "session_launch_linux.h"

#ifdef __linux__

  #include "src/logging.h"

  #include <boost/algorithm/string.hpp>

  #include <cctype>
  #include <string>

using namespace std::literals;

namespace session_launch_linux {
  namespace {

    void resolve_runtime_sockets(
      const stream_runtime::stream_runtime_t *rt,
      std::string &wayland_socket,
      std::string &x11_display
    ) {
      wayland_socket = rt ? rt->wayland_socket() : std::string {};
      if (wayland_socket.empty()) {
        wayland_socket = stream_runtime::labwc::wayland_socket();
      }
      x11_display = rt ? rt->x11_display() : std::string {};
      if (x11_display.empty()) {
        x11_display = stream_runtime::labwc::x11_display();
      }
    }

  }  // namespace

  std::string sanitize_cage_command(const std::string &cmd) {
    // Leading setsid breaks labwc kiosk primary client relaunch contract.
    // Match process.cpp steam_big_picture_command_prefix style (trim + istarts_with).
    auto rest = boost::trim_copy(cmd);
    if (!boost::istarts_with(rest, "setsid")) {
      return cmd;
    }
    if (rest.size() > 6 && !std::isspace(static_cast<unsigned char>(rest[6]))) {
      return cmd;
    }
    rest = boost::trim_copy(rest.substr(std::min<std::size_t>(rest.size(), 6)));
    if (boost::istarts_with(rest, "-f") &&
        (rest.size() == 2 || std::isspace(static_cast<unsigned char>(rest[2])))) {
      rest = boost::trim_copy(rest.substr(std::min<std::size_t>(rest.size(), 2)));
    }
    else if (boost::istarts_with(rest, "--fork") &&
             (rest.size() == 6 || std::isspace(static_cast<unsigned char>(rest[6])))) {
      rest = boost::trim_copy(rest.substr(std::min<std::size_t>(rest.size(), 6)));
    }
    if (rest != cmd) {
      BOOST_LOG(info) << "session_launch: stripped leading setsid for cage command ["sv
                      << cmd << "] -> ["sv << rest << ']';
    }
    return rest.empty() ? cmd : rest;
  }

  void apply_labwc_child_env(env_t &env, const stream_runtime::stream_runtime_t *rt) {
    std::string cage_socket;
    std::string cage_display;
    resolve_runtime_sockets(rt, cage_socket, cage_display);
    if (cage_socket.empty()) {
      return;
    }
    // Ownership marker for the desktop-Steam launch guard. This is deliberately
    // absent from desktop-mirror launches.
    env["POLARIS_PRIVATE_SESSION"] = "1";
    env["WAYLAND_DISPLAY"] = cage_socket;
    env["AT_SPI_BUS_ADDRESS"] = "";
    if (!cage_display.empty()) {
      env["DISPLAY"] = cage_display;
      BOOST_LOG(info) << "session_launch: labwc child env WAYLAND_DISPLAY="sv << cage_socket
                      << " DISPLAY="sv << cage_display
                      << " backend="sv << (rt ? rt->backend_id() : "labwc"sv);
    }
    else {
      env.erase("DISPLAY");
      BOOST_LOG(info) << "session_launch: labwc child env WAYLAND_DISPLAY="sv << cage_socket
                      << " (DISPLAY cleared)";
    }
  }

  void apply_gamescope_attach_env(
    env_t &env,
    const stream_runtime::stream_runtime_t *rt,
    bool enable_hdr
  ) {
    std::string cage_socket;
    std::string cage_display;
    resolve_runtime_sockets(rt, cage_socket, cage_display);
    // Idle attach often reports empty until runtime materializes the name — default
    // to gamescope-0 (portal + polaris-gamescope-idle contract). Never leave GAMESCOPE empty.
    if (cage_socket.empty()) {
      cage_socket = "gamescope-0";
    }
    if (cage_display.empty()) {
      // Steam base XWayland; STEAM_MULTIPLE_XWAYLANDS places games on base+1.
      cage_display = ":0";
    }

    // Ownership marker for the desktop-Steam launch guard. Pressure-vessel may
    // strip other session variables, so apply it at the final attach boundary.
    env["POLARIS_PRIVATE_SESSION"] = "1";

    // Prefer XWayland into gamescope for Proton; still pin GAMESCOPE_WAYLAND_DISPLAY
    // so nested helpers and steam find the right compositor. Unset host WAYLAND_DISPLAY
    // so clients do not attach to KDE/labwc wayland-0 by mistake.
    env.erase("WAYLAND_DISPLAY");
    env.erase("CLUTTER_BACKEND");
    env.erase("ELECTRON_OZONE_PLATFORM_HINT");
    env.erase("MOZ_ENABLE_WAYLAND");
    env.erase("ENABLE_GAMESCOPE_WSI");
    env.erase("ENABLE_HDR_WSI");
    env.erase("PROTON_ENABLE_WAYLAND");
    env["AT_SPI_BUS_ADDRESS"] = "";
    env["GAMESCOPE_WAYLAND_DISPLAY"] = cage_socket;
    // Steam/SDL on gamescope XWayland still need a way to find the nested display.
    // Setting WAYLAND_DISPLAY=gamescope-0 helps native-wayland children; X11 clients use DISPLAY.
    env["WAYLAND_DISPLAY"] = cage_socket;
    // Steam on first XWayland; STEAM_MULTIPLE_XWAYLANDS launches games on the next
    // (:0 → game :1). Using :1 as Steam base made games want missing :2 → black.
    env["DISPLAY"] = cage_display;
    env["GDK_BACKEND"] = "x11";
    env["SDL_VIDEODRIVER"] = "x11";
    env["XDG_SESSION_TYPE"] = "x11";
    env["QT_QPA_PLATFORM"] = "xcb";
    env["STEAM_MULTIPLE_XWAYLANDS"] = "1";
    // Match polaris-gamescope-session nested path: game must present HDR into
    // --hdr-enabled gamescope. Stream tags alone do not turn on DXVK HDR.
    if (enable_hdr) {
      env["DXVK_HDR"] = "1";
      env["PROTON_ENABLE_HDR"] = "1";
      env["STEAM_GAMESCOPE_HDR_SUPPORTED"] = "1";
      BOOST_LOG(info) << "session_launch: gamescope HDR env DXVK_HDR=1 PROTON_ENABLE_HDR=1 STEAM_GAMESCOPE_HDR_SUPPORTED=1"sv;
    }
    BOOST_LOG(info) << "session_launch: gamescope attach env DISPLAY="sv << cage_display
                    << " WAYLAND_DISPLAY="sv << cage_socket
                    << " GAMESCOPE_WAYLAND_DISPLAY="sv << cage_socket
                    << " enable_hdr="sv << (enable_hdr ? "true"sv : "false"sv);
  }

}  // namespace session_launch_linux

#endif
