#!/usr/bin/env python3
"""Fetch reviewed inputs before the network-free OCI build (Linux/amd64)."""
import argparse
import concurrent.futures
import configparser
import hashlib
import json
import os
import pathlib
import platform
import shutil
import subprocess
import tempfile
import urllib.request

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent
INPUTS = REPO / 'build/runtime-inputs'


def digest(path):
    if path.is_symlink() or not path.is_file():
        raise ValueError('expected a regular input: ' + str(path))
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def fetch(entry, destination):
    destination.parent.mkdir(parents=True, exist_ok=True)
    if destination.exists():
        if digest(destination) != entry['sha256']:
            raise ValueError('cached input checksum mismatch: ' + destination.name)
        return
    if not entry['url'].startswith('https://'):
        raise ValueError('input URL must use HTTPS')
    fd, temporary = tempfile.mkstemp(prefix='.download-', dir=destination.parent)
    try:
        with os.fdopen(fd, 'wb') as output, urllib.request.urlopen(entry['url'], timeout=120) as response:
            shutil.copyfileobj(response, output)
        if digest(pathlib.Path(temporary)) != entry['sha256']:
            raise ValueError('download checksum mismatch: ' + destination.name)
        os.chmod(temporary, 0o644)
        os.replace(temporary, destination)
    finally:
        pathlib.Path(temporary).unlink(missing_ok=True)


def run(arguments, **kwargs):
    return subprocess.run(arguments, check=True, **kwargs)


def checkout(url, revision, destination):
    run(['git', 'init', str(destination)])
    run(['git', '-C', str(destination), 'remote', 'add', 'origin', url])
    run(['git', '-C', str(destination), 'fetch', '--depth=1', 'origin', revision])
    run(['git', '-C', str(destination), 'checkout', '--detach', 'FETCH_HEAD'])
    actual = subprocess.check_output(['git', '-C', str(destination), 'rev-parse', 'HEAD'], text=True).strip()
    if actual != revision:
        raise ValueError('source checkout does not match its lock')


def canonical_archive(source, destination, lock, excludes):
    temporary = destination.with_suffix('.unverified.tar')
    run(['tar', '--format=gnu', '--sort=name', '--mtime=@' + str(lock['archive_epoch']),
         '--owner=0', '--group=0', '--numeric-owner', *['--exclude=' + p for p in excludes],
         '-cf', str(temporary), '-C', str(source), '.'])
    if digest(temporary) != lock['sha256']:
        raise ValueError('reconstructed source archive differs from its lock: ' + destination.name)
    temporary.replace(destination)


def prepare_plugin(lock, rust):
    archive = INPUTS / 'plugin.tar'
    if archive.exists():
        if digest(archive) != lock['sha256']:
            raise ValueError('plugin archive checksum mismatch')
        return
    with tempfile.TemporaryDirectory(prefix='plugin-', dir=INPUTS) as temporary:
        temporary = pathlib.Path(temporary)
        toolchain = temporary / 'toolchain'
        toolchain.mkdir()
        run(['tar', '-xf', str(INPUTS / 'toolchains' / pathlib.PurePosixPath(rust['url']).name),
             '-C', str(toolchain), '--strip-components=1'])
        prefix = temporary / 'rust'
        run([str(toolchain / 'install.sh'), '--prefix=' + str(prefix),
             '--components=rustc,cargo,rust-std-x86_64-unknown-linux-gnu', '--disable-ldconfig'])
        source = temporary / 'source'
        checkout(lock['url'], lock['revision'], source)
        if digest(source / 'Cargo.lock') != lock['cargo_lock_sha256']:
            raise ValueError('plugin Cargo.lock changed')
        environment = dict(os.environ, PATH=str(prefix / 'bin') + ':' + os.environ['PATH'],
                           CARGO_HOME=str(temporary / 'cargo-home'), CARGO_NET_OFFLINE='false',
                           CARGO_NET_GIT_FETCH_WITH_CLI='true')
        config = subprocess.check_output([str(prefix / 'bin/cargo'), 'vendor', '--locked',
                                          '--versioned-dirs', 'vendor'], cwd=source, env=environment)
        (source / '.cargo').mkdir(exist_ok=True)
        (source / '.cargo/config.toml').write_bytes(config)
        canonical_archive(source, archive, lock, ['./.git', './target'])


def prepare_gamescope(lock):
    archive = INPUTS / 'gamescope.tar'
    if archive.exists():
        if digest(archive) != lock['sha256']:
            raise ValueError('Gamescope archive checksum mismatch')
        return
    with tempfile.TemporaryDirectory(prefix='gamescope-', dir=INPUTS) as temporary:
        source = pathlib.Path(temporary) / 'source'
        checkout(lock['url'], lock['revision'], source)
        run(['git', '-C', str(source), 'submodule', 'update', '--init', '--recursive', '--depth=1'])
        for name, revision in lock['wraps'].items():
            config = configparser.ConfigParser()
            config.read(source / 'subprojects' / (name + '.wrap'))
            wrap = config['wrap-git']
            if wrap['revision'] != revision or wrap['directory'] != name:
                raise ValueError('Gamescope wrap differs from lock')
            destination = source / 'subprojects' / name
            checkout(wrap['url'], revision, destination)
            shutil.copytree(source / 'subprojects/packagefiles' / name, destination, dirs_exist_ok=True)
        canonical_archive(source, archive, lock, ['.git', './build'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('profile', choices=['gamescope', 'steam', 'heroic', 'lutris'])
    parser.add_argument('--nvidia', action='store_true')
    args = parser.parse_args()
    if platform.system() != 'Linux' or platform.machine() not in ('x86_64', 'amd64'):
        parser.error('prepare inputs on Linux/amd64 with Python 3.11+, GNU tar and git')
    INPUTS.mkdir(parents=True, exist_ok=True)
    package_lock = json.loads((HERE / 'locks' / (args.profile + '.packages.json')).read_text())
    downloads = []
    for role in ['runtime', 'build']:
        for package in package_lock[role]:
            filename = package['filename']
            if pathlib.Path(filename).name != filename:
                raise ValueError('unsafe locked filename')
            downloads.append((package, INPUTS / args.profile / role / filename))
    rust = json.loads((HERE / 'locks/rust.json').read_text())
    downloads.append((rust, INPUTS / 'toolchains' / pathlib.PurePosixPath(rust['url']).name))
    if args.nvidia:
        nvidia = json.loads((HERE / 'locks/nvidia.json').read_text())
        downloads.append((nvidia, INPUTS / pathlib.PurePosixPath(nvidia['url']).name))
    with concurrent.futures.ThreadPoolExecutor(max_workers=8) as pool:
        list(pool.map(lambda item: fetch(*item), downloads))
    prepare_plugin(json.loads((HERE / 'locks/plugin.json').read_text()), rust)
    prepare_gamescope(json.loads((HERE / 'locks/gamescope.json').read_text()))
    images = json.loads((HERE / 'images.lock.json').read_text())
    profile = next(p for p in images['runtime_profiles'] if p['id'] == args.profile)
    if images['schema'] != 2 or profile['reference'] != package_lock['source_root']:
        raise ValueError('package lock does not match the selected source root')
    for reference in [images['builder']['reference'], profile['reference']]:
        run(['podman', 'pull', '--platform=linux/amd64', reference])
    print('Verified all offline inputs for ' + args.profile)


if __name__ == '__main__':
    main()
