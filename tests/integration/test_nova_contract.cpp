/**
 * @file tests/integration/test_nova_contract.cpp
 * @brief Keep Polaris' served fields and the Nova contract manifest in step.
 *
 * Nova addresses Polaris' JSON by string literal and defaults anything missing,
 * so a renamed or dropped field does not fail on either side — the reader just
 * silently gets its fallback. Nova ships from a separate repository, so no build
 * here can check it directly.
 *
 * What this can do is derive what Polaris actually serves, straight from the
 * source that serves it, and refuse to let docs/nova-contract.json disagree.
 * That derivation is the point: every field list in that manifest was originally
 * written by hand from ad-hoc greps, and every one of them was wrong somewhere —
 * a `"x"` from a "1920x1080" concatenation counted as a field, a function range
 * that ran past its own body and reported the next object's fields as its own.
 * A manifest is only worth as much as the extraction behind it.
 *
 * scripts/check-nova-contract.py does the other half against a Nova checkout.
 */
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

namespace {
  std::string read_source_file(std::string_view relative_path) {
    const auto path = std::filesystem::path {POLARIS_SOURCE_DIR} / relative_path;
    std::ifstream input {path};
    std::ostringstream buffer;
    buffer << input.rdbuf();
    return buffer.str();
  }

  bool is_identifier_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
  }

  /**
   * @brief The body of a C++ function, by brace matching from its signature.
   *
   * Brace matching rather than "up to the next function", because a range that
   * overruns its own body is what made an earlier revision of the manifest
   * report `steam_launch`'s fields as `launch_mode`'s.
   */
  std::string function_body(const std::string &source, const std::string &name) {
    // The definition, not the first mention. A call site appears earlier in the
    // file for at least one of these, and matching it silently scopes to some
    // unrelated block — which is how this extractor first derived nothing at all.
    std::size_t open = std::string::npos;
    for (std::size_t sig = source.find(name + "("); sig != std::string::npos;
         sig = source.find(name + "(", sig + 1)) {
      if (sig > 0 && is_identifier_char(source[sig - 1])) {
        continue;
      }

      int parens = 0;
      std::size_t close = std::string::npos;
      for (std::size_t i = source.find('(', sig); i < source.size(); ++i) {
        if (source[i] == '(') {
          ++parens;
        } else if (source[i] == ')' && --parens == 0) {
          close = i;
          break;
        }
      }
      if (close == std::string::npos) {
        continue;
      }

      const auto next = source.find_first_not_of(" \t\n", close + 1);
      if (next != std::string::npos && source[next] == '{') {
        open = next;
        break;
      }
    }

    if (open == std::string::npos) {
      return {};
    }

    int depth = 0;
    bool in_string = false;
    for (std::size_t i = open; i < source.size(); ++i) {
      const char c = source[i];
      if (in_string) {
        if (c == '\\') {
          ++i;
        } else if (c == '"') {
          in_string = false;
        }
        continue;
      }
      if (c == '"') {
        in_string = true;
      } else if (c == '{') {
        ++depth;
      } else if (c == '}' && --depth == 0) {
        return source.substr(open, i - open + 1);
      }
    }

    return {};
  }

  /// Keys from `variable["key"] = ...` assignments.
  std::set<std::string> subscript_keys(const std::string &scope, const std::string &variable) {
    std::set<std::string> keys;
    const std::string needle = variable + "[\"";

    std::size_t pos = 0;
    while ((pos = scope.find(needle, pos)) != std::string::npos) {
      // Reject a longer identifier ending in the variable name, so `encoder`
      // does not also match `some_encoder`.
      if (pos > 0 && is_identifier_char(scope[pos - 1])) {
        pos += needle.size();
        continue;
      }

      const auto start = pos + needle.size();
      const auto end = scope.find('"', start);
      if (end == std::string::npos) {
        break;
      }

      // Only assignments define the shape. A read would not, and neither would
      // a subscript used inside a larger expression.
      const auto bracket = scope.find(']', end);
      const auto after = scope.find_first_not_of(" \t\n", bracket + 1);
      if (after != std::string::npos && scope[after] == '=' && scope[after + 1] != '=') {
        keys.insert(scope.substr(start, end - start));
      }
      pos = end;
    }

    return keys;
  }

  /// Keys from a brace-initialised object: `{"key", value},`.
  std::set<std::string> initializer_keys(const std::string &scope) {
    std::set<std::string> keys;
    std::size_t pos = 0;
    while ((pos = scope.find("{\"", pos)) != std::string::npos) {
      const auto start = pos + 2;
      const auto end = scope.find('"', start);
      if (end == std::string::npos) {
        break;
      }
      const auto after = scope.find_first_not_of(" \t\n", end + 1);
      if (after != std::string::npos && scope[after] == ',') {
        keys.insert(scope.substr(start, end - start));
      }
      pos = end;
    }
    return keys;
  }

  std::set<std::string> emitted_fields(const nlohmann::json &emitter) {
    const auto source = read_source_file(emitter.at("file").get<std::string>());
    const auto scope = emitter.contains("function") ?
                         function_body(source, emitter.at("function").get<std::string>()) :
                         source;

    if (emitter.at("style") == "initializer_list") {
      // The declaration only, so sibling objects built in the same function —
      // configuration_warnings entries, for instance — stay out of it.
      const auto decl = scope.find(emitter.at("declaration").get<std::string>());
      return decl == std::string::npos ? std::set<std::string> {} : initializer_keys(scope.substr(decl));
    }

    return subscript_keys(scope, emitter.at("variable").get<std::string>());
  }

  nlohmann::json manifest() {
    return nlohmann::json::parse(read_source_file("docs/nova-contract.json"));
  }
}  // namespace

TEST(NovaContractTests, EveryManifestObjectMatchesWhatPolarisActuallyServes) {
  const auto contract = manifest();

  for (const auto &[name, object] : contract["objects"].items()) {
    SCOPED_TRACE(name);
    ASSERT_TRUE(object.contains("polaris_emitter"))
      << "object [" << name << "] has no polaris_emitter, so its field list is hand-written "
      << "and nothing checks it against the source that serves it";

    const auto emitted = emitted_fields(object["polaris_emitter"]);
    ASSERT_FALSE(emitted.empty())
      << "derived no fields for [" << name << "]; the extractor is broken, not the contract";

    std::set<std::string> declared;
    for (const auto &field : object["fields"]) {
      declared.insert(field.get<std::string>());
    }

    std::vector<std::string> undeclared;
    std::set_difference(emitted.begin(), emitted.end(), declared.begin(), declared.end(),
                        std::back_inserter(undeclared));
    std::vector<std::string> unserved;
    std::set_difference(declared.begin(), declared.end(), emitted.begin(), emitted.end(),
                        std::back_inserter(unserved));

    for (const auto &field : undeclared) {
      ADD_FAILURE() << "[" << name << "] serves [" << field << "] but the manifest omits it. "
                    << "Add it in this commit so the Nova side has something to review against.";
    }
    for (const auto &field : unserved) {
      ADD_FAILURE() << "[" << name << "] declares [" << field << "] that Polaris does not serve. "
                    << "Removing a field Nova may still read degrades silently, so drop it here "
                    << "only once Nova has stopped reading it.";
    }
  }
}

TEST(NovaContractTests, NewLibraryFeaturesAreDeclaredInCapabilities) {
  // Nova cannot tell an absent feature from an absent value, so anything it is
  // expected to render conditionally has to be announced rather than inferred.
  const auto source = read_source_file("src/nvhttp.cpp");

  for (const auto flag : {"library_playtime_v1", "library_beat_times_v1", "display_planner_v1"}) {
    SCOPED_TRACE(flag);
    EXPECT_NE(source.find(std::string {"features[\""} + flag + "\"]"), std::string::npos);
  }
}

TEST(NovaContractTests, KnownDriftNamesFieldsThatStillExist) {
  const auto contract = manifest();
  const auto &drift = contract["known_drift"];

  ASSERT_TRUE(drift.contains("nova_reads_polaris_never_sends"));
  ASSERT_TRUE(drift.contains("polaris_sends_nova_never_reads"));

  // A field recorded as served-but-unread must still be served, or the record
  // describes something that no longer exists.
  for (const auto &[object_name, fields] : drift["polaris_sends_nova_never_reads"].items()) {
    if (object_name.starts_with("$") || !contract["objects"].contains(object_name)) {
      continue;
    }
    const auto emitted = emitted_fields(contract["objects"][object_name]["polaris_emitter"]);
    for (const auto &field : fields) {
      const auto name = field.get<std::string>();
      SCOPED_TRACE(object_name + "." + name);
      EXPECT_TRUE(emitted.count(name) > 0)
        << "known_drift claims Polaris serves it, but it is not emitted";
    }
  }

  // And a field recorded as read-but-unserved must genuinely be unserved, or the
  // drift has been fixed and the record is now the stale part.
  for (const auto &[object_name, fields] : drift["nova_reads_polaris_never_sends"].items()) {
    if (object_name.starts_with("$") || !contract["objects"].contains(object_name)) {
      continue;
    }
    const auto emitted = emitted_fields(contract["objects"][object_name]["polaris_emitter"]);
    for (const auto &field : fields) {
      const auto name = field.get<std::string>();
      SCOPED_TRACE(object_name + "." + name);
      EXPECT_EQ(0U, emitted.count(name))
        << "known_drift says Polaris never serves this, but it does now; remove the entry";
    }
  }
}

TEST(NovaContractTests, EveryObjectNamesTheFunctionScopingItsNovaReads) {
  // Scope is what went wrong repeatedly on the Nova side too, so it is pinned
  // rather than left to whoever edits the manifest next.
  //
  // An object may have no nova_reader at all: it shipped ahead of any Nova
  // consumer (display_planner did this before papi-ux/nova#197; session_timing
  // does it now). That is a real, documented state, not something this test
  // should fail on - it only pins scope for objects that claim a reader.
  for (const auto &[name, object] : manifest()["objects"].items()) {
    SCOPED_TRACE(name);
    if (!object.contains("nova_reader")) {
      continue;
    }
    EXPECT_TRUE(object["nova_reader"].contains("function"))
      << "nova_reader must name a function; Nova gives several functions in the same file "
         "a parameter called `json`, so file or receiver scoping silently mixes objects";
  }
}
