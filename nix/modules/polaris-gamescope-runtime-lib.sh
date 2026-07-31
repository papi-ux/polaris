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
  POLARIS_PROCESS_PGID="${fields[2]}"
  POLARIS_PROCESS_SESSION_ID="${fields[3]}"
  POLARIS_PROCESS_START_TIME="${fields[19]}"
  case "$POLARIS_PROCESS_PPID:$POLARIS_PROCESS_PGID:$POLARIS_PROCESS_SESSION_ID:$POLARIS_PROCESS_START_TIME" in
    *[!0-9:]*|:*|*:) return 1 ;;
  esac
  [ "$POLARIS_PROCESS_PGID" -gt 1 ] 2>/dev/null \
    && [ "$POLARIS_PROCESS_SESSION_ID" -gt 1 ] 2>/dev/null \
    && [ "$POLARIS_PROCESS_START_TIME" != 0 ]
}

polaris_read_marker() {
  local marker="$1" extra executable_name
  # Check readability first — bash still prints "No such file" for <"$missing"
  # even when the whole command redirects stderr.
  [ -r "$marker" ] || return 1
  read -r POLARIS_MARKER_PID POLARIS_MARKER_START_TIME POLARIS_MARKER_ROLE POLARIS_MARKER_EXECUTABLE extra <"$marker" || return 1
  [ -z "${extra:-}" ] || return 1
  case "$POLARIS_MARKER_PID:$POLARIS_MARKER_START_TIME" in
    *[!0-9:]*|0:*|:*|*:) return 1 ;;
  esac
  case "$POLARIS_MARKER_ROLE" in
    ''|*[!a-z-]*) return 1 ;;
  esac
  case "$POLARIS_MARKER_EXECUTABLE" in
    /*) ;;
    *) return 1 ;;
  esac
  executable_name="${POLARIS_MARKER_EXECUTABLE##*/}"
  case "$executable_name" in
    gamescope|.gamescope-wrapped) ;;
    *) return 1 ;;
  esac
}

polaris_headless_gamescope_pid() {
  local pid="$1" arg first=1 executable="" backend=0 previous=""
  local exe_path exe_name
  exe_path="$(readlink "$(polaris_proc_root)/$pid/exe" 2>/dev/null)" || return 1
  exe_name="${exe_path##*/}"
  case "$exe_name" in
    gamescope|.gamescope-wrapped) ;;
    *) return 1 ;;
  esac
  while IFS= read -r arg; do
    if [ "$first" = 1 ]; then
      executable="${arg##*/}"
      first=0
    elif [ "$arg" = "--backend=headless" ] || { [ "$previous" = "--backend" ] && [ "$arg" = headless ]; }; then
      backend=1
    fi
    previous="$arg"
  done < <(tr '\0' '\n' <"$(polaris_proc_root)/$pid/cmdline" 2>/dev/null) || return 1
  [ "$executable" = gamescope ] && [ "$backend" = 1 ] || return 1
  POLARIS_GAMESCOPE_EXECUTABLE="$exe_path"
}

polaris_validate_process_generation() {
  local pid="$1" start_time="$2" expected_executable="$3"
  polaris_process_fields "$pid" || return 1
  [ "$POLARIS_PROCESS_START_TIME" = "$start_time" ] || return 1
  polaris_headless_gamescope_pid "$pid" || return 1
  [ "$POLARIS_GAMESCOPE_EXECUTABLE" = "$expected_executable" ]
}

polaris_validate_marker() {
  local marker="$1" expected_role="${2:-}"
  polaris_read_marker "$marker" || return 1
  [ -z "$expected_role" ] || [ "$POLARIS_MARKER_ROLE" = "$expected_role" ] || return 1
  polaris_validate_process_generation "$POLARIS_MARKER_PID" "$POLARIS_MARKER_START_TIME" "$POLARIS_MARKER_EXECUTABLE"
}

polaris_process_has_argument() {
  local marker="$1" expected_role="$2" wanted="$3" arg
  polaris_validate_marker "$marker" "$expected_role" || return 1
  while IFS= read -r arg; do
    [ "$arg" = "$wanted" ] && return 0
  done < <(tr '\0' '\n' <"$(polaris_proc_root)/$POLARIS_MARKER_PID/cmdline" 2>/dev/null)
  return 1
}

polaris_write_marker_for_pid() (
  local marker="$1" pid="$2" role="$3" marker_tmp='' lock_bin="${POLARIS_FLOCK_BIN:-flock}"
  umask 077
  trap 'rm -f "${marker_tmp:-}"' EXIT
  if [ "${POLARIS_GAMESCOPE_LOCK_HELD:-0}" != 1 ]; then
    exec 9>>"${marker%/*}/polaris-gamescope.lock" || return 1
    "$lock_bin" -x 9 || return 1
  fi
  for _ in $(seq 1 100); do
    if polaris_process_fields "$pid" && polaris_headless_gamescope_pid "$pid"; then
      local start_time="$POLARIS_PROCESS_START_TIME" executable_path="$POLARIS_GAMESCOPE_EXECUTABLE"
      if polaris_validate_marker "$marker"; then
        [ "$POLARIS_MARKER_PID" = "$pid" ] \
          && [ "$POLARIS_MARKER_START_TIME" = "$start_time" ] \
          && [ "$POLARIS_MARKER_ROLE" = "$role" ] \
          && [ "$POLARIS_MARKER_EXECUTABLE" = "$executable_path" ]
        return
      fi
      # umask already 077 for this subshell function; avoid nested (..) so
      # SC2030/SC2031 do not treat marker_tmp as subshell-local vs other helpers.
      marker_tmp="$marker.tmp.$$"
      printf '%s %s %s %s\n' "$pid" "$start_time" "$role" "$executable_path" >"$marker_tmp" || return 1
      # Explicit status: set -e is ignored when this function is used in if/||.
      mv -f "$marker_tmp" "$marker" || return 1
      marker_tmp=''
      return 0
    fi
    sleep 0.02
  done
  return 1
)

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
  local wanted="$1" inode path found=""
  # /proc/net/unix columns: num ref protocol flags type state inode path …
  while read -r _ _ _ _ _ _ inode path _; do
    [ "$path" = "$wanted" ] || continue
    case "$inode" in ''|*[!0-9]*) return 1 ;; esac
    # Duplicate pathname rows are ambiguous: an unlinked old listener may
    # coexist with a successor that rebound the same filesystem path.
    [ -z "$found" ] || return 1
    found="$inode"
  done <"$(polaris_proc_net_unix)" 2>/dev/null
  [ -n "$found" ] || return 1
  printf '%s\n' "$found"
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

# True (0) when $1 is missing or has no live holder — safe to unlink.
# False (1) when a live process holds the socket, ownership metadata is
# unavailable, or the pathname is ambiguous.
polaris_socket_is_orphan() {
  local socket="$1" inode path count=0 proc_net_unix
  [ -e "$socket" ] || return 0
  proc_net_unix="$(polaris_proc_net_unix)"
  # Unknown ownership is not evidence of an orphan. Destructive reclaim must
  # fail closed when the kernel socket table cannot be read.
  [ -r "$proc_net_unix" ] || return 1
  while read -r _ _ _ _ _ _ inode path _; do
    [ "$path" = "$socket" ] || continue
    case "$inode" in ''|*[!0-9]*) continue ;; esac
    count=$((count + 1))
  done <"$proc_net_unix"
  # Any kernel row means the socket is still referenced. Its fd holder may be
  # hidden by procfs permissions, so only a path with no row is reclaimable.
  [ "$count" -eq 0 ]
}

polaris_wayland_lock_is_stable() {
  local lock="$1" fd_identity path_identity
  [ ! -L "$lock" ] || return 1
  fd_identity="$(stat -Lc '%d:%i:%u' "/proc/$BASHPID/fd/8" 2>/dev/null)" || return 1
  path_identity="$(stat -Lc '%d:%i:%u' "$lock" 2>/dev/null)" || return 1
  [ "$fd_identity" = "$path_identity" ]
}

# Fail closed if an older producer still holds an unlinked lock inode with the
# same pathname. An unlocked replacement lock cannot serialize with it.
polaris_wayland_lock_has_no_deleted_holder() {
  local lock="$1" owner proc_root process process_owner fd target seen=0
  owner="$(stat -Lc '%u' "$lock" 2>/dev/null)" || return 1
  proc_root="$(polaris_proc_root)"
  [ -d "$proc_root" ] && [ -r "$proc_root" ] && [ -x "$proc_root" ] || return 1
  for process in "$proc_root"/[0-9]*; do
    [ -d "$process" ] || continue
    seen=1
    process_owner="$(stat -Lc '%u' "$process" 2>/dev/null)" || return 1
    [ "$process_owner" = "$owner" ] || continue
    [ -d "$process/fd" ] || return 1
    for fd in "$process"/fd/*; do
      [ -e "$fd" ] || [ -L "$fd" ] || continue
      target="$(readlink "$fd" 2>/dev/null)" || return 1
      [ "$target" != "$lock (deleted)" ] || return 1
    done
  done
  [ "$seen" = 1 ]
}

# Remove one socket path if orphaned. 0 = missing/removed, 1 = live holder.
polaris_remove_orphan_socket() (
  local socket="$1" lock_bin="${POLARIS_FLOCK_BIN:-flock}" \
    socket_identity current_identity pin_identity socket_pin
  if [ ! -e "$socket" ]; then
    return 0
  fi
  # Libwayland holds this lock across bind and display lifetime. Acquire it
  # non-blocking so reclaim cannot race a compositor between lock and bind.
  [ -f "$socket.lock" ] && [ ! -L "$socket.lock" ] || return 1
  polaris_wayland_lock_has_no_deleted_holder "$socket.lock" || return 1
  exec 8<"$socket.lock" || return 1
  polaris_wayland_lock_is_stable "$socket.lock" || return 1
  "$lock_bin" -n -x 8 || return 1
  polaris_wayland_lock_is_stable "$socket.lock" || return 1
  polaris_wayland_lock_has_no_deleted_holder "$socket.lock" || return 1
  [ -e "$socket" ] || return 0
  [ ! -L "$socket" ] || return 1
  # Pin the original inode with a same-filesystem hard link. If the pathname is
  # replaced during orphan validation, the original inode cannot be recycled
  # into the replacement while this pin exists.
  socket_pin="${socket}.polaris-pin.$$.$RANDOM"
  [ ! -e "$socket_pin" ] && [ ! -L "$socket_pin" ] || return 1
  ln -- "$socket" "$socket_pin" 2>/dev/null || return 1
  trap 'rm -f -- "$socket_pin"' EXIT
  socket_identity="$(stat -Lc '%d:%i:%f' "$socket_pin" 2>/dev/null)" || return 1
  polaris_socket_is_orphan "$socket" || return 1
  polaris_wayland_lock_is_stable "$socket.lock" || return 1
  polaris_wayland_lock_has_no_deleted_holder "$socket.lock" || return 1
  current_identity="$(stat -Lc '%d:%i:%f' "$socket" 2>/dev/null)" || return 1
  pin_identity="$(stat -Lc '%d:%i:%f' "$socket_pin" 2>/dev/null)" || return 1
  [ "$pin_identity" = "$socket_identity" ] \
    && [ "$current_identity" = "$socket_identity" ] || return 1
  echo "polaris: reclaiming orphan socket $socket" >&2
  rm -f "$socket" 2>/dev/null || return 1
  [ ! -e "$socket" ] && [ ! -S "$socket" ] || return 1
  # Never unlink the lock path: a process may hold its inode while another
  # binder creates and acquires a replacement inode.
  return 0
)

# Reclaim dead gamescope-* residue after crash. Fails closed on live holders.
polaris_reclaim_orphan_gamescope_sockets() {
  local runtime_dir="$1" name socket
  for name in gamescope-0 gamescope-1 gamescope-0-ei gamescope-1-ei; do
    socket="$runtime_dir/$name"
    if [ -e "$socket" ] || [ -S "$socket" ]; then
      if ! polaris_remove_orphan_socket "$socket"; then
        echo "polaris: refusing destructive cleanup of live unowned $socket" >&2
        return 1
      fi
    fi
  done
  return 0
}

polaris_xwayland_pid() {
  local pid="$1" executable exe_path
  exe_path="$(readlink "$(polaris_proc_root)/$pid/exe" 2>/dev/null)" || return 1
  [ "${exe_path##*/}" = Xwayland ] || return 1
  IFS= read -r executable < <(tr '\0' '\n' <"$(polaris_proc_root)/$pid/cmdline" 2>/dev/null) || return 1
  [ "${executable##*/}" = Xwayland ]
}

# Xwayland ownership is exact-generation ancestry only. Service cgroups are
# scheduling containers shared by old and new compositor generations and are
# never authorization for DISPLAY routing.
polaris_pid_related_to_root() {
  local pid="$1" root="$2"
  [ "$pid" != "$root" ] && polaris_pid_is_descendant "$pid" "$root"
}

polaris_unique_unix_socket_inode() {
  local wanted="$1" inode path count=0 selected=
  [ -r "$(polaris_proc_net_unix)" ] || return 1
  while read -r _ _ _ _ _ _ inode path _; do
    [ "$path" = "$wanted" ] || continue
    case "$inode" in ''|*[!0-9]*) return 1 ;; esac
    count=$((count + 1))
    selected="$inode"
  done <"$(polaris_proc_net_unix)" || return 1
  [ "$count" -eq 1 ] || return 1
  printf '%s\n' "$selected"
}

polaris_discover_xwayland_display() {
  local marker="$1" expected_role="${2:-}" xdir socket name display inode process pid final_inode
  local best='' best_pid='' best_start='' best_inode='' best_socket='' marker_line
  polaris_validate_marker "$marker" "$expected_role" || return 1
  local root_pid="$POLARIS_MARKER_PID" root_start="$POLARIS_MARKER_START_TIME" root_executable="$POLARIS_MARKER_EXECUTABLE"
  marker_line="$(<"$marker")"
  xdir="$(polaris_x11_socket_dir)"
  for socket in "$xdir"/X*; do
    [ -e "$socket" ] || continue
    name="${socket##*/}"
    display="${name#X}"
    case "$display" in ''|*[!0-9]*) continue ;; esac
    # Duplicate pathname generations are ambiguous after unlink/rebind. The
    # process may hold an old inode while clients route to an unrelated new one.
    inode="$(polaris_unique_unix_socket_inode "$socket")" || continue
    for process in "$(polaris_proc_root)"/[0-9]*; do
      [ -d "$process" ] || continue
      pid="${process##*/}"
      case "$pid" in ''|*[!0-9]*) continue ;; esac
      [ "$pid" != "$root_pid" ] || continue
      if polaris_xwayland_pid "$pid" && polaris_pid_related_to_root "$pid" "$root_pid" \
          && polaris_pid_holds_inode "$pid" "$inode" \
          && polaris_process_fields "$pid"; then
        if [ -z "$best" ] || [ "$display" -lt "$best" ]; then
          best="$display"
          best_pid="$pid"
          best_start="$POLARIS_PROCESS_START_TIME"
          best_inode="$inode"
          best_socket="$socket"
        fi
      fi
    done
  done
  [ -n "$best" ] || return 1
  # Revalidate both process generations and the exact socket ownership after
  # the scan. Numeric PIDs and procfs metadata are not stable authorizations.
  polaris_validate_process_generation "$root_pid" "$root_start" "$root_executable" || return 1
  [ -f "$marker" ] && [ "$(<"$marker")" = "$marker_line" ] || return 1
  polaris_process_fields "$best_pid" && [ "$POLARIS_PROCESS_START_TIME" = "$best_start" ] || return 1
  polaris_xwayland_pid "$best_pid" \
    && polaris_pid_related_to_root "$best_pid" "$root_pid" \
    && polaris_pid_holds_inode "$best_pid" "$best_inode" || return 1
  final_inode="$(polaris_unique_unix_socket_inode "$best_socket")" || return 1
  [ "$final_inode" = "$best_inode" ] || return 1
  printf ':%s\n' "$best"
}

polaris_write_runtime_env() (
  local marker="$1" wayland="$2" expected_role="${3:-}" runtime_dir="$4" display final_display env_tmp=''
  local lock_bin="${POLARIS_FLOCK_BIN:-flock}" marker_line role
  umask 077
  trap 'rm -f "${env_tmp:-}"' EXIT
  if [ "${POLARIS_GAMESCOPE_LOCK_HELD:-0}" != 1 ]; then
    exec 9>>"$runtime_dir/polaris-gamescope.lock" || return 1
    "$lock_bin" -x 9 || return 1
  fi
  polaris_validate_marker "$marker" "$expected_role" || return 1
  local pid="$POLARIS_MARKER_PID" start_time="$POLARIS_MARKER_START_TIME" executable_path="$POLARIS_MARKER_EXECUTABLE"
  role="$POLARIS_MARKER_ROLE"
  marker_line="$(<"$marker")"
  polaris_marker_owns_socket "$marker" "$runtime_dir/$wayland" "$expected_role" || return 1
  display="$(polaris_discover_xwayland_display "$marker" "$expected_role")" || return 1
  polaris_validate_process_generation "$pid" "$start_time" "$executable_path" || return 1
  [ -f "$marker" ] && [ "$(<"$marker")" = "$marker_line" ] || return 1
  # umask already 077; write env atomically then rename (no nested subshell).
  env_tmp="$runtime_dir/polaris-gamescope.env.tmp.$$"
  printf 'DISPLAY=%s\nWAYLAND_DISPLAY=%s\nGAMESCOPE_WAYLAND_DISPLAY=%s\nPOLARIS_GAMESCOPE_PID=%s\nPOLARIS_GAMESCOPE_START_TIME=%s\nPOLARIS_GAMESCOPE_ROLE=%s\nPOLARIS_GAMESCOPE_EXECUTABLE=%s\n' \
    "$display" "$wayland" "$wayland" "$pid" "$start_time" "$role" "$executable_path" >"$env_tmp" || return 1
  if [ -n "${POLARIS_RUNTIME_ENV_BEFORE_COMMIT_HOOK:-}" ]; then
    "${POLARIS_RUNTIME_ENV_BEFORE_COMMIT_HOOK}" || return 1
  fi
  # Joint publication boundary: neither selected pathname may have rebound
  # while the other was being discovered or while the temporary file was built.
  polaris_validate_process_generation "$pid" "$start_time" "$executable_path" || return 1
  [ -f "$marker" ] && [ "$(<"$marker")" = "$marker_line" ] || return 1
  polaris_marker_owns_socket "$marker" "$runtime_dir/$wayland" "$expected_role" || return 1
  final_display="$(polaris_discover_xwayland_display "$marker" "$expected_role")" || return 1
  [ "$final_display" = "$display" ] || return 1
  # Explicit status: set -e is ignored when this function is used in if/||.
  # Only clear env_tmp after a successful rename so the EXIT trap still
  # removes a leftover temp when mv fails.
  mv -f "$env_tmp" "$runtime_dir/polaris-gamescope.env" || return 1
  env_tmp=''
)

# Hjem units live under ~/.config/systemd/user (higher priority than
# mask --runtime). Mask via $XDG_RUNTIME_DIR/systemd/user.control instead.
polaris_mask_idle_unit_runtime() {
  local unit="${1:-polaris-gamescope-idle.service}"
  local rt="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
  mkdir -p "$rt/systemd/user.control"
  ln -sfn /dev/null "$rt/systemd/user.control/$unit"
  rm -f "$rt/systemd/user/$unit"
  systemctl --user daemon-reload 2>/dev/null || true
  systemctl --user stop "$unit" 2>/dev/null || true
}

polaris_unmask_idle_unit_runtime() {
  local unit="${1:-polaris-gamescope-idle.service}"
  local rt="${XDG_RUNTIME_DIR:-/run/user/$(id -u)}"
  rm -f "$rt/systemd/user.control/$unit" "$rt/systemd/user/$unit"
  systemctl --user daemon-reload 2>/dev/null || true
  systemctl --user unmask --runtime "$unit" 2>/dev/null || true
}

polaris_private_session_alive() {
  local session_id="$1" proc_root process pid found=1 seen=0
  proc_root="$(polaris_proc_root)"
  [ -d "$proc_root" ] && [ -r "$proc_root" ] && [ -x "$proc_root" ] || return 2
  for process in "$proc_root"/[0-9]*; do
    [ -d "$process" ] || continue
    seen=1
    pid="${process##*/}"
    case "$pid" in ''|*[!0-9]*) continue ;; esac
    if ! polaris_process_fields "$pid"; then
      [ ! -e "$process" ] && continue
      return 2
    fi
    [ "$POLARIS_PROCESS_SESSION_ID" = "$session_id" ] || continue
    # Any live member of the authorized private session keeps teardown alive,
    # including descendants that moved to a separate process group.
    found=0
  done
  [ "$seen" = 1 ] || return 2
  return "$found"
}

polaris_stop_marked_gamescope() (
  local marker="$1" expected_role="$2" runtime_dir="$3" kill_bin="${POLARIS_KILL_BIN:-kill}"
  local lock_bin="${POLARIS_FLOCK_BIN:-flock}"
  local marker_line pid start_time executable_path pgid session_id group_leader_start group_leader_executable socket inode entry current_inode
  local owned_sockets=() term_steps="${POLARIS_STOP_WAIT_STEPS:-30}" kill_steps="${POLARIS_KILL_WAIT_STEPS:-20}" leader_stopped=0
  umask 077
  if [ "${POLARIS_GAMESCOPE_LOCK_HELD:-0}" != 1 ]; then
    exec 9>>"$runtime_dir/polaris-gamescope.lock" || return 1
    "$lock_bin" -x 9 || return 1
  fi
  polaris_validate_marker "$marker" "$expected_role" || return 1
  marker_line="$(<"$marker")"
  pid="$POLARIS_MARKER_PID"
  start_time="$POLARIS_MARKER_START_TIME"
  executable_path="$POLARIS_MARKER_EXECUTABLE"

  for socket in "$runtime_dir"/gamescope-[0-9]* "$runtime_dir"/gamescope-[0-9]*-ei; do
    [ -e "$socket" ] || [ -S "$socket" ] || continue
    if polaris_marker_owns_socket "$marker" "$socket" "$expected_role"; then
      inode="$(polaris_socket_inode "$socket")" || continue
      owned_sockets+=("$socket|$inode")
    fi
  done

  polaris_validate_process_generation "$pid" "$start_time" "$executable_path" || return 1
  pgid="$POLARIS_PROCESS_PGID"
  session_id="$POLARIS_PROCESS_SESSION_ID"
  [ "$pgid" -gt 1 ] 2>/dev/null && [ "$session_id" = "$pgid" ] || return 1
  polaris_process_fields "$pgid" || return 1
  [ "$POLARIS_PROCESS_PGID" = "$pgid" ] \
    && [ "$POLARIS_PROCESS_SESSION_ID" = "$session_id" ] || return 1
  group_leader_start="$POLARIS_PROCESS_START_TIME"
  group_leader_executable="$(readlink -f "$(polaris_proc_root)/$pgid/exe" 2>/dev/null)" || return 1
  [ -n "$group_leader_executable" ] || return 1
  [ -f "$marker" ] && [ "$(<"$marker")" = "$marker_line" ] || return 1
  polaris_validate_process_generation "$pid" "$start_time" "$executable_path" || return 1
  [ "$POLARIS_PROCESS_PGID" = "$pgid" ] \
    && [ "$POLARIS_PROCESS_SESSION_ID" = "$session_id" ] || return 1
  # Inline EXIT trap (not a nested function) so SC2329 does not flag a
  # "never invoked" helper. CONT only if we STOPped the leader and authorize it.
  trap '
    if [ "$leader_stopped" = 1 ] \
        && polaris_process_fields "$pgid" \
        && [ "$POLARIS_PROCESS_START_TIME" = "$group_leader_start" ] \
        && [ "$POLARIS_PROCESS_PGID" = "$pgid" ] \
        && [ "$POLARIS_PROCESS_SESSION_ID" = "$session_id" ] \
        && [ "$(readlink -f "$(polaris_proc_root)/$pgid/exe" 2>/dev/null)" = "$group_leader_executable" ]; then
      "$kill_bin" -CONT "$pgid" 2>/dev/null || true
    fi
  ' EXIT
  "$kill_bin" -STOP "$pgid" 2>/dev/null || return 1
  leader_stopped=1
  polaris_process_fields "$pgid" || return 1
  [ "$POLARIS_PROCESS_START_TIME" = "$group_leader_start" ] \
    && [ "$POLARIS_PROCESS_PGID" = "$pgid" ] \
    && [ "$POLARIS_PROCESS_SESSION_ID" = "$session_id" ] || return 1
  [ "$(readlink -f "$(polaris_proc_root)/$pgid/exe" 2>/dev/null)" = "$group_leader_executable" ] || return 1
  "$kill_bin" -TERM "-$pgid" 2>/dev/null || return 1
  group_rc=0
  for _ in $(seq 1 "$term_steps"); do
    [ -f "$marker" ] && [ "$(<"$marker")" = "$marker_line" ] || return 1
    if polaris_private_session_alive "$pgid"; then
      sleep 0.1
      continue
    else
      group_rc=$?
      [ "$group_rc" -eq 1 ] || return 1
      break
    fi
  done
  if polaris_private_session_alive "$pgid"; then
    [ -f "$marker" ] && [ "$(<"$marker")" = "$marker_line" ] || return 1
    # Keep the exact private-session leader allocation as an immutable PGID-reuse
    # barrier through the last negative-PGID operation. The marked compositor may
    # be a launcher child and may exit after TERM; only the retained leader can
    # authorize safe escalation.
    polaris_process_fields "$pgid" || return 1
    [ "$POLARIS_PROCESS_START_TIME" = "$group_leader_start" ] \
      && [ "$POLARIS_PROCESS_PGID" = "$pgid" ] \
      && [ "$POLARIS_PROCESS_SESSION_ID" = "$session_id" ] || return 1
    [ "$(readlink -f "$(polaris_proc_root)/$pgid/exe" 2>/dev/null)" = "$group_leader_executable" ] || return 1
    [ -f "$marker" ] && [ "$(<"$marker")" = "$marker_line" ] || return 1
    "$kill_bin" -KILL "-$pgid" 2>/dev/null || return 1
    for _ in $(seq 1 "$kill_steps"); do
      [ -f "$marker" ] && [ "$(<"$marker")" = "$marker_line" ] || return 1
      if polaris_private_session_alive "$pgid"; then
        sleep 0.1
        continue
      else
        group_rc=$?
        [ "$group_rc" -eq 1 ] || return 1
        break
      fi
    done
  else
    group_rc=$?
    [ "$group_rc" -eq 1 ] || return 1
  fi
  if polaris_private_session_alive "$pgid"; then
    return 1
  else
    group_rc=$?
    [ "$group_rc" -eq 1 ] || return 1
  fi

  # Runtime state belongs to the exact marker generation, not merely the PID.
  # Missing or changed authority fails closed and leaves env/socket state alone.
  [ -f "$marker" ] && [ "$(<"$marker")" = "$marker_line" ] || return 1
  if [ -f "$runtime_dir/polaris-gamescope.env" ] \
      && grep -qx "POLARIS_GAMESCOPE_PID=$pid" "$runtime_dir/polaris-gamescope.env" \
      && grep -qx "POLARIS_GAMESCOPE_START_TIME=$start_time" "$runtime_dir/polaris-gamescope.env" \
      && grep -qxF "POLARIS_GAMESCOPE_EXECUTABLE=$executable_path" "$runtime_dir/polaris-gamescope.env"; then
    [ "$(<"$marker")" = "$marker_line" ] || return 1
    rm -f "$runtime_dir/polaris-gamescope.env"
  fi
  for entry in "${owned_sockets[@]}"; do
    [ -f "$marker" ] && [ "$(<"$marker")" = "$marker_line" ] || return 1
    socket="${entry%|*}"
    inode="${entry##*|}"
    current_inode="$(polaris_socket_inode "$socket" 2>/dev/null || true)"
    if [ -n "$current_inode" ] && [ "$current_inode" = "$inode" ]; then
      [ "$(<"$marker")" = "$marker_line" ] || return 1
      polaris_remove_orphan_socket "$socket" || return 1
    fi
  done
  [ -f "$marker" ] && [ "$(<"$marker")" = "$marker_line" ] || return 1
  rm -f "$marker"
)
