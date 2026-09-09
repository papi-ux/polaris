#!/usr/bin/env python3
"""Build the opt-in graphics userspace layer; never run the driver installer."""
import hashlib
import json
import pathlib
import re
import shutil
import subprocess

lock = json.loads(pathlib.Path('/nvidia.lock.json').read_text())
archive = pathlib.Path('/nvidia.run')
with archive.open('rb') as source:
    if hashlib.file_digest(source, 'sha256').hexdigest() != lock['sha256']:
        raise ValueError('NVIDIA source archive checksum mismatch')
subprocess.run(['sh', str(archive), '--extract-only', '--target', '/nvidia-source'], check=True)
source = pathlib.Path('/nvidia-source')
root = pathlib.Path('/nvidia-root')
manifest = []


def copy_file(path, destination):
    if not path.is_file() or path.is_symlink():
        raise ValueError('NVIDIA input must be a regular file: ' + path.name)
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(path, destination)
    destination.chmod(0o644)
    with destination.open('rb') as content:
        digest = hashlib.file_digest(content, 'sha256').hexdigest()
    manifest.append({'path': '/' + str(destination.relative_to(root)), 'sha256': digest})


# Keep the root's generic GLVND frontends. Only vendor implementations and their
# userspace dependencies are selected; kernel, firmware and host tools are absent.
prefixes = (
    'libcuda.so.', 'libnvcuvid.so.', 'libnvoptix.so.', 'libEGL_nvidia.so.',
    'libGLX_nvidia.so.', 'libGLESv1_CM_nvidia.so.', 'libGLESv2_nvidia.so.',
    'libnvidia-allocator.so.', 'libnvidia-eglcore.so.', 'libnvidia-egl-gbm.so.',
    'libnvidia-egl-wayland.so.', 'libnvidia-egl-wayland2.so.',
    'libnvidia-egl-xcb.so.', 'libnvidia-egl-xlib.so.', 'libnvidia-encode.so.',
    'libnvidia-fbc.so.', 'libnvidia-glcore.so.', 'libnvidia-glsi.so.',
    'libnvidia-glvkspirv.so.', 'libnvidia-gpucomp.so.', 'libnvidia-ml.so.',
    'libnvidia-nvvm.so.', 'libnvidia-ptxjitcompiler.so.', 'libnvidia-rtcore.so.',
    'libnvidia-tls.so.', 'libnvidia-wayland-client.so.', 'libnvidia-present.so.',
    'libnvidia-sandboxutils.so.',
)
for origin, architecture in [(source, 'x86_64-linux-gnu'), (source / '32', 'i386-linux-gnu')]:
    destination = root / 'usr/lib' / architecture
    selected = [p for p in sorted(origin.iterdir()) if p.name.startswith(prefixes)]
    for required in ['libcuda.so.', 'libEGL_nvidia.so.', 'libGLX_nvidia.so.', 'libnvidia-encode.so.']:
        if not any(p.name.startswith(required) for p in selected):
            raise ValueError('missing NVIDIA library: ' + required)
    for path in selected:
        copy_file(path, destination / path.name)
        elf = subprocess.check_output(['readelf', '-d', str(path)], text=True)
        soname = re.search(r'\(SONAME\).*\[([^/\]]+)\]', elf)
        if not soname:
            raise ValueError('missing NVIDIA SONAME: ' + path.name)
        if soname[1] != path.name:
            (destination / soname[1]).symlink_to(path.name)
    (destination / 'gbm').mkdir()
    (destination / 'gbm/nvidia-drm_gbm.so').symlink_to('../libnvidia-allocator.so.1')

configurations = {
    '10_nvidia.json': 'glvnd/egl_vendor.d',
    'nvidia_icd.json': 'vulkan/icd.d',
    'nvidia_layers.json': 'vulkan/implicit_layer.d',
    **{name: 'egl/egl_external_platform.d' for name in [
        '09_nvidia_wayland2.json', '10_nvidia_wayland.json', '15_nvidia_gbm.json',
        '20_nvidia_xcb.json', '20_nvidia_xlib.json',
    ]},
}
for filename, directory in configurations.items():
    copy_file(source / filename, root / 'usr/share' / directory / filename)
copy_file(source / 'LICENSE', root / 'usr/share/licenses/polaris-nvidia/LICENSE')
metadata = root / 'usr/share/polaris/build'
metadata.mkdir(parents=True)
(metadata / 'nvidia.lock.json').write_text(json.dumps(lock, indent=2) + '\n')
(metadata / 'nvidia-files.json').write_text(json.dumps(manifest, indent=2) + '\n')
