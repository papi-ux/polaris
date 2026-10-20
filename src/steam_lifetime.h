#pragma once

#include <stdexcept>
#include <regex>
#include <string>
#include <string_view>

namespace proc {
  inline bool standalone_desktop_steam_launch(const std::string &command) {
    // Only the managed CLI launch can become desktop-owned. Shell follow-ups
    // and custom scripts retain their generation stamp and stop with the stream.
    static const std::regex launch {
      R"(^[ \t]*(?:setsid[ \t]+(?:-f[ \t]+)?)?steam[ \t]+(?:-gamepadui|steam://open/bigpicture|steam://rungameid/[0-9]+|-applaunch[ \t]+[0-9]+)[ \t]*$)",
      std::regex::icase
    };
    return std::regex_match(command, launch);
  }

  inline std::string normalize_desktop_steam_lifetime(std::string_view policy) {
    if (policy.empty() || policy == "inherit") return "inherit";
    if (policy == "keep" || policy == "close") return std::string {policy};
    throw std::invalid_argument("desktop-steam-lifetime must be inherit, keep or close");
  }

  inline bool keep_desktop_steam(std::string_view policy, bool host_default,
                                 std::string_view mode, bool game_mode) {
    if (game_mode || (mode != "host_virtual_display" && mode != "desktop_display")) return false;
    return policy == "keep" || (policy == "inherit" && host_default);
  }
}
