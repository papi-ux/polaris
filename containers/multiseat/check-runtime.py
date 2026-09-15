#!/usr/bin/env python3
"""Fail image construction if a fixed provider dependency is unsafe or missing."""
import json
import os
import pathlib
import stat
import subprocess
import sys

executables = ['dbus-daemon', 'pipewire', 'pw-cli', 'pw-dump', 'wireplumber', 'pactl', 'gst-launch-1.0',
               'gst-inspect-1.0', 'gamescope', 'Xwayland']
files = [pathlib.Path('/usr/bin') / name for name in executables]
package_lock = json.loads(pathlib.Path('/usr/share/polaris/build/packages.lock.json').read_text())
profile = package_lock['profile']
launchers = {'gamescope': None, 'steam': ('steam-installer', '/usr/games/steam'),
             'heroic': ('heroic', '/opt/Heroic/heroic'), 'lutris': ('lutris', '/usr/games/lutris')}
if profile not in launchers:
    raise ValueError('unknown runtime profile')
launcher = launchers[profile]
if launcher:
    package, executable = launcher
    locked = next(p for p in package_lock['runtime'] if p['name'] == package and p['architecture'] in ('amd64', 'all'))
    installed = subprocess.check_output(['dpkg-query', '-W', '-f=${Version}', package], text=True)
    if installed != locked['version']:
        raise ValueError('launcher package version differs from its lock')
    path = pathlib.Path(executable)
    if not os.access(path, os.X_OK):
        raise ValueError('launcher executable is missing')
    files.append(path)
    if profile == 'steam':
        files.append(pathlib.Path('/usr/bin/bash'))

files += [pathlib.Path('/usr/share/pipewire') / name for name in ['pipewire.conf', 'pipewire-pulse.conf']]
files += [pathlib.Path(path) for path in [
    '/usr/share/wireplumber/wireplumber.conf',
    '/usr/share/wireplumber/wireplumber.conf.d/99-polaris-seat.conf',
    '/usr/share/wireplumber/scripts/polaris-allocated-target.lua']]
plugin = pathlib.Path('/usr/lib/x86_64-linux-gnu/gstreamer-1.0/libgstwaylanddisplaysrc.so')
gl_plugin = pathlib.Path('/usr/lib/x86_64-linux-gnu/gstreamer-1.0/libgstopengl.so')
files += [plugin, gl_plugin]
encoded_game = pathlib.Path('/usr/libexec/polaris-seat/encoded-game-check')
encoded_audio = pathlib.Path('/usr/libexec/polaris-seat/encoded-audio-check')
game_status = pathlib.Path('/usr/libexec/polaris-seat/game-status')
capture_input = pathlib.Path('/usr/libexec/polaris-seat/capture-input')
workload = pathlib.Path('/usr/libexec/polaris-seat/workloads/input-pong-v1')
if sys.argv[1:] in (['--worker'], ['--nvidia']):
    files += [workload, capture_input, game_status, encoded_game, encoded_audio] + [pathlib.Path('/usr/libexec/polaris-seat') / name for name in
                          ['session-bus', 'audio', 'display-capture', 'nested-compositor', 'virtual-input', 'launcher', 'encoder', 'encode-media']]
elif sys.argv[1:]:
    raise ValueError('unknown dependency check scope')
steam_libraries = []
if profile == 'steam' and sys.argv[1:] in (['--worker'], ['--nvidia']):
    for triplet, elf_class, machine in [('x86_64-linux-gnu', 2, 62), ('i386-linux-gnu', 1, 3)]:
        library = pathlib.Path('/usr/lib') / triplet / 'libpolaris-steam-input.so'
        header = library.read_bytes()[:20]
        if (len(header) != 20 or header[:6] != bytes([127, 69, 76, 70, elf_class, 1]) or
                int.from_bytes(header[18:20], 'little') != machine or not os.access(library, os.X_OK)):
            raise ValueError('Steam input library ABI or permissions are invalid')
        steam_libraries.append(library)
    files += steam_libraries
hardware_libraries = []
if '--nvidia' in sys.argv:
    hardware_libraries = [pathlib.Path('/usr/lib/x86_64-linux-gnu') / name for name in
                          ['gstreamer-1.0/libgstnvcodec.so', 'libgstcuda-1.0.so.0.2600.0']]
    files += hardware_libraries
for path in files:
    # A root build can read through an unsearchable COPY-created directory.
    # Seat workers have neither root identity nor DAC override capabilities.
    for parent in path.parents:
        directory = parent.lstat()
        if not stat.S_ISDIR(directory.st_mode) or directory.st_uid != 0 or directory.st_mode & 0o022 or not directory.st_mode & 0o001:
            raise ValueError('untrusted or unsearchable provider directory: ' + str(parent))
    status = path.lstat()
    if not stat.S_ISREG(status.st_mode) or status.st_uid != 0 or status.st_mode & 0o022:
        raise ValueError('untrusted provider dependency: ' + str(path))
    if (path.parent == pathlib.Path('/usr/bin') or path.is_relative_to('/usr/libexec/polaris-seat')) and not os.access(path, os.X_OK):
        raise ValueError('non-executable provider dependency: ' + str(path))
for path in [pathlib.Path('/usr/bin/wireplumber'), pathlib.Path('/usr/bin/pw-dump'), pathlib.Path('/usr/bin/gamescope'), pathlib.Path('/usr/bin/Xwayland'), plugin, gl_plugin] + ([workload, capture_input, game_status, encoded_game, encoded_audio, pathlib.Path('/usr/libexec/polaris-seat/encode-media')] if any(arg in sys.argv for arg in ('--worker', '--nvidia')) else []) + hardware_libraries + steam_libraries:
    linked = subprocess.check_output(['ldd', '-r', str(path)], text=True, stderr=subprocess.STDOUT)
    if 'not found' in linked or 'undefined symbol:' in linked:
        raise ValueError('unresolved ELF dependency: ' + str(path) + '\n' + linked)
for element in ['waylanddisplaysrc', 'unixfdsink', 'unixfdsrc', 'fakesink', 'videoconvert',
                'audiotestsrc', 'audioconvert', 'audioresample', 'pulsesink', 'pulsesrc',
                'openh264enc', 'openh264dec', 'h264parse', 'opusenc', 'opusdec', 'appsink',
                'glupload', 'glcolorconvert', 'gldownload']:
    subprocess.run(['/usr/bin/gst-inspect-1.0', element], check=True, stdout=subprocess.DEVNULL)
if hardware_libraries:
    from nvidia_runtime import verify
    report = verify(pathlib.Path('/'), profile)
    pathlib.Path('/usr/share/polaris/build/nvidia-runtime.json').write_text(json.dumps(report, indent=2) + '\n')
    # Registration can legitimately expose no encoders on a build machine with
    # no GPU devices. Physical codec acceptance must require actual frames.
    subprocess.run(['/usr/bin/gst-inspect-1.0', 'nvcodec'], check=True, stdout=subprocess.DEVNULL)
help_text = subprocess.check_output(['/usr/bin/gamescope', '--help'], stderr=subprocess.STDOUT, text=True)
if '--keep-alive' not in help_text or '--expose-wayland' not in help_text:
    raise ValueError('Gamescope lacks the provider lifecycle interface')
print('All fixed provider dependencies, launcher package files and ELF links passed')
