#!/usr/bin/env python3
"""Retain vendored Rust license texts and their declared metadata in the image."""
import hashlib
import json
from pathlib import Path
import shutil
import tomllib


def collect(source, destination):
    records = []
    for directory in sorted((source / 'vendor').iterdir()):
        if directory.is_symlink() or not directory.is_dir():
            raise ValueError('vendored package must be a directory')
        package = tomllib.loads((directory / 'Cargo.toml').read_text())['package']
        notices = []
        for path in sorted(directory.rglob('*')):
            relative = path.relative_to(directory)
            # Some packages put their notices in a LICENSES directory.
            if not any(part.upper().startswith(('LICENSE', 'LICENCE', 'COPYING', 'COPYRIGHT', 'NOTICE'))
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
        records.append({'name': package['name'], 'version': package['version'],
                        'declared_license': package.get('license'),
                        'declared_license_file': package.get('license-file'),
                        'notices': notices})
    destination.mkdir(parents=True, exist_ok=True)
    (destination / 'index.json').write_text(json.dumps({'schema': 1,
        'scope': 'complete vendored plugin dependencies, including build and development inputs',
        'packages': records}, indent=2) + '\n')


if __name__ == '__main__':
    collect(Path('/src'), Path('/plugin-notices'))
