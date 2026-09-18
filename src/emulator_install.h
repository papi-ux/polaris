/**
 * @file src/emulator_install.h
 * @brief One-click emulator installs from Flathub for the ROM folder presets.
 *
 * Only a preset's own Flatpak id is ever installed, for the account Polaris runs as and
 * without a shell. Keys, firmware and BIOS images are never fetched: the prerequisites
 * checks in emulator_library.h name what the player has to supply. A job runs in the
 * background because a download takes minutes; the console polls the folder list for it.
 */
#pragma once

#include "emulator_library.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace emulator_install {
  inline constexpr std::string_view flathub_remote = "flathub";
  inline constexpr std::string_view flathub_repo_url = "https://dl.flathub.org/repo/flathub.flatpakrepo";
  inline constexpr std::chrono::milliseconds remotes_timeout = std::chrono::seconds {30};
  inline constexpr std::chrono::milliseconds remote_add_timeout = std::chrono::minutes {2};
  /// Emulators with their runtimes are hundreds of megabytes; a slow link still finishes.
  inline constexpr std::chrono::milliseconds install_timeout = std::chrono::minutes {45};
  inline constexpr std::size_t maximum_message_bytes = 400;

  enum class state_e {
    installing,
    installed,
    failed,
  };

  inline std::string_view state_name(state_e state) {
    switch (state) {
      case state_e::installing:
        return "installing";
      case state_e::installed:
        return "installed";
      case state_e::failed:
        break;
    }
    return "failed";
  }

  struct job_t {
    state_e state = state_e::installing;
    std::string message;
    std::int64_t started_at = 0;  ///< epoch seconds
    std::int64_t finished_at = 0;  ///< epoch seconds, 0 while installing
  };

  struct run_result_t {
    int exit_status = 127;
    bool timed_out = false;
    std::string output;  ///< standard output and standard error, interleaved
  };

  /// Runs one program without a shell and waits for it, killing it at the timeout.
  using runner_t = std::function<run_result_t(const std::vector<std::string> &argv, std::chrono::milliseconds timeout)>;

  inline std::vector<std::string> remotes_argv(const std::string &flatpak) {
    return {flatpak, "remotes", "--user", "--columns=name"};
  }

  inline std::vector<std::string> remote_add_argv(const std::string &flatpak) {
    return {flatpak, "remote-add", "--user", "--if-not-exists", std::string(flathub_remote), std::string(flathub_repo_url)};
  }

  inline std::vector<std::string> install_argv(const std::string &flatpak, std::string_view flatpak_id) {
    return {flatpak, "install", "--user", "--noninteractive", "-y", std::string(flathub_remote), std::string(flatpak_id)};
  }

  /// Whether `flatpak remotes --columns=name` lists the remote, one name per line.
  inline bool remote_listed(std::string_view remotes_output, std::string_view remote) {
    std::size_t start = 0;
    while (start <= remotes_output.size()) {
      const auto end = remotes_output.find('\n', start);
      const auto line = emulator_library::trim_view(remotes_output.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
      if (line == remote) {
        return true;
      }
      if (end == std::string_view::npos) {
        break;
      }
      start = end + 1;
    }
    return false;
  }

  /// The last line Flatpak printed that says something, bounded; Flatpak ends a failure with its reason.
  inline std::string last_output_line(std::string_view output) {
    std::string_view last;
    std::size_t start = 0;
    while (start <= output.size()) {
      const auto end = output.find('\n', start);
      auto line = output.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start);
      // Progress redraws a line with carriage returns; only the final state of it counts.
      if (const auto carriage = line.rfind('\r'); carriage != std::string_view::npos) {
        line = line.substr(carriage + 1);
      }
      line = emulator_library::trim_view(line);
      if (!line.empty()) {
        last = line;
      }
      if (end == std::string_view::npos) {
        break;
      }
      start = end + 1;
    }
    std::string message(last.substr(0, maximum_message_bytes));
    return message;
  }

  /**
   * @brief What a failed step says to the player, with Flatpak's own reason when it gave one.
   *
   * @param attempt what was being done, such as "Installing Eden from Flathub".
   */
  inline std::string failure_message(std::string_view attempt, const run_result_t &result) {
    if (result.timed_out) {
      return std::string(attempt) + " took too long and was stopped.";
    }
    const auto last_line = last_output_line(result.output);
    std::string_view reason = last_line;
    // Flatpak prefixes its reason with "error: "; the sentence already says it failed.
    if (constexpr std::string_view prefix = "error:"; reason.size() >= prefix.size() &&
                                                       std::equal(prefix.begin(), prefix.end(), reason.begin(), [](char a, char b) {
                                                         return a == std::tolower(static_cast<unsigned char>(b));
                                                       })) {
      reason = emulator_library::trim_view(reason.substr(prefix.size()));
    }
    if (reason.empty()) {
      return std::string(attempt) + " failed (exit status " + std::to_string(result.exit_status) + ").";
    }
    return std::string(attempt) + " failed: " + std::string(reason);
  }

  enum class start_e {
    started,
    already_running,
    not_installable,
  };

  /**
   * @brief The process-wide install jobs, one per emulator.
   *
   * The runner and the Flatpak lookup are injectable so tests drive a fake Flatpak; the
   * production pair runs the real binary found on the service's PATH.
   */
  class installer_t {
  public:
    using on_installed_t = std::function<void(const emulator_library::preset_t &)>;
    using locator_t = std::function<std::optional<std::string>()>;

    installer_t(runner_t runner, locator_t locator):
        runner_(std::move(runner)),
        locator_(std::move(locator)) {
    }

    /// The Flatpak binary this host would run, when there is one.
    std::optional<std::string> flatpak_binary() const {
      std::lock_guard lock(mutex_);
      return locator_ ? locator_() : std::nullopt;
    }

    /// A preset can be installed here when it names a Flatpak and Flatpak itself is available.
    bool installable(const emulator_library::preset_t &preset) const {
      return !preset.flatpak_id.empty() && flatpak_binary().has_value();
    }

    std::optional<job_t> job(std::string_view emulator_id) const {
      std::lock_guard lock(mutex_);
      const auto it = jobs_.find(std::string(emulator_id));
      if (it == jobs_.end()) {
        return std::nullopt;
      }
      return it->second;
    }

    start_e start(const emulator_library::preset_t &preset, on_installed_t on_installed) {
      std::unique_lock lock(mutex_);
      const auto flatpak = locator_ ? locator_() : std::nullopt;
      if (preset.flatpak_id.empty() || !flatpak) {
        return start_e::not_installable;
      }
      const std::string id {preset.id};
      if (const auto it = jobs_.find(id); it != jobs_.end() && it->second.state == state_e::installing) {
        return start_e::already_running;
      }
      jobs_[id] = job_t {state_e::installing, "Installing " + std::string(preset.label) + " from Flathub.", now_seconds(), 0};
      ++running_;
      const auto runner = runner_;
      lock.unlock();

      std::thread([this, preset, flatpak = *flatpak, runner, on_installed = std::move(on_installed)]() {
        auto outcome = run_steps(preset, flatpak, runner);
        if (outcome.state == state_e::installed && on_installed) {
          try {
            on_installed(preset);
          } catch (...) {
            // The emulator is installed either way; a stale saved command still launches, because launch resolves it.
          }
        }
        std::lock_guard finished(mutex_);
        outcome.started_at = jobs_[std::string(preset.id)].started_at;
        outcome.finished_at = now_seconds();
        jobs_[std::string(preset.id)] = outcome;
        --running_;
        idle_.notify_all();
      }).detach();
      return start_e::started;
    }

    /// Replace the runner and the Flatpak lookup, and forget every job; tests only.
    void reset_for_tests(runner_t runner, locator_t locator) {
      std::lock_guard lock(mutex_);
      runner_ = std::move(runner);
      locator_ = std::move(locator);
      jobs_.clear();
    }

    /// Wait until no job is running; tests only.
    bool wait_idle_for_tests(std::chrono::milliseconds timeout) {
      std::unique_lock lock(mutex_);
      return idle_.wait_for(lock, timeout, [this]() {
        return running_ == 0;
      });
    }

  private:
    static std::int64_t now_seconds() {
      return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    }

    static job_t run_steps(const emulator_library::preset_t &preset, const std::string &flatpak, const runner_t &runner) {
      const std::string label {preset.label};
      job_t outcome;
      if (!runner) {
        outcome.state = state_e::failed;
        outcome.message = "Polaris cannot run Flatpak on this host.";
        return outcome;
      }
      const auto remotes = runner(remotes_argv(flatpak), remotes_timeout);
      if (remotes.exit_status != 0 || !remote_listed(remotes.output, flathub_remote)) {
        const auto added = runner(remote_add_argv(flatpak), remote_add_timeout);
        if (added.exit_status != 0 || added.timed_out) {
          outcome.state = state_e::failed;
          outcome.message = failure_message("Adding Flathub for " + label, added);
          return outcome;
        }
      }
      const auto installed = runner(install_argv(flatpak, preset.flatpak_id), install_timeout);
      if (installed.exit_status != 0 || installed.timed_out) {
        outcome.state = state_e::failed;
        outcome.message = failure_message("Installing " + label + " from Flathub", installed);
        return outcome;
      }
      outcome.state = state_e::installed;
      outcome.message = label + " is installed.";
      return outcome;
    }

    mutable std::mutex mutex_;
    std::condition_variable idle_;
    runner_t runner_;
    locator_t locator_;
    std::map<std::string, job_t> jobs_;
    int running_ = 0;
  };
}  // namespace emulator_install
