"""Read only the owner's named installed-state file, without recovery or writes."""

import os
from pathlib import Path
import stat

from .installations import MAX_BYTES, ManifestError, _absolute, parse_installations


def _identity(info):
    return (info.st_dev, info.st_ino, info.st_size, info.st_mtime_ns, info.st_ctime_ns,
            info.st_uid, info.st_mode)


def read_installations(data_root, expected_uid=None):
    """Read a stable regular file using an anchored directory descriptor.

    No .prev/.tmp, account cache, sidecar or path from file contents is opened.
    Reject symlinks at the data root/file and changes during the read; callers
    may try a new snapshot on a later refresh. No retry or stale fallback here.
    Candidate metadata still does not authorize filesystem/runtime ownership.
    """
    try:
        root = Path(_absolute(data_root))
    except (ValueError, TypeError):
        raise ManifestError("Invalid Epic data root") from None
    uid = os.getuid() if expected_uid is None else expected_uid
    if type(uid) is not int or uid < 0:
        raise ManifestError("Invalid installed-state owner")
    directory_fd = file_fd = None
    try:
        directory_fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC)
        directory = os.fstat(directory_fd)
        if directory.st_uid != uid:
            raise ManifestError("Epic data root belongs to another owner")
        file_fd = os.open("installations.jsonl", os.O_RDONLY | os.O_NONBLOCK | os.O_NOFOLLOW | os.O_CLOEXEC,
                          dir_fd=directory_fd)
        before = os.fstat(file_fd)
        if not stat.S_ISREG(before.st_mode) or before.st_uid != uid:
            raise ManifestError("Installed state must be an owner-held regular file")
        if not 0 < before.st_size <= MAX_BYTES:
            raise ManifestError("Missing or oversized installed-state file")
        parts = []
        remaining = before.st_size + 1
        while remaining:
            part = os.read(file_fd, min(remaining, 64 * 1024))
            if not part:
                break
            parts.append(part)
            remaining -= len(part)
        raw = b"".join(parts)
        after = os.fstat(file_fd)
        named = os.stat("installations.jsonl", dir_fd=directory_fd, follow_symlinks=False)
        named_root = os.stat(root, follow_symlinks=False)
        if (len(raw) != before.st_size or _identity(before) != _identity(after)
                or _identity(before) != _identity(named)
                or not stat.S_ISDIR(named_root.st_mode)
                or (named_root.st_dev, named_root.st_ino, named_root.st_uid) !=
                   (directory.st_dev, directory.st_ino, directory.st_uid)):
            raise ManifestError("Installed state changed while reading")
        return parse_installations(raw, str(root))
    except OSError:
        # No raw path/file content or adjacent metadata in error messages.
        raise ManifestError("Epic installed-state file is unavailable") from None
    finally:
        if file_fd is not None:
            os.close(file_fd)
        if directory_fd is not None:
            os.close(directory_fd)
