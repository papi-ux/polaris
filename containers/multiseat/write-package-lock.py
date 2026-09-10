#!/usr/bin/env python3
"""Turn resolver evidence into a hash-pinned, architecture-specific package lock."""
import argparse
import hashlib
import json
import pathlib
import shlex


def package_lock(root, role):
    uris = {}
    for line in (root / (role + '.uris')).read_text().splitlines():
        if line.startswith("'"):
            fields = shlex.split(line)
            uris[fields[1]] = fields[0]
    entries = []
    lines = (root / (role + '.manifest')).read_text().splitlines()
    if len(lines) % 3:
        raise ValueError('incomplete package manifest')
    for offset in range(0, len(lines), 3):
        filename = lines[offset]
        name, version, architecture = lines[offset + 1].split('\t')
        digest = lines[offset + 2].split()[0]
        path = root / role / filename
        if hashlib.sha256(path.read_bytes()).hexdigest() != digest:
            raise ValueError('package bytes changed: ' + filename)
        entries.append(dict(name=name, version=version, architecture=architecture,
                            filename=filename, sha256=digest, url=uris[filename]))
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
                  snapshot='https://snapshot.ubuntu.com/ubuntu/20260120T000000Z/',
                  source_manifest_sha256=hashlib.sha256((args.input_directory / 'source-packages.tsv').read_bytes()).hexdigest(),
                  runtime=package_lock(args.input_directory, 'runtime'),
                  build=package_lock(args.input_directory, 'build'))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + '\n')


if __name__ == '__main__':
    main()
