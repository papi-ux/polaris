#!/usr/bin/env python3
"""Turn resolver evidence into a hash-pinned, architecture-specific package lock."""
import argparse
import hashlib
import json
import pathlib
import shlex


def package_lock(root, role):
    uris = {}
    local_heroic = None
    for line in (root / (role + '.uris')).read_text().splitlines():
        if line.startswith("'"):
            fields = shlex.split(line)
            if fields[0] == 'file:/launchers/heroic.deb':
                local_heroic = fields[1]
                continue
            url = fields[0].replace('http://snapshot.ubuntu.com/', 'https://snapshot.ubuntu.com/')
            if not url.startswith('https://') or fields[1] in uris:
                raise ValueError('unsupported or duplicate resolver URI')
            uris[fields[1]] = url
    heroic = json.loads(pathlib.Path(__file__).with_name('locks').joinpath('launchers.json').read_text())['heroic']
    if local_heroic:
        if local_heroic != heroic['filename']:
            raise ValueError('Heroic cache filename differs from its package identity')
        uris[local_heroic] = heroic['url']
    entries = []
    seen = set()
    lines = (root / (role + '.manifest')).read_text().splitlines()
    if len(lines) % 3:
        raise ValueError('incomplete package manifest')
    for offset in range(0, len(lines), 3):
        filename = lines[offset]
        if pathlib.Path(filename).name != filename or filename not in uris or filename in seen:
            raise ValueError('unexpected or duplicate package in resolver manifest')
        seen.add(filename)
        name, version, architecture = lines[offset + 1].split('\t')
        digest = lines[offset + 2].split()[0]
        path = root / role / filename
        if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise ValueError('package bytes changed: ' + filename)
        if filename == heroic['filename'] and any([name != heroic['name'], version != heroic['version'],
                                              architecture != heroic['architecture'], digest != heroic['sha256']]):
            raise ValueError('Heroic input differs from its publisher lock')
        entries.append(dict(name=name, version=version, architecture=architecture,
                            filename=filename, sha256=digest, url=uris[filename]))
    if len(entries) != len(uris) or {p['filename'] for p in entries} != set(uris):
        raise ValueError('package manifest does not match the resolved dependency closure')
    return sorted(entries, key=lambda package: (package['name'], package['architecture']))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument('profile')
    parser.add_argument('input_directory', type=pathlib.Path)
    parser.add_argument('output', type=pathlib.Path)
    args = parser.parse_args()
    lock_path = pathlib.Path(__file__).with_name('images.lock.json')
    profile = next(p for p in json.loads(lock_path.read_text())['runtime_profiles'] if p['id'] == args.profile)
    result = dict(schema=1, profile=args.profile, platform='linux/amd64',
                  source_root=profile['reference'],
                  snapshot='https://snapshot.ubuntu.com/ubuntu/20260918T000000Z/',
                  source_manifest_sha256=hashlib.sha256((args.input_directory / 'source-packages.tsv').read_bytes()).hexdigest(),
                  runtime=package_lock(args.input_directory, 'runtime'),
                  build=package_lock(args.input_directory, 'build'))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    main()
