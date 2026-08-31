/**
 * @file src/config_file_update.cpp
 * @brief Lossless text updates for the Polaris configuration file.
 */

#include "config_file_update.h"

#include <algorithm>
#include <cstddef>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

namespace config_file_update {
  namespace {
    bool horizontal_space(char ch) {
      return ch == ' ' || ch == '\t';
    }

    std::string_view newline_for(std::string_view content) {
      const auto newline = content.find_first_of("\r\n");
      if (newline == std::string_view::npos) {
        return "\n";
      }
      if (content[newline] == '\r' && newline + 1 < content.size() && content[newline + 1] == '\n') {
        return "\r\n";
      }
      return content.substr(newline, 1);
    }
  }  // namespace

  result_t apply(
    std::string_view existing,
    const std::unordered_map<std::string, std::string> &updates
  ) {
    result_t result {std::string(existing), false};
    if (updates.empty()) {
      return result;
    }

    std::string output;
    output.reserve(existing.size());
    std::unordered_set<std::string> handled;
    std::size_t cursor = 0;
    int list_depth = 0;

    while (cursor < existing.size()) {
      const auto line_break = existing.find_first_of("\r\n", cursor);
      const auto line_end = line_break == std::string_view::npos ? existing.size() : line_break;
      std::size_t next_line = line_end;
      if (line_break != std::string_view::npos) {
        ++next_line;
        if (existing[line_break] == '\r' && next_line < existing.size() && existing[next_line] == '\n') {
          ++next_line;
        }
      }

      const auto comment = existing.find('#', cursor);
      const auto code_end = comment != std::string_view::npos && comment < line_end ? comment : line_end;
      bool consumed = false;

      if (list_depth == 0) {
        auto key_begin = cursor;
        while (key_begin < code_end && horizontal_space(existing[key_begin])) {
          ++key_begin;
        }
        const auto equals = existing.find('=', key_begin);
        if (equals != std::string_view::npos && equals < code_end && equals != key_begin) {
          auto key_end = equals;
          while (key_end > key_begin && horizontal_space(existing[key_end - 1])) {
            --key_end;
          }
          const std::string key(existing.substr(key_begin, key_end - key_begin));
          const auto update = updates.find(key);
          if (update != updates.end()) {
            if (update->second.empty()) {
              result.changed = true;
              handled.insert(key);
              consumed = true;
            } else if (!handled.contains(key)) {
              auto value_begin = equals + 1;
              while (value_begin < code_end && horizontal_space(existing[value_begin])) {
                ++value_begin;
              }
              auto value_end = code_end;
              while (value_end > value_begin && horizontal_space(existing[value_end - 1])) {
                --value_end;
              }
              handled.insert(key);
              if (existing.substr(value_begin, value_end - value_begin) == update->second) {
                output.append(existing.substr(cursor, next_line - cursor));
              } else {
                output.append(existing.substr(cursor, value_begin - cursor));
                output.append(update->second);
                output.append(existing.substr(value_end, next_line - value_end));
                result.changed = true;
              }
              consumed = true;
            }
          }
        }
      }

      if (!consumed) {
        output.append(existing.substr(cursor, next_line - cursor));
      }

      for (auto position = cursor; position < code_end; ++position) {
        if (existing[position] == '[') {
          ++list_depth;
        } else if (existing[position] == ']' && list_depth > 0) {
          --list_depth;
        }
      }
      cursor = next_line;
    }

    std::vector<std::pair<std::string, std::string>> additions;
    additions.reserve(updates.size());
    for (const auto &[key, value] : updates) {
      if (!value.empty() && !handled.contains(key)) {
        additions.emplace_back(key, value);
      }
    }
    std::sort(additions.begin(), additions.end());

    if (!additions.empty()) {
      const auto newline = newline_for(existing);
      if (!output.empty() && output.back() != '\n' && output.back() != '\r') {
        output.append(newline);
      }
      for (const auto &[key, value] : additions) {
        output.append(key);
        output.append(" = ");
        output.append(value);
        output.append(newline);
      }
      result.changed = true;
    }

    result.content = result.changed ? std::move(output) : std::string(existing);
    return result;
  }

}  // namespace config_file_update
