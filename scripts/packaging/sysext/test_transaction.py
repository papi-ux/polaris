#!/usr/bin/env python3
"""Real RPM transaction tests; never install into the running system RPMDB."""
import os
from pathlib import Path
import shutil
import tempfile
import unittest
from types import SimpleNamespace

from assemble import extract_payload, verify_signers, verify_package_signature
from bounded import BuildError, run
from payload import parse_cpio
from transaction import package_identity, simulate
from tree import sha256


@unittest.skipUnless(shutil.which('rpm') and shutil.which('rpmbuild'), 'requires Linux RPM tooling')
class RpmTransactionTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        cls.root = Path(cls.temporary.name)
        cls.top = cls.root / 'build'
        cls.top.mkdir()
        cls.base = cls.build('fixture-base', extra='Provides: /bin/sh\n')
        cls.dependency = cls.build('fixture-dependency')
        cls.consumer = cls.build('fixture-consumer', extra='Requires: fixture-base\nRequires: fixture-dependency\n',
                                 scripts='%post\necho ran > /fixture-script-ran\n')
        cls.replacement = cls.build('fixture-base', version='2')
        cls.obsoletes = cls.build('fixture-obsoletes', extra='Obsoletes: fixture-base\n')
        cls.obsoletes_versioned = cls.build('fixture-obsoletes-versioned', extra='Obsoletes: fixture-base < 2\n')
        cls.obsoletes_historical = cls.build('fixture-obsoletes-historical',
            extra='Obsoletes: fixture-base < 1\nObsoletes: fixture-absent < 2\n')

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    @classmethod
    def build(cls, name, version='1', extra='', scripts=''):
        spec = cls.root / (name + '-' + version + '.spec')
        spec.write_text(f'''Name: {name}
Version: {version}
Release: 1
Summary: Disposable transaction fixture
License: MIT
BuildArch: noarch
AutoReqProv: no
{extra}
%description
Fixture for an offline private RPMDB test.
%install
mkdir -p %{{buildroot}}/usr/share/{name}
echo payload > %{{buildroot}}/usr/share/{name}/data
%files
/usr/share/{name}/data
{scripts}
''')
        run(['rpmbuild', '--define', '_topdir ' + str(cls.top), '--define', '_buildhost fixture.invalid',
             '-bb', spec], cwd=cls.root, timeout=120)
        return cls.top / 'RPMS/noarch' / (name + '-' + version + '-1.noarch.rpm')

    def setUp(self):
        self.private = Path(tempfile.mkdtemp(dir=self.root, prefix='database-'))
        self.db = self.private / 'usr/share/rpm'
        self.db.mkdir(parents=True)
        self.command('rpm', '--root', self.private, '--dbpath', '/usr/share/rpm', '--initdb')
        self.command('rpm', '--noplugins', '--root', self.private, '--dbpath', '/usr/share/rpm', '--install',
                     '--justdb', '--nodeps', '--noscripts', '--notriggers', '--nosignature', self.base)
        # The source snapshot is never handed to rpm. SQLite may change the
        # private copy's SHM coordination file even during ordinary queries.
        self.source = self.private
        self.source_db = self.db
        self.before = self.db_hashes(self.source_db)
        self.private = Path(tempfile.mkdtemp(dir=self.root, prefix='simulation-'))
        self.db = self.private / 'usr/share/rpm'
        shutil.copytree(self.source_db, self.db)

    def db_hashes(self, directory):
        return {path.name: sha256(path) for path in directory.iterdir() if path.is_file()}

    def command(self, tool, *args, **kwargs):
        return run([tool, *args], cwd=self.root, **kwargs)

    def test_dependency_closure_and_missing_transitive_requirement(self):
        consumer = package_identity(self.command, self.consumer)
        self.assertIn('fixture-script-ran', consumer['scripts'])
        with self.assertRaises(BuildError):
            simulate(self.command, self.private, [self.consumer], [consumer])
        self.assertEqual(self.db_hashes(self.source_db), self.before)
        dependency = package_identity(self.command, self.dependency)
        result = simulate(self.command, self.private, [self.consumer, self.dependency], [consumer, dependency])
        self.assertTrue(result['rpm_test_passed'])
        self.assertEqual(self.db_hashes(self.source_db), self.before)
        self.assertFalse((self.private / 'fixture-script-ran').exists())
        self.assertFalse((self.private / 'usr/share/fixture-consumer').exists())

    def test_replacements_and_obsoletes_rejected(self):
        for package in (self.base, self.replacement):
            with self.subTest(package=package.name), self.assertRaises(BuildError):
                simulate(self.command, self.private, [package], [package_identity(self.command, package)])
        for package in (self.obsoletes, self.obsoletes_versioned):
            identity = package_identity(self.command, package)
            self.assertTrue(identity['obsoletes'])
            with self.subTest(package=package.name), self.assertRaises(BuildError):
                simulate(self.command, self.private, [package], [identity])
        self.assertEqual(self.db_hashes(self.source_db), self.before)

    def test_historical_obsoletes_without_target_matches_are_recorded_and_allowed(self):
        identity = package_identity(self.command, self.obsoletes_historical)
        self.assertEqual(set(identity['obsoletes']), {'fixture-base < 1', 'fixture-absent < 2'})
        result = simulate(self.command, self.private, [self.obsoletes_historical], [identity])
        self.assertTrue(result['rpm_test_passed'])
        self.assertEqual(self.db_hashes(self.source_db), self.before)
        self.assertFalse((self.private / 'usr/share/fixture-obsoletes-historical').exists())

    @unittest.skipUnless(shutil.which('rpm2archive'), 'requires RPM archive tooling')
    def test_regular_archive_tool_emits_newc_or_rejects_unsupported_format(self):
        before = set(self.root.iterdir())
        help_text = self.command('rpm2archive', '--help')['stdout']
        if b'--format' in help_text:
            archive = extract_payload(self.command, self.consumer)
            entries = parse_cpio(archive)
            self.assertEqual(entries['usr/share/fixture-consumer/data'].data, b'payload\n')
        else:
            # The portable CI runner also has older RPM versions. Verify they
            # fail closed; the Fedora builder separately requires CPIO support.
            with self.assertRaises(BuildError):
                extract_payload(self.command, self.consumer)
        self.assertEqual(set(self.root.iterdir()), before)
        self.assertEqual(self.db_hashes(self.source_db), self.before)
        self.assertFalse((self.private / 'fixture-script-ran').exists())
        self.assertFalse((self.private / 'usr/share/fixture-consumer').exists())

    @unittest.skipUnless(shutil.which('rpmsign') and shutil.which('gpg') and shutil.which('gpgconf'), 'requires RPM signature fixtures')
    def test_locked_signers_exclude_wrong_unsigned_and_ambient_keys(self):
        work = Path(tempfile.mkdtemp(dir=self.root, prefix='signers-'))
        homes, keys = [], []
        try:
            for label in ('a', 'b'):
                home = work / ('secret-' + label)
                home.mkdir(mode=0o700)
                homes.append(home)
                self.command('gpg', '--homedir', home, '--batch', '--pinentry-mode', 'loopback', '--passphrase', '',
                             '--quick-generate-key', 'Sysext fixture ' + label, 'ed25519', 'sign', '0')
                listing = self.command('gpg', '--homedir', home, '--batch', '--with-colons', '--list-keys')['stdout'].decode()
                fingerprint = next(row.split(':')[9].lower() for row in listing.splitlines() if row.startswith('fpr:'))
                key = work / (label + '.asc')
                key.write_bytes(self.command('gpg', '--homedir', home, '--batch', '--armor', '--export', fingerprint)['stdout'])
                keys.append({'path': key.name, 'sha256': sha256(key), 'fingerprint': fingerprint})
            signed = work / 'signed.rpm'
            shutil.copyfile(self.consumer, signed)
            self.command('rpmsign', '--define', '_openpgp_sign gpg', '--define', '_gpg_path ' + str(homes[0]),
                         '--define', '_gpg_name ' + keys[0]['fingerprint'], '--addsign', signed)
            ambient = work / 'ambient-db'
            self.command('rpm', '--dbpath', ambient, '--initdb')
            self.command('rpmkeys', '--dbpath', ambient, '--import', work / keys[0]['path'])
            def command(tool, *args, **kwargs):
                # Model a trusted key already present in the ambient default
                # database without modifying the builder's real RPMDB.
                defaults = ['--define', '_dbpath ' + str(ambient)] if tool in ('rpm', 'rpmkeys') else []
                return self.command(tool, *defaults, *args, **kwargs)
            frozen = SimpleNamespace(destination=work)
            for index, key in enumerate(keys):
                destination = work / ('check-' + str(index))
                destination.mkdir(mode=0o700)
                keyring = verify_signers(command, frozen, {'keys': [key]}, destination)
                if index == 0:
                    verify_package_signature(command, keyring, signed)
                else:
                    with self.assertRaises(BuildError):
                        verify_package_signature(command, keyring, signed)
                with self.assertRaises(BuildError):
                    verify_package_signature(command, keyring, self.consumer)
            wrong = work / 'wrong-fingerprint'
            wrong.mkdir()
            with self.assertRaises(BuildError):
                verify_signers(command, frozen, {'keys': [{**keys[0], 'fingerprint': keys[1]['fingerprint']}]}, wrong)
        finally:
            for home in homes:
                self.command('gpgconf', '--homedir', home, '--kill', 'all', check=False)


if __name__ == '__main__':
    unittest.main()
