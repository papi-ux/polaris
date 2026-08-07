/**
 * @file src/display_planner.cpp
 * @brief The display resolution planner, ported from the web UI.
 */
#include "display_planner.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <format>

namespace display_planner {
  namespace {
    constexpr mode_t default_device {1920, 1080, 60};

    struct preset_t {
      std::string_view id;
      std::string_view title;
      std::string_view intent;
      double scale_factor;
      bool advanced;
      bool custom;
    };

    // Order and wording are the contract: DISPLAY_PLANNER_PRESETS in
    // display-resolution-planner.js, verbatim.
    constexpr preset_t presets[] = {
      {"native", "Native", "Match the client panel exactly.", 1.0, false, false},
      {"balanced", "Balanced", "Best for this device: preserve aspect ratio while easing encoder and network load.", 0.75, false, false},
      {"sharp", "Sharp / Supersampled", "Render above the client panel and downscale for extra clarity when the host has headroom.", 1.5, true, false},
      {"performance", "Performance", "Favor frame pacing and bandwidth over raw pixel count.", 0.5, false, false},
      {"custom", "Custom", "Advanced manual scale factor using the existing fallback display mode field.", 1.0, true, true},
    };

    constexpr std::string_view unsafe_reason = "Hidden because this would exceed the safe display-mode envelope for normal users.";

    double positive_number(double value, double fallback) {
      return std::isfinite(value) && value > 0 ? value : fallback;
    }

    mode_t normalize_device(const mode_t &device) {
      return {
        positive_number(device.width, default_device.width),
        positive_number(device.height, default_device.height),
        positive_number(device.fps, default_device.fps),
      };
    }

    double round_to_even(double value) {
      const double rounded = std::max(2.0, std::round(value));
      return std::fmod(rounded, 2.0) == 0.0 ? rounded : rounded + 1;
    }

    bool is_safe_mode(const mode_t &target, const limits_t &limits) {
      return target.width <= limits.max_width &&
             target.height <= limits.max_height &&
             target.width * target.height <= limits.max_pixels;
    }

    // "90" for 90, "0.75" for 0.75: what a JS template literal prints for the
    // numbers this planner produces. Fractions are capped at three decimals so
    // a rational refresh rate cannot leak its full binary expansion.
    std::string format_planner_number(double value) {
      const double rounded = std::round(value * 1000.0) / 1000.0;
      if (rounded == std::floor(rounded)) {
        if (std::fabs(rounded) < 9.0e15) {
          return std::format("{}", static_cast<long long>(rounded));
        }
        return std::format("{:.0f}", rounded);
      }
      return std::format("{:g}", rounded);
    }

    bool all_digits(std::string_view value) {
      return !value.empty() && std::all_of(value.begin(), value.end(), [](char c) {
        return std::isdigit(static_cast<unsigned char>(c));
      });
    }

    std::string preset_badge(std::string_view id, const mode_t &base, double scale_factor) {
      if (id == "balanced") {
        return "Best for this device";
      }
      if (id == "native") {
        return std::format("{}×{}", format_planner_number(std::round(base.width)), format_planner_number(std::round(base.height)));
      }
      if (id == "sharp") {
        return format_planner_number(scale_factor) + "x supersample";
      }
      if (id == "performance") {
        return format_planner_number(scale_factor) + "x downscale";
      }
      return "Advanced";
    }
  }  // namespace

  const std::vector<double> &planner_scale_factors() {
    static const std::vector<double> factors {0.5, 0.75, 1, 1.25, 1.5, 2};
    return factors;
  }

  std::optional<mode_t> parse_display_mode(std::string_view value) {
    constexpr std::string_view whitespace = " \t\n\r\f\v";
    const auto first = value.find_first_not_of(whitespace);
    if (first == std::string_view::npos) {
      return std::nullopt;
    }
    value = value.substr(first, value.find_last_not_of(whitespace) - first + 1);

    const auto first_x = value.find('x');
    if (first_x == std::string_view::npos) {
      return std::nullopt;
    }
    const auto second_x = value.find('x', first_x + 1);
    if (second_x == std::string_view::npos || value.find('x', second_x + 1) != std::string_view::npos) {
      return std::nullopt;
    }

    const auto width = value.substr(0, first_x);
    const auto height = value.substr(first_x + 1, second_x - first_x - 1);
    const auto fps = value.substr(second_x + 1);

    const auto dot = fps.find('.');
    const bool fps_valid = dot == std::string_view::npos ?
                             all_digits(fps) :
                             all_digits(fps.substr(0, dot)) && all_digits(fps.substr(dot + 1));
    if (!all_digits(width) || !all_digits(height) || !fps_valid) {
      return std::nullopt;
    }

    try {
      return mode_t {
        std::stod(std::string {width}),
        std::stod(std::string {height}),
        std::stod(std::string {fps}),
      };
    } catch (const std::exception &) {
      return std::nullopt;
    }
  }

  std::string format_display_mode(const mode_t &mode) {
    return std::format("{}x{}x{}",
                       format_planner_number(std::round(mode.width)),
                       format_planner_number(std::round(mode.height)),
                       format_planner_number(mode.fps));
  }

  double clamp_planner_scale(double value) {
    if (!std::isfinite(value)) {
      return 1;
    }
    return std::min(2.0, std::max(0.5, value));
  }

  mode_t resolve_scaled_mode(const mode_t &base, double scale_factor) {
    const double scale = clamp_planner_scale(scale_factor);
    return {
      round_to_even(base.width * scale),
      round_to_even(base.height * scale),
      base.fps,
    };
  }

  std::string aspect_ratio_label(double width, double height) {
    long long x = std::llabs(std::llround(width));
    long long y = std::llabs(std::llround(height));
    while (y != 0) {
      const long long next = x % y;
      x = y;
      y = next;
    }
    const long long divisor = x != 0 ? x : 1;
    return std::format("{}:{}", format_planner_number(width / divisor), format_planner_number(height / divisor));
  }

  planner_t build_resolution_planner(const mode_t &source, double custom_scale, const limits_t &limits) {
    const auto base = normalize_device(source);
    const double custom_factor = clamp_planner_scale(custom_scale);
    const auto aspect = aspect_ratio_label(base.width, base.height);

    planner_t plan;
    plan.source = base;
    plan.source_mode = format_display_mode(base);
    plan.source_aspect_ratio = aspect;

    for (const auto &preset : presets) {
      const double scale_factor = preset.custom ? custom_factor : preset.scale_factor;
      const auto target = resolve_scaled_mode(base, scale_factor);
      const bool safe = is_safe_mode(target, limits);

      choice_t choice;
      choice.id = preset.id;
      choice.title = preset.title;
      choice.intent = preset.intent;
      choice.badge = preset_badge(preset.id, base, scale_factor);
      choice.reason = safe ? std::string {preset.intent} : std::string {unsafe_reason};
      choice.aspect_ratio = aspect;
      choice.target = target;
      choice.target_mode = format_display_mode(target);
      choice.scale_factor = scale_factor;
      choice.advanced = preset.advanced;
      choice.custom = preset.custom;
      choice.safe = safe;
      choice.hidden = !safe;
      plan.choices.push_back(std::move(choice));
    }

    // The recommendation is what the web UI would recommend with advanced
    // collapsed: `balanced` when it is safe, else the first safe non-advanced
    // choice, else the first choice of all.
    const choice_t *recommended = nullptr;
    for (const auto &choice : plan.choices) {
      if (!choice.advanced && choice.safe && choice.id == "balanced") {
        recommended = &choice;
        break;
      }
    }
    if (!recommended) {
      for (const auto &choice : plan.choices) {
        if (!choice.advanced && choice.safe) {
          recommended = &choice;
          break;
        }
      }
    }
    if (!recommended) {
      recommended = &plan.choices.front();
    }
    plan.recommended_id = recommended->id;
    plan.recommended_title = "Best for this device";
    plan.recommended_mode = recommended->target_mode;

    for (const double factor : planner_scale_factors()) {
      const auto target = resolve_scaled_mode(base, factor);
      scale_option_t option;
      option.scale_factor = factor;
      option.label = format_planner_number(factor) + "x";
      option.target = target;
      option.target_mode = format_display_mode(target);
      option.safe = is_safe_mode(target, limits);
      plan.advanced_scale_factors.push_back(std::move(option));
    }

    return plan;
  }
}  // namespace display_planner
