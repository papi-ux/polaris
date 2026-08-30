#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../../.." && pwd)"
runtime_lib="$repo_root/nix/modules/polaris-gamescope-runtime-lib.sh"
# shellcheck source=/dev/null
. "$runtime_lib"

fail() {
  echo "FAIL: $*" >&2
  exit 1
}

work="$(mktemp -d "${TMPDIR:-/tmp}/polaris-gamescope-shell.XXXXXX")"
trap 'rm -rf "$work"' EXIT
export POLARIS_PROC_ROOT="$work/proc"
export POLARIS_PROC_NET_UNIX="$work/proc/net/unix"
export POLARIS_X11_SOCKET_DIR="$work/tmp/.X11-unix"
export POLARIS_STOP_WAIT_STEPS=2
export POLARIS_FREEZE_WAIT_STEPS=2
mkdir -p "$POLARIS_PROC_ROOT/net" "$POLARIS_X11_SOCKET_DIR" "$work/run" "$work/bin"
cat >"$work/bin/flock" <<'EOF'
#!/usr/bin/env bash
# Unit tests are single-process; production uses util-linux flock on fd 9.
exit 0
EOF
chmod +x "$work/bin/flock"
export POLARIS_FLOCK_BIN="$work/bin/flock"
printf 'lock-sentinel\n' >"$work/run/polaris-gamescope.lock"

write_process_with_group() {
  local pid="$1" ppid="$2" pgrp="$3" sid="$4" start_time="$5" exe="$6"
  shift 6
  local dir="$POLARIS_PROC_ROOT/$pid"
  rm -rf "$dir"
  mkdir -p "$dir/fd"
  ln -s "$exe" "$dir/exe"
  # state(field 3), ppid(field 4), pgrp(field 5), session(field 6),
  # fields 7..21, starttime(field 22)
  {
    printf '%s (%s) S %s %s %s' "$pid" "${exe##*/}" "$ppid" "$pgrp" "$sid"
    for _ in $(seq 1 15); do printf ' 0'; done
    printf ' %s\n' "$start_time"
  } >"$dir/stat"
  printf '%s\0' "$exe" "$@" >"$dir/cmdline"
}

write_process() {
  local pid="$1" ppid="$2" start_time="$3" exe="$4"
  shift 4
  write_process_with_group "$pid" "$ppid" "$pid" "$pid" "$start_time" "$exe" "$@"
}

# Production /proc always includes init and may include kernel-owned sessions
# whose process/session groups are not valid negative-signal authorities. They
# must be ignored by private-session enumeration without weakening the strict
# marker/generation validator used before signalling.
write_process_with_group 1 0 1 1 14 /usr/lib/systemd/systemd --system

write_unix_header() {
  printf 'Num RefCount Protocol Flags Type St Inode Path\n' >"$POLARIS_PROC_NET_UNIX"
}

# Model Linux's asynchronous group stop becoming visible in /proc. Production
# waits for the exact leader and marked compositor to report T/t; a signal-call
# ordering assertion alone would not exercise that contract.
cat >"$work/bin/fake-stop-state" <<'EOF'
#!/usr/bin/env python3
import pathlib
import sys

root = pathlib.Path(sys.argv[1])
target = sys.argv[2]
if not target.startswith("-"):
    raise SystemExit(0)
group = int(target[1:])
for stat_path in root.glob("[0-9]*/stat"):
    line = stat_path.read_text()
    marker = line.rfind(") ")
    if marker < 0:
        continue
    fields = line[marker + 2 :].split()
    if len(fields) < 4 or int(fields[2]) != group:
        continue
    if fields[0] == "Z":
        continue
    fields[0] = "T"
    stat_path.write_text(line[: marker + 2] + " ".join(fields) + "\n")
EOF
chmod +x "$work/bin/fake-stop-state"
cat >"$work/bin/fake-pid-state" <<'EOF'
#!/usr/bin/env python3
import pathlib
import sys

stat_path = pathlib.Path(sys.argv[1]) / sys.argv[2] / "stat"
line = stat_path.read_text()
marker = line.rfind(") ")
if marker < 0:
    raise SystemExit(1)
fields = line[marker + 2 :].split()
fields[0] = sys.argv[3]
stat_path.write_text(line[: marker + 2] + " ".join(fields) + "\n")
EOF
chmod +x "$work/bin/fake-pid-state"

# Rename failure must fail marker publication and leave neither authority nor
# a stale temporary file behind.
write_process 410 1 9001 /usr/bin/gamescope --backend headless
marker_rename_test="$work/run/polaris-marker-rename-test.pid"
mv() { return 1; }
if polaris_write_marker_for_pid "$marker_rename_test" 410 nested; then
  fail "failed marker rename was reported as published"
fi
unset -f mv
[ ! -e "$marker_rename_test" ] || fail "failed marker rename published authority"
if compgen -G "$marker_rename_test.tmp.*" >/dev/null; then
  fail "failed marker rename left a temporary authority file"
fi
polaris_write_marker_for_pid "$marker_rename_test" 410 nested ||
  fail "marker publication did not recover after rename failure"
rm -f "$marker_rename_test"

# A reused PID must never be signalled or allowed to remove owner state.
write_process 410 1 9001 /usr/bin/gamescope --backend headless
: >"$work/run/gamescope-0"
write_unix_header
printf '0000000000000000: 00000002 00000000 00010000 0001 01 500 %s\n' \
  "$work/run/gamescope-0" >>"$POLARIS_PROC_NET_UNIX"
ln -s 'socket:[500]' "$POLARIS_PROC_ROOT/410/fd/3"
printf '410 9000 nested /usr/bin/gamescope\n' >"$work/run/polaris-gamescope.pid"
printf 'DISPLAY=:9\nPOLARIS_GAMESCOPE_PID=410\nPOLARIS_GAMESCOPE_START_TIME=9000\nPOLARIS_GAMESCOPE_EXECUTABLE=/usr/bin/gamescope\n' \
  >"$work/run/polaris-gamescope.env"
cat >"$work/bin/kill" <<EOF
#!/usr/bin/env bash
echo "\$*" >>"$work/kills"
if [ "\${1:-}" = -STOP ]; then
  "$work/bin/fake-stop-state" "$POLARIS_PROC_ROOT" "\${2:-}"
elif [ "\${1:-}" = -KILL ] && [ "\${2:-}" = -410 ]; then
  rm -rf "$POLARIS_PROC_ROOT/410" "$POLARIS_PROC_ROOT/411"
  rm -f "$work/run/gamescope-0"
fi
EOF
chmod +x "$work/bin/kill"
export POLARIS_KILL_BIN="$work/bin/kill"
if polaris_stop_marked_gamescope "$work/run/polaris-gamescope.pid" nested "$work/run"; then
  fail "stale generation was accepted"
fi
[ ! -e "$work/kills" ] || fail "stale generation was signalled"
[ -e "$work/run/gamescope-0" ] || fail "stale generation removed a socket"
[ -e "$work/run/polaris-gamescope.env" ] || fail "stale generation removed runtime env"
[ "$(<"$work/run/polaris-gamescope.lock")" = "lock-sentinel" ] || fail "owner lock open truncated existing data"
write_process_with_group 999 1 999 999 9999 /usr/bin/sleep infinity

# The exact compositor may be a child of the setsid launcher. Signal the
# validated private process group, never assume compositor PID equals PGID.
write_process_with_group 400 1 400 400 9000 /usr/bin/sleep infinity
write_process_with_group 410 1 400 400 9001 /usr/bin/gamescope --backend headless
write_process_with_group 411 410 400 400 9002 /usr/bin/tail -f /dev/null
: >"$work/run/gamescope-0"
write_unix_header
printf '0000000000000000: 00000002 00000000 00010000 0001 01 501 %s\n' \
  "$work/run/gamescope-0" >>"$POLARIS_PROC_NET_UNIX"
ln -s 'socket:[501]' "$POLARIS_PROC_ROOT/410/fd/3"
printf '410 9001 nested /usr/bin/gamescope\n' >"$work/run/polaris-gamescope.pid"
printf 'POLARIS_GAMESCOPE_PID=410\nPOLARIS_GAMESCOPE_START_TIME=9001\nPOLARIS_GAMESCOPE_EXECUTABLE=/usr/bin/gamescope\n' \
  >"$work/run/polaris-gamescope.env"
: >"$work/kills"
cp "$work/bin/kill" "$work/bin/kill.default"
cat >"$work/bin/kill" <<EOF
#!/usr/bin/env bash
echo "\$*" >>"$work/kills"
if [ "\${1:-}" = -STOP ]; then
  "$work/bin/fake-stop-state" "$POLARIS_PROC_ROOT" "\${2:-}"
elif [ "\${1:-}" = -KILL ] && [ "\${2:-}" = -400 ]; then
  if [ "\$(awk '{ print \$3 }' "$POLARIS_PROC_ROOT/410/stat")" != T ]; then
    : >"$work/gamescope-abort"
  fi
  rm -rf "$POLARIS_PROC_ROOT/400" "$POLARIS_PROC_ROOT/410" "$POLARIS_PROC_ROOT/411"
  rm -f "$work/run/gamescope-0"
fi
EOF
chmod +x "$work/bin/kill"
polaris_stop_marked_gamescope "$work/run/polaris-gamescope.pid" nested "$work/run" ||
  fail "private child compositor group did not stop"
grep -qx -- '-STOP -400' "$work/kills" || fail "private process group was not frozen"
grep -qx -- '-KILL -400' "$work/kills" || fail "validated frozen PGID was not killed"
[ ! -e "$work/gamescope-abort" ] || fail "group teardown reached a running wrapped Gamescope"
if grep -q -- '-410' "$work/kills"; then
  fail "compositor PID was used as a process group"
fi
mv "$work/bin/kill.default" "$work/bin/kill"
chmod +x "$work/bin/kill"

# Signal delivery is not the freeze boundary. If the exact leader/compositor do
# not actually report T/t, fail before KILL and resume only the validated group.
write_process_with_group 400 1 400 400 9150 /usr/bin/sleep infinity
write_process_with_group 410 400 400 400 9151 /usr/bin/gamescope --backend headless
printf '410 9151 nested /usr/bin/gamescope\n' >"$work/run/polaris-gamescope.pid"
cat >"$work/bin/kill-no-freeze" <<EOF
#!/usr/bin/env bash
echo "\$*" >>"$work/kills"
EOF
chmod +x "$work/bin/kill-no-freeze"
: >"$work/kills"
if POLARIS_KILL_BIN="$work/bin/kill-no-freeze" POLARIS_FREEZE_WAIT_STEPS=1 \
    polaris_stop_marked_gamescope "$work/run/polaris-gamescope.pid" nested "$work/run"; then
  fail "unobserved group stop advanced to destructive teardown"
fi
grep -qx -- '-STOP -400' "$work/kills" || fail "freeze-timeout fixture did not request group stop"
! grep -q -- '-KILL' "$work/kills" || fail "unobserved group stop sent a destructive signal"
grep -qx -- '-CONT -400' "$work/kills" || fail "pre-commit freeze failure did not resume the exact group"
[ -e "$work/run/polaris-gamescope.pid" ] || fail "freeze-timeout failure cleared marker authority"
rm -rf "$POLARIS_PROC_ROOT/400" "$POLARIS_PROC_ROOT/410"

# Seeing only the leader and marked compositor stop is insufficient: every
# live group member must reach T/t before the commit boundary.
write_process_with_group 400 1 400 400 9170 /usr/bin/sleep infinity
write_process_with_group 410 400 400 400 9171 /usr/bin/gamescope --backend headless
write_process_with_group 411 410 400 400 9172 /usr/bin/Xwayland :2
printf '410 9171 nested /usr/bin/gamescope\n' >"$work/run/polaris-gamescope.pid"
cat >"$work/bin/kill-partial-freeze" <<EOF
#!/usr/bin/env bash
echo "\$*" >>"$work/kills"
if [ "\${1:-}" = -STOP ] && [ "\${2:-}" = -400 ]; then
  "$work/bin/fake-pid-state" "$POLARIS_PROC_ROOT" 400 T
  "$work/bin/fake-pid-state" "$POLARIS_PROC_ROOT" 410 T
fi
EOF
chmod +x "$work/bin/kill-partial-freeze"
: >"$work/kills"
if POLARIS_KILL_BIN="$work/bin/kill-partial-freeze" POLARIS_FREEZE_WAIT_STEPS=1 \
    polaris_stop_marked_gamescope "$work/run/polaris-gamescope.pid" nested "$work/run"; then
  fail "running group member advanced to destructive teardown"
fi
! grep -q -- '-KILL' "$work/kills" || fail "running group member was present before KILL"
grep -qx -- '-CONT -400' "$work/kills" || fail "partial pre-commit freeze did not resume the exact group"
[ -e "$work/run/polaris-gamescope.pid" ] || fail "partial freeze cleared marker authority"
rm -rf "$POLARIS_PROC_ROOT/400" "$POLARIS_PROC_ROOT/410" "$POLARIS_PROC_ROOT/411"

# A descendant that moved to another PGID but retained the private SID must be
# drained through its separately frozen and revalidated group leader.
write_process_with_group 400 1 400 400 9200 /usr/bin/sleep infinity
write_process_with_group 410 400 400 400 9201 /usr/bin/gamescope --backend headless
write_process_with_group 420 410 420 400 9202 /usr/bin/sleep infinity
: >"$work/run/gamescope-0"
write_unix_header
printf 'row row row row row row 802 %s\n' "$work/run/gamescope-0" >>"$POLARIS_PROC_NET_UNIX"
ln -sfn 'socket:[802]' "$POLARIS_PROC_ROOT/410/fd/3"
printf '410 9201 nested /usr/bin/gamescope\n' >"$work/run/polaris-gamescope.pid"
printf 'DISPLAY=:2\n' >"$work/run/polaris-gamescope.env"
cat >"$work/bin/kill-escaped-sid" <<EOF
#!/usr/bin/env bash
echo "\$*" >>"$work/kills"
if [ "\${1:-}" = -STOP ]; then
  "$work/bin/fake-stop-state" "$POLARIS_PROC_ROOT" "\${2:-}"
elif [ "\${1:-}" = -KILL ] && [ "\${2:-}" = -400 ]; then
  rm -rf "$POLARIS_PROC_ROOT/400" "$POLARIS_PROC_ROOT/410"
  rm -f "$work/run/gamescope-0"
elif [ "\${1:-}" = -KILL ] && [ "\${2:-}" = -420 ]; then
  "$work/bin/fake-pid-state" "$POLARIS_PROC_ROOT" 420 Z
fi
EOF
chmod +x "$work/bin/kill-escaped-sid"
: >"$work/kills"
POLARIS_KILL_BIN="$work/bin/kill-escaped-sid" POLARIS_STOP_WAIT_STEPS=1 POLARIS_KILL_WAIT_STEPS=1 \
  polaris_stop_marked_gamescope "$work/run/polaris-gamescope.pid" nested "$work/run" ||
  fail "separate-PGID private-session descendant did not drain"
grep -qx -- '-STOP -420' "$work/kills" || fail "escaped SID process group was not frozen"
grep -qx -- '-KILL -420' "$work/kills" || fail "escaped SID process group was not killed"
[ "$(awk '{ print $3 }' "$POLARIS_PROC_ROOT/420/stat")" = Z ] || fail "zombie fixture was not retained for terminal-state proof"
[ ! -e "$work/run/polaris-gamescope.pid" ] || fail "drained escaped SID retained marker authority"
rm -rf "$POLARIS_PROC_ROOT/400" "$POLARIS_PROC_ROOT/410" "$POLARIS_PROC_ROOT/420"

# A zombie group leader still pins its numeric PGID even though /proc/<pid>/exe
# is unavailable. Freeze and kill a live member through that exact Z leader;
# the stopped original parent must not make this state permanently unretryable.
write_process_with_group 400 1 400 400 9230 /usr/bin/sleep infinity
write_process_with_group 410 400 400 400 9231 /usr/bin/gamescope --backend headless
write_process_with_group 420 410 420 400 9232 /usr/bin/sleep infinity
write_process_with_group 421 420 420 400 9233 /usr/bin/sleep infinity
"$work/bin/fake-pid-state" "$POLARIS_PROC_ROOT" 420 Z
rm -f "$POLARIS_PROC_ROOT/420/exe"
printf '410 9231 nested /usr/bin/gamescope\n' >"$work/run/polaris-gamescope.pid"
cat >"$work/bin/kill-zombie-leader-sid" <<EOF
#!/usr/bin/env bash
echo "\$*" >>"$work/kills"
if [ "\${1:-}" = -STOP ]; then
  "$work/bin/fake-stop-state" "$POLARIS_PROC_ROOT" "\${2:-}"
elif [ "\${1:-}" = -KILL ] && [ "\${2:-}" = -420 ]; then
  rm -rf "$POLARIS_PROC_ROOT/421"
elif [ "\${1:-}" = -KILL ] && [ "\${2:-}" = -400 ]; then
  rm -rf "$POLARIS_PROC_ROOT/400" "$POLARIS_PROC_ROOT/410"
fi
EOF
chmod +x "$work/bin/kill-zombie-leader-sid"
: >"$work/kills"
POLARIS_KILL_BIN="$work/bin/kill-zombie-leader-sid" POLARIS_FREEZE_WAIT_STEPS=1 POLARIS_KILL_WAIT_STEPS=1 \
  polaris_stop_marked_gamescope "$work/run/polaris-gamescope.pid" nested "$work/run" ||
  fail "zombie-led escaped process group did not drain"
grep -qx -- '-STOP -420' "$work/kills" || fail "zombie-led group was not frozen"
grep -qx -- '-KILL -420' "$work/kills" || fail "zombie-led group was not killed"
[ -e "$POLARIS_PROC_ROOT/420/stat" ] || fail "zombie leader fixture did not remain unreaped"
[ ! -e "$work/run/polaris-gamescope.pid" ] || fail "zombie-led drain retained marker authority"
rm -rf "$POLARIS_PROC_ROOT/400" "$POLARIS_PROC_ROOT/410" "$POLARIS_PROC_ROOT/420" "$POLARIS_PROC_ROOT/421"

# A sibling process group without its leader has no immutable PGID reuse
# barrier. Keep the marker rather than signal that ambiguous numeric group.
write_process_with_group 400 1 400 400 9250 /usr/bin/sleep infinity
write_process_with_group 410 400 400 400 9251 /usr/bin/gamescope --backend headless
write_process_with_group 421 410 420 400 9252 /usr/bin/sleep infinity
: >"$work/run/gamescope-0"
write_unix_header
printf 'row row row row row row 803 %s\n' "$work/run/gamescope-0" >>"$POLARIS_PROC_NET_UNIX"
ln -sfn 'socket:[803]' "$POLARIS_PROC_ROOT/410/fd/3"
printf '410 9251 nested /usr/bin/gamescope\n' >"$work/run/polaris-gamescope.pid"
cat >"$work/bin/kill-leaderless-sid" <<EOF
#!/usr/bin/env bash
echo "\$*" >>"$work/kills"
if [ "\${1:-}" = -STOP ]; then
  "$work/bin/fake-stop-state" "$POLARIS_PROC_ROOT" "\${2:-}"
elif [ "\${1:-}" = -KILL ] && [ "\${2:-}" = -400 ]; then
  rm -rf "$POLARIS_PROC_ROOT/400" "$POLARIS_PROC_ROOT/410"
  rm -f "$work/run/gamescope-0"
fi
EOF
chmod +x "$work/bin/kill-leaderless-sid"
: >"$work/kills"
if POLARIS_KILL_BIN="$work/bin/kill-leaderless-sid" POLARIS_STOP_WAIT_STEPS=1 POLARIS_KILL_WAIT_STEPS=1 \
    polaris_stop_marked_gamescope "$work/run/polaris-gamescope.pid" nested "$work/run"; then
  fail "leaderless escaped SID process group was reported drained"
fi
! grep -qx -- '-KILL -420' "$work/kills" || fail "leaderless escaped SID process group was signalled"
[ -e "$work/run/polaris-gamescope.pid" ] || fail "leaderless escaped SID failure cleared marker authority"
! grep -qx -- '-KILL -400' "$work/kills" || fail "leaderless sibling failure destroyed retry authority"
rm -rf "$POLARIS_PROC_ROOT/421"
: >"$work/kills"
POLARIS_KILL_BIN="$work/bin/kill-leaderless-sid" POLARIS_FREEZE_WAIT_STEPS=1 POLARIS_KILL_WAIT_STEPS=1 \
  polaris_stop_marked_gamescope "$work/run/polaris-gamescope.pid" nested "$work/run" ||
  fail "leaderless escaped SID cleanup did not recover after the survivor exited"
grep -qx -- '-KILL -400' "$work/kills" || fail "retry did not terminate the retained exact group"
[ ! -e "$work/run/polaris-gamescope.pid" ] || fail "successful retry retained marker authority"
rm -rf "$POLARIS_PROC_ROOT/400" "$POLARIS_PROC_ROOT/410"

# If the private-session leader generation changes after the destructive group
# signal, numeric PGID authority is lost and cleanup must fail closed.
write_process_with_group 400 1 400 400 9300 /usr/bin/sleep infinity
write_process_with_group 410 400 400 400 9301 /usr/bin/gamescope --backend headless
write_process_with_group 411 410 400 400 9302 /usr/bin/tail -f /dev/null
: >"$work/run/gamescope-0"
write_unix_header
printf 'row row row row row row 801 %s\n' "$work/run/gamescope-0" >>"$POLARIS_PROC_NET_UNIX"
ln -sfn 'socket:[801]' "$POLARIS_PROC_ROOT/410/fd/3"
printf '410 9301 nested /usr/bin/gamescope\n' >"$work/run/polaris-gamescope.pid"
: >"$work/kills"
cat >"$work/bin/kill-reused-leader" <<EOF
#!/usr/bin/env bash
echo "\$*" >>"$work/kills"
if [ "\${1:-}" = -STOP ]; then
  "$work/bin/fake-stop-state" "$POLARIS_PROC_ROOT" "\${2:-}"
elif [ "\${1:-}" = -KILL ]; then
  printf '400 (sleep) S 1 400 400 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 9400\n' >"$POLARIS_PROC_ROOT/400/stat"
  rm -rf "$POLARIS_PROC_ROOT/410"
fi
EOF
chmod +x "$work/bin/kill-reused-leader"
if POLARIS_KILL_BIN="$work/bin/kill-reused-leader" \
    polaris_stop_marked_gamescope "$work/run/polaris-gamescope.pid" nested "$work/run"; then
  fail "recycled private-session leader authorized PGID escalation"
fi
grep -qx -- '-KILL -400' "$work/kills" || fail "ambiguous destructive signal was not exercised"
! grep -q -- '-CONT' "$work/kills" || fail "ambiguous destructive signal resumed a partial generation"
[ -e "$work/run/polaris-gamescope.pid" ] || fail "leader-reuse failure cleared marker authority"
rm -rf "$POLARIS_PROC_ROOT/400" "$POLARIS_PROC_ROOT/410" "$POLARIS_PROC_ROOT/411"

# A non-private or moved compositor group is not authorized for negative
# signaling even when PID/start/executable still match the marker.
write_process_with_group 410 1 400 401 9050 /usr/bin/gamescope --backend headless
printf '410 9050 nested /usr/bin/gamescope\n' >"$work/run/polaris-gamescope.pid"
: >"$work/kills"
if polaris_stop_marked_gamescope "$work/run/polaris-gamescope.pid" nested "$work/run"; then
  fail "mismatched process-group/session identity was accepted"
fi
[ ! -s "$work/kills" ] || fail "non-private process group was signalled"
[ -e "$work/run/polaris-gamescope.pid" ] || fail "failed group proof removed marker authority"

mv "$POLARIS_PROC_ROOT" "$work/proc-group-hidden"
if polaris_private_session_alive 400; then
  fail "missing proc root was treated as a drained private group"
else
  [ "$?" -eq 2 ] || fail "missing proc root did not return unknown group state"
fi
mv "$work/proc-group-hidden" "$POLARIS_PROC_ROOT"

# Nix wrapProgram executes .gamescope-wrapped while preserving argv[0] as
# gamescope. Capture and validate that exact executable instead of rejecting it.
write_process 420 1 9200 /nix/store/fake-gamescope/bin/.gamescope-wrapped --backend headless
printf '/nix/store/fake-gamescope/bin/gamescope\0--backend\0headless\0' >"$POLARIS_PROC_ROOT/420/cmdline"
printf '420 9200 idle /nix/store/fake-gamescope/bin/.gamescope-wrapped\n' >"$work/run/polaris-gamescope.pid"
polaris_validate_marker "$work/run/polaris-gamescope.pid" idle || fail "Nix-wrapped gamescope marker rejected"

# Select only the Xwayland that descends from and is socket-owned by the marker.
write_process 410 1 9001 /usr/bin/gamescope --backend headless --hdr-enabled
write_process 411 410 9002 /usr/bin/Xwayland :4
write_process 412 410 9003 /usr/bin/Xwayland :3
write_process 413 1 8000 /usr/bin/Xwayland :2
rm -f "$POLARIS_PROC_ROOT/412/exe"
ln -s /usr/bin/sleep "$POLARIS_PROC_ROOT/412/exe"
printf '0::/user.slice/polaris.service\n' >"$POLARIS_PROC_ROOT/410/cgroup"
printf '0::/user.slice/polaris.service\n' >"$POLARIS_PROC_ROOT/413/cgroup"
write_process 99 1 100 /usr/bin/Xorg :0
: >"$POLARIS_X11_SOCKET_DIR/X0"
: >"$POLARIS_X11_SOCKET_DIR/X2"
: >"$POLARIS_X11_SOCKET_DIR/X3"
: >"$POLARIS_X11_SOCKET_DIR/X4"
ln -s 'socket:[500]' "$POLARIS_PROC_ROOT/410/fd/3"
ln -s 'socket:[602]' "$POLARIS_PROC_ROOT/413/fd/3"
ln -s 'socket:[603]' "$POLARIS_PROC_ROOT/412/fd/3"
ln -s 'socket:[604]' "$POLARIS_PROC_ROOT/411/fd/3"
ln -s 'socket:[600]' "$POLARIS_PROC_ROOT/99/fd/3"
write_unix_header
printf '0000000000000000: 00000002 00000000 00010000 0001 01 500 %s\n' "$work/run/gamescope-0" >>"$POLARIS_PROC_NET_UNIX"
printf '0000000000000000: 00000002 00000000 00010000 0001 01 600 %s\n' "$POLARIS_X11_SOCKET_DIR/X0" >>"$POLARIS_PROC_NET_UNIX"
printf '0000000000000000: 00000002 00000000 00010000 0001 01 602 %s\n' "$POLARIS_X11_SOCKET_DIR/X2" >>"$POLARIS_PROC_NET_UNIX"
printf '0000000000000000: 00000002 00000000 00010000 0001 01 603 %s\n' "$POLARIS_X11_SOCKET_DIR/X3" >>"$POLARIS_PROC_NET_UNIX"
printf '0000000000000000: 00000002 00000000 00010000 0001 01 604 %s\n' "$POLARIS_X11_SOCKET_DIR/X4" >>"$POLARIS_PROC_NET_UNIX"
printf '410 9001 idle /usr/bin/gamescope\n' >"$work/run/polaris-gamescope.pid"

polaris_validate_marker "$work/run/polaris-gamescope.pid" idle || fail "valid marker rejected"
[ "$(polaris_discover_xwayland_display "$work/run/polaris-gamescope.pid" idle)" = :4 ] ||
  fail "did not select owned Xwayland :4"
polaris_process_has_argument "$work/run/polaris-gamescope.pid" idle --hdr-enabled ||
  fail "exact owner argument was not found"
polaris_write_runtime_env "$work/run/polaris-gamescope.pid" gamescope-0 idle "$work/run" ||
  fail "owned runtime env was not emitted"
grep -qx 'DISPLAY=:4' "$work/run/polaris-gamescope.env" || fail "runtime env routed to host X display"
grep -qx 'POLARIS_GAMESCOPE_PID=410' "$work/run/polaris-gamescope.env" || fail "runtime env lacks owner pid"
[ -e "$POLARIS_X11_SOCKET_DIR/X0" ] || fail "host X0 was removed"

# A failed atomic rename must return failure, publish no environment, remove
# its temporary file, and allow a subsequent valid publication.
rm -f "$work/run/polaris-gamescope.env"
mv() { return 1; }
if polaris_write_runtime_env "$work/run/polaris-gamescope.pid" gamescope-0 idle "$work/run"; then
  fail "failed runtime-env rename was reported as published"
fi
unset -f mv
[ ! -e "$work/run/polaris-gamescope.env" ] || fail "failed runtime-env rename published an environment"
if compgen -G "$work/run/polaris-gamescope.env.tmp.*" >/dev/null; then
  fail "failed runtime-env rename left a temporary file"
fi
polaris_write_runtime_env "$work/run/polaris-gamescope.pid" gamescope-0 idle "$work/run" ||
  fail "runtime-env publication did not recover after rename failure"

# Rebinding Wayland after X11 selection but before env publication must reject
# the entire Wayland/X11 pair and leave no committed environment.
cp "$POLARIS_PROC_NET_UNIX" "$work/unix.before-env-rebind"
cat >"$work/bin/rebind-wayland-before-env" <<EOF
#!/usr/bin/env bash
python3 - "$POLARIS_PROC_NET_UNIX" "$work/run/gamescope-0" <<'PY'
import pathlib, sys
path = pathlib.Path(sys.argv[1])
socket = sys.argv[2]
data = path.read_text()
data = data.replace(f" 500 {socket}\\n", f" 501 {socket}\\n")
path.write_text(data)
PY
EOF
chmod +x "$work/bin/rebind-wayland-before-env"
rm -f "$work/run/polaris-gamescope.env"
if POLARIS_RUNTIME_ENV_BEFORE_COMMIT_HOOK="$work/bin/rebind-wayland-before-env" \
    polaris_write_runtime_env "$work/run/polaris-gamescope.pid" gamescope-0 idle "$work/run"; then
  fail "Wayland pathname rebound was accepted at runtime-env commit"
fi
[ ! -e "$work/run/polaris-gamescope.env" ] || fail "rebound ownership published a runtime env"
mv "$work/unix.before-env-rebind" "$POLARIS_PROC_NET_UNIX"
polaris_write_runtime_env "$work/run/polaris-gamescope.pid" gamescope-0 idle "$work/run" ||
  fail "runtime env did not recover after rejected rebound"

# Duplicate pathname rows represent unlink/rebind generations; holding the old
# inode must not authorize clients to route to the replacement.
cp "$POLARIS_PROC_NET_UNIX" "$work/unix.unique"
printf 'row row row row row row 605 %s\n' "$POLARIS_X11_SOCKET_DIR/X4" >>"$POLARIS_PROC_NET_UNIX"
if polaris_discover_xwayland_display "$work/run/polaris-gamescope.pid" idle >/dev/null; then
  fail duplicate
fi
mv "$work/unix.unique" "$POLARIS_PROC_NET_UNIX"

# Exact valid ownership is signalled by process group and only its runtime
# metadata/socket are removed. The unrelated host X display survives.
polaris_stop_marked_gamescope "$work/run/polaris-gamescope.pid" idle "$work/run" ||
  fail "valid marked generation did not stop"
grep -qx -- '-KILL -410' "$work/kills" || fail "exact frozen process group was not killed"
[ ! -e "$work/run/polaris-gamescope.pid" ] || fail "owned marker survived terminal stop"
[ ! -e "$work/run/polaris-gamescope.env" ] || fail "owned runtime env survived terminal stop"
[ -e "$POLARIS_X11_SOCKET_DIR/X0" ] || fail "host X0 was removed during owned stop"

# A same-role successor replacing the PID generation during an ambiguous KILL
# result must never receive a repeated signal or lose its socket/marker.
write_process 410 1 9100 /usr/bin/gamescope --backend headless
: >"$work/run/gamescope-0"
ln -s 'socket:[700]' "$POLARIS_PROC_ROOT/410/fd/3"
write_unix_header
printf '0000000000000000: 00000002 00000000 00010000 0001 01 700 %s\n' \
  "$work/run/gamescope-0" >>"$POLARIS_PROC_NET_UNIX"
printf '410 9100 idle /usr/bin/gamescope\n' >"$work/run/polaris-gamescope.pid"
: >"$work/kills"
cat >"$work/bin/kill-successor" <<EOF
#!/usr/bin/env bash
echo "\$*" >>"$work/kills"
if [ "\${1:-}" = -STOP ]; then
  "$work/bin/fake-stop-state" "$POLARIS_PROC_ROOT" "\${2:-}"
elif [ "\${1:-}" = -KILL ]; then
  printf '410 (gamescope) S 1 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 9101\n' \
    >"$POLARIS_PROC_ROOT/410/stat"
  printf '410 9101 idle /usr/bin/gamescope\n' >"$work/run/polaris-gamescope.pid"
fi
EOF
chmod +x "$work/bin/kill-successor"
if POLARIS_KILL_BIN="$work/bin/kill-successor" \
    polaris_stop_marked_gamescope "$work/run/polaris-gamescope.pid" idle "$work/run"; then
  fail "predecessor stop accepted a replacement marker as terminal cleanup authority"
fi
[ "$(grep -cFx -- '-KILL -410' "$work/kills")" = 1 ] || fail "predecessor teardown repeated destructive signaling"
! grep -q -- '-CONT' "$work/kills" || fail "predecessor teardown resumed a successor group"
[ "$(<"$work/run/polaris-gamescope.pid")" = '410 9101 idle /usr/bin/gamescope' ] ||
  fail "same-role successor marker was removed"
[ -e "$work/run/gamescope-0" ] || fail "same-role successor socket was removed"

# Duplicate pathname rows are ambiguous after unlink/rebind and must fail closed
# rather than selecting a stale generation by /proc/net/unix row order.
printf '0000000000000001: 00000002 00000000 00010000 0001 01 701 %s\n' \
  "$work/run/gamescope-0" >>"$POLARIS_PROC_NET_UNIX"
if polaris_marker_owns_socket "$work/run/polaris-gamescope.pid" "$work/run/gamescope-0" idle; then
  fail "duplicate socket pathname was accepted as owned"
fi

# A marker removed after KILL also revokes all cleanup authority. Environment
# and socket state must remain untouched because no exact generation is current.
write_process 410 1 9200 /usr/bin/gamescope --backend headless
rm -f "$POLARIS_PROC_ROOT/410/fd/3"
ln -s 'socket:[800]' "$POLARIS_PROC_ROOT/410/fd/3"
: >"$work/run/gamescope-0"
write_unix_header
printf '0000000000000000: 00000002 00000000 00010000 0001 01 800 %s\n' \
  "$work/run/gamescope-0" >>"$POLARIS_PROC_NET_UNIX"
printf '410 9200 idle /usr/bin/gamescope\n' >"$work/run/polaris-gamescope.pid"
printf 'POLARIS_GAMESCOPE_PID=410\nPOLARIS_GAMESCOPE_START_TIME=9200\nPOLARIS_GAMESCOPE_EXECUTABLE=/usr/bin/gamescope\n' \
  >"$work/run/polaris-gamescope.env"
cat >"$work/bin/kill-missing-marker" <<EOF
#!/usr/bin/env bash
echo "\$*" >>"$work/kills"
if [ "\${1:-}" = -STOP ]; then
  "$work/bin/fake-stop-state" "$POLARIS_PROC_ROOT" "\${2:-}"
elif [ "\${1:-}" = -KILL ]; then
  rm -f "$work/run/polaris-gamescope.pid"
fi
EOF
chmod +x "$work/bin/kill-missing-marker"
if POLARIS_KILL_BIN="$work/bin/kill-missing-marker" \
    polaris_stop_marked_gamescope "$work/run/polaris-gamescope.pid" idle "$work/run"; then
  fail "stop accepted missing marker as cleanup authority"
fi
[ -e "$work/run/polaris-gamescope.env" ] || fail "missing marker allowed runtime env cleanup"
[ -e "$work/run/gamescope-0" ] || fail "missing marker allowed socket cleanup"

# Crash residue: filesystem socket with no /proc/net/unix listener is reclaimable.
: >"$work/run/gamescope-0"
: >"$work/run/gamescope-0.lock"
: >"$work/run/gamescope-0-ei"
: >"$work/run/gamescope-0-ei.lock"
write_unix_header
polaris_socket_is_orphan "$work/run/gamescope-0" || fail "filesystem residue not treated as orphan"
polaris_reclaim_orphan_gamescope_sockets "$work/run" || fail "orphan reclaim failed"
[ ! -e "$work/run/gamescope-0" ] || fail "orphan gamescope-0 survived reclaim"
[ ! -e "$work/run/gamescope-0-ei" ] || fail "orphan gamescope-0-ei survived reclaim"
[ -e "$work/run/gamescope-0.lock" ] || fail "reclaim unlinked the reusable Wayland lock"

# A lock file without a socket may belong to a compositor between lock and
# bind. It is harmless when stale and unsafe to unlink while another process
# may hold it.
: >"$work/run/gamescope-0.lock"
polaris_remove_orphan_socket "$work/run/gamescope-0" || fail "missing socket was not accepted"
[ -e "$work/run/gamescope-0.lock" ] || fail "missing socket caused lock-only race cleanup"
polaris_reclaim_orphan_gamescope_sockets "$work/run" || fail "lock-only wrapper state was rejected"
[ -e "$work/run/gamescope-0.lock" ] || fail "production wrapper removed a lock-only state"
rm -f "$work/run/gamescope-0.lock"

# A socket without its authoritative lock can be a split-lock generation from
# an earlier unlink. Reclaim must not create a replacement lock inode.
: >"$work/run/gamescope-0"
write_unix_header
if polaris_remove_orphan_socket "$work/run/gamescope-0"; then
  fail "socket without authoritative lock was reclaimed"
fi
[ -e "$work/run/gamescope-0" ] || fail "lockless socket was removed"
[ ! -e "$work/run/gamescope-0.lock" ] || fail "reclaim created a replacement lock inode"
rm -f "$work/run/gamescope-0"

# A symlink is not the authoritative Wayland lock inode and must not be followed.
: >"$work/run/gamescope-0"
: >"$work/run/foreign-lock"
ln -s "$work/run/foreign-lock" "$work/run/gamescope-0.lock"
if polaris_remove_orphan_socket "$work/run/gamescope-0"; then
  fail "symlinked Wayland lock was accepted"
fi
[ -e "$work/run/gamescope-0" ] || fail "symlink-lock socket was removed"
rm -f "$work/run/gamescope-0" "$work/run/gamescope-0.lock" "$work/run/foreign-lock"

# An unlocked regular replacement lock is still unsafe while a producer holds
# the deleted predecessor inode under the same pathname.
: >"$work/run/gamescope-0"
: >"$work/run/gamescope-0.lock"
mkdir -p "$POLARIS_PROC_ROOT/990/fd"
ln -s "$work/run/gamescope-0.lock (deleted)" "$POLARIS_PROC_ROOT/990/fd/8"
if polaris_remove_orphan_socket "$work/run/gamescope-0"; then
  fail "replacement lock was accepted while deleted predecessor was held"
fi
[ -e "$work/run/gamescope-0" ] || fail "split-lock socket was removed"
rm -rf "$POLARIS_PROC_ROOT/990"
rm -f "$work/run/gamescope-0" "$work/run/gamescope-0.lock"

# Incomplete procfs enumeration is unknown ownership, not an empty holder set.
: >"$work/run/gamescope-0.lock"
mv "$POLARIS_PROC_ROOT" "$work/proc-hidden"
if polaris_wayland_lock_has_no_deleted_holder "$work/run/gamescope-0.lock"; then
  fail "missing proc root was treated as a complete deleted-lock scan"
fi
mv "$work/proc-hidden" "$POLARIS_PROC_ROOT"
rm -f "$work/run/gamescope-0.lock"

# Replacing the lock after socket-table validation must revoke cleanup before
# unlink, even if the replacement is an unlocked regular file.
: >"$work/run/gamescope-0"
: >"$work/run/gamescope-0.lock"
polaris_socket_is_orphan() {
  rm -f "$1.lock"
  : >"$1.lock"
  return 0
}
if polaris_remove_orphan_socket "$work/run/gamescope-0"; then
  fail "bind-window lock replacement did not revoke reclaim"
fi
[ -e "$work/run/gamescope-0" ] || fail "bind-window socket was removed"
# Restore the production helper overridden by this deterministic race fixture.
# shellcheck source=/dev/null
source "$runtime_lib"
rm -f "$work/run/gamescope-0" "$work/run/gamescope-0.lock"

: >"$work/run/gamescope-0"
: >"$work/run/gamescope-0.lock"
polaris_socket_is_orphan() {
  rm -f "$1"
  : >"$1"
  return 0
}
if polaris_remove_orphan_socket "$work/run/gamescope-0"; then
  fail "socket replacement did not revoke reclaim"
fi
[ -e "$work/run/gamescope-0" ] || fail "replacement socket was removed"
if compgen -G "$work/run/gamescope-0.polaris-pin.*" >/dev/null; then
  fail "socket replacement left an inode pin behind"
fi
source "$runtime_lib"
rm -f "$work/run/gamescope-0" "$work/run/gamescope-0.lock"

# Reclaim must take the authoritative Wayland socket lock. A compositor may
# hold that lock before its socket path becomes visible.
: >"$work/run/gamescope-0"
write_unix_header
saved_flock_bin="$POLARIS_FLOCK_BIN"
export POLARIS_FLOCK_BIN=flock
exec 8>>"$work/run/gamescope-0.lock"
flock -x 8
if polaris_reclaim_orphan_gamescope_sockets "$work/run"; then
  fail "held Wayland socket lock did not block production reclaim"
fi
[ -e "$work/run/gamescope-0" ] || fail "socket was removed while its lock was held"
flock -u 8
exec 8>&-
export POLARIS_FLOCK_BIN="$saved_flock_bin"
rm -f "$work/run/gamescope-0" "$work/run/gamescope-0.lock"

# A kernel socket-table row without a visible process holder is still unknown.
# The holder may be hidden by procfs permissions, so fail closed.
: >"$work/run/gamescope-0"
write_unix_header
printf '0000000000000000: 00000002 00000000 00010000 0001 01 901 %s\n' \
  "$work/run/gamescope-0" >>"$POLARIS_PROC_NET_UNIX"
if polaris_socket_is_orphan "$work/run/gamescope-0"; then
  fail "kernel socket row without a visible holder treated as orphan"
fi
if polaris_remove_orphan_socket "$work/run/gamescope-0"; then
  fail "kernel socket row without a visible holder was removed"
fi
[ -e "$work/run/gamescope-0" ] || fail "unknown kernel socket row was removed"

# Live unowned holder must fail closed (do not unlink).
write_process 430 1 9300 /usr/bin/gamescope --backend headless
: >"$work/run/gamescope-0"
ln -s 'socket:[902]' "$POLARIS_PROC_ROOT/430/fd/3"
write_unix_header
printf '0000000000000000: 00000002 00000000 00010000 0001 01 902 %s\n' \
  "$work/run/gamescope-0" >>"$POLARIS_PROC_NET_UNIX"
if polaris_socket_is_orphan "$work/run/gamescope-0"; then
  fail "live unowned holder treated as orphan"
fi
if polaris_reclaim_orphan_gamescope_sockets "$work/run"; then
  fail "live unowned reclaim was allowed"
fi
[ -e "$work/run/gamescope-0" ] || fail "live unowned socket was removed"

# Ambiguous duplicate pathname rows fail closed.
write_unix_header
printf '0000000000000000: 00000002 00000000 00010000 0001 01 903 %s\n' \
  "$work/run/gamescope-0" >>"$POLARIS_PROC_NET_UNIX"
printf '0000000000000001: 00000002 00000000 00010000 0001 01 904 %s\n' \
  "$work/run/gamescope-0" >>"$POLARIS_PROC_NET_UNIX"
if polaris_socket_is_orphan "$work/run/gamescope-0"; then
  fail "ambiguous socket pathname treated as orphan"
fi

# Missing ownership metadata is unknown, not proof of an orphan. Reclaim must
# fail closed and leave the socket path untouched.
saved_proc_net_unix="$POLARIS_PROC_NET_UNIX"
export POLARIS_PROC_NET_UNIX="$work/proc/net/missing-unix"
if polaris_socket_is_orphan "$work/run/gamescope-0"; then
  fail "missing /proc/net/unix treated as proof of an orphan"
fi
if polaris_remove_orphan_socket "$work/run/gamescope-0"; then
  fail "missing /proc/net/unix authorized destructive reclaim"
fi
[ -e "$work/run/gamescope-0" ] || fail "unknown ownership removed the socket"
export POLARIS_PROC_NET_UNIX="$saved_proc_net_unix"

# Production call sites must use exact markers, never process-name-wide pkill/pgrep.
for source in \
  "$repo_root/src/platform/linux/stream_runtime_gamescope.cpp" \
  "$repo_root/nix/modules/polaris-gamescope-session.sh" \
  "$repo_root/nix/modules/session-lib.nix" \
  "$repo_root/scripts/install/lib/polaris-wait-gamescope.sh" \
  "$repo_root/scripts/install/lib/polaris-gamescope-idle.sh"; do
  if grep -Eq "p(kill|grep).*gamescope|gamescope.*p(kill|grep)" "$source"; then
    fail "broad gamescope process matching remains in ${source#$repo_root/}"
  fi
done
grep -q 'gamescope_process::validated_marker' "$repo_root/src/platform/linux/stream_runtime_gamescope.cpp" ||
  fail "C++ runtime does not validate exact marker generation"
grep -q 'polaris_stop_marked_gamescope' "$repo_root/nix/modules/polaris-gamescope-session.sh" ||
  fail "session lifecycle does not stop the marked generation"
grep -q 'POLARIS_GAMESCOPE_SESSION_BIN' "$repo_root/scripts/install/lib/polaris-wait-gamescope.sh" \
  || fail "non-Nix readiness helper does not use credentialed session recovery"
grep -q 'polaris-gamescope-session-id' "$repo_root/scripts/install/lib/polaris-wait-gamescope.sh" \
  || fail "non-Nix readiness helper does not require durable session identity"
grep -q 'publish_nested_claim' "$repo_root/nix/modules/polaris-gamescope-session.sh" \
  || fail "nested claim publication is not ownership-lock serialized"
grep -q 'polaris_validate_marker "$marker" idle' "$repo_root/scripts/install/lib/polaris-wait-gamescope.sh" \
  || fail "non-Nix readiness helper does not require idle marker role"
grep -q 'POLARIS_FLOCK_BIN' "$repo_root/scripts/install/lib/polaris-wait-gamescope.sh" \
  || fail "non-Nix recovery does not hold the ownership transition lock"
grep -q 'polaris_reclaim_orphan_gamescope_sockets' \
  "$repo_root/scripts/install/lib/polaris-gamescope-idle.sh" ||
  fail "idle unit does not reclaim orphan gamescope sockets"
if grep -Eq 'rm .*polaris-gamescope\.pid|rm -f .*polaris-gamescope\.pid' \
    "$repo_root/scripts/install/lib/polaris-wait-gamescope.sh"; then
  fail "non-Nix readiness helper still removes ownership markers unconditionally"
fi
# Nested mask must use user.control (Hjem ~/.config units beat mask --runtime).
grep -q 'polaris_mask_idle_unit_runtime' \
  "$repo_root/nix/modules/polaris-gamescope-session.sh" ||
  fail "nested session does not mask idle via polaris_mask_idle_unit_runtime"
grep -q 'user.control' \
  "$repo_root/nix/modules/polaris-gamescope-runtime-lib.sh" ||
  fail "runtime lib mask helpers do not target user.control"
if grep -E 'systemctl --user mask --runtime polaris-gamescope-idle' \
    "$repo_root/nix/modules/polaris-gamescope-session.sh"; then
  fail "session still uses ineffective mask --runtime for idle"
fi
grep -Fq 'ln -- "$socket" "$socket_pin"' "$runtime_lib" ||
  fail "shell socket reclaim does not pin the original inode"
if compgen -G "$work/run/*.polaris-pin.*" >/dev/null; then
  fail "socket reclaim retained an inode pin"
fi

printf 'PASS: gamescope shell ownership and display routing\n'
