#!/usr/bin/env python3
"""Acquire the pinned APT plan; the live archive supplies bytes, never metadata."""
import argparse
from concurrent.futures import ThreadPoolExecutor
from contextlib import contextmanager
import hashlib
import os
from pathlib import Path
import re
import shlex
import stat
import subprocess
import sys
import tempfile
from urllib.parse import unquote, urlsplit


class Unavailable(RuntimeError):
    """A transport failure may use the identical official archive payload."""


def parse_plan(text, snapshot, local_deb=None):
    prefix = f'/ubuntu/{snapshot}/'
    rows, names = [], set()
    local_filename = None
    for line in text.splitlines():
        if not line.startswith("'"):
            if '://' in line or '.deb' in line:
                raise ValueError('Unrecognized APT payload row')
            continue
        fields = shlex.split(line)
        # APT 2.8.3 emits file:/absolute/path, cache name, size, with no hash
        # for a local-only DEB. Only this caller-bound file may lack that hash.
        if local_deb is not None and fields and fields[0] == 'file:' + str(local_deb[0].resolve(strict=True)):
            if (len(fields) not in (3, 4) or local_filename is not None
                    or not re.fullmatch(r'[a-z0-9][A-Za-z0-9_+~.%:-]*\.deb', fields[1])
                    or re.search(r'%(?![0-9a-fA-F]{2})|%2f|%5c|%25', fields[1], re.I)
                    or not re.fullmatch(r'[a-z0-9][A-Za-z0-9_+~.:-]*\.deb', unquote(fields[1]))
                    or fields[1] in names or fields[2] != str(local_deb[1])
                    or (len(fields) == 4 and fields[3] != local_deb[2])):
                raise ValueError('Local APT row does not match the bound DEB')
            names.add(fields[1])
            # Use APT's emitted basename; do not derive one from the input path
            # or reconstruct its package/version encoding. Trust is the caller's
            # size/SHA256 binding, not the local row's absent repository hash.
            local_filename = fields[1]
            continue
        if len(fields) != 4:
            raise ValueError('APT payload row needs URI, filename, size and a strong hash')
        uri, filename, size, digest = fields
        parsed = urlsplit(uri)
        if (parsed.scheme != 'https' or parsed.netloc != 'snapshot.ubuntu.com'
                or parsed.query or parsed.fragment or not parsed.path.startswith(prefix + 'pool/')):
            raise ValueError('APT payload is outside the pinned Ubuntu snapshot')
        path = parsed.path[len(prefix):]
        # Permit APT's encoded plus/epoch/tilde, but never encoded path separators.
        if (re.search(r'%(?![0-9a-fA-F]{2})|%2f|%5c|%25', path, re.I)
                or any(part in ('', '.', '..') for part in unquote(path).split('/'))
                or not re.fullmatch(r'[A-Za-z0-9_+~.:%/-]+', unquote(path))
                or not re.fullmatch(r'[A-Za-z0-9_+~.%/-]+\.deb', path)):
            raise ValueError('Unsafe Ubuntu pool path')
        if (not re.fullmatch(r'[a-z0-9][A-Za-z0-9_+~.%:-]*\.deb', filename)
                or filename in names):
            raise ValueError('Unsafe or repeated APT cache filename')
        if not re.fullmatch(r'[1-9][0-9]*', size):
            raise ValueError('APT payload size must be positive')
        if not re.fullmatch(r'SHA256:[0-9a-f]{64}|SHA512:[0-9a-f]{128}', digest):
            raise ValueError('APT payload requires an authenticated SHA256 or SHA512')
        names.add(filename)
        rows.append((uri, filename, int(size), digest, path))
    return rows, local_filename


def verify(path, size, digest, copy_to=None):
    # Restored actions/cache entries receive the same check as new downloads.
    # APT itself can accept a cached archive by size alone.
    algorithm, expected = digest.split(':', 1)
    fd = os.open(path, os.O_RDONLY | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(fd, 'rb') as stream:
        info = os.fstat(stream.fileno())
        if not stat.S_ISREG(info.st_mode) or info.st_nlink != 1 or info.st_size != size:
            raise ValueError(f'Invalid size or file type for {path.name}')
        hasher = hashlib.new(algorithm.lower())
        for chunk in iter(lambda: stream.read(1024 * 1024), b''):
            hasher.update(chunk)
            if copy_to is not None:
                copy_to.write(chunk)
        actual = hasher.hexdigest()
    if actual != expected:
        raise ValueError(f'{algorithm} mismatch for {path.name}')


def download(uri, target, size):
    # APT's timeout is inactivity-based: allow a healthy large payload time to
    # finish, while bounding both stalls and total time (at most ten minutes).
    total_seconds = min(600, 60 + (size + 65535) // 65536)
    result = subprocess.run([
        'curl', '--fail', '--silent', '--show-error', '--proto', '=https',
        '--connect-timeout', '30', '--max-time', str(total_seconds),
        '--speed-limit', '1024', '--speed-time', '30', '--max-filesize', str(size),
        '--output', str(target), '--write-out', '%{http_code}', uri,
    ], stdout=subprocess.PIPE, text=True)
    status = result.stdout.strip()
    if result.returncode == 0 and status == '200':
        return
    # No redirect, TLS-verification bypass, retry or integrity-error fallback.
    if ((result.returncode == 22 and status in ('404', '408', '429', '500', '502', '503', '504'))
            or result.returncode in (5, 6, 7, 18, 28)):
        raise Unavailable(f'Payload unavailable: {uri} (curl {result.returncode}, HTTP {status})')
    raise RuntimeError(f'Payload refused: {uri} (curl {result.returncode}, HTTP {status})')


def acquire(row, archives):
    uri, filename, size, digest, path = row
    destination = archives / filename
    if os.path.lexists(destination):
        verify(destination, size, digest)
        return
    # Never reuse APT's partial/ files or expose incomplete cache entries.
    with tempfile.TemporaryDirectory(prefix='.polaris-payload-', dir=archives) as directory:
        target = Path(directory) / 'payload.deb'
        try:
            download(uri, target, size)
        except Unavailable as error:
            print(str(error), file=sys.stderr)
            target.unlink(missing_ok=True)
            fallback = 'https://archive.ubuntu.com/ubuntu/' + path
            print(f'Fetching the identical pinned payload from {fallback}', flush=True)
            download(fallback, target, size)
        verify(target, size, digest)
        target.chmod(0o644)
        # Atomic publication on the same filesystem without overwriting a race.
        try:
            os.link(target, destination)
        except FileExistsError:
            verify(destination, size, digest)
        target.unlink()


@contextmanager
def cache_local_deb(local_deb, filename, archives):
    # Local artifacts have no repository authentication and must not persist in
    # the shared dependency cache. Own only this copy for this one APT call.
    if filename is None:
        yield None
        return
    destination = archives / filename
    if os.path.lexists(destination):
        raise ValueError('Local APT cache slot is already occupied')
    with tempfile.TemporaryDirectory(prefix='.polaris-local-', dir=archives) as directory:
        target = Path(directory) / 'payload.deb'
        with target.open('xb') as output:
            verify(*local_deb, copy_to=output)
        target.chmod(0o644)
        verify(target, *local_deb[1:])
        owned = target.stat()
        os.link(target, destination)  # Atomic, same filesystem, no overwrite.
        try:
            target.unlink()
            verify(destination, *local_deb[1:])
            yield destination
        finally:
            current = destination.lstat()
            if (not stat.S_ISREG(current.st_mode)
                    or (current.st_dev, current.st_ino) != (owned.st_dev, owned.st_ino)):
                raise ValueError('Replaced local APT cache entry; refusing cleanup')
            destination.unlink()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--local-deb', nargs=3, metavar=('PATH', 'SIZE', 'SHA256'))
    parser.add_argument('snapshot')
    parser.add_argument('archives', type=Path)
    parser.add_argument('apt_arguments', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    if not re.fullmatch(r'[0-9]{8}T[0-9]{6}Z', args.snapshot):
        raise ValueError('Invalid pinned snapshot timestamp')
    if not stat.S_ISDIR(args.archives.lstat().st_mode):
        raise ValueError('APT archive cache must be an existing directory, not a link')
    command = args.apt_arguments
    if command[:1] == ['--']:
        command = command[1:]
    if 'install' not in command:
        raise ValueError('The unchanged APT install arguments are required')
    local_deb = None
    if args.local_deb is not None:
        path, size, digest = args.local_deb
        if (command[command.index('install'):] != ['install', '-y', path]
                or not path.startswith('./') or not path.endswith('.deb')
                or not re.fullmatch(r'[1-9][0-9]*', size)
                or not re.fullmatch(r'[0-9a-f]{64}', digest)):
            raise ValueError('Local DEB must bind the one explicit smoke install argument')
        local_deb = (Path(path), int(size), 'SHA256:' + digest)
        verify(*local_deb)
    # An empty planning cache makes APT list *all* required payloads, including
    # restored files it would otherwise skip on size alone. It changes neither
    # authenticated lists nor installed state, preferences or dependency inputs.
    with tempfile.TemporaryDirectory(prefix='polaris-apt-plan-') as directory:
        Path(directory, 'partial').mkdir()
        plan = subprocess.run([
            'apt-get', *command, '--print-uris', '--download-only',
            '-o', f'Dir::Cache::archives={directory}',
            # print-uris otherwise forces MD5 for compatibility. This option
            # belongs only to planning, never the signed update or install.
            '-o', 'Acquire::ForceHash=SHA256',
        ], stdout=subprocess.PIPE, text=True, env=os.environ | {'LC_ALL': 'C'})
    print(plan.stdout, end='', flush=True)
    plan.check_returncode()
    rows, local_filename = parse_plan(plan.stdout, args.snapshot, local_deb)
    if local_deb is not None:
        verify(*local_deb)
    if local_filename is not None and os.path.lexists(args.archives / local_filename):
        raise ValueError('Local APT cache slot is already occupied')
    # Validate the whole plan before acquiring anything. Four bounded transfers
    # keep a snapshot outage from serializing hundreds of per-payload timeouts.
    with ThreadPoolExecutor(max_workers=4) as pool:
        list(pool.map(lambda row: acquire(row, args.archives), rows))
    if local_deb is not None:
        verify(*local_deb)
        # Stage only after remote acquisition, immediately around the unchanged
        # offline install; a local build must never become a restored cache hit.
        with cache_local_deb(local_deb, local_filename, args.archives):
            verify(*local_deb)
            install = command.index('install')
            subprocess.run([
                'apt-get', *command[:install],
                '-o', f'Dir::Cache::archives={args.archives}', '--no-download',
                *command[install:],
            ], check=True)
            verify(*local_deb)


if __name__ == '__main__':
    try:
        main()
    except (OSError, ValueError, RuntimeError, subprocess.CalledProcessError) as error:
        print(f'Pinned Ubuntu payload acquisition failed: {error}', file=sys.stderr)
        sys.exit(1)
