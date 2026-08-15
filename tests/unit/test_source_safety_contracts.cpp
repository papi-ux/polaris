/**
 * @file tests/unit/test_source_safety_contracts.cpp
 * @brief Tree-wide invariants behind two bug classes this codebase has hit.
 *
 * Both were consistency failures rather than gaps in knowledge: the correct
 * helper already existed a few hundred lines away and one call site simply did
 * not reach for it. A review pass finds those once; a contract finds them every
 * time, including in code written later.
 *
 * These scan the whole of src/ rather than named files on purpose. A contract
 * that lists the files it knows about only ever covers the bugs already found.
 */
#include <filesystem>
#include <fstream>
#include <regex>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>

namespace {

  namespace fs = std::filesystem;

  struct source_file_t {
    std::string relative_path;
    std::string text;
  };

  std::vector<source_file_t> all_sources() {
    std::vector<source_file_t> sources;
    const fs::path root = fs::path {POLARIS_SOURCE_DIR} / "src";
    for (const auto &entry : fs::recursive_directory_iterator(root)) {
      if (!entry.is_regular_file()) {
        continue;
      }
      const auto extension = entry.path().extension().string();
      if (extension != ".cpp" && extension != ".h") {
        continue;
      }
      std::ifstream input(entry.path());
      std::ostringstream contents;
      contents << input.rdbuf();
      sources.push_back({fs::relative(entry.path(), fs::path {POLARIS_SOURCE_DIR}).generic_string(), contents.str()});
    }
    return sources;
  }

  std::size_t line_of(const std::string &text, std::size_t offset) {
    return static_cast<std::size_t>(std::count(text.begin(), text.begin() + offset, '\n')) + 1;
  }

  /**
   * @brief Whether @p offset sits inside a double-quoted literal on its line.
   *
   * Log messages quote the names of the very things these contracts look for --
   * kmsgrab prints a `$(readlink -f ...)` setcap hint -- and a contract that
   * flags prose gets suppressed rather than obeyed.
   */
  bool inside_string_literal(const std::string &text, std::size_t offset) {
    const auto line_start = text.rfind('\n', offset);
    std::size_t cursor = line_start == std::string::npos ? 0 : line_start + 1;
    bool quoted = false;
    for (; cursor < offset; ++cursor) {
      if (text[cursor] == '\\') {
        ++cursor;
        continue;
      }
      if (text[cursor] == '"') {
        quoted = !quoted;
      }
    }
    return quoted;
  }

  /**
   * @brief Whether @p offset sits inside a line or block comment.
   *
   * Contracts scan source text rather than the compiler AST, so comments that
   * describe a guarded call must not be treated as executable code.
   */
  bool inside_comment(const std::string &text, std::size_t offset) {
    const auto line_start = text.rfind('\n', offset);
    const auto line_comment = text.find("//", line_start == std::string::npos ? 0 : line_start + 1);
    if (line_comment != std::string::npos && line_comment < offset) {
      return true;
    }
    const auto block_open = text.rfind("/*", offset);
    const auto block_close = text.rfind("*/", offset);
    return block_open != std::string::npos &&
           (block_close == std::string::npos || block_open > block_close);
  }

  /// Text between the parenthesis at @p open and its match, exclusive.
  std::string argument_expression(const std::string &text, std::size_t open) {
    std::size_t depth = 1;
    std::size_t cursor = open + 1;
    for (; cursor < text.size() && depth > 0; ++cursor) {
      if (text[cursor] == '(') {
        ++depth;
      } else if (text[cursor] == ')') {
        --depth;
      }
    }
    return text.substr(open + 1, cursor - open - 2);
  }

}  // namespace

TEST(SourceSafetyContracts, ShellCommandsBuiltByConcatenationAreEscaped) {
  // A config value reaching std::system unescaped turned "can edit settings"
  // into "can run commands". The fix was to escape; this keeps the next
  // concatenated shell call from being written without one.
  static const std::regex shell_call {R"((std::system|(^|[^\w_])popen)\s*\()"};

  std::vector<std::string> unescaped;
  for (const auto &source : all_sources()) {
    for (auto it = std::sregex_iterator(source.text.begin(), source.text.end(), shell_call);
         it != std::sregex_iterator();
         ++it) {
      const auto open = source.text.find('(', static_cast<std::size_t>(it->position()));
      const auto expression = argument_expression(source.text, open);

      // Only concatenation can smuggle a value in; a bare literal cannot.
      if (expression.find('+') == std::string::npos) {
        continue;
      }
      if (expression.find("shell_escape") != std::string::npos ||
          expression.find("shell_quote") != std::string::npos ||
          expression.find("url_escape") != std::string::npos) {
        continue;
      }

      // Some call sites escape where the string is built rather than where it
      // is run, which no source-level check can see. Those carry a marker
      // naming where the escaping happened, so adding one costs a written
      // justification at the site instead of a silent entry in a list here.
      const auto preamble_start = open < 400 ? 0 : open - 400;
      const auto preamble = source.text.substr(preamble_start, open - preamble_start);
      if (preamble.find("shell-escape-checked") != std::string::npos) {
        continue;
      }

      unescaped.push_back(source.relative_path + ":" + std::to_string(line_of(source.text, open)));
    }
  }

  // Every concatenated shell call escapes. A new one lands here rather than in
  // an audit, and the fix is to escape it -- not to extend this list.
  EXPECT_TRUE(unescaped.empty()) << [&] {
    std::ostringstream out;
    out << "concatenated shell call(s) without an escape helper:";
    for (const auto &site : unescaped) {
      out << "\n  " << site;
    }
    return out.str();
  }();
}

TEST(SourceSafetyContracts, ReadlinkResultsAreCheckedBeforeUse) {
  // readlink reports failure as -1. Widening that into a size handed a
  // string_view a length of SIZE_MAX; the other seven call sites all checked.
  std::vector<std::string> unchecked;
  for (const auto &source : all_sources()) {
    std::size_t at = 0;
    while ((at = source.text.find("readlink", at)) != std::string::npos) {
      const auto open = source.text.find('(', at);
      if (open == std::string::npos) {
        break;
      }
      const auto next = at + std::string("readlink").size();
      // Only a real call has its parenthesis immediately after the name, and
      // only prose writes "readlink()" with nothing in it.
      if (source.text.compare(next, 1, "(") != 0 || source.text.compare(next, 2, "()") == 0 ||
          inside_string_literal(source.text, at) || inside_comment(source.text, at)) {
        at = next;
        continue;
      }

      const auto window = source.text.substr(open, 320);
      static const std::regex guard {R"(<\s*0|<=\s*0|>\s*0|!=\s*-1|==\s*-1)"};
      if (!std::regex_search(window, guard)) {
        unchecked.push_back(source.relative_path + ":" + std::to_string(line_of(source.text, at)));
      }
      at = next;
    }
  }

  EXPECT_TRUE(unchecked.empty()) << [&] {
    std::ostringstream out;
    out << "readlink call(s) whose result is not checked nearby:";
    for (const auto &site : unchecked) {
      out << "\n  " << site;
    }
    return out.str();
  }();
}
