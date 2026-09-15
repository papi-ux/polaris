/**
 * @file src/kernel_gpu_lines.h
 * @brief The kernel's own GPU lines, filtered out of a journal dump for the support bundle.
 *
 * A whole-machine freeze writes nothing to Polaris' log, by definition. What it
 * sometimes writes is a kernel line seconds before: an NVIDIA Xid, an i915 GPU
 * hang, a hung task. The bundle carries the GPU-related lines of this boot and
 * the previous one so a freeze report has them without a second round trip.
 */
#pragma once

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <string_view>
#include <vector>

namespace kernel_gpu_lines {
  inline constexpr std::size_t k_default_max_lines = 150;
  inline constexpr std::size_t k_max_line_length = 400;

  inline std::string lowercase(std::string_view text) {
    std::string out(text);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
      return static_cast<char>(std::tolower(c));
    });
    return out;
  }

  /// The line mentions a GPU driver, a virtual display driver, or a hang.
  inline bool is_gpu_related(std::string_view line) {
    const auto lower = lowercase(line);
    for (const auto term : {"nvidia", "nvrm", "amdgpu", "radeon", "i915", " xe ", "xe 0000:", "[drm]", " drm ", "xid",
                            "gpu hang", "hung task", "oops", "bug:", "kernel panic", "watchdog", "lockup",
                            "evdi", "vkms", "hermes-kms", "vibeshine"}) {
      if (lower.find(term) != std::string::npos) {
        return true;
      }
    }
    return false;
  }

  /// The last max_lines GPU-related lines of a journal dump, each trimmed to k_max_line_length.
  inline std::vector<std::string> filter(std::string_view text, std::size_t max_lines = k_default_max_lines) {
    std::vector<std::string> kept;
    std::size_t start = 0;
    while (start < text.size()) {
      auto end = text.find('\n', start);
      if (end == std::string_view::npos) {
        end = text.size();
      }
      const auto line = text.substr(start, end - start);
      if (!line.empty() && is_gpu_related(line)) {
        kept.emplace_back(line.substr(0, k_max_line_length));
      }
      start = end + 1;
    }
    if (kept.size() > max_lines) {
      kept.erase(kept.begin(), kept.begin() + static_cast<std::ptrdiff_t>(kept.size() - max_lines));
    }
    return kept;
  }

  /// journalctl said it could not show the kernel journal to this account.
  inline bool journal_unreadable(std::string_view text) {
    const auto lower = lowercase(text);
    for (const auto marker : {"no journal files were found", "not seeing messages", "permission denied", "failed to open journal", "failed to access"}) {
      if (lower.find(marker) != std::string::npos) {
        return true;
      }
    }
    return false;
  }
}  // namespace kernel_gpu_lines
