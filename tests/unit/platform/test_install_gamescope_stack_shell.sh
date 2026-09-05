#!/usr/bin/env bash
# The scripts/install stack had no coverage at all; a classifier change broke
# every such host for eight days before anyone noticed. This runs the installer
# into a scratch prefix with the systemd reload skipped and checks what it wrote.
set -euo pipefail

fail() {
  printf 'FAIL: %s\n' "$*" >&2
  exit 1
}

source_dir="${POLARIS_SOURCE_DIR:?}"
work="$(mktemp -d "${TMPDIR:-/tmp}/polaris-install-stack.XXXXXX")"
trap 'rm -rf "$work"' EXIT
mkdir -p "$work/bin" "$work/home"
printf '#!/usr/bin/env bash\nexit 0\n' >"$work/bin/polaris"
chmod +x "$work/bin/polaris"
# A systemctl on PATH would prove the skip flag is not honoured.
printf '#!/usr/bin/env bash\necho "systemctl must not run under POLARIS_INSTALL_SKIP_SYSTEMD" >&2\nexit 97\n' >"$work/bin/systemctl"
chmod +x "$work/bin/systemctl"

HOME="$work/home" \
PATH="$work/bin:$PATH" \
PREFIX="$work/home/.local" \
SYSTEMD_USER_DIR="$work/home/.config/systemd/user" \
CONFIG_DIR="$work/home/.config/polaris" \
POLARIS_INSTALL_SKIP_SYSTEMD=1 \
  bash "$source_dir/scripts/install/03-install-gamescope-stack.sh" --polaris-bin "$work/bin/polaris" >"$work/install.log" 2>&1 \
  || fail "installer exited non-zero: $(tail -5 "$work/install.log")"

bin="$work/home/.local/bin"
for helper in polaris-gamescope-session polaris-gamescope-idle polaris-wait-gamescope polaris-start polaris-gamescope-runtime-lib.sh; do
  [ -f "$bin/$helper" ] || fail "installer did not write $helper"
  bash -n "$bin/$helper" || fail "$helper does not parse"
done
[ -x "$bin/polaris-gamescope-session" ] || fail "session launcher is not executable"
# The launcher is the shared module with the non-NixOS header on top: it must
# still source its runtime library from its own directory.
grep -q 'polaris-gamescope-runtime-lib.sh' "$bin/polaris-gamescope-session" || fail "launcher lost its runtime library reference"
grep -q 'export POLARIS_GAMESCOPE_BIN=' "$bin/polaris-gamescope-session" || fail "launcher lost the non-NixOS header"
grep -q "printf 'host-portal" "$bin/polaris-gamescope-session" || fail "launcher module is not the current shared body"
module="$source_dir/nix/modules/polaris-gamescope-session.sh"
tail -c "$(stat -c %s "$module")" "$bin/polaris-gamescope-session" | cmp -s - "$module" \
  || fail "installed launcher does not end with nix/modules/polaris-gamescope-session.sh verbatim"

units="$work/home/.config/systemd/user"
[ -f "$units/polaris-gamescope-idle.service" ] || fail "idle unit missing"
[ -f "$units/polaris.service" ] || fail "polaris unit missing"
grep -q "^ExecStart=$bin/polaris-gamescope-idle$" "$units/polaris-gamescope-idle.service" || fail "idle unit does not start the installed helper"
grep -q "^ExecStartPre=$bin/polaris-wait-gamescope$" "$units/polaris.service" || fail "polaris unit does not wait for gamescope"
grep -q "^ExecStart=$bin/polaris-start$" "$units/polaris.service" || fail "polaris unit does not start through polaris-start"
grep -q "^Environment=PATH=$bin:" "$units/polaris.service" || fail "polaris unit PATH does not lead with the install prefix"
[ -f "$work/home/.config/polaris/polaris.conf.gamescope-stream.example" ] || fail "config seed missing"
grep -q '^linux_stream_mode = gamescope_stream$' "$work/home/.config/polaris/polaris.conf.gamescope-stream.example" || fail "config seed lost the stream mode"
grep -q 'systemd --user daemon-reload skipped' "$work/install.log" || fail "installer did not report the skipped reload"
grep -q "$work/bin/polaris" "$bin/polaris-start" || fail "polaris-start does not exec the given binary"

echo "PASS: scripts/install gamescope stack installer"
