"""Verify the packaged NVIDIA userspace ABI without opening GPU devices."""
import hashlib
import json
import pathlib
import re
import stat
import struct
import subprocess

ARCHITECTURES = {'amd64': ('x86_64-linux-gnu', 2, 62), 'i386': ('i386-linux-gnu', 1, 3)}
FRONTENDS = ('libGL.so.1', 'libGLX.so.0', 'libEGL.so.1', 'libGLESv2.so.2', 'libvulkan.so.1')
REQUIRED_VENDOR = ('libcuda.so.1', 'libEGL_nvidia.so.0', 'libGLX_nvidia.so.0', 'libnvidia-encode.so.1')
CONFIGURATIONS = {
    'glvnd/egl_vendor.d/10_nvidia.json': 'libEGL_nvidia.so.0',
    'vulkan/icd.d/nvidia_icd.json': 'libGLX_nvidia.so.0',
    'vulkan/implicit_layer.d/nvidia_layers.json': None,
    **{'egl/egl_external_platform.d/' + filename: soname for filename, soname in [
        ('09_nvidia_wayland2.json', 'libnvidia-egl-wayland2.so.1'),
        ('10_nvidia_wayland.json', 'libnvidia-egl-wayland.so.1'),
        ('15_nvidia_gbm.json', 'libnvidia-egl-gbm.so.1'),
        ('20_nvidia_xcb.json', 'libnvidia-egl-xcb.so.1'),
        ('20_nvidia_xlib.json', 'libnvidia-egl-xlib.so.1')]},
}


def architectures(profile):
    if profile not in ('gamescope', 'steam', 'heroic', 'lutris'):
        raise ValueError('unknown NVIDIA runtime profile')
    return ['amd64'] if profile == 'gamescope' else ['amd64', 'i386']


def elf_identity(path, architecture):
    with pathlib.Path(path).open('rb') as source:
        header = source.read(20)
    _, elf_class, machine = ARCHITECTURES[architecture]
    if (len(header) != 20 or header[:4] != b'\x7fELF' or header[4:7] != bytes([elf_class, 1, 1]) or
            struct.unpack('<HH', header[16:20]) != (3, machine)):
        raise ValueError('NVIDIA library has the wrong ELF ABI: ' + str(path))


def soname_valid(name):
    return isinstance(name, str) and re.fullmatch(r'lib[A-Za-z0-9_+.-]+\.so(?:\.[0-9]+)*', name) is not None


def relative_path(value):
    path = pathlib.PurePosixPath(value)
    if not isinstance(value, str) or not path.is_absolute() or '..' in path.parts or str(path) != value:
        raise ValueError('invalid NVIDIA manifest path')
    return pathlib.Path(*path.parts[1:])


def trusted_parents(root, relative, owner_uid):
    path = root / relative
    for parent in [path.parent, *path.parent.parents]:
        if parent == root.parent:
            break
        status = parent.lstat()
        if (not stat.S_ISDIR(status.st_mode) or status.st_uid != owner_uid or
                status.st_mode & 0o022 or not status.st_mode & 0o001):
            raise ValueError('untrusted NVIDIA directory: ' + str(parent))


def trusted_file(root, relative, owner_uid):
    path = root / relative
    trusted_parents(root, relative, owner_uid)
    status = path.lstat()
    if (not stat.S_ISREG(status.st_mode) or status.st_uid != owner_uid or
            status.st_mode & 0o022 or not status.st_mode & 0o004):
        raise ValueError('untrusted NVIDIA file: ' + str(path))
    return path


def linked_library(path):
    result = subprocess.run(['/usr/bin/ldd', '-r', str(path)], text=True,
                            stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=30,
                            env={'PATH': '/usr/bin:/bin', 'LC_ALL': 'C'})
    # glibc ldd can report missing libraries and unresolved relocations with
    # exit status zero. Both the complete result and the status are required.
    if (result.returncode != 0 or not result.stdout.strip() or
            any(marker in result.stdout for marker in
                ('not found', 'undefined symbol:', 'not a dynamic executable', 'statically linked'))):
        raise ValueError('unresolved NVIDIA runtime dependency: ' + str(path) + '\n' + result.stdout)


def verify(root, profile, owner_uid=0, link_check=linked_library):
    root = pathlib.Path(root).resolve(strict=True)
    metadata = pathlib.Path('usr/share/polaris/build')
    manifest_path = trusted_file(root, metadata / 'nvidia-files.json', owner_uid)
    lock_path = trusted_file(root, metadata / 'nvidia.lock.json', owner_uid)
    manifest = json.loads(manifest_path.read_text())
    lock = json.loads(lock_path.read_text())
    expected_architectures = architectures(profile)
    if (set(manifest) != {'schema', 'driver_version', 'architectures', 'files', 'symlinks'} or
            manifest['schema'] != 1 or manifest['driver_version'] != lock['version'] or
            manifest['architectures'] != expected_architectures or
            not isinstance(manifest['files'], list) or not 1 <= len(manifest['files']) <= 256 or
            not isinstance(manifest['symlinks'], list) or len(manifest['symlinks']) > 256):
        raise ValueError('NVIDIA manifest identity is invalid')
    records, libraries, aliases, configurations = {}, {}, {}, set()
    for entry in manifest['files']:
        relative = relative_path(entry['path'])
        if entry['path'] in records:
            raise ValueError('duplicate NVIDIA manifest file')
        path = trusted_file(root, relative, owner_uid)
        with path.open('rb') as source:
            if hashlib.file_digest(source, 'sha256').hexdigest() != entry['sha256']:
                raise ValueError('NVIDIA file hash differs from its manifest')
        if 'architecture' in entry:
            architecture = entry['architecture']
            if (set(entry) != {'path', 'sha256', 'architecture', 'soname'} or
                    architecture not in expected_architectures or not soname_valid(entry['soname']) or
                    relative.parent != pathlib.Path('usr/lib') / ARCHITECTURES[architecture][0]):
                raise ValueError('invalid NVIDIA library identity')
            elf_identity(path, architecture)
            key = (architecture, entry['soname'])
            if key in libraries:
                raise ValueError('duplicate NVIDIA library SONAME')
            libraries[key] = path
        elif set(entry) != {'path', 'sha256'} or not relative.is_relative_to('usr/share'):
            raise ValueError('invalid NVIDIA non-library file')
        else:
            configurations.add(str(relative.relative_to('usr/share')))
        records[entry['path']] = entry
    if configurations != set(CONFIGURATIONS) | {'licenses/polaris-nvidia/LICENSE'}:
        raise ValueError('NVIDIA configuration or license files are incomplete')
    for name, soname in CONFIGURATIONS.items():
        config = json.loads((root / 'usr/share' / name).read_text())
        if soname and (config.get('ICD', {}).get('library_path') != soname or
                       any((architecture, soname) not in libraries for architecture in expected_architectures)):
            raise ValueError('NVIDIA graphics loader points to an unavailable vendor library')
    for entry in manifest['symlinks']:
        relative = relative_path(entry['path'])
        target = entry['target']
        path = root / relative
        trusted_parents(root, relative, owner_uid)
        status = path.lstat()
        if (set(entry) != {'path', 'target'} or entry['path'] in records or entry['path'] in aliases or
                not isinstance(target, str) or pathlib.PurePosixPath(target).is_absolute() or
                not stat.S_ISLNK(status.st_mode) or status.st_uid != owner_uid or str(path.readlink()) != target):
            raise ValueError('NVIDIA SONAME link differs from its manifest')
        resolved = path.resolve(strict=True)
        if not resolved.is_relative_to(root) or '/' + str(resolved.relative_to(root)) not in records:
            raise ValueError('NVIDIA link escapes its packaged files')
        aliases[entry['path']] = resolved
    counts = {}
    for architecture in expected_architectures:
        directory = pathlib.Path('usr/lib') / ARCHITECTURES[architecture][0]
        for name in REQUIRED_VENDOR:
            if (architecture, name) not in libraries:
                raise ValueError('missing required NVIDIA vendor library: ' + name)
        for (abi, soname), path in libraries.items():
            if abi != architecture:
                continue
            alias = root / directory / soname
            if alias != path and aliases.get('/' + str(directory / soname)) != path:
                raise ValueError('missing NVIDIA SONAME alias: ' + soname)
            link_check(path)
        gbm = '/' + str(directory / 'gbm/nvidia-drm_gbm.so')
        allocator = libraries.get((architecture, 'libnvidia-allocator.so.1'))
        if allocator is None or aliases.get(gbm) != allocator:
            raise ValueError('missing NVIDIA GBM backend alias')
        for name in FRONTENDS:
            resolved = (root / directory / name).resolve(strict=True)
            if not resolved.is_relative_to(root):
                raise ValueError('graphics frontend escapes runtime root')
            path = trusted_file(root, resolved.relative_to(root), owner_uid)
            elf_identity(path, architecture)
            link_check(path)
        counts[architecture] = sum(abi == architecture for abi, _ in libraries)
    return {'schema': 1, 'result': 'passed', 'driver_version': lock['version'],
            'architectures': expected_architectures, 'vendor_library_counts': counts,
            'manifest_sha256': hashlib.sha256(manifest_path.read_bytes()).hexdigest(),
            'scope': 'ELF identity, package hashes, SONAME links and dynamic dependencies; no GPU or game execution'}
