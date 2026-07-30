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

clear_invalid_marker() (
  local lock_bin="${POLARIS_FLOCK_BIN:-flock}"
  exec 9>>"$rt/polaris-gamescope.lock" || return 1
  "$lock_bin" -x 9 || return 1
  polaris_validate_marker "$marker" && return 0
  [ ! -e "$marker" ] || rm -f -- "$marker"
)

idle_ownership_is_current() {
  [ ! -e "$rt/polaris-gamescope-wsi-nested" ] \
    && polaris_validate_marker "$marker" idle \
    && polaris_marker_owns_socket "$marker" "$rt/gamescope-0" 2>/dev/null
}

# Serialize invalid-marker removal with every cooperating ownership publisher,
# then revalidate: never unlink a marker based on a lockless stale observation.
if ! polaris_validate_marker "$marker"; then
  clear_invalid_marker || exit 1
fi
if ! polaris_validate_marker "$marker"; then
  if ! polaris_reclaim_orphan_gamescope_sockets "$rt"; then
    echo "polaris: live unowned gamescope sockets block startup" >&2
    exit 1
  fi
elif ! polaris_marker_owns_socket "$marker" "$rt/gamescope-0" 2>/dev/null; then
  # Valid marker but not holding gamescope-0 — still drop dead residue only.
  polaris_reclaim_orphan_gamescope_sockets "$rt" || true
fi

# Nested recovery is credentialed and transactional: only the session stop state
# machine may drain exact-session Steam, restore idle ownership, rebind the
# private portal, and clear the durable claim.
if [ -f "$rt/polaris-gamescope-wsi-nested" ]; then
  session_cmd="${POLARIS_GAMESCOPE_SESSION_BIN:-polaris-gamescope-session}"
  { [ -s "$rt/polaris-gamescope-session-state" ] \
      || [ -s "$rt/polaris-gamescope-session-id" ]; } || {
    echo "polaris: nested recovery lacks its immutable session credential" >&2
    exit 1
  }
  command -v "$session_cmd" >/dev/null 2>&1 || {
    echo "polaris: credentialed gamescope session recovery command is unavailable" >&2
    exit 1
  }
  echo "polaris: complete credentialed nested recovery" >&2
  POLARIS_SESSION_INSTANCE_ID='' "$session_cmd" stop || exit 1
  [ ! -e "$rt/polaris-gamescope-wsi-nested" ] || exit 1
elif [ ! -S "$rt/gamescope-0" ]; then
  echo "polaris: restore missing idle gamescope-0" >&2
  polaris_unmask_idle_unit_runtime
  systemctl --user restart polaris-gamescope-idle.service 2>/dev/null \
    || systemctl --user start polaris-gamescope-idle.service 2>/dev/null || exit 1
fi

deadline=$((SECONDS + 60))
while [ ! -S "$rt/gamescope-0" ]; do
  if [ "$SECONDS" -ge "$deadline" ]; then
    echo "polaris: timed out waiting for gamescope-0" >&2
    exit 1
  fi
  sleep 0.2
done

# Hold the same ownership-transition lock used by marker writers and nested
# claim publication through idle validation and portal rebinding.
exec 8>>"$rt/polaris-gamescope.lock" || exit 1
"${POLARIS_FLOCK_BIN:-flock}" -x 8 || exit 1

if ! idle_ownership_is_current; then
  echo "polaris: gamescope-0 appeared without exact idle ownership" >&2
  exit 1
fi

bus_path="$rt/polaris-portal/bus"
if [ ! -e "$bus_path" ]; then
  idle_ownership_is_current || exit 1
  echo "polaris: gamescope-0 ready (no private portal bus — host portal or gamescopegrab OK)" >&2
  exit 0
fi

export DBUS_SESSION_BUS_ADDRESS="unix:path=$bus_path"
idle_ownership_is_current || {
  echo "polaris: idle ownership changed before private portal restart" >&2
  exit 1
}
systemctl --user restart polaris-portal-gamescope.service >/dev/null 2>&1 || {
  echo "polaris: failed to restart private gamescope portal" >&2
  exit 1
}
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
    idle_ownership_is_current || {
      echo "polaris: nested generation replaced idle ownership during portal recovery" >&2
      exit 1
    }
    echo "polaris: private ScreenCast ready (gamescope-0 + portal, cursor_modes=$modes)" >&2
    exit 0
  fi
  if [ "$SECONDS" -ge "$deadline" ]; then
    echo "polaris: private ScreenCast portal did not become ready" >&2
    exit 1
  fi
  sleep 0.25
done
