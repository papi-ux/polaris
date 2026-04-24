#!/usr/bin/env bash
set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
build_dir="${POLARIS_WEB_BUILD_DIR:-$repo_root/build/assets/web}"
asset_dir="${POLARIS_WEB_ASSET_DIR:-/usr/local/assets/web}"
skip_build=0

for arg in "$@"; do
  case "$arg" in
    --skip-build)
      skip_build=1
      ;;
    -h|--help)
      printf 'Usage: %s [--skip-build]\n' "$0"
      printf 'Environment: POLARIS_WEB_BUILD_DIR, POLARIS_WEB_ASSET_DIR\n'
      exit 0
      ;;
    *)
      printf 'Unknown argument: %s\n' "$arg" >&2
      exit 2
      ;;
  esac
done

if [[ "$skip_build" -eq 0 ]]; then
  (cd "$repo_root" && npm run build)
fi

if [[ ! -f "$build_dir/index.html" ]]; then
  printf 'Missing build output: %s/index.html\n' "$build_dir" >&2
  exit 1
fi

asset_parent="$(dirname "$asset_dir")"
asset_name="$(basename "$asset_dir")"
sudo_cmd=()
if [[ "${EUID:-$(id -u)}" -ne 0 ]]; then
  if [[ ( -e "$asset_dir" && ! -w "$asset_dir" ) || ( ! -e "$asset_dir" && ! -w "$asset_parent" ) ]]; then
    sudo_cmd=(sudo)
  fi
fi

backup_root="${POLARIS_WEB_BACKUP_DIR:-$asset_parent}"
backup_sudo_cmd=()
if [[ "${EUID:-$(id -u)}" -ne 0 && ! -w "$backup_root" ]]; then
  if [[ -n "${POLARIS_WEB_BACKUP_DIR:-}" || "${#sudo_cmd[@]}" -gt 0 ]]; then
    backup_sudo_cmd=(sudo)
  elif sudo -n true 2>/dev/null; then
    backup_sudo_cmd=(sudo)
  else
    backup_root="${XDG_CACHE_HOME:-$HOME/.cache}/polaris/web-assets-backups"
    mkdir -p "$backup_root"
  fi
fi

timestamp="$(date +%Y%m%d-%H%M%S)"
backup_dir="${backup_root}/${asset_name}.backup-${timestamp}"

if [[ -e "$asset_dir" ]]; then
  "${backup_sudo_cmd[@]}" mkdir -p "$backup_root"
  "${backup_sudo_cmd[@]}" cp -a "$asset_dir" "$backup_dir"
fi

"${sudo_cmd[@]}" mkdir -p "$asset_dir"
"${sudo_cmd[@]}" rsync -a --delete "$build_dir"/ "$asset_dir"/

build_ref="$(grep -o 'assets/index-[^"]*\.js' "$build_dir/index.html" | head -n1 || true)"
live_ref="$(grep -o 'assets/index-[^"]*\.js' "$asset_dir/index.html" | head -n1 || true)"

if [[ -z "$build_ref" || "$build_ref" != "$live_ref" ]]; then
  printf 'Asset sync verification failed: build=%s live=%s\n' "${build_ref:-missing}" "${live_ref:-missing}" >&2
  exit 1
fi

printf 'Synced %s to %s\n' "$build_ref" "$asset_dir"
if [[ -e "$backup_dir" ]]; then
  printf 'Backed up previous assets to %s\n' "$backup_dir"
fi
