#!/usr/bin/env bash
# Ensure gamescope-0 is up before polaris starts (non-NixOS).
# Optional private portal bus at $XDG_RUNTIME_DIR/polaris-portal/bus.
set -euo pipefail

rt="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"

# Nested stop can leave runtime-masked idle / no gamescope-0.
if [ -f "$rt/polaris-gamescope-wsi-nested" ] || [ ! -S "$rt/gamescope-0" ]; then
  echo "polaris: recover idle gamescope-0 (nested leftover or missing socket)" >&2
  rm -f "$rt/polaris-gamescope-wsi-nested" "$rt/polaris-gamescope-appid" \
    "$rt/polaris-gamescope-audio-sink" "$rt/polaris-gamescope.pid" || true
  systemctl --user unmask --runtime polaris-gamescope-idle.service 2>/dev/null || true
  if [ ! -S "$rt/gamescope-0" ]; then
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
