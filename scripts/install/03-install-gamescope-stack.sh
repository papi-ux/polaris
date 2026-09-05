#!/usr/bin/env bash
# Install gamescope_stream helpers + systemd user units (non-NixOS).
# Does not build patched gamescope/portal; uses system gamescope + host portal
# or optional private portal if you deploy it yourself.
#
# Usage: ./03-install-gamescope-stack.sh [--prefix DIR] [--polaris-bin PATH]
set -euo pipefail
# shellcheck source=common.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

POLARIS_BIN="${POLARIS_BIN:-}"
while [ $# -gt 0 ]; do
  case "$1" in
    --prefix) PREFIX="$2"; BIN_DIR="$PREFIX/bin"; LIBEXEC_DIR="$PREFIX/libexec/polaris"; shift 2 ;;
    --polaris-bin) POLARIS_BIN="$2"; shift 2 ;;
    -h|--help)
      cat <<EOF
Install gamescope_stream user stack for non-NixOS.

Installs:
  $BIN_DIR/polaris-gamescope-idle
  $BIN_DIR/polaris-gamescope-session
  $BIN_DIR/polaris-wait-gamescope
  $BIN_DIR/polaris-start          (config seed + exec)
  $SYSTEMD_USER_DIR/polaris-gamescope-idle.service
  $SYSTEMD_USER_DIR/polaris.service  (gamescope-oriented)

Usage: $0 [--prefix DIR] [--polaris-bin PATH]
EOF
      exit 0
      ;;
    *) die "unknown option: $1" ;;
  esac
done

if [ -z "$POLARIS_BIN" ]; then
  if [ -x "$BIN_DIR/polaris" ]; then
    POLARIS_BIN="$BIN_DIR/polaris"
  elif command -v polaris >/dev/null 2>&1; then
    POLARIS_BIN="$(command -v polaris)"
  else
    die "polaris binary not found; pass --polaris-bin or run 02-build-polaris.sh first"
  fi
fi
[ -x "$POLARIS_BIN" ] || die "not executable: $POLARIS_BIN"

command -v gamescope >/dev/null 2>&1 || warn "gamescope not on PATH — install it before starting services"
command -v steam >/dev/null 2>&1 || warn "steam not on PATH — library launches need Steam"

mkdir -p "$SYSTEMD_USER_DIR" "$CONFIG_DIR"
# Prefix directories may need root when installing system-wide. Never attempt a
# plain mkdir under /usr/local before privilege selection.
if is_user_prefix; then
  mkdir -p "$LIBEXEC_DIR" "$BIN_DIR"
else
  maybe_sudo mkdir -p "$LIBEXEC_DIR" "$BIN_DIR"
fi

# --- conf seed ---
SEED="$CONFIG_DIR/polaris.conf.gamescope-stream.example"
cat >"$SEED" <<EOF
# Example seed for gamescope_stream (copied once by polaris-start if conf missing)
headless_mode = enabled
linux_use_cage_compositor = enabled
linux_prefer_gpu_native_capture = disabled
linux_stream_mode = gamescope_stream
capture = portal
encoder = nvenc
hevc_mode = 3
av1_mode = 0
stream_audio = enabled
enable_pairing = enabled
enable_discovery = enabled
max_sessions = 2
EOF
log "example conf: $SEED"

# --- helper scripts ---
install_user_or_sudo "$REPO_ROOT/nix/modules/polaris-gamescope-runtime-lib.sh" "$BIN_DIR/polaris-gamescope-runtime-lib.sh"
install_user_or_sudo "$INSTALL_DIR/lib/polaris-gamescope-idle.sh" "$BIN_DIR/polaris-gamescope-idle"
install_user_or_sudo "$INSTALL_DIR/lib/polaris-wait-gamescope.sh" "$BIN_DIR/polaris-wait-gamescope"

# Session script: non-NixOS header + shared module body (nix/modules/…)
SESSION_OUT="$(mktemp)"
{
  cat <<'HDR'
#!/usr/bin/env bash
# polaris-gamescope-session (non-NixOS) — body from nix/modules/polaris-gamescope-session.sh
set -euo pipefail
export POLARIS_GAMESCOPE_BIN="${POLARIS_GAMESCOPE_BIN:-gamescope}"
# Prefer system tools; keep user PATH.
export PATH="/usr/local/bin:/usr/bin:/bin:${PATH:-}"
# Module may re-export PATH via POLARIS_SESSION_PATH when set by the unit.
HDR
  cat "$REPO_ROOT/nix/modules/polaris-gamescope-session.sh"
} >"$SESSION_OUT"
install_user_or_sudo "$SESSION_OUT" "$BIN_DIR/polaris-gamescope-session"
rm -f "$SESSION_OUT"

# polaris-start from template
START_OUT="$(mktemp)"
sed -e "s|@POLARIS_BIN@|$POLARIS_BIN|g" \
    -e "s|@CONF_SEED@|$SEED|g" \
    "$INSTALL_DIR/lib/polaris-start.sh.in" >"$START_OUT"
install_user_or_sudo "$START_OUT" "$BIN_DIR/polaris-start"
rm -f "$START_OUT"

# --- systemd user units ---
IDLE_UNIT="$SYSTEMD_USER_DIR/polaris-gamescope-idle.service"
cat >"$IDLE_UNIT" <<EOF
[Unit]
Description=Idle gamescope for Polaris (portal capture target)
After=graphical-session.target
PartOf=graphical-session.target

[Service]
Type=simple
ExecStart=$BIN_DIR/polaris-gamescope-idle
# Nested WSI stop/mask must not look like a crash restart.
Restart=on-abnormal
RestartSec=5s
TimeoutStopSec=10s
Environment=POLARIS_HDR_WIDTH=$POLARIS_HDR_WIDTH
Environment=POLARIS_HDR_HEIGHT=$POLARIS_HDR_HEIGHT
Environment=POLARIS_HDR_REFRESH=$POLARIS_HDR_REFRESH
PassEnvironment=XDG_RUNTIME_DIR DBUS_SESSION_BUS_ADDRESS

[Install]
WantedBy=graphical-session.target
EOF
log "wrote $IDLE_UNIT"

# Prefer not overwriting a package polaris.service without backup
POLARIS_UNIT="$SYSTEMD_USER_DIR/polaris.service"
if [ -f "$POLARIS_UNIT" ] && [ ! -f "$POLARIS_UNIT.bak-non-nixos" ]; then
  cp -a "$POLARIS_UNIT" "$POLARIS_UNIT.bak-non-nixos"
  log "backed up existing polaris.service → polaris.service.bak-non-nixos"
fi

# PATH for Steam + helpers (distro-specific; user can override via drop-in)
EXTRA_PATH="$BIN_DIR:/usr/local/bin:/usr/bin"
if [ -d /usr/games ]; then
  EXTRA_PATH="$EXTRA_PATH:/usr/games"
fi

cat >"$POLARIS_UNIT" <<EOF
[Unit]
Description=Polaris game stream host for Moonlight (gamescope_stream)
After=graphical-session.target polaris-gamescope-idle.service
Wants=polaris-gamescope-idle.service
PartOf=graphical-session.target

[Service]
Type=simple
ExecStartPre=$BIN_DIR/polaris-wait-gamescope
ExecStart=$BIN_DIR/polaris-start
Restart=on-failure
RestartSec=5s
LimitRTPRIO=95
LimitNICE=-10
Environment=POLARIS_HDR_WIDTH=$POLARIS_HDR_WIDTH
Environment=POLARIS_HDR_HEIGHT=$POLARIS_HDR_HEIGHT
Environment=POLARIS_HDR_REFRESH=$POLARIS_HDR_REFRESH
Environment=GAMESCOPE_WAYLAND_DISPLAY=gamescope-0
Environment=XDG_CURRENT_DESKTOP=gamescope
Environment=DISPLAY=:0
Environment=PATH=$EXTRA_PATH
# Do not PassEnvironment WAYLAND_DISPLAY — host compositor must not override gamescope path.
PassEnvironment=DISPLAY XDG_SESSION_TYPE XDG_SESSION_ID XAUTHORITY XDG_RUNTIME_DIR DBUS_SESSION_BUS_ADDRESS
# Unset host Wayland so probes do not bind KWin/wlgrab incorrectly.
UnsetEnvironment=WAYLAND_DISPLAY

[Install]
WantedBy=graphical-session.target
EOF
log "wrote $POLARIS_UNIT"

# Drop-in note for optional private portal
DROPIN_DIR="$SYSTEMD_USER_DIR/polaris.service.d"
mkdir -p "$DROPIN_DIR"
cat >"$DROPIN_DIR/README-portal.conf.example" <<'EOF'
# Optional: if you run a private gamescope portal on $XDG_RUNTIME_DIR/polaris-portal/bus
# (NixOS polaris-portal stack), copy to 10-private-portal.conf and adjust:
#
# [Service]
# Environment=POLARIS_PORTAL_DBUS_ADDRESS=unix:path=%t/polaris-portal/bus
EOF

if [ "${POLARIS_INSTALL_SKIP_SYSTEMD:-0}" = 1 ]; then
  log "systemd --user daemon-reload skipped (POLARIS_INSTALL_SKIP_SYSTEMD=1)"
else
  systemctl --user daemon-reload
  log "systemd --user daemon-reload done"
fi
log "next: ./04-enable-services.sh"
log "or:    systemctl --user enable --now polaris-gamescope-idle polaris"
log ""
log "Host setup (udev/modules) once:  sudo -H \"$POLARIS_BIN\" --setup-host"
log "Web UI: https://127.0.0.1:47990"
