#include "ai_claude_cli.h"
#include "posix_child_reaper.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <vector>

#include <nlohmann/json.hpp>

#ifndef _WIN32
#ifdef __linux__
#include "platform/linux/process_environment.h"
#endif
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <crt_externs.h>
#else
extern char **environ;
#endif
#endif

namespace ai_optimizer::claude_cli {
  namespace {
    constexpr std::size_t max_response_bytes = 64 * 1024;

#ifndef _WIN32
    struct workspace_t {
      std::filesystem::path path;
      workspace_t() {
#ifdef __linux__
        std::lock_guard lock(process_environment::mutex);
#endif
        auto name = (std::filesystem::temp_directory_path() / "polaris-claude-XXXXXX").string();
        if (mkdtemp(name.data())) path = name;
      }
      ~workspace_t() {
        if (path.empty()) return;
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
      }
      workspace_t(const workspace_t &) = delete;
      workspace_t &operator=(const workspace_t &) = delete;
    };

    struct fd_t {
      int value = -1;
      ~fd_t() { if (value >= 0) close(value); }
    };

    bool write_input(const std::filesystem::path &path, const std::string &value) {
      fd_t file {open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600)};
      if (file.value < 0) return false;
      std::size_t offset = 0;
      while (offset < value.size()) {
        const auto count = write(file.value, value.data() + offset, value.size() - offset);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) return false;
        offset += static_cast<std::size_t>(count);
      }
      return true;
    }

    using environment_t = std::map<std::string, std::string>;

    environment_t snapshot_environment() {
#ifdef __linux__
      return process_environment::snapshot();
#else
      environment_t result;
      for (auto entry = *_NSGetEnviron(); entry && *entry; ++entry) {
        const std::string value {*entry};
        const auto separator = value.find('=');
        if (separator != std::string::npos) result[value.substr(0, separator)] = value.substr(separator + 1);
      }
      return result;
#endif
    }

    std::string binary(const environment_t &environment) {
      const auto home = environment.find("HOME");
      const std::array<std::string, 3> candidates {
        home != environment.end() ? home->second + "/.local/bin/claude" : "",
        "/usr/local/bin/claude", "/usr/bin/claude"
      };
      for (const auto &candidate : candidates) {
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec) && access(candidate.c_str(), X_OK) == 0) return candidate;
      }
      return "claude";
    }

    // Resolve PATH from the same owned snapshot passed to the child. spawnp
    // consults the live parent PATH even when given an independent envp.
    std::string resolve_binary(const std::string &name, const environment_t &environment) {
      if (name.find('/') != std::string::npos) return std::filesystem::absolute(name).string();
      const auto found = environment.find("PATH");
      std::string path;
      if (found != environment.end()) path = found->second;
      else {
        path.resize(confstr(_CS_PATH, nullptr, 0));
        if (!path.empty()) {
          confstr(_CS_PATH, path.data(), path.size());
          path.pop_back();
        }
      }
      std::size_t start = 0;
      do {
        const auto end = path.find(':', start);
        const auto directory = path.substr(start, end == std::string::npos ? end : end - start);
        const auto candidate = std::filesystem::absolute(std::filesystem::path(directory) / name);
        std::error_code ec;
        if (std::filesystem::is_regular_file(candidate, ec) && access(candidate.c_str(), X_OK) == 0) return candidate.string();
        if (end == std::string::npos) break;
        start = end + 1;
      } while (true);
      return {};
    }

    struct process_result_t {
      int exit_code = -1;
      bool timed_out = false;
      bool truncated = false;
      std::string output;
    };

    // No shell, global chdir, or environment mutation. Keep the child unreaped
    // until its process group has been stopped, including pipe holders that
    // remain in that group. Administrator hooks are trusted host software.
    process_result_t run(const std::vector<std::string> &args,
                         const std::filesystem::path &cwd,
                         const std::filesystem::path &input,
                         int timeout_ms, const environment_t &environment) {
      process_result_t result;
      const auto executable = resolve_binary(args.front(), environment);
      if (executable.empty()) {
        result.exit_code = 127;
        return result;
      }
      int pipe_fds[2];
#ifdef __linux__
      if (pipe2(pipe_fds, O_CLOEXEC) != 0) return result;
#else
      if (pipe(pipe_fds) != 0) return result;
#endif
      fd_t read_end {pipe_fds[0]}, write_end {pipe_fds[1]};
      for (auto *fd : {&read_end, &write_end}) {
        if (fd->value > STDERR_FILENO) continue;
        const auto replacement = fcntl(fd->value, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
        if (replacement < 0) return result;
        close(fd->value);
        fd->value = replacement;
      }
      if (fcntl(read_end.value, F_SETFD, FD_CLOEXEC) < 0 ||
          fcntl(write_end.value, F_SETFD, FD_CLOEXEC) < 0 ||
          fcntl(read_end.value, F_SETFL, O_NONBLOCK) < 0) return result;

      struct actions_t {
        posix_spawn_file_actions_t value;
        bool ready = posix_spawn_file_actions_init(&value) == 0;
        ~actions_t() { if (ready) posix_spawn_file_actions_destroy(&value); }
      } actions;
      struct attributes_t {
        posix_spawnattr_t value;
        bool ready = posix_spawnattr_init(&value) == 0;
        ~attributes_t() { if (ready) posix_spawnattr_destroy(&value); }
      } attributes;
      if (!actions.ready || !attributes.ready) return result;
      sigset_t mask, defaults;
      sigemptyset(&mask);
      sigemptyset(&defaults);
      for (int signal : {SIGTERM, SIGINT, SIGPIPE}) sigaddset(&defaults, signal);
      short flags = POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
#ifdef __APPLE__
      flags |= POSIX_SPAWN_CLOEXEC_DEFAULT;
#endif
#if defined(__APPLE__) && __MAC_OS_X_VERSION_MIN_REQUIRED >= 260000
      const auto chdir_error = posix_spawn_file_actions_addchdir(&actions.value, cwd.c_str());
#else
      const auto chdir_error = posix_spawn_file_actions_addchdir_np(&actions.value, cwd.c_str());
#endif
      if (chdir_error != 0 || posix_spawnattr_setflags(&attributes.value, flags) != 0 ||
          posix_spawnattr_setpgroup(&attributes.value, 0) != 0 ||
          posix_spawnattr_setsigmask(&attributes.value, &mask) != 0 ||
          posix_spawnattr_setsigdefault(&attributes.value, &defaults) != 0 ||
          posix_spawn_file_actions_addopen(&actions.value, STDIN_FILENO, input.c_str(), O_RDONLY, 0) != 0 ||
          posix_spawn_file_actions_adddup2(&actions.value, write_end.value, STDOUT_FILENO) != 0 ||
          posix_spawn_file_actions_addopen(&actions.value, STDERR_FILENO, "/dev/null", O_WRONLY, 0) != 0 ||
          posix_spawn_file_actions_addclose(&actions.value, read_end.value) != 0 ||
          posix_spawn_file_actions_addclose(&actions.value, write_end.value) != 0) return result;
#ifdef __linux__
      // Do not pass unrelated host sockets/devices to auth status or inference.
      if (posix_spawn_file_actions_addclosefrom_np(&actions.value, STDERR_FILENO + 1) != 0) return result;
#endif
      std::vector<char *> argv;
      for (const auto &arg : args) argv.push_back(const_cast<char *>(arg.c_str()));
      argv.push_back(nullptr);
      pid_t child = -1;
      std::vector<std::string> environment_storage;
      environment_storage.reserve(environment.size());
      for (const auto &[key, value] : environment) environment_storage.push_back(key + "=" + value);
      std::vector<char *> envp;
      for (auto &entry : environment_storage) envp.push_back(entry.data());
      envp.push_back(nullptr);
      util::posix_children::protected_child_t ownership;
      const int error = posix_spawn(&child, executable.c_str(), &actions.value, &attributes.value, argv.data(), envp.data());
      if (error != 0) {
        result.exit_code = error == ENOENT ? 127 : 126;
        return result;
      }
      ownership.publish(child);
      close(write_end.value);
      write_end.value = -1;
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
      bool eof = false;
      for (;;) {
        // One bounded read per iteration prevents a flooding child starving the deadline.
        std::array<char, 4096> buffer;
        const auto count = read(read_end.value, buffer.data(), buffer.size());
        if (count > 0) {
          if (result.output.size() + static_cast<std::size_t>(count) > max_response_bytes) {
            result.truncated = true;
            break;
          }
          result.output.append(buffer.data(), static_cast<std::size_t>(count));
        } else if (count == 0) {
          eof = true;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
          break;
        }
        siginfo_t info {};
        if (waitid(P_PID, child, &info, WEXITED | WNOHANG | WNOWAIT) < 0 && errno != EINTR) break;
        if (info.si_pid == child && eof) break;
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) {
          result.timed_out = true;
          break;
        }
        if (count <= 0) {
          pollfd descriptor {read_end.value, POLLIN, 0};
          // An EOF pipe is always readable; use a timed wait while its child exits.
          poll(eof ? nullptr : &descriptor, eof ? 0 : 1, static_cast<int>(std::min<std::int64_t>(remaining, 20)));
        }
      }
      result.exit_code = ownership.finish();
      return result;
    }
#endif

    result_t failure(std::string code, std::string error, std::string action) {
      return {std::nullopt, std::move(code), std::move(error), std::move(action)};
    }
  }

#ifndef _WIN32
  static status_t status_with_environment(const std::string &executable, const environment_t &environment) {
    try {
      workspace_t workspace;
      if (workspace.path.empty()) return {};
      const auto result = run({executable.empty() ? binary(environment) : executable, "--safe-mode", "auth", "status"}, workspace.path, "/dev/null", 5000, environment);
      status_t status {result.exit_code != 127 && result.exit_code != 126 && result.exit_code != -1, std::nullopt};
      if (!result.timed_out && !result.truncated && (result.exit_code == 0 || result.exit_code == 1)) {
        const auto parsed = nlohmann::json::parse(result.output, nullptr, false);
        if (parsed.is_object() && parsed.contains("loggedIn") && parsed["loggedIn"].is_boolean()) {
          if (!parsed["loggedIn"].get<bool>()) {
            status.authenticated = false;
          } else if (parsed.contains("authMethod") && parsed["authMethod"].is_string()) {
            status.authenticated = result.exit_code == 0 && parsed["authMethod"] == "claude.ai";
          }
        }
      }
      return status;
    } catch (...) { return {}; }
  }
#endif

  status_t status(const std::string &executable) {
#ifndef _WIN32
    try { return status_with_environment(executable, snapshot_environment()); }
    catch (...) { return {}; }
#else
    return {};
#endif
  }

  result_t explain(const std::string &model, const std::string &system_prompt,
                   const std::string &schema, const std::string &evidence, int timeout_ms,
                   const std::string &executable) {
#ifndef _WIN32
    try {
      const auto environment = snapshot_environment();
      const auto cli = resolve_binary(executable.empty() ? binary(environment) : executable, environment);
      if (cli.empty()) return failure("cli_unavailable", "Claude CLI is unavailable to Polaris", "Install Claude Code for the user running Polaris, then retry.");
      const auto auth = status_with_environment(cli, environment);
      if (!auth.available) return failure("cli_unavailable", "Claude CLI is unavailable to Polaris", "Install Claude Code for the user running Polaris, then retry.");
      if (auth.authenticated == false) return failure("authentication_failed", "Claude subscription is not signed in", "Run claude auth login on the Polaris host as the user running Polaris, then retry.");
      if (!auth.authenticated.has_value()) return failure("cli_auth_unverified", "Claude sign-in could not be verified", "Update Claude Code and run claude auth status as the user running Polaris, then retry.");
      workspace_t workspace;
      if (workspace.path.empty() || evidence.size() > max_response_bytes ||
          !write_input(workspace.path / "prompt.txt", evidence) ||
          !write_input(workspace.path / "system.txt", system_prompt)) {
        return failure("cli_request_failed", "Could not prepare the Claude explanation request", "Retry with a smaller support report and check temporary-directory access.");
      }
      // --bare intentionally ignores subscription OAuth. Safe mode preserves
      // authentication while disabling ordinary discovered customizations.
      // Administrator-managed hooks/policy remain part of the trusted installed
      // CLI. Polaris never interprets its response as a host-control command.
      // Tools and MCP are disabled separately; never relax flags for an older CLI.
      const auto result = run({cli, "--safe-mode", "--print",
        "--tools", "", "--strict-mcp-config", "--mcp-config", "{\"mcpServers\":{}}",
        "--disable-slash-commands", "--setting-sources", "",
        "--settings", "{\"disableAllHooks\":true,\"autoMemoryEnabled\":false}",
        "--no-session-persistence", "--no-chrome", "--max-turns", "3",
        "--system-prompt-file", (workspace.path / "system.txt").string(),
        "--model", model, "--output-format", "json", "--json-schema", schema},
        workspace.path, workspace.path / "prompt.txt", std::clamp(timeout_ms, 1000, 120000), environment);
      if (result.timed_out) return failure("inference_timeout", "Claude explanation timed out", "Increase Provider timeout (up to 120000 ms), then retry.");
      if (result.truncated) return failure("invalid_response", "Claude returned an oversized response", "Use a concise explanation model, then retry.");
      const auto parsed = nlohmann::json::parse(result.output, nullptr, false);
      if (result.exit_code != 0 || !parsed.is_object() || parsed.value("type", std::string()) != "result" ||
          parsed.value("subtype", std::string()) != "success" || parsed.value("is_error", true)) {
        return failure("cli_request_failed", "Claude did not complete the explanation request", "Check the selected model and update Claude Code to a version supporting --safe-mode, then retry.");
      }
      if (!parsed.contains("structured_output") || !parsed["structured_output"].is_object()) {
        return failure("invalid_response", "Claude returned no structured explanation", "Update Claude Code and use a model that supports structured output, then retry.");
      }
      return {parsed["structured_output"].dump(), {}, {}, {}};
    } catch (...) {
      // JSON exceptions and CLI output may contain evidence or account details.
      return failure("invalid_response", "Claude returned an unreadable response", "Check the selected model and Claude Code installation, then retry.");
    }
#else
    return failure("explanation_transport_unsupported", "Claude Doctor explanations require Linux or macOS", "Choose an OpenAI-compatible endpoint on this host.");
#endif
  }
}
