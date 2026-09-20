#!/usr/bin/env python3
"""Build the opt-in graphics userspace layer; never run the driver installer."""
import hashlib
import json
import pathlib
import re
import shutil
import subprocess
import sys
from nvidia_runtime import ARCHITECTURES, architectures, elf_identity, soname_valid

# A host-driver layer ships only what the machine's own driver package does not
# provide: the EGL platform libraries and their descriptions. Everything the
# reviewed contract names is borrowed from the host at run time instead.
host_driver = '--host' in sys.argv
contract_prefixes = ()
if host_driver:
    contract = json.loads(pathlib.Path('/nvidia-host-contract.json').read_text())
    contract_prefixes = tuple(contract['library_prefixes'])

lock = json.loads(pathlib.Path('/nvidia.lock.json').read_text())
archive = pathlib.Path('/nvidia.run')
with archive.open('rb') as source:
    if hashlib.file_digest(source, 'sha256').hexdigest() != lock['sha256']:
        raise ValueError('NVIDIA source archive checksum mismatch')
subprocess.run(['sh', str(archive), '--extract-only', '--target', '/nvidia-source'], check=True)
source = pathlib.Path('/nvidia-source')
root = pathlib.Path('/nvidia-root')
manifest = []
symlinks = []
profile = json.loads(pathlib.Path('/locks/packages.json').read_text())['profile']
selected_architectures = architectures(profile)


def copy_file(path, destination, **identity):
    if not path.is_file() or path.is_symlink():
        raise ValueError('NVIDIA input must be a regular file: ' + path.name)
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(path, destination)
    destination.chmod(0o644)
    with destination.open('rb') as content:
        digest = hashlib.file_digest(content, 'sha256').hexdigest()
    manifest.append({'path': '/' + str(destination.relative_to(root)), 'sha256': digest, **identity})


def link_file(path, target):
    path.symlink_to(target)
    symlinks.append({'path': '/' + str(path.relative_to(root)), 'target': target})


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
for architecture in selected_architectures:
    origin = source if architecture == 'amd64' else source / '32'
    destination = root / 'usr/lib' / ARCHITECTURES[architecture][0]
    selected = [p for p in sorted(origin.iterdir()) if p.name.startswith(prefixes)]
    if host_driver:
        selected = [p for p in selected if not p.name.startswith(contract_prefixes)]
        if not selected:
            raise ValueError('a host-driver layer still ships the EGL platform libraries')
    else:
        for required in ['libcuda.so.', 'libEGL_nvidia.so.', 'libGLX_nvidia.so.', 'libnvidia-encode.so.']:
            if not any(p.name.startswith(required) for p in selected):
                raise ValueError('missing NVIDIA library: ' + required)
    for path in selected:
        elf_identity(path, architecture)
        elf = subprocess.check_output(['readelf', '-d', str(path)], text=True)
        soname = re.search(r'\(SONAME\).*\[([^/\]]+)\]', elf)
        if not soname or not soname_valid(soname[1]):
            raise ValueError('missing NVIDIA SONAME: ' + path.name)
        copy_file(path, destination / path.name, architecture=architecture, soname=soname[1])
        if soname[1] != path.name:
            link_file(destination / soname[1], path.name)
    (destination / 'gbm').mkdir()
    # The allocator is host-mounted in a host-driver layer, so this alias only
    # resolves once a Space runs. It is recorded either way.
    link_file(destination / 'gbm/nvidia-drm_gbm.so', '../libnvidia-allocator.so.1')

configurations = {
    '10_nvidia.json': 'glvnd/egl_vendor.d',
    'nvidia_icd.json': 'vulkan/icd.d',
    'nvidia_layers.json': 'vulkan/implicit_layer.d',
    **{name: 'egl/egl_external_platform.d' for name in [
        '09_nvidia_wayland2.json', '10_nvidia_wayland.json', '15_nvidia_gbm.json',
        '20_nvidia_xcb.json', '20_nvidia_xlib.json',
    ]},
}
if host_driver:
    # The machine's own Vulkan and EGL descriptions are rewritten by Polaris and
    # mounted, because a distribution may write absolute host paths into them.
    for name in ['10_nvidia.json', 'nvidia_icd.json', 'nvidia_layers.json']:
        configurations.pop(name, None)
for filename, directory in configurations.items():
    copy_file(source / filename, root / 'usr/share' / directory / filename)
copy_file(source / 'LICENSE', root / 'usr/share/licenses/polaris-nvidia/LICENSE')
metadata = root / 'usr/share/polaris/build'
metadata.mkdir(parents=True)
(metadata / 'nvidia.lock.json').write_text(json.dumps(lock, indent=2) + '\n')
(metadata / 'nvidia-files.json').write_text(json.dumps({
    'schema': 1, 'driver_version': '' if host_driver else lock['version'],
    'architectures': selected_architectures, 'files': manifest, 'symlinks': symlinks,
    'source': 'host' if host_driver else 'image'}, indent=2) + '\n')
if host_driver:
    shutil.copyfile('/nvidia-host-contract.json', metadata / 'nvidia-host-contract.json')
