#!/usr/bin/env bash
set -euo pipefail

fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
work="$(mktemp -d "${TMPDIR:-/tmp}/polaris-gamescope-session-geometry.XXXXXX")"
trap 'rm -rf "$work"' EXIT

script="${POLARIS_SOURCE_DIR:?}/nix/modules/polaris-gamescope-session.sh"
prelude="$work/geometry-prelude.sh"
runtime_stub="$work/runtime-stub.sh"
: >"$runtime_stub"

# Exercise the production resolver without entering the helper's command
# dispatch. The geometry contract lives entirely before the first state path.
awk '/^session_id_file=/{exit} {print}' "$script" >"$prelude"

resolve_geometry() {
  local session_width="$1" session_height="$2" session_fps="$3"
  local legacy_width="$4" legacy_height="$5" legacy_refresh="$6"
  local -a values=("POLARIS_GEOMETRY_TEST=1")

  [ "$session_width" = unset ] || values+=("POLARIS_SESSION_TARGET_WIDTH=$session_width")
  [ "$session_height" = unset ] || values+=("POLARIS_SESSION_TARGET_HEIGHT=$session_height")
  [ "$session_fps" = unset ] || values+=("POLARIS_SESSION_TARGET_FPS=$session_fps")
  [ "$legacy_width" = unset ] || values+=("POLARIS_HDR_WIDTH=$legacy_width")
  [ "$legacy_height" = unset ] || values+=("POLARIS_HDR_HEIGHT=$legacy_height")
  [ "$legacy_refresh" = unset ] || values+=("POLARIS_HDR_REFRESH=$legacy_refresh")

  env -u POLARIS_SESSION_TARGET_WIDTH -u POLARIS_SESSION_TARGET_HEIGHT \
    -u POLARIS_SESSION_TARGET_FPS -u POLARIS_HDR_WIDTH \
    -u POLARIS_HDR_HEIGHT -u POLARIS_HDR_REFRESH \
    XDG_RUNTIME_DIR="$work/run" \
    POLARIS_GAMESCOPE_RUNTIME_LIB="$runtime_stub" \
    "${values[@]}" \
    bash -c '. "$1"; printf "%sx%s@%s\n" "$gs_width" "$gs_height" "$gs_refresh"' \
    _ "$prelude"
}

[ "$(resolve_geometry 1920 1080 59.940 3840 2160 120)" = "1920x1080@59.940" ] ||
  fail "session geometry did not take precedence"
[ "$(resolve_geometry unset unset unset 2560 1440 144)" = "2560x1440@144" ] ||
  fail "legacy geometry fallback was not preserved"
[ "$(resolve_geometry unset unset unset unset unset unset)" = "3840x2160@120" ] ||
  fail "standalone defaults were not preserved"
[ "$(resolve_geometry bad 0 0 invalid -1 nope)" = "3840x2160@120" ] ||
  fail "invalid geometry did not fail safely to defaults"

printf 'PASS: gamescope session geometry contract\n'
