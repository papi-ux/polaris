#!/usr/bin/env python3
"""Verify every offline build input against the repository's reviewed locks."""
import hashlib
import json
import pathlib
import sys


def verify(path, expected):
    if path.is_symlink() or not path.is_file():
        raise ValueError('input must be a regular file: ' + str(path))
    with path.open('rb') as stream:
        digest = hashlib.file_digest(stream, 'sha256').hexdigest()
    if digest != expected:
        raise ValueError('input checksum mismatch: ' + path.name)


def verify_inputs(root, locks, role):
    lock = json.loads((locks / 'packages.json').read_text())
    if role not in ('build', 'runtime') or lock['platform'] != 'linux/amd64':
        raise ValueError('unsupported package role or architecture')
    packages = root / 'packages'
    if packages.is_symlink() or not packages.is_dir():
        raise ValueError('expected a regular input directory')
    expected = set()
    for package in lock[role]:
        filename = package['filename']
        if pathlib.Path(filename).name != filename or not filename.endswith('.deb') or filename in expected:
            raise ValueError('unsafe or duplicate package filename')
        expected.add(filename)
        verify(packages / filename, package['sha256'])
    actual = {p.name for p in packages.glob('*.deb')}
    if actual != expected:
        raise ValueError('unexpected or missing dependency input')
    if role == 'build':
        for filename, lockname in [('rust.tar.xz', 'rust.json'), ('plugin.tar', 'plugin.json'), ('gamescope.tar', 'gamescope.json')]:
            verify(root / filename, json.loads((locks / lockname).read_text())['sha256'])


if __name__ == '__main__':
    verify_inputs(pathlib.Path('/inputs'), pathlib.Path('/locks'), sys.argv[1])
