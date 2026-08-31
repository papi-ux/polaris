#!/usr/bin/env bash
set -euo pipefail

fail() {
  printf 'FAIL: %s\n' "$*" >&2
  exit 1
}

work="$(mktemp -d "${TMPDIR:-/tmp}/polaris-gamescope-primary-child.XXXXXX")"
keeper_pid=""
cleanup() {
  if [ -n "$keeper_pid" ] && kill -0 "$keeper_pid" 2>/dev/null; then
    kill -TERM "$keeper_pid" 2>/dev/null || true
    wait "$keeper_pid" 2>/dev/null || true
  fi
  rm -rf "$work"
}
trap cleanup EXIT
mkdir -p "$work/bin" "$work/run"

printf '%s\n' 'polaris_validate_marker() { return 1; }' >"$work/runtime-stub.sh"
printf '%s\n' \
  '#!/usr/bin/env bash' \
  'printf "%s\\n" "$@" >"$POLARIS_STEAM_ARGS"' \
  'exit "${POLARIS_STEAM_EXIT_CODE:-0}"' >"$work/bin/steam"
chmod +x "$work/bin/steam"

script="${POLARIS_SOURCE_DIR:?}/nix/modules/polaris-gamescope-session.sh"
state="$work/run/polaris-gamescope-session-state"
exit_marker="$work/run/polaris-gamescope-primary-child-exit"
printf 'session-A nested standalone\n' >"$state"

helper_env=(
  env
  "PATH=$work/bin:/usr/bin:/bin"
  "POLARIS_SESSION_PATH=$work/bin:/usr/bin:/bin"
  "XDG_RUNTIME_DIR=$work/run"
  "POLARIS_GAMESCOPE_RUNTIME_LIB=$work/runtime-stub.sh"
  POLARIS_SESSION_INSTANCE_ID=session-A
  "POLARIS_STEAM_ARGS=$work/steam.args"
)
run_helper() {
  "${helper_env[@]}" bash "$script" "$@"
}

# Steam can exit without returning from Gamescope's primary child. The exact
# credential is published for the wait process, while the keeper stays alive
# until fenced teardown owns the compositor generation.
"${helper_env[@]}" bash "$script" nested-primary-child -- steam -gamepadui -applaunch 870780 \
  >"$work/keeper.log" 2>&1 &
keeper_pid=$!
for _ in $(seq 1 100); do
  [ -f "$exit_marker" ] && break
  sleep 0.02
done
[ -f "$exit_marker" ] || fail "primary child did not publish Steam terminal state"
[ "$(tr -d '\r\n' <"$exit_marker")" = session-A ] ||
  fail "primary child terminal state was not bound to the session credential"
kill -0 "$keeper_pid" 2>/dev/null || fail "primary child returned after Steam exit"
printf '%s\n' -gamepadui -applaunch 870780 >"$work/expected.args"
cmp -s "$work/expected.args" "$work/steam.args" || fail "primary child changed the Steam command"
grep -Fq 'primary child holding for fenced compositor teardown' "$work/keeper.log" ||
  fail "primary child did not report its fenced hold state"

# The Moonlight wait process consumes only the exact credential marker and
# releases so Polaris can execute the normal authenticated stop command.
run_helper wait >"$work/wait.log" 2>&1 || fail "wait rejected the exact primary-child marker"
grep -Fq 'exact-session Steam primary command exited' "$work/wait.log" ||
  fail "wait did not identify the exact primary-child terminal marker"

printf 'session-B\n' >"$exit_marker"
if run_helper wait >/dev/null 2>&1; then
  fail "wait accepted another session's primary-child terminal marker"
fi

kill -TERM "$keeper_pid"
wait "$keeper_pid" || fail "primary child did not terminate cleanly with its parent"
keeper_pid=""

# Missing credentials and non-Steam payloads are never accepted as an internal
# Gamescope primary child invocation.
if env PATH="$work/bin:/usr/bin:/bin" POLARIS_SESSION_PATH="$work/bin:/usr/bin:/bin" \
    XDG_RUNTIME_DIR="$work/run" POLARIS_GAMESCOPE_RUNTIME_LIB="$work/runtime-stub.sh" \
    POLARIS_SESSION_INSTANCE_ID= \
    POLARIS_STEAM_ARGS="$work/rejected.args" \
    bash "$script" nested-primary-child -- steam -gamepadui >/dev/null 2>&1; then
  fail "primary child accepted a missing session credential"
fi
if run_helper nested-primary-child -- /bin/true >/dev/null 2>&1; then
  fail "primary child accepted a non-Steam command"
fi
[ ! -e "$work/rejected.args" ] || fail "rejected primary child launched Steam"

# The production launch must bind wrapper lifetime directly to Gamescope.
grep -Fq 'setpriv --pdeathsig TERM --' "$script" ||
  fail "nested launch does not arm the primary child parent-death signal"
grep -Fq 'bash "$0" nested-primary-child -- "${steam_launch[@]}"' "$script" ||
  fail "nested launch does not use the session-owned primary child"
grep -Fq 'rm -f -- "$nested_primary_exit_file"' "$script" ||
  fail "nested launch does not clear an old primary-child terminal marker"

printf 'PASS: gamescope primary child keeps natural exit behind the exact fence\n'
