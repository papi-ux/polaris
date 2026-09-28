#!/usr/bin/env bash
# Stock gamescope, SteamOS's and Fedora's included, exits on
# --pipewire-composite-cursor, which only Polaris's patched build accepts
# (#792). Both launchers must ask the exact binary before passing it. This
# checks the runtime library's probe against fake gamescopes, then runs the
# idle compositor and the nested session launcher far enough to capture the
# arguments each one hands gamescope.
set -euo pipefail

fail() {
  printf 'FAIL: %s\n' "$*" >&2
  exit 1
}

source_dir="${POLARIS_SOURCE_DIR:?}"
runtime_lib="$source_dir/nix/modules/polaris-gamescope-runtime-lib.sh"
idle_script="$source_dir/scripts/install/lib/polaris-gamescope-idle.sh"
session_script="$source_dir/nix/modules/polaris-gamescope-session.sh"
work="$(mktemp -d "${TMPDIR:-/tmp}/polaris-gamescope-composite-cursor.XXXXXX")"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/bin"
probes="$work/probes"
args="$work/args"
: >"$probes"
export POLARIS_TEST_PROBES="$probes" POLARIS_TEST_ARGS="$args"

# A fake gamescope prints its usage on stderr for --help, as the real one does,
# and records that it was asked. Files beside it decide whether the usage lists
# the option or only a longer one that starts with its name, whether --help
# hangs, and its exit status. Any other call records its arguments and, like
# stock gamescope, refuses the option it does not have.
make_gamescope() {
  local dir="$work/$1" lists="$2" help_status="$3"
  mkdir -p "$dir"
  cat >"$dir/gamescope" <<'EOF'
#!/usr/bin/env bash
dir="$(cd "$(dirname "$0")" && pwd)"
if [ "${1:-}" = --help ]; then
  printf '%s\n' "$dir" >>"$POLARIS_TEST_PROBES"
  [ -e "$dir/hangs" ] && sleep 30
  printf 'usage: gamescope [options...] -- [command...]\n\nOptions:\n  --help                         show help message\n' >&2
  if [ -e "$dir/lists-option" ]; then
    printf '  --pipewire-composite-cursor    composite the cursor into the PipeWire capture stream\n' >&2
  fi
  if [ -e "$dir/lists-lookalike" ]; then
    printf '  --pipewire-composite-cursor-scale N    scale of the composited cursor\n' >&2
  fi
  exit "$(cat "$dir/help-status")"
fi
printf '%s\n' "$@" >"$POLARIS_TEST_ARGS"
for arg in "$@"; do
  [ "$arg" = -- ] && break
  if [ "$arg" = --pipewire-composite-cursor ] && [ ! -e "$dir/lists-option" ]; then
    printf "gamescope: unrecognized option '--pipewire-composite-cursor'\n" >&2
    exit 1
  fi
done
exit 0
EOF
  chmod +x "$dir/gamescope"
  printf '%s\n' "$help_status" >"$dir/help-status"
  case "$lists" in
    lists) : >"$dir/lists-option" ;;
    lookalike) : >"$dir/lists-lookalike" ;;
  esac
  return 0
}

probe_count() {
  grep -cxF "$work/$1" "$probes" || true
}

make_gamescope stock plain 0
make_gamescope patched lists 0
make_gamescope patched-mtime lists 0
make_gamescope patched-noisy lists 1
make_gamescope stock-noisy plain 1
make_gamescope patched-forced-off lists 0
make_gamescope stock-forced-on plain 0
make_gamescope patched-other-value lists 0
make_gamescope patched-empty lists 0
make_gamescope stock-hangs plain 0
: >"$work/stock-hangs/hangs"
make_gamescope stock-lookalike lookalike 0

# --- The probe itself, in this shell so its cache lives across calls. ---
# shellcheck source=/dev/null
. "$runtime_lib"
unset POLARIS_GAMESCOPE_COMPOSITE_CURSOR

decision=""
decision_log=""
decide() {
  if polaris_gamescope_composite_cursor_enabled "$1" cursor-test 2>"$work/decide.log"; then
    decision=on
  else
    decision=off
  fi
  decision_log="$(<"$work/decide.log")"
}

decide "$work/stock/gamescope"
[ "$decision" = off ] || fail "a gamescope without the option was given it"
case "$decision_log" in
  "cursor-test: composite cursor off ($work/stock/gamescope does not list --pipewire-composite-cursor;"*) ;;
  *) fail "no line said the stock gamescope lacks the option: $decision_log" ;;
esac
decide "$work/stock/gamescope"
[ "$decision" = off ] || fail "the cached answer for stock gamescope changed"
[ "$(probe_count stock)" = 1 ] || fail "stock gamescope was asked $(probe_count stock) times, not once"

decide "$work/patched/gamescope"
[ "$decision" = on ] || fail "a gamescope that lists the option was not given it"
[ "$decision_log" = "cursor-test: composite cursor on ($work/patched/gamescope lists --pipewire-composite-cursor)" ] ||
  fail "no line said the patched gamescope has the option: $decision_log"
decide "$work/patched/gamescope"
[ "$decision" = on ] || fail "the cached answer for patched gamescope changed"
# The launchers' default is a bare name resolved through PATH; it is the same
# binary, so it is the same cache entry.
PATH="$work/patched:$PATH" decide gamescope
[ "$decision" = on ] || fail "a bare gamescope name did not resolve through PATH"
# A shell function of that name is not what exec runs, so it is not what is
# asked either. It shadows the name and is never called.
# shellcheck disable=SC2329
gamescope() { return 0; }
PATH="$work/patched:$PATH" decide gamescope
unset -f gamescope
[ "$decision" = on ] || fail "a shell function named gamescope hid the binary on PATH: $decision_log"
[ "$(probe_count patched)" = 1 ] || fail "patched gamescope was asked $(probe_count patched) times, not once"
[ "$(probe_count stock)" = 1 ] || fail "asking one gamescope asked another"

# A binary replaced in place is asked again.
decide "$work/patched-mtime/gamescope"
touch -d '2001-02-03 04:05:06' "$work/patched-mtime/gamescope"
decide "$work/patched-mtime/gamescope"
[ "$(probe_count patched-mtime)" = 2 ] || fail "a changed modification time did not ask again"

# The exit status of --help does not decide; the usage text does.
decide "$work/patched-noisy/gamescope"
[ "$decision" = on ] || fail "a non-zero --help exit hid an option the usage lists"
decide "$work/stock-noisy/gamescope"
[ "$decision" = off ] || fail "a non-zero --help exit without the option turned it on"
case "$decision_log" in
  *"--help exited 1;"*) ;;
  *) fail "the off line did not mention the failed --help: $decision_log" ;;
esac

# A --help that never returns is cut off, and counts as not listing the option.
decide "$work/stock-hangs/gamescope"
[ "$decision" = off ] || fail "a gamescope whose --help hangs was given the option"
case "$decision_log" in
  *"--help timed out after 5 s;"*) ;;
  *) fail "the off line did not say --help timed out: $decision_log" ;;
esac

# Only the option itself counts, not a longer one that starts with its name.
decide "$work/stock-lookalike/gamescope"
[ "$decision" = off ] || fail "a longer option that starts with the name turned the option on"

decide "$work/missing/gamescope"
[ "$decision" = off ] || fail "a gamescope that is not there was given the option"
case "$decision_log" in
  *"not found to ask"*) ;;
  *) fail "no line said the gamescope was not found: $decision_log" ;;
esac

POLARIS_GAMESCOPE_COMPOSITE_CURSOR=0 decide "$work/patched-forced-off/gamescope"
[ "$decision" = off ] || fail "POLARIS_GAMESCOPE_COMPOSITE_CURSOR=0 did not turn the option off"
[ "$decision_log" = "cursor-test: composite cursor off (POLARIS_GAMESCOPE_COMPOSITE_CURSOR=0)" ] ||
  fail "no line said the environment turned it off: $decision_log"
[ "$(probe_count patched-forced-off)" = 0 ] || fail "POLARIS_GAMESCOPE_COMPOSITE_CURSOR=0 still asked gamescope"

POLARIS_GAMESCOPE_COMPOSITE_CURSOR=1 decide "$work/stock-forced-on/gamescope"
[ "$decision" = on ] || fail "POLARIS_GAMESCOPE_COMPOSITE_CURSOR=1 did not force the option on"
[ "$decision_log" = "cursor-test: composite cursor on (forced by POLARIS_GAMESCOPE_COMPOSITE_CURSOR=1)" ] ||
  fail "no line said the environment forced it on: $decision_log"
[ "$(probe_count stock-forced-on)" = 0 ] || fail "POLARIS_GAMESCOPE_COMPOSITE_CURSOR=1 still asked gamescope"

# Before the probe, any value but 1 turned the option off. That still holds.
POLARIS_GAMESCOPE_COMPOSITE_CURSOR=false decide "$work/patched-other-value/gamescope"
[ "$decision" = off ] || fail "a value other than 0 or 1 no longer turns the option off"
POLARIS_GAMESCOPE_COMPOSITE_CURSOR='' decide "$work/patched-empty/gamescope"
[ "$decision" = on ] || fail "an empty POLARIS_GAMESCOPE_COMPOSITE_CURSOR did not ask the binary"

# --- The launchers, with the real library and fake gamescopes. ---
# The ownership steps a scratch runtime directory cannot satisfy are stubbed;
# the probe is the production one.
cat >"$work/runtime-lib.sh" <<'EOF'
# shellcheck source=/dev/null
. "$POLARIS_TEST_RUNTIME_LIB"
polaris_validate_marker() { return 1; }
polaris_reclaim_orphan_gamescope_sockets() { return 0; }
polaris_headless_gamescope_pid() { return 1; }
polaris_write_marker_for_pid() { return 0; }
polaris_read_marker() {
  POLARIS_MARKER_PID=0
  POLARIS_MARKER_START_TIME=1
  POLARIS_MARKER_ROLE=idle
}
polaris_write_runtime_env() { return 0; }
EOF
cat >"$work/bin/flock" <<'EOF'
#!/usr/bin/env bash
exit 0
EOF
# A distro package: neither the idle compositor unit nor the private portal.
cat >"$work/bin/systemctl" <<'EOF'
#!/usr/bin/env bash
case "$*" in
  *'show -p LoadState --value'*) printf 'not-found\n' ;;
esac
exit 0
EOF
cat >"$work/bin/pactl" <<'EOF'
#!/usr/bin/env bash
exit 0
EOF
cat >"$work/bin/pgrep" <<'EOF'
#!/usr/bin/env bash
exit 1
EOF
chmod +x "$work/bin/flock" "$work/bin/systemctl" "$work/bin/pactl" "$work/bin/pgrep"

launcher_log="$work/launcher.log"
run_launcher() {
  local launcher="$1" gamescope="$2"
  shift 2
  rm -rf "$work/run" "$args"
  mkdir -p "$work/run"
  : >"$probes"
  local -a command=(bash "$idle_script")
  [ "$launcher" = session ] && command=(bash "$session_script" start)
  env -u POLARIS_GAMESCOPE_COMPOSITE_CURSOR -u POLARIS_SESSION_PATH \
    -u POLARIS_SESSION_OPERATION_LOCK_HELD -u POLARIS_GAMESCOPE_LOCK_HELD \
    -u POLARIS_CLIENT_HDR -u POLARIS_GAMESCOPE_WSI -u POLARIS_GAMESCOPE_PREFER_VK \
    "$@" \
    PATH="$work/bin:$PATH" \
    XDG_RUNTIME_DIR="$work/run" \
    POLARIS_GAMESCOPE_RUNTIME_LIB="$work/runtime-lib.sh" \
    POLARIS_TEST_RUNTIME_LIB="$runtime_lib" \
    POLARIS_FLOCK_BIN="$work/bin/flock" \
    POLARIS_GAMESCOPE_BIN="$work/$gamescope/gamescope" \
    POLARIS_SESSION_INSTANCE_ID=composite-cursor-test \
    POLARIS_IDLE_WAIT_STEPS=2 \
    "${command[@]}" >"$launcher_log" 2>&1 || true
  for _ in $(seq 1 50); do
    [ -s "$args" ] && break
    sleep 0.1
  done
  [ -s "$args" ] || fail "$launcher never started $gamescope: $(tail -5 "$launcher_log")"
}

# Only what comes before gamescope's own "--" are its options.
passed_flag() {
  awk '$0 == "--" { exit } { print }' "$args" | grep -qxF -- --pipewire-composite-cursor
}

expect_launch() {
  local launcher="$1" gamescope="$2" want="$3" asked="$4" said="$5"
  shift 5
  run_launcher "$launcher" "$gamescope" "$@"
  if [ "$want" = on ]; then
    passed_flag || fail "$launcher did not pass the option to $gamescope ($*)"
  else
    ! passed_flag || fail "$launcher passed the option to $gamescope ($*)"
  fi
  grep -qxF -- '--backend' "$args" || fail "$launcher recorded no gamescope options for $gamescope"
  [ "$(probe_count "$gamescope")" = "$asked" ] ||
    fail "$launcher asked $gamescope $(probe_count "$gamescope") times, expected $asked"
  [ "$(grep -c 'composite cursor' "$launcher_log" || true)" = 1 ] ||
    fail "$launcher did not log exactly one composite cursor line: $(grep 'composite cursor' "$launcher_log" || true)"
  grep -qF -- "polaris-gamescope-$launcher: composite cursor $said" "$launcher_log" ||
    fail "$launcher did not log 'composite cursor $said': $(grep 'composite cursor' "$launcher_log" || true)"
}

for launcher in idle session; do
  expect_launch "$launcher" stock off 1 "off ($work/stock/gamescope does not list"
  expect_launch "$launcher" patched on 1 "on ($work/patched/gamescope lists"
  expect_launch "$launcher" patched off 0 "off (POLARIS_GAMESCOPE_COMPOSITE_CURSOR=0)" \
    POLARIS_GAMESCOPE_COMPOSITE_CURSOR=0
  expect_launch "$launcher" stock on 0 "on (forced by POLARIS_GAMESCOPE_COMPOSITE_CURSOR=1)" \
    POLARIS_GAMESCOPE_COMPOSITE_CURSOR=1
  # Forcing it onto a gamescope that lacks it reproduces #792 on purpose, and
  # gamescope's refusal must reach the launcher's own stderr, the journal. The
  # idle compositor shares its stderr with gamescope; the session launcher sends
  # gamescope's to a log in the runtime directory, so it names that log and
  # repeats its last lines when the launch fails.
  grep -qF "unrecognized option '--pipewire-composite-cursor'" "$launcher_log" ||
    fail "the forced $launcher launch on stock gamescope did not show gamescope's refusal: $(tail -5 "$launcher_log")"
  if [ "$launcher" = session ]; then
    grep -qF "gamescope's output is in $work/run/polaris-gamescope-steam-wsi." "$launcher_log" ||
      fail "the failed session launch did not name gamescope's log: $(tail -5 "$launcher_log")"
  fi
done

printf 'PASS: gamescope composite cursor probe\n'
