/** @file src/platform/linux/kms_connector_selection.h
 * Stable KMS selectors with legacy enumeration positions preserved.
 */
#pragma once
#include <charconv>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace platf::kms_selection {
  struct output_t {
    std::string gpu;
    std::string connector;
    bool connected {true};
    uint32_t crtc_id {1};
  };

  inline std::string qualified_name(const output_t &output) {
    if (output.gpu.empty() || output.connector.empty() || !output.connected || !output.crtc_id) return {};
    return "kms:" + output.gpu + '/' + output.connector;
  }

  inline std::optional<std::pair<std::string_view, std::string_view>> split_name(std::string_view name) {
    if (!name.starts_with("kms:")) return std::nullopt;
    name.remove_prefix(4);
    const auto slash = name.find('/');
    if (slash == std::string_view::npos || slash == 0 || slash + 1 == name.size()) return std::nullopt;
    return std::pair {name.substr(0, slash), name.substr(slash + 1)};
  }

  inline std::optional<int> legacy_index(std::string_view name) {
    if (name.empty()) return 0;
    int value = -1;
    const auto parsed = std::from_chars(name.data(), name.data() + name.size(), value);
    if (parsed.ec == std::errc {} && parsed.ptr == name.data() + name.size() && value >= 0) return value;
    return std::nullopt;
  }

  inline std::vector<std::string> display_names(const std::vector<output_t> &outputs) {
    std::vector<std::string> names;
    names.reserve(outputs.size());
    for (std::size_t i = 0; i < outputs.size(); ++i) {
      auto name = qualified_name(outputs[i]);
      int matches = 0;
      if (!name.empty()) {
        for (const auto &output : outputs) matches += qualified_name(output) == name;
      }
      // Multiple active planes for one connector do not establish one named
      // capture surface. Preserve every legacy position, but withhold aliases.
      names.push_back(matches == 1 ? std::move(name) : std::to_string(i));
    }
    return names;
  }

  inline std::optional<int> find_alias(const std::vector<std::string> &names, std::string_view requested) {
    if (requested.empty()) return std::nullopt;
    if (requested.find('/') == std::string_view::npos) {
      for (const auto &name : names) {
        // An unnamed entry may be another occurrence of this connector on a
        // GPU whose identity is unknown. Require qualification in that case.
        if (legacy_index(name)) return std::nullopt;
      }
    }
    std::optional<int> match;
    for (std::size_t i = 0; i < names.size(); ++i) {
      const auto parts = split_name(names[i]);
      if (!parts) continue;
      if (requested != names[i] && requested != std::string_view(names[i]).substr(4) && requested != parts->second) continue;
      if (match) return std::nullopt;
      match = static_cast<int>(i);
    }
    return match;
  }

  /**
   * @brief The entry KMS capture opens for a display name, or nothing when it would refuse.
   *
   * The two lookups capture makes, folded into one: the capture thread's search of the enumerated
   * names for the configured output, and the KMS display's own reading of the name it is handed. An
   * empty name is the first entry, a number is a legacy position, and anything else is an entry's own
   * name or an alias for exactly one of them.
   */
  inline std::optional<std::size_t> capture_index(const std::vector<std::string> &names, std::string_view display_name) {
    if (const auto legacy = legacy_index(display_name)) {
      if (static_cast<std::size_t>(*legacy) < names.size()) return static_cast<std::size_t>(*legacy);
      return std::nullopt;
    }
    for (std::size_t i = 0; i < names.size(); ++i) {
      if (names[i] == display_name) return i;
    }
    if (const auto alias = find_alias(names, display_name)) return static_cast<std::size_t>(*alias);
    return std::nullopt;
  }
}
