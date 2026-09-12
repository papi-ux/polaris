#!/usr/bin/env python3
"""Build only the locked nvcodec plugin and its missing CUDA support library."""
import hashlib
import json
import pathlib
import shutil
import subprocess

lock = json.loads(pathlib.Path('/nvcodec.lock.json').read_text())
archive = pathlib.Path('/nvcodec.tar.xz')
with archive.open('rb') as stream:
    if hashlib.file_digest(stream, 'sha256').hexdigest() != lock['sha256']:
        raise ValueError('nvcodec source archive checksum mismatch')
source = pathlib.Path('/nvcodec-src')
source.mkdir()
subprocess.run(['tar', '-xf', str(archive), '-C', str(source), '--strip-components=1'], check=True)
build = source / 'build'
subprocess.run(['meson', 'setup', str(build), str(source), '--prefix=/usr',
                '--libdir=lib/x86_64-linux-gnu', '--buildtype=release', '--wrap-mode=nodownload',
                '-Dauto_features=disabled', '-Dnvcodec=enabled', '-Dnvcodec-cuda-precompile=disabled',
                '-Dcuda-nvmm=disabled', '-Dtests=disabled', '-Dexamples=disabled',
                '-Ddoc=disabled', '-Dintrospection=disabled'], check=True)
subprocess.run(['meson', 'compile', '-C', str(build), '-j', '4'], check=True)
subprocess.run(['meson', 'install', '-C', str(build), '--no-rebuild', '--destdir=/nvcodec-install'], check=True)
installed = pathlib.Path('/nvcodec-install/usr/lib/x86_64-linux-gnu')
# Preserve the runtime root's GStreamer ABI libraries and plugins. The source
# release matches the locked 1.26.0 package roots; check-runtime resolves every
# symbol against those installed dependencies in the final NVIDIA image.
root = pathlib.Path('/nvcodec-root')
lib = root / 'usr/lib/x86_64-linux-gnu'
manifest = []
for origin, relative in [
    (installed / 'gstreamer-1.0/libgstnvcodec.so', 'gstreamer-1.0/libgstnvcodec.so'),
    (installed / 'libgstcuda-1.0.so.0.2600.0', 'libgstcuda-1.0.so.0.2600.0'),
]:
    destination = lib / relative
    destination.parent.mkdir(parents=True, exist_ok=True)
    shutil.copyfile(origin, destination)
    destination.chmod(0o644)
    manifest.append({'path': '/' + str(destination.relative_to(root)),
                     'sha256': hashlib.sha256(destination.read_bytes()).hexdigest()})
(lib / 'libgstcuda-1.0.so.0').symlink_to('libgstcuda-1.0.so.0.2600.0')
metadata = root / 'usr/share/polaris/build'
metadata.mkdir(parents=True)
(metadata / 'nvcodec.lock.json').write_text(json.dumps(lock, indent=2) + '\n')
(metadata / 'nvcodec-files.json').write_text(json.dumps(manifest, indent=2) + '\n')

license_dir = root / 'usr/share/licenses/polaris-nvcodec'
license_dir.mkdir(parents=True)
shutil.copyfile(source / 'COPYING', license_dir / 'COPYING')
