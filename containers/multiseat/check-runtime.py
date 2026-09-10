#!/usr/bin/env python3
"""Fail image construction if a fixed provider dependency is unsafe or missing."""
import os
import pathlib
import stat
import subprocess
import sys

executables = ['dbus-daemon', 'pipewire', 'pw-cli', 'pw-dump', 'wireplumber', 'pactl', 'gst-launch-1.0',
               'gst-inspect-1.0', 'gamescope', 'Xwayland']
files = [pathlib.Path('/usr/bin') / name for name in executables]
files += [pathlib.Path('/usr/share/pipewire') / name for name in ['pipewire.conf', 'pipewire-pulse.conf']]
files += [pathlib.Path(path) for path in [
    '/usr/share/wireplumber/wireplumber.conf',
    '/usr/share/wireplumber/wireplumber.conf.d/99-polaris-seat.conf',
    '/usr/share/polaris/wireplumber/allocated-target.lua']]
plugin = pathlib.Path('/usr/lib/x86_64-linux-gnu/gstreamer-1.0/libgstwaylanddisplaysrc.so')
gl_plugin = pathlib.Path('/usr/lib/x86_64-linux-gnu/gstreamer-1.0/libgstopengl.so')
files += [plugin, gl_plugin]
encoded_game = pathlib.Path('/usr/libexec/polaris-seat/encoded-game-check')
encoded_audio = pathlib.Path('/usr/libexec/polaris-seat/encoded-audio-check')
game_status = pathlib.Path('/usr/libexec/polaris-seat/game-status')
capture_input = pathlib.Path('/usr/libexec/polaris-seat/capture-input')
workload = pathlib.Path('/usr/libexec/polaris-seat/workloads/input-pong-v1')
if sys.argv[1:] == ['--worker']:
    files += [workload, capture_input, game_status, encoded_game, encoded_audio] + [pathlib.Path('/usr/libexec/polaris-seat') / name for name in
                          ['session-bus', 'audio', 'display-capture', 'nested-compositor', 'virtual-input', 'launcher']]
elif sys.argv[1:]:
    raise ValueError('unknown dependency check scope')
for path in files:
    status = path.lstat()
    if not stat.S_ISREG(status.st_mode) or status.st_uid != 0 or status.st_mode & 0o022:
        raise ValueError('untrusted provider dependency: ' + str(path))
    if (path.parent == pathlib.Path('/usr/bin') or path.is_relative_to('/usr/libexec/polaris-seat')) and not os.access(path, os.X_OK):
        raise ValueError('non-executable provider dependency: ' + str(path))
for path in [pathlib.Path('/usr/bin/wireplumber'), pathlib.Path('/usr/bin/pw-dump'), pathlib.Path('/usr/bin/gamescope'), pathlib.Path('/usr/bin/Xwayland'), plugin, gl_plugin] + ([workload, capture_input, game_status, encoded_game, encoded_audio] if '--worker' in sys.argv else []):
    linked = subprocess.check_output(['ldd', str(path)], text=True, stderr=subprocess.STDOUT)
    if 'not found' in linked:
        raise ValueError('unresolved ELF dependency: ' + str(path))
for element in ['waylanddisplaysrc', 'unixfdsink', 'unixfdsrc', 'fakesink', 'videoconvert',
                'audiotestsrc', 'audioconvert', 'audioresample', 'pulsesink', 'pulsesrc',
                'openh264enc', 'openh264dec', 'h264parse', 'opusenc', 'opusdec', 'appsink',
                'glupload', 'glcolorconvert', 'gldownload']:
    subprocess.run(['/usr/bin/gst-inspect-1.0', element], check=True, stdout=subprocess.DEVNULL)
help_text = subprocess.check_output(['/usr/bin/gamescope', '--help'], stderr=subprocess.STDOUT, text=True)
if '--keep-alive' not in help_text or '--expose-wayland' not in help_text:
    raise ValueError('Gamescope lacks the provider lifecycle interface')
print('All fixed provider dependencies and ELF links passed')
