#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -ne 2 ]; then
  echo "Usage: $0 <polaris-binary> <strings-report>" >&2
  exit 2
fi

binary="$1"
report="$2"

if [ ! -r "$binary" ]; then
  echo "Polaris binary is not readable: $binary" >&2
  exit 2
fi

if [[ "$binary" -ef "$report" ]]; then
  echo "Binary and report must be different files" >&2
  exit 2
fi

mkdir -p "$(dirname "$report")"
LC_ALL=C strings -a "$binary" > "$report"

grep_status=0
grep -Eq '/__w/|src_assets/.*/assets/shaders' "$report" || grep_status=$?
case "$grep_status" in
  0)
    echo "Packaged Polaris binary contains a forbidden build/source path" >&2
    exit 1
    ;;
  1)
    ;;
  *)
    echo "Failed to scan packaged Polaris binary strings (grep status $grep_status)" >&2
    exit 2
    ;;
esac
