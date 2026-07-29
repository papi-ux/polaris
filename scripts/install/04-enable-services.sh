#!/usr/bin/env bash
# Enable and start Polaris user services (non-NixOS).
# Usage: ./04-enable-services.sh [--labwc] [--no-start]
set -euo pipefail
# shellcheck source=common.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

LABWC_ONLY=0
NO_START=0
for arg in "$@"; do
  case "$arg" in
    --labwc) LABWC_ONLY=1 ;;
    --no-start) NO_START=1 ;;
    -h|--help)
      cat <<EOF
Enable Polaris systemd user units.

Usage: $0 [--labwc] [--no-start]

  --labwc     Only polaris.service (package default / headless labwc)
  --no-start  enable only; do not start now
EOF
      exit 0
      ;;
    *) die "unknown option: $arg" ;;
  esac
done

need_cmd systemctl

systemctl --user daemon-reload

if [ "$LABWC_ONLY" = 1 ]; then
  log "enabling polaris.service (labwc / package default)"
  systemctl --user enable polaris.service
  if [ "$NO_START" = 0 ]; then
    systemctl --user restart polaris.service || systemctl --user start polaris.service
  fi
else
  log "enabling polaris-gamescope-idle + polaris (gamescope_stream)"
  if ! systemctl --user cat polaris-gamescope-idle.service >/dev/null 2>&1; then
    die "polaris-gamescope-idle.service missing — run 03-install-gamescope-stack.sh first"
  fi
  systemctl --user enable polaris-gamescope-idle.service polaris.service
  if [ "$NO_START" = 0 ]; then
    systemctl --user restart polaris-gamescope-idle.service \
      || systemctl --user start polaris-gamescope-idle.service
    sleep 1
    systemctl --user restart polaris.service \
      || systemctl --user start polaris.service
  fi
fi

log "status:"
systemctl --user --no-pager --full status polaris.service 2>/dev/null | head -20 || true
if [ "$LABWC_ONLY" = 0 ]; then
  systemctl --user --no-pager --full status polaris-gamescope-idle.service 2>/dev/null | head -15 || true
  rt="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
  if [ -S "$rt/gamescope-0" ]; then
    log "gamescope-0 socket: OK ($rt/gamescope-0)"
  else
    warn "gamescope-0 socket missing — check: journalctl --user -u polaris-gamescope-idle -n 50"
  fi
fi

log "Web UI: https://127.0.0.1:47990"
log "logs:   journalctl --user -u polaris -f"
