#!/usr/bin/env bash
# Phase timing for a CI step that is one opaque block.
#
# The SteamOS package build is a single workflow step wrapping a single `docker run`, so an hour of
# wall clock arrives as one undifferentiated log. Nothing says whether the time went to Valve's
# frozen mirrors, to populating the chroot, or to compiling Polaris, which makes it impossible to
# decide what is worth caching. This records where the time actually goes.
#
# Sourcing this file has no effect beyond defining functions; the caller starts the clock. Timings
# are appended as they complete, so a build that dies still says which phase it died in.
#
# Usage:
#   . "$SOURCE/scripts/ci/phase-timings.sh"
#   phase_timings_init /output/steamos3.8-phase-timings.txt
#   phase mirror-sync
#   ...
#   phase pacstrap
#   ...
#   phase_timings_finish        # closes the phase in flight and prints the summary

PHASE_TIMINGS_FILE=
PHASE_TIMINGS_NAME=
PHASE_TIMINGS_START=0

phase_timings_now() {
  date +%s
}

phase_timings_init() {
  PHASE_TIMINGS_FILE="$1"
  PHASE_TIMINGS_NAME=
  PHASE_TIMINGS_START=0
  : > "$PHASE_TIMINGS_FILE"
}

# Closes the phase in flight, if any, and opens the named one. An empty name only closes.
phase() {
  local next="${1-}" now elapsed
  now="$(phase_timings_now)"
  if [ -n "$PHASE_TIMINGS_NAME" ]; then
    elapsed="$((now - PHASE_TIMINGS_START))"
    printf '%s\t%s\n' "$PHASE_TIMINGS_NAME" "$elapsed" >> "$PHASE_TIMINGS_FILE"
    printf '::endgroup::\n'
    printf '::notice title=SteamOS phase::%s took %ss\n' "$PHASE_TIMINGS_NAME" "$elapsed"
  fi
  PHASE_TIMINGS_NAME="$next"
  PHASE_TIMINGS_START="$now"
  if [ -n "$next" ]; then
    # A workflow command in step output is honoured whatever produced it, including a process inside
    # a container, so this makes the single step navigable.
    printf '::group::steamos phase: %s\n' "$next"
  fi
}

# Safe to call more than once: the second call has no phase in flight and nothing left to total.
phase_timings_finish() {
  phase ''
  [ -s "$PHASE_TIMINGS_FILE" ] || return 0
  printf '\n==== phase timings ====\n'
  awk -F'\t' '{ total += $2; printf "  %-22s %6ds\n", $1, $2 }
              END { printf "  %-22s %6ds\n", "total", total }' "$PHASE_TIMINGS_FILE"
  printf '=======================\n\n'
}
