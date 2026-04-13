/**
 * @file src/game_classifier.cpp
 * @brief Game classification for streaming optimization hints.
 */

#include "game_classifier.h"

#include <algorithm>
#include <unordered_set>

namespace game_classifier {

  // Genre strings that map to each category (case-insensitive matching)
  static const std::unordered_set<std::string> vr_genres = {
    "vr", "virtual reality", "vr only", "vr supported"
  };

  static const std::unordered_set<std::string> fast_action_genres = {
    "action", "racing", "fighting", "shooter", "fps",
    "battle royale", "competitive", "sports"
  };

  static const std::unordered_set<std::string> cinematic_genres = {
    "rpg", "adventure", "story rich", "visual novel",
    "puzzle", "strategy", "simulation", "rts",
    "turn-based", "turn-based strategy", "city builder",
    "management", "survival", "horror", "platformer",
    "metroidvania", "roguelike", "roguelite",
    "indie", "casual"
  };

  static const std::unordered_set<std::string> desktop_genres = {
    "utilities", "design & illustration", "software",
    "education", "photo editing", "video production",
    "web publishing", "game development", "audio production"
  };

  static std::string to_lower(const std::string &s) {
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), ::tolower);
    return out;
  }

  category_t classify(const std::string &name, const std::vector<std::string> &genres) {
    // Check VR first (highest priority — VR has strict requirements)
    for (const auto &g : genres) {
      if (vr_genres.count(to_lower(g))) return category_t::vr;
    }
    // VR heuristic from name
    auto lower_name = to_lower(name);
    if (lower_name.find(" vr") != std::string::npos ||
        lower_name.find("vr ") != std::string::npos) {
      return category_t::vr;
    }

    // Check desktop (software, not games)
    for (const auto &g : genres) {
      if (desktop_genres.count(to_lower(g))) return category_t::desktop;
    }

    // Check fast_action vs cinematic — fast_action wins if both match
    bool has_action = false;
    bool has_cinematic = false;
    for (const auto &g : genres) {
      auto gl = to_lower(g);
      if (fast_action_genres.count(gl)) has_action = true;
      if (cinematic_genres.count(gl)) has_cinematic = true;
    }

    if (has_action) return category_t::fast_action;
    if (has_cinematic) return category_t::cinematic;

    return category_t::unknown;
  }

  std::string category_to_string(category_t cat) {
    switch (cat) {
      case category_t::fast_action: return "fast_action";
      case category_t::cinematic:   return "cinematic";
      case category_t::desktop:     return "desktop";
      case category_t::vr:          return "vr";
      default:                      return "unknown";
    }
  }

  category_t string_to_category(const std::string &s) {
    if (s == "fast_action") return category_t::fast_action;
    if (s == "cinematic")   return category_t::cinematic;
    if (s == "desktop")     return category_t::desktop;
    if (s == "vr")          return category_t::vr;
    return category_t::unknown;
  }

  std::string category_hint(category_t cat) {
    switch (cat) {
      case category_t::fast_action:
        return "fast_action (FPS/racing/competitive — prioritize ultra-low latency, accept lower resolution)";
      case category_t::cinematic:
        return "cinematic (RPG/adventure/story — prioritize visual quality, HDR, higher bitrate)";
      case category_t::desktop:
        return "desktop (productivity/browsing — lower FPS acceptable, prioritize full resolution)";
      case category_t::vr:
        return "vr (virtual reality — maximum FPS required, device-native resolution)";
      default:
        return "";
    }
  }

}  // namespace game_classifier
