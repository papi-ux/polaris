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

namespace vrr_clock {
  class compositor_lease_t;
}

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
    std::size_t uninspected_process_count {0};
  };

  struct close_result_t {
    bool closed {false};
    bool signalled {false};
    std::string reason;
  };

  namespace detail {
    struct engine_t;
  }

  /** An authorized exact-game grant; Mirror needs owner consent. Not URI causality. Destruction only closes the pidfd. */
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

  /** A previously proven original executable, never a guessed path or mounted inner ELF. */
  class launcher_proof_t {
  public:
    std::filesystem::path path() const;

  private:
    struct state_t;
    explicit launcher_proof_t(std::shared_ptr<const state_t>);
    std::shared_ptr<const state_t> state_;
    friend struct detail::engine_t;
  };

  // Counts process identities, not logical UI instances. Unlike Mirror presence, nullopt is
  // unknown. An unassociated AppImage runtime cannot establish absence, even if renamed. A task
  // running the retained launcher's file counts whatever path it runs it by. A protected
  // (unreadable) task counts as not Epic only when its world-readable stat, status and cgroup
  // facts, and its ancestors', place it as the user manager, a vendor user service or a login
  // session (D-A (B), see session_role); any other protected task leaves this unknown.
  std::optional<std::size_t> launcher_instances(std::string &reason, const launcher_proof_t *known_launcher = nullptr);
  std::optional<launcher_proof_t> prove_launcher(std::string &reason);
  // Revalidates the retained original file without requiring a launcher or AppImage mount.
  bool verify_launcher(const launcher_proof_t &, std::string &reason);

  // A sampled admission check, never dispatch authority: true means the requested game
  // and its exact prefix server are absent, false means busy, nullopt means unknown.
  // Recheck immediately before dispatch; concurrent launches remain possible afterwards.
  // Protected tasks are set aside by the same role rule as launcher_instances. A missing or
  // unreadable prefix reports "title_prefix_unavailable"; a first play in Mirror Desktop creates one.
  std::optional<bool> title_runtime_absent(const title_t &, std::string &reason);

  /**
   * An opaque scope of the caller's validated, published private compositor generation.
   * Its supervisor is derived from the retained lease, never from request-supplied integers.
   */
  class private_session_t {
  public:
    private_session_t(const private_session_t &) = default;
    private_session_t &operator=(const private_session_t &) = default;

  private:
    struct state_t;
    explicit private_session_t(std::shared_ptr<const state_t>);
    std::shared_ptr<const state_t> state_;
    friend struct detail::engine_t;
  };

  // Only pass the runtime lease obtained for this exact validated generation and socket.
  std::optional<private_session_t> pin_private_session(
    std::shared_ptr<const vrr_clock::compositor_lease_t>, const std::string &generation, std::string &reason);
  // The trusted caller records a nonzero dispatch bound in /proc start-time ticks. It may
  // precede supervisor startup when the URI is its kiosk client; ancestry also fences birth.
  observation_t observe_in_session(const title_t &, const private_session_t &, std::uint64_t not_before_ticks);
  // Ancestry/time identify candidates, not URI causation. The caller must separately establish
  // dispatch/exclusivity authority; neither a different title nor an arbitrary session is adoptable.
  std::unique_ptr<adopted_game_t> adopt_in_session(
    const title_t &, const private_session_t &, std::uint64_t not_before_ticks, const observation_t &, std::string &error);

  // One step of a private session's lazy claim on its requested game, from a fresh in-session
  // observation. The request window bounds the game's birth, never when status or End looks: an
  // exact game born inside it is retained however late it is first seen, and one born after it
  // never is. A game not seen yet keeps the claim open; any other unavailable answer retires it.
  enum class private_claim_t { retain, keep_looking, retire };
  private_claim_t private_claim(const observation_t &in_session, std::uint64_t request_ticks, std::uint64_t birth_upper_ticks);

  // What private status says about the requested game. Until the window ends, a game not seen yet
  // is awaited. After it, or with no clock to tell, the game is unverified: it may have opened on
  // the desktop. The claim itself stays open, so a later look may still retain an early-born game.
  enum class private_game_t { managed, waiting, unverified };
  private_game_t private_game_state(bool retained, bool claim_retired, const observation_t &latest,
                                    std::optional<std::uint64_t> now_ticks, std::uint64_t birth_upper_ticks);

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

    struct link_result_t {
      std::optional<std::filesystem::path> target;
      bool permission_denied {false};
    };

    enum class process_absence_t { unproven, vanished };

    struct status_result_t {
      std::optional<std::string> text;
      process_absence_t absence {process_absence_t::unproven};
    };

    /** What this account's systemd user manager reports for one of its loaded units. */
    struct unit_t {
      std::string id, load_state, fragment_path, control_group;
      std::vector<std::string> drop_in_paths;
      bool transient {true};
      bool operator==(const unit_t &) const = default;
    };

    struct backend_t {
      virtual ~backend_t() = default;
      virtual std::uint64_t owner_uid() const = 0;
      virtual std::optional<std::string> read(const std::filesystem::path &, std::size_t limit) = 0;
      // Only initial census status may prove absence; ordinary read failures never do.
      virtual status_result_t initial_status(const std::filesystem::path &process, std::size_t limit) { return {read(process / "status", limit)}; }
      // A /proc/<pid>/exe path reads the executable that task runs, wherever its own path points.
      virtual std::optional<std::string> prefix(const std::filesystem::path &, std::size_t length) = 0;
      virtual std::optional<std::filesystem::path> link(const std::filesystem::path &) = 0;
      // Missing/raced links are not permission denials. Fixture backends opt in explicitly.
      virtual link_result_t inspect_link(const std::filesystem::path &path) { return {link(path), false}; }
      virtual std::optional<std::filesystem::path> canonical(const std::filesystem::path &) = 0;
      // A bounded read of the user manager, which assigns the unit; nullopt when it cannot be
      // asked, does not answer in time or has no such unit loaded. Fixture backends opt in.
      virtual std::optional<unit_t> user_unit(const std::string &) { return std::nullopt; }
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
    std::optional<std::size_t> launcher_instances(std::string &reason, std::shared_ptr<backend_t>, const launcher_proof_t *known_launcher = nullptr);
    std::optional<launcher_proof_t> prove_launcher(std::string &reason, std::shared_ptr<backend_t>);
    std::optional<bool> title_runtime_absent(const title_t &, std::string &reason, std::shared_ptr<backend_t>);
    std::optional<private_session_t> pin_private_session(
      std::shared_ptr<const vrr_clock::compositor_lease_t>, const std::string &generation, std::string &reason, std::shared_ptr<backend_t>);
    // The actual bounded reader, also usable with inert fixture files (including size-zero procfs).
    std::optional<std::string> read_bounded(const std::filesystem::path &, std::size_t limit);
  }
}  // namespace platf::epic
