#!/usr/bin/env python3
"""Build a private package-only sysext candidate from reviewed offline inputs."""
from __future__ import annotations

import argparse
import datetime
import hashlib
import io
import json
import os
from pathlib import Path
import signal
import stat
import sys

from bounded import BuildError, copy_verified, require, run
import manifest as contract
from payload import MAX_PAYLOAD, merge_payloads, parse_cpio, materialize, with_metadata
from staging import Inputs, snapshot_metadata, verify_snapshot
from transaction import package_identity, simulate
from tree import inventory, sha256


def write_json(path, value):
    with Path(path).open('x', encoding='utf-8') as output:
        os.fchmod(output.fileno(), 0o600)
        json.dump(value, output, indent=2, sort_keys=True)
        output.write('\n')
        output.flush()
        os.fsync(output.fileno())


def finish_receipt(output, receipt):
    """An exclusive, durable completion marker is the last successful write."""
    temporary = output / 'receipt.pending.json'
    write_json(temporary, receipt)
    fd = os.open(output, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    linked = False
    try:
        os.link(temporary, output / 'build-receipt.json')
        linked = True
        os.fsync(fd)
        temporary.unlink()
        os.fsync(fd)
    except BaseException:
        if linked:
            (output / 'build-receipt.json').unlink()
        raise
    finally:
        os.close(fd)


class Commands:
    def __init__(self, work, tools):
        self.work, self.tools = work, tools
        self.records = []
        (work / 'commands').mkdir(mode=0o700)

    def __call__(self, tool, *args, **kwargs):
        argv = ['/' + self.tools[tool]['path'], *map(str, args)]
        check = kwargs.pop('check', True)
        result = run(argv, cwd=self.work, check=False, **kwargs)
        record = {'tool': tool, 'arguments': [str(value) for value in args], 'returncode': result['returncode']}
        for stream in ('stdout', 'stderr'):
            path = self.work / 'commands' / (str(len(self.records)) + '-' + stream + '.log')
            with path.open('xb') as dest:
                dest.write(result[stream])
            record[stream] = {'path': str(path.relative_to(self.work)), 'sha256': sha256(path)}
        self.records.append(record)
        require(not check or result['returncode'] == 0, 'command failed: ' + tool + '; see commands/' + str(len(self.records) - 1) + '-stderr.log')
        return result


def verify_tools(lock):
    root = os.open('/', os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
    try:
        for record in lock['tools'].values():
            copy_verified(root, record['path'], record['sha256'], io.BytesIO())
            info = Path('/' + record['path']).stat()
            require(info.st_uid == 0 and not info.st_mode & 0o022 and info.st_mode & 0o111, 'untrusted build tool')
        require(Path(sys.executable).resolve() == Path('/' + lock['tools']['python3']['path']), 'unexpected Python interpreter')
    finally:
        os.close(root)


def admit_output_parent(parent):
    """Every ancestor must prevent another user from replacing the output tree."""
    require(parent.is_absolute(), 'output parent must be absolute')
    fd = os.open('/', os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC)
    try:
        for index, part in enumerate(parent.parts[1:]):
            child = os.open(part, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW | os.O_CLOEXEC, dir_fd=fd)
            os.close(fd)
            fd = child
            info = os.fstat(fd)
            sticky_ancestor = bool(info.st_mode & stat.S_ISVTX) and index < len(parent.parts) - 2
            require(info.st_uid == 0 and (not info.st_mode & 0o022 or sticky_ancestor), 'untrusted output ancestor')
    finally:
        os.close(fd)


def verify_signers(command, frozen, dependencies, work):
    home = work / 'gnupg'
    home.mkdir(mode=0o700)
    keyring = work / 'signing-rpmdb'
    command('rpm', '--dbpath', keyring, '--initdb')
    for key in dependencies['keys']:
        path = frozen.destination / key['path']
        result = command('gpg', '--homedir', home, '--batch', '--no-options', '--with-colons',
                         '--import-options', 'show-only', '--import', path)['stdout'].decode('utf-8')
        rows = [line.split(':') for line in result.splitlines()]
        require(sum(row[0] == 'pub' for row in rows) == 1, 'signing key bundle must have one primary key')
        fingerprints = [row[9].lower() for row in rows if row[0] == 'fpr' and len(row) > 9]
        require(fingerprints and fingerprints[0] == key['fingerprint'], 'signer fingerprint mismatch')
        command('rpmkeys', '--dbpath', keyring, '--import', path)
    return keyring


def assemble(args):
    require(sys.platform == 'linux' and os.geteuid() == 0, 'use a disposable Linux builder as root')
    require(args.output.is_absolute() and args.output.parent.resolve() == args.output.parent,
            'output requires a canonical absolute parent')
    admit_output_parent(args.output.parent)
    os.umask(0o077)
    args.output.mkdir(mode=0o700)  # Existing output, including symlinks, is rejected.
    receipt = {'schema_version': 1, 'kind': 'polaris-sysext-package-build', 'verdict': 'FAIL',
               'captured_at_utc': datetime.datetime.now(datetime.timezone.utc).strftime('%Y-%m-%dT%H:%M:%SZ'),
               'release_ready': False, 'physical_stream_validated': False}
    frozen = None
    command = None
    try:
        frozen = Inputs(args.inputs, args.output / 'inputs')
        inputs = contract.build_manifest(frozen.json({'path': args.manifest, 'sha256': args.manifest_sha256}))
        target = contract.target_lock(frozen.json(inputs['target']))
        toolchain = contract.toolchain_lock(frozen.json(inputs['toolchain']))
        require(toolchain['builder_digest'] == args.builder_digest, 'builder digest differs from reviewed toolchain')
        verify_tools(toolchain)
        dependencies = contract.dependency_lock(frozen.json(inputs['dependencies']), target['image_digest'])
        attestation = contract.build_attestation(frozen.json(inputs['build_receipt']), inputs)
        target_files = frozen.json(target['inventory'], limit=4 * 1024 * 1024)
        packages = [frozen.copy(inputs['candidate'])]
        for key in dependencies['keys']:
            frozen.copy({name: key[name] for name in ('path', 'sha256')}, limit=1024 * 1024)
        for item in dependencies['packages']:
            packages.append(frozen.copy({key: item[key] for key in ('path', 'sha256')}))
        private = args.output / 'target-metadata'
        snapshot_metadata(args.target_root, private, target_files, target['policy'])
        command = Commands(args.output, toolchain['tools'])
        for name, item in toolchain['tools'].items():
            identity = command('rpm', '-qf', '--qf', '%{NAME}-%{EPOCHNUM}:%{VERSION}-%{RELEASE}.%{ARCH}',
                               '/' + item['path'])['stdout'].decode('utf-8')
            require(identity == item['package'], 'build tool package identity changed: ' + name)
        keyring = verify_signers(command, frozen, dependencies, args.output)
        identities = [package_identity(command, packages[0])]
        require(identities[0]['name'] == 'polaris' and identities[0]['nevra'] == attestation['nevra'] and
                identities[0]['architecture'] == 'x86_64', 'candidate RPM identity mismatch')
        for item, path in zip(dependencies['packages'], packages[1:]):
            signature = command('rpmkeys', '--dbpath', keyring, '--checksig', path)['stdout'].decode('utf-8')
            require('signatures OK' in signature, 'dependency has no verified signature')
            identity = package_identity(command, path)
            require(all(identity[key] == item[key] for key in ('name', 'nevra', 'architecture', 'license')),
                    'dependency RPM identity mismatch')
            identities.append(identity)
        require(len({item['name'] for item in identities}) == len(identities), 'duplicate candidate/dependency name')
        receipt['transaction'] = simulate(command, private, packages, identities)
        verify_snapshot(args.target_root, target_files)
        payloads, total = [], 0
        for path in packages:
            data = command('rpm2cpio', path, stdout_limit=MAX_PAYLOAD, timeout=90)['stdout']
            total += len(data)
            require(total <= 256 * 1024 * 1024, 'combined RPM payload exceeds bound')
            payloads.append(parse_cpio(data))
        owners = {}
        for identity, entries in zip(identities, payloads):
            for name in entries:
                owners.setdefault(name, []).append(identity['nevra'])
        entries = merge_payloads(payloads)
        binary = inputs['binary']
        executable = entries.get(binary['path'])
        require(executable is not None and stat.S_ISREG(executable.mode) and executable.mode & 0o111,
                'candidate executable missing or not a regular executable')
        require(executable.data.startswith(b'\x7fELF') and hashlib.sha256(executable.data).hexdigest() == binary['sha256'],
                'candidate executable digest mismatch')
        metadata = ('ID=bazzite\nVERSION_ID=44\nARCHITECTURE=x86-64\nEXTENSION_RELOAD_MANAGER=1\n'
                    + 'POLARIS_VERSION=' + binary['version'] + '\nPOLARIS_SOURCE_COMMIT=' + inputs['source']['commit']
                    + '\nPOLARIS_SOURCE_TREE=' + inputs['source']['tree'] + '\n')
        entries = with_metadata(entries, metadata)
        tree = args.output / 'tree'
        materialize(entries, tree)
        expected = {'.': {'mode': 0o755, 'type': 'directory'}, **{name: entry.manifest() for name, entry in entries.items()}}
        require(inventory(tree) == expected, 'materialized payload differs from admitted archive')
        command('setfiles', '-F', '-r', tree, private / target['policy'], tree)
        projected = inventory(tree, labels=True)
        raw = args.output / 'polaris.raw'
        command('mksquashfs', tree, raw, '-comp', 'zstd', '-noappend', '-all-root', '-mkfs-time', '0',
                '-all-time', '0', '-no-progress', '-processors', '2', timeout=300)
        roundtrip = args.output / 'roundtrip'
        command('unsquashfs', '-xattrs', '-no-progress', '-d', roundtrip, raw, timeout=300)
        require(inventory(roundtrip, labels=True) == projected, 'SquashFS content or labels changed')
        verify_snapshot(args.target_root, target_files)
        verify_tools(toolchain)
        package_manifest = {'schema_version': 1, 'packages': identities, 'owners': owners, 'files': projected,
                            'dependency_lock_sha256': inputs['dependencies']['sha256']}
        write_json(args.output / 'package-manifest.json', package_manifest)
        with raw.open('rb') as handle:
            os.fsync(handle.fileno())
        receipt.update(verdict='PRIVATE_PACKAGE_BUILT', source=inputs['source'], target=target,
                       candidate={'path': 'polaris.raw', 'sha256': sha256(raw), 'bytes': raw.stat().st_size},
                       binary=binary, inputs=frozen.frozen, toolchain=toolchain,
                       package_manifest={'path': 'package-manifest.json', 'sha256': sha256(args.output / 'package-manifest.json')},
                       roundtrip_match=True,
                       pending=['clean-base ELF closure and non-root version execution', 'scriptlet omission runtime review',
                                'second clean build reproducibility', 'isolated extension lifecycle', 'physical hardware acceptance'])
    except BaseException as error:
        receipt['error'] = str(error)
        # No successful receipt is emitted if any assembly or verification fails.
        receipt['verdict'] = 'FAIL'
    finally:
        if frozen is not None:
            frozen.close()
        if command is not None:
            receipt['commands'] = command.records
    finish_receipt(args.output, receipt)
    print(json.dumps({'verdict': receipt['verdict'], 'receipt': str(args.output / 'build-receipt.json')}))
    return 0 if receipt['verdict'] == 'PRIVATE_PACKAGE_BUILT' else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--inputs', type=Path, required=True)
    parser.add_argument('--manifest', default='manifest.json')
    parser.add_argument('--manifest-sha256', required=True)
    parser.add_argument('--target-root', type=Path, required=True)
    parser.add_argument('--builder-digest', required=True,
                        help='immutable builder identity, independently verified by the launch operator')
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    contract.digest(args.manifest_sha256)
    def interrupted(signum, _frame):
        raise BuildError('interrupted by signal ' + str(signum))
    for signum in (signal.SIGTERM, signal.SIGHUP, signal.SIGINT):
        signal.signal(signum, interrupted)
    try:
        return assemble(args)
    except (BuildError, OSError, ValueError) as error:
        print(str(error), file=sys.stderr)
        return 1


if __name__ == '__main__':
    raise SystemExit(main())
