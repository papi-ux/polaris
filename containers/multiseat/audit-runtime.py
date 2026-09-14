#!/usr/bin/env python3
"""Scan exact runtime artifacts and declared dependencies before signing."""
import argparse
from collections import Counter
from datetime import datetime, timezone, timedelta
import hashlib
import json
import os
from pathlib import Path
import re
import subprocess

from oci_archive import verify_archive

HERE = Path(__file__).resolve().parent


def checksum(path):
    if path.is_symlink() or not path.is_file():
        raise ValueError('audit input must be a regular file')
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def subject(directory, revision):
    artifact = json.loads((directory / 'artifact.json').read_text())
    if not re.fullmatch(r'[0-9a-f]{40}', revision) or artifact['source_revision'] != revision:
        raise ValueError('artifact source differs from the requested revision')
    if artifact['schema'] != 1 or artifact['platform'] != 'linux/amd64' or artifact['development']:
        raise ValueError('expected a built Linux runtime artifact')
    for name in ('worker.oci.tar', 'sbom.cdx.json', 'providers.json', 'packages.tsv'):
        record = artifact['files'][name]
        path = directory / name
        if checksum(path) != record['sha256'] or path.stat().st_size != record['bytes']:
            raise ValueError('artifact input hash or size mismatch: ' + name)
    if verify_archive(directory / 'worker.oci.tar', artifact['worker_config_digest']) != artifact['worker_digest']:
        raise ValueError('artifact manifest differs from the exported image')
    return artifact


def assess(artifact, inventory, reports, scanners, now=None):
    now = now or datetime.now(timezone.utc)
    if set(reports) != {'image', 'declared'}:
        raise ValueError('both image and declared dependency scans are required')
    if (inventory['descriptor']['name'] != 'syft' or
            inventory['descriptor']['version'] != scanners['syft']['version'] or
            inventory['source']['type'] != 'image' or
            inventory['source']['metadata']['imageID'] != artifact['worker_config_digest'] or
            inventory['source']['metadata']['manifestDigest'] != artifact['worker_digest'] or
            not inventory['artifacts']):
        raise ValueError('image inventory does not identify the scanned runtime')
    counts = {}
    databases = []
    for scope, report in reports.items():
        descriptor = report['descriptor']
        status = descriptor['db']['status']
        built = datetime.fromisoformat(status['built'].replace('Z', '+00:00'))
        if (descriptor['name'] != 'grype' or descriptor['version'] != scanners['grype']['version'] or
                status['valid'] is not True or not timedelta(0) <= now - built <= timedelta(days=5)):
            raise ValueError('scan used an unexpected scanner or invalid/stale database')
        if report.get('ignoredMatches'):
            raise ValueError('audit must retain all vulnerability matches')
        counts[scope] = dict(Counter(match['vulnerability']['severity'] for match in report['matches']))
        databases.append({key: status[key] for key in ('schemaVersion', 'built', 'from')})
    if len(databases) != 2 or databases[0] != databases[1]:
        raise ValueError('image and declared dependency scans must use the same database')
    return {'result': 'blocked' if any(c.get('High', 0) or c.get('Critical', 0) for c in counts.values()) else 'passed',
            'match_counts': counts, 'database': databases[0],
            'scope': 'image inventory and complete declared build closure; no reachability or license clearance'}


def verify_receipt(directory, revision):
    artifact = subject(directory, revision)
    evidence = directory / 'audit'
    receipt = json.loads((evidence / 'audit.json').read_text())
    scanners = json.loads((HERE / 'locks/scanners.json').read_text())
    if (receipt['schema'] != 1 or receipt['scanners'] != scanners or
            receipt['source_revision'] != revision or
            receipt['artifact_sha256'] != checksum(directory / 'artifact.json') or
            receipt['worker_digest'] != artifact['worker_digest'] or
            receipt['worker_config_digest'] != artifact['worker_config_digest']):
        raise ValueError('audit receipt differs from the artifact or scanner locks')
    for name in ('worker.oci.tar', 'sbom.cdx.json'):
        if receipt['inputs'][name] != checksum(directory / name):
            raise ValueError('audit input was substituted')
    for name in ('image-sbom.syft.json', 'image-sbom.cdx.json', 'image-vulnerabilities.json', 'declared-vulnerabilities.json'):
        if receipt['outputs'][name] != checksum(evidence / name):
            raise ValueError('audit output was substituted')
    inventory = json.loads((evidence / 'image-sbom.syft.json').read_text())
    reports = {scope: json.loads((evidence / (scope + '-vulnerabilities.json')).read_text())
               for scope in ('image', 'declared')}
    result = assess(artifact, inventory, reports, scanners)
    if any(receipt[key] != value for key, value in result.items()) or result['result'] != 'passed':
        raise ValueError('audit is blocked or its summary was changed')
    return receipt


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('artifact', type=Path)
    parser.add_argument('--source-revision', required=True)
    parser.add_argument('--cache', type=Path)
    parser.add_argument('--output', type=Path)
    parser.add_argument('--verify', action='store_true', help='recheck existing audit bytes and database age before signing')
    parser.add_argument('--offline', action='store_true', help='require cached pinned tools and a current database')
    args = parser.parse_args()
    if args.verify:
        verify_receipt(args.artifact.resolve(), args.source_revision)
        print('Verified runtime audit for ' + args.source_revision)
        return 0
    if args.cache is None or args.output is None:
        parser.error('scanning requires --cache and --output')
    directory, cache, destination = (path.resolve() for path in (args.artifact, args.cache, args.output))
    artifact = subject(directory, args.source_revision)
    for path in (directory, cache, destination):
        if any(character in str(path) for character in (',', '\n', '\r')):
            parser.error('unsupported mount path')
    # A fresh output directory avoids a previous success receipt surviving failure.
    destination.mkdir(parents=True, exist_ok=False)
    cache.mkdir(parents=True, exist_ok=True)
    temporary = destination / 'temporary'; temporary.mkdir()
    scanners = json.loads((HERE / 'locks/scanners.json').read_text())
    if scanners['schema'] != 1 or scanners['platform'] != 'linux/amd64':
        raise ValueError('unsupported scanner lock')
    for entry in (scanners['syft'], scanners['grype']):
        if not re.fullmatch(r'docker.io/anchore/(syft|grype)@sha256:[0-9a-f]{64}', entry['reference']):
            raise ValueError('scanner must be pinned by digest')
        subprocess.run(['docker', 'image', 'inspect', entry['reference']] if args.offline else
                       ['docker', 'pull', '--platform=linux/amd64', entry['reference']],
                       check=True, stdout=subprocess.DEVNULL)
    common = ['docker', 'run', '--rm', '--read-only', '--platform=linux/amd64',
              '--user=' + str(os.getuid()) + ':' + str(os.getgid()), '--cap-drop=all',
              '--security-opt=no-new-privileges', '--cpus=2', '--memory=4g',
              '--env', 'SYFT_CHECK_FOR_APP_UPDATE=false', '--env', 'GRYPE_CHECK_FOR_APP_UPDATE=false',
              '--env', 'GRYPE_DB_CACHE_DIR=/cache', '--env', 'GRYPE_DB_AUTO_UPDATE=false',
              '--volume', str(HERE / 'scanner-config.yaml') + ':/scanner.yaml:ro,z']

    def invoke(tool, arguments, mounts, log, network=False):
        command = common + ([] if network else ['--network=none'])
        for source, target, mode in mounts:
            command += ['--volume', str(source) + ':' + target + ':' + mode]
        with (destination / log).open('wb') as output:
            subprocess.run(command + [scanners[tool]['reference'], '--config', '/scanner.yaml'] + arguments,
                           check=True, stdout=output, stderr=subprocess.STDOUT)

    if not args.offline:
        invoke('grype', ['db', 'update'], [(cache, '/cache', 'Z')], 'database-update.log', network=True)
    invoke('syft', ['scan', 'oci-archive:/input/worker.oci.tar', '--scope', 'squashed',
                   '-o', 'syft-json=/out/image-sbom.syft.json', '-o', 'cyclonedx-json=/out/image-sbom.cdx.json'],
           [(directory / 'worker.oci.tar', '/input/worker.oci.tar', 'ro,z'),
            (temporary, '/tmp', 'Z'), (destination, '/out', 'Z')], 'inventory.log')
    reports = {}
    for scope, input_path in [('image', destination / 'image-sbom.syft.json'),
                              ('declared', directory / 'sbom.cdx.json')]:
        # Only stdout contains the scan JSON; diagnostics have their own file.
        command = common + ['--network=none', '--volume', str(cache) + ':/cache:ro,z',
                  '--volume', str(input_path) + ':/input/sbom.json:ro,z',
                  scanners['grype']['reference'], '--config', '/scanner.yaml', 'sbom:/input/sbom.json', '-o', 'json']
        with (destination / (scope + '-vulnerabilities.json')).open('wb') as output, (destination / (scope + '.log')).open('wb') as log:
            subprocess.run(command, stdout=output, stderr=log, check=True)
        reports[scope] = json.loads((destination / (scope + '-vulnerabilities.json')).read_text())
    inventory = json.loads((destination / 'image-sbom.syft.json').read_text())
    report = assess(artifact, inventory, reports, scanners)
    # Recheck original bytes after scanners have exited.
    subject(directory, args.source_revision)
    report.update(schema=1, source_revision=args.source_revision,
                  worker_digest=artifact['worker_digest'], worker_config_digest=artifact['worker_config_digest'],
                  artifact_sha256=checksum(directory / 'artifact.json'), scanners=scanners,
                  inputs={name: checksum(directory / name) for name in ('worker.oci.tar', 'sbom.cdx.json')},
                  outputs={name: checksum(destination / name) for name in
                           ('image-sbom.syft.json', 'image-sbom.cdx.json', 'image-vulnerabilities.json', 'declared-vulnerabilities.json')})
    (destination / 'audit.json').write_text(json.dumps(report, indent=2) + '\n')
    print(json.dumps({'result': report['result'], 'match_counts': report['match_counts']}))
    return 0 if report['result'] == 'passed' else 1


if __name__ == '__main__':
    raise SystemExit(main())
