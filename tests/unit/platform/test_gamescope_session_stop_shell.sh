#!/usr/bin/env bash
set -euo pipefail

fail() { printf 'FAIL: %s\n' "$*" >&2; exit 1; }
work="$(mktemp -d "${TMPDIR:-/tmp}/polaris-gamescope-session-stop.XXXXXX")"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/bin" "$work/run" "$work/proc"
actions="$work/actions"
: >"$actions"

cat >"$work/runtime-stub.sh" <<'EOF'
polaris_proc_root() { printf '%s\n' "$POLARIS_PROC_ROOT"; }
polaris_process_fields() {
  [ -r "$POLARIS_PROC_ROOT/$1/start" ] || return 1
  POLARIS_PROCESS_START_TIME="$(<"$POLARIS_PROC_ROOT/$1/start")"
}
polaris_validate_marker() {
  case "${2:-}" in
    nested) [ "${NESTED_VALID:-0}" = 1 ] ;;
    idle) [ "${IDLE_VALID:-0}" = 1 ] ;;
    *) return 1 ;;
  esac
}
polaris_stop_marked_gamescope() {
  printf 'stop-nested\n' >>"$POLARIS_ACTIONS"
  [ "${STOP_OK:-0}" = 1 ]
}
polaris_reclaim_orphan_gamescope_sockets() {
  printf 'reclaim\n' >>"$POLARIS_ACTIONS"
  [ "${RECLAIM_OK:-0}" = 1 ]
}
polaris_unmask_idle_unit_runtime() { printf 'unmask-idle\n' >>"$POLARIS_ACTIONS"; }
polaris_marker_owns_socket() { [ "${IDLE_OWNS_SOCKET:-0}" = 1 ]; }
polaris_write_runtime_env() {
  printf 'write-idle-env\n' >>"$POLARIS_ACTIONS"
  [ "${WRITE_ENV_OK:-0}" = 1 ]
}
EOF

cat >"$work/bin/systemctl" <<'EOF'
#!/usr/bin/env bash
printf 'systemctl %s\n' "$*" >>"$POLARIS_ACTIONS"
case "$*" in
  *'start polaris-gamescope-idle.service'*) [ "${IDLE_START_OK:-1}" = 1 ] ;;
  *'restart polaris-portal-gamescope.service'*) [ "${PORTAL_RESTART_OK:-1}" = 1 ] ;;
  *) exit 0 ;;
esac
EOF
cat >"$work/bin/busctl" <<'EOF'
#!/usr/bin/env bash
printf 'busctl\n' >>"$POLARIS_ACTIONS"
[ "${PORTAL_READY:-1}" = 1 ]
EOF
cat >"$work/bin/pgrep" <<'EOF'
#!/usr/bin/env bash
printf '%s\n' "${POLARIS_PGREP_OUTPUT:-}"
exit "${POLARIS_PGREP_STATUS:-0}"
EOF
cat >"$work/bin/kill" <<'EOF'
#!/usr/bin/env bash
printf 'kill %s\n' "$*" >>"$POLARIS_ACTIONS"
pid="${2:-}"
case "$pid" in ''|*[!0-9]*) exit 1 ;; esac
rm -rf "$POLARIS_PROC_ROOT/$pid"
EOF
chmod +x "$work/bin/"*

script="${POLARIS_SOURCE_DIR:?}/nix/modules/polaris-gamescope-session.sh"
run_stop() {
  env \
    PATH="$work/bin:$PATH" \
    XDG_RUNTIME_DIR="$work/run" \
    POLARIS_GAMESCOPE_RUNTIME_LIB="$work/runtime-stub.sh" \
    POLARIS_PROC_ROOT="$work/proc" \
    POLARIS_ACTIONS="$actions" \
    POLARIS_KILL_BIN="$work/bin/kill" \
    POLARIS_SESSION_INSTANCE_ID="${POLARIS_SESSION_INSTANCE_ID-session-test}" \
    POLARIS_PGREP_OUTPUT="${POLARIS_PGREP_OUTPUT:-}" \
    POLARIS_PGREP_STATUS="${POLARIS_PGREP_STATUS:-0}" \
    POLARIS_IDLE_WAIT_STEPS=2 POLARIS_PORTAL_WAIT_STEPS=2 \
    NESTED_VALID="${NESTED_VALID:-0}" STOP_OK="${STOP_OK:-0}" \
    RECLAIM_OK="${RECLAIM_OK:-0}" IDLE_VALID="${IDLE_VALID:-0}" \
    IDLE_OWNS_SOCKET="${IDLE_OWNS_SOCKET:-0}" WRITE_ENV_OK="${WRITE_ENV_OK:-0}" \
    IDLE_START_OK="${IDLE_START_OK:-1}" PORTAL_RESTART_OK="${PORTAL_RESTART_OK:-1}" \
    PORTAL_READY="${PORTAL_READY:-1}" \
    bash "$script" stop
}
reset_state() {
  rm -rf "$work/run"/*
  mkdir -p "$work/run"
  printf '1\n' >"$work/run/polaris-gamescope-wsi-nested"
  printf '1\n' >"$work/run/polaris-gamescope-force"
  : >"$work/run/polaris-gamescope.pid"
  : >"$actions"
}

# Exact nested stop failure is a recovery state, never a successful handoff.
reset_state
if NESTED_VALID=1 STOP_OK=0 run_stop >/dev/null 2>&1; then
  fail "exact nested stop failure returned success"
fi
[ -e "$work/run/polaris-gamescope-wsi-nested" ] || fail "stop failure cleared nested claim"
[ "$(tr -d '[:space:]' <"$work/run/polaris-gamescope-force")" = 1 ] || fail "stop failure reset force state"
! grep -Eq 'unmask|start polaris-gamescope-idle|restart polaris-portal|busctl' "$actions" ||
  fail "stop failure advanced idle/portal handoff"

# An unclassified nested launch is never converted into orphan-socket cleanup:
# without an exact marker, recovery authority is incomplete and the claim stays.
reset_state
if NESTED_VALID=0 RECLAIM_OK=1 run_stop >/dev/null 2>&1; then
  fail "unclassified nested launch returned success"
fi
[ "$(tr -d '[:space:]' <"$work/run/polaris-gamescope-wsi-nested")" = 1 ] ||
  fail "unclassified launch advanced its recovery claim"
! grep -qx 'reclaim' "$actions" || fail "unclassified launch attempted orphan reclaim"
! grep -Eq 'unmask|start polaris-gamescope-idle|restart polaris-portal|busctl' "$actions" ||
  fail "unclassified launch advanced idle/portal handoff"

# A failed idle handoff retains restore-idle state for a safe retry.
reset_state
if NESTED_VALID=1 STOP_OK=1 IDLE_VALID=0 run_stop >/dev/null 2>&1; then
  fail "missing idle owner returned success"
fi
[ "$(tr -d '[:space:]' <"$work/run/polaris-gamescope-wsi-nested")" = restore-idle ] ||
  fail "idle failure did not retain restore-idle claim"

# Retry from restore-idle skips nested teardown and commits only after portal readiness.
: >"$actions"
NESTED_VALID=0 STOP_OK=0 IDLE_VALID=1 IDLE_OWNS_SOCKET=1 WRITE_ENV_OK=1 \
  PORTAL_READY=1 run_stop >/dev/null 2>&1 || fail "restore-idle retry failed"
[ ! -e "$work/run/polaris-gamescope-wsi-nested" ] || fail "successful handoff retained nested claim"
[ "$(tr -d '[:space:]' <"$work/run/polaris-gamescope-force")" = 0 ] || fail "successful handoff did not reset force"
! grep -qx 'stop-nested' "$actions" || fail "restore-idle retry repeated nested stop"
grep -qx 'write-idle-env' "$actions" || fail "idle runtime environment was not committed"
grep -q 'restart polaris-portal-gamescope.service' "$actions" || fail "portal was not rebound"

# Enumeration and procfs failures are unknown ownership, never "no Steam".
reset_state
if POLARIS_PGREP_STATUS=2 NESTED_VALID=1 STOP_OK=1 run_stop >/dev/null 2>&1; then
  fail "pgrep failure was treated as an empty exact-session drain"
fi
[ "$(tr -d '[:space:]' <"$work/run/polaris-gamescope-wsi-nested")" = 1 ] ||
  fail "pgrep failure advanced the recovery claim"

reset_state
mkdir -p "$work/proc/102"
printf '12\n' >"$work/proc/102/start"
if POLARIS_PGREP_OUTPUT=102 NESTED_VALID=1 STOP_OK=1 run_stop >/dev/null 2>&1; then
  fail "unreadable Steam environment was treated as unowned"
fi
[ "$(tr -d '[:space:]' <"$work/run/polaris-gamescope-wsi-nested")" = 1 ] ||
  fail "unreadable Steam metadata advanced the recovery claim"
rm -rf "$work/proc/102"

# Only Steam carrying the exact Polaris session credential may be signalled.
reset_state
mkdir -p "$work/proc/100" "$work/proc/101"
printf '10\n' >"$work/proc/100/start"
printf '11\n' >"$work/proc/101/start"
printf 'HOME=/srv/example\0' >"$work/proc/100/environ"
printf 'HOME=/srv/example\0POLARIS_SESSION_INSTANCE_ID=session-A\0' >"$work/proc/101/environ"
POLARIS_SESSION_INSTANCE_ID=session-A POLARIS_PGREP_OUTPUT=$'100\n101' \
  NESTED_VALID=1 STOP_OK=1 IDLE_VALID=1 IDLE_OWNS_SOCKET=1 WRITE_ENV_OK=1 \
  PORTAL_READY=1 run_stop >/dev/null 2>&1 || fail "exact-session Steam handoff failed"
grep -qx 'kill -TERM 101' "$actions" || fail "exact-session Steam was not signalled"
! grep -q 'kill .*100' "$actions" || fail "desktop Steam was signalled"
[ -d "$work/proc/100" ] || fail "desktop Steam process was removed"

# Recovery may run in a fresh process environment; the immutable credential is
# persisted until the full idle/portal handoff completes.
reset_state
mkdir -p "$work/proc/101"
printf '11\n' >"$work/proc/101/start"
printf 'POLARIS_SESSION_INSTANCE_ID=session-A\0' >"$work/proc/101/environ"
printf 'session-A\n' >"$work/run/polaris-gamescope-session-id"
POLARIS_SESSION_INSTANCE_ID= POLARIS_PGREP_OUTPUT=101 \
  NESTED_VALID=1 STOP_OK=1 IDLE_VALID=1 IDLE_OWNS_SOCKET=1 WRITE_ENV_OK=1 \
  PORTAL_READY=1 run_stop >/dev/null 2>&1 || fail "persisted-session recovery failed"
grep -qx 'kill -TERM 101' "$actions" || fail "recovery did not use persisted exact-session credential"
[ ! -e "$work/run/polaris-gamescope-session-id" ] || fail "successful recovery retained session credential"

grep -Fq 'if [ -s "$session_id_file" ]; then' "$script" ||
  fail "start does not recover every persisted session credential before replacement"
grep -Fq 'POLARIS_SESSION_INSTANCE_ID= "$0" stop || exit 1' "$script" ||
  fail "start does not route persisted attach/nested recovery through credentialed stop"

echo "PASS: gamescope session stop state machine"
