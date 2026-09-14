#!/usr/bin/env python3
"""Retain vendored Rust license texts and their declared metadata in the image."""
import hashlib
import json
from pathlib import Path
import shutil
import tomllib


def collect(source, destination, supplements=None):
    records = []
    extra = {}
    if supplements is not None:
        index = json.loads((supplements / 'index.json').read_text())
        if index['schema'] != 1:
            raise ValueError('unsupported supplemental license index')
        for entry in index['packages']:
            if entry['package'] in extra:
                raise ValueError('duplicate supplemental package')
            extra[entry['package']] = entry
    for directory in sorted((source / 'vendor').iterdir()):
        if directory.is_symlink() or not directory.is_dir():
            raise ValueError('vendored package must be a directory')
        package = tomllib.loads((directory / 'Cargo.toml').read_text())['package']
        notices = []
        for path in sorted(directory.rglob('*')):
            relative = path.relative_to(directory)
            # Some packages put their notices in a LICENSES directory.
            if not any(part.upper().startswith(('LICENSE', 'LICENCE', 'COPYING', 'COPYRIGHT', 'NOTICE', 'AUTHORS'))
                       for part in relative.parts):
                continue
            if path.is_symlink():
                raise ValueError('license notice must not be a symlink')
            if not path.is_file():
                continue
            target = destination / directory.name / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(path, target)
            notices.append({'path': str(target.relative_to(destination)),
                            'sha256': hashlib.sha256(target.read_bytes()).hexdigest()})
        entry = extra.pop(directory.name, None)
        if entry is not None:
            vcs = json.loads((directory / '.cargo_vcs_info.json').read_text())
            if vcs['git']['sha1'] != entry['vcs_revision']:
                raise ValueError('supplemental notices belong to another source revision')
            for notice in entry['files']:
                relative = Path(notice['path'])
                if relative.is_absolute() or '..' in relative.parts:
                    raise ValueError('unsafe supplemental notice path')
                original = supplements / directory.name / relative
                if original.is_symlink() or not original.is_file():
                    raise ValueError('supplemental notice must be a regular file')
                data = original.read_bytes()
                if hashlib.sha256(data).hexdigest() != notice['sha256']:
                    raise ValueError('supplemental notice checksum mismatch')
                target = destination / directory.name / relative
                if target.exists():
                    raise ValueError('supplemental notice would replace a vendored file')
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_bytes(data)
                notices.append({'path': str(target.relative_to(destination)),
                                'sha256': notice['sha256'], 'source_url': notice['url'],
                                'source_revision': entry['vcs_revision']})
        records.append({'name': package['name'], 'version': package['version'],
                        'declared_license': package.get('license'),
                        'declared_license_file': package.get('license-file'),
                        'notices': notices})
    if extra:
        raise ValueError('supplemental package no longer belongs to the vendor lock')
    destination.mkdir(parents=True, exist_ok=True)
    (destination / 'index.json').write_text(json.dumps({'schema': 1,
        'scope': 'complete vendored plugin dependencies, including build and development inputs',
        'packages': records}, indent=2) + '\n')


if __name__ == '__main__':
    collect(Path('/src'), Path('/plugin-notices'), Path('/plugin-license-supplements'))
