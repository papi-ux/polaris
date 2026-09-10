#!/usr/bin/env python3
"""Inventory only an owned assembly tree, including root and SELinux xattrs."""
from __future__ import annotations

import hashlib
import os
from pathlib import Path
import stat

from bounded import require


def sha256(path):
    digest = hashlib.sha256()
    with Path(path).open('rb') as source:
        for chunk in iter(lambda: source.read(1024 * 1024), b''):
            digest.update(chunk)
    return digest.hexdigest()


def inventory(root, *, labels=False):
    root = Path(root)
    result = {}
    for path in [root, *sorted(root.rglob('*'))]:
        info = path.lstat()
        name = path.relative_to(root).as_posix()
        require(info.st_uid == info.st_gid == 0, 'unexpected assembled ownership')
        item = {'mode': stat.S_IMODE(info.st_mode)}
        if stat.S_ISLNK(info.st_mode):
            item.update(type='link', target=os.readlink(path))
        elif stat.S_ISDIR(info.st_mode):
            item.update(type='directory')
        elif stat.S_ISREG(info.st_mode):
            item.update(type='file', bytes=info.st_size, sha256=sha256(path))
        else:
            raise ValueError('unexpected assembled special node')
        attrs = os.listxattr(path, follow_symlinks=False)
        require(set(attrs) <= {'security.selinux'}, 'unexpected assembled xattr')
        if labels:
            label = os.getxattr(path, 'security.selinux', follow_symlinks=False).rstrip(bytes([0])).decode('ascii')
            fields = label.split(':')
            require(len(fields) >= 4 and fields[0] == 'system_u' and fields[1] == 'object_r', 'invalid SELinux context')
            require(fields[2] not in ('unlabeled_t', 'user_tmp_t', 'user_home_t', 'tmp_t', 'container_file_t'), 'inappropriate SELinux label')
            item['selinux'] = label
        result[name] = item
    return result
