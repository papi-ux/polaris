#!/usr/bin/env python3
"""Install the locked NVIDIA toolkit in a disposable Fedora 45 build container.

NVIDIA's Fedora 45 repository has no toolkit. These are NVIDIA's Fedora 43
CUDA 13.2.1 packages, matching pc-papi's toolkit on Fedora 45. This is a build
dependency, not a driver installation or a supported-platform claim by NVIDIA.
Every NVIDIA package is local, hash/identity/signature verified, and version
locked; the NVIDIA network repository is never enabled in DNF's resolver.
"""
import argparse
import concurrent.futures
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess
import tempfile
import urllib.request

REPOSITORY = 'https://developer.download.nvidia.com/compute/cuda/repos/fedora43/x86_64/'
FINGERPRINT = '8033132AA30E535FF7F6059764E003B61940C73E'
KEY_SHA256 = '95272017d499bb2380fd31f088c0bd74a8ebc00d2f14193dbdcf4438b3fddd1b'
HERE = Path(__file__).resolve().parent


def digest(path):
    h = hashlib.sha256()
    with path.open('rb') as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b''):
            h.update(block)
    return h.hexdigest()


def load_lock(path):
    lock = json.loads(path.read_text())
    if (lock.get('schema') != 1 or lock.get('toolkit') != '13.2.1'
            or lock.get('build_release') != '45' or lock.get('repository') != REPOSITORY
            or lock.get('key_fingerprint') != FINGERPRINT or lock.get('key_sha256') != KEY_SHA256):
        raise ValueError('unreviewed CUDA repository, toolkit or signing identity')
    rows = lock.get('packages', [])
    if len(rows) != 64 or len({p['name'] for p in rows}) != len(rows):
        raise ValueError('the complete 64-package NVIDIA toolkit lock is required')
    for p in rows:
        if (not re.fullmatch(r'[A-Za-z0-9_.+-]+\.rpm', p['file'])
                or not re.fullmatch(r'[a-f0-9]{64}', p['sha256'])
                or p['arch'] not in ('x86_64', 'noarch') or p['epoch'] != '0'
                or not isinstance(p['bytes'], int) or p['bytes'] <= 0
                or not all(re.fullmatch(r'[A-Za-z0-9_.+-]+', p[k])
                           for k in ('name', 'version', 'release'))):
            raise ValueError('incomplete or unsafe NVIDIA package pin')
    for name, version in (('cuda-toolkit-13-2', '13.2.1'), ('cuda-nvcc-13-2', '13.2.78')):
        if not any(p['name'] == name and p['version'] == version and p['release'] == '1' for p in rows):
            raise ValueError('the reviewed CUDA 13.2.1 toolkit/nvcc build is required')
    if any(p['name'].startswith(('nvidia-driver', 'cuda-drivers', 'cuda-runtime'))
           or p['name'] in ('cuda', 'cuda-13-2') for p in rows):
        raise ValueError('driver installation is outside this toolkit recipe')
    return lock


def fetch_package(row, cache):
    path = cache / row['file']
    if not path.exists():
        # Exclusive creation and one download: a partial failed transfer stays a
        # failure; a later invocation cannot silently resume or accept it.
        with urllib.request.urlopen(REPOSITORY + row['file'], timeout=120) as response, path.open('xb') as out:
            for block in iter(lambda: response.read(1024 * 1024), b''):
                out.write(block)
    if (path.is_symlink() or not path.is_file() or path.stat().st_size != row['bytes']
            or digest(path) != row['sha256']):
        raise ValueError('CUDA package bytes do not match lock: ' + row['file'])
    return path


def command(argv):
    print('+ ' + ' '.join(str(x) for x in argv), flush=True)
    result = subprocess.run([str(x) for x in argv], text=True, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    print(result.stdout, end='', flush=True)
    result.check_returncode()
    return result.stdout


def verify_key(key, gnupg_home, run=command):
    if key.is_symlink() or digest(key) != KEY_SHA256:
        raise ValueError('NVIDIA public key bytes do not match the reviewed key')
    text = run(['gpg', '--batch', '--homedir', gnupg_home, '--with-colons', '--show-keys', key])
    fingerprints = [line.split(':')[9] for line in text.splitlines() if line.startswith('fpr:')]
    if fingerprints != [FINGERPRINT]:
        raise ValueError('NVIDIA public key fingerprint is not the reviewed identity')


def expected_identity(row):
    return '\t'.join(row[k] for k in ('name', 'epoch', 'version', 'release', 'arch'))


def install_verified_packages(lock, paths, key, work, run=command):
    private_db = work / 'rpmdb'
    private_db.mkdir(mode=0o700)
    run(['rpm', '--dbpath', private_db, '--initdb'])
    run(['rpmkeys', '--dbpath', private_db, '--import', key])
    # A private database containing ONLY the pinned NVIDIA key makes host trust
    # irrelevant; no system import or transaction is attempted on a refusal.
    for row, path in zip(lock['packages'], paths):
        signature = run(['rpmkeys', '--dbpath', private_db, '--define', '_pkgverify_level all',
                         '--checksig', '--verbose', path])
        # Some rpmkeys versions can return success for an unsigned digest-only
        # result. Require a positive signature line as well as command success.
        if (not re.search(r'(?im)^\s*.*\bsignature\b.*:\s*OK\s*$', signature)
                or re.search(r'(?i)NOKEY|NOT OK|NOTTRUSTED|BAD SIGNATURE', signature)):
            raise ValueError('CUDA package has no verified NVIDIA signature: ' + row['file'])
        actual = run(['rpm', '--dbpath', private_db, '-qp', '--qf',
                      '%{NAME}\t%{EPOCHNUM}\t%{VERSION}\t%{RELEASE}\t%{ARCH}\n', path])
        if actual.strip() != expected_identity(row):
            raise ValueError('CUDA package identity does not match lock: ' + row['file'])
    run(['rpmkeys', '--import', key])
    # No NVIDIA repo is enabled: every vendor dependency is in this explicit
    # local list. Only ordinary OS dependencies resolve from Fedora's repos.
    transaction = ['dnf', '--assumeyes', '--setopt=localpkg_gpgcheck=true',
                   '--setopt=install_weak_deps=false', 'install', *paths]
    run(transaction[:1] + ['--setopt=tsflags=test'] + transaction[1:])
    run(transaction)
    for row in lock['packages']:
        actual = run(['rpm', '-q', '--qf',
                      '%{NAME}\t%{EPOCHNUM}\t%{VERSION}\t%{RELEASE}\t%{ARCH}\n', row['name']])
        if actual.strip() != expected_identity(row):
            raise ValueError('installed CUDA package drifted from lock: ' + row['name'])


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cache', type=Path, required=True)
    args = parser.parse_args()
    release = dict(line.rstrip().split('=', 1) for line in Path('/etc/os-release').read_text().splitlines()
                   if '=' in line)
    if release.get('ID', '').strip('"') != 'fedora' or release.get('VERSION_ID', '').strip('"') != '45':
        raise ValueError('the locked RPM route is only for the Fedora 45 build container')
    workspace = Path(os.environ['GITHUB_WORKSPACE']).resolve()
    cuda = workspace / '.cuda'
    if cuda.exists() or cuda.is_symlink():
        raise ValueError('a fresh CUDA install is required; .cuda already exists')
    for compiler in ('gcc-15', 'g++-15'):
        if command([compiler, '-dumpfullversion']).strip().split('.')[0] != '15':
            raise ValueError('CUDA build requires the reviewed GCC 15 compiler')
    lock = load_lock(HERE / 'cuda-fedora45-rpm-lock.json')
    args.cache.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix='polaris-cuda-rpm-verify-') as temporary:
        work = Path(temporary)
        gpg_home = work / 'gnupg'
        gpg_home.mkdir(mode=0o700)
        key = HERE / 'nvidia-fedora43-cuda.pub'
        verify_key(key, gpg_home)
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            paths = list(pool.map(lambda row: fetch_package(row, args.cache), lock['packages']))
        install_verified_packages(lock, paths, key, work)
    installed = Path('/usr/local/cuda-13.2')
    if not (installed / 'bin/nvcc').is_file():
        raise ValueError('locked toolkit installed without nvcc')
    version = command([installed / 'bin/nvcc', '--version'])
    if 'release 13.2, V13.2.78' not in version:
        raise ValueError('installed nvcc is not the reviewed CUDA build')
    cuda.symlink_to(installed, target_is_directory=True)
    receipt = {'toolkit': lock['toolkit'], 'repository': REPOSITORY, 'fingerprint': FINGERPRINT,
               'packages': lock['packages'], 'nvcc_version': version, 'cuda_path': str(cuda),
               'signature_checking': True, 'transaction_test': True, 'installed': True}
    with (workspace / 'CUDA-RPM-INSTALL-RECEIPT.json').open('x') as out:
        json.dump(receipt, out, indent=2)


if __name__ == '__main__':
    main()
