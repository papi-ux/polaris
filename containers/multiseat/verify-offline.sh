#!/bin/sh
# Checksums are generated from the committed JSON locks in the private context.
# Run before any package script or compiler from those inputs can execute.
set -eu
cd "${1:-/inputs}"
test -d packages && test ! -L packages
test -f checksums.sha256 && test ! -L checksums.sha256
count=0
for package in packages/*.deb; do
  test -f "$package" && test ! -L "$package"
  count=$((count + 1))
done
test "$count" -gt 0
test "$count" -eq "$(awk '$2 ~ /^packages\// { count++ } END { print count+0 }' checksums.sha256)"
sha256sum --check --strict checksums.sha256
