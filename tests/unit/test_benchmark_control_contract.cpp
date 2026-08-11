/**
 * @file tests/unit/test_benchmark_control_contract.cpp
 * @brief Keep the published benchmark-control OpenAPI contract in lockstep with Polaris.
 */
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
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

  std::string function_body(const std::string &source, const std::string &name) {
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

  std::set<std::string> property_names(const nlohmann::json &schema) {
    std::set<std::string> fields;
    for (const auto &[name, unused] : schema.at("properties").items()) {
      static_cast<void>(unused);
      fields.insert(name);
    }
    return fields;
  }

  std::set<std::string> required_names(const nlohmann::json &schema) {
    std::set<std::string> fields;
    for (const auto &name : schema.at("required")) {
      fields.insert(name.get<std::string>());
    }
    return fields;
  }

  std::set<std::string> object_keys(const nlohmann::json &object) {
    std::set<std::string> fields;
    for (const auto &[name, unused] : object.items()) {
      static_cast<void>(unused);
      fields.insert(name);
    }
    return fields;
  }

  std::set<std::string> subscript_assignment_keys(const std::string &scope, const std::string &variable) {
    std::set<std::string> keys;
    const std::string needle = variable + "[\"";

    std::size_t pos = 0;
    while ((pos = scope.find(needle, pos)) != std::string::npos) {
      if (pos > 0 && is_identifier_char(scope[pos - 1])) {
        pos += needle.size();
        continue;
      }
      const auto start = pos + needle.size();
      const auto end = scope.find('"', start);
      const auto bracket = scope.find(']', end);
      const auto after = scope.find_first_not_of(" \t\n", bracket + 1);
      if (end == std::string::npos || bracket == std::string::npos || after == std::string::npos) {
        break;
      }
      if (scope[after] == '=' && (after + 1 >= scope.size() || scope[after + 1] != '=')) {
        keys.insert(scope.substr(start, end - start));
      }
      pos = end;
    }
    return keys;
  }

  std::set<std::string> json_value_keys(const std::string &scope, const std::string &variable) {
    std::set<std::string> keys;
    const std::string needle = variable + ".value(\"";
    std::size_t pos = 0;
    while ((pos = scope.find(needle, pos)) != std::string::npos) {
      const auto start = pos + needle.size();
      const auto end = scope.find('"', start);
      if (end == std::string::npos) {
        break;
      }
      keys.insert(scope.substr(start, end - start));
      pos = end;
    }
    return keys;
  }

  std::string without_whitespace(std::string text) {
    std::erase_if(text, [](unsigned char c) {
      return std::isspace(c) != 0;
    });
    return text;
  }

  nlohmann::json contract() {
    return nlohmann::json::parse(read_source_file("docs/benchmark-control-openapi.json"));
  }

  const nlohmann::json &schema(const nlohmann::json &document, std::string_view name) {
    return document.at("components").at("schemas").at(name);
  }
}  // namespace

TEST(BenchmarkControlContractTests, DeclaresEveryRegisteredRouteAndHandler) {
  const auto document = contract();
  const auto source = read_source_file("src/confighttp.cpp");
  std::size_t operation_count = 0;

  for (const auto &[path, path_item] : document.at("paths").items()) {
    SCOPED_TRACE(path);
    for (const auto method : {"get", "post", "delete"}) {
      if (!path_item.contains(method)) {
        continue;
      }
      ++operation_count;
      const auto &operation = path_item.at(method);
      const auto source_regex = operation.at("x-polaris-source-regex").get<std::string>();
      const auto handler = operation.at("x-polaris-handler").get<std::string>();
      std::string uppercase_method {method};
      std::ranges::transform(uppercase_method, uppercase_method.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
      });
      const auto registration =
        "server.resource[\"" + source_regex + "\"][\"" + uppercase_method + "\"] = " + handler + ";";
      EXPECT_NE(source.find(registration), std::string::npos)
        << "OpenAPI operation does not match a live Polaris route registration";

      const auto body = function_body(source, handler);
      ASSERT_FALSE(body.empty()) << "could not find handler body for " << handler;
      EXPECT_NE(body.find("authorizeBenchmarkHarnessRequest"), std::string::npos)
        << handler << " does not enforce the contract's bearer API-key security scheme";
    }
  }

  EXPECT_EQ(operation_count, 5U);
  std::size_t registered_count = 0;
  for (std::size_t pos = 0;
       (pos = source.find("server.resource[\"^/polaris/v1/session/timing/runs", pos)) != std::string::npos;
       ++pos) {
    ++registered_count;
  }
  EXPECT_EQ(registered_count, operation_count)
    << "A live benchmark route exists without a published OpenAPI operation";
}

TEST(BenchmarkControlContractTests, CreateRequestMatchesParserFieldsAndStorageUnits) {
  const auto document = contract();
  const auto &request_schema = schema(document, "CreateBenchmarkRunRequest");
  const auto confighttp = read_source_file("src/confighttp.cpp");
  const auto parser = function_body(confighttp, "createBenchmarkRun");

  ASSERT_FALSE(parser.empty());
  EXPECT_EQ(json_value_keys(parser, "inputTree"), property_names(request_schema));
  EXPECT_EQ(required_names(request_schema), property_names(request_schema));

  const auto engine = without_whitespace(function_body(read_source_file("src/stream_stats.cpp"), "create_benchmark_run"));
  ASSERT_FALSE(engine.empty());
  for (const auto &[field_name, field] : request_schema.at("properties").items()) {
    if (field.contains("minimum") && field.contains("maximum") && field_name != "sample_capacity_frames") {
      const auto range_check = without_whitespace(
        "if (request." + field_name + " < " + field.at("minimum").dump() +
        " || request." + field_name + " > " + field.at("maximum").dump() + ")");
      EXPECT_NE(engine.find(range_check), std::string::npos)
        << field_name << " range in OpenAPI does not match create_benchmark_run";
    }
    if (!field.contains("x-storage-field")) {
      continue;
    }
    const auto conversion = without_whitespace(
      "run." + field.at("x-storage-field").get<std::string>() + " = std::chrono::" +
      field.at("x-chrono-constructor").get<std::string>() + "(request." + field_name + ");");
    EXPECT_NE(engine.find(conversion), std::string::npos)
      << field_name << " unit conversion in OpenAPI does not match create_benchmark_run";
  }
}

TEST(BenchmarkControlContractTests, ResponseSchemasMatchLiveEmitters) {
  const auto document = contract();
  const auto source = read_source_file("src/confighttp.cpp");
  const auto &stage_schema = schema(document, "BenchmarkStageCapture");
  const auto &run_schema = schema(document, "BenchmarkRunRecord");
  const auto &command_schema = schema(document, "CommandResponse");

  EXPECT_EQ(required_names(stage_schema), property_names(stage_schema));
  EXPECT_EQ(required_names(run_schema), property_names(run_schema));
  EXPECT_EQ(required_names(command_schema), property_names(command_schema));
  EXPECT_EQ(
    subscript_assignment_keys(function_body(source, "benchmark_stage_capture_json"), "output"),
    property_names(stage_schema));
  EXPECT_EQ(
    subscript_assignment_keys(function_body(source, "benchmark_run_json"), "output"),
    property_names(run_schema));

  for (const auto handler : {
         "createBenchmarkRun", "startBenchmarkRun", "stopBenchmarkRun", "getBenchmarkRun", "deleteBenchmarkRun"}) {
    SCOPED_TRACE(handler);
    EXPECT_EQ(subscript_assignment_keys(function_body(source, handler), "outputTree"), property_names(command_schema));
  }

  const auto run_emitter = function_body(source, "benchmark_run_json");
  for (const auto fixed_field : {"schema_version", "measurement_spec_id", "clock_domain"}) {
    const auto expected =
      "output[\"" + std::string {fixed_field} + "\"] = " + run_schema.at("properties").at(fixed_field).at("const").dump() + ";";
    EXPECT_NE(run_emitter.find(expected), std::string::npos)
      << fixed_field << " fixed value in OpenAPI does not match benchmark_run_json";
  }
}

TEST(BenchmarkControlContractTests, CanonicalExamplesCoverEveryContractField) {
  const auto document = contract();
  const auto &request_schema = schema(document, "CreateBenchmarkRunRequest");
  const auto &command_schema = schema(document, "CommandResponse");
  const auto &stage_schema = schema(document, "BenchmarkStageCapture");
  const auto &run_schema = schema(document, "BenchmarkRunRecord");
  const auto &request_example = document.at("paths")
                                  .at("/polaris/v1/session/timing/runs")
                                  .at("post")
                                  .at("requestBody")
                                  .at("content")
                                  .at("application/json")
                                  .at("example");
  const auto &command_example = command_schema.at("example");
  const auto &run_example = run_schema.at("example");

  EXPECT_EQ(object_keys(request_example), property_names(request_schema));
  EXPECT_EQ(object_keys(command_example), property_names(command_schema));
  EXPECT_EQ(object_keys(run_example), property_names(run_schema));
  for (const auto stage_name : {"capture_to_encode", "encode_to_send_release", "capture_to_send_release"}) {
    SCOPED_TRACE(stage_name);
    EXPECT_EQ(object_keys(run_example.at(stage_name)), property_names(stage_schema));
  }

  EXPECT_EQ(run_example.at("schema_version"), run_schema.at("properties").at("schema_version").at("const"));
  EXPECT_EQ(
    run_example.at("measurement_spec_id"), run_schema.at("properties").at("measurement_spec_id").at("const"));
  EXPECT_EQ(run_example.at("clock_domain"), run_schema.at("properties").at("clock_domain").at("const"));
}
