"""Source contracts and inert command-driver tests; no real signer or container."""
import contextlib
import importlib.util
import io
import json
import os
import re
from pathlib import Path
import runpy
import subprocess
import tempfile
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / '.github/workflows/build.yml'
CHECKER = ROOT / 'scripts/check-release-package-dependencies.py'
TOOLS = r'''#!/usr/bin/env python3
import json, os, sys
from pathlib import Path
name = Path(sys.argv[0]).name
with open(os.environ['COMMAND_LOG'], 'a') as out:
    out.write(json.dumps([name, *sys.argv[1:]]) + '\n')
if name == 'gpg' and '--list-secret-keys' in sys.argv:
    print('sec:u:2048:1:1111111111111111:0:0:::::sc:')
    print('fpr:::::::::1111111111111111111111111111111111111111:')
elif name == 'gpg' and '--export' in sys.argv:
    print('inert lab public key fixture')
elif name == 'rpmsign':
    with open(sys.argv[-1], 'ab') as out: out.write(b'LAB SIGNATURE FIXTURE')
elif name == 'rpm':
    print('RSA/SHA512, Key ID 1111111111111111')
elif name == 'docker' and sys.argv[1:3] == ['image', 'inspect']:
    print('fedora@sha256:' + 'a' * 64)
elif name == 'docker' and sys.argv[1] == 'run' and os.environ.get('FAIL_CONTAINER') == '1':
    sys.exit(1)
'''


class FedoraReleaseMatrix(unittest.TestCase):
    def test_fedora45_uses_locked_nvidia_rpms_and44_keeps_runfile(self):
        text = WORKFLOW.read_text()
        block = re.search(r'^  fedora-rpm-build:\n.*?(?=^  \S|\Z)', text, re.M | re.S).group()
        route = 'python3 scripts/ci/install-fedora45-cuda.py --cache "$cuda_rpm_cache"'
        self.assertIn(route, block, 'Fedora 45 needs the locked NVIDIA RPM route')
        self.assertIn('if [ "${{ matrix.fedora }}" = "45" ]; then', block)
        read = Path.read_text
        for mutation in (text.replace(route, 'true # lost RPM route', 1),
                         text.replace('if [ "${{ matrix.fedora }}" = "45" ]; then',
                                      'if [ "${{ matrix.fedora }}" = "44" ]; then', 1)):
            def substituted(path, *args, **kwargs):
                return mutation if path == WORKFLOW else read(path, *args, **kwargs)
            with self.subTest(mutation=mutation[-40:]), mock.patch.object(Path, 'read_text', substituted):
                with contextlib.redirect_stdout(io.StringIO()), self.assertRaisesRegex(AssertionError, 'Fedora 45 CUDA RPM route'):
                    runpy.run_path(str(CHECKER), run_name='__main__')

    def test_both_families_are_mandatory_version_check_inputs(self):
        spec = importlib.util.spec_from_file_location('versions', ROOT / 'scripts/ci/check-release-package-versions.py')
        module = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(module)
        for family in ('44', '45'):
            for component in ('', '-kms'):
                self.assertIn(f'Polaris{component}-fedora{family}-x86_64.rpm', module.REQUIRED)

    def test_real_static_checker_refuses_a_missing_45_lane_or_asset(self):
        text = WORKFLOW.read_text()
        self.assertIn("          - fedora: '45'", text)
        read = Path.read_text
        with contextlib.redirect_stdout(io.StringIO()):
            runpy.run_path(str(CHECKER), run_name='__main__')
        for mutation, reason in ((text.replace("          - fedora: '45'", "          - fedora: '46'", 1), 'Fedora CI strategy'),
                                 (text.replace('copy_fedora_rpms 45 release-assets/raw/fedora45', '', 1), 'Fedora release asset assembly')):
            def substituted(path, *args, **kwargs):
                return mutation if path == WORKFLOW else read(path, *args, **kwargs)
            with self.subTest(mutation=mutation[-40:]), mock.patch.object(Path, 'read_text', substituted):
                with contextlib.redirect_stdout(io.StringIO()), self.assertRaisesRegex(AssertionError, reason):
                    runpy.run_path(str(CHECKER), run_name='__main__')

    def test_fedora_commit_identity_must_come_from_the_checkout(self):
        text = WORKFLOW.read_text()
        binding = '          COMMIT="$(git -C "$GITHUB_WORKSPACE" rev-parse HEAD)"\n          export COMMIT\n'
        self.assertTrue(binding in text, "Fedora configure must export the checked-out commit")
        read = Path.read_text
        with contextlib.redirect_stdout(io.StringIO()):
            runpy.run_path(str(CHECKER), run_name='__main__')
        start = text.index('  fedora-rpm-build:')
        mutation = text[:start] + text[start:].replace(binding, '', 1)
        def substituted(path, *args, **kwargs):
            return mutation if path == WORKFLOW else read(path, *args, **kwargs)
        with mock.patch.object(Path, 'read_text', substituted):
            with contextlib.redirect_stdout(io.StringIO()), self.assertRaisesRegex(AssertionError, 'Fedora RPM commit identity'):
                runpy.run_path(str(CHECKER), run_name='__main__')

    def test_each_release_configure_exports_its_checked_out_commit(self):
        text = WORKFLOW.read_text()
        binding = '          COMMIT="$(git -C "$GITHUB_WORKSPACE" rev-parse HEAD)"\n          export COMMIT\n'
        read = Path.read_text
        for job in ('arch-build', 'ubuntu-build', 'fedora-rpm-build'):
            block = re.search(r'^  ' + job + r':\n.*?(?=^  \S|\Z)', text, re.M | re.S).group()
            with self.subTest(job=job):
                self.assertTrue(binding in block, job + ' must derive and export COMMIT')
                mutation = text.replace(block, block.replace(binding, '', 1), 1)
                def substituted(path, *args, **kwargs):
                    return mutation if path == WORKFLOW else read(path, *args, **kwargs)
                with mock.patch.object(Path, 'read_text', substituted):
                    with contextlib.redirect_stdout(io.StringIO()), self.assertRaisesRegex(AssertionError, 'commit identity'):
                        runpy.run_path(str(CHECKER), run_name='__main__')

    def test_shipped_arch_and_steamos_builds_keep_the_commit_export(self):
        read = Path.read_text
        for relative, binding in (
                ('packaging/linux/Arch/PKGBUILD', 'export COMMIT="${_commit}"'),
                ('packaging/linux/SteamOS/PKGBUILD', 'export COMMIT="${_commit}"'),
                ('scripts/ci/build-steamos-package.sh', 'export COMMIT="$REQUESTED_COMMIT"')):
            target = ROOT / relative
            original = target.read_text()
            self.assertIn(binding, original)
            mutation = original.replace(binding, '', 1)
            def substituted(path, *args, **kwargs):
                return mutation if path == target else read(path, *args, **kwargs)
            with self.subTest(path=relative), mock.patch.object(Path, 'read_text', substituted):
                with contextlib.redirect_stdout(io.StringIO()), self.assertRaisesRegex(AssertionError, 'commit identity'):
                    runpy.run_path(str(CHECKER), run_name='__main__')

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix='polaris-fedora-drivers-')
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.tools = self.root / 'bin'
        self.tools.mkdir()
        self.log = self.root / 'commands.jsonl'
        for name in ('gpg', 'gpgconf', 'rpm', 'rpmkeys', 'rpmsign', 'docker'):
            path = self.tools / name
            path.write_text(TOOLS)
            path.chmod(0o755)
        self.env = dict(os.environ, PATH=str(self.tools) + os.pathsep + os.environ['PATH'],
                        COMMAND_LOG=str(self.log), RUNNER_TEMP=str(self.root))

    def calls(self):
        return [json.loads(line) for line in self.log.read_text().splitlines()] if self.log.exists() else []

    def test_lab_smoke_signs_only_private_copies_and_keeps_uploaded_bytes(self):
        original = self.root / 'cpack'
        original.mkdir()
        for name in ('Polaris.rpm', 'Polaris-kms.rpm'):
            (original / name).write_bytes(b'original CPack bytes')
        target = self.root / 'private-smoke'
        script = ROOT / 'scripts/ci/prepare-fedora-rpm-smoke.sh'
        result = subprocess.run(['bash', str(script), str(original), str(target)], env=self.env,
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        for name in ('Polaris.rpm', 'Polaris-kms.rpm'):
            self.assertEqual((original / name).read_bytes(), b'original CPack bytes')
            self.assertTrue((target / name).read_bytes().endswith(b'LAB SIGNATURE FIXTURE'))
        self.assertEqual(sum(row[0] == 'rpmsign' for row in self.calls()), 2)
        self.assertFalse(any('PRIVATE_KEY' in ' '.join(row) for row in self.calls()))
        self.assertFalse(list(target.glob('*.gpg')))

    def release_pair(self):
        assets = self.root / 'final'
        assets.mkdir()
        for family in ('44', '45'):
            for component in ('', '-kms'):
                (assets / f'Polaris{component}-fedora{family}-x86_64.rpm').write_bytes(b'inert vendor-signed fixture')
        return assets

    def test_final_signed_pair_checks_use_both_clean_pinned_images(self):
        assets = self.release_pair()
        script = ROOT / 'scripts/ci/verify-signed-fedora-release-pairs.sh'
        result = subprocess.run(['bash', str(script), str(assets)], env=self.env,
                                capture_output=True, text=True, timeout=30)
        self.assertEqual(result.returncode, 0, result.stderr)
        calls = self.calls()
        self.assertEqual([row[-1] for row in calls if row[:2] == ['docker', 'pull']], ['fedora:44', 'fedora:45'])
        containers = [row for row in calls if row[:2] == ['docker', 'run']]
        self.assertEqual(len(containers), 2)
        for row in containers:
            self.assertTrue(any('@sha256:' in value for value in row))
            self.assertIn('localpkg_gpgcheck=true', ' '.join(row))
            self.assertIn('tsflags=test', ' '.join(row))
            self.assertNotIn('PRIVATE_KEY', ' '.join(row))

    def test_failed_final_container_check_stops_publication_path(self):
        assets = self.release_pair()
        self.env['FAIL_CONTAINER'] = '1'
        result = subprocess.run(['bash', str(ROOT / 'scripts/ci/verify-signed-fedora-release-pairs.sh'), str(assets)],
                                env=self.env, capture_output=True, text=True, timeout=30)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(sum(row[:2] == ['docker', 'run'] for row in self.calls()), 1)


if __name__ == '__main__':
    unittest.main()
