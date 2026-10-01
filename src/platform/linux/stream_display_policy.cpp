/**
 * @file src/platform/linux/stream_display_policy.cpp
 * @brief Linux stream display policy — facade over stream_path registry.
 */

#include "stream_display_policy.h"

#include "display_topology.h"
#include "desktop_takeover.h"
#include "stream_path.h"
#include "src/config.h"
#include "src/logging.h"
#include "src/platform/common.h"
#include "virtual_display.h"

#include <cctype>
#include <mutex>
#include <optional>

namespace stream_display_policy {

  namespace {

    using stream_path::to_lower_copy;

    /**
     * The host's own display policy, held while a session-scoped override has
     * rewritten the live config. A paused session keeps that rewrite standing
     * for as long as it stays resumable, so anything that advises a client what
     * to ask for reads this instead of the live values.
     */
    struct host_default_t {
      legacy_booleans_t booleans;
      std::string stream_mode;
      std::string capture;
    };

    std::mutex host_default_mutex;
    std::optional<host_default_t> held_host_default;

    std::optional<host_default_t> load_host_default() {
      const std::lock_guard<std::mutex> guard {host_default_mutex};
      return held_host_default;
    }

    /** One held snapshot resolves to exactly one selection, lock-free. */
    std::string selection_for_host_default(const host_default_t &held) {
      if (!held.stream_mode.empty() && stream_path::find(held.stream_mode)) {
        return stream_path::to_lower_copy(held.stream_mode);
      }
      return selection_from_legacy_booleans(held.booleans);
    }

    legacy_booleans_t live_booleans() {
      const auto &linux_display = config::video.linux_display;
      return legacy_booleans_t {
        linux_display.headless_mode,
        linux_display.use_cage_compositor,
        linux_display.prefer_gpu_native_capture,
      };
    }

    /** Resolve one explicit policy state rather than whatever config holds now. */
    resolved_t resolve_for_state(
      const input_t &input,
      std::string_view selection,
      std::string_view stream_mode,
      const legacy_booleans_t &booleans,
      std::string_view configured_capture
    ) {
      const auto *path = stream_path::find(selection);
      stream_path::descriptor_t desc {};
      if (path) {
        desc = *path;
      }
      else {
        desc = *stream_path::find(stream_path::k_desktop_display);
      }

      // Edge: legacy !headless + cage without stream_mode -> windowed private labwc.
      if (stream_mode.empty() &&
          !booleans.headless_mode &&
          booleans.use_cage_compositor) {
        if (const auto *windowed = stream_path::find(stream_path::k_windowed_stream)) {
          desc = *windowed;
          desc.request_headless = false;
        }
      }

      auto caps = stream_path::probe_host_capabilities();
      caps.configured_capture = std::string {configured_capture};
      if (input.virtual_display_available) {
        caps.virtual_display_available = true;
      }

      return stream_path::resolve_path(
        desc,
        caps,
        input.active_encoder_requires_gpu_native_capture,
        input.runtime_gpu_native_override_active
      );
    }

    /// A capture value, and the id of the rule that produced it when a rule replaced the one given.
    struct capture_decision_t {
      std::string capture;
      std::string_view reason;
    };

    /**
     * An override is an explicit choice that a rule replaced with a different backend. Auto asked
     * the host to choose, and an alias replaced by the backend it names is no change at all.
     */
    std::optional<capture_override_t> override_for(
      std::string_view configured,
      const capture_decision_t &decision
    ) {
      const auto configured_backend = canonical_capture_backend(configured);
      if (decision.reason.empty() ||
          configured_backend.empty() ||
          canonical_capture_backend(decision.capture) == configured_backend) {
        return std::nullopt;
      }
      return capture_override_t {
        std::string {configured},
        decision.capture,
        std::string {decision.reason},
      };
    }

    capture_decision_t decide_capture_for_host_virtual_display_backend(
      virtual_display::backend_e backend,
      std::string_view current_capture
    ) {
      if (backend == virtual_display::backend_e::WAYLAND_WLR) {
        return {"wlr", k_capture_override_virtual_display_backend};
      }

      // All three are ordinary KWin monitors to capture: kwingrab streams them by
      // output name through portal_grab.
      if (backend == virtual_display::backend_e::EVDI ||
          backend == virtual_display::backend_e::KSCREEN_DOCTOR ||
          backend == virtual_display::backend_e::KWIN_VIRTUAL_OUTPUT) {
        return {"portal", k_capture_override_virtual_display_backend};
      }

      return {std::string {current_capture}, {}};
    }

    capture_decision_t decide_capture_for_session_transition(
      std::string_view configured_selection,
      std::string_view session_selection,
      std::string_view current_capture
    ) {
      const auto configured = to_lower_copy(configured_selection);
      const auto session = to_lower_copy(session_selection);
      if (configured == session) {
        return {std::string {current_capture}, {}};
      }

      if (session == k_headless_stream || session == k_windowed_stream) {
        return {"wlr", k_capture_override_private_compositor};
      }

      if (session == k_gamescope_stream) {
        return {"portal", k_capture_override_gamescope_session};
      }
      if (session == k_headless_dongle) {
        return {"portal", k_capture_override_dongle_session};
      }

      const auto capture = to_lower_copy(current_capture);
      if (session == k_desktop_display &&
          (capture == "wlr" || capture == "wlroots" || capture == "auto")) {
        // Let desktop capture discovery prefer a working KMS path when the
        // process retained CAP_SYS_ADMIN, then fall through to Portal on an
        // ordinary unprivileged installation. Pinning WLR here leaves neither.
        return {{}, k_capture_override_desktop_discovery};
      }

      return {std::string {current_capture}, {}};
    }

    capture_decision_t decide_capture_for_mode(
      std::string_view configured_capture,
      std::string_view stream_mode,
      bool use_cage_compositor,
      bool substitution_active,
      bool exact_output_owned
    ) {
      if (use_cage_compositor) {
        return {"wlr", k_capture_override_private_compositor};
      }
      // A Gamescope session is not the host desktop, and auto would land on a backend that
      // captures the desktop instead of it; keep the configured one and let it fail visibly.
      if (substitution_active && !exact_output_owned &&
          to_lower_copy(stream_mode) != k_gamescope_stream) {
        return {{}, k_capture_override_substituted};
      }
      return {std::string {configured_capture}, {}};
    }

    /// The capture setting as the last load parsed it; see loaded_capture_setting().
    std::mutex loaded_capture_mutex;
    std::string loaded_capture;

    /// A line the last load had to say before logging started; see log_config_load_notes().
    struct load_note_t {
      bool warning;
      std::string line;
    };
    std::vector<load_note_t> load_notes;  ///< guarded by loaded_capture_mutex

    void keep_load_note(bool warning, std::string line) {
      const std::lock_guard<std::mutex> guard {loaded_capture_mutex};
      load_notes.push_back({warning, std::move(line)});
    }

    /// Who rewrote the live capture setting, which decides how long the record of why lasts.
    enum class rewrite_source_e {
      load,  ///< a Host Virtual Display load; lasts until the next load
      session,  ///< a launch; lasts until the host setting is back at teardown
      game_mode,  ///< Steam Game Mode; lasts until the hold is given back
    };

    /**
     * Why the live capture setting holds a value a rule wrote there. The live setting keeps only
     * the value, and after a rewrite it no longer says what polaris.conf chose or which rule chose
     * otherwise, so a session asking why its request differs finds the answer here.
     */
    struct capture_rewrite_t {
      rewrite_source_e source;
      std::string capture;  ///< canonical_capture_backend() of what the rule wrote
      std::string reason;  ///< a k_capture_override_* id
    };

    std::mutex capture_rewrites_mutex;
    std::vector<capture_rewrite_t> capture_rewrites;  ///< oldest first, one per source

    void note_capture_rewrite(rewrite_source_e source, std::string_view capture, std::string_view reason) {
      if (reason.empty()) {
        return;
      }
      const std::lock_guard<std::mutex> guard {capture_rewrites_mutex};
      std::erase_if(capture_rewrites, [source](const capture_rewrite_t &rewrite) {
        return rewrite.source == source;
      });
      capture_rewrites.push_back({source, canonical_capture_backend(capture), std::string {reason}});
    }

    /// Forget the rewrites one source made, or every rewrite when none is named.
    void forget_capture_rewrites(std::optional<rewrite_source_e> source) {
      const std::lock_guard<std::mutex> guard {capture_rewrites_mutex};
      std::erase_if(capture_rewrites, [source](const capture_rewrite_t &rewrite) {
        return !source || rewrite.source == *source;
      });
    }

    /// The reason of the newest rewrite that wrote the backend capture names, or empty.
    std::string capture_rewrite_reason_for(std::string_view capture) {
      const auto backend = canonical_capture_backend(capture);
      const std::lock_guard<std::mutex> guard {capture_rewrites_mutex};
      for (auto rewrite = capture_rewrites.rbegin(); rewrite != capture_rewrites.rend(); ++rewrite) {
        if (rewrite->capture == backend) {
          return rewrite->reason;
        }
      }
      return {};
    }

    std::string describe_capture(std::string_view capture) {
      return capture.empty() ? std::string {"auto"} : std::string {capture};
    }

    /**
     * What a mode says when it fills an unset capture with portal. The load and a launch that
     * enters the mode fill it the same way, so they say the same line.
     */
    std::string portal_fill_line(std::string_view mode) {
      if (mode == k_gamescope_stream) {
        return "stream_display_policy: stream mode [gamescope_stream] captures the Gamescope session "
               "through [portal], with capture set to [auto]";
      }
      return "stream_display_policy: stream mode [headless_dongle] captures the host desktop "
             "through [portal] after the topology swap, with capture set to [auto]";
    }

    /// Say a portal fill at info. A load waits for logging, and a preview is put back by its caller.
    void say_portal_fill(std::string_view mode, capture_rewrite_scope_e scope) {
      if (scope == capture_rewrite_scope_e::preview) {
        return;
      }
      if (scope == capture_rewrite_scope_e::load) {
        keep_load_note(false, portal_fill_line(mode));
        return;
      }
      BOOST_LOG(info) << portal_fill_line(mode);
    }

    void clear_connector_output_authority(bool retire_connectors) {
      auto &linux_display = config::video.linux_display;
      linux_display.auto_manage_displays = false;
      linux_display.headless_swap_mode.clear();
      if (retire_connectors) {
        if (!linux_display.streaming_output.empty() &&
            config::video.output_name == linux_display.streaming_output) {
          config::video.output_name.clear();
        }
        linux_display.streaming_output.clear();
        linux_display.primary_output.clear();
      }
    }

    void normalize_host_virtual_display_state(std::string_view stream_mode, capture_rewrite_scope_e scope) {
      normalize_host_virtual_display_state_for_backend(
        virtual_display::detect_backend(),
        scope,
        stream_mode
      );
    }

    bool selection_available_for_capabilities(
      std::string_view selection,
      bool virtual_display_available
    ) {
      if (const auto *path = stream_path::find(selection)) {
        if (!path->available) {
          return false;
        }
        if (path->runtime == stream_path::runtime_kind_e::LABWC) {
          return stream_path::labwc_runtime_available(
            stream_path::probe_host_capabilities()
          );
        }
        if (path->id == stream_path::k_gamescope_stream) {
          return stream_path::probe_host_capabilities().gamescope_present;
        }
        if (path->id == stream_path::k_host_virtual_display) {
          return virtual_display_available;
        }
        if (path->id == stream_path::k_desktop_takeover) {
          return virtual_display_available && desktop_takeover::is_available();
        }
        return true;
      }
      return false;
    }

    std::string selection_unavailable_reason_for_capabilities(
      std::string_view selection,
      bool virtual_display_available
    ) {
      if (const auto *path = stream_path::find(selection)) {
        if (path->runtime == stream_path::runtime_kind_e::LABWC) {
          const auto caps = stream_path::probe_host_capabilities();
          if (!stream_path::labwc_runtime_available(caps)) {
            return stream_path::labwc_runtime_unavailable_reason(caps);
          }
        }
        if (path->id == stream_path::k_gamescope_stream &&
            !stream_path::probe_host_capabilities().gamescope_present) {
          return "gamescope binary not found on PATH";
        }
        if (path->id == stream_path::k_host_virtual_display &&
            !virtual_display_available) {
          return "Host virtual display is not available on this host.";
        }
        if (path->id == stream_path::k_desktop_takeover &&
            !virtual_display_available) {
          return "Desktop Takeover requires a virtual-display backend that creates a new output.";
        }
        if (path->id == stream_path::k_desktop_takeover &&
            !desktop_takeover::is_available()) {
          return desktop_takeover::unavailable_reason();
        }
        if (!path->unavailable_reason.empty()) {
          return std::string {path->unavailable_reason};
        }
        return std::string {path->label} + " is not available on this host.";
      }
      return "Unknown stream display mode.";
    }

  }  // namespace

  std::string configured_selection() {
    auto &linux_display = config::video.linux_display;
    if (!linux_display.stream_mode.empty() && stream_path::find(linux_display.stream_mode)) {
      return stream_path::to_lower_copy(linux_display.stream_mode);
    }
    return selection_from_legacy_booleans({
      linux_display.headless_mode,
      linux_display.use_cage_compositor,
      linux_display.prefer_gpu_native_capture,
    });
  }

  bool selection_owns_launch_refresh_rate(std::string_view selection) {
    const auto key = stream_path::to_lower_copy(selection);
    return key == k_headless_stream ||
           key == k_windowed_stream ||
           key == k_host_virtual_display ||
           key == k_desktop_takeover ||
           key == k_gamescope_stream;
  }

  std::string label_for_selection(std::string_view selection) {
    if (const auto *path = stream_path::find(selection)) {
      return std::string {path->label};
    }
    return {};
  }

  std::string reason_for_selection(std::string_view selection, bool virtual_display_available) {
    if (const auto *path = stream_path::find(selection)) {
      if ((path->id == stream_path::k_host_virtual_display ||
           path->id == stream_path::k_desktop_takeover) &&
          !virtual_display_available) {
        return "Polaris requested a host virtual display, but no backend is currently available.";
      }
      return std::string {path->reason};
    }
    return "Polaris will mirror the current desktop session.";
  }

  bool selection_available(std::string_view selection) {
    const auto key = to_lower_copy(selection);
    return selection_available_for_capabilities(
      key,
      (key != k_host_virtual_display && key != k_desktop_takeover) ||
        virtual_display::is_available()
    );
  }

  bool desktop_mirror_yields_to_selection(std::string_view selection) {
    return selection == k_desktop_takeover || selection == k_host_virtual_display;
  }

  bool selection_session_overridable(std::string_view selection) {
    if (const auto *path = stream_path::find(selection)) {
      // Swapping the host's primary output rearranges the machine itself, so it
      // is a host decision rather than something one client turns on per launch.
      return path->topology != stream_path::topology_kind_e::SWAP_PRIMARY;
    }
    return false;
  }

  bool private_runtime_selection_available() {
    stream_path::host_capabilities_t caps;
    caps.labwc_present = stream_path::binary_on_path("labwc");
    caps.wlr_randr_present = stream_path::binary_on_path("wlr-randr");
    caps.gamescope_present = stream_path::binary_on_path("gamescope");
    for (const auto &path : stream_path::registry()) {
      if (!path.available || !selection_session_overridable(path.id)) {
        continue;
      }
      if (path.runtime == stream_path::runtime_kind_e::LABWC && stream_path::labwc_runtime_available(caps)) {
        return true;
      }
      if (path.runtime == stream_path::runtime_kind_e::GAMESCOPE && caps.gamescope_present) {
        return true;
      }
    }
    return false;
  }

  std::string selection_unavailable_reason(std::string_view selection) {
    const auto key = to_lower_copy(selection);
    const bool virtual_display_available =
      (key != k_host_virtual_display && key != k_desktop_takeover) ||
        virtual_display::is_available();
    if ((key == k_host_virtual_display || key == k_desktop_takeover) &&
        !virtual_display_available) {
      const auto backend_reason = virtual_display::unavailable_reason();
      if (!backend_reason.empty()) {
        return backend_reason;
      }
    }
    return selection_unavailable_reason_for_capabilities(
      key,
      virtual_display_available
    );
  }

  std::string selection_from_legacy_booleans(const legacy_booleans_t &booleans) {
    if (!booleans.headless_mode) {
      if (booleans.use_cage_compositor) {
        return std::string {k_windowed_stream};
      }
      return std::string {k_desktop_display};
    }
    if (!booleans.use_cage_compositor) {
      return std::string {k_host_virtual_display};
    }
    if (booleans.prefer_gpu_native_capture) {
      return std::string {k_windowed_stream};
    }
    return std::string {k_headless_stream};
  }

  legacy_booleans_t legacy_booleans_for_selection(std::string_view selection) {
    legacy_booleans_t booleans;
    const auto key = to_lower_copy(selection);
    if (key == k_headless_stream) {
      booleans.headless_mode = true;
      booleans.use_cage_compositor = true;
      booleans.prefer_gpu_native_capture = false;
    }
    else if (key == k_windowed_stream) {
      booleans.headless_mode = true;
      booleans.use_cage_compositor = true;
      booleans.prefer_gpu_native_capture = true;
    }
    else if (key == k_host_virtual_display ||
             key == k_desktop_takeover ||
             key == stream_path::k_headless_dongle) {
      // Dongle: host desktop path with topology swap — not private labwc.
      booleans.headless_mode = true;
      booleans.use_cage_compositor = false;
      booleans.prefer_gpu_native_capture = false;
    }
    else if (key == k_gamescope_stream) {
      booleans.headless_mode = true;
      booleans.use_cage_compositor = false;
      booleans.prefer_gpu_native_capture = false;
    }
    else {
      booleans.headless_mode = false;
      booleans.use_cage_compositor = false;
      booleans.prefer_gpu_native_capture = false;
    }
    return booleans;
  }

  app_launch_as_t resolve_app_launch_as(std::string_view launch_as, std::string_view client_named_selection) {
    if (launch_as == "host_default" || launch_as == k_desktop_display) {
      return {};
    }
    // Registry lookups accept case-folded host settings. An app pin is an
    // exact saved value, so reject a lookup whose canonical id differs.
    const auto *path = stream_path::find(launch_as);
    if (!path || path->id != launch_as || !selection_session_overridable(launch_as)) {
      return {app_launch_as_t::verdict_e::not_a_launch_mode, {}};
    }
    const bool conflict = !client_named_selection.empty() && client_named_selection != launch_as;
    return {conflict ? app_launch_as_t::verdict_e::conflict : app_launch_as_t::verdict_e::pinned,
      std::string {launch_as}};
  }

  std::string host_default_launch_selection(const host_default_launch_input_t &input) {
    const auto &[requested_selection, mirror_desktop, launch_virtual_display,
      virtual_display_user_locked, virtual_display_optimization_present,
      host_provides_private_display] = input;
    if (mirror_desktop) {
      return std::string {k_desktop_display};
    }
    if (virtual_display_user_locked) {
      if (!requested_selection.empty()) {
        return std::string {requested_selection};
      }
      if (launch_virtual_display) {
        return std::string {k_host_virtual_display};
      }
    }
    // An unlocked client toggle does not override a host
    // that already provides the session's display. The private labwc runtime
    // creates that output itself, so a second one only trades a GPU-native
    // path for an EVDI one nobody asked for.
    if (host_provides_private_display) {
      if (!requested_selection.empty()) {
        return std::string {requested_selection};
      }
      return {};
    }
    if (!requested_selection.empty()) {
      return std::string {requested_selection};
    }
    if (launch_virtual_display) {
      return std::string {k_host_virtual_display};
    }
    return {};
  }

  std::string capture_output_name_for_virtual_display(
    std::string_view created_output_name,
    std::string_view mapped_output_name
  ) {
    return std::string {mapped_output_name.empty() ? created_output_name : mapped_output_name};
  }

  std::string capture_for_host_virtual_display_backend(
    virtual_display::backend_e backend,
    std::string_view current_capture
  ) {
    return decide_capture_for_host_virtual_display_backend(backend, current_capture).capture;
  }

  std::string capture_for_session_transition(
    std::string_view configured_selection,
    std::string_view session_selection,
    std::string_view current_capture
  ) {
    const auto decision = decide_capture_for_session_transition(
      configured_selection,
      session_selection,
      current_capture
    );
    return decision.capture;
  }

  std::string capture_for_mode(
    std::string_view configured_capture,
    std::string_view stream_mode,
    bool use_cage_compositor,
    bool substitution_active,
    bool exact_output_owned
  ) {
    const auto decision = decide_capture_for_mode(
      configured_capture,
      stream_mode,
      use_cage_compositor,
      substitution_active,
      exact_output_owned
    );
    return decision.capture;
  }

  std::string capture_for_current_mode(bool exact_output_owned) {
    return capture_for_mode(
      config::video.capture,
      config::video.linux_display.stream_mode,
      config::video.linux_display.use_cage_compositor,
      !platf::capture_backend_substitution_note().empty(),
      exact_output_owned
    );
  }

  std::string capture_filled_for_mode(std::string_view stream_mode, std::string_view capture) {
    const auto mode = to_lower_copy(stream_mode);
    // A Gamescope session is not the host desktop, and the automatic search would land on a
    // backend that captures the desktop instead of it.
    if (mode == k_gamescope_stream && capture.empty()) {
      return "portal";
    }
    // KMS needs CAP_SYS_ADMIN and returns an empty monitor list without it, so the dongle captures
    // the host desktop through the portal once the topology is swapped. Explicit kms is kept.
    if (mode == k_headless_dongle && (capture.empty() || capture == "auto")) {
      return "portal";
    }
    return std::string {capture};
  }

  std::string capture_for_launch_into_current_mode(bool exact_output_owned) {
    const auto &stream_mode = config::video.linux_display.stream_mode;
    return capture_for_mode(
      capture_filled_for_mode(stream_mode, config::video.capture),
      stream_mode,
      config::video.linux_display.use_cage_compositor,
      !platf::capture_backend_substitution_note().empty(),
      exact_output_owned
    );
  }

  std::optional<capture_override_t> capture_mode_override(
    std::string_view configured_capture,
    std::string_view stream_mode,
    bool use_cage_compositor,
    bool substitution_active,
    bool exact_output_owned
  ) {
    return override_for(
      configured_capture,
      decide_capture_for_mode(
        configured_capture,
        stream_mode,
        use_cage_compositor,
        substitution_active,
        exact_output_owned
      )
    );
  }

  std::optional<capture_override_t> capture_mode_override_for_current_mode(bool exact_output_owned) {
    // Decided on the live setting, as capture_for_current_mode() is, and measured against
    // polaris.conf as loaded: the live setting may already be a replacement nobody chose.
    return override_for(
      loaded_capture_setting(),
      decide_capture_for_mode(
        config::video.capture,
        config::video.linux_display.stream_mode,
        config::video.linux_display.use_cage_compositor,
        !platf::capture_backend_substitution_note().empty(),
        exact_output_owned
      )
    );
  }

  std::string capture_request_override_reason(
    std::string_view preference,
    std::string_view requested,
    std::string_view stream_mode,
    bool use_cage_compositor,
    bool exact_output_owned
  ) {
    const auto preferred = canonical_capture_backend(preference);
    const auto asked = canonical_capture_backend(requested);
    if (preferred.empty() || preferred == asked) {
      return {};
    }
    const auto &live = config::video.capture;
    const auto decision = decide_capture_for_mode(
      live,
      stream_mode,
      use_cage_compositor,
      !platf::capture_backend_substitution_note().empty(),
      exact_output_owned
    );
    // The mode rule answers only for the backend it asked for. A rule that set the live setting
    // aside for another backend did not produce this request.
    if (!decision.reason.empty() &&
        canonical_capture_backend(decision.capture) != canonical_capture_backend(live) &&
        canonical_capture_backend(decision.capture) == asked) {
      return std::string {decision.reason};
    }
    // Otherwise the newest rule that wrote the backend this request asked for.
    return capture_rewrite_reason_for(asked);
  }

  std::optional<capture_override_t> capture_session_transition_override(
    std::string_view configured_selection,
    std::string_view session_selection,
    std::string_view current_capture
  ) {
    return override_for(
      current_capture,
      decide_capture_for_session_transition(configured_selection, session_selection, current_capture)
    );
  }

  void apply_capture_for_session_transition(
    std::string_view configured_selection,
    std::string_view session_selection
  ) {
    const auto decision = decide_capture_for_session_transition(
      configured_selection,
      session_selection,
      config::video.capture
    );
    if (decision.capture == config::video.capture) {
      // Nothing to rewrite: an older rule already wrote this backend, such as a Host Virtual
      // Display load before a launch into a Gamescope session. This session's own rule still asks
      // for it, and that rule, not the older one, is why its request differs from polaris.conf.
      note_capture_rewrite(rewrite_source_e::session, decision.capture, decision.reason);
      return;
    }
    // Measured against polaris.conf as loaded. After a Host Virtual Display load the live setting is
    // already the replacement, and a host set to kms was told that its configured backend was portal.
    if (const auto discarded = override_for(loaded_capture_setting(), decision)) {
      // An explicit choice set aside, such as kms under a Gamescope session, is what a host on a
      // capture path nobody picked looks like, so it is a warning and not a note.
      BOOST_LOG(warning) << describe_capture_override(*discarded, session_selection)
                         << "; for this session only, and the host setting comes back at teardown";
    }
    else {
      BOOST_LOG(info) << "process: session capture backend override [" << describe_capture(config::video.capture)
                      << "] -> [" << describe_capture(decision.capture) << "] for stream mode ["
                      << session_selection << "]; host default restored at teardown";
    }
    config::video.capture = decision.capture;
    note_capture_rewrite(rewrite_source_e::session, decision.capture, decision.reason);
  }

  std::optional<capture_override_t> capture_host_virtual_display_override(
    virtual_display::backend_e backend,
    std::string_view current_capture
  ) {
    return override_for(
      current_capture,
      decide_capture_for_host_virtual_display_backend(backend, current_capture)
    );
  }

  std::string describe_capture_override(
    const capture_override_t &override,
    std::string_view stream_mode
  ) {
    std::string_view why;
    if (override.reason == k_capture_override_private_compositor) {
      why = "a private labwc session can only be captured through wlroots";
    }
    else if (override.reason == k_capture_override_substituted) {
      why = "the configured backend captured nothing in this mode, and auto lands on the backend the evaluation found";
    }
    else if (override.reason == k_capture_override_gamescope_session) {
      why = "a Gamescope session is captured through the portal";
    }
    else if (override.reason == k_capture_override_dongle_session) {
      why = "a dongle session captures the host desktop through the portal once the topology is swapped";
    }
    else if (override.reason == k_capture_override_desktop_discovery) {
      why = "Mirror Desktop lets desktop capture discovery choose, which prefers KMS when the process may use it and the portal otherwise";
    }
    else if (override.reason == k_capture_override_virtual_display_backend) {
      why = "a host virtual display can only be captured the way its backend exposes it";
    }
    else {
      why = "a stream mode rule replaced it";
    }

    std::string line = "capture override: stream mode [";
    line += stream_mode.empty() ? std::string_view {"unset"} : stream_mode;
    line += "] asks for [";
    line += describe_capture(override.effective);
    line += "], not the configured [";
    line += describe_capture(override.configured);
    line += "], because ";
    line += why;
    return line;
  }

  std::string loaded_capture_setting() {
    const std::lock_guard<std::mutex> guard {loaded_capture_mutex};
    return loaded_capture;
  }

  void log_config_load_notes() {
    std::vector<load_note_t> notes;
    {
      const std::lock_guard<std::mutex> guard {loaded_capture_mutex};
      notes.swap(load_notes);
    }
    for (const auto &note : notes) {
      if (note.warning) {
        BOOST_LOG(warning) << note.line;
      }
      else {
        BOOST_LOG(info) << note.line;
      }
    }
  }

  void normalize_host_virtual_display_state_for_backend(
      virtual_display::backend_e backend,
      capture_rewrite_scope_e scope,
      std::string_view stream_mode) {
    auto &linux_display = config::video.linux_display;
    clear_connector_output_authority(
      host_virtual_backend_creates_output(backend)
    );
    // KScreen needs the streaming connector it manages, but never the dongle
    // profile's separate primary-output authority. EVDI, KWin and wlroots
    // create a new output, so their old connector and capture-output pins are
    // retired.
    linux_display.primary_output.clear();
    // Entering from a private or desktop mode, the active connector was retired
    // on load; KScreen borrows the saved one.
    if (!host_virtual_backend_creates_output(backend) && linux_display.streaming_output.empty()) {
      linux_display.streaming_output = linux_display.saved_streaming_output;
    }
    const auto previous_capture = config::video.capture;
    config::video.capture = capture_for_host_virtual_display_backend(
      backend,
      previous_capture
    );
    // A preview is put back by its caller, and the launch that enters the mode applies it again and
    // speaks for it then. A line here described a state the host never kept.
    if (config::video.capture == previous_capture || scope == capture_rewrite_scope_e::preview) {
      return;
    }
    note_capture_rewrite(
      scope == capture_rewrite_scope_e::load ? rewrite_source_e::load : rewrite_source_e::session,
      config::video.capture,
      k_capture_override_virtual_display_backend
    );

    const std::string mode = stream_mode.empty() ? linux_display.stream_mode : std::string {stream_mode};
    const std::string backend_label = virtual_display::backend_name(backend);
    const bool other_backend = scope == capture_rewrite_scope_e::backend_change;
    // Measured against polaris.conf as loaded, in every scope. The live setting the rewrite found may
    // already be a replacement nobody chose: what a Host Virtual Display load put in place, which a
    // launch finds when the virtual display backend changed since, or what a launch put in place
    // before its display came up on another backend. At load the two are the same value.
    const auto configured = loaded_capture_setting();
    const auto override = capture_host_virtual_display_override(backend, configured);
    std::string line;
    if (override) {
      // This discards a capture backend the operator chose. Say so, rather than leaving them to
      // infer it from a capture path they did not pick, and say how long it lasts.
      line = describe_capture_override(*override, mode);
      line += other_backend ? ". The virtual display came up on [" : ". The backend is [";
      line += backend_label;
      line += other_backend ? "], not on the backend the launch checked" : "]";
      line += scope == capture_rewrite_scope_e::load ?
                "; this lasts until Polaris restarts, and polaris.conf is not changed" :
                "; for this session only, and the host setting comes back at teardown";
    }
    else if (other_backend) {
      line = "stream_display_policy: the virtual display came up on [" + backend_label +
             "], not on the backend the launch checked, so stream mode [" + mode + "] captures it through [" +
             describe_capture(config::video.capture) + "] instead of [" + describe_capture(previous_capture) + "]";
    }
    else {
      line = "stream_display_policy: stream mode [" + mode + "] captures the [" + backend_label +
             "] virtual display through [" + describe_capture(config::video.capture) + "], with capture set to [" +
             describe_capture(configured) + "]";
    }

    if (scope == capture_rewrite_scope_e::load) {
      // polaris.conf is parsed before logging starts, and a line logged now reaches stdout only.
      keep_load_note(override.has_value(), std::move(line));
    }
    else if (override) {
      BOOST_LOG(warning) << line;
    }
    else {
      BOOST_LOG(info) << line;
    }
  }

  bool host_virtual_backend_creates_output(
      virtual_display::backend_e backend) {
    return backend == virtual_display::backend_e::EVDI ||
           backend == virtual_display::backend_e::WAYLAND_WLR ||
           backend == virtual_display::backend_e::KWIN_VIRTUAL_OUTPUT;
  }

  bool host_virtual_connector_state_matches(
      virtual_display::backend_e backend,
      std::string_view streaming_output,
      std::string_view output_name) {
    return !host_virtual_backend_creates_output(backend) ||
           (streaming_output.empty() && output_name.empty());
  }

  resolved_t resolve(const input_t &input) {
    return resolve_for_state(
      input,
      configured_selection(),
      config::video.linux_display.stream_mode,
      live_booleans(),
      config::video.capture
    );
  }

  void remember_host_default(
      const legacy_booleans_t &booleans,
      std::string_view stream_mode,
      std::string_view capture) {
    const std::lock_guard<std::mutex> guard {host_default_mutex};
    held_host_default = host_default_t {
      booleans,
      std::string {stream_mode},
      std::string {capture},
    };
  }

  namespace {
    /// Everything a switch to the mirror can reset, which is also what execute_impl keeps for a
    /// session-scoped mode: a dongle or Host Virtual Display host has its connectors, swap mode and
    /// display management cleared by the switch, and would come back from Game Mode without them.
    struct game_mode_hold_t {
      legacy_booleans_t booleans;
      std::string stream_mode;
      std::string private_runtime;
      std::string headless_swap_mode;
      std::string streaming_output;
      std::string primary_output;
      bool auto_manage_displays {false};
      std::string capture;
      std::string output_name;
      std::string selection;
    };

    std::mutex game_mode_mutex;
    std::optional<game_mode_hold_t> game_mode_hold;
  }  // namespace

  game_mode_reconcile_e reconcile_game_mode(bool session_live, bool stream_active) {
    const std::lock_guard<std::mutex> guard {game_mode_mutex};
    auto &linux_display = config::video.linux_display;
    const auto configured = configured_selection();

    if (session_live) {
      if (configured == k_desktop_display || stream_active) {
        return game_mode_reconcile_e::unchanged;
      }
      game_mode_hold_t hold {
        live_booleans(),
        linux_display.stream_mode,
        linux_display.private_runtime,
        linux_display.headless_swap_mode,
        linux_display.streaming_output,
        linux_display.primary_output,
        linux_display.auto_manage_displays,
        config::video.capture,
        config::video.output_name,
        configured,
      };
      std::string error;
      if (!apply_selection(k_desktop_display, error)) {
        return game_mode_reconcile_e::unchanged;
      }
      const auto decision = decide_capture_for_session_transition(configured, k_desktop_display, hold.capture);
      config::video.capture = decision.capture;
      note_capture_rewrite(rewrite_source_e::game_mode, decision.capture, decision.reason);
      // Measured against polaris.conf as loaded, as a launch's transition is. After a Host Virtual
      // Display load the held setting is that load's replacement, and naming it as the configured
      // backend told a host set to kms that it had chosen wlr, and warned a host with capture unset.
      if (const auto override = override_for(loaded_capture_setting(), decision)) {
        BOOST_LOG(warning) << describe_capture_override(*override, k_desktop_display)
                           << "; Steam Game Mode is running, and [" << describe_capture(hold.capture)
                           << "] comes back when the Game Mode session ends";
      }
      else if (config::video.capture != hold.capture) {
        BOOST_LOG(info) << "game_mode: capture [" << describe_capture(hold.capture) << "] is ["
                        << describe_capture(config::video.capture) << "] while Steam Game Mode is running";
      }
      game_mode_hold = std::move(hold);
      return game_mode_reconcile_e::entered;
    }

    if (!game_mode_hold || stream_active) {
      return game_mode_reconcile_e::unchanged;
    }
    // A mode chosen on purpose, or a config reload, drops the hold (forget_game_mode_hold), so a
    // hold that is still here is the one this function took. Anything but the mirror is a change
    // it did not see, and the newer values stay.
    if (configured == k_desktop_display) {
      linux_display.stream_mode = game_mode_hold->stream_mode;
      linux_display.headless_mode = game_mode_hold->booleans.headless_mode;
      linux_display.use_cage_compositor = game_mode_hold->booleans.use_cage_compositor;
      linux_display.prefer_gpu_native_capture = game_mode_hold->booleans.prefer_gpu_native_capture;
      linux_display.private_runtime = game_mode_hold->private_runtime;
      linux_display.headless_swap_mode = game_mode_hold->headless_swap_mode;
      linux_display.streaming_output = game_mode_hold->streaming_output;
      linux_display.primary_output = game_mode_hold->primary_output;
      linux_display.auto_manage_displays = game_mode_hold->auto_manage_displays;
      config::video.capture = game_mode_hold->capture;
      // The switch clears the output name only when it named the streaming connector.
      if (config::video.output_name.empty()) {
        config::video.output_name = game_mode_hold->output_name;
      }
    }
    game_mode_hold.reset();
    forget_capture_rewrites(rewrite_source_e::game_mode);
    return game_mode_reconcile_e::left;
  }

  std::string game_mode_held_selection() {
    const std::lock_guard<std::mutex> guard {game_mode_mutex};
    return game_mode_hold ? game_mode_hold->selection : std::string {};
  }

  void forget_game_mode_hold() {
    const std::lock_guard<std::mutex> guard {game_mode_mutex};
    game_mode_hold.reset();
    forget_capture_rewrites(rewrite_source_e::game_mode);
  }

  void forget_host_default() {
    {
      const std::lock_guard<std::mutex> guard {host_default_mutex};
      held_host_default.reset();
    }
    // The live setting is the host default again, so a launch's rewrite no longer explains it.
    forget_capture_rewrites(rewrite_source_e::session);
  }

  std::string host_default_selection() {
    const auto held = load_host_default();
    if (!held) {
      return configured_selection();
    }
    return selection_for_host_default(*held);
  }

  bool host_default_provides_private_display() {
    const auto resolved = resolve_host_default(input_t {
      virtual_display::is_available(),
      false,
      false,
    });
    return resolved.uses_labwc() && resolved.requested_headless;
  }

  resolved_t resolve_host_default(const input_t &input) {
    // One read of the held snapshot for the selection and the state it resolves
    // against, so a teardown landing mid-call cannot mix the two.
    const auto held = load_host_default();
    if (!held) {
      return resolve(input);
    }
    return resolve_for_state(
      input,
      selection_for_host_default(*held),
      held->stream_mode,
      held->booleans,
      held->capture
    );
  }

  resolved_t resolve_current(bool active_encoder_requires_gpu_native_capture,
                             bool runtime_gpu_native_override_active) {
    return resolve(input_t {
      virtual_display::is_available(),
      active_encoder_requires_gpu_native_capture,
      runtime_gpu_native_override_active,
    });
  }

  resolved_t resolve_effective(const input_t &input,
                               bool streaming,
                               bool session_uses_virtual_display,
                               bool runtime_effective_headless) {
    if (!streaming) {
      return resolve(input);
    }

    auto configured = resolve(input);
    if (input.runtime_gpu_native_override_active) {
      configured.selection = std::string {k_windowed_stream};
      configured.label = label_for_selection(k_windowed_stream);
      configured.effective_headless = false;
      configured.prefer_gpu_native_capture = true;
      configured.backend_name = "labwc";
      return configured;
    }
    if (session_uses_virtual_display) {
      const auto active_virtual_mode = configured.selection == k_desktop_takeover ?
                                         k_desktop_takeover :
                                         k_host_virtual_display;
      if (const auto *path = stream_path::find(active_virtual_mode)) {
        auto caps = stream_path::probe_host_capabilities();
        caps.virtual_display_available = input.virtual_display_available;
        return stream_path::resolve_path(*path, caps);
      }
    }
    if (configured.selection == k_windowed_stream && runtime_effective_headless) {
      return configured;
    }
    if (runtime_effective_headless && configured.uses_labwc()) {
      configured.selection = std::string {k_headless_stream};
      configured.label = label_for_selection(k_headless_stream);
      configured.effective_headless = true;
      return configured;
    }
    return configured;
  }

  bool selection_valid_for_capabilities(
    std::string_view selection,
    bool virtual_display_available,
    std::string &error
  ) {
    const auto key = to_lower_copy(selection);
    if (!stream_path::find(key) && key != k_desktop_display) {
      error = "stream_display_mode must be a known stream path id (see /client-settings modes)";
      return false;
    }
    if (!selection_available_for_capabilities(key, virtual_display_available)) {
      error = selection_unavailable_reason_for_capabilities(
        key,
        virtual_display_available
      );
      return false;
    }
    return true;
  }

  bool selection_valid(std::string_view selection, std::string &error) {
    const auto key = to_lower_copy(selection);
    return selection_valid_for_capabilities(
      key,
      (key != k_host_virtual_display && key != k_desktop_takeover) ||
        virtual_display::is_available(),
      error
    );
  }

  bool selection_valid_fresh(std::string_view selection, std::string &error) {
    const auto key = to_lower_copy(selection);
    if (key == k_desktop_takeover && !desktop_takeover::is_available_fresh()) {
      error = desktop_takeover::unavailable_reason();
      return false;
    }
    return selection_valid_for_capabilities(
      key,
      (key != k_host_virtual_display && key != k_desktop_takeover) ||
        virtual_display::is_available_fresh(),
      error
    );
  }

  bool selection_companion_state_matches(std::string_view selection) {
    const auto key = to_lower_copy(selection);
    const auto &linux_display = config::video.linux_display;
    if (linux_display.stream_mode != key) {
      return false;
    }

    const auto expected = legacy_booleans_for_selection(key);
    if (linux_display.headless_mode != expected.headless_mode ||
        linux_display.use_cage_compositor != expected.use_cage_compositor ||
        linux_display.prefer_gpu_native_capture != expected.prefer_gpu_native_capture) {
      return false;
    }

    if (const auto *path = stream_path::find(key)) {
      auto expected_runtime = std::string {stream_path::runtime_kind_id(path->runtime)};
      if (expected_runtime.empty() && expected.use_cage_compositor) {
        expected_runtime = std::string {k_runtime_labwc};
      }
      if (linux_display.private_runtime != expected_runtime) {
        return false;
      }

      if (key != stream_path::k_headless_dongle &&
          linux_display.auto_manage_displays) {
        return false;
      }
      if (key == k_host_virtual_display || key == k_desktop_takeover) {
        const auto backend = virtual_display::detect_backend();
        if (config::video.capture != capture_for_host_virtual_display_backend(
                                       backend,
                                       config::video.capture
                                     ) ||
            !host_virtual_connector_state_matches(
              backend,
              linux_display.streaming_output,
              config::video.output_name
            )) {
          return false;
        }
      }
      if (key == stream_path::k_gamescope_stream && config::video.capture.empty()) {
        return false;
      }
      if (key == stream_path::k_headless_dongle) {
        return linux_display.auto_manage_displays &&
               !linux_display.headless_swap_mode.empty() &&
               !linux_display.streaming_output.empty() &&
               !linux_display.primary_output.empty() &&
               linux_display.streaming_output != linux_display.primary_output &&
               config::video.capture != "auto" &&
               !config::video.capture.empty() &&
               !config::video.output_name.empty();
      }
      return true;
    }

    return key == k_desktop_display;
  }

  bool apply_selection(std::string_view selection, std::string &error, capture_rewrite_scope_e scope) {
    if (!selection_valid_fresh(selection, error)) {
      return false;
    }
    const auto key = to_lower_copy(selection);

    auto &linux_display = config::video.linux_display;

    // Connector/capture-target ownership belongs only to canonical dongle mode
    // and the KScreen Host Virtual fallback. Clear it for Desktop, Gamescope,
    // and compositor-private modes so a prior dongle cannot pin capture or SDL
    // fullscreen hints after its topology actuator has been disabled.
    if (key != stream_path::k_headless_dongle &&
        key != k_host_virtual_display && key != k_desktop_takeover) {
      const bool retiring_connector_state =
        linux_display.stream_mode == stream_path::k_headless_dongle ||
        linux_display.stream_mode == k_host_virtual_display ||
        linux_display.stream_mode == k_desktop_takeover ||
        linux_display.auto_manage_displays ||
        !linux_display.headless_swap_mode.empty();
      clear_connector_output_authority(retiring_connector_state);
    }

    if (key == k_host_virtual_display || key == k_desktop_takeover) {
      normalize_host_virtual_display_state(key, scope);
    }

    if (key == stream_path::k_gamescope_stream) {
      auto caps = stream_path::probe_host_capabilities();
      if (!caps.gamescope_present) {
        error = "gamescope_stream requires the gamescope binary on PATH";
        return false;
      }
      // Prefer portal capture of gamescope; leave idle unit free to attach.
      if (auto filled = capture_filled_for_mode(key, config::video.capture); filled != config::video.capture) {
        config::video.capture = std::move(filled);
        say_portal_fill(key, scope);
      }
    }

    // Dongle path: auto-detect connectors from DRM if unset (sysfs), then validate.
    if (key == stream_path::k_headless_dongle) {
      if (!display_topology::ensure_dongle_outputs_configured()) {
        error = "headless_dongle: could not auto-detect distinct dongle and panel outputs; set linux_streaming_output and linux_primary_output (see kscreen-doctor -o / sysfs drm)";
        return false;
      }
      if (linux_display.streaming_output == linux_display.primary_output) {
        error = "headless_dongle needs distinct streaming and primary outputs";
        return false;
      }
      linux_display.auto_manage_displays = true;
      if (linux_display.headless_swap_mode.empty()) {
        linux_display.headless_swap_mode = "privacy";
      }
      // Prefer portal on Wayland hosts: KMS needs CAP_SYS_ADMIN and returns an empty
      // monitor list without it (lea). Portal ScreenCast after topology prepare is the
      // working path once the portal lock-contract is fixed. Explicit capture=kms is kept.
      if (auto filled = capture_filled_for_mode(key, config::video.capture); filled != config::video.capture) {
        config::video.capture = std::move(filled);
        say_portal_fill(key, scope);
      }
      if (config::video.output_name.empty()) {
        config::video.output_name = linux_display.streaming_output;
      }
    }

    const auto booleans = legacy_booleans_for_selection(key);
    linux_display.stream_mode = key;
    linux_display.headless_mode = booleans.headless_mode;
    linux_display.use_cage_compositor = booleans.use_cage_compositor;
    linux_display.prefer_gpu_native_capture = booleans.prefer_gpu_native_capture;

    if (const auto *path = stream_path::find(key)) {
      linux_display.private_runtime = std::string {stream_path::runtime_kind_id(path->runtime)};
      if (linux_display.private_runtime.empty() && booleans.use_cage_compositor) {
        linux_display.private_runtime = std::string {k_runtime_labwc};
      }
    }
    else if (booleans.use_cage_compositor) {
      linux_display.private_runtime = std::string {k_runtime_labwc};
    }

    return true;
  }

  void normalize_config_from_load() {
    // What was just loaded is what the host is configured for now. A hold taken before the load
    // would put back older values when Game Mode ends; the next reconcile holds these instead.
    forget_game_mode_hold();
    {
      const std::lock_guard<std::mutex> guard {loaded_capture_mutex};
      loaded_capture = config::video.capture;
      // Lines an older load kept and nobody said belong to a configuration that is gone.
      load_notes.clear();
    }
    // So does every record of why the live setting differed from the one that was loaded before.
    forget_capture_rewrites(std::nullopt);
    auto &linux_display = config::video.linux_display;

    if (!linux_display.stream_mode.empty()) {
      if (const auto *path = stream_path::find(linux_display.stream_mode)) {
        const auto booleans = legacy_booleans_for_selection(path->id);
        linux_display.headless_mode = booleans.headless_mode;
        linux_display.use_cage_compositor = booleans.use_cage_compositor;
        linux_display.prefer_gpu_native_capture = booleans.prefer_gpu_native_capture;
        linux_display.private_runtime = std::string {
          stream_path::runtime_kind_id(path->runtime)
        };
        if (linux_display.private_runtime.empty() && booleans.use_cage_compositor) {
          linux_display.private_runtime = std::string {k_runtime_labwc};
        }
        if (path->id == k_host_virtual_display || path->id == k_desktop_takeover) {
          normalize_host_virtual_display_state(path->id, capture_rewrite_scope_e::load);
        }
        if (path->id != stream_path::k_headless_dongle &&
            path->id != k_host_virtual_display &&
            path->id != k_desktop_takeover) {
          // These modes never own a connector. The previous condition (leftover
          // auto-management or swap mode) could not be false here, because
          // headless_swap_mode cannot load empty, so retire unconditionally.
          // Host Virtual Display reads saved_streaming_output instead.
          clear_connector_output_authority(true);
        }
        // headless_dongle: default to portal (host desktop after topology swap).
        // Do not force KMS — without CAP_SYS_ADMIN encoder probe fails empty.
        if (path->id == stream_path::k_headless_dongle) {
          if (auto filled = capture_filled_for_mode(path->id, config::video.capture);
              filled != config::video.capture) {
            say_portal_fill(path->id, capture_rewrite_scope_e::load);
            config::video.capture = std::move(filled);
          }
        }
        return;
      }
      linux_display.stream_mode.clear();
    }

    linux_display.stream_mode = selection_from_legacy_booleans({
      linux_display.headless_mode,
      linux_display.use_cage_compositor,
      linux_display.prefer_gpu_native_capture,
    });
    if (linux_display.stream_mode == k_host_virtual_display ||
        linux_display.stream_mode == k_desktop_takeover) {
      normalize_host_virtual_display_state(linux_display.stream_mode, capture_rewrite_scope_e::load);
    }

    if (const auto *path = stream_path::find(linux_display.stream_mode)) {
      linux_display.private_runtime = std::string {
        stream_path::runtime_kind_id(path->runtime)
      };
      if (linux_display.private_runtime.empty() && linux_display.use_cage_compositor) {
        linux_display.private_runtime = std::string {k_runtime_labwc};
      }
    }
  }

  std::vector<mode_option_t> mode_options(const stream_path::host_capabilities_t &caps) {
    return mode_options(caps.virtual_display_available);
  }

  std::vector<mode_option_t> mode_options(bool virtual_display_available) {
    auto caps = stream_path::probe_host_capabilities();
    caps.virtual_display_available = virtual_display_available;
    std::vector<mode_option_t> options;
    for (const auto &path : stream_path::options_for_host(caps)) {
      mode_option_t option;
      option.value = std::string {path.id};
      option.label = std::string {path.label};
      option.badge = std::string {path.badge};
      option.reason = reason_for_selection(path.id, virtual_display_available);
      const bool needs_virtual_display =
        path.id == stream_path::k_host_virtual_display ||
        path.id == stream_path::k_desktop_takeover;
      option.available = path.available &&
        (!needs_virtual_display || virtual_display_available);
      option.unavailable_reason = std::string {path.unavailable_reason};
      if (!option.available &&
          needs_virtual_display &&
          option.unavailable_reason.empty()) {
        option.unavailable_reason = path.id == stream_path::k_desktop_takeover ?
          "Desktop Takeover requires a virtual-display backend that creates a new output." :
          "Host virtual display is not available on this host.";
      }
      option.group = std::string {path.group};
      option.runtime = std::string {stream_path::runtime_kind_id(path.runtime)};
      option.capture = std::string {stream_path::capture_kind_id(path.capture)};
      option.topology = std::string {stream_path::topology_kind_id(path.topology)};
      options.push_back(std::move(option));
    }
    return options;
  }

  std::vector<std::string> allowed_launch_modes(bool virtual_display_available,
                                                bool include_unavailable) {
    std::vector<std::string> modes;
    auto caps = stream_path::probe_host_capabilities();
    caps.virtual_display_available = virtual_display_available;
    // Use options_for_host so gamescope_present (and future host probes) match
    // mode_options / selection_available — no dual-truth availability.
    for (const auto &path : stream_path::options_for_host(caps)) {
      if (!path.available && !include_unavailable) {
        continue;
      }
      // Launch contract only lists primary user paths by default.
      if (path.group == "experimental" && !include_unavailable) {
        continue;
      }
      if ((path.id == stream_path::k_host_virtual_display ||
           path.id == stream_path::k_desktop_takeover) &&
          !virtual_display_available) {
        continue;
      }
      // Dongle is always listable when available; apply auto-fills outputs.
      modes.emplace_back(path.id);
    }
    return modes;
  }

}  // namespace stream_display_policy
