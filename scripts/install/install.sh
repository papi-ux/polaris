#!/usr/bin/env bash
# One-shot non-NixOS install for Polaris (+ optional gamescope_stream stack).
#
# Examples:
#   ./install.sh --from-source --cuda          # deps + build + gamescope stack + enable
#   ./install.sh --package-only                # assume polaris already installed
#   PREFIX=$HOME/.local ./install.sh --from-source
set -euo pipefail
# shellcheck source=common.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

FROM_SOURCE=0
PACKAGE_ONLY=0
WITH_CUDA=0
SKIP_DEPS=0
SKIP_STACK=0
LABWC_ONLY=0
JOBS="$(nproc)"

while [ $# -gt 0 ]; do
  case "$1" in
    --from-source) FROM_SOURCE=1; shift ;;
    --package-only) PACKAGE_ONLY=1; shift ;;
    --cuda) WITH_CUDA=1; shift ;;
    --skip-deps) SKIP_DEPS=1; shift ;;
    --skip-stack) SKIP_STACK=1; shift ;;
    --labwc) LABWC_ONLY=1; SKIP_STACK=1; shift ;;
    --prefix) PREFIX="$2"; BIN_DIR="$PREFIX/bin"; LIBEXEC_DIR="$PREFIX/libexec/polaris"; shift 2 ;;
    --jobs|-j) JOBS="$2"; shift 2 ;;
    -h|--help)
      cat <<'EOF'
Non-NixOS Polaris installer.

Usage:
  ./install.sh --from-source [--cuda] [--prefix DIR]
  ./install.sh --package-only [--labwc]     # polaris already on PATH / package install

Steps:
  1) install distro deps          (01-install-deps.sh)
  2) build + install polaris      (02-build-polaris.sh)   [ --from-source ]
  3) host udev/modules            (sudo -H polaris --setup-host)
  4) gamescope_stream user stack  (03-install-gamescope-stack.sh)
  5) enable user services         (04-enable-services.sh)

Environment:
  PREFIX=/usr/local          install prefix
  POLARIS_STREAM_MODE=...    gamescope_stream (default) or headless_stream
  POLARIS_HDR_WIDTH/HEIGHT/REFRESH   gamescope geometry (default 3840x2160@120)

Notes:
  - Prefer distro packages (Fedora/Arch/Ubuntu) from GitHub Releases when available;
    use --from-source for this branch's gamescope_stream helpers or CUDA builds.
  - Private polaris-portal bus (NixOS) is optional; host portal + gamescopegrab work
    for many setups. See scripts/install/README.md.
EOF
      exit 0
      ;;
    *) die "unknown option: $1 (try --help)" ;;
  esac
done

if [ "$FROM_SOURCE" = 0 ] && [ "$PACKAGE_ONLY" = 0 ]; then
  die "choose --from-source or --package-only (see --help)"
fi

if [ "$FROM_SOURCE" = 1 ] && [ "$SKIP_DEPS" = 0 ]; then
  DEPS_ARGS=()
  [ "$WITH_CUDA" = 1 ] && DEPS_ARGS+=(--cuda)
  if [ "$SKIP_STACK" = 0 ] && [ "$LABWC_ONLY" = 0 ]; then
    DEPS_ARGS+=(--gamescope-stack)
  fi
  "$INSTALL_DIR/01-install-deps.sh" "${DEPS_ARGS[@]}"
fi

if [ "$FROM_SOURCE" = 1 ]; then
  BUILD_ARGS=(--prefix "$PREFIX" --jobs "$JOBS")
  [ "$WITH_CUDA" = 1 ] && BUILD_ARGS+=(--cuda)
  "$INSTALL_DIR/02-build-polaris.sh" "${BUILD_ARGS[@]}"
fi

POLARIS_BIN="${POLARIS_BIN:-}"
if [ -z "$POLARIS_BIN" ]; then
  if [ -x "$BIN_DIR/polaris" ]; then
    POLARIS_BIN="$BIN_DIR/polaris"
  elif command -v polaris >/dev/null 2>&1; then
    POLARIS_BIN="$(command -v polaris)"
  else
    die "polaris not found after install"
  fi
fi

log "running host setup (udev/modules) — may prompt for sudo"
if [ "$(id -u)" -eq 0 ]; then
  "$POLARIS_BIN" --setup-host || warn "setup-host failed"
else
  sudo -H "$POLARIS_BIN" --setup-host || warn "setup-host failed (run later: sudo -H $POLARIS_BIN --setup-host)"
fi

if [ "$SKIP_STACK" = 0 ] && [ "$LABWC_ONLY" = 0 ]; then
  "$INSTALL_DIR/03-install-gamescope-stack.sh" --prefix "$PREFIX" --polaris-bin "$POLARIS_BIN"
  "$INSTALL_DIR/04-enable-services.sh"
else
  "$INSTALL_DIR/04-enable-services.sh" --labwc
fi

log "done"
log "  Web UI:  https://127.0.0.1:47990"
log "  status:  systemctl --user status polaris"
log "  logs:    journalctl --user -u polaris -f"
