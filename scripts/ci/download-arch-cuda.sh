#!/usr/bin/env bash
# Prefetch the large CUDA dependency without changing the installed toolchain.
# pacman's low-speed timeout can reject a temporary archive stall. Bound each
# download attempt externally instead; keep the pinned database and signatures.
set -euo pipefail

download_status=1
for attempt in 1 2 3; do
  if timeout --kill-after=30s 10m \
      pacman -Sw --noconfirm --needed --disable-download-timeout cuda; then
    exit 0
  else
    download_status=$?
  fi
  if [ "$attempt" -lt 3 ]; then
    printf 'CUDA download attempt %s failed (status %s); retrying cached download.\n' \
      "$attempt" "$download_status" >&2
    sleep 5
  fi
done

printf 'CUDA download failed after three bounded attempts (status %s).\n' "$download_status" >&2
exit "$download_status"
