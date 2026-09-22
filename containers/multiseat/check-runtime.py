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
    if profile == 'heroic':
        # Heroic never talks to a store itself: it runs that store's own
        # downloader, legendary for Epic, gogdl for GOG and nile for Amazon,
        # and the target grammar admits all three. A missing one is a library
        # that reads empty and a launch that never starts, so they are found
        # rather than assumed at a path a new Heroic release may move.
        found = {}
        for candidate in pathlib.Path('/opt/Heroic').rglob('*'):
            if candidate.name in ('legendary', 'gogdl', 'nile') and candidate.is_file() \
                    and os.access(candidate, os.X_OK):
                found.setdefault(candidate.name, candidate)
        missing = [name for name in ('legendary', 'gogdl', 'nile') if name not in found]
        if missing:
            raise ValueError('Heroic store backends are missing: ' + ', '.join(missing) +
                             '. Either the package stopped bundling them or the target grammar '
                             'admits a runner this image cannot serve.')
        files += [found[name] for name in sorted(found)]

    if profile in ('heroic', 'lutris'):
        # These launchers draw with GTK, whose image loader sandboxes itself in
        # a way a Space refuses. /usr/bin/bwrap answers that one request in
        # words the loader takes as "no sandbox here" and passes the rest on.
        # Both halves are run, because a wrapper that lost either one is a
        # launcher that aborts before its first window or a Proton that cannot
        # build its container.
        wrapper, packaged = pathlib.Path('/usr/bin/bwrap'), pathlib.Path('/usr/bin/bwrap.real')
        if not packaged.is_file() or packaged.is_symlink() or not wrapper.is_file() or \
                wrapper.read_bytes()[:10] != b'#!/bin/sh\n':
            raise ValueError('bwrap is not wrapped for a Space')
        refused = subprocess.run([str(wrapper), '--unshare-all', '--die-with-parent', '--ro-bind', '/', '/', '/usr/bin/true'],
                                 capture_output=True, text=True)
        if refused.returncode != 1:
            raise ValueError('the bwrap wrapper no longer refuses the sandbox a Space cannot give')
        # The refusal only works while the glycin in this image reads it as "no
        # sandbox here". That is decided by sentences compiled into glycin, so
        # the one the wrapper says is looked for in the library itself: a lock
        # refresh that brings a glycin with other words stops the build here,
        # instead of shipping a launcher that aborts before its first window.
        libraries = sorted(pathlib.Path('/usr/lib/x86_64-linux-gnu').glob('libglycin-*.so*'))
        if not libraries:
            raise ValueError('no glycin library in this image: if GTK no longer loads images through it, '
                             'the bwrap wrapper has nothing left to do and should go')
        known = [sentence for sentence in (b'Creating new namespace failed', b'No permissions to create a new namespace',
                                           b'No permissions to creating new namespace', b'No permissions to create new namespace',
                                           b'bwrap: setting up uid map: Permission denied')
                 if any(sentence in library.read_bytes() for library in libraries if library.is_file())]
        if not any(sentence.decode() in refused.stderr for sentence in known):
            raise ValueError('the glycin in this image does not read the bwrap wrapper\'s refusal as an unavailable sandbox')
        # A value or a program argument that spells the request is not the request.
        for argv in (['--version'], ['--setenv', 'POLARIS_CHECK', '--unshare-net', '--version']):
            passed = subprocess.run([str(wrapper)] + argv, capture_output=True, text=True)
            if passed.returncode != 0 or not passed.stdout.startswith('bubblewrap '):
                raise ValueError('the bwrap wrapper no longer reaches the packaged bwrap')
        files += [wrapper, packaged]
    if profile == 'lutris':
        # The worker starts Lutris through its interpreter, as a trusted file it
        # follows no link to reach, and /usr/bin/python3 is a link. It names
        # the file this base carries; when the base moves to another Python the
        # image stops building here instead of a Space failing to start.
        interpreter = pathlib.Path('/usr/bin/python3.14')
        if pathlib.Path('/usr/bin/python3').resolve() != interpreter:
            raise ValueError('python3 is not ' + str(interpreter) + ': update lutrisInterpreter in '
                             'multiseat_worker/internal/seatprovider/launcher_linux.go and this check together')
        files.append(interpreter)
        # A Space's library is read out of Lutris's own database, from a copy
        # held in memory so nothing is opened for writing on a read-only mount.
        import sqlite3
        if not hasattr(sqlite3.Connection, 'deserialize'):
            raise ValueError('this Python cannot read a database held in memory, which the Lutris library reader needs')

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
host_driver = sys.argv[1:] == ['--nvidia-host']
if sys.argv[1:] in (['--worker'], ['--nvidia'], ['--nvidia-host']):
    files += [workload, capture_input, game_status, encoded_game, encoded_audio] + [pathlib.Path('/usr/libexec/polaris-seat') / name for name in
                          ['session-bus', 'audio', 'display-capture', 'nested-compositor', 'virtual-input', 'launcher', 'encoder', 'encode-media']]
elif sys.argv[1:]:
    raise ValueError('unknown dependency check scope')
steam_libraries = []
if profile == 'steam' and sys.argv[1:] in (['--worker'], ['--nvidia'], ['--nvidia-host']):
    for triplet, elf_class, machine in [('x86_64-linux-gnu', 2, 62), ('i386-linux-gnu', 1, 3)]:
        library = pathlib.Path('/usr/lib') / triplet / 'libpolaris-steam-input.so'
        header = library.read_bytes()[:20]
        if (len(header) != 20 or header[:6] != bytes([127, 69, 76, 70, elf_class, 1]) or
                int.from_bytes(header[18:20], 'little') != machine or not os.access(library, os.X_OK)):
            raise ValueError('Steam input library ABI or permissions are invalid')
        steam_libraries.append(library)
    files += steam_libraries
hardware_libraries = []
if '--nvidia' in sys.argv or host_driver:
    hardware_libraries = [pathlib.Path('/usr/lib/x86_64-linux-gnu') / name for name in
                          ['gstreamer-1.0/libgstnvcodec.so', 'libgstcuda-1.0.so.0.2600.0']]
    files += hardware_libraries
if host_driver:
    # An image that borrows the machine's driver has to carry the contract it
    # was built against and the probe that proves the borrowed files load.
    files += [pathlib.Path('/usr/share/polaris/build/nvidia-host-contract.json')]
    # The 32-bit probe is built wherever a 32-bit game can run, which is every launcher family
    # but gamescope. Asking every image for it failed the gamescope build of this variant.
    probes = ['graphics-check'] if profile == 'gamescope' else ['graphics-check', 'graphics-check-32']
    for probe in probes:
        files.append(pathlib.Path('/usr/libexec/polaris-seat') / probe)
    if pathlib.Path('/etc/ld.so.cache').resolve() != pathlib.Path('/etc/polaris-ld/ld.so.cache'):
        raise ValueError('the loader cache must be rebuilt over the borrowed driver at start')
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
for path in [pathlib.Path('/usr/bin/wireplumber'), pathlib.Path('/usr/bin/pw-dump'), pathlib.Path('/usr/bin/gamescope'), pathlib.Path('/usr/bin/Xwayland'), plugin, gl_plugin] + ([workload, capture_input, game_status, encoded_game, encoded_audio, pathlib.Path('/usr/libexec/polaris-seat/encode-media')] if any(arg in sys.argv for arg in ('--worker', '--nvidia', '--nvidia-host')) else []) + hardware_libraries + steam_libraries:
    linked = subprocess.check_output(['ldd', '-r', str(path)], text=True, stderr=subprocess.STDOUT)
    unresolved = [line for line in linked.splitlines() if 'not found' in line or 'undefined symbol:' in line]
    if host_driver:
        # The driver itself arrives at run time, so its sonames are expected to
        # be missing here. Anything else is still a broken image.
        borrowed = tuple(json.loads(pathlib.Path('/usr/share/polaris/build/nvidia-host-contract.json').read_text())['library_prefixes'])
        unresolved = [line for line in unresolved if not line.strip().startswith(borrowed)]
    if unresolved:
        raise ValueError('unresolved ELF dependency: ' + str(path) + '\n' + '\n'.join(unresolved))
for element in ['waylanddisplaysrc', 'unixfdsink', 'unixfdsrc', 'fakesink', 'videoconvert',
                'audiotestsrc', 'audioconvert', 'audioresample', 'pulsesink', 'pulsesrc',
                'openh264enc', 'openh264dec', 'h264parse', 'opusenc', 'opusdec', 'appsink',
                'glupload', 'glcolorconvert', 'gldownload']:
    subprocess.run(['/usr/bin/gst-inspect-1.0', element], check=True, stdout=subprocess.DEVNULL)
if hardware_libraries:
    from nvidia_runtime import verify
    report = verify(pathlib.Path('/'), profile, source='host' if host_driver else 'image')
    pathlib.Path('/usr/share/polaris/build/nvidia-runtime.json').write_text(json.dumps(report, indent=2) + '\n')
    # Registration can legitimately expose no encoders on a build machine with
    # no GPU devices. Physical codec acceptance must require actual frames.
    subprocess.run(['/usr/bin/gst-inspect-1.0', 'nvcodec'], check=True, stdout=subprocess.DEVNULL)
help_text = subprocess.check_output(['/usr/bin/gamescope', '--help'], stderr=subprocess.STDOUT, text=True)
if '--keep-alive' not in help_text or '--expose-wayland' not in help_text:
    raise ValueError('Gamescope lacks the provider lifecycle interface')
print('All fixed provider dependencies, launcher package files and ELF links passed')
