#!/usr/bin/env bash
# Idle headless gamescope for portal / gamescopegrab capture (non-NixOS).
set -euo pipefail
umask 077

if ! declare -F polaris_validate_marker >/dev/null 2>&1; then
  runtime_lib="${POLARIS_GAMESCOPE_RUNTIME_LIB:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/polaris-gamescope-runtime-lib.sh}"
  # shellcheck source=/dev/null
  . "$runtime_lib"
fi

width="${POLARIS_HDR_WIDTH:-3840}"
height="${POLARIS_HDR_HEIGHT:-2160}"
refresh="${POLARIS_HDR_REFRESH:-120}"
rt="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
gs="${POLARIS_GAMESCOPE_BIN:-gamescope}"
marker="$rt/polaris-gamescope.pid"
child=""
child_start=""

command -v "$gs" >/dev/null 2>&1 || {
  echo "polaris-gamescope-idle: gamescope not found (set POLARIS_GAMESCOPE_BIN)" >&2
  exit 1
}

cleanup() {
  if [ -n "$child" ] && polaris_read_marker "$marker" &&
     [ "$POLARIS_MARKER_PID" = "$child" ] &&
     [ "$POLARIS_MARKER_START_TIME" = "$child_start" ] &&
     [ "$POLARIS_MARKER_ROLE" = idle ]; then
    polaris_stop_marked_gamescope "$marker" idle "$rt" || true
  fi
}
trap cleanup EXIT
trap 'exit 143' TERM INT

if polaris_validate_marker "$marker"; then
  case "$POLARIS_MARKER_ROLE" in
    idle)
      polaris_stop_marked_gamescope "$marker" idle "$rt" || {
        echo "polaris-gamescope-idle: existing idle owner did not stop" >&2
        exit 1
      }
      ;;
    nested|runtime)
      # Nested WSI / owned spawn holds gamescope-0. Exit 0 so Restart=on-failure
      # does not fight polaris-gamescope-session (mask can race with restart).
      echo "polaris-gamescope-idle: yielding to active $POLARIS_MARKER_ROLE owner pid=$POLARIS_MARKER_PID" >&2
      exit 0
      ;;
    *)
      echo "polaris-gamescope-idle: refusing to replace active $POLARIS_MARKER_ROLE owner pid=$POLARIS_MARKER_PID" >&2
      exit 1
      ;;
  esac
else
  # No valid marker. If gamescope-0 is still live (nested race, or marker write
  # lag), yield cleanly — do NOT delete someone else's state or restart-loop.
  if [ -e "$rt/gamescope-0" ] || [ -S "$rt/gamescope-0" ]; then
    if ! polaris_socket_is_orphan "$rt/gamescope-0"; then
      echo "polaris-gamescope-idle: yielding to live gamescope-0 holder (not idle-owned)" >&2
      exit 0
    fi
  fi
  # Invalid marker + orphan sockets only: safe to discard and reclaim residue.
  rm -f "$marker"
  rm -f "$rt/polaris-gamescope.env"
fi

if ! polaris_reclaim_orphan_gamescope_sockets "$rt"; then
  # Live holder appeared between checks (nested start race): yield, don't loop.
  echo "polaris-gamescope-idle: yielding; live gamescope sockets remain" >&2
  exit 0
fi

prefer_vk=()
if [ -n "${POLARIS_GAMESCOPE_PREFER_VK:-}" ]; then
  prefer_vk=(--prefer-vk-device "$POLARIS_GAMESCOPE_PREFER_VK")
  echo "polaris-gamescope-idle: --prefer-vk-device=$POLARIS_GAMESCOPE_PREFER_VK" >&2
fi

force=0
if [ -f "$rt/polaris-gamescope-force" ]; then
  force="$(tr -d '[:space:]' <"$rt/polaris-gamescope-force" || true)"
fi
hdr_flags=()
if [ "$force" = 1 ] || [ "$force" = true ]; then
  echo "polaris-gamescope-idle: HDR mode" >&2
  hdr_flags=(
    --hdr-enabled
    --sdr-gamut-wideness "${POLARIS_SDR_GAMUT_WIDENESS:-0.000000}"
    --hdr-sdr-content-nits "${POLARIS_SDR_CONTENT_NITS:-203}"
  )
else
  echo "polaris-gamescope-idle: SDR mode" >&2
fi

setsid "$gs" \
  --backend headless \
  --expose-wayland \
  --steam \
  --xwayland-count 2 \
  "${prefer_vk[@]}" \
  "${hdr_flags[@]}" \
  -W "$width" -H "$height" -r "$refresh" \
  -w "$width" -h "$height" \
  -- sleep infinity &
child=$!

for _ in $(seq 1 100); do
  if polaris_write_marker_for_pid "$marker" "$child" idle; then
    polaris_read_marker "$marker"
    child_start="$POLARIS_MARKER_START_TIME"
    break
  fi
  kill -0 "$child" 2>/dev/null || break
  sleep 0.02
done
if [ -z "$child_start" ]; then
  echo "polaris-gamescope-idle: failed to record exact gamescope generation" >&2
  kill "$child" 2>/dev/null || true
  wait "$child" 2>/dev/null || true
  exit 1
fi

ready=0
for _ in $(seq 1 300); do
  if polaris_write_runtime_env "$marker" gamescope-0 idle "$rt"; then
    ready=1
    break
  fi
  kill -0 "$child" 2>/dev/null || break
  sleep 0.1
done
if [ "$ready" != 1 ]; then
  echo "polaris-gamescope-idle: owned gamescope-0/Xwayland did not become ready" >&2
  exit 1
fi

echo "polaris-gamescope-idle: ready pid=$child generation=$child_start" >&2
wait "$child"
