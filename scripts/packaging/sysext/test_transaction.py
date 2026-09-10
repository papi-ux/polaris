#!/usr/bin/env python3
"""Real RPM transaction tests; never install into the running system RPMDB."""
import os
from pathlib import Path
import shutil
import tempfile
import unittest

from bounded import BuildError, run
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
        with self.assertRaises(BuildError):
            package_identity(self.command, self.obsoletes)
        self.assertEqual(self.db_hashes(self.source_db), self.before)


if __name__ == '__main__':
    unittest.main()
