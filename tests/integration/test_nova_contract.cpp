/**
 * @file tests/integration/test_nova_contract.cpp
 * @brief Keep Polaris' served fields and the Nova contract manifest in step.
 *
 * Nova addresses Polaris' JSON by string literal and defaults anything missing,
 * so a renamed or dropped field does not fail on either side — the reader just
 * silently gets its fallback. Nova ships from a separate repository, so no build
 * here can check it directly.
 *
 * What this can do is refuse to let Polaris change the shape of a response
 * without saying so in docs/nova-contract.json, which is the record both sides
 * review against. scripts/check-nova-contract.py does the other half against a
 * Nova checkout.
 */
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

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

  /// Every key assigned as `game["<key>"]` in the served source.
  std::set<std::string> emitted_game_fields() {
    const auto source = read_source_file("src/nvhttp.cpp");
    std::set<std::string> fields;

    constexpr std::string_view prefix = "game[\"";
    std::size_t pos = 0;
    while ((pos = source.find(prefix, pos)) != std::string::npos) {
      const auto start = pos + prefix.size();
      const auto end = source.find('"', start);
      if (end == std::string::npos) {
        break;
      }
      // Only assignments define the response shape; a read would not.
      const auto after = source.find_first_not_of(" \t", source.find(']', end) + 1);
      if (after != std::string::npos && source[after] == '=' && source[after + 1] != '=') {
        fields.insert(source.substr(start, end - start));
      }
      pos = end;
    }

    return fields;
  }

  std::set<std::string> manifest_game_fields() {
    const auto manifest = nlohmann::json::parse(read_source_file("docs/nova-contract.json"));
    std::set<std::string> fields;
    for (const auto &field : manifest["objects"]["game"]["fields"]) {
      fields.insert(field.get<std::string>());
    }
    return fields;
  }
}  // namespace

TEST(NovaContractTests, TheManifestListsExactlyTheGameFieldsPolarisServes) {
  const auto emitted = emitted_game_fields();
  const auto declared = manifest_game_fields();

  ASSERT_FALSE(emitted.empty()) << "no game fields found in src/nvhttp.cpp; the scan is broken, "
                                   "not the contract";

  std::vector<std::string> undeclared;
  std::set_difference(
    emitted.begin(), emitted.end(),
    declared.begin(), declared.end(),
    std::back_inserter(undeclared)
  );
  std::vector<std::string> unserved;
  std::set_difference(
    declared.begin(), declared.end(),
    emitted.begin(), emitted.end(),
    std::back_inserter(unserved)
  );

  for (const auto &field : undeclared) {
    ADD_FAILURE() << "game field [" << field << "] is served but missing from docs/nova-contract.json. "
                  << "Add it there in this commit so the Nova side has something to review against.";
  }
  for (const auto &field : unserved) {
    ADD_FAILURE() << "docs/nova-contract.json declares game field [" << field << "] that Polaris no "
                  << "longer serves. Removing a field Nova may still read is exactly the change "
                  << "that degrades silently — remove it here only once Nova has stopped reading it.";
  }
}

TEST(NovaContractTests, NewLibraryFeaturesAreDeclaredInCapabilities) {
  // Nova cannot tell an absent feature from an absent value, so anything it is
  // expected to render conditionally has to be announced rather than inferred.
  const auto source = read_source_file("src/nvhttp.cpp");

  for (const auto flag : {"library_playtime_v1", "library_beat_times_v1"}) {
    SCOPED_TRACE(flag);
    EXPECT_NE(source.find(std::string {"features[\""} + flag + "\"]"), std::string::npos);
  }
}

TEST(NovaContractTests, KnownDriftStaysRecordedUntilItIsFixed) {
  const auto manifest = nlohmann::json::parse(read_source_file("docs/nova-contract.json"));
  const auto &drift = manifest["known_drift"];

  ASSERT_TRUE(drift.contains("nova_reads_polaris_never_sends"));
  ASSERT_TRUE(drift.contains("polaris_sends_nova_never_reads"));

  // The fields Polaris serves that Nova ignores must at least be fields Polaris
  // really serves, or the record is describing something that no longer exists.
  const auto emitted = emitted_game_fields();
  for (const auto &field : drift["polaris_sends_nova_never_reads"]["game"]) {
    const auto name = field.get<std::string>();
    SCOPED_TRACE(name);
    EXPECT_TRUE(emitted.count(name) > 0)
      << "known_drift claims Polaris serves [" << name << "] but it is no longer emitted; "
      << "update docs/nova-contract.json";
  }
}
