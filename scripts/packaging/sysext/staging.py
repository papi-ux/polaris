#!/usr/bin/env python3
"""Freeze reviewed files into an exclusive private assembly directory."""
from __future__ import annotations

import os
from pathlib import Path
import stat

from bounded import copy_verified, require, strict_json
from manifest import digest, file_ref, object_keys, versioned


class Inputs:
    def __init__(self, source, destination):
        self.fd = os.open(source, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
        self.destination = Path(destination)
        self.destination.mkdir(mode=0o700)
        self.frozen = {}

    def close(self):
        os.close(self.fd)

    def copy(self, record, *, limit=128 * 1024 * 1024):
        file_ref(record)
        name = record['path']
        require(name not in self.frozen, 'duplicate staged destination')
        dest = self.destination / name
        dest.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
        with dest.open('xb') as output:
            os.fchmod(output.fileno(), 0o600)
            size = copy_verified(self.fd, name, record['sha256'], output, limit=limit)
            output.flush()
            os.fsync(output.fileno())
        self.frozen[name] = {'sha256': record['sha256'], 'bytes': size}
        return dest

    def json(self, record, *, limit=1024 * 1024):
        return strict_json(self.copy(record, limit=limit).read_bytes())


def snapshot_metadata(source, destination, inventory, policy):
    """Copy only RPMDB and all locked file_contexts companions, never host /var."""
    object_keys(inventory, {'schema_version', 'kind', 'files'}, 'target metadata inventory')
    versioned(inventory, 'polaris-sysext-target-metadata')
    rows = inventory['files']
    require(isinstance(rows, dict) and 0 < len(rows) <= 128, 'invalid target metadata inventory')
    for name, row in rows.items():
        object_keys(row, {'sha256', 'mode'}, 'target metadata file')
        digest(row['sha256'])
        require(type(row['mode']) is int and 0 <= row['mode'] <= 0o777 and not row['mode'] & 0o022, 'untrusted metadata mode')
        require((name.startswith('usr/share/rpm/') and '/' not in name[len('usr/share/rpm/'):]) or
                (name.startswith(policy) and '/' not in name[len(policy):]), 'unexpected target metadata path')
    require(policy in rows and 'usr/share/rpm/rpmdb.sqlite' in rows, 'missing RPMDB or policy')
    root_fd = os.open(source, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        # Inventory the source directories through no-follow descriptors. An
        # unlisted WAL or policy override would change the meaning of the copy.
        observed = set()
        for directory, prefix in (('usr/share/rpm', ''), (str(Path(policy).parent), 'file_contexts')):
            fd = os.dup(root_fd)
            try:
                for part in Path(directory).parts:
                    child = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC, dir_fd=fd)
                    os.close(fd)
                    fd = child
                for name in os.listdir(fd):
                    if not name.startswith(prefix):
                        continue
                    info = os.stat(name, dir_fd=fd, follow_symlinks=False)
                    require(stat.S_ISREG(info.st_mode) and info.st_uid == 0 and not info.st_mode & 0o022, 'untrusted target metadata')
                    require(directory + '/' + name in rows and stat.S_IMODE(info.st_mode) == rows[directory + '/' + name]['mode'],
                            'target metadata mode changed')
                    observed.add(directory + '/' + name)
            finally:
                os.close(fd)
        require(observed == set(rows), 'target metadata inventory changed')
        for name, row in rows.items():
            path = Path(destination) / name
            path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
            with path.open('xb') as output:
                copy_verified(root_fd, name, row['sha256'], output, limit=512 * 1024 * 1024)
                os.fchmod(output.fileno(), row['mode'])
    finally:
        os.close(root_fd)


def verify_snapshot(source, inventory):
    fd = os.open(source, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
    try:
        for name, row in inventory['files'].items():
            # Verification writes nowhere and repeats the descriptor-bound read.
            copy_verified(fd, name, row['sha256'], Discard(), limit=512 * 1024 * 1024)
    finally:
        os.close(fd)


class Discard:
    def write(self, _data):
        pass
