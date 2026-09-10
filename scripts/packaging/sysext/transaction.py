#!/usr/bin/env python3
"""RPM queries and a test-only transaction against a private target RPMDB copy."""
from __future__ import annotations

from bounded import require

IDENTITY = '%{NAME}|%{NAME}-%{EPOCHNUM}:%{VERSION}-%{RELEASE}.%{ARCH}|%{ARCH}|%{LICENSE}\n'
def package_identity(command, rpm):
    line = command('rpm', '-qp', '--qf', IDENTITY, rpm)['stdout'].decode('utf-8').strip()
    fields = line.split('|')
    require(len(fields) == 4, 'invalid RPM identity response')
    name, nevra, architecture, license_name = fields
    require(architecture in ('x86_64', 'noarch'), 'unsupported RPM architecture')
    caps = command('rpm', '-qp', '--qf', '[%{FILECAPS}\n]', rpm)['stdout'].decode('utf-8')
    require(all(value in ('', '(none)') for value in caps.splitlines()), 'RPM carries file capabilities')
    require(not command('rpm', '-qp', '--obsoletes', rpm)['stdout'].strip(), 'extension package declares replacements')
    scripts = command('rpm', '-qp', '--scripts', '--triggers', '--filetriggers', rpm)['stdout'].decode('utf-8')
    return {'name': name, 'nevra': nevra, 'architecture': architecture, 'license': license_name, 'scripts': scripts}


def database_packages(command, private):
    result = command('rpm', '--root', private, '--dbpath', '/usr/share/rpm', '-qa', '--qf', '%{NAME}|%{NAME}-%{EPOCHNUM}:%{VERSION}-%{RELEASE}.%{ARCH}\n')['stdout'].decode('utf-8')
    rows = [line.split('|') for line in result.splitlines()]
    require(all(len(row) == 2 for row in rows), 'invalid target package inventory')
    require(rows, 'empty target package inventory')
    return sorted(rows)


def simulate(command, private, packages, identities):
    before = database_packages(command, private)
    installed = {row[0] for row in before}
    require(not installed.intersection(item['name'] for item in identities), 'candidate would replace an installed base package')
    # No repository resolver and no network path. --test performs dependency,
    # conflict, file and transaction checks without installing or running scripts.
    command('rpm', '--noplugins', '--root', private, '--dbpath', '/usr/share/rpm', '--install', '--test',
            '--noscripts', '--notriggers', '--nosignature', *packages, timeout=300)
    require(database_packages(command, private) == before, 'test transaction changed private RPMDB inventory')
    return {'base_package_count': len(before), 'added_nevras': [item['nevra'] for item in identities],
            'rpm_test_passed': True, 'base_replacement_rejected': True, 'scriptlets_executed': False}
