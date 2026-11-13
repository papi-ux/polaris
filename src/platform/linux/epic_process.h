/**
 * @file src/platform/linux/epic_process.h
 * @brief Observe an Epic title and retain one explicitly adopted process.
 */
#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "../../epic_library.h"

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
    // Packaging is explicit: sandbox games never enter native observation/adoption.
    std::optional<epic_library::flatpak_deployment_t> flatpak;
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

  // Independent Mirror-only prerequisite, never native proof, adoption or signal authority.
  class flatpak_desktop_proof_t {
  public:
    flatpak_desktop_proof_t(const flatpak_desktop_proof_t &) = default;
  private:
    struct state_t;
    explicit flatpak_desktop_proof_t(std::shared_ptr<const state_t>);
    std::shared_ptr<const state_t> state_;
    friend struct detail::engine_t;
  };
  std::optional<flatpak_desktop_proof_t> prove_flatpak_desktop(const epic_library::flatpak_deployment_t &, std::string &reason);
  bool verify_flatpak_desktop(const flatpak_desktop_proof_t &, std::string &reason);

  /** A previously proven original executable, never a guessed path or mounted inner ELF. */
  class launcher_proof_t {
  public:
    std::filesystem::path path() const;
    // The original is a type-2 AppImage, whose runtime FUSE-mounts its own payload.
    bool appimage() const;

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
  // session (D-A (B), see session_role); any other protected task leaves this unknown. It answers
  // only from a listing in which every owner task has been judged, so another user's process or an
  // unrelated owner task coming and going does not make it unknown. A candidate changing does.
  // Optional presentation only: a bounded basename of one confirmed blocker, never admission or
  // process-control authority. Every other owner task must be read as no AppImage, or be a protected
  // task placed by the same roles; it is empty for incomplete, unsafe, changed or ambiguous observations.
  // Like the census, it judges by delta, so unrelated tasks coming and going do not empty it.
  // The caller must restrict it to the authenticated host console; reason stays non-identifying.
  // When the census is launcher_census_unresolved and the label is empty, owner_blocker_reason gets
  // why: a fixed code for the journal, never the label, a path or a command line.
  std::optional<std::size_t> launcher_instances(std::string &reason, const launcher_proof_t *known_launcher = nullptr, std::string *owner_blocker_label = nullptr,
                                                std::string *owner_blocker_reason = nullptr);
  std::optional<launcher_proof_t> prove_launcher(std::string &reason);
  // Revalidates the retained original file without requiring a launcher or AppImage mount.
  bool verify_launcher(const launcher_proof_t &, std::string &reason);
  // File identity and inspection context, without I/O. Neither comparison grants authority.
  bool same_launcher_file(const launcher_proof_t &, const launcher_proof_t &);
  // The same desktop task, boot, namespace, cgroup, mount and original file at two samples.
  // Matching bookends do not establish continuous presence between samples.
  bool same_launcher_observation(const launcher_proof_t &, const launcher_proof_t &);

  enum class capture_origin_t { provider_choice, launch_discovery, scan };
  enum class capture_state_t { captured, unchanged, unavailable, superseded, unsupported };
  struct capture_result_t {
    capture_state_t state {capture_state_t::unavailable};
    std::uint64_t generation {0};
    std::string reason;
  };
  enum class scan_state_t { required, checking, complete, metadata_unavailable, launcher_unobserved, launcher_changed, superseded, session_busy, unsupported };
  enum class receipt_state_t { required, complete, previous, unknown };

  /** In-memory setup history only. Caller serializes access; methods do no native I/O.
   * This owns no launch/adoption permission. Replacing this object (host restart) drops history.
   * The web never supplies a ticket, launcher proof, metadata snapshot or completed flag.
   */
  class setup_history_t {
  public:
    class scan_ticket_t {
    private:
      std::uint64_t sequence {0}, generation {0};
      std::shared_ptr<const char> epoch;
      std::optional<launcher_proof_t> before;
      friend class setup_history_t;
    };
    capture_result_t capture(const launcher_proof_t &, capture_origin_t);
    scan_ticket_t begin_scan();
    // Rejects a slow capture if another scan or a different file generation won publication.
    capture_result_t observe_scan(scan_ticket_t &, const launcher_proof_t &);
    scan_state_t finish_scan(const scan_ticket_t &, const epic_library::snapshot_t &,
                            const launcher_proof_t *after, bool idle_same_session);
    receipt_state_t receipt(const epic_library::snapshot_t &, bool proof_valid) const;
    const std::optional<launcher_proof_t> &proof() const { return proof_; }
    std::uint64_t generation() const { return generation_; }
    std::uint64_t revision() const { return revision_; }
    std::uint64_t receipt_revision() const { return receipt_revision_; }
    scan_state_t last_scan() const { return last_scan_; }
    capture_origin_t origin() const { return origin_; }
    void invalidate_receipt();
#ifdef POLARIS_TESTS
    void set_metadata_observer_for_tests(std::function<void()> observer) { metadata_observer_ = std::move(observer); }
#endif

  private:
#ifdef POLARIS_TESTS
    std::function<void()> metadata_observer_;
#endif
    bool current(const scan_ticket_t &) const;
    std::optional<launcher_proof_t> proof_;
    std::shared_ptr<const epic_library::snapshot_t> snapshot_;
    std::uint64_t generation_ {0}, sequence_ {0}, revision_ {0}, receipt_revision_ {0};
    std::shared_ptr<const char> epoch_ {std::make_shared<const char>()};
    scan_state_t last_scan_ {scan_state_t::required};
    capture_origin_t origin_ {capture_origin_t::provider_choice};
  };

  enum class desktop_state_t { not_checked, open, closed, unknown };
  enum class proof_state_t { required, valid, changed, unavailable };
  enum class runtime_state_t { not_checked, not_selected, absent, busy, unknown };
  enum class prerequisite_state_t { available, unavailable, unknown };
  enum class readiness_state_t { busy, needs_action, launcher_closed, ready, unknown };
  struct setup_readiness_t {
    desktop_state_t desktop {desktop_state_t::not_checked};
    proof_state_t proof {proof_state_t::required};
    runtime_state_t runtime {runtime_state_t::not_checked};
    readiness_state_t state {readiness_state_t::unknown};
    std::string reason;
  };
  // Passive sampled facts only: no URI, pidfd/adoption, compositor, logs or launch_failure writes.
  // A missing scan receipt is deliberately NOT a launch prerequisite.
  setup_readiness_t evaluate_setup(const launcher_proof_t *, const title_t *, bool host_idle,
                                   prerequisite_state_t = prerequisite_state_t::unknown);
  // Whether a retained AppImage's runtime knows --appimage-mount. One that does not would treat the
  // flag as the launcher's own argument and start it outside any sandbox, without the request.
  bool appimage_mounts_itself(const launcher_proof_t &);

  // A sampled admission check, never dispatch authority: true means the requested game
  // and its exact prefix server are absent, false means busy, nullopt means unknown.
  // Recheck immediately before dispatch; concurrent launches remain possible afterwards.
  // Protected tasks are set aside by the same role rule as launcher_instances, and it answers from
  // the same kind of listing. A missing or unreadable prefix reports "title_prefix_unavailable"; a
  // first play in Mirror Desktop creates one.
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
    std::optional<std::size_t> launcher_instances(std::string &reason, std::shared_ptr<backend_t>, const launcher_proof_t *known_launcher = nullptr, std::string *owner_blocker_label = nullptr,
                                                  std::string *owner_blocker_reason = nullptr);
    std::optional<launcher_proof_t> prove_launcher(std::string &reason, std::shared_ptr<backend_t>);
    std::optional<flatpak_desktop_proof_t> prove_flatpak_desktop(const epic_library::flatpak_deployment_t &, std::string &reason, std::shared_ptr<backend_t>);
    setup_readiness_t evaluate_setup(const launcher_proof_t *, const title_t *, bool host_idle,
                                     prerequisite_state_t, std::shared_ptr<backend_t>);
    std::optional<bool> title_runtime_absent(const title_t &, std::string &reason, std::shared_ptr<backend_t>);
    std::optional<private_session_t> pin_private_session(
      std::shared_ptr<const vrr_clock::compositor_lease_t>, const std::string &generation, std::string &reason, std::shared_ptr<backend_t>);
    // The actual bounded reader, also usable with inert fixture files (including size-zero procfs).
    std::optional<std::string> read_bounded(const std::filesystem::path &, std::size_t limit);
    // The production backend's initial status of a real /proc entry.
    status_result_t real_initial_status(const std::filesystem::path &process, std::size_t limit);
    // Whether a failed status read can begin the proof that a task has gone.
    bool status_failure_may_be_absence(int error);
  }
}  // namespace platf::epic
