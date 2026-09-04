#!/bin/bash
# Kept executable as a standalone script so distro packages can install this
# shared body directly. Nix and the manual installer prepend their own wrapper;
# this shebang is then an inert comment.
export PATH="${POLARIS_SESSION_PATH:-$PATH}"

set -euo pipefail
umask 077

if ! declare -F polaris_validate_marker >/dev/null 2>&1; then
  runtime_lib="${POLARIS_GAMESCOPE_RUNTIME_LIB:-$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/polaris-gamescope-runtime-lib.sh}"
  # shellcheck source=/dev/null
  . "$runtime_lib"
fi

rt="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
marker="$rt/polaris-gamescope.pid"
export DBUS_SESSION_BUS_ADDRESS="unix:path=$rt/bus"
# Module option services.polarisGamescopeSession.hdr — allow force-HDR path at all.
allow_client_hdr=1

polaris_valid_dimension() {
  [[ "${1:-}" =~ ^[1-9][0-9]*$ ]]
}

polaris_valid_refresh() {
  [[ "${1:-}" =~ ^[0-9]+([.][0-9]+)?$ ]] &&
    [[ ! "${1:-}" =~ ^0+([.]0+)?$ ]]
}

polaris_runtime_ready_timeout_seconds() {
  local candidate="${POLARIS_GAMESCOPE_READY_TIMEOUT_SECONDS:-30}"
  if ! polaris_valid_dimension "$candidate" || [ "$candidate" -gt 120 ] 2>/dev/null; then
    candidate=30
  fi
  printf '%s\n' "$candidate"
}

polaris_first_valid_geometry_value() {
  local validator="$1" candidate
  shift
  for candidate in "$@"; do
    if "$validator" "$candidate"; then
      printf '%s\n' "$candidate"
      return 0
    fi
  done
  return 1
}

# Prefer the final negotiated/render geometry published by Polaris. Retain the
# legacy HDR overrides and 4K120 defaults for standalone/manual helper use.
gs_width="$(polaris_first_valid_geometry_value polaris_valid_dimension \
  "${POLARIS_SESSION_TARGET_WIDTH:-}" "${POLARIS_HDR_WIDTH:-}" 3840)"
gs_height="$(polaris_first_valid_geometry_value polaris_valid_dimension \
  "${POLARIS_SESSION_TARGET_HEIGHT:-}" "${POLARIS_HDR_HEIGHT:-}" 2160)"
gs_refresh="$(polaris_first_valid_geometry_value polaris_valid_refresh \
  "${POLARIS_SESSION_TARGET_FPS:-}" "${POLARIS_HDR_REFRESH:-}" 120)"

session_id_file="$rt/polaris-gamescope-session-id"
session_mode_file="$rt/polaris-gamescope-session-mode"
session_state_file="$rt/polaris-gamescope-session-state"
session_operation_lock="$rt/polaris-gamescope-session-operation.lock"
nested_primary_exit_file="$rt/polaris-gamescope-primary-child-exit"

polaris_gamescope_service_mode() {
  local idle_state portal_state
  idle_state="$(systemctl --user show -p LoadState --value polaris-gamescope-idle.service 2>/dev/null || true)"
  portal_state="$(systemctl --user show -p LoadState --value polaris-portal-gamescope.service 2>/dev/null || true)"
  case "$idle_state:$portal_state" in
    loaded:loaded)
      printf 'managed\n'
      ;;
    not-found:not-found)
      # Distro packages install the nested-session helper without the Nix-only
      # idle compositor/private portal units. Their safe baseline is no
      # gamescope generation at all, not an impossible idle-unit restoration.
      printf 'standalone\n'
      ;;
    *)
      echo "polaris-gamescope-session: inconsistent gamescope services idle=$idle_state portal=$portal_state" >&2
      return 1
      ;;
  esac
}

acquire_session_operation_lock() {
  local lock_bin="${POLARIS_FLOCK_BIN:-flock}" fd_identity path_identity
  if [ "${POLARIS_SESSION_OPERATION_LOCK_HELD:-0}" = 1 ]; then
    fd_identity="$(stat -Lc '%d:%i' "/proc/$BASHPID/fd/7" 2>/dev/null || true)"
    path_identity="$(stat -Lc '%d:%i' "$session_operation_lock" 2>/dev/null || true)"
    if [ -n "$fd_identity" ] && [ "$fd_identity" = "$path_identity" ]; then
      return 0
    fi
  fi
  unset POLARIS_SESSION_OPERATION_LOCK_HELD
  exec 7>>"$session_operation_lock" || return 1
  "$lock_bin" -x 7 || return 1
  export POLARIS_SESSION_OPERATION_LOCK_HELD=1
}

run_without_session_operation_lock() {
  exec 7>&-
  unset POLARIS_SESSION_OPERATION_LOCK_HELD
  exec "$@"
}

publish_nested_claim() (
  local new_state="$1" expected_state="${2:-absent}"
  local lock_bin="${POLARIS_FLOCK_BIN:-flock}" tmp current_state=absent
  exec 9>>"$rt/polaris-gamescope.lock" || return 1
  "$lock_bin" -x 9 || return 1
  if [ -f "$rt/polaris-gamescope-wsi-nested" ]; then
    current_state="$(tr -d '[:space:]' <"$rt/polaris-gamescope-wsi-nested")"
  fi
  [ "$current_state" = "$expected_state" ] || return 1
  tmp="$rt/.polaris-gamescope-wsi-nested.$$"
  printf '%s\n' "$new_state" >"$tmp" || return 1
  mv -f -- "$tmp" "$rt/polaris-gamescope-wsi-nested"
)

publish_session_mode() (
  local mode="$1" lock_bin="${POLARIS_FLOCK_BIN:-flock}" tmp service_mode
  case "$mode" in attach|nested) ;; *) return 1 ;; esac
  service_mode="$(polaris_gamescope_service_mode)" || return 1
  exec 9>>"$rt/polaris-gamescope.lock" || return 1
  "$lock_bin" -x 9 || return 1
  [ ! -e "$session_state_file" ] \
    && [ ! -e "$session_id_file" ] \
    && [ ! -e "$session_mode_file" ] || return 1
  tmp="$session_state_file.tmp.$$"
  trap 'rm -f -- "$tmp"' EXIT
  printf '%s %s %s\n' "$POLARIS_SESSION_INSTANCE_ID" "$mode" "$service_mode" >"$tmp" || return 1
  if [ -n "${POLARIS_SESSION_STATE_BEFORE_COMMIT_HOOK:-}" ]; then
    eval "$POLARIS_SESSION_STATE_BEFORE_COMMIT_HOOK" || return 1
  fi
  mv -f -- "$tmp" "$session_state_file"
  echo "polaris-gamescope-session: runtime services=$service_mode" >&2
)

complete_standalone_nested_handoff() (
  local nested_claim="$1" lock_bin="${POLARIS_FLOCK_BIN:-flock}" uid marker_identity current_identity
  exec 9>>"$rt/polaris-gamescope.lock" || return 1
  "$lock_bin" -x 9 || return 1
  [ -f "$nested_claim" ] && [ ! -L "$nested_claim" ] \
    && [ "$(tr -d '[:space:]' <"$nested_claim")" = restore-idle ] || return 1
  # The nested owner was already drained and its endpoints were already proved
  # absent/orphaned before this commit. A live generation must never be erased.
  ! polaris_validate_marker "$marker" || return 1
  polaris_reclaim_orphan_gamescope_sockets "$rt" || return 1
  uid="$(id -u)"
  if [ -e "$marker" ] || [ -L "$marker" ]; then
    [ -f "$marker" ] && [ ! -L "$marker" ] \
      && [ "$(stat -Lc '%u:%h' "$marker" 2>/dev/null)" = "$uid:1" ] || return 1
    marker_identity="$(stat -Lc '%d:%i:%f' "$marker" 2>/dev/null)" || return 1
    ! polaris_validate_marker "$marker" || return 1
    current_identity="$(stat -Lc '%d:%i:%f' "$marker" 2>/dev/null)" || return 1
    [ "$current_identity" = "$marker_identity" ] || return 1
  fi
  if [ -e "$rt/polaris-gamescope.env" ] || [ -L "$rt/polaris-gamescope.env" ]; then
    [ -f "$rt/polaris-gamescope.env" ] && [ ! -L "$rt/polaris-gamescope.env" ] \
      && [ "$(stat -Lc '%u:%h' "$rt/polaris-gamescope.env" 2>/dev/null)" = "$uid:1" ] || return 1
  fi
  rm -f -- "$marker" "$rt/polaris-gamescope.env" "$nested_claim"
)

recover_missing_nested_claim() (
  local lock_bin="${POLARIS_FLOCK_BIN:-flock}" tmp persisted persisted_mode persisted_service_mode extra
  exec 9>>"$rt/polaris-gamescope.lock" || return 1
  "$lock_bin" -x 9 || return 1
  [ ! -e "$rt/polaris-gamescope-wsi-nested" ] || return 0
  if [ -f "$session_state_file" ]; then
    read -r persisted persisted_mode persisted_service_mode extra <"$session_state_file" || return 1
    [ -z "${extra:-}" ] \
      && [ "$persisted" = "$POLARIS_SESSION_INSTANCE_ID" ] \
      && [ "$persisted_mode" = nested ] || return 1
    case "${persisted_service_mode:-}" in ''|managed|standalone) ;; *) return 1 ;; esac
  else
    [ -s "$session_id_file" ] \
      && [ "$(tr -d '\r\n' <"$session_id_file")" = "$POLARIS_SESSION_INSTANCE_ID" ] \
      && [ -f "$session_mode_file" ] \
      && [ "$(tr -d '[:space:]' <"$session_mode_file")" = nested ] || return 1
  fi
  # Hold flock only; reclaim/validate do not need POLARIS_GAMESCOPE_LOCK_HELD
  # (that flag is for write_runtime_env / stop_marked to skip nested flock).
  if ! polaris_validate_marker "$marker" idle \
      && ! polaris_reclaim_orphan_gamescope_sockets "$rt"; then
    return 1
  fi
  tmp="$rt/.polaris-gamescope-wsi-nested.$$"
  printf 'transition\n' >"$tmp" || return 1
  mv -f -- "$tmp" "$rt/polaris-gamescope-wsi-nested"
)

remove_nested_claim() (
  local expected_state="$1" lock_bin="${POLARIS_FLOCK_BIN:-flock}" current_state
  exec 9>>"$rt/polaris-gamescope.lock" || return 1
  "$lock_bin" -x 9 || return 1
  [ -f "$rt/polaris-gamescope-wsi-nested" ] || return 1
  current_state="$(tr -d '[:space:]' <"$rt/polaris-gamescope-wsi-nested")"
  [ "$current_state" = "$expected_state" ] || return 1
  rm -f -- "$rt/polaris-gamescope-wsi-nested"
)

load_session_instance_id() {
  local persisted persisted_mode persisted_service_mode extra
  POLARIS_PERSISTED_SESSION_MODE=""
  POLARIS_PERSISTED_SERVICE_MODE=""
  if [ -f "$session_state_file" ]; then
    read -r persisted persisted_mode persisted_service_mode extra <"$session_state_file" || return 1
    [ -z "${extra:-}" ] || return 1
    case "$persisted_mode" in attach|nested) ;; *) return 1 ;; esac
    case "${persisted_service_mode:-}" in ''|managed|standalone) ;; *) return 1 ;; esac
    [ -n "$persisted" ] || return 1
    if [ -n "${POLARIS_SESSION_INSTANCE_ID:-}" ]; then
      [ "$persisted" = "$POLARIS_SESSION_INSTANCE_ID" ] || return 1
    else
      export POLARIS_SESSION_INSTANCE_ID="$persisted"
    fi
    POLARIS_PERSISTED_SESSION_MODE="$persisted_mode"
    POLARIS_PERSISTED_SERVICE_MODE="${persisted_service_mode:-}"
    return 0
  fi
  if [ -n "${POLARIS_SESSION_INSTANCE_ID:-}" ]; then
    if [ -f "$session_id_file" ]; then
      persisted="$(tr -d '\n' <"$session_id_file")" || return 1
      [ "$persisted" = "$POLARIS_SESSION_INSTANCE_ID" ] || return 1
    fi
    return 0
  fi
  [ -f "$session_id_file" ] || return 1
  persisted="$(tr -d '\n' <"$session_id_file")" || return 1
  [ -n "$persisted" ] || return 1
  export POLARIS_SESSION_INSTANCE_ID="$persisted"
}

prepare_nested_runtime_services() {
  load_session_instance_id || return 1
  [ "$POLARIS_PERSISTED_SESSION_MODE" = nested ] || return 1
  case "$POLARIS_PERSISTED_SERVICE_MODE" in
    managed)
      # Only Nix-managed hosts have an idle compositor that can respawn and
      # therefore needs a runtime mask during the nested handoff.
      polaris_mask_idle_unit_runtime
      ;;
    standalone)
      # Distro packages intentionally omit the idle and private-portal units.
      # Masking an absent unit changes LoadState from not-found to masked and
      # makes the persisted standalone service model impossible to restore.
      ;;
    *)
      return 1
      ;;
  esac
}

rebind_private_portal_after_nested_start() {
  local portal_bus portal_ready
  case "$POLARIS_PERSISTED_SERVICE_MODE" in
    managed)
      # Rebind the private portal backend to this gamescope generation. Without
      # this, xdg-desktop-portal-gamescope keeps the prior (idle) wayland
      # connection and Start fails with "gamescope stream not available: failed
      # to connect to wayland socket" when the compositor was replaced under it.
      # Restart only the gamescope impl, not the full portal frontend, so we
      # avoid the udev/controller race that killing xdg-desktop-portal caused.
      # polaris must Wants= (not Requires=) this unit so rebind never cascade-stops it.
      systemctl --user restart polaris-portal-gamescope.service 2>/dev/null || true
      portal_bus="unix:path=$rt/polaris-portal/bus"
      portal_ready=0
      for _ in $(seq 1 80); do
        if busctl --address="$portal_bus" --no-pager \
            status org.freedesktop.impl.portal.desktop.gamescope >/dev/null 2>&1; then
          portal_ready=1
          break
        fi
        sleep 0.1
      done
      if [ "$portal_ready" != 1 ]; then
        echo "polaris-gamescope-session: portal-gamescope did not rebind after nested start" >&2
        # Non-fatal: stream may still gamescopegrab the PW node.
      else
        echo "polaris-gamescope-session: portal-gamescope rebound to nested gamescope-0" >&2
      fi
      ;;
    standalone)
      # Distro packages ship no private portal unit. Polaris captures this
      # generation through the host portal or gamescopegrab, so there is no
      # backend to rebind and no private bus worth eight seconds of polling.
      echo "polaris-gamescope-session: standalone package, no private portal unit to rebind" >&2
      ;;
    *)
      return 1
      ;;
  esac
}

session_steam_pids() {
  local p pids rc envf env_lines session_id="${POLARIS_SESSION_INSTANCE_ID:-}" proc_root
  [ -n "$session_id" ] || return 2
  proc_root="$(polaris_proc_root)"
  if pids="$(pgrep -x steam 2>/dev/null)"; then
    :
  else
    rc=$?
    [ "$rc" -eq 1 ] || return 2
    pids=""
  fi
  for p in $pids; do
    case "$p" in ''|*[!0-9]*) return 2 ;; esac
    envf="$proc_root/$p/environ"
    if [ ! -r "$envf" ]; then
      [ ! -e "$proc_root/$p" ] && continue
      return 2
    fi
    env_lines="$(tr '\0' '\n' <"$envf" 2>/dev/null)" || return 2
    grep -qxF "POLARIS_SESSION_INSTANCE_ID=$session_id" <<<"$env_lines" || continue
    printf '%s\n' "$p"
  done
}

session_steam_alive() {
  local pids
  pids="$(session_steam_pids)" || return 2
  [ -n "$pids" ]
}

session_steam_absent() {
  local rc
  if session_steam_alive; then
    return 1
  else
    rc=$?
  fi
  [ "$rc" -eq 1 ]
}

session_xwayland_pids() {
  local p pids rc envf env_lines session_id="${POLARIS_SESSION_INSTANCE_ID:-}" proc_root
  [ -n "$session_id" ] || return 2
  proc_root="$(polaris_proc_root)"
  if pids="$(pgrep -x Xwayland 2>/dev/null)"; then
    :
  else
    rc=$?
    [ "$rc" -eq 1 ] || return 2
    pids=""
  fi
  for p in $pids; do
    case "$p" in ''|*[!0-9]*) return 2 ;; esac
    envf="$proc_root/$p/environ"
    if [ ! -r "$envf" ]; then
      [ ! -e "$proc_root/$p" ] && continue
      return 2
    fi
    polaris_xwayland_pid "$p" || {
      [ ! -e "$proc_root/$p" ] && continue
      return 2
    }
    env_lines="$(tr '\0' '\n' <"$envf" 2>/dev/null)" || return 2
    grep -qxF "POLARIS_SESSION_INSTANCE_ID=$session_id" <<<"$env_lines" || continue
    printf '%s\n' "$p"
  done
}

session_xwayland_alive() {
  local pids
  pids="$(session_xwayland_pids)" || return 2
  [ -n "$pids" ]
}

session_xwayland_absent() {
  local rc
  if session_xwayland_alive; then
    return 1
  else
    rc=$?
  fi
  [ "$rc" -eq 1 ]
}

signal_session_xwayland() {
  local signal="$1" pid pids start_time env_lines proc_root
  local kill_bin="${POLARIS_KILL_BIN:-kill}" session_id="${POLARIS_SESSION_INSTANCE_ID:-}"
  [ -n "$session_id" ] || return 1
  proc_root="$(polaris_proc_root)"
  pids="$(session_xwayland_pids)" || return 1
  while read -r pid; do
    [ -n "$pid" ] || continue
    if ! polaris_process_fields "$pid"; then
      [ ! -e "$proc_root/$pid" ] && continue
      return 1
    fi
    start_time="$POLARIS_PROCESS_START_TIME"
    if ! polaris_xwayland_pid "$pid"; then
      [ ! -e "$proc_root/$pid" ] && continue
      return 1
    fi
    if ! env_lines="$(tr '\0' '\n' <"$proc_root/$pid/environ" 2>/dev/null)"; then
      [ ! -e "$proc_root/$pid" ] && continue
      return 1
    fi
    grep -qxF "POLARIS_SESSION_INSTANCE_ID=$session_id" <<<"$env_lines" || return 1
    if ! polaris_process_fields "$pid"; then
      [ ! -e "$proc_root/$pid" ] && continue
      return 1
    fi
    [ "$POLARIS_PROCESS_START_TIME" = "$start_time" ] || return 1
    if ! polaris_xwayland_pid "$pid"; then
      [ ! -e "$proc_root/$pid" ] && continue
      return 1
    fi
    if ! env_lines="$(tr '\0' '\n' <"$proc_root/$pid/environ" 2>/dev/null)"; then
      [ ! -e "$proc_root/$pid" ] && continue
      return 1
    fi
    grep -qxF "POLARIS_SESSION_INSTANCE_ID=$session_id" <<<"$env_lines" || return 1
    if ! "$kill_bin" "$signal" "$pid" 2>/dev/null; then
      [ ! -e "$proc_root/$pid" ] && continue
      return 1
    fi
  done <<<"$pids"
}

retire_session_xwayland() {
  local rc
  session_xwayland_absent && return 0
  rc=$?
  [ "$rc" -eq 1 ] || return 1
  signal_session_xwayland -TERM || return 1
  for _ in $(seq 1 "${POLARIS_XWAYLAND_TERM_STEPS:-40}"); do
    if session_xwayland_alive; then
      sleep 0.25
      continue
    else
      rc=$?
      [ "$rc" -eq 1 ] || return 1
      return 0
    fi
  done
  signal_session_xwayland -KILL || return 1
  for _ in $(seq 1 "${POLARIS_XWAYLAND_KILL_STEPS:-20}"); do
    if session_xwayland_alive; then
      sleep 0.1
      continue
    else
      rc=$?
      [ "$rc" -eq 1 ] || return 1
      return 0
    fi
  done
  session_xwayland_absent
}

signal_session_steam() {
  local signal="$1" pid pids start_time env_lines proc_root
  local kill_bin="${POLARIS_KILL_BIN:-kill}" session_id="${POLARIS_SESSION_INSTANCE_ID:-}"
  [ -n "$session_id" ] || return 1
  proc_root="$(polaris_proc_root)"
  pids="$(session_steam_pids)" || return 1
  while read -r pid; do
    [ -n "$pid" ] || continue
    if ! polaris_process_fields "$pid"; then
      [ ! -e "$proc_root/$pid" ] && continue
      return 1
    fi
    start_time="$POLARIS_PROCESS_START_TIME"
    # Bind the numeric PID and environment classification immediately before
    # signaling. Desktop Steam lacks this exact session credential and survives.
    if ! polaris_process_fields "$pid"; then
      [ ! -e "$proc_root/$pid" ] && continue
      return 1
    fi
    [ "$POLARIS_PROCESS_START_TIME" = "$start_time" ] || return 1
    if ! env_lines="$(tr '\0' '\n' <"$proc_root/$pid/environ" 2>/dev/null)"; then
      [ ! -e "$proc_root/$pid" ] && continue
      return 1
    fi
    grep -qxF "POLARIS_SESSION_INSTANCE_ID=$session_id" <<<"$env_lines" || return 1
    if ! "$kill_bin" "$signal" "$pid" 2>/dev/null; then
      [ ! -e "$proc_root/$pid" ] && continue
      return 1
    fi
  done <<<"$pids"
}

kill_session_steam() {
  local rc
  session_steam_absent && return 0
  rc=$?
  [ "$rc" -eq 1 ] || return 1
  signal_session_steam -TERM || return 1
  for _ in $(seq 1 "${POLARIS_STEAM_TERM_STEPS:-40}"); do
    session_steam_absent && return 0
    session_steam_alive || [ "$?" -eq 1 ] || return 1
    sleep 0.25
  done
  # A credential-bound session Steam that ignores TERM must not strand the
  # compositor generation and hand teardown back to an unfenced outer kill.
  # Re-enumerate and revalidate immediately before the exact KILL fallback.
  signal_session_steam -KILL || return 1
  for _ in $(seq 1 "${POLARIS_STEAM_KILL_STEPS:-20}"); do
    session_steam_absent && return 0
    session_steam_alive || [ "$?" -eq 1 ] || return 1
    sleep 0.1
  done
  session_steam_absent
}

publish_nested_primary_child_exit() (
  local tmp
  load_session_instance_id || return 1
  [ "$POLARIS_PERSISTED_SESSION_MODE" = nested ] || return 1
  tmp="$nested_primary_exit_file.tmp.$$"
  trap 'rm -f -- "$tmp"' EXIT
  printf '%s\n' "$POLARIS_SESSION_INSTANCE_ID" >"$tmp" || return 1
  mv -f -- "$tmp" "$nested_primary_exit_file"
)

run_nested_primary_child() {
  local steam_pid="" steam_rc=0 wait_pid="" steam_spawn_started=0
  local primary_reaper_pid="$PPID" primary_reaper_start_time=""
  local primary_reaper_parent_pid="" primary_reaper_executable=""
  local primary_gamescope_pid="" primary_gamescope_start_time=""
  local primary_gamescope_executable=""
  local primary_group_id="" primary_session_id=""
  local primary_wrapper_pid="$$" primary_wrapper_start_time=""
  local primary_fence_armed=0
  local kill_bin="${POLARIS_KILL_BIN:-kill}"
  [ -n "${POLARIS_SESSION_INSTANCE_ID:-}" ] || {
    echo "polaris-gamescope-session: primary child requires an explicit session credential" >&2
    return 1
  }
  load_session_instance_id || {
    echo "polaris-gamescope-session: primary child lacks an exact session credential" >&2
    return 1
  }
  [ "$POLARIS_PERSISTED_SESSION_MODE" = nested ] || {
    echo "polaris-gamescope-session: primary child is not bound to a nested session" >&2
    return 1
  }
  [ "${1:-}" = -- ] && [ "$#" -ge 2 ] || {
    echo "polaris-gamescope-session: primary child requires a Steam command" >&2
    return 2
  }
  shift
  [ "$1" = steam ] || {
    echo "polaris-gamescope-session: primary child refused a non-Steam command" >&2
    return 2
  }

  nested_primary_parent_gone() {
    local child_pid=""
    # Production nested Gamescope is the immutable leader of a private
    # process group and session. Keep this wrapper alive through TERM so it can
    # retire the complete group, including Steam descendants which outlive the
    # direct launcher. The final KILL intentionally includes this wrapper.
    if [ "$primary_fence_armed" = 1 ]; then
      trap '' TERM INT HUP
      signal_session_steam -TERM || true
      "$kill_bin" -TERM "-$primary_group_id" 2>/dev/null || true
      for _ in $(seq 1 "${POLARIS_PRIMARY_STEAM_TERM_STEPS:-20}"); do
        sleep 0.05
      done
      # Gamescope is already gone, so no durable compositor marker remains for
      # a later retry. Keep this exact wrapper alive and positively fence every
      # other member of the private session. This also covers a sibling process
      # group whose leader exited before the parent-death signal arrived.
      while ! polaris_kill_private_session_members_with_authority \
          "$primary_session_id" "$primary_wrapper_pid" \
          "$primary_wrapper_start_time" "$primary_group_id"; do
        sleep 0.05
      done
      signal_session_steam -KILL || true
      "$kill_bin" -KILL "-$primary_group_id" 2>/dev/null || true
      exit 0
    fi

    # Cross-platform unit helpers do not own a real Linux process group. They
    # retain the bounded direct-child fallback while production requires the
    # private group proof below before Steam can start.
    trap - TERM INT HUP
    if [ "$steam_spawn_started" = 1 ]; then
      child_pid="$steam_pid"
      # Bash updates $! as part of starting the asynchronous command. Use it
      # when TERM arrives in the narrow interval before the explicit copy.
      [ -n "$child_pid" ] || child_pid="${!:-}"
    fi
    if [ -n "$child_pid" ] && kill -0 "$child_pid" 2>/dev/null; then
      "$kill_bin" -TERM "$child_pid" 2>/dev/null || true
      for _ in $(seq 1 "${POLARIS_PRIMARY_STEAM_TERM_STEPS:-20}"); do
        kill -0 "$child_pid" 2>/dev/null || break
        sleep 0.05
      done
      if kill -0 "$child_pid" 2>/dev/null; then
        "$kill_bin" -KILL "$child_pid" 2>/dev/null || true
      fi
    fi
    [ -z "$child_pid" ] || wait "$child_pid" 2>/dev/null || true
    exit 0
  }
  nested_primary_keeper_exit() {
    trap - TERM INT HUP
    if [ -n "$wait_pid" ]; then
      kill -TERM "$wait_pid" 2>/dev/null || true
      wait "$wait_pid" 2>/dev/null || true
    fi
    exit 0
  }
  # setpriv gives this wrapper a parent-death signal from Gamescope's
  # gamescopereaper. The reaper has its own parent-death signal from Gamescope,
  # so compositor death is relayed through the exact upstream ownership chain.
  trap nested_primary_parent_gone TERM INT HUP
  # PR_SET_PDEATHSIG cannot report a parent that died before setpriv armed the
  # signal. Re-prove Gamescope -> gamescopereaper -> this wrapper after exec and
  # before Steam starts; later parent death is covered by the installed trap.
  if ! polaris_gamescope_reaper_pid "$primary_reaper_pid"; then
    trap - TERM INT HUP
    echo "polaris-gamescope-session: primary child lost its exact Gamescope reaper before Steam launch" >&2
    return 1
  fi
  if polaris_process_fields "$primary_reaper_pid" 2>/dev/null; then
    primary_reaper_start_time="$POLARIS_PROCESS_START_TIME"
    primary_reaper_parent_pid="$POLARIS_PROCESS_PPID"
    primary_group_id="$POLARIS_PROCESS_PGID"
    primary_session_id="$POLARIS_PROCESS_SESSION_ID"
    primary_reaper_executable="$POLARIS_GAMESCOPE_REAPER_EXECUTABLE"
    primary_gamescope_pid="$primary_reaper_parent_pid"
    if ! polaris_headless_gamescope_pid "$primary_gamescope_pid" \
        || ! polaris_process_fields "$primary_gamescope_pid"; then
      trap - TERM INT HUP
      echo "polaris-gamescope-session: primary child lost its exact Gamescope generation before Steam launch" >&2
      return 1
    fi
    primary_gamescope_start_time="$POLARIS_PROCESS_START_TIME"
    primary_gamescope_executable="$POLARIS_GAMESCOPE_EXECUTABLE"
    [ "$POLARIS_PROCESS_PGID" = "$primary_gamescope_pid" ] \
      && [ "$POLARIS_PROCESS_SESSION_ID" = "$primary_gamescope_pid" ] \
      && [ "$primary_group_id" = "$primary_gamescope_pid" ] \
      && [ "$primary_session_id" = "$primary_gamescope_pid" ] || {
        trap - TERM INT HUP
        echo "polaris-gamescope-session: primary child refused a non-private Gamescope chain" >&2
        return 1
      }
    if ! polaris_gamescope_reaper_pid "$primary_reaper_pid" \
        || ! polaris_gamescope_executables_match \
          "$POLARIS_GAMESCOPE_REAPER_EXECUTABLE" "$primary_reaper_executable" \
        || ! polaris_process_fields "$primary_reaper_pid" \
        || [ "$POLARIS_PROCESS_START_TIME" != "$primary_reaper_start_time" ] \
        || [ "$POLARIS_PROCESS_PPID" != "$primary_gamescope_pid" ] \
        || [ "$POLARIS_PROCESS_PGID" != "$primary_group_id" ] \
        || [ "$POLARIS_PROCESS_SESSION_ID" != "$primary_session_id" ]; then
      trap - TERM INT HUP
      echo "polaris-gamescope-session: primary child lost its private Gamescope reaper before Steam launch" >&2
      return 1
    fi
    if ! polaris_headless_gamescope_pid "$primary_gamescope_pid" \
        || ! polaris_gamescope_executables_match \
          "$POLARIS_GAMESCOPE_EXECUTABLE" "$primary_gamescope_executable" \
        || ! polaris_process_fields "$primary_gamescope_pid" \
        || [ "$POLARIS_PROCESS_START_TIME" != "$primary_gamescope_start_time" ] \
        || [ "$POLARIS_PROCESS_PGID" != "$primary_group_id" ] \
        || [ "$POLARIS_PROCESS_SESSION_ID" != "$primary_session_id" ]; then
      trap - TERM INT HUP
      echo "polaris-gamescope-session: primary child lost its private Gamescope generation before Steam launch" >&2
      return 1
    fi
    if ! polaris_process_fields "$primary_wrapper_pid" \
        || [ "$POLARIS_PROCESS_PPID" != "$primary_reaper_pid" ] \
        || [ "$POLARIS_PROCESS_PGID" != "$primary_group_id" ] \
        || [ "$POLARIS_PROCESS_SESSION_ID" != "$primary_session_id" ] \
        || [ "$POLARIS_PROCESS_STATE" = Z ]; then
      trap - TERM INT HUP
      echo "polaris-gamescope-session: primary child could not prove its cleanup wrapper" >&2
      return 1
    fi
    primary_wrapper_start_time="$POLARIS_PROCESS_START_TIME"
    # Publish the complete cleanup identity to the asynchronous parent-death
    # handler only after every component has been validated and captured.
    primary_fence_armed=1
  elif [ -z "${POLARIS_TEST_GAMESCOPE_PID:-}" ]; then
    trap - TERM INT HUP
    echo "polaris-gamescope-session: primary child could not prove its private Gamescope group" >&2
    return 1
  fi

  steam_spawn_started=1
  "$@" &
  steam_pid=$!
  if wait "$steam_pid"; then
    steam_rc=0
  else
    steam_rc=$?
  fi

  if publish_nested_primary_child_exit; then
    echo "polaris-gamescope-session: exact-session Steam exited (rc=$steam_rc); primary child holding for fenced compositor teardown" >&2
  else
    echo "polaris-gamescope-session: exact-session Steam exited but its terminal marker could not be published; holding for explicit teardown" >&2
  fi

  # Do not return to Gamescope here. Gamescope 3.16 can fault in Vulkan global
  # destruction when its primary child exits. Polaris's exact stop path freezes
  # and SIGKILLs the private generation, so this keeper remains until that
  # already-authorized fence commits. SIGKILL never enters Gamescope's broken
  # exit handlers.
  # Production must retain the exact parent-death fence after Steam's primary
  # command returns: Steam may leave helpers in the private session, including
  # a leaderless sibling group. Cross-platform tests without Linux process
  # identity retain the bounded keeper-only exit path.
  if [ "$primary_fence_armed" != 1 ]; then
    trap nested_primary_keeper_exit TERM INT HUP
  fi
  while :; do
    sleep 3600 &
    wait_pid=$!
    wait "$wait_pid" || true
  done
}

nested_primary_child_exit_matches() {
  [ -f "$nested_primary_exit_file" ] \
    && [ ! -L "$nested_primary_exit_file" ] \
    && [ "$(tr -d '\r\n' <"$nested_primary_exit_file")" = "${POLARIS_SESSION_INSTANCE_ID:-}" ]
}

wait_for_nested_primary_child_exit() {
  for _ in $(seq 1 "${POLARIS_PRIMARY_EXIT_WAIT_STEPS:-100}"); do
    nested_primary_child_exit_matches && return 0
    if [ -e "$nested_primary_exit_file" ] || [ -L "$nested_primary_exit_file" ]; then
      return 1
    fi
    sleep 0.02
  done
  nested_primary_child_exit_matches
}

retire_marked_nested_gamescope_under_fence() (
  local lock_bin="${POLARIS_FLOCK_BIN:-flock}" rc steam_was_present=0
  if [ "${POLARIS_GAMESCOPE_LOCK_HELD:-0}" != 1 ]; then
    exec 9>>"$rt/polaris-gamescope.lock" || return 1
    "$lock_bin" -x 9 || return 1
    export POLARIS_GAMESCOPE_LOCK_HELD=1
  fi
  polaris_validate_marker "$marker" nested || return 1
  if session_steam_alive; then
    steam_was_present=1
  else
    rc=$?
    [ "$rc" -eq 1 ] || return 1
  fi

  # Stop the exact compositor before asking session Steam to exit. The direct
  # primary-child wrapper remains runnable and can publish its exact terminal
  # marker, while Gamescope cannot observe Xwayland disappearing and enter its
  # X11 I/O abort path.
  if ! polaris_freeze_marked_gamescope "$marker" nested "$rt"; then
    echo "polaris-gamescope-session: exact nested compositor did not enter the pre-teardown fence" >&2
    return 1
  fi
  echo "polaris-gamescope-session: exact nested compositor paused before exact-session Steam exit" >&2
  if ! kill_session_steam || ! session_steam_absent; then
    echo "polaris-gamescope-session: exact-session Steam did not reach terminal state" >&2
    return 1
  fi
  if [ "$steam_was_present" = 1 ] && ! wait_for_nested_primary_child_exit; then
    echo "polaris-gamescope-session: exact-session Steam exited without a matching primary-child terminal marker" >&2
    return 1
  fi

  # Commit the already-paused compositor to the complete private-group fence.
  # The pre-frozen contract prevents any failure from resuming Gamescope over
  # the now-terminal Steam/Xwayland generation.
  if POLARIS_GAMESCOPE_PREFROZEN=1 \
      polaris_stop_marked_gamescope "$marker" nested "$rt"; then
    echo "polaris-gamescope-session: exact nested generation fenced after exact-session Steam" >&2
    return 0
  fi
  # The compositor may finish in the narrow interval between the Steam check
  # and the fenced helper's first identity check. Accept only a dead marker and
  # the same safe orphan-socket proof used by recovery; otherwise retain the
  # durable claim for an exact retry.
  if polaris_validate_marker "$marker" nested \
      || ! polaris_reclaim_orphan_gamescope_sockets "$rt"; then
    return 1
  fi
  echo "polaris-gamescope-session: nested owner exited while the exact-generation fence was acquiring authority" >&2
  return 0
)

# True while a real game process for $1 (Steam appid) is running.
# Steam client / webhelper also inherit SteamAppId — exclude those so
# Big Picture alone does not count as "game still up".
steam_app_game_alive() {
  local appid="$1" pid cmd envf env_lines proc_root session_id="${POLARIS_SESSION_INSTANCE_ID:-}"
  [ -n "$appid" ] && [ -n "$session_id" ] || return 1
  proc_root="$(polaris_proc_root)"
  for envf in "$proc_root"/[0-9]*/environ; do
    pid="${envf#"$proc_root"/}"
    pid="${pid%/environ}"
    case "$pid" in
      *[!0-9]*) continue ;;
    esac
    env_lines="$(tr '\0' '\n' <"$envf" 2>/dev/null)" || continue
    if ! grep -qxF "POLARIS_SESSION_INSTANCE_ID=$session_id" <<<"$env_lines" \
        || ! grep -qx "SteamAppId=${appid}" <<<"$env_lines"; then
      continue
    fi
    cmd="$(tr '\0' ' ' <"$proc_root/$pid/cmdline" 2>/dev/null || true)"
    # Skip empty / Steam client helpers (not the game binary).
    if [ -z "$cmd" ]; then
      continue
    fi
    case "$cmd" in
      *steamwebhelper*|*steam-runtime*|*/steam.sh*|*/ubuntu12_32/steam*|*/ubuntu12_64/steam*|*steam\ -gamepadui*|*steam\ -silent*|*srt-logger*)
        continue
        ;;
    esac
    return 0
  done
  return 1
}

case "${1:-}" in
  nested-primary-child)
    run_nested_primary_child "${@:2}"
    ;;
  start)
    acquire_session_operation_lock || {
      echo "polaris-gamescope-session: could not serialize session start" >&2
      exit 1
    }
    requested_session_id="${POLARIS_SESSION_INSTANCE_ID:-}"
    [ -n "$requested_session_id" ] || {
      echo "polaris-gamescope-session: missing immutable session credential" >&2
      exit 1
    }
    if [ -e "$session_state_file" ] || [ -s "$session_id_file" ] || [ -f "$rt/polaris-gamescope-wsi-nested" ]; then
      echo "polaris-gamescope-session: complete prior session recovery before new launch" >&2
      # Re-exec via bash so a non-executable script path still works when $0 is the .sh file.
      if ! POLARIS_SESSION_INSTANCE_ID='' bash "$0" stop; then
        echo "polaris-gamescope-session: prior session recovery failed; retaining its exact claim" >&2
        exit 1
      fi
      POLARIS_SESSION_INSTANCE_ID="$requested_session_id"
      export POLARIS_SESSION_INSTANCE_ID
    fi
    rm -f -- "$nested_primary_exit_file"
    # Soft env hint for nested games (PULSE_SINK / PIPEWIRE_NODE).
    # Polaris claims sink-sunshine-* as the session default (stream capture);
    # do not point children at EasyEffects and do not re-pin sink-inputs here.
    audio_cfg="${POLARIS_CLIENT_AUDIO_CONFIGURATION:-}"
    # Quoted names: shellcheck SC2100 treats unquoted *51 as arithmetic.
    audio_sink="sink-sunshine-surround51"
    audio_map="front-left,front-right,rear-left,rear-right,front-center,lfe"
    audio_desc="Polaris-5.1"
    case "$audio_cfg" in
      *2.0*|*[Ss]tereo*|*2ch*|2)
        audio_sink="sink-sunshine-stereo"
        audio_map="front-left,front-right"
        audio_desc="Polaris-stereo"
        ;;
      *7.1*|7)
        audio_sink="sink-sunshine-surround71"
        audio_map="front-left,front-right,rear-left,rear-right,front-center,lfe,side-left,side-right"
        audio_desc="Polaris-7.1"
        ;;
      *5.1*|5|"")
        audio_sink="sink-sunshine-surround51"
        audio_map="front-left,front-right,rear-left,rear-right,front-center,lfe"
        audio_desc="Polaris-5.1"
        ;;
    esac
    echo "polaris-gamescope-session: audio_cfg=${audio_cfg:-unset} → sink=$audio_sink" >&2

    ensure_null_sink() {
      local name="$1" map="$2" desc="$3"
      if ! pactl list short sinks 2>/dev/null | awk '{print $2}' | grep -qx "$name"; then
        pactl load-module module-null-sink \
          media.class=Audio/Sink \
          sink_name="$name" \
          channel_map="$map" \
          sink_properties="node.description=$desc" \
          >/dev/null 2>&1 || true
      fi
    }
    ensure_null_sink "sink-sunshine-stereo" \
      "front-left,front-right" \
      "Polaris-stereo"
    ensure_null_sink "sink-sunshine-surround51" \
      "front-left,front-right,rear-left,rear-right,front-center,lfe" \
      "Polaris-5.1"
    ensure_null_sink "sink-sunshine-surround71" \
      "front-left,front-right,rear-left,rear-right,front-center,lfe,side-left,side-right" \
      "Polaris-7.1"
    ensure_null_sink "$audio_sink" "$audio_map" "$audio_desc"

    printf '%s\n' "$audio_sink" >"$rt/polaris-gamescope-audio-sink"
    echo "polaris-gamescope-session: audio env sink=$audio_sink (host claims default; soft PULSE_SINK hint only)" >&2

    # True SDR when POLARIS_CLIENT_HDR is false: force file 0 + no --hdr-enabled.
    # Hybrid (HDR gamescope + SDR encode) is the iPhone chroma disaster; polaris 06 also syncs force.
    want_hdr=0
    if [ "$allow_client_hdr" = 1 ]; then
      case "${POLARIS_CLIENT_HDR:-false}" in
        true|TRUE|1|yes|YES) want_hdr=1 ;;
      esac
    fi
    # Nested WSI by default: Steam is gamescope primary child.
    # Polaris prep-cmd strips leading "env FOO=1 …", so WSI cannot be
    # toggled via apps.json env — default on; set POLARIS_GAMESCOPE_WSI=0
    # only for explicit attach experiments.
    # Attach-to-idle often blacks out direct applaunch (no focus/paint).
    want_wsi=1
    case "${POLARIS_GAMESCOPE_WSI:-1}" in
      false|FALSE|0|no|NO) want_wsi=0 ;;
    esac
    if [ "$want_wsi" = 1 ]; then
      publish_session_mode nested || {
        echo "polaris-gamescope-session: could not publish durable nested recovery mode" >&2
        exit 1
      }
    else
      publish_session_mode attach || {
        echo "polaris-gamescope-session: could not publish durable attach recovery mode" >&2
        exit 1
      }
    fi

    # Nested WSI: a session-owned keeper is Gamescope's primary child and
    # launches Steam beneath it. Steam may then reach terminal state without
    # returning from Gamescope's primary child and triggering the known-bad
    # Gamescope 3.16 Vulkan destructor path.
    # Plain "-silent -applaunch" creates a WSI surface but often no
    # PipeWire paint (black stream). BP works because -gamepadui owns
    # focus; direct titles use the same UI + applaunch.
    steam_launch=(steam -gamepadui)
    if [ -n "${2:-}" ]; then
      case "$2" in
        *[!0-9]*)
          echo "polaris-gamescope-session: appid must be numeric, got: $2" >&2
          exit 2
          ;;
      esac
      # Direct library title: same HDR gate as Big Picture
      # (POLARIS_CLIENT_HDR / client profile). No force-SDR.
      steam_launch=(steam -gamepadui -applaunch "$2")
      # wait monitors this: game exit → steam -shutdown → stream ends
      # (otherwise gamepadui returns to BP and the stream never stops).
      printf '%s\n' "$2" >"$rt/polaris-gamescope-appid"
      echo "polaris-gamescope-session: Steam -gamepadui -applaunch $2 (nested WSI, hdr=$want_hdr, exit-on-game-close)" >&2
    else
      rm -f "$rt/polaris-gamescope-appid"
    fi

    prev_force="$(tr -d '[:space:]' <"$rt/polaris-gamescope-force" 2>/dev/null || true)"
    printf '%s\n' "$want_hdr" >"$rt/polaris-gamescope-force"
    if [ "$want_hdr" = 1 ]; then
      echo "polaris-gamescope-session: HDR session (force=1, ${POLARIS_GAMESCOPE_BIN:-gamescope} --hdr-enabled when started)" >&2
    else
      echo "polaris-gamescope-session: true SDR (force=0, no --hdr-enabled; not hybrid PQ+SDR)" >&2
    fi

    if [ "$want_wsi" = 1 ]; then
      # --- Nested WSI path (games as gamescope child) ---
      echo "polaris-gamescope-session: WSI nested mode — exact-session keeper owns ${POLARIS_GAMESCOPE_BIN:-gamescope} primary-child lifetime" >&2
      # Publish a fenced transition before masking/stopping idle ownership so no
      # readiness actor can interpret the destructive handoff as an unowned gap.
      publish_nested_claim transition absent || {
        echo "polaris-gamescope-session: another ownership transition is already active" >&2
        exit 1
      }
      # Runtime-mask only a managed idle unit so polaris Wants= /
      # portal-gamescope Wants= cannot respawn it while nested needs exclusive
      # gamescope-0. Standalone packages have no such unit to mask.
      prepare_nested_runtime_services || {
        echo "polaris-gamescope-session: could not prepare nested runtime services" >&2
        exit 1
      }
      # Stop only the exact marked owner of gamescope-0. Unknown sockets fail
      # closed instead of risking another user's compositor.
      if polaris_validate_marker "$marker"; then
        case "$POLARIS_MARKER_ROLE" in
          idle|nested)
            polaris_stop_marked_gamescope "$marker" "$POLARIS_MARKER_ROLE" "$rt" || {
              echo "polaris-gamescope-session: marked $POLARIS_MARKER_ROLE owner did not stop" >&2
              exit 1
            }
            ;;
          *)
            echo "polaris-gamescope-session: refusing to replace marked $POLARIS_MARKER_ROLE owner" >&2
            exit 1
            ;;
        esac
      elif ! polaris_reclaim_orphan_gamescope_sockets "$rt"; then
        echo "polaris-gamescope-session: refusing unowned gamescope socket cleanup" >&2
        exit 1
      fi
      for _ in $(seq 1 "${POLARIS_IDLE_WAIT_STEPS:-100}"); do
        [ ! -S "$rt/gamescope-0" ] && [ ! -S "$rt/gamescope-1" ] && break
        sleep 0.1
      done
      if [ -S "$rt/gamescope-0" ] || [ -S "$rt/gamescope-1" ]; then
        echo "polaris-gamescope-session: headless ${POLARIS_GAMESCOPE_BIN:-gamescope} socket still held after stop" >&2
        exit 1
      fi

      # shellcheck source=/dev/null

      prefer_vk=()
      if [ -n "${POLARIS_GAMESCOPE_PREFER_VK:-}" ]; then
        prefer_vk=(--prefer-vk-device "$POLARIS_GAMESCOPE_PREFER_VK")
      fi
      hdr_flags=()
      if [ "$want_hdr" = 1 ]; then
        # Nested: --hdr-enabled only (no --hdr-debug-force-*).
        # WSI can still create HDR10 swapchains; PW spa 81 may need force later.
        hdr_flags=(
          --hdr-enabled
          --sdr-gamut-wideness 0.000000
          --hdr-sdr-content-nits 203
        )
        echo "polaris-gamescope-session: nested HDR (enabled, no debug-force-*)" >&2
      fi
      # Always --steam on nested WSI (input + multi-xwayland Steam integration).
      steam_flags=(--steam)

      # gamescope itself setenv(ENABLE_GAMESCOPE_WSI,1) for nested children.
      # Do NOT set ENABLE_HDR_WSI (separate VK_hdr_layer; can break Gamescope WSI).
      # Do NOT pass host WAYLAND_DISPLAY into children: FROG WSI only creates
      # Gamescope surfaces when GAMESCOPE_WAYLAND_DISPLAY is set and does not
      # conflict with another non-empty WAYLAND_DISPLAY (KWin wayland-0).
      # Desktop nested AC6 success had WAYLAND_DISPLAY unset on the game.
      # No GAMESCOPE_WSI_FORCE_BYPASS: bypass kept swapchains non-HDR.
      child_env=(
        # Stream capture only: leave system default + EasyEffects alone.
        PULSE_SINK="$audio_sink"
        PIPEWIRE_NODE="$audio_sink"
        POLARIS_SESSION_AUDIO_SINK="$audio_sink"
        STEAM_MULTIPLE_XWAYLANDS=1
        QT_QPA_PLATFORM=xcb
        DISABLE_HDR_WSI=1
      )
      if [ "$want_hdr" = 1 ]; then
        child_env+=(STEAM_GAMESCOPE_HDR_SUPPORTED=1 DXVK_HDR=1)
      fi

      steam_log="$(mktemp "$rt/polaris-gamescope-steam-wsi.XXXXXX.log")"
      # The keeper launches Steam in Gamescope's WSI environment but remains
      # alive after Steam exits so the exact stop fence, not Gamescope's natural
      # destructor path, owns compositor retirement. Portal still captures
      # gamescope-0.
      # Omit --expose-wayland so children stay on XWayland + GAMESCOPE_WAYLAND_DISPLAY
      # (WSI X11 path). Portal uses compositor socket gamescope-0, not child WAYLAND.
      # env -u: drop host KWin Wayland/DISPLAY inherited from polaris user session.
      echo "polaris-gamescope-session: nested geometry ${gs_width}x${gs_height}@${gs_refresh}" >&2
      # Claim ownership before launch so marker-capture failure remains
      # recoverable without an unsafe numeric PGID fallback.
      publish_nested_claim nested transition || {
        echo "polaris-gamescope-session: ownership transition changed before nested launch" >&2
        exit 1
      }
      (
        # SteamOS ships Gamescope with cap_sys_nice=eip. Prevent that file
        # capability from changing procfs ownership/readability: exact marker
        # publication and socket ownership checks must remain possible for the
        # same unprivileged user that launched this private compositor.
        run_without_session_operation_lock setsid env -u WAYLAND_DISPLAY -u DISPLAY -u ENABLE_HDR_WSI \
          "${child_env[@]}" setpriv --no-new-privs -- \
          "${POLARIS_GAMESCOPE_BIN:-gamescope}" \
          --backend headless \
          "${steam_flags[@]}" \
          --xwayland-count 2 \
          "${prefer_vk[@]}" \
          "${hdr_flags[@]}" \
          -W "$gs_width" -H "$gs_height" -r "$gs_refresh" \
          -w "$gs_width" -h "$gs_height" \
          -- setpriv --pdeathsig TERM -- \
          bash "$0" nested-primary-child -- "${steam_launch[@]}"
      ) >"$steam_log" 2>&1 &
      nested_launch_pid=$!

      # The one explicit subshell execs setsid, so $! remains the private PGID/SID
      # leader across env/wrapProgram execs. Resolve and pin the compositor itself.
      resolve_nested_gamescope_pid() {
        local root="$1" p
        if polaris_headless_gamescope_pid "$root" 2>/dev/null; then
          printf '%s\n' "$root"
          return 0
        fi
        for p in $(pgrep -P "$root" 2>/dev/null || true); do
          if polaris_headless_gamescope_pid "$p" 2>/dev/null; then
            printf '%s\n' "$p"
            return 0
          fi
        done
        return 1
      }

      nested_marked=0
      nested_gamescope_pid=""
      for _ in $(seq 1 150); do
        gs_pid="$(resolve_nested_gamescope_pid "$nested_launch_pid" 2>/dev/null || true)"
        if [ -n "${gs_pid:-}" ] \
            && polaris_write_marker_for_pid "$marker" "$gs_pid" nested \
            && polaris_validate_marker "$marker" nested \
            && [ "$POLARIS_PROCESS_PGID" = "$nested_launch_pid" ] \
            && [ "$POLARIS_PROCESS_SESSION_ID" = "$nested_launch_pid" ]; then
          nested_marked=1
          nested_gamescope_pid="$gs_pid"
          break
        fi
        kill -0 "$nested_launch_pid" 2>/dev/null || break
        sleep 0.02
      done
      if [ "$nested_marked" != 1 ]; then
        echo "polaris-gamescope-session: failed to record an exact nested gamescope generation in its private setsid group" >&2
        if [ -f "$marker" ] && polaris_validate_marker "$marker" nested; then
          retire_marked_nested_gamescope_under_fence || true
        fi
        # Without an exact marker plus PGID/SID proof there is no safe numeric
        # fallback. Preserve the recovery claim and let service/cgroup teardown
        # contain an unclassified launch instead of risking PID/PGID reuse.
        exit 1
      fi

      ready=0
      ready_timeout="$(polaris_runtime_ready_timeout_seconds)"
      ready_deadline=$((SECONDS + ready_timeout))
      while [ "$SECONDS" -lt "$ready_deadline" ]; do
        if polaris_write_runtime_env "$marker" gamescope-0 nested "$rt"; then
          ready=1
          break
        fi
        kill -0 "$nested_gamescope_pid" 2>/dev/null || break
        sleep 0.1
      done
      if [ "$ready" != 1 ]; then
        echo "polaris-gamescope-session: owned nested gamescope-0/Xwayland not ready — see $steam_log" >&2
        retire_marked_nested_gamescope_under_fence || true
        exit 1
      fi
      publish_nested_claim nested nested || {
        echo "polaris-gamescope-session: nested ownership changed before portal rebind" >&2
        exit 1
      }
      # Portal + polaris-gamescope.env assume gamescope-0. Bail if we lost the race.
      if rg -q "wayland display 'gamescope-1'" "$steam_log" 2>/dev/null; then
        echo "polaris-gamescope-session: nested bound gamescope-1 (portal captures gamescope-0) — see $steam_log" >&2
        exit 1
      fi
      # Only a managed (Nix) host has a private portal backend to rebind; a
      # standalone package has neither the unit nor the bus.
      rebind_private_portal_after_nested_start || {
        echo "polaris-gamescope-session: nested runtime services lost their classification before portal rebind" >&2
        exit 1
      }
      # Brief settle so Steam's first controller udev events land after portal is up.
      sleep 1
      echo "polaris-gamescope-session: nested ${POLARIS_GAMESCOPE_BIN:-gamescope} ready; WSI logs → $steam_log and ~/.local/share/Steam/logs/console-linux.txt" >&2
      echo "polaris-gamescope-session: pass = 'Creating Gamescope surface' + 'hdr formats exposed: true'" >&2
    else
      # --- Attach path (known-good stream, no WSI) ---
      if [ -f "$rt/polaris-gamescope-wsi-nested" ]; then
        echo "polaris-gamescope-session: nested recovery claim changed during attach setup" >&2
        exit 1
      elif ! systemctl --user is-active --quiet polaris-gamescope-idle.service 2>/dev/null; then
        systemctl --user start polaris-gamescope-idle.service || true
      elif [ "${prev_force:-}" != "$want_hdr" ]; then
        echo "polaris-gamescope-session: attach force=$want_hdr (was ${prev_force:-unset}); restart idle" >&2
        systemctl --user restart polaris-gamescope-idle.service || true
      fi

      attach_ready=0
      ready_timeout="$(polaris_runtime_ready_timeout_seconds)"
      ready_deadline=$((SECONDS + ready_timeout))
      while [ "$SECONDS" -lt "$ready_deadline" ]; do
        if polaris_write_runtime_env "$marker" gamescope-0 idle "$rt"; then
          attach_ready=1
          break
        fi
        sleep 0.1
      done
      if [ "$attach_ready" != 1 ]; then
        echo "polaris-gamescope-session: validated idle gamescope-0/Xwayland not ready" >&2
        exit 1
      fi

      # shellcheck disable=SC1091
      . "$rt/polaris-gamescope.env"
      hdr_steam_env=()
      if [ "$want_hdr" = 1 ]; then
        hdr_steam_env=(STEAM_GAMESCOPE_HDR_SUPPORTED=1 DXVK_HDR=1)
        echo "polaris-gamescope-session: attach Steam HDR env on (no WSI)" >&2
      else
        echo "polaris-gamescope-session: attach Steam HDR env off" >&2
      fi
      steam_log="$(mktemp "$rt/polaris-gamescope-steam-attach.XXXXXX.log")"
      # Force X11 on gamescope XWayland for attach. Polaris inherits the
      # host KDE session (WAYLAND_DISPLAY=wayland-0, GDK_BACKEND=wayland);
      # native titles (e.g. bg3) then paint on KWin while the portal
      # captures empty gamescope → black stream (measured 2026-07-14).
      # Do not pass host WAYLAND_DISPLAY; do not enable FROG WSI here.
      echo "polaris-gamescope-session: attach Steam on DISPLAY=${DISPLAY:-:1} (X11 only, host Wayland stripped)" >&2
      (
        run_without_session_operation_lock setsid -f env \
          -u WAYLAND_DISPLAY \
          -u CLUTTER_BACKEND \
          -u ELECTRON_OZONE_PLATFORM_HINT \
          -u MOZ_ENABLE_WAYLAND \
          -u ENABLE_GAMESCOPE_WSI \
          -u ENABLE_HDR_WSI \
          DISPLAY="${DISPLAY:-:1}" \
          GAMESCOPE_WAYLAND_DISPLAY=gamescope-0 \
          PULSE_SINK="$audio_sink" \
          PIPEWIRE_NODE="$audio_sink" \
          POLARIS_SESSION_AUDIO_SINK="$audio_sink" \
          STEAM_MULTIPLE_XWAYLANDS=1 \
          QT_QPA_PLATFORM=xcb \
          GDK_BACKEND=x11 \
          SDL_VIDEODRIVER=x11 \
          XDG_SESSION_TYPE=x11 \
          "${hdr_steam_env[@]}" \
          setpriv --inh-caps=-all --ambient-caps=-all -- \
          "${steam_launch[@]}"
      ) >"$steam_log" 2>&1
      sleep 2
    fi
    ;;
  wait)
    # Hold the Moonlight session open until Polaris SIGTERMs this process
    # (process-group kill on disconnect / app stop). Do NOT exit early on
    # Steam probe failures — that raced portal attach and tore nested
    # gamescope down within ~50ms (Response code 2).
    load_session_instance_id || {
      echo "polaris-gamescope-session: missing or mismatched session credential during wait" >&2
      exit 1
    }
    appid=""
    if [ -f "$rt/polaris-gamescope-appid" ]; then
      appid="$(tr -d '[:space:]' <"$rt/polaris-gamescope-appid" || true)"
    fi
    echo "polaris-gamescope-session: wait holding stream (appid=${appid:-none}, credential=$POLARIS_SESSION_INSTANCE_ID)" >&2
    trap 'echo "polaris-gamescope-session: wait got signal — releasing" >&2; exit 0' TERM INT
    seen=0
    gone=0
    while :; do
      if [ -e "$nested_primary_exit_file" ] || [ -L "$nested_primary_exit_file" ]; then
        if [ -f "$nested_primary_exit_file" ] \
            && [ ! -L "$nested_primary_exit_file" ] \
            && [ "$(tr -d '\r\n' <"$nested_primary_exit_file")" = "$POLARIS_SESSION_INSTANCE_ID" ]; then
          echo "polaris-gamescope-session: exact-session Steam primary command exited — releasing wait" >&2
          exit 0
        fi
        echo "polaris-gamescope-session: invalid nested primary-child terminal marker" >&2
        exit 1
      fi
      if [ -n "$appid" ] && steam_app_game_alive "$appid"; then
        if [ "$seen" = 0 ]; then
          echo "polaris-gamescope-session: game process for appid=$appid seen" >&2
        fi
        seen=1
        gone=0
      elif [ "$seen" = 1 ]; then
        gone=$((gone + 1))
        # ~15s debounce: launchers/anti-cheat may respawn; Steam may be slow.
        if [ "$gone" -ge 30 ]; then
          echo "polaris-gamescope-session: game appid=$appid exited — releasing wait (end stream)" >&2
          kill_session_steam 2>/dev/null || true
          exit 0
        fi
      fi
      # Nested compositor gone after marker published → release.
      if [ -f "$marker" ]; then
        if polaris_validate_marker "$marker" nested; then
          :
        else
          if [ ! -e "$rt/gamescope-0" ]; then
            echo "polaris-gamescope-session: nested gamescope gone — releasing wait" >&2
            exit 0
          fi
        fi
      fi
      sleep 0.5
    done
    ;;
  stop)
    acquire_session_operation_lock || {
      echo "polaris-gamescope-session: could not serialize session stop" >&2
      exit 1
    }
    if [ ! -e "$session_state_file" ] \
        && [ ! -s "$session_id_file" ] \
        && [ ! -f "$session_mode_file" ] \
        && [ ! -f "$rt/polaris-gamescope-wsi-nested" ]; then
      if polaris_validate_marker "$marker" nested; then
        echo "polaris-gamescope-session: nested owner remains without a durable recovery claim" >&2
        exit 1
      fi
      # A partial standalone startup may have created a user.control mask
      # before publishing durable state. Make the idempotent stop path repair
      # that residue so the next start still classifies absent units correctly.
      polaris_unmask_idle_unit_runtime
      echo "polaris-gamescope-session: stop already complete" >&2
      exit 0
    fi
    load_session_instance_id || {
      echo "polaris-gamescope-session: missing or mismatched session credential during stop" >&2
      exit 1
    }
    if [ -n "${POLARIS_PERSISTED_SESSION_MODE:-}" ]; then
      session_mode="$POLARIS_PERSISTED_SESSION_MODE"
    else
      [ -f "$session_mode_file" ] || {
        echo "polaris-gamescope-session: durable session mode missing; retaining credential" >&2
        exit 1
      }
      session_mode="$(tr -d '[:space:]' <"$session_mode_file")"
    fi
    case "$session_mode" in attach|nested) ;; *)
      echo "polaris-gamescope-session: invalid durable session mode; retaining credential" >&2
      exit 1
      ;;
    esac
    rm -f "$rt/polaris-gamescope-appid" "$rt/polaris-gamescope-audio-sink" "$rt/polaris-gamescope-audio-skip-pin"
    # Keep null sinks loaded (permanent capture targets).
    rm -f "$rt/polaris-gamescope-sink-module"
    # Legacy flags from builds that killed EasyEffects / hijacked default.
    rm -f "$rt/polaris-gamescope-prev-default-sink" "$rt/polaris-gamescope-easyeffects-units"
    nested_claim="$rt/polaris-gamescope-wsi-nested"
    if [ "$session_mode" = nested ] && [ ! -e "$nested_claim" ]; then
      recover_missing_nested_claim || {
        echo "polaris-gamescope-session: claimless nested credential is not provably pre-transition; retaining it" >&2
        exit 1
      }
    fi
    if [ -f "$nested_claim" ]; then
      [ "$session_mode" = nested ] || {
        echo "polaris-gamescope-session: nested claim conflicts with durable attach mode" >&2
        exit 1
      }
      claim_state="$(tr -d '[:space:]' <"$nested_claim")"
      case "$claim_state" in
        transition)
          if polaris_validate_marker "$marker" nested; then
            echo "polaris-gamescope-session: transition claim unexpectedly owns a nested generation" >&2
            exit 1
          fi
          if ! polaris_validate_marker "$marker" idle \
              && ! polaris_reclaim_orphan_gamescope_sockets "$rt"; then
            echo "polaris-gamescope-session: transition recovery cannot prove idle-or-orphan ownership" >&2
            exit 1
          fi
          publish_nested_claim restore-idle "$claim_state" || {
            echo "polaris-gamescope-session: transition claim changed before idle restoration" >&2
            exit 1
          }
          ;;
        1|nested)
          echo "polaris-gamescope-session: tearing down marked nested WSI gamescope" >&2
          if polaris_validate_marker "$marker" nested; then
            # Pause the exact compositor first, retire credential-bound Steam
            # while the primary wrapper remains runnable, then commit the full
            # private-group fence. This prevents both X11 I/O abort and Vulkan
            # destructor paths without touching desktop Steam.
            if ! retire_marked_nested_gamescope_under_fence; then
              echo "polaris-gamescope-session: nested owner did not reach terminal state; retaining recovery claim" >&2
              exit 1
            fi
          else
            if ! kill_session_steam || ! session_steam_absent; then
              echo "polaris-gamescope-session: exact-session Steam did not reach terminal state; retaining recovery claim" >&2
              exit 1
            fi
            # Nested generation already dead (host hang dump, gamescope crash, or
            # kill raced marker validation). Only advance when sockets are absent
            # or reclaimable — live foreign ownership must keep the durable claim
            # so we never steal another compositor's gamescope-0.
            if ! polaris_reclaim_orphan_gamescope_sockets "$rt"; then
              echo "polaris-gamescope-session: nested launch lacks a valid exact marker; retaining recovery claim" >&2
              exit 1
            fi
            echo "polaris-gamescope-session: nested marker invalid/dead; sockets orphan or absent — restoring idle" >&2
          fi
          if ! retire_session_xwayland || ! session_xwayland_absent; then
            echo "polaris-gamescope-session: exact-session Xwayland did not reach terminal state; retaining recovery claim" >&2
            exit 1
          fi
          publish_nested_claim restore-idle "$claim_state" || {
            echo "polaris-gamescope-session: transition claim changed before idle restoration" >&2
            exit 1
          }
          ;;
        restore-idle)
          ;;
        *)
          echo "polaris-gamescope-session: invalid nested recovery state '$claim_state'" >&2
          exit 1
          ;;
      esac

      # Ordered handoff: idle must own gamescope-0 and publish an exact runtime
      # environment before the portal may rebind. The restore-idle claim stays
      # durable across every failure so a retry never repeats nested signaling.
      printf '0\n' >"$rt/polaris-gamescope-force"
      polaris_unmask_idle_unit_runtime
      current_service_mode="$(polaris_gamescope_service_mode)" || {
        echo "polaris-gamescope-session: cannot classify post-session gamescope services; retaining restore-idle claim" >&2
        exit 1
      }
      service_mode="${POLARIS_PERSISTED_SERVICE_MODE:-$current_service_mode}"
      if [ "$service_mode" != "$current_service_mode" ]; then
        echo "polaris-gamescope-session: gamescope service model changed from $service_mode to $current_service_mode; retaining restore-idle claim" >&2
        exit 1
      fi
      if [ "$service_mode" = standalone ]; then
        complete_standalone_nested_handoff "$nested_claim" || {
          echo "polaris-gamescope-session: standalone cleanup could not prove an empty gamescope generation; retaining restore-idle claim" >&2
          exit 1
        }
        echo "polaris-gamescope-session: standalone package runtime restored with no idle gamescope" >&2
      else
        systemctl --user reset-failed polaris-gamescope-idle.service 2>/dev/null || true
        if ! systemctl --user start polaris-gamescope-idle.service 2>/dev/null; then
          echo "polaris-gamescope-session: failed to start idle gamescope; retaining restore-idle claim" >&2
          exit 1
        fi
        idle_ready=0
        for _ in $(seq 1 "${POLARIS_IDLE_WAIT_STEPS:-100}"); do
          if polaris_validate_marker "$marker" idle \
              && polaris_marker_owns_socket "$marker" "$rt/gamescope-0" idle \
              && polaris_write_runtime_env "$marker" gamescope-0 idle "$rt" 2>/dev/null; then
            idle_ready=1
            break
          fi
          sleep 0.1
        done
        if [ "$idle_ready" != 1 ]; then
          echo "polaris-gamescope-session: idle gamescope-0 not ready; retaining restore-idle claim" >&2
          exit 1
        fi
        exec 8>>"$rt/polaris-gamescope.lock" || exit 1
        "${POLARIS_FLOCK_BIN:-flock}" -x 8 || exit 1
        export POLARIS_GAMESCOPE_LOCK_HELD=1
        if ! {
          [ -f "$nested_claim" ] \
            && [ "$(tr -d '[:space:]' <"$nested_claim")" = restore-idle ] \
            && polaris_validate_marker "$marker" idle \
            && polaris_marker_owns_socket "$marker" "$rt/gamescope-0" idle \
            && polaris_write_runtime_env "$marker" gamescope-0 idle "$rt"
        }; then
          echo "polaris-gamescope-session: idle ownership changed before portal handoff" >&2
          exit 1
        fi
        echo "polaris-gamescope-session: idle gamescope restored after nested stop" >&2

        if ! systemctl --user restart polaris-portal-gamescope.service 2>/dev/null; then
          echo "polaris-gamescope-session: portal restart failed; retaining restore-idle claim" >&2
          exit 1
        fi
        portal_bus="unix:path=$rt/polaris-portal/bus"
        portal_ready=0
        for _ in $(seq 1 "${POLARIS_PORTAL_WAIT_STEPS:-50}"); do
          if busctl --address="$portal_bus" --no-pager \
              status org.freedesktop.impl.portal.desktop.gamescope >/dev/null 2>&1; then
            portal_ready=1
            break
          fi
          sleep 0.1
        done
        if [ "$portal_ready" != 1 ]; then
          echo "polaris-gamescope-session: portal did not bind idle generation; retaining restore-idle claim" >&2
          exit 1
        fi
        if ! {
          polaris_validate_marker "$marker" idle \
            && polaris_marker_owns_socket "$marker" "$rt/gamescope-0" idle \
            && [ -f "$nested_claim" ] \
            && [ "$(tr -d '[:space:]' <"$nested_claim")" = restore-idle ]
        }; then
          echo "polaris-gamescope-session: ownership changed during portal readiness" >&2
          exit 1
        fi
        rm -f -- "$nested_claim"
        "${POLARIS_FLOCK_BIN:-flock}" -u 8
        export POLARIS_GAMESCOPE_LOCK_HELD=0
      fi
    else
      [ "$session_mode" = attach ] || {
        echo "polaris-gamescope-session: nested mode lost its recovery claim; retaining credential" >&2
        exit 1
      }
      kill_session_steam || {
        echo "polaris-gamescope-session: attach recovery could not terminate exact-session Steam" >&2
        exit 1
      }
      session_steam_absent || {
        echo "polaris-gamescope-session: attach generation still alive; retaining credential" >&2
        exit 1
      }
      if [ -f "$rt/polaris-gamescope-force" ] \
          && [ "$(tr -d '[:space:]' <"$rt/polaris-gamescope-force")" = "1" ]; then
        printf '0\n' >"$rt/polaris-gamescope-force"
        systemctl --user restart polaris-gamescope-idle.service || true
      fi
    fi
    rm -f "$session_state_file" "$session_mode_file" "$session_id_file" "$nested_primary_exit_file"
    ;;
  *)
    echo "usage: polaris-gamescope-session start [steam_appid]|wait|stop" >&2
    exit 2
    ;;
esac
