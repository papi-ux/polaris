#!/usr/bin/env bash
# Kept executable as a standalone script so distro packages can install this
# shared body directly. Nix and the manual installer prepend their own wrapper;
# this shebang is then an inert comment.
export PATH="${POLARIS_SESSION_PATH:-$PATH}"

set -euo pipefail
umask 077

if ! declare -F polaris_validate_marker >/dev/null 2>&1; then
  runtime_lib="${POLARIS_GAMESCOPE_RUNTIME_LIB:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/polaris-gamescope-runtime-lib.sh}"
  # shellcheck source=/dev/null
  . "$runtime_lib"
fi

rt="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
marker="$rt/polaris-gamescope.pid"
export DBUS_SESSION_BUS_ADDRESS="unix:path=$rt/bus"
# Module option services.polarisGamescopeSession.hdr — allow force-HDR path at all.
allow_client_hdr=1
gs_width="${POLARIS_HDR_WIDTH:-3840}"
gs_height="${POLARIS_HDR_HEIGHT:-2160}"
gs_refresh="${POLARIS_HDR_REFRESH:-120}"

session_id_file="$rt/polaris-gamescope-session-id"
session_mode_file="$rt/polaris-gamescope-session-mode"
session_state_file="$rt/polaris-gamescope-session-state"

publish_nested_claim() (
  local new_state="$1" expected_state="${2:-absent}"
  local lock_bin="${POLARIS_FLOCK_BIN:-flock}" tmp current_state=absent
  exec 9>>"$rt/polaris-gamescope.lock" || return 1
  "$lock_bin" -x 9 || return 1
  if [ -f "$rt/polaris-gamescope-wsi-nested" ]; then
    current_state="$(tr -d '[:space:]' <"$rt/polaris-gamescope-wsi-nested")"
  fi
  [ "$current_state" = "$expected_state" ] || return 1
  tmp="$rt/.polaris-gamescope-wsi-nested.$$"
  printf '%s\n' "$new_state" >"$tmp" || return 1
  mv -f -- "$tmp" "$rt/polaris-gamescope-wsi-nested"
)

publish_session_mode() (
  local mode="$1" lock_bin="${POLARIS_FLOCK_BIN:-flock}" tmp
  case "$mode" in attach|nested) ;; *) return 1 ;; esac
  exec 9>>"$rt/polaris-gamescope.lock" || return 1
  "$lock_bin" -x 9 || return 1
  [ ! -e "$session_state_file" ] \
    && [ ! -e "$session_id_file" ] \
    && [ ! -e "$session_mode_file" ] || return 1
  tmp="$session_state_file.tmp.$$"
  trap 'rm -f -- "$tmp"' EXIT
  printf '%s %s\n' "$POLARIS_SESSION_INSTANCE_ID" "$mode" >"$tmp" || return 1
  if [ -n "${POLARIS_SESSION_STATE_BEFORE_COMMIT_HOOK:-}" ]; then
    eval "$POLARIS_SESSION_STATE_BEFORE_COMMIT_HOOK" || return 1
  fi
  mv -f -- "$tmp" "$session_state_file"
)

recover_missing_nested_claim() (
  local lock_bin="${POLARIS_FLOCK_BIN:-flock}" tmp persisted persisted_mode extra
  exec 9>>"$rt/polaris-gamescope.lock" || return 1
  "$lock_bin" -x 9 || return 1
  [ ! -e "$rt/polaris-gamescope-wsi-nested" ] || return 0
  if [ -f "$session_state_file" ]; then
    read -r persisted persisted_mode extra <"$session_state_file" || return 1
    [ -z "${extra:-}" ] \
      && [ "$persisted" = "$POLARIS_SESSION_INSTANCE_ID" ] \
      && [ "$persisted_mode" = nested ] || return 1
  else
    [ -s "$session_id_file" ] \
      && [ "$(tr -d '\r\n' <"$session_id_file")" = "$POLARIS_SESSION_INSTANCE_ID" ] \
      && [ -f "$session_mode_file" ] \
      && [ "$(tr -d '[:space:]' <"$session_mode_file")" = nested ] || return 1
  fi
  # Hold flock only; reclaim/validate do not need POLARIS_GAMESCOPE_LOCK_HELD
  # (that flag is for write_runtime_env / stop_marked to skip nested flock).
  if ! polaris_validate_marker "$marker" idle \
      && ! polaris_reclaim_orphan_gamescope_sockets "$rt"; then
    return 1
  fi
  tmp="$rt/.polaris-gamescope-wsi-nested.$$"
  printf 'transition\n' >"$tmp" || return 1
  mv -f -- "$tmp" "$rt/polaris-gamescope-wsi-nested"
)

remove_nested_claim() (
  local expected_state="$1" lock_bin="${POLARIS_FLOCK_BIN:-flock}" current_state
  exec 9>>"$rt/polaris-gamescope.lock" || return 1
  "$lock_bin" -x 9 || return 1
  [ -f "$rt/polaris-gamescope-wsi-nested" ] || return 1
  current_state="$(tr -d '[:space:]' <"$rt/polaris-gamescope-wsi-nested")"
  [ "$current_state" = "$expected_state" ] || return 1
  rm -f -- "$rt/polaris-gamescope-wsi-nested"
)

load_session_instance_id() {
  local persisted persisted_mode extra
  POLARIS_PERSISTED_SESSION_MODE=""
  if [ -f "$session_state_file" ]; then
    read -r persisted persisted_mode extra <"$session_state_file" || return 1
    [ -z "${extra:-}" ] || return 1
    case "$persisted_mode" in attach|nested) ;; *) return 1 ;; esac
    [ -n "$persisted" ] || return 1
    if [ -n "${POLARIS_SESSION_INSTANCE_ID:-}" ]; then
      [ "$persisted" = "$POLARIS_SESSION_INSTANCE_ID" ] || return 1
    else
      export POLARIS_SESSION_INSTANCE_ID="$persisted"
    fi
    POLARIS_PERSISTED_SESSION_MODE="$persisted_mode"
    return 0
  fi
  if [ -n "${POLARIS_SESSION_INSTANCE_ID:-}" ]; then
    if [ -f "$session_id_file" ]; then
      persisted="$(tr -d '\n' <"$session_id_file")" || return 1
      [ "$persisted" = "$POLARIS_SESSION_INSTANCE_ID" ] || return 1
    fi
    return 0
  fi
  [ -f "$session_id_file" ] || return 1
  persisted="$(tr -d '\n' <"$session_id_file")" || return 1
  [ -n "$persisted" ] || return 1
  export POLARIS_SESSION_INSTANCE_ID="$persisted"
}

session_steam_pids() {
  local p pids rc envf env_lines session_id="${POLARIS_SESSION_INSTANCE_ID:-}" proc_root
  [ -n "$session_id" ] || return 2
  proc_root="$(polaris_proc_root)"
  if pids="$(pgrep -x steam 2>/dev/null)"; then
    :
  else
    rc=$?
    [ "$rc" -eq 1 ] || return 2
    pids=""
  fi
  for p in $pids; do
    case "$p" in ''|*[!0-9]*) return 2 ;; esac
    envf="$proc_root/$p/environ"
    if [ ! -r "$envf" ]; then
      [ ! -e "$proc_root/$p" ] && continue
      return 2
    fi
    env_lines="$(tr '\0' '\n' <"$envf" 2>/dev/null)" || return 2
    grep -qxF "POLARIS_SESSION_INSTANCE_ID=$session_id" <<<"$env_lines" || continue
    printf '%s\n' "$p"
  done
}

session_steam_alive() {
  local pids
  pids="$(session_steam_pids)" || return 2
  [ -n "$pids" ]
}

session_steam_absent() {
  local rc
  if session_steam_alive; then
    return 1
  else
    rc=$?
  fi
  [ "$rc" -eq 1 ]
}

kill_session_steam() {
  local pid pids start_time kill_bin="${POLARIS_KILL_BIN:-kill}" session_id="${POLARIS_SESSION_INSTANCE_ID:-}"
  [ -n "$session_id" ] || return 1
  pids="$(session_steam_pids)" || return 1
  while read -r pid; do
    [ -n "$pid" ] || continue
    polaris_process_fields "$pid" || return 1
    start_time="$POLARIS_PROCESS_START_TIME"
    # Bind the numeric PID and environment classification immediately before
    # signaling. Desktop Steam lacks this exact session credential and survives.
    polaris_process_fields "$pid" \
      && [ "$POLARIS_PROCESS_START_TIME" = "$start_time" ] \
      && tr '\0' '\n' <"$(polaris_proc_root)/$pid/environ" 2>/dev/null |
           grep -qxF "POLARIS_SESSION_INSTANCE_ID=$session_id" \
      || return 1
    "$kill_bin" -TERM "$pid" 2>/dev/null || return 1
  done <<<"$pids"
  for _ in $(seq 1 40); do
    session_steam_absent && return 0
    session_steam_alive || [ "$?" -eq 1 ] || return 1
    sleep 0.25
  done
  session_steam_absent
}

# True while a real game process for $1 (Steam appid) is running.
# Steam client / webhelper also inherit SteamAppId — exclude those so
# Big Picture alone does not count as "game still up".
steam_app_game_alive() {
  local appid="$1" pid cmd envf env_lines proc_root session_id="${POLARIS_SESSION_INSTANCE_ID:-}"
  [ -n "$appid" ] && [ -n "$session_id" ] || return 1
  proc_root="$(polaris_proc_root)"
  for envf in "$proc_root"/[0-9]*/environ; do
    pid="${envf#"$proc_root"/}"
    pid="${pid%/environ}"
    case "$pid" in
      *[!0-9]*) continue ;;
    esac
    env_lines="$(tr '\0' '\n' <"$envf" 2>/dev/null)" || continue
    if ! grep -qxF "POLARIS_SESSION_INSTANCE_ID=$session_id" <<<"$env_lines" \
        || ! grep -qx "SteamAppId=${appid}" <<<"$env_lines"; then
      continue
    fi
    cmd="$(tr '\0' ' ' <"$proc_root/$pid/cmdline" 2>/dev/null || true)"
    # Skip empty / Steam client helpers (not the game binary).
    if [ -z "$cmd" ]; then
      continue
    fi
    case "$cmd" in
      *steamwebhelper*|*steam-runtime*|*/steam.sh*|*/ubuntu12_32/steam*|*/ubuntu12_64/steam*|*steam\ -gamepadui*|*steam\ -silent*|*srt-logger*)
        continue
        ;;
    esac
    return 0
  done
  return 1
}

case "${1:-}" in
  start)
    requested_session_id="${POLARIS_SESSION_INSTANCE_ID:-}"
    [ -n "$requested_session_id" ] || {
      echo "polaris-gamescope-session: missing immutable session credential" >&2
      exit 1
    }
    if [ -e "$session_state_file" ] || [ -s "$session_id_file" ] || [ -f "$rt/polaris-gamescope-wsi-nested" ]; then
      echo "polaris-gamescope-session: complete prior session recovery before new launch" >&2
      # Re-exec via bash so a non-executable script path still works when $0 is the .sh file.
      if ! POLARIS_SESSION_INSTANCE_ID='' bash "$0" stop; then
        echo "polaris-gamescope-session: stop recovery failed — forcing clean slate" >&2
        if polaris_validate_marker "$marker" 2>/dev/null; then
          polaris_stop_marked_gamescope "$marker" "$POLARIS_MARKER_ROLE" "$rt" 2>/dev/null || true
        fi
        polaris_reclaim_orphan_gamescope_sockets "$rt" 2>/dev/null || true
        rm -f -- \
          "$marker" \
          "$rt/polaris-gamescope.env" \
          "$rt/polaris-gamescope-force" \
          "$rt/polaris-gamescope-wsi-nested" \
          "$session_state_file" \
          "$session_id_file" \
          "$session_mode_file" \
          "$rt/polaris-gamescope-appid" \
          "$rt/polaris-gamescope-audio-sink" \
          "$rt"/polaris-gamescope-steam-wsi*.log \
          "$rt"/gamescope-0 "$rt"/gamescope-0.lock \
          "$rt"/gamescope-0-ei "$rt"/gamescope-0-ei.lock \
          "$rt"/gamescope-1 "$rt"/gamescope-1.lock \
          "$rt"/gamescope-1-ei "$rt"/gamescope-1-ei.lock
      fi
      POLARIS_SESSION_INSTANCE_ID="$requested_session_id"
      export POLARIS_SESSION_INSTANCE_ID
    fi
    # Soft env hint for nested games (PULSE_SINK / PIPEWIRE_NODE).
    # Polaris claims sink-sunshine-* as the session default (stream capture);
    # do not point children at EasyEffects and do not re-pin sink-inputs here.
    audio_cfg="${POLARIS_CLIENT_AUDIO_CONFIGURATION:-}"
    # Quoted names: shellcheck SC2100 treats unquoted *51 as arithmetic.
    audio_sink="sink-sunshine-surround51"
    audio_map="front-left,front-right,rear-left,rear-right,front-center,lfe"
    audio_desc="Polaris-5.1"
    case "$audio_cfg" in
      *2.0*|*[Ss]tereo*|*2ch*|2)
        audio_sink="sink-sunshine-stereo"
        audio_map="front-left,front-right"
        audio_desc="Polaris-stereo"
        ;;
      *7.1*|7)
        audio_sink="sink-sunshine-surround71"
        audio_map="front-left,front-right,rear-left,rear-right,front-center,lfe,side-left,side-right"
        audio_desc="Polaris-7.1"
        ;;
      *5.1*|5|"")
        audio_sink="sink-sunshine-surround51"
        audio_map="front-left,front-right,rear-left,rear-right,front-center,lfe"
        audio_desc="Polaris-5.1"
        ;;
    esac
    echo "polaris-gamescope-session: audio_cfg=${audio_cfg:-unset} → sink=$audio_sink" >&2

    ensure_null_sink() {
      local name="$1" map="$2" desc="$3"
      if ! pactl list short sinks 2>/dev/null | awk '{print $2}' | grep -qx "$name"; then
        pactl load-module module-null-sink \
          media.class=Audio/Sink \
          sink_name="$name" \
          channel_map="$map" \
          sink_properties="node.description=$desc" \
          >/dev/null 2>&1 || true
      fi
    }
    ensure_null_sink "sink-sunshine-stereo" \
      "front-left,front-right" \
      "Polaris-stereo"
    ensure_null_sink "sink-sunshine-surround51" \
      "front-left,front-right,rear-left,rear-right,front-center,lfe" \
      "Polaris-5.1"
    ensure_null_sink "sink-sunshine-surround71" \
      "front-left,front-right,rear-left,rear-right,front-center,lfe,side-left,side-right" \
      "Polaris-7.1"
    ensure_null_sink "$audio_sink" "$audio_map" "$audio_desc"

    printf '%s\n' "$audio_sink" >"$rt/polaris-gamescope-audio-sink"
    echo "polaris-gamescope-session: audio env sink=$audio_sink (host claims default; soft PULSE_SINK hint only)" >&2

    # True SDR when POLARIS_CLIENT_HDR is false: force file 0 + no --hdr-enabled.
    # Hybrid (HDR gamescope + SDR encode) is the iPhone chroma disaster; polaris 06 also syncs force.
    want_hdr=0
    if [ "$allow_client_hdr" = 1 ]; then
      case "${POLARIS_CLIENT_HDR:-false}" in
        true|TRUE|1|yes|YES) want_hdr=1 ;;
      esac
    fi
    # Nested WSI by default: Steam is gamescope primary child.
    # Polaris prep-cmd strips leading "env FOO=1 …", so WSI cannot be
    # toggled via apps.json env — default on; set POLARIS_GAMESCOPE_WSI=0
    # only for explicit attach experiments.
    # Attach-to-idle often blacks out direct applaunch (no focus/paint).
    want_wsi=1
    case "${POLARIS_GAMESCOPE_WSI:-1}" in
      false|FALSE|0|no|NO) want_wsi=0 ;;
    esac
    if [ "$want_wsi" = 1 ]; then
      publish_session_mode nested || {
        echo "polaris-gamescope-session: could not publish durable nested recovery mode" >&2
        exit 1
      }
    else
      publish_session_mode attach || {
        echo "polaris-gamescope-session: could not publish durable attach recovery mode" >&2
        exit 1
      }
    fi

    # Nested WSI: gamescope --steam with Steam as primary child.
    # Plain "-silent -applaunch" creates a WSI surface but often no
    # PipeWire paint (black stream). BP works because -gamepadui owns
    # focus; direct titles use the same UI + applaunch.
    steam_launch=(steam -gamepadui)
    if [ -n "${2:-}" ]; then
      case "$2" in
        *[!0-9]*)
          echo "polaris-gamescope-session: appid must be numeric, got: $2" >&2
          exit 2
          ;;
      esac
      # Direct library title: same HDR gate as Big Picture
      # (POLARIS_CLIENT_HDR / client profile). No force-SDR.
      steam_launch=(steam -gamepadui -applaunch "$2")
      # wait monitors this: game exit → steam -shutdown → stream ends
      # (otherwise gamepadui returns to BP and the stream never stops).
      printf '%s\n' "$2" >"$rt/polaris-gamescope-appid"
      echo "polaris-gamescope-session: Steam -gamepadui -applaunch $2 (nested WSI, hdr=$want_hdr, exit-on-game-close)" >&2
    else
      rm -f "$rt/polaris-gamescope-appid"
    fi

    prev_force="$(tr -d '[:space:]' <"$rt/polaris-gamescope-force" 2>/dev/null || true)"
    printf '%s\n' "$want_hdr" >"$rt/polaris-gamescope-force"
    if [ "$want_hdr" = 1 ]; then
      echo "polaris-gamescope-session: HDR session (force=1, ${POLARIS_GAMESCOPE_BIN:-gamescope} --hdr-enabled when started)" >&2
    else
      echo "polaris-gamescope-session: true SDR (force=0, no --hdr-enabled; not hybrid PQ+SDR)" >&2
    fi

    if [ "$want_wsi" = 1 ]; then
      # --- Nested WSI path (games as gamescope child) ---
      echo "polaris-gamescope-session: WSI nested mode — Steam is ${POLARIS_GAMESCOPE_BIN:-gamescope} primary child" >&2
      # Publish a fenced transition before masking/stopping idle ownership so no
      # readiness actor can interpret the destructive handoff as an unowned gap.
      publish_nested_claim transition absent || {
        echo "polaris-gamescope-session: another ownership transition is already active" >&2
        exit 1
      }
      # Runtime-mask so polaris Wants= / portal-gamescope Wants= cannot respawn
      # idle while nested needs exclusive gamescope-0 (portal is hard-wired).
      # Use user.control — plain mask --runtime loses to Hjem ~/.config units.
      polaris_mask_idle_unit_runtime
      # Stop only the exact marked owner of gamescope-0. Unknown sockets fail
      # closed instead of risking another user's compositor.
      if polaris_validate_marker "$marker"; then
        case "$POLARIS_MARKER_ROLE" in
          idle|nested)
            polaris_stop_marked_gamescope "$marker" "$POLARIS_MARKER_ROLE" "$rt" || {
              echo "polaris-gamescope-session: marked $POLARIS_MARKER_ROLE owner did not stop" >&2
              exit 1
            }
            ;;
          *)
            echo "polaris-gamescope-session: refusing to replace marked $POLARIS_MARKER_ROLE owner" >&2
            exit 1
            ;;
        esac
      elif ! polaris_reclaim_orphan_gamescope_sockets "$rt"; then
        echo "polaris-gamescope-session: refusing unowned gamescope socket cleanup" >&2
        exit 1
      fi
      for _ in $(seq 1 "${POLARIS_IDLE_WAIT_STEPS:-100}"); do
        [ ! -S "$rt/gamescope-0" ] && [ ! -S "$rt/gamescope-1" ] && break
        sleep 0.1
      done
      if [ -S "$rt/gamescope-0" ] || [ -S "$rt/gamescope-1" ]; then
        echo "polaris-gamescope-session: headless ${POLARIS_GAMESCOPE_BIN:-gamescope} socket still held after stop" >&2
        exit 1
      fi

      # shellcheck source=/dev/null

      prefer_vk=()
      if [ -n "${POLARIS_GAMESCOPE_PREFER_VK:-}" ]; then
        prefer_vk=(--prefer-vk-device "$POLARIS_GAMESCOPE_PREFER_VK")
      fi
      hdr_flags=()
      if [ "$want_hdr" = 1 ]; then
        # Nested: --hdr-enabled only (no --hdr-debug-force-*).
        # WSI can still create HDR10 swapchains; PW spa 81 may need force later.
        hdr_flags=(
          --hdr-enabled
          --sdr-gamut-wideness 0.000000
          --hdr-sdr-content-nits 203
        )
        echo "polaris-gamescope-session: nested HDR (enabled, no debug-force-*)" >&2
      fi
      # Always --steam on nested WSI (input + multi-xwayland Steam integration).
      steam_flags=(--steam)

      # gamescope itself setenv(ENABLE_GAMESCOPE_WSI,1) for nested children.
      # Do NOT set ENABLE_HDR_WSI (separate VK_hdr_layer; can break Gamescope WSI).
      # Do NOT pass host WAYLAND_DISPLAY into children: FROG WSI only creates
      # Gamescope surfaces when GAMESCOPE_WAYLAND_DISPLAY is set and does not
      # conflict with another non-empty WAYLAND_DISPLAY (KWin wayland-0).
      # Desktop nested AC6 success had WAYLAND_DISPLAY unset on the game.
      # No GAMESCOPE_WSI_FORCE_BYPASS: bypass kept swapchains non-HDR.
      child_env=(
        # Stream capture only: leave system default + EasyEffects alone.
        PULSE_SINK="$audio_sink"
        PIPEWIRE_NODE="$audio_sink"
        POLARIS_SESSION_AUDIO_SINK="$audio_sink"
        STEAM_MULTIPLE_XWAYLANDS=1
        QT_QPA_PLATFORM=xcb
        DISABLE_HDR_WSI=1
      )
      if [ "$want_hdr" = 1 ]; then
        child_env+=(STEAM_GAMESCOPE_HDR_SUPPORTED=1 DXVK_HDR=1)
      fi

      steam_log="$(mktemp "$rt/polaris-gamescope-steam-wsi.XXXXXX.log")"
      # gamescope runs Steam as primary child; portal still captures gamescope-0.
      # Omit --expose-wayland so children stay on XWayland + GAMESCOPE_WAYLAND_DISPLAY
      # (WSI X11 path). Portal uses compositor socket gamescope-0, not child WAYLAND.
      # env -u: drop host KWin Wayland/DISPLAY inherited from polaris user session.
      echo "polaris-gamescope-session: nested geometry ${gs_width}x${gs_height}@${gs_refresh}" >&2
      # Claim ownership before launch so marker-capture failure remains
      # recoverable without an unsafe numeric PGID fallback.
      publish_nested_claim nested transition || {
        echo "polaris-gamescope-session: ownership transition changed before nested launch" >&2
        exit 1
      }
      setsid env -u WAYLAND_DISPLAY -u DISPLAY -u ENABLE_HDR_WSI \
        "${child_env[@]}" "${POLARIS_GAMESCOPE_BIN:-gamescope}" \
        --backend headless \
        "${steam_flags[@]}" \
        --xwayland-count 2 \
        "${prefer_vk[@]}" \
        "${hdr_flags[@]}" \
        -W "$gs_width" -H "$gs_height" -r "$gs_refresh" \
        -w "$gs_width" -h "$gs_height" \
        -- "${steam_launch[@]}" \
        >"$steam_log" 2>&1 &
      nested_launch_pid=$!

      # Resolve the real headless gamescope PID. setsid/env/wrapProgram may leave
      # $! as a launcher; preserve that PID as the private PGID/SID and pin the
      # separately discovered compositor generation in the marker.
      resolve_nested_gamescope_pid() {
        local root="$1" p
        if polaris_headless_gamescope_pid "$root" 2>/dev/null; then
          printf '%s\n' "$root"
          return 0
        fi
        for p in $(pgrep -P "$root" 2>/dev/null || true); do
          if polaris_headless_gamescope_pid "$p" 2>/dev/null; then
            printf '%s\n' "$p"
            return 0
          fi
        done
        return 1
      }

      nested_marked=0
      nested_gamescope_pid=""
      for _ in $(seq 1 150); do
        gs_pid="$(resolve_nested_gamescope_pid "$nested_launch_pid" 2>/dev/null || true)"
        if [ -n "${gs_pid:-}" ] \
            && polaris_write_marker_for_pid "$marker" "$gs_pid" nested \
            && polaris_validate_marker "$marker" nested \
            && [ "$POLARIS_PROCESS_PGID" = "$nested_launch_pid" ] \
            && [ "$POLARIS_PROCESS_SESSION_ID" = "$nested_launch_pid" ]; then
          nested_marked=1
          nested_gamescope_pid="$gs_pid"
          break
        fi
        kill -0 "$nested_launch_pid" 2>/dev/null || break
        sleep 0.02
      done
      if [ "$nested_marked" != 1 ]; then
        echo "polaris-gamescope-session: failed to record an exact nested gamescope generation in its private setsid group" >&2
        if [ -f "$marker" ] && polaris_validate_marker "$marker" nested; then
          polaris_stop_marked_gamescope "$marker" nested "$rt" || true
        fi
        # Without an exact marker plus PGID/SID proof there is no safe numeric
        # fallback. Preserve the recovery claim and let service/cgroup teardown
        # contain an unclassified launch instead of risking PID/PGID reuse.
        exit 1
      fi

      ready=0
      for _ in $(seq 1 300); do
        if polaris_write_runtime_env "$marker" gamescope-0 nested "$rt"; then
          ready=1
          break
        fi
        kill -0 "$nested_gamescope_pid" 2>/dev/null || break
        sleep 0.1
      done
      if [ "$ready" != 1 ]; then
        echo "polaris-gamescope-session: owned nested gamescope-0/Xwayland not ready — see $steam_log" >&2
        polaris_stop_marked_gamescope "$marker" nested "$rt" || true
        exit 1
      fi
      publish_nested_claim nested nested || {
        echo "polaris-gamescope-session: nested ownership changed before portal rebind" >&2
        exit 1
      }
      # Portal + polaris-gamescope.env assume gamescope-0. Bail if we lost the race.
      if rg -q "wayland display 'gamescope-1'" "$steam_log" 2>/dev/null; then
        echo "polaris-gamescope-session: nested bound gamescope-1 (portal captures gamescope-0) — see $steam_log" >&2
        exit 1
      fi
      # Rebind private portal backend to this gamescope generation. Without this,
      # xdg-desktop-portal-gamescope keeps the prior (idle) wayland connection and
      # Start fails with "gamescope stream not available: failed to connect to
      # wayland socket" when the compositor was replaced under it.
      # Restart only the gamescope impl — not the full portal frontend — so we
      # avoid the udev/controller race that killing xdg-desktop-portal caused.
      # polaris must Wants= (not Requires=) this unit so rebind never cascade-stops it.
      systemctl --user restart polaris-portal-gamescope.service 2>/dev/null || true
      portal_bus="unix:path=$rt/polaris-portal/bus"
      portal_ready=0
      for _ in $(seq 1 80); do
        if busctl --address="$portal_bus" --no-pager \
            status org.freedesktop.impl.portal.desktop.gamescope >/dev/null 2>&1; then
          portal_ready=1
          break
        fi
        sleep 0.1
      done
      if [ "$portal_ready" != 1 ]; then
        echo "polaris-gamescope-session: portal-gamescope did not rebind after nested start" >&2
        # Non-fatal: stream may still gamescopegrab the PW node.
      else
        echo "polaris-gamescope-session: portal-gamescope rebound to nested gamescope-0" >&2
      fi
      # Brief settle so Steam's first controller udev events land after portal is up.
      sleep 1
      echo "polaris-gamescope-session: nested ${POLARIS_GAMESCOPE_BIN:-gamescope} ready; WSI logs → $steam_log and ~/.local/share/Steam/logs/console-linux.txt" >&2
      echo "polaris-gamescope-session: pass = 'Creating Gamescope surface' + 'hdr formats exposed: true'" >&2
    else
      # --- Attach path (known-good stream, no WSI) ---
      if [ -f "$rt/polaris-gamescope-wsi-nested" ]; then
        echo "polaris-gamescope-session: nested recovery claim changed during attach setup" >&2
        exit 1
      elif ! systemctl --user is-active --quiet polaris-gamescope-idle.service 2>/dev/null; then
        systemctl --user start polaris-gamescope-idle.service || true
      elif [ "${prev_force:-}" != "$want_hdr" ]; then
        echo "polaris-gamescope-session: attach force=$want_hdr (was ${prev_force:-unset}); restart idle" >&2
        systemctl --user restart polaris-gamescope-idle.service || true
      fi

      attach_ready=0
      for _ in $(seq 1 300); do
        if polaris_write_runtime_env "$marker" gamescope-0 idle "$rt"; then
          attach_ready=1
          break
        fi
        sleep 0.1
      done
      if [ "$attach_ready" != 1 ]; then
        echo "polaris-gamescope-session: validated idle gamescope-0/Xwayland not ready" >&2
        exit 1
      fi

      # shellcheck disable=SC1091
      . "$rt/polaris-gamescope.env"
      hdr_steam_env=()
      if [ "$want_hdr" = 1 ]; then
        hdr_steam_env=(STEAM_GAMESCOPE_HDR_SUPPORTED=1 DXVK_HDR=1)
        echo "polaris-gamescope-session: attach Steam HDR env on (no WSI)" >&2
      else
        echo "polaris-gamescope-session: attach Steam HDR env off" >&2
      fi
      steam_log="$(mktemp "$rt/polaris-gamescope-steam-attach.XXXXXX.log")"
      # Force X11 on gamescope XWayland for attach. Polaris inherits the
      # host KDE session (WAYLAND_DISPLAY=wayland-0, GDK_BACKEND=wayland);
      # native titles (e.g. bg3) then paint on KWin while the portal
      # captures empty gamescope → black stream (measured 2026-07-14).
      # Do not pass host WAYLAND_DISPLAY; do not enable FROG WSI here.
      echo "polaris-gamescope-session: attach Steam on DISPLAY=${DISPLAY:-:1} (X11 only, host Wayland stripped)" >&2
      setsid -f env \
        -u WAYLAND_DISPLAY \
        -u CLUTTER_BACKEND \
        -u ELECTRON_OZONE_PLATFORM_HINT \
        -u MOZ_ENABLE_WAYLAND \
        -u ENABLE_GAMESCOPE_WSI \
        -u ENABLE_HDR_WSI \
        DISPLAY="${DISPLAY:-:1}" \
        GAMESCOPE_WAYLAND_DISPLAY=gamescope-0 \
        PULSE_SINK="$audio_sink" \
        PIPEWIRE_NODE="$audio_sink" \
        POLARIS_SESSION_AUDIO_SINK="$audio_sink" \
        STEAM_MULTIPLE_XWAYLANDS=1 \
        QT_QPA_PLATFORM=xcb \
        GDK_BACKEND=x11 \
        SDL_VIDEODRIVER=x11 \
        XDG_SESSION_TYPE=x11 \
        "${hdr_steam_env[@]}" \
        setpriv --inh-caps=-all --ambient-caps=-all -- \
        "${steam_launch[@]}" >"$steam_log" 2>&1
      sleep 2
    fi
    ;;
  wait)
    # Hold the Moonlight session open until Polaris SIGTERMs this process
    # (process-group kill on disconnect / app stop). Do NOT exit early on
    # Steam probe failures — that raced portal attach and tore nested
    # gamescope down within ~50ms (Response code 2).
    load_session_instance_id || {
      echo "polaris-gamescope-session: missing or mismatched session credential during wait" >&2
      exit 1
    }
    appid=""
    if [ -f "$rt/polaris-gamescope-appid" ]; then
      appid="$(tr -d '[:space:]' <"$rt/polaris-gamescope-appid" || true)"
    fi
    echo "polaris-gamescope-session: wait holding stream (appid=${appid:-none}, credential=$POLARIS_SESSION_INSTANCE_ID)" >&2
    trap 'echo "polaris-gamescope-session: wait got signal — releasing" >&2; exit 0' TERM INT
    seen=0
    gone=0
    while :; do
      if [ -n "$appid" ] && steam_app_game_alive "$appid"; then
        if [ "$seen" = 0 ]; then
          echo "polaris-gamescope-session: game process for appid=$appid seen" >&2
        fi
        seen=1
        gone=0
      elif [ "$seen" = 1 ]; then
        gone=$((gone + 1))
        # ~15s debounce: launchers/anti-cheat may respawn; Steam may be slow.
        if [ "$gone" -ge 30 ]; then
          echo "polaris-gamescope-session: game appid=$appid exited — releasing wait (end stream)" >&2
          kill_session_steam 2>/dev/null || true
          exit 0
        fi
      fi
      # Nested compositor gone after marker published → release.
      if [ -f "$marker" ]; then
        if polaris_validate_marker "$marker" nested; then
          :
        else
          if [ ! -e "$rt/gamescope-0" ]; then
            echo "polaris-gamescope-session: nested gamescope gone — releasing wait" >&2
            exit 0
          fi
        fi
      fi
      sleep 0.5
    done
    ;;
  stop)
    load_session_instance_id || {
      echo "polaris-gamescope-session: missing or mismatched session credential during stop" >&2
      exit 1
    }
    if [ -n "${POLARIS_PERSISTED_SESSION_MODE:-}" ]; then
      session_mode="$POLARIS_PERSISTED_SESSION_MODE"
    else
      [ -f "$session_mode_file" ] || {
        echo "polaris-gamescope-session: durable session mode missing; retaining credential" >&2
        exit 1
      }
      session_mode="$(tr -d '[:space:]' <"$session_mode_file")"
    fi
    case "$session_mode" in attach|nested) ;; *)
      echo "polaris-gamescope-session: invalid durable session mode; retaining credential" >&2
      exit 1
      ;;
    esac
    rm -f "$rt/polaris-gamescope-appid" "$rt/polaris-gamescope-audio-sink" "$rt/polaris-gamescope-audio-skip-pin"
    # Keep null sinks loaded (permanent capture targets).
    rm -f "$rt/polaris-gamescope-sink-module"
    # Legacy flags from builds that killed EasyEffects / hijacked default.
    rm -f "$rt/polaris-gamescope-prev-default-sink" "$rt/polaris-gamescope-easyeffects-units"
    nested_claim="$rt/polaris-gamescope-wsi-nested"
    if [ "$session_mode" = nested ] && [ ! -e "$nested_claim" ]; then
      recover_missing_nested_claim || {
        echo "polaris-gamescope-session: claimless nested credential is not provably pre-transition; retaining it" >&2
        exit 1
      }
    fi
    if [ -f "$nested_claim" ]; then
      [ "$session_mode" = nested ] || {
        echo "polaris-gamescope-session: nested claim conflicts with durable attach mode" >&2
        exit 1
      }
      claim_state="$(tr -d '[:space:]' <"$nested_claim")"
      case "$claim_state" in
        transition)
          if polaris_validate_marker "$marker" nested; then
            echo "polaris-gamescope-session: transition claim unexpectedly owns a nested generation" >&2
            exit 1
          fi
          if ! polaris_validate_marker "$marker" idle \
              && ! polaris_reclaim_orphan_gamescope_sockets "$rt"; then
            echo "polaris-gamescope-session: transition recovery cannot prove idle-or-orphan ownership" >&2
            exit 1
          fi
          publish_nested_claim restore-idle "$claim_state" || {
            echo "polaris-gamescope-session: transition claim changed before idle restoration" >&2
            exit 1
          }
          ;;
        1|nested)
          echo "polaris-gamescope-session: tearing down marked nested WSI gamescope" >&2
          if polaris_validate_marker "$marker" nested; then
            if ! polaris_stop_marked_gamescope "$marker" nested "$rt"; then
              echo "polaris-gamescope-session: nested owner did not reach terminal state; retaining recovery claim" >&2
              exit 1
            fi
          else
            # Nested generation already dead (host hang dump, gamescope crash, or
            # kill raced marker validation). Only advance when sockets are absent
            # or reclaimable — live foreign ownership must keep the durable claim
            # so we never steal another compositor's gamescope-0.
            if ! polaris_reclaim_orphan_gamescope_sockets "$rt"; then
              echo "polaris-gamescope-session: nested launch lacks a valid exact marker; retaining recovery claim" >&2
              exit 1
            fi
            echo "polaris-gamescope-session: nested marker invalid/dead; sockets orphan or absent — restoring idle" >&2
          fi
          if ! kill_session_steam || ! session_steam_absent; then
            echo "polaris-gamescope-session: exact-session Steam did not reach terminal state; retaining recovery claim" >&2
            exit 1
          fi
          publish_nested_claim restore-idle "$claim_state" || {
            echo "polaris-gamescope-session: transition claim changed before idle restoration" >&2
            exit 1
          }
          ;;
        restore-idle)
          ;;
        *)
          echo "polaris-gamescope-session: invalid nested recovery state '$claim_state'" >&2
          exit 1
          ;;
      esac

      # Ordered handoff: idle must own gamescope-0 and publish an exact runtime
      # environment before the portal may rebind. The restore-idle claim stays
      # durable across every failure so a retry never repeats nested signaling.
      printf '0\n' >"$rt/polaris-gamescope-force"
      polaris_unmask_idle_unit_runtime
      systemctl --user reset-failed polaris-gamescope-idle.service 2>/dev/null || true
      if ! systemctl --user start polaris-gamescope-idle.service 2>/dev/null; then
        echo "polaris-gamescope-session: failed to start idle gamescope; retaining restore-idle claim" >&2
        exit 1
      fi
      idle_ready=0
      for _ in $(seq 1 "${POLARIS_IDLE_WAIT_STEPS:-100}"); do
        if polaris_validate_marker "$marker" idle \
            && polaris_marker_owns_socket "$marker" "$rt/gamescope-0" idle \
            && polaris_write_runtime_env "$marker" gamescope-0 idle "$rt" 2>/dev/null; then
          idle_ready=1
          break
        fi
        sleep 0.1
      done
      if [ "$idle_ready" != 1 ]; then
        echo "polaris-gamescope-session: idle gamescope-0 not ready; retaining restore-idle claim" >&2
        exit 1
      fi
      exec 8>>"$rt/polaris-gamescope.lock" || exit 1
      "${POLARIS_FLOCK_BIN:-flock}" -x 8 || exit 1
      export POLARIS_GAMESCOPE_LOCK_HELD=1
      if ! {
        [ -f "$nested_claim" ] \
          && [ "$(tr -d '[:space:]' <"$nested_claim")" = restore-idle ] \
          && polaris_validate_marker "$marker" idle \
          && polaris_marker_owns_socket "$marker" "$rt/gamescope-0" idle \
          && polaris_write_runtime_env "$marker" gamescope-0 idle "$rt"
      }; then
        echo "polaris-gamescope-session: idle ownership changed before portal handoff" >&2
        exit 1
      fi
      echo "polaris-gamescope-session: idle gamescope restored after nested stop" >&2

      if ! systemctl --user restart polaris-portal-gamescope.service 2>/dev/null; then
        echo "polaris-gamescope-session: portal restart failed; retaining restore-idle claim" >&2
        exit 1
      fi
      portal_bus="unix:path=$rt/polaris-portal/bus"
      portal_ready=0
      for _ in $(seq 1 "${POLARIS_PORTAL_WAIT_STEPS:-50}"); do
        if busctl --address="$portal_bus" --no-pager \
            status org.freedesktop.impl.portal.desktop.gamescope >/dev/null 2>&1; then
          portal_ready=1
          break
        fi
        sleep 0.1
      done
      if [ "$portal_ready" != 1 ]; then
        echo "polaris-gamescope-session: portal did not bind idle generation; retaining restore-idle claim" >&2
        exit 1
      fi
      if ! {
        polaris_validate_marker "$marker" idle \
          && polaris_marker_owns_socket "$marker" "$rt/gamescope-0" idle \
          && [ -f "$nested_claim" ] \
          && [ "$(tr -d '[:space:]' <"$nested_claim")" = restore-idle ]
      }; then
        echo "polaris-gamescope-session: ownership changed during portal readiness" >&2
        exit 1
      fi
      rm -f -- "$nested_claim"
      "${POLARIS_FLOCK_BIN:-flock}" -u 8
      export POLARIS_GAMESCOPE_LOCK_HELD=0
    else
      [ "$session_mode" = attach ] || {
        echo "polaris-gamescope-session: nested mode lost its recovery claim; retaining credential" >&2
        exit 1
      }
      kill_session_steam || {
        echo "polaris-gamescope-session: attach recovery could not terminate exact-session Steam" >&2
        exit 1
      }
      session_steam_absent || {
        echo "polaris-gamescope-session: attach generation still alive; retaining credential" >&2
        exit 1
      }
      if [ -f "$rt/polaris-gamescope-force" ] \
          && [ "$(tr -d '[:space:]' <"$rt/polaris-gamescope-force")" = "1" ]; then
        printf '0\n' >"$rt/polaris-gamescope-force"
        systemctl --user restart polaris-gamescope-idle.service || true
      fi
    fi
    rm -f "$session_state_file" "$session_mode_file" "$session_id_file"
    ;;
  *)
    echo "usage: polaris-gamescope-session start [steam_appid]|wait|stop" >&2
    exit 2
    ;;
esac
