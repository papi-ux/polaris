#!/usr/bin/env bash
# Shared helpers for non-NixOS Polaris install scripts.
set -euo pipefail

# Repo root: scripts/install/ → ../..
INSTALL_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "$INSTALL_DIR/../.." && pwd)"

# Install prefix (override with PREFIX= or --prefix)
PREFIX="${PREFIX:-/usr/local}"
BIN_DIR="${BIN_DIR:-$PREFIX/bin}"
LIBEXEC_DIR="${LIBEXEC_DIR:-$PREFIX/libexec/polaris}"
SYSTEMD_USER_DIR="${SYSTEMD_USER_DIR:-${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user}"
CONFIG_DIR="${CONFIG_DIR:-${XDG_CONFIG_HOME:-$HOME/.config}/polaris}"

# Geometry defaults for gamescope idle / nested WSI
POLARIS_HDR_WIDTH="${POLARIS_HDR_WIDTH:-3840}"
POLARIS_HDR_HEIGHT="${POLARIS_HDR_HEIGHT:-2160}"
POLARIS_HDR_REFRESH="${POLARIS_HDR_REFRESH:-120}"

# Stream mode seed: gamescope_stream | headless_stream
POLARIS_STREAM_MODE="${POLARIS_STREAM_MODE:-gamescope_stream}"

log()  { printf '==> %s\n' "$*"; }
warn() { printf 'warning: %s\n' "$*" >&2; }
die()  { printf 'error: %s\n' "$*" >&2; exit 1; }

need_cmd() {
  command -v "$1" >/dev/null 2>&1 || die "missing required command: $1"
}

# Detect id-like distro family for package managers.
detect_distro() {
  if [ -f /etc/os-release ]; then
    # shellcheck source=/dev/null
    . /etc/os-release
    case "${ID_LIKE:-$ID}" in
      *fedora*|*rhel*|*centos*) echo fedora ;;
      *arch*|cachyos) echo arch ;;
      *debian*|*ubuntu*) echo debian ;;
      *suse*) echo suse ;;
      *) echo "${ID:-unknown}" ;;
    esac
  else
    echo unknown
  fi
}

maybe_sudo() {
  if [ "$(id -u)" -eq 0 ]; then
    "$@"
  elif command -v sudo >/dev/null 2>&1; then
    sudo "$@"
  else
    die "need root (or sudo) to run: $*"
  fi
}

install_bin() {
  local src="$1" dest="$2"
  maybe_sudo install -d "$(dirname "$dest")"
  maybe_sudo install -m 755 "$src" "$dest"
  log "installed $dest"
}

write_file() {
  local dest="$1" mode="${2:-644}"
  maybe_sudo install -d "$(dirname "$dest")"
  maybe_sudo tee "$dest" >/dev/null
  maybe_sudo chmod "$mode" "$dest"
  log "wrote $dest"
}

# User-writable install without root when PREFIX is under $HOME.
is_user_prefix() {
  case "$PREFIX" in
    "$HOME"/*|"$HOME") return 0 ;;
    *) return 1 ;;
  esac
}

install_user_or_sudo() {
  if is_user_prefix; then
    install -d "$(dirname "$2")"
    install -m "${3:-755}" "$1" "$2"
    log "installed $2"
  else
    install_bin "$1" "$2"
  fi
}

write_user_or_sudo() {
  local dest="$1" mode="${2:-644}"
  if is_user_prefix || [[ "$dest" == "$HOME"/* ]]; then
    mkdir -p "$(dirname "$dest")"
    cat >"$dest"
    chmod "$mode" "$dest"
    log "wrote $dest"
  else
    write_file "$dest" "$mode"
  fi
}
