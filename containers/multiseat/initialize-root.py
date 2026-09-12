#!/usr/bin/env python3
"""Establish Polaris's image account and directories without a launcher init."""
import os
import pathlib
import pwd
import subprocess

# Production currently admits UID 1000 only. Numeric UID translation belongs
# in a separately reviewed identity adapter, not an elevated container init.
account = pwd.getpwuid(1000)
if account.pw_name != 'ubuntu' or account.pw_gid != 1000:
    raise ValueError('distribution account differs from the locked base')
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
