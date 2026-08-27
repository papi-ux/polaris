#!/usr/bin/env bash
set -euo pipefail

: "${POLARIS_TEST_BINARY:?POLARIS_TEST_BINARY must name the Polaris executable}"

root="$(mktemp -d "${TMPDIR:-/tmp}/polaris-setup-host-home.XXXXXX")"
trap 'rm -rf -- "$root"' EXIT

home="$root/home"
xdg="$root/xdg"
mkdir -m 700 -- "$home" "$xdg"

env HOME="$home" XDG_CONFIG_HOME="$xdg" "$POLARIS_TEST_BINARY" --setup-host --help >/dev/null

# Preserve parser-supported prefixes before the command.
env HOME="$home" XDG_CONFIG_HOME="$xdg" "$POLARIS_TEST_BINARY" -0 --setup-host --help >/dev/null
env HOME="$home" XDG_CONFIG_HOME="$xdg" "$POLARIS_TEST_BINARY" min_log_level=2 --setup-host --help >/dev/null
env HOME="$home" XDG_CONFIG_HOME="$xdg" "$POLARIS_TEST_BINARY" "$root/nonexistent.conf" --setup-host --help >/dev/null

if [[ -n "$(find "$home" "$xdg" -mindepth 1 -print -quit)" ]]; then
  printf '%s\n' '--setup-host initialized per-user state before command dispatch' >&2
  find "$home" "$xdg" -mindepth 1 -print >&2
  exit 1
fi

assert_invalid_prefix_does_not_dispatch() {
  local label="$1"
  local expected_diagnostic="$2"
  shift 2
  local invalid_home="$root/$label-home"
  local invalid_xdg="$root/$label-xdg"
  local output
  mkdir -m 700 -- "$invalid_home" "$invalid_xdg"

  set +e
  output="$(env HOME="$invalid_home" XDG_CONFIG_HOME="$invalid_xdg" \
    "$POLARIS_TEST_BINARY" "$@" --setup-host 2>&1)"
  set -e

  if grep -Fq 'Polaris host setup requires root' <<<"$output"; then
    printf '%s\n' "invalid prefix reached --setup-host dispatch: $label" >&2
    printf '%s\n' "$output" >&2
    exit 1
  fi
  if ! grep -Fq "$expected_diagnostic" <<<"$output"; then
    printf '%s\n' "invalid prefix did not produce parser diagnostic: $label" >&2
    printf '%s\n' "$output" >&2
    exit 1
  fi
  if [[ -n "$(find "$invalid_home" "$invalid_xdg" -mindepth 1 -print -quit)" ]]; then
    printf '%s\n' "invalid prefix initialized per-user state: $label" >&2
    find "$invalid_home" "$invalid_xdg" -mindepth 1 -print >&2
    exit 1
  fi
}

assert_invalid_prefix_does_not_dispatch unsupported-short 'Unrecognized flag: [x]' -x
assert_invalid_prefix_does_not_dispatch malformed-option 'Usage:' '=invalid'

# The headless-boot flags must reach dispatch, refuse to act without root, echo
# themselves in the sudo hint, and touch nothing in the caller's home. Skipped
# under a root test runner because the command would then really enable
# lingering for the invoking account.
if [[ "$(id -u)" -ne 0 ]]; then
  headless_home="$root/headless-home"
  headless_xdg="$root/headless-xdg"
  mkdir -m 700 -- "$headless_home" "$headless_xdg"

  set +e
  output="$(env HOME="$headless_home" XDG_CONFIG_HOME="$headless_xdg" \
    "$POLARIS_TEST_BINARY" --setup-host --enable-headless-boot 2>&1)"
  rc=$?
  set -e

  if [[ $rc -eq 0 ]]; then
    printf '%s\n' '--enable-headless-boot succeeded without root' >&2
    exit 1
  fi
  if ! grep -Fq 'Polaris host setup requires root' <<<"$output"; then
    printf '%s\n' '--enable-headless-boot did not reach the root gate' >&2
    printf '%s\n' "$output" >&2
    exit 1
  fi
  if ! grep -Fq -- '--enable-headless-boot' <<<"$output"; then
    printf '%s\n' 'the sudo hint dropped --enable-headless-boot' >&2
    printf '%s\n' "$output" >&2
    exit 1
  fi
  if [[ -n "$(find "$headless_home" "$headless_xdg" -mindepth 1 -print -quit)" ]]; then
    printf '%s\n' '--enable-headless-boot wrote per-user state without root' >&2
    find "$headless_home" "$headless_xdg" -mindepth 1 -print >&2
    exit 1
  fi
fi