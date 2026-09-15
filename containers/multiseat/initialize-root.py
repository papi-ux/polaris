#!/usr/bin/env python3
"""Establish Polaris's image account and directories without a launcher init."""
import os
import hashlib
import pathlib
import pwd
import stat
import subprocess


def remove_inherited_init(root, expected='b47bdf55665e4041821db93ad28ca03ce2ff3fffaebd7d5de143bb08f7ce5588'):
    # This unmanaged executable belongs to the pinned distribution image, not
    # to a dpkg package. Polaris supplies its own worker and never invokes it.
    # A changed base requires reviewing this exact removal before rebuilding.
    path = root / 'usr/bin/pebble'
    info = path.lstat()
    if (not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or
            hashlib.sha256(path.read_bytes()).hexdigest() != expected):
        raise ValueError('inherited image init differs from the reviewed base')
    path.unlink()


def main():
    # Production currently admits UID 1000 only. Numeric UID translation belongs
    # in a separately reviewed identity adapter, not an elevated container init.
    account = pwd.getpwuid(1000)
    if account.pw_name != 'ubuntu' or account.pw_gid != 1000:
        raise ValueError('distribution account differs from the locked base')
    remove_inherited_init(pathlib.Path('/'))
    subprocess.run(['usermod', '--login=polaris', '--home=/home/polaris', '--groups=', 'ubuntu'], check=True)
    subprocess.run(['groupmod', '--new-name=polaris', 'ubuntu'], check=True)
    subprocess.run(['usermod', '--lock', '--shell=/usr/sbin/nologin', 'polaris'], check=True)
    for name in ['/home/polaris', '/run/polaris-seat', '/var/lib/polaris-seat']:
        path = pathlib.Path(name)
        path.mkdir(parents=True, exist_ok=True)
        path.chmod(0o700)
        if name == '/home/polaris':
            os.chown(path, 1000, 1000)
    # Xwayland requires its common parent to exist even with a read-only root.
    path = pathlib.Path('/tmp/.X11-unix')
    path.mkdir(exist_ok=True)
    path.chmod(0o1777)


if __name__ == '__main__':
    main()
