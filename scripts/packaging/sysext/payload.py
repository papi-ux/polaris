#!/usr/bin/env python3
"""Bounded newc parsing and projection for private system-extension assembly.

All admission checks remain active under python -O. No RPM scriptlet executes.
The caller owns an empty private staging directory; no payload path can replace
or traverse a symlink during materialization.
"""
from __future__ import annotations

from dataclasses import dataclass
import hashlib
import os
from pathlib import Path, PurePosixPath
import posixpath
import re
import stat

MAX_PAYLOAD = 128 * 1024 * 1024
MAX_ENTRIES = 65536
MAX_MATERIALIZED = 256 * 1024 * 1024
METADATA_DIR = 'usr/lib/extension-release.d'


class PayloadError(ValueError):
    pass


def require(condition, message):
    if not condition:
        raise PayloadError(message)


@dataclass(frozen=True)
class Entry:
    mode: int
    data: bytes = b''

    def manifest(self):
        result = {'mode': stat.S_IMODE(self.mode)}
        if stat.S_ISDIR(self.mode):
            result['type'] = 'directory'
        elif stat.S_ISLNK(self.mode):
            result.update(type='link', target=self.data.decode('utf-8'))
        else:
            result.update(type='file', bytes=len(self.data), sha256=hashlib.sha256(self.data).hexdigest())
        return result


def relative_name(name):
    while name.startswith('./'):
        name = name[2:]
    require(bool(name) and not name.startswith('/') and '\\' not in name, 'invalid archive path')
    require(all(part not in ('', '.', '..') for part in name.split('/')), 'invalid archive component')
    require(name.split('/')[0] in ('usr', 'opt'), 'payload outside /usr and /opt')
    require(name != METADATA_DIR and not name.startswith(METADATA_DIR + '/'), 'reserved extension metadata')
    return name


def validate_ancestors(entries):
    for name in entries:
        for parent in PurePosixPath(name).parents:
            if str(parent) == '.':
                break
            require(str(parent) not in entries or stat.S_ISDIR(entries[str(parent)].mode),
                    'non-directory archive ancestor: ' + name)


def validate_size(entries):
    # Hardlink members are materialized as separate regular files. Account for
    # every resulting copy, including groups whose archive stores the body once.
    require(len(entries) <= MAX_ENTRIES, 'materialized payload exceeds entry bound')
    total = 0
    for entry in entries.values():
        if stat.S_ISREG(entry.mode):
            total += len(entry.data)
            require(total <= MAX_MATERIALIZED, 'materialized payload exceeds size bound')


def parse_cpio(data):
    require(len(data) <= MAX_PAYLOAD, 'oversized RPM payload')
    entries, groups = {}, {}
    offset = 0
    while True:
        header = data[offset:offset + 110]
        require(len(header) == 110 and header[:6] in (b'070701', b'070702'), 'bad CPIO header')
        require(re.fullmatch(b'[0-9a-fA-F]{104}', header[6:]) is not None, 'bad CPIO integer')
        fields = [int(header[i:i + 8], 16) for i in range(6, 110, 8)]
        ino, mode, uid, gid, nlink, _mtime, size, major, minor, _rmajor, _rminor, namesize, checksum = fields
        require(0 < namesize <= 4096, 'bad CPIO name length')
        start = offset + 110
        raw = data[start:start + namesize]
        require(len(raw) == namesize and raw[-1:] == b'\0' and b'\0' not in raw[:-1], 'bad CPIO name')
        try:
            name = raw[:-1].decode('utf-8')
        except UnicodeDecodeError as error:
            raise PayloadError('invalid UTF-8 path') from error
        offset = (start + namesize + 3) & ~3
        body = data[offset:offset + size]
        require(len(body) == size, 'truncated CPIO body')
        offset = (offset + size + 3) & ~3
        require(offset <= len(data), 'truncated CPIO padding')
        if header[:6] == b'070702':
            require(sum(body) & 0xffffffff == checksum, 'CPIO checksum mismatch')
        else:
            require(checksum == 0, 'unexpected newc checksum')
        if name == 'TRAILER!!!':
            require(size == 0 and not data[offset:].strip(b'\0'), 'invalid CPIO trailer')
            break
        require(len(entries) < MAX_ENTRIES, 'too many CPIO entries')
        name = relative_name(name)
        require(name not in entries, 'duplicate CPIO path')
        require(uid == gid == 0, 'unexpected package ownership')
        require(stat.S_ISREG(mode) or stat.S_ISDIR(mode) or stat.S_ISLNK(mode), 'special node in RPM')
        require(not mode & 0o6000, 'privileged mode in RPM')
        require(stat.S_ISLNK(mode) or not mode & 0o022, 'writable trusted payload')
        require(nlink > 0, 'invalid link count')
        if stat.S_ISDIR(mode):
            require(size == 0, 'directory has data')
        if stat.S_ISLNK(mode):
            try:
                target = body.decode('utf-8')
            except UnicodeDecodeError as error:
                raise PayloadError('invalid UTF-8 link') from error
            require(0 < len(body) <= 4096 and '\0' not in target and '\\' not in target, 'invalid link target')
            resolved = posixpath.normpath(target if target.startswith('/') else '/' + posixpath.dirname(name) + '/' + target)
            require(resolved in ('/usr', '/opt') or resolved.startswith(('/usr/', '/opt/')), 'escaping link target')
        if stat.S_ISREG(mode) and nlink > 1:
            groups.setdefault((major, minor, ino), []).append((name, mode, nlink, body))
        entries[name] = Entry(mode, body)
    for members in groups.values():
        require(len(members) == members[0][2], 'incomplete hardlink set')
        require(all((mode, count) == (members[0][1], members[0][2]) for _, mode, count, _ in members), 'inconsistent hardlinks')
        contents = [body for _, _, _, body in members if body]
        require(len(contents) <= 1, 'ambiguous hardlink contents')
        content = contents[0] if contents else b''
        for name, mode, _, _ in members:
            entries[name] = Entry(mode, content)
    validate_ancestors(entries)
    validate_size(entries)
    return entries


def merge_payloads(payloads):
    merged = {}
    for entries in payloads:
        for name, entry in entries.items():
            if name in merged:
                require(stat.S_ISDIR(entry.mode) and merged[name] == entry, 'cross-package collision: ' + name)
            else:
                require(len(merged) < MAX_ENTRIES, 'combined payload exceeds entry bound')
                merged[name] = entry
    validate_ancestors(merged)
    validate_size(merged)
    # Record implicit parents explicitly so round-trip manifests are complete.
    for name in list(merged):
        for parent in PurePosixPath(name).parents:
            if str(parent) == '.':
                break
            if str(parent) not in merged:
                require(len(merged) < MAX_ENTRIES, 'implicit directories exceed entry bound')
                merged[str(parent)] = Entry(stat.S_IFDIR | 0o755)
    return merged


def with_metadata(entries, content):
    require(not any(name == METADATA_DIR or name.startswith(METADATA_DIR + '/') for name in entries), 'metadata collision')
    metadata = {METADATA_DIR: Entry(stat.S_IFDIR | 0o755),
                METADATA_DIR + '/extension-release.polaris': Entry(stat.S_IFREG | 0o644, content.encode('utf-8'))}
    return merge_payloads([entries, metadata])


def materialize(entries, root):
    """Create a new tree below a caller-owned private parent, using no-follow FDs."""
    validate_ancestors(entries)
    validate_size(entries)
    root = Path(root)
    root.mkdir(mode=0o755)
    flags = os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC
    root_fd = os.open(root, flags)
    os.fchmod(root_fd, 0o755)
    try:
        for name, entry in sorted(entries.items(), key=lambda pair: (len(PurePosixPath(pair[0]).parts), pair[0])):
            parts = PurePosixPath(name).parts
            require(parts and not name.startswith('/') and all(p not in ('.', '..') for p in parts), 'invalid projection path')
            fd = os.dup(root_fd)
            try:
                for part in parts[:-1]:
                    child = os.open(part, flags, dir_fd=fd)
                    os.close(fd)
                    fd = child
                if stat.S_ISDIR(entry.mode):
                    os.mkdir(parts[-1], stat.S_IMODE(entry.mode), dir_fd=fd)
                    directory = os.open(parts[-1], flags, dir_fd=fd)
                    try:
                        os.fchmod(directory, stat.S_IMODE(entry.mode))
                    finally:
                        os.close(directory)
                elif stat.S_ISLNK(entry.mode):
                    os.symlink(entry.data.decode('utf-8'), parts[-1], dir_fd=fd)
                else:
                    dest = os.open(parts[-1], os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW | os.O_CLOEXEC,
                                   stat.S_IMODE(entry.mode), dir_fd=fd)
                    with os.fdopen(dest, 'wb') as output:
                        output.write(entry.data)
                        os.fchmod(output.fileno(), stat.S_IMODE(entry.mode))
            finally:
                os.close(fd)
    finally:
        os.close(root_fd)
