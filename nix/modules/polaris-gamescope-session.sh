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

session_steam_pids() {
  local p h
  for p in $(pgrep -x steam 2>/dev/null || true); do
    h="$(tr '\0' '\n' <"/proc/$p/environ" 2>/dev/null | sed -n 's/^HOME=//p' | head -n1 || true)"
    if [ "$h" = "${HOME}" ]; then
      printf '%s\n' "$p"
    fi
  done
}
session_steam_alive() { [ -n "$(session_steam_pids)" ]; }

kill_session_steam() {
  if session_steam_alive; then
    steam -shutdown 2>/dev/null || true
    sleep 1
    pkill -TERM -x steam 2>/dev/null || true
    for _ in $(seq 1 40); do
      session_steam_alive || break
      sleep 0.25
    done
  fi
}

# True while a real game process for $1 (Steam appid) is running.
# Steam client / webhelper also inherit SteamAppId — exclude those so
# Big Picture alone does not count as "game still up".
steam_app_game_alive() {
  local appid="$1" pid cmd envf
  [ -n "$appid" ] || return 1
  for envf in /proc/[0-9]*/environ; do
    pid="${envf#/proc/}"
    pid="${pid%/environ}"
    case "$pid" in
      *[!0-9]*) continue ;;
    esac
    if ! tr '\0' '\n' <"$envf" 2>/dev/null | grep -qx "SteamAppId=${appid}"; then
      continue
    fi
    cmd="$(tr '\0' ' ' <"/proc/$pid/cmdline" 2>/dev/null || true)"
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
    # Soft env hint for nested games (PULSE_SINK / PIPEWIRE_NODE).
    # Polaris itself picks the capture sink: EasyEffects when it is the
    # host default (FMOD target.object sticks there); otherwise virtual
    # sink-sunshine-*. Do NOT re-pin streams here — that thrash fought
    # WirePlumber and killed main-menu audio.
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
    # Virtual sinks for non-EE isolation (Polaris also creates them).
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

    # Prefer host processing sink for child env when EE/JamesDSP is default
    # (matches Polaris capture; avoids PULSE_SINK pointing at empty null).
    host_default="$(pactl get-default-sink 2>/dev/null || true)"
    case "$host_default" in
      *easyeffects*|*EasyEffects*|*jamesdsp*|*JamesDSP*|*pulseeffects*|*PulseEffects*)
        audio_sink="$host_default"
        echo "polaris-gamescope-session: host default is processing sink [$host_default] — child env uses it" >&2
        ;;
    esac

    printf '%s\n' "$audio_sink" >"$rt/polaris-gamescope-audio-sink"
    echo "polaris-gamescope-session: audio env sink=$audio_sink (no re-pin; EasyEffects left running)" >&2

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

    kill_session_steam

    if [ "$want_wsi" = 1 ]; then
      # --- Nested WSI path (games as gamescope child) ---
      echo "polaris-gamescope-session: WSI nested mode — Steam is ${POLARIS_GAMESCOPE_BIN:-gamescope} primary child" >&2
      # Runtime-mask so polaris Wants= / on-failure cannot respawn idle
      # while nested needs exclusive gamescope-0 (portal is hard-wired).
      systemctl --user mask --runtime polaris-gamescope-idle.service 2>/dev/null || true
      systemctl --user stop polaris-gamescope-idle.service 2>/dev/null || true
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
      for _ in $(seq 1 100); do
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
      nested_pid=$!

      # Resolve the real headless gamescope PID. setsid/env/wrapProgram may leave
      # $! as a short-lived parent; marker must pin the compositor generation.
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
      for _ in $(seq 1 150); do
        gs_pid="$(resolve_nested_gamescope_pid "$nested_pid" 2>/dev/null || true)"
        if [ -n "${gs_pid:-}" ] && polaris_write_marker_for_pid "$marker" "$gs_pid" nested; then
          nested_marked=1
          nested_pid="$gs_pid"
          break
        fi
        kill -0 "$nested_pid" 2>/dev/null || break
        sleep 0.02
      done
      if [ "$nested_marked" != 1 ]; then
        echo "polaris-gamescope-session: failed to record exact nested gamescope generation" >&2
        kill "$nested_pid" 2>/dev/null || true
        wait "$nested_pid" 2>/dev/null || true
        exit 1
      fi

      ready=0
      for _ in $(seq 1 300); do
        if polaris_write_runtime_env "$marker" gamescope-0 nested "$rt"; then
          ready=1
          break
        fi
        kill -0 "$nested_pid" 2>/dev/null || break
        sleep 0.1
      done
      if [ "$ready" != 1 ]; then
        echo "polaris-gamescope-session: owned nested gamescope-0/Xwayland not ready — see $steam_log" >&2
        polaris_stop_marked_gamescope "$marker" nested "$rt" || true
        exit 1
      fi
      printf '1\n' >"$rt/polaris-gamescope-wsi-nested"
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
        if polaris_validate_marker "$marker" nested; then
          polaris_stop_marked_gamescope "$marker" nested "$rt" || {
            echo "polaris-gamescope-session: nested owner did not stop" >&2
            exit 1
          }
        elif ! polaris_reclaim_orphan_gamescope_sockets "$rt"; then
          echo "polaris-gamescope-session: refusing to clean unowned nested sockets" >&2
          exit 1
        fi
        rm -f "$rt/polaris-gamescope-wsi-nested"
        systemctl --user unmask --runtime polaris-gamescope-idle.service 2>/dev/null || true
        systemctl --user start polaris-gamescope-idle.service || true
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
    for _ in $(seq 1 60); do
      session_steam_alive && break
      sleep 0.5
    done
    appid="$(tr -d '[:space:]' <"$rt/polaris-gamescope-appid" 2>/dev/null || true)"
    if [ -n "$appid" ]; then
      # Direct library launch: end stream when the game exits, not when
      # the user leaves Big Picture. gamepadui keeps Steam alive after
      # the title quits → without this, Moonlight stays on BP forever.
      echo "polaris-gamescope-session: wait appid=$appid (exit-on-game-close)" >&2
      seen=0
      gone=0
      while :; do
        if ! session_steam_alive; then
          # Steam already gone (user quit BP / crash).
          break
        fi
        if steam_app_game_alive "$appid"; then
          if [ "$seen" = 0 ]; then
            echo "polaris-gamescope-session: game process for appid=$appid seen" >&2
          fi
          seen=1
          gone=0
        elif [ "$seen" = 1 ]; then
          gone=$((gone + 1))
          # ~3s debounce: launchers/anti-cheat may respawn once.
          if [ "$gone" -ge 6 ]; then
            echo "polaris-gamescope-session: game appid=$appid exited — shutting down Steam (end stream)" >&2
            kill_session_steam
            break
          fi
        fi
        sleep 0.5
      done
    else
      # Big Picture / no appid: stream lives until Steam exits.
      gone=0
      while :; do
        if session_steam_alive; then
          gone=0
        else
          gone=$((gone + 1))
          [ "$gone" -ge 6 ] && break
        fi
        sleep 0.5
      done
    fi
    ;;
  stop)
    kill_session_steam
    rm -f "$rt/polaris-gamescope-appid" "$rt/polaris-gamescope-audio-sink" "$rt/polaris-gamescope-audio-skip-pin"
    # Keep null sinks loaded (permanent capture targets).
    rm -f "$rt/polaris-gamescope-sink-module"
    # Legacy flags from builds that killed EasyEffects / hijacked default.
    rm -f "$rt/polaris-gamescope-prev-default-sink" "$rt/polaris-gamescope-easyeffects-units"
    if [ -f "$rt/polaris-gamescope-wsi-nested" ]; then
      echo "polaris-gamescope-session: tearing down marked nested WSI gamescope" >&2
      if polaris_validate_marker "$marker" nested; then
        polaris_stop_marked_gamescope "$marker" nested "$rt" ||
          echo "polaris-gamescope-session: nested owner did not reach terminal state" >&2
      elif ! polaris_reclaim_orphan_gamescope_sockets "$rt"; then
        echo "polaris-gamescope-session: leaving live unowned gamescope sockets untouched" >&2
      fi
      rm -f "$rt/polaris-gamescope-wsi-nested"
      printf '0\n' >"$rt/polaris-gamescope-force"
      # Ordered handoff: idle must own gamescope-0 before portal capture can
      # reconnect. Otherwise portal-gamescope hits "failed to connect to wayland
      # socket" (Response code 2) during the gap.
      systemctl --user unmask --runtime polaris-gamescope-idle.service 2>/dev/null || true
      systemctl --user reset-failed polaris-gamescope-idle.service 2>/dev/null || true
      systemctl --user start polaris-gamescope-idle.service 2>/dev/null || true
      idle_ready=0
      for _ in $(seq 1 100); do
        if polaris_validate_marker "$marker" idle &&
           polaris_marker_owns_socket "$marker" "$rt/gamescope-0" idle; then
          polaris_write_runtime_env "$marker" gamescope-0 idle "$rt" 2>/dev/null || true
          idle_ready=1
          break
        fi
        sleep 0.1
      done
      if [ "$idle_ready" != 1 ]; then
        echo "polaris-gamescope-session: idle gamescope-0 not ready after nested stop" >&2
      else
        echo "polaris-gamescope-session: idle gamescope restored after nested stop" >&2
      fi
      # Rebind portal backend to idle generation for the next stream.
      systemctl --user restart polaris-portal-gamescope.service 2>/dev/null || true
      portal_bus="unix:path=$rt/polaris-portal/bus"
      for _ in $(seq 1 50); do
        busctl --address="$portal_bus" --no-pager \
          status org.freedesktop.impl.portal.desktop.gamescope >/dev/null 2>&1 && break
        sleep 0.1
      done
    elif [ -f "$rt/polaris-gamescope-force" ] && [ "$(tr -d '[:space:]' <"$rt/polaris-gamescope-force")" = "1" ]; then
      printf '0\n' >"$rt/polaris-gamescope-force"
      systemctl --user restart polaris-gamescope-idle.service || true
    fi
    ;;
  *)
    echo "usage: polaris-gamescope-session start [steam_appid]|wait|stop" >&2
    exit 2
    ;;
esac

