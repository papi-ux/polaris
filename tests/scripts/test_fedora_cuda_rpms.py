"""Locked-input and command-boundary tests, without networking or installation."""
import contextlib
import copy
import importlib.util
import io
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
SPEC = importlib.util.spec_from_file_location('cuda_rpms', ROOT / 'scripts/ci/install-fedora45-cuda.py')
cuda = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(cuda)
LOCK = ROOT / 'scripts/ci/cuda-fedora45-rpm-lock.json'


class CudaRpmInputs(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix='cuda-rpm-controls-')
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def test_complete_lock_and_public_identity(self):
        lock = cuda.load_lock(LOCK)
        self.assertEqual(len(lock['packages']), 64)
        self.assertEqual(sum(p['bytes'] for p in lock['packages']), 4071884719)
        key = ROOT / 'scripts/ci/nvidia-fedora43-cuda.pub'
        self.assertEqual(cuda.digest(key), cuda.KEY_SHA256)
        cuda.verify_key(key, self.root, run=lambda argv: 'fpr:::::::::' + cuda.FINGERPRINT + ':\n')

    def test_lock_rejects_missing_float_and_cross_repository_inputs(self):
        original = json.loads(LOCK.read_text())
        mutations = []
        for field, value in [('repository', 'https://example.invalid/'), ('toolkit', '13.2.2'),
                             ('key_fingerprint', '0' * 40), ('key_sha256', '0' * 64)]:
            changed = copy.deepcopy(original)
            changed[field] = value
            mutations.append(changed)
        for field, value in [('file', '../unlocked.rpm'), ('sha256', ''), ('bytes', 0)]:
            changed = copy.deepcopy(original)
            changed['packages'][0][field] = value
            mutations.append(changed)
        changed = copy.deepcopy(original)
        changed['packages'].pop()
        mutations.append(changed)
        for index, changed in enumerate(mutations):
            path = self.root / (str(index) + '.json')
            path.write_text(json.dumps(changed))
            with self.subTest(index=index), self.assertRaises(ValueError):
                cuda.load_lock(path)

    def test_public_key_wrong_fingerprint_or_multiple_keys_refuses(self):
        key = ROOT / 'scripts/ci/nvidia-fedora43-cuda.pub'
        for text in ['fpr:::::::::' + '0' * 40 + ':\n',
                     'fpr:::::::::' + cuda.FINGERPRINT + ':\nfpr:::::::::' + '0' * 40 + ':\n']:
            with self.subTest(text=text), self.assertRaisesRegex(ValueError, 'fingerprint'):
                cuda.verify_key(key, self.root, run=lambda argv: text)

    def test_wrong_bytes_and_symlinks_refuse_before_signature_or_install(self):
        row = {'file': 'inert.rpm', 'bytes': 4, 'sha256': '0' * 64}
        (self.root / 'inert.rpm').write_bytes(b'bad!')
        with self.assertRaisesRegex(ValueError, 'bytes do not match'):
            cuda.fetch_package(row, self.root)
        (self.root / 'inert.rpm').unlink()
        (self.root / 'inert.rpm').symlink_to(LOCK)
        with self.assertRaisesRegex(ValueError, 'bytes do not match'):
            cuda.fetch_package(row, self.root)

    def test_failed_command_keeps_its_actual_output(self):
        result = subprocess.CompletedProcess(['inert'], 7, stdout='retained first failure\n')
        output = io.StringIO()
        with mock.patch.object(cuda.subprocess, 'run', return_value=result), contextlib.redirect_stdout(output):
            with self.assertRaises(subprocess.CalledProcessError):
                cuda.command(['inert'])
        self.assertIn('retained first failure', output.getvalue())


class CudaRpmTransaction(unittest.TestCase):
    def exercise(self, signature='Header V4 RSA/SHA512 Signature, key ID 1940c73e: OK\n',
                 wrong_identity=False, fail_at=None):
        temporary = tempfile.TemporaryDirectory(prefix='cuda-rpm-transaction-')
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        row = {'name': 'cuda-nvcc-13-2', 'epoch': '0', 'version': '13.2.78',
               'release': '1', 'arch': 'x86_64', 'file': 'fixture.rpm'}
        calls = []
        def run(argv):
            argv = [str(x) for x in argv]
            calls.append(argv)
            if fail_at and fail_at in argv:
                raise subprocess.CalledProcessError(1, argv)
            if '--checksig' in argv:
                return signature
            if '-qp' in argv or '-q' in argv:
                return 'wrong identity\n' if wrong_identity else cuda.expected_identity(row) + '\n'
            return ''
        error = None
        try:
            cuda.install_verified_packages({'packages': [row]}, [root / 'fixture.rpm'], root / 'key.pub', root, run=run)
        except (ValueError, subprocess.CalledProcessError) as caught:
            error = caught
        return calls, error

    def assert_no_system_effect(self, calls):
        self.assertFalse(any(c[0] == 'dnf' for c in calls))
        self.assertFalse(any(c[:2] == ['rpmkeys', '--import'] for c in calls))

    def test_unsigned_digest_only_success_still_refuses(self):
        calls, error = self.exercise(signature='Header SHA256 digest: OK\nPayload SHA256 digest: OK\n')
        self.assertIsInstance(error, ValueError)
        self.assert_no_system_effect(calls)

    def test_unknown_key_or_failed_signature_refuses(self):
        for options in [{'signature': 'Header RSA Signature: OK\nNOKEY\n'}, {'fail_at': '--checksig'}]:
            calls, error = self.exercise(**options)
            self.assertIsNotNone(error)
            self.assert_no_system_effect(calls)

    def test_wrong_rpm_identity_refuses_even_with_valid_signature(self):
        calls, error = self.exercise(wrong_identity=True)
        self.assertIsInstance(error, ValueError)
        self.assert_no_system_effect(calls)

    def test_test_transaction_failure_prevents_real_install(self):
        calls, error = self.exercise(fail_at='--setopt=tsflags=test')
        self.assertIsInstance(error, subprocess.CalledProcessError)
        self.assertEqual(sum(c[0] == 'dnf' for c in calls), 1)

    def test_verified_local_pins_test_then_install_with_enforcement(self):
        calls, error = self.exercise()
        self.assertIsNone(error)
        dnfs = [c for c in calls if c[0] == 'dnf']
        self.assertEqual(len(dnfs), 2)
        self.assertEqual(dnfs[0][1], '--setopt=tsflags=test')
        self.assertEqual(dnfs[0][:1] + dnfs[0][2:], dnfs[1])
        self.assertTrue(all('--setopt=localpkg_gpgcheck=true' in c for c in dnfs))
        self.assertTrue(all('--setopt=install_weak_deps=false' in c for c in dnfs))
        self.assertFalse(any('gpgcheck=false' in ' '.join(c) or '--nosignature' in c for c in calls))
        checked = next(i for i, c in enumerate(calls) if '--checksig' in c)
        imported = next(i for i, c in enumerate(calls) if c[:2] == ['rpmkeys', '--import'])
        self.assertLess(checked, imported)


if __name__ == '__main__':
    unittest.main()
