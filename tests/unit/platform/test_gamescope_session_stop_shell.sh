#!/usr/bin/env bash
set -euo pipefail

fail() {
  printf 'FAIL: %s\n' "$*" >&2
  if [ -n "${actions:-}" ] && [ -f "$actions" ]; then
    sed 's/^/  action: /' "$actions" >&2
  fi
  exit 1
}
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
polaris_xwayland_pid() { [ -e "$POLARIS_PROC_ROOT/$1/xwayland" ]; }
polaris_validate_marker() {
  case "${2:-}" in
    nested) [ "${NESTED_VALID:-0}" = 1 ] && [ ! -e "$POLARIS_ACTIONS.nested-stopped" ] ;;
    idle) [ "${IDLE_VALID:-0}" = 1 ] ;;
    *) return 1 ;;
  esac
}
polaris_stop_marked_gamescope() {
  printf 'stop-nested\n' >>"$POLARIS_ACTIONS"
  printf 'stop-credential=%s\n' "${POLARIS_SESSION_INSTANCE_ID:-}" >>"$POLARIS_ACTIONS"
  if [ -n "${STOP_DELAY:-}" ]; then
    sleep "$STOP_DELAY"
  fi
  [ "${STOP_OK:-0}" = 1 ] || return 1
  : >"$POLARIS_ACTIONS.nested-stopped"
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
  *'show -p LoadState --value polaris-gamescope-idle.service'*) printf '%s\n' "${IDLE_LOAD_STATE:-loaded}" ;;
  *'show -p LoadState --value polaris-portal-gamescope.service'*) printf '%s\n' "${PORTAL_LOAD_STATE:-loaded}" ;;
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
case "$*" in
  '-x steam')
    printf '%s\n' "${POLARIS_PGREP_OUTPUT:-}"
    exit "${POLARIS_PGREP_STATUS:-0}"
    ;;
  '-x Xwayland')
    printf '%s\n' "${POLARIS_XWAYLAND_PGREP_OUTPUT:-}"
    exit "${POLARIS_XWAYLAND_PGREP_STATUS:-0}"
    ;;
esac
exit 2
EOF
cat >"$work/bin/kill" <<'EOF'
#!/usr/bin/env bash
printf 'kill %s\n' "$*" >>"$POLARIS_ACTIONS"
signal="${1:-}"
pid="${2:-}"
case "$pid" in ''|*[!0-9]*) exit 1 ;; esac
if [ "$signal" = -TERM ] && [ "${STEAM_IGNORES_TERM:-0}" = 1 ]; then
  exit 0
fi
rm -rf "$POLARIS_PROC_ROOT/$pid"
if [ "${NESTED_EXITS_WITH_STEAM:-0}" = 1 ]; then
  : >"$POLARIS_ACTIONS.nested-stopped"
fi
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
    POLARIS_XWAYLAND_PGREP_OUTPUT="${POLARIS_XWAYLAND_PGREP_OUTPUT:-}" \
    POLARIS_XWAYLAND_PGREP_STATUS="${POLARIS_XWAYLAND_PGREP_STATUS:-0}" \
    POLARIS_STEAM_TERM_STEPS=1 POLARIS_STEAM_KILL_STEPS=1 \
    POLARIS_XWAYLAND_TERM_STEPS=1 POLARIS_XWAYLAND_KILL_STEPS=1 \
    POLARIS_NESTED_EXIT_WAIT_STEPS=1 POLARIS_NESTED_EXIT_WAIT_INTERVAL=0 \
    POLARIS_IDLE_WAIT_STEPS=2 POLARIS_PORTAL_WAIT_STEPS=2 \
    NESTED_VALID="${NESTED_VALID:-0}" STOP_OK="${STOP_OK:-0}" \
    STOP_DELAY="${STOP_DELAY:-}" \
    STEAM_IGNORES_TERM="${STEAM_IGNORES_TERM:-0}" \
    NESTED_EXITS_WITH_STEAM="${NESTED_EXITS_WITH_STEAM:-0}" \
    RECLAIM_OK="${RECLAIM_OK:-0}" IDLE_VALID="${IDLE_VALID:-0}" \
    IDLE_OWNS_SOCKET="${IDLE_OWNS_SOCKET:-0}" WRITE_ENV_OK="${WRITE_ENV_OK:-0}" \
    IDLE_START_OK="${IDLE_START_OK:-1}" PORTAL_RESTART_OK="${PORTAL_RESTART_OK:-1}" \
    PORTAL_READY="${PORTAL_READY:-1}" \
    IDLE_LOAD_STATE="${IDLE_LOAD_STATE:-loaded}" \
    PORTAL_LOAD_STATE="${PORTAL_LOAD_STATE:-loaded}" \
    bash "$script" stop
}
reset_state() {
  rm -rf "$work/run"/*
  rm -f "$actions".*
  mkdir -p "$work/run"
  printf '1\n' >"$work/run/polaris-gamescope-wsi-nested"
  printf 'nested\n' >"$work/run/polaris-gamescope-session-mode"
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

# Dead/invalid nested marker may only advance when sockets are orphan/absent.
# Live foreign ownership (reclaim fails) keeps the durable claim.
reset_state
if NESTED_VALID=0 RECLAIM_OK=0 run_stop >/dev/null 2>&1; then
  fail "foreign-owned nested sockets returned success"
fi
[ "$(tr -d '[:space:]' <"$work/run/polaris-gamescope-wsi-nested")" = 1 ] ||
  fail "foreign ownership advanced its recovery claim"
grep -qx 'reclaim' "$actions" || fail "invalid marker did not attempt orphan reclaim"
! grep -Eq 'unmask|start polaris-gamescope-idle|restart polaris-portal|busctl' "$actions" ||
  fail "foreign ownership advanced idle/portal handoff"

# Dead nested + reclaimable sockets: advance through restore-idle handoff.
reset_state
: >"$actions"
NESTED_VALID=0 RECLAIM_OK=1 STOP_OK=0 IDLE_VALID=1 IDLE_OWNS_SOCKET=1 WRITE_ENV_OK=1 \
  PORTAL_READY=1 run_stop >/dev/null 2>&1 || fail "dead nested with orphan reclaim failed"
[ ! -e "$work/run/polaris-gamescope-wsi-nested" ] || fail "dead nested handoff retained claim"
grep -qx 'reclaim' "$actions" || fail "dead nested did not reclaim orphan sockets"
! grep -qx 'stop-nested' "$actions" || fail "dead nested attempted exact stop-nested"
grep -qx 'unmask-idle' "$actions" || fail "dead nested did not unmask idle"

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

# Distro packages intentionally lack the Nix-only idle and private-portal
# units. Once the exact nested owner is gone and sockets are reclaimable, stop
# must remove only its durable state and return to an empty compositor baseline.
reset_state
printf 'session-A nested standalone\n' >"$work/run/polaris-gamescope-session-state"
rm -f "$work/run/polaris-gamescope-session-id" "$work/run/polaris-gamescope-session-mode"
printf '1929404 30063385 nested /usr/bin/gamescope\n' >"$work/run/polaris-gamescope.pid"
printf 'DISPLAY=:1\n' >"$work/run/polaris-gamescope.env"
IDLE_LOAD_STATE=not-found PORTAL_LOAD_STATE=not-found \
  POLARIS_SESSION_INSTANCE_ID= NESTED_VALID=0 RECLAIM_OK=1 run_stop >/dev/null 2>&1 ||
  fail "standalone package cleanup failed"
[ ! -e "$work/run/polaris-gamescope-wsi-nested" ] || fail "standalone cleanup retained claim"
[ ! -e "$work/run/polaris-gamescope-session-state" ] || fail "standalone cleanup retained session state"
[ ! -e "$work/run/polaris-gamescope.pid" ] || fail "standalone cleanup retained dead marker"
[ ! -e "$work/run/polaris-gamescope.env" ] || fail "standalone cleanup retained runtime env"
grep -qx 'unmask-idle' "$actions" || fail "standalone cleanup retained runtime mask"
! grep -Eq 'start polaris-gamescope-idle|restart polaris-portal|busctl' "$actions" ||
  fail "standalone cleanup tried to start absent Nix services"

# A dead compositor may leave credential-bound Xwayland children behind. Drain
# only that exact generation before declaring standalone recovery complete.
reset_state
printf 'session-A nested standalone\n' >"$work/run/polaris-gamescope-session-state"
rm -f "$work/run/polaris-gamescope-session-id" "$work/run/polaris-gamescope-session-mode"
mkdir -p "$work/proc/200" "$work/proc/201"
printf '20\n' >"$work/proc/200/start"
printf '21\n' >"$work/proc/201/start"
: >"$work/proc/200/xwayland"
: >"$work/proc/201/xwayland"
printf 'HOME=/srv/example\0' >"$work/proc/200/environ"
printf 'HOME=/srv/example\0POLARIS_SESSION_INSTANCE_ID=session-A\0' >"$work/proc/201/environ"
POLARIS_SESSION_INSTANCE_ID='' NESTED_VALID=0 RECLAIM_OK=1 \
  IDLE_LOAD_STATE=not-found PORTAL_LOAD_STATE=not-found \
  POLARIS_XWAYLAND_PGREP_OUTPUT=$'200\n201' run_stop >/dev/null 2>&1 ||
  fail "standalone exact-session Xwayland cleanup failed"
grep -qx 'kill -TERM 201' "$actions" || fail "exact-session Xwayland was not signalled"
! grep -q 'kill .*200' "$actions" || fail "foreign Xwayland was signalled"
[ -d "$work/proc/200" ] || fail "foreign Xwayland process was removed"
[ ! -d "$work/proc/201" ] || fail "exact-session Xwayland survived recovery"
[ ! -e "$work/run/polaris-gamescope-session-state" ] ||
  fail "exact-session Xwayland cleanup retained standalone state"

# Unknown Xwayland enumeration is not evidence that recovery is complete.
reset_state
printf 'session-A nested standalone\n' >"$work/run/polaris-gamescope-session-state"
rm -f "$work/run/polaris-gamescope-session-id" "$work/run/polaris-gamescope-session-mode"
if POLARIS_SESSION_INSTANCE_ID='' NESTED_VALID=0 RECLAIM_OK=1 \
    IDLE_LOAD_STATE=not-found PORTAL_LOAD_STATE=not-found \
    POLARIS_XWAYLAND_PGREP_STATUS=2 run_stop >/dev/null 2>&1; then
  fail "Xwayland enumeration failure was treated as an empty generation"
fi
[ -e "$work/run/polaris-gamescope-session-state" ] ||
  fail "Xwayland enumeration failure cleared the recovery state"
[ "$(tr -d '[:space:]' <"$work/run/polaris-gamescope-wsi-nested")" = 1 ] ||
  fail "Xwayland enumeration failure advanced the recovery claim"

# If the installed service model changes during a launch, do not reinterpret
# that generation. Keep its exact recovery claim for operator remediation.
reset_state
printf 'session-A nested standalone\n' >"$work/run/polaris-gamescope-session-state"
rm -f "$work/run/polaris-gamescope-session-id" "$work/run/polaris-gamescope-session-mode"
if POLARIS_SESSION_INSTANCE_ID= IDLE_LOAD_STATE=loaded PORTAL_LOAD_STATE=loaded \
    NESTED_VALID=0 RECLAIM_OK=1 run_stop >/dev/null 2>&1; then
  fail "changed gamescope service model returned success"
fi
[ "$(tr -d '[:space:]' <"$work/run/polaris-gamescope-wsi-nested")" = restore-idle ] ||
  fail "changed gamescope service model cleared recovery claim"

# A crash after the standalone cleanup commit but before the outer credential
# removal leaves only the atomic state record. Retry must reconstruct the claim
# and finish without inventing a managed idle generation.
reset_state
rm -f "$work/run/polaris-gamescope-wsi-nested" "$work/run/polaris-gamescope.pid"
printf 'session-A nested standalone\n' >"$work/run/polaris-gamescope-session-state"
rm -f "$work/run/polaris-gamescope-session-id" "$work/run/polaris-gamescope-session-mode"
POLARIS_SESSION_INSTANCE_ID= IDLE_LOAD_STATE=not-found PORTAL_LOAD_STATE=not-found \
  NESTED_VALID=0 RECLAIM_OK=1 run_stop >/dev/null 2>&1 ||
  fail "standalone post-commit retry failed"
[ ! -e "$work/run/polaris-gamescope-session-state" ] ||
  fail "standalone post-commit retry retained session state"
[ ! -e "$work/run/polaris-gamescope-wsi-nested" ] ||
  fail "standalone post-commit retry retained reconstructed claim"

# Standalone cleanup never follows or unlinks an untrusted marker symlink.
reset_state
foreign_marker="$work/foreign-marker"
printf 'foreign\n' >"$foreign_marker"
rm -f "$work/run/polaris-gamescope.pid"
ln -s "$foreign_marker" "$work/run/polaris-gamescope.pid"
printf 'session-A nested standalone\n' >"$work/run/polaris-gamescope-session-state"
rm -f "$work/run/polaris-gamescope-session-id" "$work/run/polaris-gamescope-session-mode"
if POLARIS_SESSION_INSTANCE_ID= IDLE_LOAD_STATE=not-found PORTAL_LOAD_STATE=not-found \
    NESTED_VALID=0 RECLAIM_OK=1 run_stop >/dev/null 2>&1; then
  fail "standalone cleanup accepted a marker symlink"
fi
[ "$(<"$foreign_marker")" = foreign ] || fail "standalone cleanup changed symlink target"
[ -L "$work/run/polaris-gamescope.pid" ] || fail "standalone cleanup removed marker symlink"
[ "$(tr -d '[:space:]' <"$work/run/polaris-gamescope-wsi-nested")" = restore-idle ] ||
  fail "standalone symlink rejection cleared recovery claim"

# A partial unit deployment is neither a managed Nix runtime nor a standalone
# package runtime. Keep the recovery claim instead of guessing a baseline.
reset_state
if IDLE_LOAD_STATE=loaded PORTAL_LOAD_STATE=not-found \
    NESTED_VALID=0 RECLAIM_OK=1 run_stop >/dev/null 2>&1; then
  fail "partial gamescope service deployment returned success"
fi
[ "$(tr -d '[:space:]' <"$work/run/polaris-gamescope-wsi-nested")" = restore-idle ] ||
  fail "partial service deployment cleared recovery claim"

# Enumeration and procfs failures are unknown ownership, never "no Steam".
reset_state
if POLARIS_PGREP_STATUS=2 NESTED_VALID=0 STOP_OK=1 run_stop >/dev/null 2>&1; then
  fail "pgrep failure was treated as an empty exact-session drain"
fi
[ "$(tr -d '[:space:]' <"$work/run/polaris-gamescope-wsi-nested")" = 1 ] ||
  fail "pgrep failure advanced the recovery claim"

reset_state
mkdir -p "$work/proc/102"
printf '12\n' >"$work/proc/102/start"
if POLARIS_PGREP_OUTPUT=102 NESTED_VALID=0 STOP_OK=1 run_stop >/dev/null 2>&1; then
  fail "unreadable Steam environment was treated as unowned"
fi
[ "$(tr -d '[:space:]' <"$work/run/polaris-gamescope-wsi-nested")" = 1 ] ||
  fail "unreadable Steam metadata advanced the recovery claim"
rm -rf "$work/proc/102"

# A live nested generation asks credential-bound Steam to exit before using the
# exact process-group fence. Desktop Steam is never signalled or removed.
reset_state
mkdir -p "$work/proc/100" "$work/proc/101"
printf '10\n' >"$work/proc/100/start"
printf '11\n' >"$work/proc/101/start"
printf 'HOME=/srv/example\0' >"$work/proc/100/environ"
printf 'HOME=/srv/example\0POLARIS_SESSION_INSTANCE_ID=session-A\0' >"$work/proc/101/environ"
POLARIS_SESSION_INSTANCE_ID=session-A POLARIS_PGREP_OUTPUT=$'100\n101' \
  NESTED_VALID=1 STOP_OK=1 IDLE_VALID=1 IDLE_OWNS_SOCKET=1 WRITE_ENV_OK=1 \
  PORTAL_READY=1 run_stop >/dev/null 2>&1 || fail "exact-session Steam handoff failed"
grep -qx 'kill -TERM 101' "$actions" || fail "nested teardown did not ask exact-session Steam to exit"
grep -qx 'stop-nested' "$actions" || fail "nested generation did not use its fenced stop"
! grep -q 'kill .*100' "$actions" || fail "desktop Steam was signalled"
[ -d "$work/proc/100" ] || fail "desktop Steam process was removed"
steam_line="$(grep -nFx 'kill -TERM 101' "$actions" | head -n1 | cut -d: -f1)"
fence_line="$(grep -nFx 'stop-nested' "$actions" | head -n1 | cut -d: -f1)"
[ "$steam_line" -lt "$fence_line" ] || fail "nested compositor fence ran before exact-session Steam exit"

# When child exit terminates Gamescope normally, accept only a dead-marker plus
# reclaimable-socket proof and never enter the destructive compositor fence.
reset_state
mkdir -p "$work/proc/101"
printf '11\n' >"$work/proc/101/start"
printf 'POLARIS_SESSION_INSTANCE_ID=session-A\0' >"$work/proc/101/environ"
POLARIS_SESSION_INSTANCE_ID=session-A POLARIS_PGREP_OUTPUT=101 \
  NESTED_VALID=1 NESTED_EXITS_WITH_STEAM=1 STOP_OK=0 RECLAIM_OK=1 \
  IDLE_VALID=1 IDLE_OWNS_SOCKET=1 WRITE_ENV_OK=1 PORTAL_READY=1 \
  run_stop >/dev/null 2>&1 || fail "graceful nested exit handoff failed"
grep -qx 'kill -TERM 101' "$actions" || fail "graceful exit did not signal exact-session Steam"
grep -qx 'reclaim' "$actions" || fail "graceful exit did not prove old sockets reclaimable"
! grep -qx 'stop-nested' "$actions" || fail "graceful exit unnecessarily fenced the compositor"

# Overlapping stop attempts serialize on one lifecycle lock. The first consumes
# the durable state; the waiter then observes an idempotently completed stop.
reset_state
NESTED_VALID=1 STOP_OK=1 STOP_DELAY=0.3 RECLAIM_OK=1 \
  IDLE_VALID=1 IDLE_OWNS_SOCKET=1 WRITE_ENV_OK=1 PORTAL_READY=1 \
  run_stop >/dev/null 2>&1 &
first_stop=$!
for _ in $(seq 1 100); do
  grep -qx 'stop-nested' "$actions" 2>/dev/null && break
  sleep 0.01
done
grep -qx 'stop-nested' "$actions" || fail "first overlapping stop never entered teardown"
NESTED_VALID=1 STOP_OK=1 RECLAIM_OK=1 \
  IDLE_VALID=1 IDLE_OWNS_SOCKET=1 WRITE_ENV_OK=1 PORTAL_READY=1 \
  run_stop >/dev/null 2>&1 &
second_stop=$!
wait "$first_stop" || fail "first overlapping stop failed"
wait "$second_stop" || fail "serialized overlapping stop was not idempotent"
[ "$(grep -cx 'stop-nested' "$actions")" = 1 ] || fail "overlapping stops repeated compositor teardown"

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
grep -qx 'stop-credential=session-A' "$actions" ||
  fail "recovery did not load the persisted exact-session credential"
[ ! -e "$work/run/polaris-gamescope-session-id" ] || fail "successful recovery retained session credential"

# Persisted attach mode is also a durable recovery claim: exact-session Steam
# must be absent before either the mode or credential can be cleared.
reset_state
rm -f "$work/run/polaris-gamescope-wsi-nested"
printf 'attach\n' >"$work/run/polaris-gamescope-session-mode"
printf '0\n' >"$work/run/polaris-gamescope-force"
printf 'session-A\n' >"$work/run/polaris-gamescope-session-id"
mkdir -p "$work/proc/101"
printf '11\n' >"$work/proc/101/start"
printf 'POLARIS_SESSION_INSTANCE_ID=session-A\0' >"$work/proc/101/environ"
POLARIS_SESSION_INSTANCE_ID= POLARIS_PGREP_OUTPUT=101 run_stop >/dev/null 2>&1 ||
  fail "persisted attach recovery failed"
grep -qx 'kill -TERM 101' "$actions" || fail "attach recovery did not terminate exact-session Steam"
[ ! -e "$work/run/polaris-gamescope-session-id" ] || fail "attach recovery cleared no credential"
[ ! -e "$work/run/polaris-gamescope-session-mode" ] || fail "attach recovery retained mode"

# Attach mode has no owned compositor group. If its exact Steam ignores TERM,
# revalidate and KILL only that credential-bound process; desktop Steam stays.
reset_state
rm -f "$work/run/polaris-gamescope-wsi-nested"
printf 'session-A attach standalone\n' >"$work/run/polaris-gamescope-session-state"
rm -f "$work/run/polaris-gamescope-session-id" "$work/run/polaris-gamescope-session-mode"
mkdir -p "$work/proc/100" "$work/proc/101"
printf '10\n' >"$work/proc/100/start"
printf '11\n' >"$work/proc/101/start"
printf 'HOME=/srv/example\0' >"$work/proc/100/environ"
printf 'POLARIS_SESSION_INSTANCE_ID=session-A\0' >"$work/proc/101/environ"
POLARIS_SESSION_INSTANCE_ID= POLARIS_PGREP_OUTPUT=$'100\n101' STEAM_IGNORES_TERM=1 \
  IDLE_LOAD_STATE=not-found PORTAL_LOAD_STATE=not-found run_stop >/dev/null 2>&1 ||
  fail "attach exact-session Steam KILL fallback failed"
grep -qx 'kill -TERM 101' "$actions" || fail "attach fallback did not try TERM first"
grep -qx 'kill -KILL 101' "$actions" || fail "attach fallback did not KILL exact Steam"
! grep -q 'kill .*100' "$actions" || fail "attach fallback signalled desktop Steam"
[ -d "$work/proc/100" ] || fail "attach fallback removed desktop Steam"
[ ! -d "$work/proc/101" ] || fail "attach fallback left exact-session Steam alive"

# New credentials publish ID+mode as one atomic record; stop must consume that
# record and clear it only after exact-session Steam is absent.
reset_state
rm -f "$work/run/polaris-gamescope-wsi-nested" \
  "$work/run/polaris-gamescope-session-id" "$work/run/polaris-gamescope-session-mode"
printf 'session-A attach\n' >"$work/run/polaris-gamescope-session-state"
mkdir -p "$work/proc/101"
printf '11\n' >"$work/proc/101/start"
printf 'POLARIS_SESSION_INSTANCE_ID=session-A\0' >"$work/proc/101/environ"
POLARIS_SESSION_INSTANCE_ID= POLARIS_PGREP_OUTPUT=101 run_stop >/dev/null 2>&1 ||
  fail "atomic persisted attach recovery failed"
[ ! -e "$work/run/polaris-gamescope-session-state" ] || fail "atomic credential survived complete stop"

# Interruption before the one rename leaves no half credential; retry commits one
# complete ID+mode record.
prefix="$work/session-functions.sh"
: >"$prefix"
while IFS= read -r line; do
  [ "$line" = 'case "${1:-}" in' ] && break
  printf '%s\n' "$line" >>"$prefix"
done <"$script"

# The lifecycle lock belongs to the short-lived start/stop operation. The single
# launch subshell must exec the detached target with fd 7 closed while preserving
# $! as that target's PID, process-group leader, and session leader.
(
  export XDG_RUNTIME_DIR="$work/run"
  export POLARIS_GAMESCOPE_RUNTIME_LIB="$work/runtime-stub.sh"
  export POLARIS_FLOCK_BIN="$(command -v flock)"
  # shellcheck source=/dev/null
  . "$prefix"
  acquire_session_operation_lock || fail "could not acquire operation lock for inheritance test"
  parent_identity="$(stat -Lc '%d:%i' "/proc/$BASHPID/fd/7")"
  path_identity="$(stat -Lc '%d:%i' "$session_operation_lock")"
  [ "$parent_identity" = "$path_identity" ] || fail "parent does not hold operation lock"
  child_result="$work/child-operation-lock"
  child_pid=
  cleanup_operation_lock_child() {
    if [ -n "${child_pid:-}" ] && kill -0 "$child_pid" 2>/dev/null; then
      kill -TERM "$child_pid" 2>/dev/null || true
      wait "$child_pid" 2>/dev/null || true
    fi
  }
  trap cleanup_operation_lock_child EXIT
  (
    run_without_session_operation_lock setsid bash -c '
      if [ -e "/proc/$BASHPID/fd/7" ]; then fd7=inherited; else fd7=closed; fi
      process_stat="$(<"/proc/$BASHPID/stat")"
      process_fields="${process_stat#*) }"
      set -- $process_fields
      pgid="$3"
      sid="$4"
      printf "fd7=%s\npid=%s\npgid=%s\nsid=%s\n" "$fd7" "$BASHPID" "$pgid" "$sid"
      trap "exit 0" TERM
      while :; do sleep 1; done
    '
  ) >"$child_result" &
  child_pid=$!
  child_ready=0
  for _ in $(seq 1 100); do
    if [ "$(wc -l <"$child_result")" -ge 4 ]; then
      child_ready=1
      break
    fi
    kill -0 "$child_pid" 2>/dev/null || fail "session child exited before topology capture"
    sleep 0.02
  done
  [ "$child_ready" = 1 ] || fail "session child topology capture timed out"
  [ "$(sed -n '1p' "$child_result")" = fd7=closed ] ||
    fail "session child inherited operation lock fd"
  [ "$(sed -n '2p' "$child_result")" = "pid=$child_pid" ] ||
    fail "launch pid does not identify the session child"
  [ "$(sed -n '3p' "$child_result")" = "pgid=$child_pid" ] ||
    fail "launch pid is not the child process-group leader"
  [ "$(sed -n '4p' "$child_result")" = "sid=$child_pid" ] ||
    fail "launch pid is not the child session leader"
  child_exe="$(readlink -f "/proc/$child_pid/exe")"
  bash_exe="$(readlink -f "$(command -v bash)")"
  [ "$child_exe" = "$bash_exe" ] || fail "launch pid does not identify the target executable"
  kill -TERM "$child_pid"
  wait "$child_pid" || true
  child_pid=
  trap - EXIT
)
grep -Fq 'run_without_session_operation_lock() {' "$script" ||
  fail "operation-lock helper adds an extra subshell"
grep -Fq 'run_without_session_operation_lock setsid env' "$script" ||
  fail "nested Gamescope launch does not drop the operation lock"
grep -Fq 'run_without_session_operation_lock setsid -f env' "$script" ||
  fail "attach Steam launch does not drop the operation lock"
(
  export PATH="$work/bin:$PATH"
  export XDG_RUNTIME_DIR="$work/run"
  export POLARIS_GAMESCOPE_RUNTIME_LIB="$work/runtime-stub.sh"
  export POLARIS_ACTIONS="$actions"
  export IDLE_LOAD_STATE=loaded
  export PORTAL_LOAD_STATE=loaded
  export POLARIS_SESSION_INSTANCE_ID=session-B
  rm -f "$work/run/polaris-gamescope-session-state" \
    "$work/run/polaris-gamescope-session-id" "$work/run/polaris-gamescope-session-mode"
  # shellcheck source=/dev/null
  . "$prefix"
  if POLARIS_SESSION_STATE_BEFORE_COMMIT_HOOK=false publish_session_mode nested; then
    fail "interrupted atomic session publication unexpectedly succeeded"
  fi
  [ ! -e "$session_state_file" ] || fail "interrupted publication exposed a half credential"
  if compgen -G "$session_state_file.tmp.*" >/dev/null; then
    fail "interrupted publication retained a temporary credential"
  fi
  publish_session_mode nested || fail "atomic session publication retry failed"
  [ "$(<"$session_state_file")" = 'session-B nested managed' ] ||
    fail "atomic session record did not contain exact ID and mode"
)
rm -f "$work/run/polaris-gamescope-session-state"

# Failed startup recovery is fail-closed. It may not erase the prior claim,
# marker, socket paths, or credential and continue with a replacement launch.
reset_state
printf 'session-old nested\n' >"$work/run/polaris-gamescope-session-state"
printf 'sentinel\n' >"$work/run/gamescope-0"
before_state="$(sha256sum "$work/run/polaris-gamescope-session-state" | cut -d' ' -f1)"
before_claim="$(sha256sum "$work/run/polaris-gamescope-wsi-nested" | cut -d' ' -f1)"
if env PATH="$work/bin:$PATH" XDG_RUNTIME_DIR="$work/run" \
    POLARIS_GAMESCOPE_RUNTIME_LIB="$work/runtime-stub.sh" POLARIS_PROC_ROOT="$work/proc" \
    POLARIS_ACTIONS="$actions" POLARIS_SESSION_INSTANCE_ID=session-new \
    POLARIS_PGREP_STATUS=0 POLARIS_PGREP_OUTPUT= NESTED_VALID=0 RECLAIM_OK=0 \
    IDLE_LOAD_STATE=loaded PORTAL_LOAD_STATE=loaded \
    bash "$script" start 870780 >/dev/null 2>&1; then
  fail "startup continued after failed prior recovery"
fi
[ "$(sha256sum "$work/run/polaris-gamescope-session-state" | cut -d' ' -f1)" = "$before_state" ] ||
  fail "failed startup recovery rewrote prior credential"
[ "$(sha256sum "$work/run/polaris-gamescope-wsi-nested" | cut -d' ' -f1)" = "$before_claim" ] ||
  fail "failed startup recovery rewrote prior claim"
[ "$(<"$work/run/gamescope-0")" = sentinel ] || fail "failed startup recovery removed socket path"
! grep -q 'mask' "$actions" || fail "failed startup recovery reached replacement launch"

# A crash after nested mode publication but before transition publication is
# recoverable only while exact idle ownership proves destruction never began.
reset_state
rm -f "$work/run/polaris-gamescope-wsi-nested"
printf 'nested\n' >"$work/run/polaris-gamescope-session-mode"
printf 'session-A\n' >"$work/run/polaris-gamescope-session-id"
POLARIS_SESSION_INSTANCE_ID= IDLE_VALID=1 IDLE_OWNS_SOCKET=1 WRITE_ENV_OK=1 PORTAL_READY=1 \
  run_stop >/dev/null 2>&1 || fail "pre-transition nested credential recovery failed"
[ ! -e "$work/run/polaris-gamescope-session-id" ] || fail "pre-transition recovery retained credential"

# A missing claim with a live nested marker remains ambiguous and fails closed.
reset_state
rm -f "$work/run/polaris-gamescope-wsi-nested"
printf 'nested\n' >"$work/run/polaris-gamescope-session-mode"
printf 'session-A\n' >"$work/run/polaris-gamescope-session-id"
if POLARIS_SESSION_INSTANCE_ID= NESTED_VALID=1 IDLE_VALID=0 run_stop >/dev/null 2>&1; then
  fail "claimless live nested generation was adopted without its claim"
fi
[ -e "$work/run/polaris-gamescope-session-id" ] || fail "ambiguous nested failure cleared credential"

grep -Fq 'if [ -e "$session_state_file" ] || [ -s "$session_id_file" ] || [ -f "$rt/polaris-gamescope-wsi-nested" ]; then' "$script" ||
  fail "start does not recover every atomic or legacy credential before replacement"
grep -Fq 'prior session recovery failed; retaining its exact claim' "$script" ||
  fail "start does not fail closed when prior stop recovery fails"
grep -Fq "POLARIS_SESSION_INSTANCE_ID='' bash \"\$0\" stop" "$script" ||
  fail "start does not route persisted attach/nested recovery through credentialed stop"
transition_line="$(grep -nF 'publish_nested_claim transition absent' "$script" | head -n1 | cut -d: -f1)"
mask_line="$(grep -nF 'polaris_mask_idle_unit_runtime' "$script" | tail -n1 | cut -d: -f1)"
[ -n "$transition_line" ] && [ -n "$mask_line" ] && [ "$transition_line" -lt "$mask_line" ] ||
  fail "nested transition claim is not published before idle destruction"
grep -Fq 'publish_nested_claim nested transition' "$script" ||
  fail "nested launch does not CAS transition ownership before spawn"
grep -Fq 'publish_nested_claim nested nested' "$script" ||
  fail "nested portal rebind does not revalidate its claim"
if grep -Eq '^[[:space:]]*publish_nested_claim[[:space:]]*$' "$script"; then
  fail "nested claim helper was called without CAS arguments"
fi
grep -Fq "printf '%s %s %s\\n' \"\$POLARIS_SESSION_INSTANCE_ID\" \"\$mode\" \"\$service_mode\" >\"\$tmp\"" "$script" ||
  fail "session ID and mode are not assembled into one atomic record"
grep -Fq 'mv -f -- "$tmp" "$session_state_file"' "$script" ||
  fail "atomic session record is not committed by one rename"
grep -Fq 'POLARIS_SESSION_STATE_BEFORE_COMMIT_HOOK' "$script" ||
  fail "atomic publication interruption hook is missing"
grep -Fq 'export POLARIS_GAMESCOPE_LOCK_HELD=1' "$script" ||
  fail "portal handoff is not finalized under the ownership lock"
grep -Fq 'acquire_session_operation_lock' "$script" ||
  fail "nested lifecycle operations are not serialized"
grep -Fq 'wait_for_nested_gamescope_exit' "$script" ||
  fail "nested stop does not provide a bounded graceful compositor-exit window"
steam_stop_line="$(grep -nF 'if ! kill_session_steam || ! session_steam_absent; then' "$script" | head -n1 | cut -d: -f1)"
fenced_stop_line="$(grep -nF 'if polaris_stop_marked_gamescope "$marker" nested "$rt"; then' "$script" | head -n1 | cut -d: -f1)"
[ -n "$steam_stop_line" ] && [ -n "$fenced_stop_line" ] && [ "$steam_stop_line" -lt "$fenced_stop_line" ] ||
  fail "nested stop does not order exact-session Steam before compositor fallback"
grep -Fq 'polaris_stop_marked_gamescope "$marker" nested "$rt"' "$script" ||
  fail "nested stop does not use the exact-generation compositor fence"
[ "$(grep -cF 'retire_marked_nested_gamescope_child_first' "$script")" -ge 4 ] ||
  fail "nested startup failures do not share child-first teardown"

echo "PASS: gamescope session stop state machine"
