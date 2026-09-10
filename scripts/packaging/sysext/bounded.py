#!/usr/bin/env python3
"""Descriptor-bound input staging and bounded child execution for the assembler."""
from __future__ import annotations

import hashlib
import functools
import json
import os
from pathlib import PurePosixPath
import selectors
import signal
import stat
import subprocess
import sys
import time


class BuildError(ValueError):
    pass


def require(value, message):
    if not value:
        raise BuildError(message)


def strict_json(data):
    def members(pairs):
        value = {}
        for key, item in pairs:
            require(key not in value, 'duplicate JSON member: ' + key)
            value[key] = item
        return value
    try:
        return json.loads(data, object_pairs_hook=members,
                          parse_constant=lambda value: (_ for _ in ()).throw(BuildError('non-finite JSON value')))
    except (UnicodeDecodeError, json.JSONDecodeError) as error:
        raise BuildError('invalid JSON') from error


def relative_path(name):
    require(isinstance(name, str) and name and '\0' not in name and '\\' not in name, 'invalid relative path')
    parts = PurePosixPath(name).parts
    require(parts and not name.startswith('/') and all(part not in ('.', '..') for part in parts), 'escaping path')
    require('/'.join(parts) == name, 'noncanonical path')
    return parts


def open_beneath(root_fd, name, *, trusted=False):
    parts = relative_path(name)
    fd = os.dup(root_fd)
    try:
        if trusted:
            require_trusted(os.fstat(fd))
        for part in parts[:-1]:
            child = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC, dir_fd=fd)
            os.close(fd)
            fd = child
            if trusted:
                require_trusted(os.fstat(fd))
        result = os.open(parts[-1], os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK | os.O_CLOEXEC, dir_fd=fd)
        try:
            if trusted:
                require_trusted(os.fstat(result))
            return result
        except BaseException:
            os.close(result)
            raise
    finally:
        os.close(fd)


def require_trusted(info):
    require(info.st_uid == 0 and not info.st_mode & 0o022, 'untrusted executable path')


def identity(info):
    return info.st_dev, info.st_ino, info.st_mode, info.st_size, info.st_mtime_ns, info.st_ctime_ns


def copy_verified(root_fd, name, digest, output, *, limit=128 * 1024 * 1024, trusted=False):
    """Read one immutable descriptor; output is an exclusive caller-owned file."""
    fd = open_beneath(root_fd, name, trusted=trusted)
    try:
        before = os.fstat(fd)
        require(stat.S_ISREG(before.st_mode) and before.st_size <= limit, 'input is not a bounded regular file')
        hasher = hashlib.sha256()
        total = 0
        while True:
            chunk = os.read(fd, min(1024 * 1024, limit - total + 1))
            if not chunk:
                break
            total += len(chunk)
            require(total <= limit, 'input grew beyond size limit')
            hasher.update(chunk)
            output.write(chunk)
        require(identity(before) == identity(os.fstat(fd)) and total == before.st_size, 'input changed while reading')
        require(hasher.hexdigest() == digest, 'input digest mismatch: ' + name)
        return total
    finally:
        os.close(fd)


def run(argv, *, cwd, timeout=180, stdout_limit=1024 * 1024, stderr_limit=1024 * 1024, check=True):
    """Enforce output limits during reads and kill/reap the owned group on failure."""
    require(timeout > 0 and stdout_limit >= 0 and stderr_limit >= 0, 'invalid command bounds')
    require(sys.platform == 'linux' and hasattr(os, 'waitid'), 'owned command execution requires Linux waitid')
    deadline = time.monotonic() + timeout
    termination = {signal.SIGTERM, signal.SIGHUP, signal.SIGINT}
    original_mask = signal.pthread_sigmask(signal.SIG_BLOCK, termination)
    child = None
    buffers = {'stdout': bytearray(), 'stderr': bytearray()}
    limits = {'stdout': stdout_limit, 'stderr': stderr_limit}
    try:
        # This CLI is single-threaded. Block termination until the returned
        # child is owned by this try/finally; the child restores the caller's
        # original mask before exec so tool termination remains effective.
        child = subprocess.Popen([str(value) for value in argv], cwd=cwd, stdout=subprocess.PIPE,
                                 stderr=subprocess.PIPE, start_new_session=True,
                                 preexec_fn=functools.partial(signal.pthread_sigmask, signal.SIG_SETMASK, original_mask),
                                 env={'PATH': '/usr/sbin:/usr/bin:/sbin:/bin', 'LANG': 'C.UTF-8'})
        signal.pthread_sigmask(signal.SIG_SETMASK, original_mask)
        with selectors.DefaultSelector() as selector:
            for name in buffers:
                pipe = getattr(child, name)
                os.set_blocking(pipe.fileno(), False)
                selector.register(pipe, selectors.EVENT_READ, name)
            while selector.get_map():
                remaining = deadline - time.monotonic()
                require(remaining > 0, 'command timed out')
                for key, _ in selector.select(min(remaining, 0.1)):
                    name = key.data
                    chunk = os.read(key.fileobj.fileno(), min(65536, limits[name] - len(buffers[name]) + 1))
                    if not chunk:
                        selector.unregister(key.fileobj)
                    else:
                        buffers[name].extend(chunk)
                        require(len(buffers[name]) <= limits[name], 'command output exceeded limit')
            while True:
                require(time.monotonic() < deadline, 'command timed out')
                # Observe exit without reaping. Keeping the leader waitable
                # prevents its numeric process-group ID from being reused
                # before owned-group cleanup has finished.
                exited = os.waitid(os.P_PID, child.pid, os.WEXITED | os.WNOHANG | os.WNOWAIT)
                if exited is not None:
                    code = exited.si_status if exited.si_code == os.CLD_EXITED else -exited.si_status
                    break
                time.sleep(min(0.01, max(0, deadline - time.monotonic())))
        require(not check or code == 0, 'command failed with exit ' + str(code))
        return {'returncode': code, **{key: bytes(value) for key, value in buffers.items()}}
    finally:
        signal.pthread_sigmask(signal.SIG_BLOCK, termination)
        try:
            if child is not None:
                try:
                    # Also remove children that closed their output pipes on a
                    # successful leader exit; reap the leader only afterwards.
                    try:
                        try:
                            os.killpg(child.pid, signal.SIGKILL)
                        except ProcessLookupError:
                            pass
                    finally:
                        child.wait(timeout=5)
                finally:
                    child.stdout.close()
                    child.stderr.close()
        finally:
            signal.pthread_sigmask(signal.SIG_SETMASK, original_mask)
