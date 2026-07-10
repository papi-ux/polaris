#!/usr/bin/env bash
# Smoke Nova -> Polaris Mirror Desktop on an Android/Moonlight client.
#
# This is intentionally a developer/lab smoke runner: it drives an attached
# Android device via adb, launches Nova's MOUSE Mirror Desktop flow, handles the
# KDE XDG Desktop Portal screen-share prompt through AT-SPI when it appears, and
# saves proof artifacts under ~/.local/share/polaris-debug-artifacts/.
#
# Safety: this script force-stops only the Nova client package. It does not kill
# desktop Steam and does not modify Polaris file capabilities.

set -euo pipefail

PKG="${PKG:-com.papi.nova.debug}"
ART_ROOT="${ART_ROOT:-$HOME/.local/share/polaris-debug-artifacts}"
ART="$ART_ROOT/mirror-desktop-auto-$(date +%Y%m%d-%H%M%S)"
GAME_CARD_TAP_X="${GAME_CARD_TAP_X:-320}"
GAME_CARD_TAP_Y="${GAME_CARD_TAP_Y:-285}"
PRIMARY_TAP_X="${PRIMARY_TAP_X:-960}"
PRIMARY_TAP_Y="${PRIMARY_TAP_Y:-940}"
PIPEWIRE_WAIT_SECONDS="${PIPEWIRE_WAIT_SECONDS:-45}"
VISUAL_WAIT_SECONDS="${VISUAL_WAIT_SECONDS:-45}"
RESTART_PORTAL_A11Y="${RESTART_PORTAL_A11Y:-1}"
RESTART_POLARIS="${RESTART_POLARIS:-1}"
LEAVE_RUNNING="${LEAVE_RUNNING:-1}"
PORTAL_SCREEN_MATCH="${PORTAL_SCREEN_MATCH:-}"
COPY_ARTIFACTS_HOST="${COPY_ARTIFACTS_HOST:-}"

mkdir -p "$ART"
echo "$ART" | tee /tmp/polaris-last-mirror-smoke-artifact.txt >/dev/null

log() { printf '[%s] %s\n' "$(date +%H:%M:%S)" "$*" | tee -a "$ART/summary.txt"; }
copy_for_review() {
  if [[ -n "$COPY_ARTIFACTS_HOST" ]]; then
    scp -q "$@" "$COPY_ARTIFACTS_HOST:/tmp/" 2>/dev/null || true
  fi
}

resolve_serial() {
  if [[ -n "${SERIAL:-}" ]]; then
    printf '%s\n' "$SERIAL"
    return
  fi
  mapfile -t devices < <(adb devices | awk 'NR>1 && $2=="device" {print $1}')
  if (( ${#devices[@]} != 1 )); then
    printf 'Set SERIAL=<adb-serial>; found %s attached devices\n' "${#devices[@]}" >&2
    adb devices >&2 || true
    exit 2
  fi
  printf '%s\n' "${devices[0]}"
}

SERIAL="$(resolve_serial)"

log "artifact=$ART"
log "serial=$SERIAL package=$PKG"
log "polaris_target=$(readlink -f /usr/bin/polaris 2>/dev/null || true)"
getcap -v "$(readlink -f /usr/bin/polaris)" > "$ART/polaris-capability.txt" 2>&1 || true
cat "$ART/polaris-capability.txt" >> "$ART/summary.txt"

if [[ "$RESTART_PORTAL_A11Y" == "1" ]]; then
  log "enabling Qt accessibility for KDE portal backend"
  systemctl --user set-environment QT_ACCESSIBILITY=1 QT_LINUX_ACCESSIBILITY_ALWAYS_ON=1 || true
  systemctl --user restart plasma-xdg-desktop-portal-kde.service || true
  sleep 2
fi

log "force-stopping Nova client to end prior stream"
adb -s "$SERIAL" shell am force-stop "$PKG" || true
portal_out=$(kdotool search --title 'Share screen with' getwindowname %@ 2>/dev/null || true)
if grep -q 'Share screen with' <<<"$portal_out"; then
  kdotool search --title 'Share screen with' windowclose %@ >/dev/null 2>&1 || true
fi
sleep 2

if [[ "$RESTART_POLARIS" == "1" ]]; then
  log "restarting Polaris user service"
  systemctl --user restart polaris
  sleep 4
fi

systemctl --user is-active polaris > "$ART/polaris-active.txt" || true
log "polaris_active=$(cat "$ART/polaris-active.txt" 2>/dev/null || true)"

log "starting Nova PcView/library"
adb -s "$SERIAL" shell am start -n "$PKG/com.papi.nova.PcView" >/dev/null 2>&1 || \
  adb -s "$SERIAL" shell monkey -p "$PKG" 1 >/dev/null 2>&1 || true
sleep 3
state=$(adb -s "$SERIAL" shell dumpsys activity activities | tr -d '\r' | grep -E 'topResumedActivity|ResumedActivity' | head -n 2 || true)
printf '%s\n' "$state" > "$ART/activity-after-start.txt"
if grep -q '/com.papi.nova.Game' "$ART/activity-after-start.txt"; then
  log "Nova restored to Game; navigating back/end-session to library"
  adb -s "$SERIAL" shell input keyevent KEYCODE_BACK || true
  sleep 1
  adb -s "$SERIAL" shell input tap 900 152 || true
  sleep 1
  adb -s "$SERIAL" shell input tap 960 910 || true
  sleep 3
  adb -s "$SERIAL" shell am start -n "$PKG/com.papi.nova.PcView" >/dev/null 2>&1 || true
  sleep 2
fi
adb -s "$SERIAL" exec-out screencap -p > "$ART/01-library-before-launch.png" || true
copy_for_review "$ART/01-library-before-launch.png"

adb -s "$SERIAL" logcat -c || true
STAMP=$(date --iso-8601=seconds)
printf '%s\n' "$STAMP" > "$ART/stamp.txt"
log "launch_stamp=$STAMP"

log "opening game detail / launch panel at ${GAME_CARD_TAP_X},${GAME_CARD_TAP_Y}"
adb -s "$SERIAL" shell input tap "$GAME_CARD_TAP_X" "$GAME_CARD_TAP_Y"
sleep 2
adb -s "$SERIAL" exec-out screencap -p > "$ART/02-detail-before-primary.png" || true
copy_for_review "$ART/02-detail-before-primary.png"

log "tapping primary launch at ${PRIMARY_TAP_X},${PRIMARY_TAP_Y}"
adb -s "$SERIAL" shell input tap "$PRIMARY_TAP_X" "$PRIMARY_TAP_Y"
sleep 7

press_portal_with_atspi() {
  PORTAL_SCREEN_MATCH="$PORTAL_SCREEN_MATCH" python3 - <<'ATSPIPY'
import os
import sys
import time
import pyatspi

screen_match = os.environ.get('PORTAL_SCREEN_MATCH', '').lower()

def actions(node):
    try:
        iface = node.queryAction()
        return [iface.getName(i) for i in range(iface.nActions)], iface
    except Exception:
        return [], None

def walk(node):
    yield node
    try:
        count = node.childCount
    except Exception:
        return
    for i in range(min(count, 160)):
        try:
            yield from walk(node.getChildAtIndex(i))
        except Exception:
            pass

def acceptable_button(name):
    lowered = name.lower()
    if not name.strip():
        return False
    if any(bad in lowered for bad in ('virtual', 'region', 'remove', 'clear search')):
        return False
    if screen_match:
        return screen_match in lowered
    return True

desktop = pyatspi.Registry.getDesktop(0)
buttons = []
for i in range(desktop.childCount):
    app = desktop.getChildAtIndex(i)
    try:
        app_name = app.name or ''
    except Exception:
        continue
    if 'xdg-desktop-portal-kde' not in app_name:
        continue
    for node in walk(app):
        try:
            role = node.getRoleName()
            name = node.name or ''
        except Exception:
            continue
        acts, action_iface = actions(node)
        if role == 'button' and 'Press' in acts and acceptable_button(name):
            buttons.append((node, action_iface, acts, name))

if not buttons:
    print('AT_SPI_NO_PORTAL_BUTTON')
    sys.exit(2)

node, action_iface, acts, name = buttons[0]
idx = acts.index('Press')
try:
    node.queryComponent().grabFocus()
except Exception:
    pass
time.sleep(0.2)
ok = action_iface.doAction(idx)
print('AT_SPI_PRESSED', repr(name), ok)
ATSPIPY
}

log "checking portal dialog"
kdotool search --title 'Share screen with' getwindowgeometry %1 getwindowname %1 > "$ART/portal-window-before-a11y.txt" 2>&1 || true
if grep -q 'Share screen with' "$ART/portal-window-before-a11y.txt"; then
  log "portal dialog present; accepting screen through AT-SPI"
  if press_portal_with_atspi > "$ART/atspi-consent.txt" 2>&1; then
    cat "$ART/atspi-consent.txt" | tee -a "$ART/summary.txt"
  else
    cat "$ART/atspi-consent.txt" | tee -a "$ART/summary.txt"
    log "AT-SPI consent failed"
  fi
else
  printf 'no portal window\n' > "$ART/portal-window-before-a11y.txt"
  log "no portal dialog; relying on saved restore token"
fi

log "waiting for PipeWire video capture / first frames"
end=$((SECONDS + PIPEWIRE_WAIT_SECONDS))
pipewire_ok=0
while (( SECONDS < end )); do
  journalctl --user -u polaris --since "$STAMP" --no-pager -o short-iso > "$ART/polaris-journal-live.txt" || true
  if grep -q 'portal: PipeWire state: paused -> streaming' "$ART/polaris-journal-live.txt" && grep -q 'portal: PipeWire format negotiated' "$ART/polaris-journal-live.txt"; then
    pipewire_ok=1
    break
  fi
  sleep 2
done
log "pipewire_video_ok=$pipewire_ok"

log "waiting for visible game/desktop frame"
visual_ok=0
visual_end=$((SECONDS + VISUAL_WAIT_SECONDS))
attempt=0
while (( SECONDS < visual_end )); do
  attempt=$((attempt + 1))
  adb -s "$SERIAL" exec-out screencap -p > "$ART/03-after-launch-final.png" || true
  python3 - <<IMGCHK > "$ART/image-check.txt"
from pathlib import Path
from PIL import Image, ImageStat
p = Path('$ART/03-after-launch-final.png')
if not p.exists():
    print('image_missing=1')
    raise SystemExit(3)
im = Image.open(p).convert('RGB')
w, h = im.size
crop = im.crop((w // 4, h // 4, 3 * w // 4, 3 * h // 4)).resize((320, 180))
pix = list(crop.getdata())
nonblack = sum(1 for r, g, b in pix if max(r, g, b) > 24)
mean = ImageStat.Stat(crop).mean
print(f'image_size={w}x{h}')
print(f'central_nonblack_320x180={nonblack}')
print('central_mean=' + ','.join(f'{v:.2f}' for v in mean))
print('visual_content_ok=' + ('1' if nonblack > 2500 else '0'))
IMGCHK
  visual_ok=$(awk -F= '/visual_content_ok/ {print $2}' "$ART/image-check.txt" | tail -n1)
  central_nonblack=$(awk -F= '/central_nonblack/ {print $2}' "$ART/image-check.txt" | tail -n1)
  log "visual_attempt=$attempt visual_content_ok=${visual_ok:-0} central_nonblack=${central_nonblack:-unknown}"
  [[ "${visual_ok:-0}" == "1" ]] && break
  sleep 3
done
copy_for_review "$ART/03-after-launch-final.png"

adb -s "$SERIAL" logcat -d -v time | tr -d '\r' > "$ART/logcat-final.txt" || true
journalctl --user -u polaris --since "$STAMP" --no-pager -o short-iso > "$ART/polaris-journal-final.txt" || true
kdotool search --title 'Share screen with' getwindowgeometry %1 getwindowname %1 > "$ART/portal-window-final.txt" 2>&1 || true
if grep -q 'Share screen with' "$ART/portal-window-final.txt"; then
  portal_final=1
else
  portal_final=0
  printf 'no portal window\n' > "$ART/portal-window-final.txt"
fi
log "portal_window_final=$portal_final"
cat "$ART/image-check.txt" | tee -a "$ART/summary.txt"
visual_ok=$(awk -F= '/visual_content_ok/ {print $2}' "$ART/image-check.txt" | tail -n1)

grep -Ei 'mirrorDesktop=1.*launchMode=mirror_desktop|launchMode=mirror_desktop.*mirrorDesktop=1' "$ART/logcat-final.txt" > "$ART/nova-launch-contract.txt" || true
grep -Ei 'status_code="200"|Launched new game session|Received first video packet|Nova SSE: stream_active' "$ART/logcat-final.txt" > "$ART/nova-stream-contract.txt" || true
grep -E 'portal: Selecting sources \(type=1\)|portal: PipeWire node ID|portal: PipeWire format negotiated|portal: PipeWire state: paused -> streaming' "$ART/polaris-journal-final.txt" > "$ART/polaris-portal-contract.txt" || true

if grep -q 'mirrorDesktop=0' "$ART/logcat-final.txt" 2>/dev/null; then
  log "mirrorDesktop_zero_seen=1"
else
  log "mirrorDesktop_zero_seen=0"
fi
nova_launch_ok=0; [[ -s "$ART/nova-launch-contract.txt" ]] && ! grep -q 'mirrorDesktop=0' "$ART/logcat-final.txt" 2>/dev/null && nova_launch_ok=1
nova_stream_ok=0; grep -q 'Received first video packet' "$ART/nova-stream-contract.txt" 2>/dev/null && nova_stream_ok=1
portal_type_ok=0; grep -q 'portal: Selecting sources (type=1)' "$ART/polaris-portal-contract.txt" 2>/dev/null && portal_type_ok=1
portal_stream_ok=0; grep -q 'portal: PipeWire state: paused -> streaming' "$ART/polaris-portal-contract.txt" 2>/dev/null && portal_stream_ok=1
log "nova_launch_ok=$nova_launch_ok nova_stream_ok=$nova_stream_ok portal_type_ok=$portal_type_ok portal_stream_ok=$portal_stream_ok visual_content_ok=${visual_ok:-0}"

if [[ "$LEAVE_RUNNING" != "1" ]]; then
  log "LEAVE_RUNNING!=1; force-stopping Nova client"
  adb -s "$SERIAL" shell am force-stop "$PKG" || true
fi

if [[ "$nova_launch_ok" == 1 && "$nova_stream_ok" == 1 && "$portal_type_ok" == 1 && "$portal_stream_ok" == 1 && "${visual_ok:-0}" == 1 && "$portal_final" == 0 ]]; then
  log "RESULT=PASS"
  exit 0
fi
log "RESULT=FAIL"
exit 1
