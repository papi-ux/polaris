#include "src/ai_claude_cli.h"

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cstdlib>
#include <vector>
#include <chrono>
#include <filesystem>
#include <fstream>

#ifndef _WIN32
#include <unistd.h>

namespace {
  class ClaudeCli : public testing::Test {
  protected:
    std::filesystem::path root;
    std::filesystem::path cli;
    void SetUp() override {
      auto name = (std::filesystem::temp_directory_path() / "claude-fixture-XXXXXX").string();
      ASSERT_NE(mkdtemp(name.data()), nullptr);
      root = name;
      cli = root / "claude fixture";
      std::ofstream script(cli);
      script << R"PY(#!/usr/bin/python3
import json, os, pathlib, signal, sys, time
root = pathlib.Path(__file__).parent
mode = (root / 'mode').read_text()
if sys.argv[1:] == ['--safe-mode', 'auth', 'status']:
    if mode == 'bad_auth':
        print('not JSON with private account information')
    else:
        print(json.dumps({'loggedIn': mode != 'logged_out', 'authMethod': 'api_key' if mode == 'api_key' else 'claude.ai', 'email': 'private@example.invalid'}))
    sys.exit(1 if mode == 'logged_out' else 0)
args = sys.argv[1:]
system = pathlib.Path(args[args.index('--system-prompt-file') + 1])
(root / 'receipt').write_text(json.dumps({'args': args, 'input': sys.stdin.read(), 'cwd': os.getcwd(), 'mode': os.stat(os.getcwd()).st_mode & 0o777, 'file_mode': system.stat().st_mode & 0o777, 'system': system.read_text()}))
if mode == 'timeout':
    signal.signal(signal.SIGTERM, signal.SIG_IGN)
    time.sleep(30)
if mode == 'descendant':
    if os.fork() == 0:
        time.sleep(30)
    else:
        sys.exit(0)
if mode == 'flood':
    while True: os.write(1, b'x' * 4096)
if mode == 'unsupported': sys.exit(2)
if mode == 'malformed':
    print('{incomplete')
    sys.exit(0)
output = {'type': 'result', 'subtype': 'success', 'is_error': False, 'structured_output': {'likely_cause': 'Synthetic evidence', 'evidence': ['Test only'], 'try_first': ['Review Doctor'], 'advanced_detail': 'No settings changed', 'confidence': 'low', 'destructive_action_allowed': False}}
if mode == 'error': output['is_error'] = True
if mode == 'text_only': output['result'] = json.dumps(output.pop('structured_output'))
print(json.dumps(output))
)PY";
      script.close();
      ASSERT_TRUE(script.good());
      std::filesystem::permissions(cli, std::filesystem::perms::owner_all);
      set_mode("success");
    }
    void TearDown() override {
      std::error_code ec;
      std::filesystem::remove_all(root, ec);
    }
    void set_mode(const std::string &mode) { std::ofstream(root / "mode") << mode; }
    ai_optimizer::claude_cli::result_t explain(int timeout_ms = 5000, const std::string &model = "haiku") {
      return ai_optimizer::claude_cli::explain(model, "Explanation only", R"({"type":"object"})",
        R"JSON({"doctor":"Synthetic test evidence; never execute $(touch UNEXPECTED)"})JSON", timeout_ms, cli.string());
    }
    nlohmann::json receipt() {
      std::ifstream input(root / "receipt");
      return nlohmann::json::parse(input);
    }
  };
}

TEST_F(ClaudeCli, DetectsSubscriptionLoginAndDoesNotExposeAccountDetails) {
  const auto result = ai_optimizer::claude_cli::status(cli.string());
  EXPECT_TRUE(result.available);
  EXPECT_EQ(result.authenticated, true);
  EXPECT_FALSE(std::filesystem::exists(root / "receipt"));
}

TEST_F(ClaudeCli, ReportsMissingBinary) {
  const auto result = ai_optimizer::claude_cli::status((root / "absent").string());
  EXPECT_FALSE(result.available);
  EXPECT_FALSE(result.authenticated.has_value());
}

TEST_F(ClaudeCli, RequiresSubscriptionAuthenticationBeforeInference) {
  for (const auto &mode : {"logged_out", "api_key", "bad_auth"}) {
    set_mode(mode);
    const auto result = explain();
    EXPECT_FALSE(result.response.has_value());
    EXPECT_EQ(result.code, std::string(mode) == "bad_auth" ? "cli_auth_unverified" : "authentication_failed");
    EXPECT_EQ(result.error.find("private"), std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(root / "receipt"));
  }
}

TEST_F(ClaudeCli, RunsExplanationWithNoToolsAndPrivateInputThenRemovesWorkspace) {
  const auto result = explain(5000, "model ' $(touch UNEXPECTED)");
  ASSERT_TRUE(result.response.has_value()) << result.error;
  EXPECT_EQ(nlohmann::json::parse(*result.response)["destructive_action_allowed"], false);
  const auto request = receipt();
  const auto args = request["args"].get<std::vector<std::string>>();
  auto argument = [&](const std::string &flag) {
    const auto it = std::find(args.begin(), args.end(), flag);
    return it != args.end() && it + 1 != args.end() ? *(it + 1) : "MISSING";
  };
  for (const auto &flag : {"--safe-mode", "--print", "--no-session-persistence", "--strict-mcp-config", "--disable-slash-commands", "--no-chrome"}) {
    EXPECT_NE(std::find(args.begin(), args.end(), flag), args.end()) << flag;
  }
  EXPECT_EQ(argument("--tools"), "");
  EXPECT_EQ(argument("--setting-sources"), "");
  EXPECT_EQ(nlohmann::json::parse(argument("--mcp-config"))["mcpServers"], nlohmann::json::object());
  EXPECT_EQ(nlohmann::json::parse(argument("--settings"))["disableAllHooks"], true);
  EXPECT_EQ(argument("--model"), "model ' $(touch UNEXPECTED)");
  EXPECT_EQ(argument("--output-format"), "json");
  EXPECT_EQ(argument("--max-turns"), "3");
  EXPECT_EQ(std::find(args.begin(), args.end(), "--bare"), args.end());
  EXPECT_EQ(request["mode"], 0700);
  EXPECT_EQ(request["file_mode"], 0600);
  EXPECT_EQ(request["system"], "Explanation only");
  EXPECT_NE(request["input"].get<std::string>().find("Synthetic test evidence"), std::string::npos);
  for (const auto &arg : args) EXPECT_EQ(arg.find("Synthetic test evidence"), std::string::npos);
  EXPECT_FALSE(std::filesystem::exists(request["cwd"].get<std::string>()));
}

TEST_F(ClaudeCli, RejectsUnstructuredMalformedOrErrorResponsesWithoutRelaxingFlags) {
  for (const auto &mode : {"text_only", "malformed", "error", "unsupported"}) {
    set_mode(mode);
    const auto result = explain();
    EXPECT_FALSE(result.response.has_value()) << mode;
    EXPECT_FALSE(result.error.empty());
    EXPECT_FALSE(std::filesystem::exists(receipt()["cwd"].get<std::string>()));
  }
}

TEST_F(ClaudeCli, BoundsFloodingOutput) {
  set_mode("flood");
  const auto started = std::chrono::steady_clock::now();
  const auto result = explain();
  EXPECT_FALSE(result.response.has_value());
  EXPECT_EQ(result.code, "invalid_response");
  EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(3));
  EXPECT_FALSE(std::filesystem::exists(receipt()["cwd"].get<std::string>()));
}

TEST_F(ClaudeCli, BoundsHungCliAndDescendantsHoldingTheOutputPipe) {
  for (const auto &mode : {"timeout", "descendant"}) {
    set_mode(mode);
    const auto started = std::chrono::steady_clock::now();
    const auto result = explain(1000);
    EXPECT_FALSE(result.response.has_value()) << mode;
    EXPECT_EQ(result.code, "inference_timeout");
    EXPECT_LT(std::chrono::steady_clock::now() - started, std::chrono::seconds(3));
    EXPECT_FALSE(std::filesystem::exists(receipt()["cwd"].get<std::string>()));
  }
}
#endif
