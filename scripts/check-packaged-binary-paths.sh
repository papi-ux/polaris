#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 2 ] || [ "$#" -gt 3 ]; then
  echo "Usage: $0 <polaris-binary> <strings-report> [--forbid-native-pipewire-audio]" >&2
  exit 2
fi

binary="$1"
report="$2"
native_audio_policy="${3:-}"

case "$native_audio_policy" in
  "" | --forbid-native-pipewire-audio)
    ;;
  *)
    echo "Unknown packaged-binary policy: $native_audio_policy" >&2
    exit 2
    ;;
esac

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

if ! grep -Fq "portal: PipeWire format negotiated:" "$report"; then
  echo "Packaged Polaris binary does not contain XDG Desktop Portal capture support" >&2
  exit 1
fi

if [ "$native_audio_policy" = "--forbid-native-pipewire-audio" ]; then
  native_audio_status=0
  grep -Fq "PipeWire detected, will prefer native PipeWire for audio capture" "$report" || native_audio_status=$?
  case "$native_audio_status" in
    0)
      echo "Packaged Polaris binary unexpectedly contains native PipeWire audio support" >&2
      exit 1
      ;;
    1)
      ;;
    *)
      echo "Failed to scan for native PipeWire audio support (grep status $native_audio_status)" >&2
      exit 2
      ;;
  esac
fi
