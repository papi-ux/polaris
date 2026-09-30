/**
 * @file src/platform/linux/stream_display_policy.h
 * @brief Resolve/apply facade over stream_path (path ids + descriptors are SoT).
 *
 * stream_path owns path vocabulary and resolved_t; this layer maps config →
 * resolved session flags and applies user selections into legacy bools.
 */
#pragma once

#include "stream_path.h"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace virtual_display {
  enum class backend_e;
}

namespace stream_display_policy {

  struct input_t {
    bool virtual_display_available = false;
    bool active_encoder_requires_gpu_native_capture = false;
    bool runtime_gpu_native_override_active = false;
  };

  /// Sole resolved session description (SoT lives in stream_path).
  using resolved_t = stream_path::resolved_t;

  struct mode_option_t {
    std::string value;
    std::string label;
    std::string badge;
    std::string reason;
    bool available = true;
    std::string unavailable_reason;
    std::string group;  // private | host | advanced | experimental
    std::string runtime;  // labwc | gamescope | ""
    std::string capture;  // auto | portal | wlroots | kms
    std::string topology;  // leave_alone | host_virtual | swap_primary
  };

  struct legacy_booleans_t {
    bool headless_mode = false;
    bool use_cage_compositor = false;
    bool prefer_gpu_native_capture = false;
  };

  /** @brief Resolve an exact per-app pin without probing or changing host state. */
  struct app_launch_as_t {
    enum class verdict_e { follow, pinned, not_a_launch_mode, conflict } verdict = verdict_e::follow;
    std::string selection;
  };

  app_launch_as_t resolve_app_launch_as(std::string_view launch_as, std::string_view client_named_selection);

  struct host_default_launch_input_t {
    std::string_view requested_selection;
    bool mirror_desktop = false;
    bool launch_virtual_display = false;
    bool virtual_display_user_locked = false;
    bool virtual_display_optimization_present = false;
    bool host_provides_private_display = false;
  };

  /**
   * @brief Resolve client choices for a Host default app, with no app preference.
   * @details The typed input and new name make stale positional calls fail to
   *          compile. Fixed app modes bypass this resolver entirely.
   */
  std::string host_default_launch_selection(const host_default_launch_input_t &input);

  /**
   * @brief Whether the host's own configuration already provides the display.
   *
   * True for a headless labwc host: the private runtime creates the stream
   * output itself. Reads the host default rather than the live config, so a
   * session parked on a virtual display does not make the host stop being a
   * private host.
   */
  bool host_default_provides_private_display();

  /**
   * @brief Keep the created virtual connector name when display mapping cannot
   *        produce a backend-specific identifier.
   */
  std::string capture_output_name_for_virtual_display(
    std::string_view created_output_name,
    std::string_view mapped_output_name
  );

  /**
   * @brief Resolve the capture backend required by a host virtual display.
   *
   * Native wlroots headless outputs are capturable directly by output name.
   * EVDI and KScreen outputs are reachable only through the portal/KWin path,
   * so an explicitly configured backend does not apply to them and is
   * replaced. This discards an operator's choice,
   * which is why normalize_host_virtual_display_state_for_backend says so in
   * the log. A session-scoped override restores the host setting at teardown.
   * A mode loaded from polaris.conf keeps the replacement in memory until
   * Polaris restarts: a later mode switch puts back the value it found, which
   * is already the replacement.
   */
  std::string capture_for_host_virtual_display_backend(
    virtual_display::backend_e backend,
    std::string_view current_capture
  );

  /**
   * @brief Resolve a temporary capture backend when one launch changes paths.
   *
   * Private labwc outputs require direct wlroots capture. Conversely, wlroots
   * cannot capture an ordinary host desktop or a Gamescope session. This
   * helper changes only a session-scoped override; the caller remains
   * responsible for restoring the saved host capture setting at teardown.
   * Explicit compatible choices such as Desktop + KMS are preserved.
   */
  std::string capture_for_session_transition(
    std::string_view configured_selection,
    std::string_view session_selection,
    std::string_view current_capture
  );

  /**
   * @brief The capture backend a stream generation asks for.
   *
   * The capture setting is a host preference, and two things override it for a
   * generation. A private labwc session can only be captured through wlroots,
   * so it always asks for wlr. And when the last capture-source evaluation
   * found that the configured backend captures nothing in this mode and
   * substituted another, the generation asks for auto, which lands on that
   * substitute. Asking for the configured backend by name is what failed every
   * stream in #739 while the encoder probe, which always asks for auto,
   * passed. A generation that owns an exact output keeps the configured
   * backend, because auto cannot address an exact output, and so does a
   * Gamescope session, where auto would capture the desktop instead.
   */
  std::string capture_for_mode(
    std::string_view configured_capture,
    std::string_view stream_mode,
    bool use_cage_compositor,
    bool substitution_active,
    bool exact_output_owned
  );

  /**
   * @brief capture_for_mode() for the live configuration and the last
   *        capture-source evaluation.
   */
  std::string capture_for_current_mode(bool exact_output_owned = false);

  /**
   * @brief The capture a stream mode runs with once it has filled an unset one.
   *
   * Gamescope Stream fills an unset capture with portal when a launch enters it, and the dongle
   * fills an unset or auto one, at load and when a launch enters it. Every other mode, and an
   * explicit backend in any mode, keeps what it is given. The fills and the Doctor ask this one
   * rule, so the Doctor never reads an unset capture that no launch would keep.
   */
  std::string capture_filled_for_mode(std::string_view stream_mode, std::string_view capture);

  /**
   * @brief capture_for_current_mode() as a launch into the live mode asks for it: after the mode
   *        has filled an unset capture (capture_filled_for_mode()), which a launch does first.
   */
  std::string capture_for_launch_into_current_mode(bool exact_output_owned = false);

  /**
   * @brief A capture backend the operator named and the host does not ask for, and why.
   *
   * Only an explicit choice counts. An empty capture setting, or a literal
   * auto, asks the host to choose, so the host choosing is not a rewrite.
   */
  struct capture_override_t {
    std::string configured;  ///< the backend the capture setting names; never auto
    std::string effective;  ///< the backend asked for instead; empty is auto
    std::string reason;  ///< one of the k_capture_override_* ids
  };

  /// A private labwc session can only be captured through wlroots.
  constexpr std::string_view k_capture_override_private_compositor = "private_compositor";
  /// The configured backend captured nothing in this mode and the evaluation substituted another.
  constexpr std::string_view k_capture_override_substituted = "substituted";
  /// A Gamescope session is captured through the portal.
  constexpr std::string_view k_capture_override_gamescope_session = "gamescope_session";
  /// A dongle session captures the host desktop through the portal after the topology swap.
  constexpr std::string_view k_capture_override_dongle_session = "dongle_session";
  /// Mirror Desktop lets desktop capture discovery choose instead of pinning wlroots.
  constexpr std::string_view k_capture_override_desktop_discovery = "desktop_discovery";
  /// A host virtual display can only be captured the way its backend exposes it.
  constexpr std::string_view k_capture_override_virtual_display_backend = "virtual_display_backend";

  /**
   * @brief The backend a capture value names, read the way capture dispatch reads it.
   *
   * This is stream_path::canonical_capture_backend(), which says what it reads.
   * Every capture policy reader reaches it through this namespace.
   */
  using stream_path::canonical_capture_backend;

  /**
   * @brief What capture_for_mode() does to an explicitly configured backend, and why.
   *
   * Takes exactly what capture_for_mode() takes and shares its decision, so the
   * two cannot disagree: when this returns a value, effective is what
   * capture_for_mode() returns for the same arguments. Nothing when the
   * configured backend stands or when the capture setting is auto.
   */
  std::optional<capture_override_t> capture_mode_override(
    std::string_view configured_capture,
    std::string_view stream_mode,
    bool use_cage_compositor,
    bool substitution_active,
    bool exact_output_owned
  );

  /**
   * @brief capture_mode_override() for the live configuration and the last
   *        capture-source evaluation, with the inputs capture_for_current_mode() uses.
   *
   * The decision is made on the live setting, so effective is what capture_for_current_mode()
   * returns. What it is measured against, and names as configured, is loaded_capture_setting().
   * After a Host Virtual Display load or a session transition the live setting is already the
   * replacement: named as configured, it told a host set to kms that it had chosen portal, and
   * warned a host with capture unset that an explicit portal had been set aside.
   */
  std::optional<capture_override_t> capture_mode_override_for_current_mode(bool exact_output_owned = false);

  /**
   * @brief The k_capture_override_* id of the rule that asked a generation for another backend
   *        than polaris.conf names. Empty when no rule did.
   *
   * preference is polaris.conf's capture as loaded (loaded_capture_setting()), and requested is what
   * the generation asked dispatch for. Both are read the way dispatch reads them, so an alias is no
   * rewrite, and a preference that asks the host to choose is never set aside. The mode rule is
   * asked first, with the generation's own mode, compositor and exact output, because it is the
   * last step between the live setting and a request, and it answers only when it asked for the
   * backend requested names. Otherwise the newest rule that wrote that backend answers: a launch
   * entering another mode, kept even when an older rule had already written the same backend, a
   * Host Virtual Display load or launch, or Steam Game Mode. A difference no recorded rule explains
   * stays empty rather than being given a guessed reason.
   */
  std::string capture_request_override_reason(
    std::string_view preference,
    std::string_view requested,
    std::string_view stream_mode,
    bool use_cage_compositor,
    bool exact_output_owned
  );

  /**
   * @brief What capture_for_session_transition() does to an explicitly configured
   *        backend, and why. Shares its decision, like capture_mode_override().
   */
  std::optional<capture_override_t> capture_session_transition_override(
    std::string_view configured_selection,
    std::string_view session_selection,
    std::string_view current_capture
  );

  /**
   * @brief Rewrite the live capture setting for a launch entering session_selection, and say so.
   *
   * The rewrite is capture_for_session_transition() on the live setting. When it sets aside a
   * backend polaris.conf names, measured against loaded_capture_setting() for the reason
   * capture_mode_override_for_current_mode() is, the line is a warning naming that backend. Any
   * other change, such as a replacement a Host Virtual Display load made or an unset capture
   * filled in, is an info line. The caller puts the host setting back at teardown.
   */
  void apply_capture_for_session_transition(
    std::string_view configured_selection,
    std::string_view session_selection
  );

  /**
   * @brief What capture_for_host_virtual_display_backend() does to an explicitly
   *        configured backend, and why. Shares its decision, like capture_mode_override().
   */
  std::optional<capture_override_t> capture_host_virtual_display_override(
    virtual_display::backend_e backend,
    std::string_view current_capture
  );

  /**
   * @brief One log line naming the stream mode, the configured and the effective
   *        backend, and the reason. Callers append how long the rewrite lasts.
   */
  std::string describe_capture_override(
    const capture_override_t &override,
    std::string_view stream_mode
  );

  /**
   * @brief The capture setting as the last configuration load parsed it, before
   *        any stream mode rewrote it in memory.
   *
   * Nothing puts it in JSON. The line logged when a display opens names it as
   * the configured backend, and a virtual display that comes up on another
   * backend than the launch checked is measured against it. The live setting
   * cannot answer either question: every writer of it after the load is an
   * in-memory rewrite. Empty before any load and when the setting is auto.
   */
  std::string loaded_capture_setting();

  /**
   * @brief Who rewrites the capture setting for a virtual display, which decides
   *        how long the rewrite lasts and what the host says about it.
   */
  enum class capture_rewrite_scope_e {
    load,  ///< polaris.conf was just parsed, before logging starts; lasts until Polaris restarts
    session,  ///< a launch entering the mode; teardown puts the host setting back
    preview,  ///< the caller puts the capture setting back itself, so nothing is said
    backend_change,  ///< the display came up on another backend than the launch checked
  };

  /**
   * @brief Log what the last configuration load did to the capture setting.
   *
   * The load runs inside config::parse, before logging::init, and a line logged
   * there reaches stdout but never polaris.log or the console's log viewer. So
   * the load keeps its lines, and main() calls this once logging is up. Each
   * line is said once, and a new load drops the lines an older one kept.
   */
  void log_config_load_notes();

  /**
   * @brief Normalize connector and capture authority for the backend that will
   *        or did create the Host Virtual display.
   *
   * The caller must pass the authoritative backend. In particular, the launch
   * actuator calls this again with vdisplay_t::backend after creation so a
   * backend change after preflight cannot carry KScreen connector authority
   * into an EVDI or wlroots session.
   *
   * @param scope Who is asking. It decides what the line about a rewritten
   *        capture setting says, how long it says the rewrite lasts, and whether
   *        there is one. The rewrite is measured against polaris.conf as loaded
   *        (loaded_capture_setting()) whoever asks. The actuator passes
   *        backend_change.
   * @param stream_mode The mode being entered, for the log line that names a
   *        replaced capture backend. Empty reads the live stream mode, which
   *        apply_selection() has not set yet when it calls this.
   */
  void normalize_host_virtual_display_state_for_backend(
    virtual_display::backend_e backend,
    capture_rewrite_scope_e scope = capture_rewrite_scope_e::session,
    std::string_view stream_mode = {}
  );

  /**
   * @brief Whether Host Virtual Display creates a new output connector.
   *
   * KScreen manages an existing connector. EVDI and wlroots create their own
   * output, so carrying a dongle connector into those backends would leak game
   * placement and capture authority from the previous topology.
   */
  bool host_virtual_backend_creates_output(
    virtual_display::backend_e backend
  );

  /**
   * @brief Whether retained connector authority is valid for a Host Virtual backend.
   *
   * KScreen manages an existing connector. EVDI and wlroots create a new one,
   * so their final launch fast path must force normalization while any prior
   * connector or capture-output pin remains.
   */
  bool host_virtual_connector_state_matches(
    virtual_display::backend_e backend,
    std::string_view streaming_output,
    std::string_view output_name
  );

  /** Stable selection ids — SoT is stream_path (no dual string tables). */
  constexpr std::string_view k_headless_stream = stream_path::k_headless_stream;
  constexpr std::string_view k_windowed_stream = stream_path::k_windowed_stream;
  constexpr std::string_view k_host_virtual_display = stream_path::k_host_virtual_display;
  constexpr std::string_view k_desktop_takeover = stream_path::k_desktop_takeover;
  constexpr std::string_view k_desktop_display = stream_path::k_desktop_display;
  constexpr std::string_view k_gamescope_stream = stream_path::k_gamescope_stream;
  constexpr std::string_view k_headless_dongle = stream_path::k_headless_dongle;

  constexpr std::string_view k_runtime_labwc = stream_path::k_runtime_labwc;
  constexpr std::string_view k_runtime_gamescope = stream_path::k_runtime_gamescope;

  /**
   * @brief Human label for a selection id.
   */
  std::string label_for_selection(std::string_view selection);

  /**
   * @brief Reason copy for a selection id (may depend on virtual display availability).
   */
  std::string reason_for_selection(std::string_view selection, bool virtual_display_available = false);

  /**
   * @brief Whether a mode is available on this host build.
   */
  bool selection_available(std::string_view selection);

  /**
   * @brief Whether a client may drive this selection for one session.
   *
   * A path that swaps host output topology changes the machine's physical display
   * arrangement, so it stays host-default-only rather than something a single
   * client can turn on for one launch. Derived from the path's topology rather
   * than an id list, so a future swapping path inherits the rule.
   */
  bool selection_session_overridable(std::string_view selection);

  /**
   * @brief Whether a client may pick, for one launch, a stream mode that runs its own compositor.
   *
   * Such a mode is captured from that compositor and never from a KMS scanout, so what the host
   * desktop scans out does not reach it. Answered from the binaries on PATH, as the capability
   * listing answers it for these modes, without the virtual display probes other modes need.
   */
  bool private_runtime_selection_available();

  /**
   * @brief Whether an app that mirrors the desktop should step aside for this selection.
   *
   * An entry with desktop-mirror semantics exists so that "Desktop" shows the real desktop rather
   * than an empty private compositor, which is what a Private Stream, a GPU-native stream or a
   * gamescope session with nothing launched into it would be. Two selections are not that: a host
   * virtual display extends the live session onto a new screen, and a desktop takeover moves the
   * live session onto one. Both still show the desktop, so both are worth honouring.
   *
   * Pure, and deliberately without probes or config reads: the same answer has to be reachable from
   * the HTTP policy and from the launch resolver, and those two disagreeing is how a launch comes to
   * promise one topology and deliver another.
   */
  bool desktop_mirror_yields_to_selection(std::string_view selection);

  std::string selection_unavailable_reason(std::string_view selection);

  /**
   * @brief Whether a selection would pass apply_selection's id and availability checks.
   *
   * The single validity truth for network-facing validators: apply_selection
   * calls this itself, so a validator delegating here can never accept a mode
   * apply would then reject - the drift that used to 400 modes the host had
   * just advertised in allowed_modes.
   *
   * @param error On failure, the reason to serve (the host's real unavailable
   *              reason for registered-but-unavailable modes).
   */
  bool selection_valid(std::string_view selection, std::string &error);

  /**
   * @brief Validate against a newly probed host capability snapshot.
   *
   * Use this at exact optimize/parse/final-launch boundaries. Status and UI
   * catalogs may use selection_valid(), which accepts the short probe cache.
   */
  bool selection_valid_fresh(std::string_view selection, std::string &error);

  /**
   * @brief Capability-injected form used to keep served availability and the
   *        launch validator on one deterministic rule.
   */
  bool selection_valid_for_capabilities(
    std::string_view selection,
    bool virtual_display_available,
    std::string &error
  );

  /**
   * @brief Derive the configured selection from legacy booleans when
   *        linux_stream_mode is unset.
   */
  std::string selection_from_legacy_booleans(const legacy_booleans_t &booleans);

  /**
   * @brief Return the configured selection after legacy-boolean normalization.
   *
   * This reads the live config, which a session-scoped override rewrites in
   * place. Use host_default_selection() for anything that advises a client.
   */
  std::string configured_selection();

  /**
   * @brief Remember the host's own display policy while an override stands.
   *
   * A session-scoped override rewrites stream_mode, the legacy booleans and the
   * capture backend in place, and a paused session keeps them rewritten for as
   * long as it remains resumable. Surfaces that tell a client what to ask for
   * have to answer for the host rather than for the session parked on it, or
   * one client's topology silently becomes the next client's recommendation.
   */
  void remember_host_default(
    const legacy_booleans_t &booleans,
    std::string_view stream_mode,
    std::string_view capture
  );

  /** @brief Forget the remembered host policy once the override is restored. */
  void forget_host_default();

  /**
   * @brief The host's own configured selection, ignoring any live override.
   *
   * Falls back to configured_selection() when no override is standing, so this
   * is always safe to call.
   */
  std::string host_default_selection();

  /// What reconcile_game_mode() did on this call.
  enum class game_mode_reconcile_e {
    unchanged,
    entered,  ///< a Game Mode session is live: the host now mirrors that screen
    left,  ///< the session ended: the configured mode is back
  };

  /**
   * @brief Keep the host's mode honest while it is in Steam Game Mode.
   *
   * A host in Game Mode has one screen and no desktop to build a private display beside, so the
   * only mode it can run is a mirror of that screen. Left configured for Private Stream it fails
   * before any launch: every serverinfo poll starts a private compositor to probe encoders, and
   * a launch is refused because the Steam that is running is the session itself. So while a
   * session is live the mode is Mirror Desktop, in memory only. polaris.conf is never written,
   * and what was configured comes back when the session ends, which is what switching to
   * Desktop Mode looks like to a Polaris that was started at boot.
   *
   * Nothing moves while a stream is up. A config reload or a mode chosen on purpose drops the
   * hold (forget_game_mode_hold), and the next call holds the new values if they still need it.
   */
  game_mode_reconcile_e reconcile_game_mode(bool session_live, bool stream_active);

  /// The mode that is waiting for the Game Mode session to end; empty when none is held.
  std::string game_mode_held_selection();

  /**
   * @brief Drop the held mode, because the host's mode was just set on purpose.
   *
   * A mode chosen while Game Mode is running is newer than the held one. Kept, the hold would put
   * the old mode back when the session ends: the file would say one mode and the process run another.
   */
  void forget_game_mode_hold();

  /**
   * @brief Whether a mode creates/owns the stream output refresh rate.
   */
  bool selection_owns_launch_refresh_rate(std::string_view selection);

  /**
   * @brief Map a selection id to the legacy boolean triple used by older clients.
   */
  legacy_booleans_t legacy_booleans_for_selection(std::string_view selection);

  /**
   * @brief Resolve the configured (desired) mode from config + optional inputs.
   */
  resolved_t resolve(const input_t &input = {});

  /**
   * @brief Convenience: resolve using virtual_display::is_available() and flags.
   */
  resolved_t resolve_current(bool active_encoder_requires_gpu_native_capture = false,
                             bool runtime_gpu_native_override_active = false);

  /**
   * @brief Resolve the host's own configured mode, ignoring a live override.
   */
  resolved_t resolve_host_default(const input_t &input = {});

  /**
   * @brief Resolve the effective mode while a session is live.
   */
  resolved_t resolve_effective(const input_t &input,
                               bool streaming,
                               bool session_uses_virtual_display,
                               bool runtime_effective_headless);

  /**
   * @brief Whether a selection ID and its deterministic companion state agree.
   */
  bool selection_companion_state_matches(std::string_view selection);

  /**
   * @brief Apply a user/API selection into config (stream_mode + legacy bools + runtime).
   *
   * Gamescope Stream and the dongle fill an unset capture with portal, and say so
   * at info with the line the dongle load fill says, unless scope is preview.
   *
   * @param scope How long a capture rewrite made here lasts. A caller that puts
   *        the capture setting back itself passes preview.
   * @return false and sets error on failure.
   */
  bool apply_selection(
    std::string_view selection,
    std::string &error,
    capture_rewrite_scope_e scope = capture_rewrite_scope_e::session
  );

  /**
   * @brief Normalize config after load: if stream_mode set, sync booleans; else
   *        leave booleans authoritative and fill stream_mode in memory.
   */
  void normalize_config_from_load();

  /**
   * @brief Options list for client-settings / UI (includes unavailable gamescope).
   */
  std::vector<mode_option_t> mode_options(bool virtual_display_available = false);

  /**
   * @brief Allowed launch-mode selection ids for Nova (excludes unavailable modes).
   */
  std::vector<std::string> allowed_launch_modes(bool virtual_display_available,
                                                bool include_unavailable = false);

}  // namespace stream_display_policy
