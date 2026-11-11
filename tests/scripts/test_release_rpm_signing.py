"""Run the release-signing driver with inert command stand-ins, never a key."""
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCRIPT = ROOT / 'scripts/ci/sign-release-rpms.sh'
FINGERPRINT = '58017EDFFA9F803E07ED26F835F13F14FAAD15CC'
STAND_IN = r'''#!/usr/bin/env python3
import json, os, sys
from pathlib import Path
name = Path(sys.argv[0]).name
with open(os.environ['COMMAND_LOG'], 'a') as out:
    out.write(json.dumps([name, *sys.argv[1:]]) + '\n')
if name == 'gpg' and '--list-secret-keys' in sys.argv:
    print('sec:u:2048:1:35F13F14FAAD15CC:0:0:::::sc:')
    print('fpr:::::::::58017EDFFA9F803E07ED26F835F13F14FAAD15CC:')
elif name == 'rpm':
    mode = os.environ.get('SIGNATURE_MODE', 'vendor')
    print('(none)' if mode == 'missing' else 'RSA/SHA512, Key ID ' + ('0000000000000000' if mode == 'foreign' else '35f13f14faad15cc'))
elif name == 'rpmkeys' and '--checksig' in sys.argv and os.environ.get('VERIFY_FAIL') == '1':
    sys.exit(1)
'''


class ReleaseRpmSigning(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='polaris-signing-driver-')
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.assets = self.directory / 'assets'
        self.assets.mkdir()
        self.tools = self.directory / 'bin'
        self.tools.mkdir()
        self.log = self.directory / 'commands.jsonl'
        for name in ('gpg', 'gpgconf', 'rpm', 'rpmkeys', 'rpmsign'):
            path = self.tools / name
            path.write_text(STAND_IN)
            path.chmod(0o755)
        self.environment = dict(os.environ, PATH=str(self.tools) + os.pathsep + os.environ['PATH'],
                                COMMAND_LOG=str(self.log), RUNNER_TEMP=str(self.directory),
                                POLARIS_REPO_GPG_PRIVATE_KEY='inert fixture, not a key',
                                POLARIS_REPO_GPG_KEY_ID=FINGERPRINT)

    def pair(self, release):
        for component in ('', '-kms'):
            (self.assets / f'Polaris{component}-fedora{release}-x86_64.rpm').write_bytes(b'not an RPM; command stand-ins only')

    def run_driver(self):
        return subprocess.run(['bash', str(SCRIPT), str(self.assets)], env=self.environment,
                              text=True, capture_output=True, timeout=30)

    def calls(self):
        return [json.loads(line) for line in self.log.read_text().splitlines()] if self.log.exists() else []

    def test_each_present_fedora_pair_is_signed_then_independently_checked(self):
        self.pair('44')
        self.pair('45')
        result = self.run_driver()
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.calls()
        signed = [row for row in calls if row[0] == 'rpmsign']
        self.assertEqual(len(signed), 4)
        for row in signed:
            self.assertEqual(row[1:4], ['--define', '_gpg_name ' + FINGERPRINT, '--addsign'])
            self.assertTrue(any(x[0] == 'rpmkeys' and '--checksig' in x and row[-1] == x[-1] for x in calls))
        self.assertFalse(any('gh' == row[0] for row in calls))

    def test_missing_secret_refuses_before_any_signing(self):
        self.pair('44')
        self.environment.pop('POLARIS_REPO_GPG_PRIVATE_KEY')
        result = self.run_driver()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(any(row[0] == 'rpmsign' for row in self.calls()))

    def test_different_signing_identity_refuses_before_any_signing(self):
        self.pair('44')
        self.environment['POLARIS_REPO_GPG_KEY_ID'] = 'A' * 40
        result = self.run_driver()
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(any(row[0] == 'rpmsign' for row in self.calls()))

    def test_zero_exit_without_signature_is_not_signing_success(self):
        self.pair('44')
        self.environment['SIGNATURE_MODE'] = 'missing'
        result = self.run_driver()
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('signature', result.stderr.lower())

    def test_foreign_key_id_postcondition_is_refused(self):
        self.pair('44')
        self.environment['SIGNATURE_MODE'] = 'foreign'
        self.assertNotEqual(self.run_driver().returncode, 0)

    def test_missing_member_refuses_before_any_signing(self):
        (self.assets / 'Polaris-fedora45-x86_64.rpm').write_bytes(b'fixture')
        self.assertNotEqual(self.run_driver().returncode, 0)
        self.assertFalse(any(row[0] == 'rpmsign' for row in self.calls()))

    def test_native_signature_verification_failure_is_refused(self):
        self.pair('44')
        self.environment['VERIFY_FAIL'] = '1'
        self.assertNotEqual(self.run_driver().returncode, 0)

    def test_workflow_signs_before_staging_or_uploading_release_assets(self):
        workflow = (ROOT / '.github/workflows/build.yml').read_text()
        start = workflow.index('      - name: Sign the Fedora release RPMs')
        self.assertLess(start, workflow.index('      - name: Stage curated GitHub release'))
        step = workflow[start:workflow.index('\n      - name:', start + 1)]
        self.assertIn('secrets.POLARIS_REPO_GPG_PRIVATE_KEY', step)
        self.assertIn('secrets.POLARIS_REPO_GPG_KEY_ID', step)
        self.assertIn('scripts/ci/sign-release-rpms.sh release-assets/final', step)
        self.assertNotIn('continue-on-error', step)
        self.assertNotIn('        if:', step)


if __name__ == '__main__':
    unittest.main()
