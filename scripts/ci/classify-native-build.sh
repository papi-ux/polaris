#!/usr/bin/env bash

set -euo pipefail

if [ "$#" -ne 0 ]; then
  echo "Usage: provide NUL-delimited changed paths on stdin" >&2
  exit 2
fi

native_required=false
path_count=0

while IFS= read -r -d '' changed_path; do
  path_count=$((path_count + 1))
  case "$changed_path" in
    src_assets/common/assets/web/*)
      ;;
    *)
      native_required=true
      ;;
  esac
done

# An empty diff is unexpected for a pull request. Fail closed so a broken
# comparison cannot silently bypass native validation.
if [ "$path_count" -eq 0 ]; then
  native_required=true
fi

printf '%s\n' "$native_required"
