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
  local marker="$1" pid="$2" role="$3" tmp lock_bin="${POLARIS_FLOCK_BIN:-flock}"
  umask 077
  exec 9>>"${marker%/*}/polaris-gamescope.lock" || return 1
  "$lock_bin" -x 9 || return 1
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
      tmp="$marker.tmp.$$"
      (umask 077; printf '%s %s %s %s\n' "$pid" "$start_time" "$role" "$executable_path" >"$tmp") || return 1
      mv -f "$tmp" "$marker"
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

polaris_any_process_holds_inode() {
  local inode="$1" process pid
  for process in "$(polaris_proc_root)"/[0-9]*; do
    [ -d "$process" ] || continue
    pid="${process##*/}"
    case "$pid" in ''|*[!0-9]*) continue ;; esac
    if polaris_pid_holds_inode "$pid" "$inode"; then
      return 0
    fi
  done
  return 1
}

# True (0) when $1 is missing or has no live holder — safe to unlink.
# False (1) when a live process holds the socket or the pathname is ambiguous.
polaris_socket_is_orphan() {
  local socket="$1" inode path found="" count=0
  [ -e "$socket" ] || return 0
  while read -r _ _ _ _ _ _ inode path _; do
    [ "$path" = "$socket" ] || continue
    case "$inode" in ''|*[!0-9]*) continue ;; esac
    count=$((count + 1))
    found="$inode"
  done <"$(polaris_proc_net_unix)" 2>/dev/null
  # Duplicate pathname rows are ambiguous (unlink/rebind); refuse reclaim.
  [ "$count" -le 1 ] || return 1
  # Filesystem residue with no /proc/net/unix listener is safe to remove.
  [ "$count" -eq 1 ] || return 0
  if polaris_any_process_holds_inode "$found"; then
    return 1
  fi
  return 0
}

# Remove one socket path if orphaned. 0 = missing/removed, 1 = live holder.
polaris_remove_orphan_socket() {
  local socket="$1"
  if [ ! -e "$socket" ]; then
    rm -f "$socket.lock" 2>/dev/null || true
    return 0
  fi
  polaris_socket_is_orphan "$socket" || return 1
  echo "polaris: reclaiming orphan socket $socket" >&2
  rm -f "$socket" "$socket.lock" 2>/dev/null || true
  return 0
}

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
    else
      rm -f "$socket.lock" 2>/dev/null || true
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

# gamescope may reparent Xwayland under the user manager while keeping the
# service cgroup; treat same-cgroup as related when ancestry is gone.
polaris_pid_same_cgroup() {
  local a="$1" b="$2" ca cb
  ca="$(cat "$(polaris_proc_root)/$a/cgroup" 2>/dev/null)" || return 1
  cb="$(cat "$(polaris_proc_root)/$b/cgroup" 2>/dev/null)" || return 1
  [ -n "$ca" ] && [ "$ca" = "$cb" ]
}

polaris_pid_related_to_root() {
  local pid="$1" root="$2"
  [ "$pid" = "$root" ] && return 0
  polaris_pid_is_descendant "$pid" "$root" && return 0
  polaris_pid_same_cgroup "$pid" "$root"
}

polaris_discover_xwayland_display() {
  local marker="$1" expected_role="${2:-}" xdir socket name display inode path process pid best=
  polaris_validate_marker "$marker" "$expected_role" || return 1
  local root_pid="$POLARIS_MARKER_PID"
  xdir="$(polaris_x11_socket_dir)"
  for socket in "$xdir"/X*; do
    [ -e "$socket" ] || continue
    name="${socket##*/}"
    display="${name#X}"
    case "$display" in ''|*[!0-9]*) continue ;; esac
    # Walk every /proc/net/unix row for this path. Ambiguous unlink/rebind
    # residue is ok if a related Xwayland still holds one of the inodes.
    while read -r _ _ _ _ _ _ inode path _; do
      [ "$path" = "$socket" ] || continue
      case "$inode" in ''|*[!0-9]*) continue ;; esac
      for process in "$(polaris_proc_root)"/[0-9]*; do
        [ -d "$process" ] || continue
        pid="${process##*/}"
        case "$pid" in ''|*[!0-9]*) continue ;; esac
        [ "$pid" != "$root_pid" ] || continue
        if polaris_xwayland_pid "$pid" && polaris_pid_related_to_root "$pid" "$root_pid" \
            && polaris_pid_holds_inode "$pid" "$inode"; then
          if [ -z "$best" ] || [ "$display" -lt "$best" ]; then
            best="$display"
          fi
        fi
      done
    done <"$(polaris_proc_net_unix)" 2>/dev/null
  done
  [ -n "$best" ] || return 1
  printf ':%s\n' "$best"
}

polaris_write_runtime_env() (
  local marker="$1" wayland="$2" expected_role="${3:-}" runtime_dir="$4" display tmp
  local lock_bin="${POLARIS_FLOCK_BIN:-flock}" marker_line role
  umask 077
  exec 9>>"$runtime_dir/polaris-gamescope.lock" || return 1
  "$lock_bin" -x 9 || return 1
  polaris_validate_marker "$marker" "$expected_role" || return 1
  local pid="$POLARIS_MARKER_PID" start_time="$POLARIS_MARKER_START_TIME" executable_path="$POLARIS_MARKER_EXECUTABLE"
  role="$POLARIS_MARKER_ROLE"
  marker_line="$(<"$marker")"
  polaris_marker_owns_socket "$marker" "$runtime_dir/$wayland" "$expected_role" || return 1
  display="$(polaris_discover_xwayland_display "$marker" "$expected_role")" || return 1
  polaris_validate_process_generation "$pid" "$start_time" "$executable_path" || return 1
  [ -f "$marker" ] && [ "$(<"$marker")" = "$marker_line" ] || return 1
  tmp="$runtime_dir/polaris-gamescope.env.tmp.$$"
  (umask 077; printf 'DISPLAY=%s\nWAYLAND_DISPLAY=%s\nGAMESCOPE_WAYLAND_DISPLAY=%s\nPOLARIS_GAMESCOPE_PID=%s\nPOLARIS_GAMESCOPE_START_TIME=%s\nPOLARIS_GAMESCOPE_ROLE=%s\nPOLARIS_GAMESCOPE_EXECUTABLE=%s\n' \
    "$display" "$wayland" "$wayland" "$pid" "$start_time" "$role" "$executable_path" >"$tmp") || return 1
  mv -f "$tmp" "$runtime_dir/polaris-gamescope.env"
)

# Hjem / NixOS install polaris units under ~/.config/systemd/user, which has
# higher search priority than $XDG_RUNTIME_DIR/systemd/user. Plain
# `systemctl --user mask --runtime` therefore does NOT mask those units, and
# polaris-portal-gamescope Wants= / polaris Wants= keep restarting idle under
# nested WSI (yield loop). Mask via user.control (first path) instead.
polaris_idle_unit_control_dir() {
  printf '%s\n' "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/systemd/user.control"
}

polaris_mask_idle_unit_runtime() {
  local unit="${1:-polaris-gamescope-idle.service}"
  local control
  control="$(polaris_idle_unit_control_dir)"
  mkdir -p "$control"
  ln -sfn /dev/null "$control/$unit"
  # Drop legacy ineffective mask --runtime symlink if present.
  rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/systemd/user/$unit"
  systemctl --user daemon-reload 2>/dev/null || true
  systemctl --user stop "$unit" 2>/dev/null || true
}

polaris_unmask_idle_unit_runtime() {
  local unit="${1:-polaris-gamescope-idle.service}"
  local control
  control="$(polaris_idle_unit_control_dir)"
  rm -f "$control/$unit"
  rm -f "${XDG_RUNTIME_DIR:-/run/user/$(id -u)}/systemd/user/$unit"
  systemctl --user daemon-reload 2>/dev/null || true
  systemctl --user unmask --runtime "$unit" 2>/dev/null || true
}

polaris_stop_marked_gamescope() (
  local marker="$1" expected_role="$2" runtime_dir="$3" kill_bin="${POLARIS_KILL_BIN:-kill}"
  local lock_bin="${POLARIS_FLOCK_BIN:-flock}"
  local marker_line pid start_time executable_path socket inode entry current_inode
  local owned_sockets=() term_steps="${POLARIS_STOP_WAIT_STEPS:-30}" kill_steps="${POLARIS_KILL_WAIT_STEPS:-20}"
  umask 077
  exec 9>>"$runtime_dir/polaris-gamescope.lock" || return 1
  "$lock_bin" -x 9 || return 1
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
  [ -f "$marker" ] && [ "$(<"$marker")" = "$marker_line" ] || return 1
  "$kill_bin" -TERM "-$pid" 2>/dev/null || "$kill_bin" -TERM "$pid" 2>/dev/null || return 1
  for _ in $(seq 1 "$term_steps"); do
    [ -f "$marker" ] && [ "$(<"$marker")" = "$marker_line" ] || return 1
    if ! polaris_validate_process_generation "$pid" "$start_time" "$executable_path"; then
      break
    fi
    sleep 0.1
  done
  if polaris_validate_process_generation "$pid" "$start_time" "$executable_path"; then
    [ -f "$marker" ] && [ "$(<"$marker")" = "$marker_line" ] || return 1
    "$kill_bin" -KILL "-$pid" 2>/dev/null || "$kill_bin" -KILL "$pid" 2>/dev/null || return 1
    for _ in $(seq 1 "$kill_steps"); do
      [ -f "$marker" ] && [ "$(<"$marker")" = "$marker_line" ] || return 1
      polaris_validate_process_generation "$pid" "$start_time" "$executable_path" || break
      sleep 0.1
    done
  fi
  polaris_validate_process_generation "$pid" "$start_time" "$executable_path" && return 1

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
      rm -f "$socket" "$socket.lock"
    fi
  done
  [ -f "$marker" ] && [ "$(<"$marker")" = "$marker_line" ] || return 1
  rm -f "$marker"
)
