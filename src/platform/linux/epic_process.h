/**
 * @file src/platform/linux/epic_process.h
 * @brief Observe an Epic title and retain one explicitly adopted process.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace platf::epic {
  struct title_t {
    std::string namespace_id, item_id, artifact_id;
    std::filesystem::path install_dir;
    std::string launch_executable;
    std::filesystem::path data_root;
    std::string platform;
  };

  struct observation_t {
    bool available {false};
    std::string reason, revision, runtime;
    std::uint64_t pid {0}, start_ticks {0};
  };

  struct close_result_t {
    bool closed {false};
    bool signalled {false};
    std::string reason;
  };

  namespace detail {
    struct engine_t;
  }

  /** An owner consent grant, not a launch/URI causality claim. Destruction only closes the pidfd. */
  class adopted_game_t {
  public:
    ~adopted_game_t();
    adopted_game_t(adopted_game_t &&) noexcept;
    adopted_game_t &operator=(adopted_game_t &&) noexcept;
    adopted_game_t(const adopted_game_t &) = delete;
    adopted_game_t &operator=(const adopted_game_t &) = delete;

  private:
    struct state_t;
    explicit adopted_game_t(std::unique_ptr<state_t> state);
    std::unique_ptr<state_t> state_;
    friend struct detail::engine_t;
    friend observation_t inspect(const adopted_game_t &);
    friend close_result_t ask_to_close(adopted_game_t &, std::chrono::milliseconds);
  };

  // Selection alone grants no authority. Only the authenticated host-owner consent path calls adopt.
  observation_t observe(const title_t &title);
  std::unique_ptr<adopted_game_t> adopt(const title_t &, const observation_t &, std::string &error);
  observation_t inspect(const adopted_game_t &);
  close_result_t ask_to_close(adopted_game_t &, std::chrono::milliseconds timeout);
  bool desktop_launcher_running(std::string &reason);
  std::optional<std::filesystem::path> desktop_launcher_executable(std::string &reason);

  /**
   * Explicit, per-call fixture dependencies. Production never installs a global override.
   * A fixture cannot manufacture a production grant: its backend stays attached to the grant.
   * No argv, environ or account store is part of this interface's reader implementation.
   */
  namespace testing {
    struct file_t {
      std::uint64_t device {0}, inode {0}, uid {0}, mode {0}, size {0};
      std::int64_t modified_ns {0}, changed_ns {0};
    };

    enum class exit_state_t { alive, exited, unavailable };

    struct backend_t {
      virtual ~backend_t() = default;
      virtual std::uint64_t owner_uid() const = 0;
      virtual std::optional<std::string> read(const std::filesystem::path &, std::size_t limit) = 0;
      virtual std::optional<std::string> prefix(const std::filesystem::path &, std::size_t length) = 0;
      virtual std::optional<std::filesystem::path> link(const std::filesystem::path &) = 0;
      virtual std::optional<std::filesystem::path> canonical(const std::filesystem::path &) = 0;
      virtual std::optional<file_t> stat(const std::filesystem::path &) = 0;
      virtual std::optional<std::vector<std::string>> list(const std::filesystem::path &, std::size_t limit) = 0;
      virtual int open_pidfd(std::uint64_t pid) = 0;
      virtual std::optional<std::uint64_t> pidfd_pid(int fd) = 0;
      virtual exit_state_t wait(int fd, std::chrono::milliseconds timeout) = 0;
      virtual bool term(int fd) = 0;
      virtual void close(int fd) = 0;
    };

    observation_t observe(const title_t &, std::shared_ptr<backend_t>);
    std::unique_ptr<adopted_game_t> adopt(const title_t &, const observation_t &, std::string &error, std::shared_ptr<backend_t>);
    std::optional<std::filesystem::path> desktop_launcher_executable(std::string &reason, std::shared_ptr<backend_t>);
    // The actual bounded reader, also usable with inert fixture files (including size-zero procfs).
    std::optional<std::string> read_bounded(const std::filesystem::path &, std::size_t limit);
  }
}  // namespace platf::epic
