#!/usr/bin/env bash

set -euo pipefail

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
classifier="$repo_root/scripts/ci/classify-native-build.sh"

assert_scope() {
  local expected="$1"
  shift

  local actual
  actual="$(printf '%s\0' "$@" | "$classifier")"
  if [ "$actual" != "$expected" ]; then
    echo "Expected native_required=$expected, got $actual for: $*" >&2
    exit 1
  fi
}

assert_scope false \
  src_assets/common/assets/web/app.css
assert_scope false \
  src_assets/common/assets/web/views/DashboardView.vue \
  'src_assets/common/assets/web/tests/layout with spaces.test.js'
assert_scope true \
  src/video.cpp
assert_scope true \
  src_assets/common/assets/web/app.css \
  tests/unit/test_video.cpp
assert_scope true \
  package-lock.json

empty_scope="$(printf '' | "$classifier")"
if [ "$empty_scope" != true ]; then
  echo "Expected an empty diff to require native validation" >&2
  exit 1
fi
