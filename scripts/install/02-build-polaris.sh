#!/usr/bin/env bash
# Build Polaris from this checkout and install into PREFIX (default /usr/local).
# Usage: ./02-build-polaris.sh [--cuda] [--prefix DIR] [--jobs N]
set -euo pipefail
# shellcheck source=common.sh
source "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/common.sh"

WITH_CUDA=0
JOBS="$(nproc)"
BUILD_DIR="${BUILD_DIR:-$REPO_ROOT/build-non-nixos}"

while [ $# -gt 0 ]; do
  case "$1" in
    --cuda) WITH_CUDA=1; shift ;;
    --prefix) PREFIX="$2"; BIN_DIR="$PREFIX/bin"; LIBEXEC_DIR="$PREFIX/libexec/polaris"; shift 2 ;;
    --jobs|-j) JOBS="$2"; shift 2 ;;
    --build-dir) BUILD_DIR="$2"; shift 2 ;;
    -h|--help)
      cat <<EOF
Build and install Polaris from source (non-NixOS).

Usage: $0 [--cuda] [--prefix DIR] [--jobs N] [--build-dir DIR]

Defaults:
  PREFIX=$PREFIX
  BUILD_DIR=$BUILD_DIR
EOF
      exit 0
      ;;
    *) die "unknown option: $1" ;;
  esac
done

need_cmd cmake
need_cmd ninja
need_cmd git
need_cmd npm

cd "$REPO_ROOT"

if [ ! -d third-party/moonlight-common-c/.git ] && [ ! -f third-party/moonlight-common-c/CMakeLists.txt ]; then
  log "initializing git submodules"
  git submodule update --init --recursive
fi

log "npm ci (web UI)"
if [ -f package-lock.json ]; then
  npm ci --no-audit --fund=false
else
  npm install --no-audit --fund=false
fi

CUDA_ARGS=(
  -DPOLARIS_ENABLE_CUDA=OFF
  -DCUDA_FAIL_ON_MISSING=OFF
)
if [ "$WITH_CUDA" = 1 ]; then
  if command -v nvcc >/dev/null 2>&1 || [ -x /opt/cuda/bin/nvcc ]; then
    CUDA_ARGS=(
      -DPOLARIS_ENABLE_CUDA=ON
      -DCUDA_FAIL_ON_MISSING=ON
    )
    # Arch puts nvcc in /opt/cuda/bin
    if [ -x /opt/cuda/bin/nvcc ] && ! command -v nvcc >/dev/null 2>&1; then
      export PATH="/opt/cuda/bin:$PATH"
    fi
    log "CUDA enabled (nvcc=$(command -v nvcc || echo /opt/cuda/bin/nvcc))"
  else
    die "--cuda requested but nvcc not found"
  fi
fi

log "cmake configure → $BUILD_DIR (prefix=$PREFIX)"
cmake -S "$REPO_ROOT" -B "$BUILD_DIR" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$PREFIX" \
  "${CUDA_ARGS[@]}"

log "cmake build -j$JOBS"
cmake --build "$BUILD_DIR" -j"$JOBS"

log "cmake install"
if is_user_prefix; then
  cmake --install "$BUILD_DIR"
else
  maybe_sudo cmake --install "$BUILD_DIR"
fi

if [ -x "$BIN_DIR/polaris" ]; then
  log "polaris installed: $BIN_DIR/polaris"
else
  # Some layouts install versioned names
  if command -v polaris >/dev/null 2>&1; then
    log "polaris on PATH: $(command -v polaris)"
  else
    warn "could not find polaris binary under $BIN_DIR — check cmake install paths"
  fi
fi

log "next: sudo -H $BIN_DIR/polaris --setup-host"
log "then: ./03-install-gamescope-stack.sh   # for gamescope_stream mode"
log "or:   ./04-enable-services.sh --labwc  # stock labwc headless only"
