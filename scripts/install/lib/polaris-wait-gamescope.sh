#!/usr/bin/env bash
# Ensure gamescope-0 is up before polaris starts (non-NixOS).
# Optional private portal bus at $XDG_RUNTIME_DIR/polaris-portal/bus.
set -euo pipefail

if ! declare -F polaris_validate_marker >/dev/null 2>&1; then
  runtime_lib="${POLARIS_GAMESCOPE_RUNTIME_LIB:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/polaris-gamescope-runtime-lib.sh}"
  # shellcheck source=/dev/null
  . "$runtime_lib"
fi

rt="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
marker="$rt/polaris-gamescope.pid"

# Crash residue: a gamescope-0 path without a live holder looks "ready" to -S
# but cannot accept clients. Reclaim orphans before treating the socket as up.
if ! polaris_validate_marker "$marker"; then
  rm -f "$marker"
  if ! polaris_reclaim_orphan_gamescope_sockets "$rt"; then
    echo "polaris: live unowned gamescope sockets block startup" >&2
    exit 1
  fi
elif ! polaris_marker_owns_socket "$marker" "$rt/gamescope-0" 2>/dev/null; then
  # Valid marker but not holding gamescope-0 — still drop dead residue only.
  polaris_reclaim_orphan_gamescope_sockets "$rt" || true
fi

# Nested stop can leave runtime-masked idle / no gamescope-0.
if [ -f "$rt/polaris-gamescope-wsi-nested" ] || [ ! -S "$rt/gamescope-0" ]; then
  echo "polaris: recover idle gamescope-0 (nested leftover or missing socket)" >&2
  marker_role=""
  if polaris_validate_marker "$marker"; then
    marker_role="$POLARIS_MARKER_ROLE"
    if [ "$marker_role" = nested ]; then
      polaris_stop_marked_gamescope "$marker" nested "$rt" || {
        echo "polaris: refusing to replace a live nested gamescope generation" >&2
        exit 1
      }
      marker_role=""
    fi
  fi
  rm -f "$rt/polaris-gamescope-wsi-nested" "$rt/polaris-gamescope-appid" \
    "$rt/polaris-gamescope-audio-sink" || true
  polaris_unmask_idle_unit_runtime
  if [ ! -S "$rt/gamescope-0" ] && [ "$marker_role" != runtime ]; then
    systemctl --user restart polaris-gamescope-idle.service 2>/dev/null \
      || systemctl --user start polaris-gamescope-idle.service 2>/dev/null || true
  fi
fi

deadline=$((SECONDS + 60))
while [ ! -S "$rt/gamescope-0" ]; do
  if [ "$SECONDS" -ge "$deadline" ]; then
    echo "polaris: timed out waiting for gamescope-0" >&2
    exit 1
  fi
  sleep 0.2
done

bus_path="$rt/polaris-portal/bus"
if [ ! -e "$bus_path" ]; then
  echo "polaris: gamescope-0 ready (no private portal bus — host portal or gamescopegrab OK)" >&2
  exit 0
fi

export DBUS_SESSION_BUS_ADDRESS="unix:path=$bus_path"
deadline=$((SECONDS + 45))
while true; do
  modes=""
  if command -v busctl >/dev/null 2>&1; then
    modes="$(busctl --user get-property org.freedesktop.impl.portal.desktop.gamescope \
      /org/freedesktop/portal/desktop \
      org.freedesktop.impl.portal.ScreenCast AvailableCursorModes 2>/dev/null \
      | awk '{print $2}' || true)"
  fi
  if [ -n "${modes:-}" ] && [ "${modes}" != "0" ]; then
    echo "polaris: private ScreenCast ready (gamescope-0 + portal, cursor_modes=$modes)" >&2
    exit 0
  fi
  if [ "$SECONDS" -ge "$deadline" ]; then
    echo "polaris: portal not ready; continuing (gamescopegrab may still work)" >&2
    exit 0
  fi
  sleep 0.25
done
