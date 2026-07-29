#!/usr/bin/env bash
# Shared gamescope exact-generation ownership helpers.
# Callers must set POLARIS_PROC_ROOT/POLARIS_PROC_NET_UNIX/POLARIS_X11_SOCKET_DIR
# only in tests; production defaults are Linux procfs and the X11 socket directory.

polaris_proc_root() { printf '%s\n' "${POLARIS_PROC_ROOT:-/proc}"; }
polaris_proc_net_unix() { printf '%s\n' "${POLARIS_PROC_NET_UNIX:-/proc/net/unix}"; }
polaris_x11_socket_dir() { printf '%s\n' "${POLARIS_X11_SOCKET_DIR:-/tmp/.X11-unix}"; }

polaris_process_fields() {
  local pid="$1" stat rest
  [ -r "$(polaris_proc_root)/$pid/stat" ] || return 1
  IFS= read -r stat <"$(polaris_proc_root)/$pid/stat" || return 1
  case "$stat" in
    *') '*) rest="${stat##*) }" ;;
    *) return 1 ;;
  esac
  # shellcheck disable=SC2206
  local fields=( $rest )
  [ "${#fields[@]}" -ge 20 ] || return 1
  POLARIS_PROCESS_PPID="${fields[1]}"
  POLARIS_PROCESS_START_TIME="${fields[19]}"
  case "$POLARIS_PROCESS_PPID:$POLARIS_PROCESS_START_TIME" in
    *[!0-9:]*|:*|*:) return 1 ;;
  esac
  [ "$POLARIS_PROCESS_START_TIME" != 0 ]
}

polaris_read_marker() {
  local marker="$1" extra
  read -r POLARIS_MARKER_PID POLARIS_MARKER_START_TIME POLARIS_MARKER_ROLE extra <"$marker" 2>/dev/null || return 1
  [ -z "${extra:-}" ] || return 1
  case "$POLARIS_MARKER_PID:$POLARIS_MARKER_START_TIME" in
    *[!0-9:]*|0:*|:*|*:) return 1 ;;
  esac
  case "$POLARIS_MARKER_ROLE" in
    ''|*[!a-z-]*) return 1 ;;
  esac
}

polaris_headless_gamescope_pid() {
  local pid="$1" arg first=1 executable= backend=0 previous=
  local exe_path exe_name
  exe_path="$(readlink "$(polaris_proc_root)/$pid/exe" 2>/dev/null)" || return 1
  exe_name="${exe_path##*/}"
  [ "$exe_name" = gamescope ] || return 1
  while IFS= read -r arg; do
    if [ "$first" = 1 ]; then
      executable="${arg##*/}"
      first=0
    elif [ "$arg" = "--backend=headless" ] || { [ "$previous" = "--backend" ] && [ "$arg" = headless ]; }; then
      backend=1
    fi
    previous="$arg"
  done < <(tr '\0' '\n' <"$(polaris_proc_root)/$pid/cmdline" 2>/dev/null) || return 1
  [ "$executable" = gamescope ] && [ "$backend" = 1 ]
}

polaris_validate_process_generation() {
  local pid="$1" start_time="$2"
  polaris_process_fields "$pid" || return 1
  [ "$POLARIS_PROCESS_START_TIME" = "$start_time" ] || return 1
  polaris_headless_gamescope_pid "$pid"
}

polaris_validate_marker() {
  local marker="$1" expected_role="${2:-}"
  polaris_read_marker "$marker" || return 1
  [ -z "$expected_role" ] || [ "$POLARIS_MARKER_ROLE" = "$expected_role" ] || return 1
  polaris_validate_process_generation "$POLARIS_MARKER_PID" "$POLARIS_MARKER_START_TIME"
}

polaris_process_has_argument() {
  local marker="$1" expected_role="$2" wanted="$3" arg
  polaris_validate_marker "$marker" "$expected_role" || return 1
  while IFS= read -r arg; do
    [ "$arg" = "$wanted" ] && return 0
  done < <(tr '\0' '\n' <"$(polaris_proc_root)/$POLARIS_MARKER_PID/cmdline" 2>/dev/null)
  return 1
}

polaris_write_marker_for_pid() {
  local marker="$1" pid="$2" role="$3" attempt tmp
  for attempt in $(seq 1 100); do
    if polaris_process_fields "$pid" && polaris_headless_gamescope_pid "$pid"; then
      tmp="$marker.tmp.$$"
      (umask 077; printf '%s %s %s\n' "$pid" "$POLARIS_PROCESS_START_TIME" "$role" >"$tmp") || return 1
      mv -f "$tmp" "$marker"
      return 0
    fi
    sleep 0.02
  done
  return 1
}

polaris_pid_is_descendant() {
  local candidate="$1" root="$2" depth=0
  while [ "$candidate" -gt 0 ] 2>/dev/null && [ "$depth" -lt 256 ]; do
    [ "$candidate" = "$root" ] && return 0
    polaris_process_fields "$candidate" || return 1
    candidate="$POLARIS_PROCESS_PPID"
    depth=$((depth + 1))
  done
  return 1
}

polaris_socket_inode() {
  local wanted="$1" num ref protocol flags type state inode path rest
  while read -r num ref protocol flags type state inode path rest; do
    [ "$path" = "$wanted" ] || continue
    case "$inode" in ''|*[!0-9]*) return 1 ;; esac
    printf '%s\n' "$inode"
    return 0
  done <"$(polaris_proc_net_unix)" 2>/dev/null
  return 1
}

polaris_pid_holds_inode() {
  local pid="$1" inode="$2" fd target
  for fd in "$(polaris_proc_root)/$pid/fd"/*; do
    [ -L "$fd" ] || continue
    target="$(readlink "$fd" 2>/dev/null || true)"
    [ "$target" = "socket:[$inode]" ] && return 0
  done
  return 1
}

polaris_process_tree_holds_inode() {
  local root="$1" inode="$2" process pid
  for process in "$(polaris_proc_root)"/[0-9]*; do
    [ -d "$process" ] || continue
    pid="${process##*/}"
    if polaris_pid_is_descendant "$pid" "$root" && polaris_pid_holds_inode "$pid" "$inode"; then
      return 0
    fi
  done
  return 1
}

polaris_marker_owns_socket() {
  local marker="$1" socket="$2" expected_role="${3:-}" inode
  polaris_validate_marker "$marker" "$expected_role" || return 1
  inode="$(polaris_socket_inode "$socket")" || return 1
  polaris_process_tree_holds_inode "$POLARIS_MARKER_PID" "$inode"
}

polaris_xwayland_pid() {
  local pid="$1" executable exe_path
  exe_path="$(readlink "$(polaris_proc_root)/$pid/exe" 2>/dev/null)" || return 1
  [ "${exe_path##*/}" = Xwayland ] || return 1
  IFS= read -r executable < <(tr '\0' '\n' <"$(polaris_proc_root)/$pid/cmdline" 2>/dev/null) || return 1
  [ "${executable##*/}" = Xwayland ]
}

polaris_discover_xwayland_display() {
  local marker="$1" expected_role="${2:-}" xdir socket name display inode process pid best=
  polaris_validate_marker "$marker" "$expected_role" || return 1
  local root_pid="$POLARIS_MARKER_PID"
  xdir="$(polaris_x11_socket_dir)"
  for socket in "$xdir"/X*; do
    [ -e "$socket" ] || continue
    name="${socket##*/}"
    display="${name#X}"
    case "$display" in ''|*[!0-9]*) continue ;; esac
    inode="$(polaris_socket_inode "$socket")" || continue
    for process in "$(polaris_proc_root)"/[0-9]*; do
      [ -d "$process" ] || continue
      pid="${process##*/}"
      [ "$pid" != "$root_pid" ] || continue
      if polaris_pid_is_descendant "$pid" "$root_pid" && polaris_xwayland_pid "$pid" \
          && polaris_pid_holds_inode "$pid" "$inode"; then
        if [ -z "$best" ] || [ "$display" -lt "$best" ]; then
          best="$display"
        fi
      fi
    done
  done
  [ -n "$best" ] || return 1
  printf ':%s\n' "$best"
}

polaris_write_runtime_env() {
  local marker="$1" wayland="$2" expected_role="${3:-}" runtime_dir="$4" display tmp
  polaris_validate_marker "$marker" "$expected_role" || return 1
  local pid="$POLARIS_MARKER_PID" start_time="$POLARIS_MARKER_START_TIME"
  polaris_marker_owns_socket "$marker" "$runtime_dir/$wayland" "$expected_role" || return 1
  display="$(polaris_discover_xwayland_display "$marker" "$expected_role")" || return 1
  tmp="$runtime_dir/polaris-gamescope.env.tmp.$$"
  (umask 077; printf 'DISPLAY=%s\nWAYLAND_DISPLAY=%s\nGAMESCOPE_WAYLAND_DISPLAY=%s\nPOLARIS_GAMESCOPE_PID=%s\nPOLARIS_GAMESCOPE_START_TIME=%s\nPOLARIS_GAMESCOPE_ROLE=%s\n' \
    "$display" "$wayland" "$wayland" "$pid" "$start_time" "$POLARIS_MARKER_ROLE" >"$tmp") || return 1
  mv -f "$tmp" "$runtime_dir/polaris-gamescope.env"
}

polaris_stop_marked_gamescope() {
  local marker="$1" expected_role="$2" runtime_dir="$3" kill_bin="${POLARIS_KILL_BIN:-kill}"
  local marker_line pid start_time socket inode entry current_inode attempt marker_replaced=0
  local owned_sockets=() term_steps="${POLARIS_STOP_WAIT_STEPS:-30}" kill_steps="${POLARIS_KILL_WAIT_STEPS:-20}"
  polaris_validate_marker "$marker" "$expected_role" || return 1
  marker_line="$(<"$marker")"
  pid="$POLARIS_MARKER_PID"
  start_time="$POLARIS_MARKER_START_TIME"

  for socket in "$runtime_dir"/gamescope-[0-9]* "$runtime_dir"/gamescope-[0-9]*-ei; do
    [ -e "$socket" ] || [ -S "$socket" ] || continue
    if polaris_marker_owns_socket "$marker" "$socket" "$expected_role"; then
      inode="$(polaris_socket_inode "$socket")" || continue
      owned_sockets+=("$socket|$inode")
    fi
  done

  polaris_validate_process_generation "$pid" "$start_time" || return 1
  "$kill_bin" -TERM "-$pid" 2>/dev/null || "$kill_bin" -TERM "$pid" 2>/dev/null || return 1
  for attempt in $(seq 1 "$term_steps"); do
    if ! polaris_validate_process_generation "$pid" "$start_time"; then
      break
    fi
    sleep 0.1
  done
  if polaris_validate_process_generation "$pid" "$start_time"; then
    "$kill_bin" -KILL "-$pid" 2>/dev/null || "$kill_bin" -KILL "$pid" 2>/dev/null || return 1
    for attempt in $(seq 1 "$kill_steps"); do
      polaris_validate_process_generation "$pid" "$start_time" || break
      sleep 0.1
    done
  fi
  polaris_validate_process_generation "$pid" "$start_time" && return 1

  if [ -f "$runtime_dir/polaris-gamescope.env" ] \
      && grep -qx "POLARIS_GAMESCOPE_PID=$pid" "$runtime_dir/polaris-gamescope.env" \
      && grep -qx "POLARIS_GAMESCOPE_START_TIME=$start_time" "$runtime_dir/polaris-gamescope.env"; then
    rm -f "$runtime_dir/polaris-gamescope.env"
  fi
  if [ -f "$marker" ] && [ "$(<"$marker")" != "$marker_line" ]; then
    marker_replaced=1
  fi
  if [ "$marker_replaced" = 0 ]; then
    for entry in "${owned_sockets[@]}"; do
      socket="${entry%|*}"
      inode="${entry##*|}"
      current_inode="$(polaris_socket_inode "$socket" 2>/dev/null || true)"
      if [ -n "$current_inode" ] && [ "$current_inode" = "$inode" ]; then
        rm -f "$socket" "$socket.lock"
      fi
    done
  fi
  if [ "$marker_replaced" = 0 ] && [ -f "$marker" ] && [ "$(<"$marker")" = "$marker_line" ]; then
    rm -f "$marker"
  fi
}
