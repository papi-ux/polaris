#!/usr/bin/env bash
set -euo pipefail

fail() {
  printf 'FAIL: %s\n' "$*" >&2
  exit 1
}

work="$(mktemp -d "${TMPDIR:-/tmp}/polaris-gamescope-primary-child.XXXXXX")"
keeper_pid=""
parent_death_child_pid=""
parent_death_steam_pid=""
parent_death_descendant_pid=""
parent_death_leaderless_pid=""
keeper_death_child_pid=""
keeper_death_descendant_pid=""
keeper_death_leaderless_pid=""
cleanup() {
  if [ -n "$keeper_pid" ] && kill -0 "$keeper_pid" 2>/dev/null; then
    kill -TERM "$keeper_pid" 2>/dev/null || true
    wait "$keeper_pid" 2>/dev/null || true
  fi
  if [ -n "$parent_death_child_pid" ] && kill -0 "$parent_death_child_pid" 2>/dev/null; then
    kill -KILL "$parent_death_child_pid" 2>/dev/null || true
  fi
  if [ -n "$parent_death_steam_pid" ] && kill -0 "$parent_death_steam_pid" 2>/dev/null; then
    kill -KILL "$parent_death_steam_pid" 2>/dev/null || true
  fi
  if [ -n "$parent_death_descendant_pid" ] && kill -0 "$parent_death_descendant_pid" 2>/dev/null; then
    kill -KILL "$parent_death_descendant_pid" 2>/dev/null || true
  fi
  if [ -n "$parent_death_leaderless_pid" ] && kill -0 "$parent_death_leaderless_pid" 2>/dev/null; then
    kill -KILL "$parent_death_leaderless_pid" 2>/dev/null || true
  fi
  if [ -n "$keeper_death_child_pid" ] && kill -0 "$keeper_death_child_pid" 2>/dev/null; then
    kill -KILL "$keeper_death_child_pid" 2>/dev/null || true
  fi
  if [ -n "$keeper_death_descendant_pid" ] && kill -0 "$keeper_death_descendant_pid" 2>/dev/null; then
    kill -KILL "$keeper_death_descendant_pid" 2>/dev/null || true
  fi
  if [ -n "$keeper_death_leaderless_pid" ] && kill -0 "$keeper_death_leaderless_pid" 2>/dev/null; then
    kill -KILL "$keeper_death_leaderless_pid" 2>/dev/null || true
  fi
  rm -rf "$work"
}
trap cleanup EXIT
mkdir -p "$work/bin" "$work/run"

printf '%s\n' \
  'polaris_validate_marker() { return 1; }' \
  'polaris_gamescope_reaper_pid() { [ "${1:-}" = "${POLARIS_TEST_GAMESCOPE_PID:-}" ]; }' \
  >"$work/runtime-stub.sh"
printf '%s\n' \
  '#!/usr/bin/env bash' \
  'printf "%s\\n" "$@" >"$POLARIS_STEAM_ARGS"' \
  'spawn_test_descendants() {' \
  '  if [ "${POLARIS_STEAM_FORK_DESCENDANT:-0}" = 1 ]; then' \
  '    (' \
  '      trap '\''exit 0'\'' TERM INT HUP' \
  '      printf "%s\\n" "$BASHPID" >"$POLARIS_STEAM_DESCENDANT_PID_FILE"' \
  '      while :; do sleep 0.05; done' \
  '    ) &' \
  '  fi' \
  '  if [ "${POLARIS_STEAM_FORK_LEADERLESS:-0}" = 1 ]; then' \
  '    "$POLARIS_STEAM_LEADERLESS_HELPER"' \
  '    leaderless_group="$(tr -d "\\r\\n" <"$POLARIS_STEAM_LEADERLESS_GROUP_FILE")"' \
  '    leaderless_member="$(tr -d "\\r\\n" <"$POLARIS_STEAM_LEADERLESS_MEMBER_FILE")"' \
  '    [ ! -e "/proc/$leaderless_group" ] || exit 91' \
  '    [ "$(awk '\''{ print $5 }'\'' "/proc/$leaderless_member/stat")" = "$leaderless_group" ] || exit 92' \
  '  fi' \
  '}' \
  'if [ "${POLARIS_STEAM_HOLD:-0}" = 1 ] || [ "${POLARIS_STEAM_EXIT_WITH_DESCENDANTS:-0}" = 1 ]; then' \
  '  spawn_test_descendants' \
  '  printf "%s\\n" "$$" >"$POLARIS_STEAM_PID_FILE"' \
  '  : >"$POLARIS_STEAM_STARTED_FILE"' \
  '  [ "${POLARIS_STEAM_EXIT_WITH_DESCENDANTS:-0}" != 1 ] || exit 0' \
  '  trap '\''exit 0'\'' TERM INT HUP' \
  '  while :; do sleep 0.05; done' \
  'fi' \
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
  "POLARIS_TEST_GAMESCOPE_PID=$$"
  POLARIS_SESSION_INSTANCE_ID=session-A
  "POLARIS_STEAM_ARGS=$work/steam.args"
)
run_helper() {
  "${helper_env[@]}" bash "$script" "$@"
}

process_is_live_non_zombie() {
  local pid="$1" state
  kill -0 "$pid" 2>/dev/null || return 1
  state="$(awk '{ print $3 }' "/proc/$pid/stat" 2>/dev/null || true)"
  [ "$state" != Z ]
}

# A stale/reparented wrapper must reject the launch even when a parent-death
# signal was missed before setpriv could arm it.
if "${helper_env[@]}" POLARIS_TEST_GAMESCOPE_PID=999999 \
    POLARIS_STEAM_ARGS="$work/rejected-parent.args" \
    bash "$script" nested-primary-child -- steam -gamepadui >/dev/null 2>&1; then
  fail "primary child accepted a non-Gamescope parent"
fi
[ ! -e "$work/rejected-parent.args" ] || fail "invalid primary parent launched Steam"

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

if [ "$(uname -s)" = Linux ] && command -v setpriv >/dev/null 2>&1 && [ -r "/proc/$$/stat" ]; then
  "${CC:-cc}" -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror \
    "$POLARIS_SOURCE_DIR/tests/unit/platform/fake_gamescope_parent.c" \
    -o "$work/bin/gamescope"
  "${CC:-cc}" -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror \
    "$POLARIS_SOURCE_DIR/tests/unit/platform/fake_gamescope_parent.c" \
    -o "$work/bin/gamescopereaper"
  "${CC:-cc}" -std=c11 -D_DEFAULT_SOURCE -O2 -Wall -Wextra -Werror \
    "$POLARIS_SOURCE_DIR/tests/unit/platform/fake_leaderless_session_member.c" \
    -o "$work/bin/leaderless-session-member"

  POLARIS_GAMESCOPE_RUNTIME_LIB="$POLARIS_SOURCE_DIR/nix/modules/polaris-gamescope-runtime-lib.sh" \
  POLARIS_FAKE_CHILD_PID_FILE="$work/parent-death-child.pid" \
  POLARIS_STEAM_HOLD=1 \
  POLARIS_STEAM_FORK_DESCENDANT=1 \
  POLARIS_STEAM_FORK_LEADERLESS=1 \
  POLARIS_STEAM_LEADERLESS_HELPER="$work/bin/leaderless-session-member" \
  POLARIS_STEAM_LEADERLESS_GROUP_FILE="$work/parent-death-leaderless-group.pid" \
  POLARIS_STEAM_LEADERLESS_MEMBER_FILE="$work/parent-death-leaderless-member.pid" \
  POLARIS_STEAM_STARTED_FILE="$work/parent-death-steam.started" \
  POLARIS_STEAM_PID_FILE="$work/parent-death-steam.pid" \
  POLARIS_STEAM_DESCENDANT_PID_FILE="$work/parent-death-descendant.pid" \
  POLARIS_STEAM_ARGS="$work/parent-death-steam.args" \
  PATH="$work/bin:/usr/bin:/bin" \
  POLARIS_SESSION_PATH="$work/bin:/usr/bin:/bin" \
  XDG_RUNTIME_DIR="$work/run" \
  POLARIS_SESSION_INSTANCE_ID=session-A \
    "$work/bin/gamescope" --backend headless -- \
      setpriv --pdeathsig TERM -- bash "$script" nested-primary-child -- steam -gamepadui

  parent_death_child_pid="$(tr -d '\r\n' <"$work/parent-death-child.pid")"
  parent_death_steam_pid="$(tr -d '\r\n' <"$work/parent-death-steam.pid")"
  parent_death_descendant_pid="$(tr -d '\r\n' <"$work/parent-death-descendant.pid")"
  parent_death_leaderless_pid="$(tr -d '\r\n' <"$work/parent-death-leaderless-member.pid")"
  for _ in $(seq 1 200); do
    process_is_live_non_zombie "$parent_death_child_pid" || break
    sleep 0.02
  done
  process_is_live_non_zombie "$parent_death_child_pid" &&
    fail "primary child survived its Gamescope parent"
  for _ in $(seq 1 200); do
    process_is_live_non_zombie "$parent_death_steam_pid" || break
    sleep 0.02
  done
  process_is_live_non_zombie "$parent_death_steam_pid" &&
    fail "Steam survived its Gamescope parent"
  for _ in $(seq 1 200); do
    process_is_live_non_zombie "$parent_death_descendant_pid" || break
    sleep 0.02
  done
  process_is_live_non_zombie "$parent_death_descendant_pid" &&
    fail "Steam descendant survived its Gamescope parent"
  for _ in $(seq 1 200); do
    process_is_live_non_zombie "$parent_death_leaderless_pid" || break
    sleep 0.02
  done
  process_is_live_non_zombie "$parent_death_leaderless_pid" &&
    fail "leaderless Steam session member survived its Gamescope parent"
  parent_death_child_pid=""
  parent_death_steam_pid=""
  parent_death_descendant_pid=""
  parent_death_leaderless_pid=""

  # Steam's primary command may return while same-session helpers remain.
  # Wait until the wrapper publishes that terminal state, then kill Gamescope;
  # the retained parent-death authority must still drain both an ordinary
  # descendant and a stubborn member of a now-leaderless sibling group.
  rm -f -- "$exit_marker"
  POLARIS_GAMESCOPE_RUNTIME_LIB="$POLARIS_SOURCE_DIR/nix/modules/polaris-gamescope-runtime-lib.sh" \
  POLARIS_FAKE_CHILD_PID_FILE="$work/keeper-death-child.pid" \
  POLARIS_FAKE_PARENT_EXIT_GATE_FILE="$exit_marker" \
  POLARIS_STEAM_EXIT_WITH_DESCENDANTS=1 \
  POLARIS_STEAM_FORK_DESCENDANT=1 \
  POLARIS_STEAM_FORK_LEADERLESS=1 \
  POLARIS_STEAM_LEADERLESS_HELPER="$work/bin/leaderless-session-member" \
  POLARIS_STEAM_LEADERLESS_GROUP_FILE="$work/keeper-death-leaderless-group.pid" \
  POLARIS_STEAM_LEADERLESS_MEMBER_FILE="$work/keeper-death-leaderless-member.pid" \
  POLARIS_STEAM_STARTED_FILE="$work/keeper-death-steam.started" \
  POLARIS_STEAM_PID_FILE="$work/keeper-death-steam.pid" \
  POLARIS_STEAM_DESCENDANT_PID_FILE="$work/keeper-death-descendant.pid" \
  POLARIS_STEAM_ARGS="$work/keeper-death-steam.args" \
  PATH="$work/bin:/usr/bin:/bin" \
  POLARIS_SESSION_PATH="$work/bin:/usr/bin:/bin" \
  XDG_RUNTIME_DIR="$work/run" \
  POLARIS_SESSION_INSTANCE_ID=session-A \
    "$work/bin/gamescope" --backend headless -- \
      setpriv --pdeathsig TERM -- bash "$script" nested-primary-child -- steam -gamepadui

  [ -f "$exit_marker" ] || fail "keeper did not publish Steam terminal state before Gamescope death"
  keeper_death_child_pid="$(tr -d '\r\n' <"$work/keeper-death-child.pid")"
  keeper_death_descendant_pid="$(tr -d '\r\n' <"$work/keeper-death-descendant.pid")"
  keeper_death_leaderless_pid="$(tr -d '\r\n' <"$work/keeper-death-leaderless-member.pid")"
  for _ in $(seq 1 200); do
    process_is_live_non_zombie "$keeper_death_child_pid" || break
    sleep 0.02
  done
  process_is_live_non_zombie "$keeper_death_child_pid" &&
    fail "terminal-state keeper survived its Gamescope parent"
  for _ in $(seq 1 200); do
    process_is_live_non_zombie "$keeper_death_descendant_pid" || break
    sleep 0.02
  done
  process_is_live_non_zombie "$keeper_death_descendant_pid" &&
    fail "Steam descendant survived Gamescope death after primary command exit"
  for _ in $(seq 1 200); do
    process_is_live_non_zombie "$keeper_death_leaderless_pid" || break
    sleep 0.02
  done
  process_is_live_non_zombie "$keeper_death_leaderless_pid" &&
    fail "leaderless Steam helper survived Gamescope death after primary command exit"
  keeper_death_child_pid=""
  keeper_death_descendant_pid=""
  keeper_death_leaderless_pid=""
fi

# The production launch must bind wrapper lifetime directly to Gamescope.
grep -Fq 'setpriv --pdeathsig TERM --' "$script" ||
  fail "nested launch does not arm the primary child parent-death signal"
grep -Fq 'bash "$0" nested-primary-child -- "${steam_launch[@]}"' "$script" ||
  fail "nested launch does not use the session-owned primary child"
grep -Fq 'rm -f -- "$nested_primary_exit_file"' "$script" ||
  fail "nested launch does not clear an old primary-child terminal marker"
fence_arm_line="$(grep -nF 'primary_fence_armed=1' "$script" | head -n 1 | cut -d: -f1)"
steam_start_line="$(grep -nF 'steam_spawn_started=1' "$script" | head -n 1 | cut -d: -f1)"
[ -n "$fence_arm_line" ] && [ -n "$steam_start_line" ] \
  && [ "$fence_arm_line" -lt "$steam_start_line" ] ||
  fail "nested launch can start Steam before cleanup authority is fully armed"

printf 'PASS: gamescope primary child keeps natural exit behind the exact fence\n'
