/**
 * @file src/game_classifier.h
 * @brief Game classification for streaming optimization hints.
 */
#pragma once

#include <string>
#include <vector>

namespace game_classifier {

  enum class category_t {
    unknown,
    fast_action,  // FPS, racing, fighting, competitive
    cinematic,    // RPG, adventure, story-driven, strategy
    desktop,      // Productivity, browsing, utilities
    vr            // Virtual reality
  };

  /**
   * @brief Classify a game based on its Steam genre tags.
   * @param name The game name (used for VR heuristic fallback).
   * @param genres Steam genre strings (e.g. "Action", "RPG", "VR").
   * @return The detected category.
   */
  category_t classify(const std::string &name, const std::vector<std::string> &genres);

  /**
   * @brief Convert category enum to string.
   */
  std::string category_to_string(category_t cat);

  /**
   * @brief Parse category from string (e.g. from apps.json).
   */
  category_t string_to_category(const std::string &s);

  /**
   * @brief Get a human-readable hint for the AI optimizer prompt.
   * @return Empty string for unknown, otherwise a descriptive hint.
   */
  std::string category_hint(category_t cat);

}  // namespace game_classifier
