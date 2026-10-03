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
   * @brief How deep in braces `at` sits, counting from `from`.
   *
   * Comments, string literals, raw strings and character literals are skipped, so a brace inside one
   * does not count. From the start of a handler lambda, a statement directly in its body is at 1.
   */
  int brace_depth(const std::string &source, std::size_t from, std::size_t at) {
    int depth = 0;
    const auto limit = std::min(at, source.size());
    for (std::size_t i = from; i < limit; ++i) {
      const char c = source[i];
      const char next = i + 1 < source.size() ? source[i + 1] : '\0';
      if (c == '/' && next == '/') {
        i = source.find('\n', i);
        if (i == std::string::npos) {
          break;
        }
      } else if (c == '/' && next == '*') {
        i = source.find("*/", i + 2);
        if (i == std::string::npos) {
          break;
        }
        ++i;
      } else if (c == 'R' && next == '"' && (i == 0 || !is_identifier_char(source[i - 1]))) {
        const auto open = source.find('(', i + 2);
        const auto delimiter = ")" + source.substr(i + 2, open - (i + 2)) + "\"";
        const auto close = source.find(delimiter, open);
        if (close == std::string::npos) {
          break;
        }
        i = close + delimiter.size() - 1;
      } else if (c == '\'' && i > 0 && std::isxdigit(static_cast<unsigned char>(source[i - 1])) &&
                 std::isxdigit(static_cast<unsigned char>(next))) {
        // A digit separator, as in 1'000.
      } else if (c == '"' || c == '\'') {
        for (++i; i < source.size() && source[i] != c; ++i) {
          if (source[i] == '\\') {
            ++i;
          }
        }
      } else if (c == '{') {
        ++depth;
      } else if (c == '}') {
        --depth;
      }
    }
    return depth;
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

  std::set<std::string> emitted_fields_from_one(const nlohmann::json &emitter) {
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

  /// An object may be assembled in more than one place. `game` is: nvhttp.cpp writes most of it by
  /// subscript and then merges in the launcher metadata process.cpp builds. A list of emitters keeps
  /// every field checked against the source that serves it rather than hand-listing the merged ones,
  /// which is what the whole test exists to prevent.
  std::set<std::string> emitted_fields(const nlohmann::json &emitter) {
    if (!emitter.is_array()) {
      return emitted_fields_from_one(emitter);
    }

    std::set<std::string> all;
    for (const auto &one : emitter) {
      const auto some = emitted_fields_from_one(one);
      all.insert(some.begin(), some.end());
    }
    return all;
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
    for (const auto &one : object["polaris_emitter"].is_array() ?
                             object["polaris_emitter"] : nlohmann::json::array({object["polaris_emitter"]})) {
      // A listed emitter that derives nothing is a silent hole: the union still looks healthy while
      // one of its halves has stopped matching the source it names.
      ASSERT_FALSE(emitted_fields_from_one(one).empty())
        << "emitter " << one.dump() << " for [" << name << "] derives no fields, so it no longer "
        << "describes the source that serves them";
    }

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

  for (const auto flag : {"library_playtime_v1", "library_beat_times_v1", "display_planner_v1", "support_client_report_v1", "library_emulators_v1", "spaces_v1"}) {
    SCOPED_TRACE(flag);
    EXPECT_NE(source.find(std::string {"features[\""} + flag + "\"]"), std::string::npos);
  }
}

TEST(NovaContractTests, BitrateUnitsAreAnnouncedAndServedForEveryStream) {
  // A client reads bitrate_units only once capabilities announce it, and reads it on every codec, so
  // the flag and the statement that serves the object sit directly in their handler's body, under no
  // condition of their own: no codec branch, and no other branch either.
  const auto source = read_source_file("src/nvhttp.cpp");

  const auto capabilities = source.find("auto polarisCapabilities = [");
  ASSERT_NE(capabilities, std::string::npos);
  const auto capabilities_end = source.find("auto polarisPyroWaveAdvice = [", capabilities);
  const auto features = source.find("auto &features = output[\"features\"];", capabilities);
  const auto flag = source.find("features[\"bitrate_units_v1\"] = true;", capabilities);
  ASSERT_NE(capabilities_end, std::string::npos);
  ASSERT_NE(features, std::string::npos);
  ASSERT_NE(flag, std::string::npos);
  EXPECT_LT(flag, capabilities_end);
  EXPECT_EQ(brace_depth(source, capabilities, features), 1);
  EXPECT_EQ(brace_depth(source, capabilities, flag), 1);

  const auto status = source.find("auto polarisSessionStatus = [");
  ASSERT_NE(status, std::string::npos);
  const auto status_end = source.find("auto polarisStreamPolicy = [", status);
  const auto first_statement = source.find("print_req<PolarisHTTPS>(request);", status);
  const auto served = source.find("output[\"bitrate_units\"] = std::move(bitrate_units);", status);
  ASSERT_NE(status_end, std::string::npos);
  ASSERT_NE(first_statement, std::string::npos);
  ASSERT_NE(served, std::string::npos);
  EXPECT_LT(served, status_end);
  const auto built = source.rfind(
    "if (auto bitrate_units = stream_stats::bitrate_units_json(stats, requester_generation);", served
  );
  ASSERT_NE(built, std::string::npos);
  EXPECT_LT(status, built);
  EXPECT_EQ(brace_depth(source, status, first_statement), 1);
  EXPECT_EQ(brace_depth(source, status, built), 1);
  EXPECT_EQ(brace_depth(source, status, served), 2);
  const auto guard = source.substr(built, served - built);
  EXPECT_EQ(guard.find("codec"), std::string::npos) << guard;
  EXPECT_EQ(guard.find("pyrowave"), std::string::npos) << guard;
}

TEST(NovaContractTests, EveryManualBitrateEndpointTakesUpTo500Mbps) {
  // A client sets its own bitrate through its paired client settings, a live change, a resolved
  // launch, a Space resolve and the launch profile route. The last three are exercised directly
  // elsewhere; the first two are handlers, so this holds them to the shared range, 1000 to 500000 kbps.
  const auto nvhttp = read_source_file("src/nvhttp.cpp");
  const auto between = [&](std::string_view first, std::string_view next) {
    const auto start = nvhttp.find(first);
    const auto end = nvhttp.find(next, start);
    EXPECT_NE(start, std::string::npos) << first;
    EXPECT_NE(end, std::string::npos) << next;
    return start == std::string::npos || end == std::string::npos ? std::string {} : nvhttp.substr(start, end - start);
  };
  const auto settings = between("auto polarisClientSettings = [", "auto polarisGames = [");
  EXPECT_NE(settings.find("target_bitrate_kbps != 0 && !stream_bitrate::request_in_range(target_bitrate_kbps)"),
            std::string::npos);
  EXPECT_NE(settings.find("\"target_bitrate_kbps must be 0 or \" + stream_bitrate::request_range_text()"),
            std::string::npos);
  const auto live = between("auto polarisSetBitrate = [", "auto polarisSetAdaptiveBitrate = [");
  EXPECT_NE(live.find("if (!stream_bitrate::request_in_range(bitrate_kbps)) {"), std::string::npos);
  EXPECT_NE(live.find("\"bitrate_kbps must be \" + stream_bitrate::request_range_text()"), std::string::npos);
  EXPECT_NE(nvhttp.find("consumed != raw_bitrate.size() || !stream_bitrate::request_in_range(parsed)"), std::string::npos);
  EXPECT_NE(nvhttp.find("bitrate > stream_bitrate::k_max_request_kbps"), std::string::npos);

  // No endpoint keeps the old limit of its own.
  for (const auto *file : {"src/nvhttp.cpp", "src/launch_profile.cpp", "src/doctor_trial.cpp"}) {
    const auto source = read_source_file(file);
    EXPECT_EQ(source.find("300000"), std::string::npos) << file;
    EXPECT_EQ(source.find("300'000"), std::string::npos) << file;
  }
}

TEST(NovaContractTests, ManualBitrateLimitIsAnnouncedInCapabilities) {
  // A 1.4.13 host refuses a manual bitrate above 300000 kbps, a resolved launch outright, so a client
  // offers up to 500 Mbps only to a host that announces it. The number comes from the same constant the
  // endpoints check against, directly in the capabilities handler's body, under no condition of its own.
  const auto source = read_source_file("src/nvhttp.cpp");
  const auto capabilities = source.find("auto polarisCapabilities = [");
  ASSERT_NE(capabilities, std::string::npos);
  const auto capabilities_end = source.find("auto polarisPyroWaveAdvice = [", capabilities);
  const auto flag = source.find("features[\"manual_bitrate_max_kbps\"] = stream_bitrate::k_max_request_kbps;", capabilities);
  ASSERT_NE(capabilities_end, std::string::npos);
  ASSERT_NE(flag, std::string::npos);
  EXPECT_LT(flag, capabilities_end);
  EXPECT_EQ(brace_depth(source, capabilities, flag), 1);

  const auto bitrate = read_source_file("src/stream_bitrate.h");
  EXPECT_NE(bitrate.find("inline constexpr int k_max_request_kbps = 500000;"), std::string::npos);

  // The manifest tells Nova what the number means and what a host without it takes.
  const auto units = manifest()["objects"]["bitrate_units"]["$comment"];
  std::string comment;
  for (const auto &line : units) {
    comment += line.get<std::string>() + " ";
  }
  EXPECT_NE(comment.find("features.manual_bitrate_max_kbps"), std::string::npos) << comment;
  EXPECT_NE(comment.find("A host without it takes 300000"), std::string::npos) << comment;
}

TEST(NovaContractTests, PyroWaveAdviceIsGrossedUpForTheStreamsOwnLink) {
  // The advice route and the PyroWave Live Tuning floor take the FEC share and audio from the stream's
  // recorded handshake through stream_link, as pyrowave_bitrate does, so a config reload mid stream
  // leaves none of them disagreeing with bitrate_units.
  const auto nvhttp = read_source_file("src/nvhttp.cpp");
  const auto route = nvhttp.find("auto polarisPyroWaveAdvice = [");
  ASSERT_NE(route, std::string::npos);
  const auto reply = nvhttp.find("pyrowave_advice::advice_reply(", route);
  ASSERT_NE(reply, std::string::npos);
  const auto handler = nvhttp.substr(route, reply - route);
  EXPECT_NE(handler.find("pyrowave_advice::stream_link("), std::string::npos) << handler;
  EXPECT_NE(handler.find("host.fec_percentage = link.fec_percentage;"), std::string::npos) << handler;
  EXPECT_NE(handler.find("host.audio_kbps = link.audio_kbps;"), std::string::npos) << handler;
  EXPECT_EQ(handler.find("host.fec_percentage = config::stream.fec_percentage"), std::string::npos) << handler;

  const auto stream = read_source_file("src/stream.cpp");
  const auto floor = stream.find("adaptive_bitrate::set_session_floor(advice.floor_encoder_kbps, \"pyrowave_advice\");");
  ASSERT_NE(floor, std::string::npos);
  const auto advise = stream.rfind("pyrowave_advice::advise(", floor);
  ASSERT_NE(advise, std::string::npos);
  const auto call = stream.substr(advise, floor - advise);
  EXPECT_NE(call.find("pyrowave_advice::stream_link(&request, config::stream.fec_percentage)"), std::string::npos) << call;
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
  // items() holds a proxy into the json it was called on. manifest() returns by
  // value, so iterating its result directly walks a container that is destroyed at
  // the end of the full expression. Bind it first, as the tests above already do.
  const auto contract = manifest();
  for (const auto &[name, object] : contract["objects"].items()) {
    SCOPED_TRACE(name);
    if (!object.contains("nova_reader")) {
      continue;
    }
    EXPECT_TRUE(object["nova_reader"].contains("function"))
      << "nova_reader must name a function; Nova gives several functions in the same file "
         "a parameter called `json`, so file or receiver scoping silently mixes objects";
  }
}
