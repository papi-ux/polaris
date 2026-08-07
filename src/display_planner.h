/**
 * @file src/display_planner.h
 * @brief The display resolution planner, ported from the web UI.
 *
 * src_assets/common/assets/web/display-resolution-planner.js is the reference
 * implementation; this is the same planner computed host-side so it can be
 * served to clients that never load the web UI. Behaviour must not drift from
 * the JS: both surfaces plan from the same fallback display mode, and a client
 * following a served recommendation should land on the exact mode the web UI
 * would have shown. tests/unit/test_display_planner.cpp pins the shared
 * fixture values.
 */
#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace display_planner {
  /**
   * @brief A display mode. Dimensions are doubles because the JS plans in
   * doubles; they hold integral values everywhere a mode came from parsing.
   */
  struct mode_t {
    double width;
    double height;
    double fps;
  };

  /**
   * @brief The safe-mode envelope. Choices beyond it are served but hidden.
   */
  struct limits_t {
    double max_width = 7680;
    double max_height = 4320;
    double max_pixels = 7680.0 * 4320.0;
  };

  /**
   * @brief One planned preset: native, balanced, sharp, performance or custom.
   */
  struct choice_t {
    std::string id;
    std::string title;
    std::string intent;
    std::string badge;
    std::string reason;
    std::string aspect_ratio;
    std::string target_mode;
    mode_t target;
    double scale_factor;
    bool advanced;
    bool custom;
    bool safe;
    /**
     * @brief Hidden means unsafe, nothing else. The JS folds its own
     * show-advanced toggle into `hidden`; a served contract keeps the axes
     * separate so a client can offer its own toggle from the `advanced` flag
     * without a hidden bit fighting it.
     */
    bool hidden;
  };

  /**
   * @brief One entry of the advanced scale-factor strip.
   */
  struct scale_option_t {
    double scale_factor;
    std::string label;
    std::string target_mode;
    mode_t target;
    bool safe;
  };

  /**
   * @brief The full plan for one source mode.
   */
  struct planner_t {
    mode_t source;
    std::string source_mode;
    std::string source_aspect_ratio;
    std::string recommended_id;
    std::string recommended_title;
    std::string recommended_mode;
    std::vector<choice_t> choices;
    std::vector<scale_option_t> advanced_scale_factors;
  };

  /**
   * @brief The scale factors offered on the advanced strip.
   */
  const std::vector<double> &planner_scale_factors();

  /**
   * @brief Parse a "2560x1600x90" mode string; fps may be fractional.
   * @returns std::nullopt when the string is not exactly that shape.
   */
  std::optional<mode_t> parse_display_mode(std::string_view value);

  /**
   * @brief Format a mode as "2560x1600x90"; integral fps carries no decimals.
   */
  std::string format_display_mode(const mode_t &mode);

  /**
   * @brief Clamp a scale factor to [0.5, 2]; non-finite values become 1.
   */
  double clamp_planner_scale(double value);

  /**
   * @brief Scale a mode, rounding both dimensions up to even and never below 2.
   */
  mode_t resolve_scaled_mode(const mode_t &base, double scale_factor);

  /**
   * @brief Reduced aspect label, "8:5" for 2560x1600.
   */
  std::string aspect_ratio_label(double width, double height);

  /**
   * @brief Build the plan for a source mode.
   *
   * Degenerate source components fall back to 1920x1080x60 exactly as the JS
   * normalizeDevice does, so this always returns a fully-populated plan. The
   * recommendation is chosen among safe, non-advanced choices — what the web
   * UI shows with advanced collapsed — preferring `balanced`, then falling
   * back to the first such choice, then to the first choice of all.
   */
  planner_t build_resolution_planner(const mode_t &source, double custom_scale = 1.0, const limits_t &limits = {});
}  // namespace display_planner
