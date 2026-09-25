#!/usr/bin/env bash
# Prefetch the large CUDA dependency without changing the installed toolchain.
# pacman's low-speed timeout can reject a temporary archive stall. Bound each
# download attempt externally instead; keep the pinned database and signatures.
set -euo pipefail

# An attempt that hits the deadline is killed while pacman holds the database
# lock, and the lock outlives it. Every later attempt then fails instantly with
# "could not lock database: File exists", so the retry that exists to resume a
# partial download could never run, and the log blamed a lock while the real
# cause was a 4.8 GB download that needed more than one attempt's worth of time.
#
# Clearing a lock no live pacman owns is what makes the loop do what it says:
# -Sw resumes into the same package cache, so three bounded attempts become
# three resumes rather than three restarts.
pacman_lock="${POLARIS_PACMAN_LOCK:-/var/lib/pacman/db.lck}"

release_stale_lock() {
  [ -e "$pacman_lock" ] || return 0
  if command -v pgrep >/dev/null 2>&1 && pgrep -x pacman >/dev/null 2>&1; then
    printf 'pacman is still running; leaving %s in place\n' "$pacman_lock" >&2
    return 0
  fi
  printf 'clearing the database lock a killed attempt left behind\n' >&2
  rm -f "$pacman_lock"
}

download_status=1
for attempt in 1 2 3; do
  release_stale_lock
  if timeout --kill-after=30s 10m \
      pacman -Sw --noconfirm --needed --disable-download-timeout cuda; then
    exit 0
  else
    download_status=$?
  fi
  if [ "$attempt" -lt 3 ]; then
    if [ "$download_status" -eq 124 ]; then
      printf 'CUDA download attempt %s hit the deadline; resuming the cached download.\n' \
        "$attempt" >&2
    else
      printf 'CUDA download attempt %s failed (status %s); retrying cached download.\n' \
        "$attempt" "$download_status" >&2
    fi
    sleep 5
  fi
done

printf 'CUDA download failed after three bounded attempts (status %s).\n' "$download_status" >&2
exit "$download_status"
