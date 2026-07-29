#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "$0")/../../.." && pwd)"
# shellcheck source=/dev/null
. "$repo_root/nix/modules/polaris-gamescope-runtime-lib.sh"

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
mkdir -p "$POLARIS_PROC_ROOT/net" "$POLARIS_X11_SOCKET_DIR" "$work/run" "$work/bin"
cat >"$work/bin/flock" <<'EOF'
#!/usr/bin/env bash
# Unit tests are single-process; production uses util-linux flock on fd 9.
exit 0
EOF
chmod +x "$work/bin/flock"
export POLARIS_FLOCK_BIN="$work/bin/flock"
printf 'lock-sentinel\n' >"$work/run/polaris-gamescope.lock"

write_process() {
  local pid="$1" ppid="$2" start_time="$3" exe="$4"
  shift 4
  local dir="$POLARIS_PROC_ROOT/$pid"
  rm -rf "$dir"
  mkdir -p "$dir/fd"
  ln -s "$exe" "$dir/exe"
  # state(field 3), ppid(field 4), fields 5..21, starttime(field 22)
  printf '%s (%s) S %s 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 0 %s\n' \
    "$pid" "${exe##*/}" "$ppid" "$start_time" >"$dir/stat"
  printf '%s\0' "$exe" "$@" >"$dir/cmdline"
}

write_unix_header() {
  printf 'Num RefCount Protocol Flags Type St Inode Path\n' >"$POLARIS_PROC_NET_UNIX"
}

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
if [ "\${1:-}" = -TERM ] && [ "\${2:-}" = -410 ]; then
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
rm -f "$POLARIS_PROC_ROOT/412/exe"
ln -s /usr/bin/sleep "$POLARIS_PROC_ROOT/412/exe"
write_process 99 1 100 /usr/bin/Xorg :0
: >"$POLARIS_X11_SOCKET_DIR/X0"
: >"$POLARIS_X11_SOCKET_DIR/X3"
: >"$POLARIS_X11_SOCKET_DIR/X4"
ln -s 'socket:[500]' "$POLARIS_PROC_ROOT/410/fd/3"
ln -s 'socket:[603]' "$POLARIS_PROC_ROOT/412/fd/3"
ln -s 'socket:[604]' "$POLARIS_PROC_ROOT/411/fd/3"
ln -s 'socket:[600]' "$POLARIS_PROC_ROOT/99/fd/3"
write_unix_header
printf '0000000000000000: 00000002 00000000 00010000 0001 01 500 %s\n' "$work/run/gamescope-0" >>"$POLARIS_PROC_NET_UNIX"
printf '0000000000000000: 00000002 00000000 00010000 0001 01 600 %s\n' "$POLARIS_X11_SOCKET_DIR/X0" >>"$POLARIS_PROC_NET_UNIX"
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

# Exact valid ownership is signalled by process group and only its runtime
# metadata/socket are removed. The unrelated host X display survives.
polaris_stop_marked_gamescope "$work/run/polaris-gamescope.pid" idle "$work/run" ||
  fail "valid marked generation did not stop"
grep -qx -- '-TERM -410' "$work/kills" || fail "exact marked process group was not signalled"
[ ! -e "$work/run/polaris-gamescope.pid" ] || fail "owned marker survived terminal stop"
[ ! -e "$work/run/polaris-gamescope.env" ] || fail "owned runtime env survived terminal stop"
[ -e "$POLARIS_X11_SOCKET_DIR/X0" ] || fail "host X0 was removed during owned stop"

# A same-role successor replacing the PID generation during TERM must never
# receive the predecessor's escalation or lose its socket/marker.
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
if [ "\${1:-}" = -TERM ]; then
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
if grep -q -- '-KILL' "$work/kills"; then
  fail "same-role successor received predecessor escalation"
fi
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

# A marker removed after TERM also revokes all cleanup authority. Environment
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
if [ "\${1:-}" = -TERM ]; then
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
write_unix_header
polaris_socket_is_orphan "$work/run/gamescope-0" || fail "filesystem residue not treated as orphan"
polaris_reclaim_orphan_gamescope_sockets "$work/run" || fail "orphan reclaim failed"
[ ! -e "$work/run/gamescope-0" ] || fail "orphan gamescope-0 survived reclaim"
[ ! -e "$work/run/gamescope-0-ei" ] || fail "orphan gamescope-0-ei survived reclaim"
[ ! -e "$work/run/gamescope-0.lock" ] || fail "orphan lock survived reclaim"

# Dead listener inode with no process holder is reclaimable.
: >"$work/run/gamescope-0"
write_unix_header
printf '0000000000000000: 00000002 00000000 00010000 0001 01 901 %s\n' \
  "$work/run/gamescope-0" >>"$POLARIS_PROC_NET_UNIX"
polaris_socket_is_orphan "$work/run/gamescope-0" || fail "dead listener not treated as orphan"
polaris_remove_orphan_socket "$work/run/gamescope-0" || fail "dead listener not removed"
[ ! -e "$work/run/gamescope-0" ] || fail "dead listener socket survived"

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
grep -q 'polaris_stop_marked_gamescope' "$repo_root/scripts/install/lib/polaris-wait-gamescope.sh" ||
  fail "non-Nix readiness helper does not stop nested ownership exactly"
grep -q 'polaris_reclaim_orphan_gamescope_sockets' \
  "$repo_root/scripts/install/lib/polaris-gamescope-idle.sh" ||
  fail "idle unit does not reclaim orphan gamescope sockets"
if grep -Eq 'rm .*polaris-gamescope\.pid|rm -f .*polaris-gamescope\.pid' \
    "$repo_root/scripts/install/lib/polaris-wait-gamescope.sh"; then
  fail "non-Nix readiness helper still removes ownership markers unconditionally"
fi

printf 'PASS: gamescope shell ownership and display routing\n'
