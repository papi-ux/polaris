#!/usr/bin/env bash
# Idle headless gamescope for portal / gamescopegrab capture (non-NixOS).
set -euo pipefail

width="${POLARIS_HDR_WIDTH:-3840}"
height="${POLARIS_HDR_HEIGHT:-2160}"
refresh="${POLARIS_HDR_REFRESH:-120}"
rt="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
gs="${POLARIS_GAMESCOPE_BIN:-gamescope}"

command -v "$gs" >/dev/null 2>&1 || {
  echo "polaris-gamescope-idle: gamescope not found (set POLARIS_GAMESCOPE_BIN)" >&2
  exit 1
}

# Only kill idle-style headless (sleep infinity), never nested Steam sessions.
pkill -u "$(id -u)" -f 'gamescope .*--backend headless.*sleep infinity' 2>/dev/null || true
n=10
while [ "$n" -gt 0 ]; do
  if ! pgrep -u "$(id -u)" -f 'gamescope .*--backend headless.*sleep infinity' >/dev/null 2>&1; then
    break
  fi
  sleep 0.2
  n=$((n - 1))
done
if ! pgrep -u "$(id -u)" -f 'gamescope .*--backend headless' >/dev/null 2>&1; then
  rm -f \
    "$rt/gamescope-0" "$rt/gamescope-0.lock" \
    "$rt/gamescope-0-ei" "$rt/gamescope-0-ei.lock" \
    "$rt/gamescope-1" "$rt/gamescope-1.lock" \
    2>/dev/null || true
fi

# Steam base XWayland :0; STEAM_MULTIPLE_XWAYLANDS → games on :1.
printf 'DISPLAY=:0\nWAYLAND_DISPLAY=gamescope-0\nGAMESCOPE_WAYLAND_DISPLAY=gamescope-0\n' \
  >"$rt/polaris-gamescope.env"

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
if [ "$force" = "1" ] || [ "$force" = "true" ]; then
  echo "polaris-gamescope-idle: HDR mode" >&2
  hdr_flags=(
    --hdr-enabled
    --sdr-gamut-wideness 0.000000
    --hdr-sdr-content-nits 203
  )
else
  echo "polaris-gamescope-idle: SDR mode" >&2
fi

exec "$gs" \
  --backend headless \
  --expose-wayland \
  --steam \
  --xwayland-count 2 \
  "${prefer_vk[@]}" \
  "${hdr_flags[@]}" \
  -W "$width" -H "$height" -r "$refresh" \
  -w "$width" -h "$height" \
  -- sleep infinity
