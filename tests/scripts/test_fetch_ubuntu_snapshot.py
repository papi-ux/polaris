"""Run the real Ubuntu install step with fake APT/curl, never host package tools."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
WORKFLOW = ROOT / '.github/workflows/build.yml'
STAMP = '20260901T000000Z'
PAYLOAD = b'!<arch>\nowned fake package payload\n'
NAME = 'xtrans-dev_1.4.0-1_all.deb'
URI = f'https://snapshot.ubuntu.com/ubuntu/{STAMP}/pool/main/x/xtrans/{NAME}'

# Every command that could install or download is replaced before Bash runs.
FAKE = r'''
import hashlib, json, os, re
from pathlib import Path
import sys
root = Path(os.environ['FAKE_APT_ROOT'])
config = json.loads((root / 'config.json').read_text())
name, args = Path(sys.argv[0]).name, sys.argv[1:]
with (root / 'calls.jsonl').open('a') as f:
    f.write(json.dumps([name, args]) + '\n')
if name == 'sudo':
    if args[0] in ('chown', 'chmod'):
        sys.exit(0)
    os.execvp(args[0], args)
if name == 'apt-cache':
    print('libcurl4-openssl-dev | 8.5.0-2ubuntu10.7 | pinned snapshot')
elif name == 'apt-get':
    if 'update' in args:
        sys.exit(0)
    if '--print-uris' in args:
        # Model APT's real size-only shortcut, including restored bad bytes.
        cache = Path([a.split('=', 1)[1] for a in args if a.startswith('Dir::Cache::archives=')][-1])
        if not config.get('plan_exit') and all((cache / n).is_file() and
                (cache / n).stat().st_size == len(bytes.fromhex(config['payload']))
                for n in config['required_cache']):
            print('All needed archives already cached')
            sys.exit(0)
        plan = config['plan']
        # Actual APT 2.8.3 print-uris defaults to MD5 for compatibility.
        if 'Acquire::ForceHash=SHA256' not in args:
            plan = re.sub(r'SHA(?:256|512):[0-9a-f]+',
                          'MD5Sum:' + hashlib.md5(bytes.fromhex(config['payload'])).hexdigest(), plan)
        print(plan)
        sys.exit(config.get('plan_exit', 0))
    if '--no-download' in args:
        for filename in config['required_cache']:
            p = root / 'cache' / filename
            if not p.is_file() or p.stat().st_size != len(bytes.fromhex(config['payload'])):
                print('Required exact payload unavailable in cache', file=sys.stderr)
                sys.exit(100)
        print('Fake install used only the acquired exact cache bytes')
        sys.exit(0)
    print('Err: snapshot payload 503 Service Unavailable', file=sys.stderr)
    sys.exit(100)
elif name == 'curl':
    location = 'snapshot' if args[-1].startswith('https://snapshot.ubuntu.com/') else 'archive'
    response = config[location]
    Path(args[args.index('--output') + 1]).write_bytes(bytes.fromhex(response['body']))
    print(response['status'], end='')
    sys.exit(response['exit'])
else:
    raise RuntimeError('Unexpected fake command: ' + name)
'''


def install_step(job):
    """Extract the existing Bash seam, without copying its acquisition logic."""
    lines = WORKFLOW.read_text().splitlines()
    start = lines.index(f'  {job}:')
    start = next(i for i in range(start, len(lines))
                 if lines[i] == '      - name: Install dependencies')
    start = next(i for i in range(start, len(lines)) if lines[i] == '        run: |') + 1
    body = []
    for line in lines[start:]:
        if line and not line.startswith('          '):
            break
        body.append(line[10:] if line else '')
    return '\n'.join(body)


class SnapshotPayloadTests(unittest.TestCase):
    def run_step(self, *, job='cpp-sanitizer-tests', plan=None, plan_exit=0,
                 snapshot=None, archive=None, cached=None, filename=NAME,
                 before=None):
        digest = hashlib.sha256(PAYLOAD).hexdigest()
        if plan is None:
            plan = f"'{URI}' {filename} {len(PAYLOAD)} SHA256:{digest}"
        success = {'status': '200', 'exit': 0, 'body': PAYLOAD.hex()}
        unavailable = {'status': '503', 'exit': 22, 'body': ''}
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'cache/partial').mkdir(parents=True)
            (root / 'cache/partial/retained.partial').write_bytes(b'original apt partial')
            (root / 'bin').mkdir()
            (root / 'runner').mkdir()
            (root / 'pipewire-client.conf').write_text('owned fixture')
            (root / 'cache/unrelated.keep').write_bytes(b'preserve unrelated cache state')
            if cached is not None:
                (root / 'cache' / filename).write_bytes(cached)
            if before is not None:
                before(root)
            config = {'plan': plan, 'plan_exit': plan_exit, 'payload': PAYLOAD.hex(),
                      'required_cache': [filename], 'snapshot': snapshot or unavailable,
                      'archive': archive or success}
            (root / 'config.json').write_text(json.dumps(config))
            for command in ('sudo', 'apt-get', 'apt-cache', 'curl'):
                p = root / 'bin' / command
                p.write_text(f'#!{sys.executable}\n' + FAKE)
                p.chmod(0o755)
            # Rebind only two host paths; all resolver/acquisition/install code is real.
            script = install_step(job).replace('/var/tmp/polaris-ubuntu-archives', str(root / 'cache'))
            script = script.replace('/usr/share/pipewire/client.conf', str(root / 'pipewire-client.conf'))
            result = subprocess.run(
                ['bash', '--noprofile', '--norc', '-e', '-o', 'pipefail', '-c', script],
                cwd=ROOT, env=os.environ | {'PATH': str(root / 'bin') + os.pathsep + os.environ['PATH'],
                                          'RUNNER_TEMP': str(root / 'runner'),
                                          'UBUNTU_APT_SNAPSHOT': STAMP, 'FAKE_APT_ROOT': str(root)},
                capture_output=True, text=True, timeout=10,
            )
            calls = [json.loads(s) for s in (root / 'calls.jsonl').read_text().splitlines()]
            files = {p.name: p.read_bytes() for p in (root / 'cache').iterdir()
                     if p.is_file() and not p.is_symlink()}
            self.assertEqual(files['unrelated.keep'], b'preserve unrelated cache state')
            self.assertEqual((root / 'cache/partial/retained.partial').read_bytes(), b'original apt partial')
            self.assertFalse(any(p.name.startswith('.polaris-payload-') for p in (root / 'cache').iterdir()))
            for name, args in calls:
                if name == 'curl':
                    self.assertIn('--proto', args)
                    self.assertEqual(args[args.index('--proto') + 1], '=https')
                    self.assertEqual(args[args.index('--max-time') + 1], '30')
                    self.assertFalse(set(args) & {'-k', '--insecure', '--retry', '--location'})
            return result, calls, files

    def test_official_fallback_keeps_exact_payload(self):
        result, calls, files = self.run_step()
        self.assertEqual(result.returncode, 0, result.stderr)
        urls = [args[-1] for name, args in calls if name == 'curl']
        self.assertEqual(urls, [URI, URI.replace(f'snapshot.ubuntu.com/ubuntu/{STAMP}',
                                               'archive.ubuntu.com/ubuntu')])
        self.assertEqual(files[NAME], PAYLOAD)

    def test_snapshot_success_does_not_contact_archive(self):
        result, calls, _ = self.run_step(snapshot={'status': '200', 'exit': 0, 'body': PAYLOAD.hex()})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual([args[-1] for name, args in calls if name == 'curl'], [URI])

    def test_both_existing_ubuntu_jobs_preserve_install_selection(self):
        for job in ('cpp-sanitizer-tests', 'ubuntu-build'):
            with self.subTest(job=job):
                result, calls, _ = self.run_step(job=job)
                self.assertEqual(result.returncode, 0, result.stderr)
                apt = [args for name, args in calls if name == 'apt-get']
                self.assertEqual(len(apt), 3)  # signed update, plan, cache-only install
                self.assertIn('Acquire::ForceHash=SHA256', apt[1])
                self.assertFalse(any('ForceHash' in arg for args in (apt[0], apt[2]) for arg in args))
                self.assertIn('--download-only', apt[1])
                self.assertIn('--no-download', apt[2])
                for args in apt[1:]:
                    self.assertIn('--allow-downgrades', args)
                    self.assertIn('libcurl4t64=8.5.0-2ubuntu10.7', args)
                    self.assertIn('libcurl4-openssl-dev=8.5.0-2ubuntu10.7', args)
                self.assertEqual(apt[1][apt[1].index('install'):][:len(apt[2][apt[2].index('install'):])],
                                 apt[2][apt[2].index('install'):])

    def test_partial_snapshot_is_discarded_before_one_fallback(self):
        result, calls, files = self.run_step(snapshot={'status': '200', 'exit': 18, 'body': PAYLOAD[:5].hex()})
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(sum(name == 'curl' for name, _ in calls), 2)
        self.assertEqual(files[NAME], PAYLOAD)

    def test_unavailable_archive_refuses_install_and_leaves_no_payload(self):
        result, calls, files = self.run_step(archive={'status': '404', 'exit': 22, 'body': ''})
        self.assertNotEqual(result.returncode, 0)
        self.assertNotIn(NAME, files)
        self.assertFalse(any(name == 'apt-get' and '--no-download' in args for name, args in calls))

    def test_integrity_failures_never_admit_bytes(self):
        for body in (PAYLOAD[:-1], PAYLOAD + b'x', b'x' * len(PAYLOAD)):
            with self.subTest(body=body):
                result, calls, files = self.run_step(archive={'status': '200', 'exit': 0, 'body': body.hex()})
                self.assertNotEqual(result.returncode, 0)
                self.assertNotIn(NAME, files)
                self.assertEqual(sum(name == 'curl' for name, _ in calls), 2)

    def test_snapshot_tamper_or_tls_failure_does_not_try_fallback(self):
        for response in ({'status': '200', 'exit': 0, 'body': (b'x' * len(PAYLOAD)).hex()},
                         {'status': '000', 'exit': 60, 'body': ''}):
            with self.subTest(response=response):
                result, calls, files = self.run_step(snapshot=response)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(sum(name == 'curl' for name, _ in calls), 1)
                self.assertNotIn(NAME, files)

    def test_invalid_or_weak_plan_is_rejected_before_any_fetch(self):
        good = f"'{URI}' {NAME} {len(PAYLOAD)} SHA256:{hashlib.sha256(PAYLOAD).hexdigest()}"
        plans = [good.replace('SHA256:' + hashlib.sha256(PAYLOAD).hexdigest(),
                              'MD5Sum:' + '0' * 32),
                 good.replace('SHA256:' + hashlib.sha256(PAYLOAD).hexdigest(),
                              'SHA1:' + '0' * 40),
                 good.replace('SHA256:', 'SHA512:'),
                 good.replace('SHA256:', ''), good.replace(NAME + ' ', '../escape.deb '),
                 good.replace('snapshot.ubuntu.com', 'foreign.example'),
                 good.replace(STAMP, '20260902T000000Z'),
                 good.replace('/pool/main/', '/pool/../main/'),
                 good.replace('/pool/main/', '/pool/%2e%2e/main/'),
                 good.replace('/pool/main/', '/pool/main%2fescape/'),
                 good.replace('https://', 'http://'),
                 good.replace(".deb'", ".deb?untrusted=1'"),
                 good.replace('snapshot.ubuntu.com', 'snapshot.ubuntu.com@foreign.example'),
                 good + '\n' + good, good + '\n' + "'foreign' malformed"]
        for plan in plans:
            with self.subTest(plan=plan):
                result, calls, files = self.run_step(plan=plan)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(any(name == 'curl' for name, _ in calls))
                self.assertNotIn(NAME, files)

    def test_failed_metadata_plan_never_downloads_or_installs(self):
        result, calls, files = self.run_step(plan_exit=100)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(any(name == 'curl' for name, _ in calls))
        self.assertNotIn(NAME, files)

    def test_valid_cache_is_reused_but_tampered_cache_fails_closed(self):
        for body, expected in [(PAYLOAD, 0), (b'x' * len(PAYLOAD), 1)]:
            with self.subTest(body=body):
                result, calls, files = self.run_step(cached=body)
                self.assertEqual(result.returncode, expected, result.stderr)
                self.assertFalse(any(name == 'curl' for name, _ in calls))
                self.assertEqual(files[NAME], body)

    def test_symlink_cache_entry_never_touches_its_target(self):
        def before(root):
            (root / 'foreign').write_bytes(b'not a cache payload')
            (root / 'cache' / NAME).symlink_to(root / 'foreign')
        result, calls, files = self.run_step(before=before)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(any(name == 'curl' for name, _ in calls))
        self.assertNotIn(NAME, files)

    def test_apt_cache_filename_can_differ_from_encoded_pool_basename(self):
        filename = 'example_1%3a1.0%2bfix_amd64.deb'
        uri = URI.replace('/xtrans/' + NAME, '/example/example_1.0%2bfix_amd64.deb')
        plan = f"'{uri}' {filename} {len(PAYLOAD)} SHA256:{hashlib.sha256(PAYLOAD).hexdigest()}"
        result, calls, files = self.run_step(plan=plan, filename=filename)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(files[filename], PAYLOAD)
        self.assertEqual([args[-1] for name, args in calls if name == 'curl'][-1],
                         uri.replace(f'snapshot.ubuntu.com/ubuntu/{STAMP}', 'archive.ubuntu.com/ubuntu'))

    def test_a_stronger_sha512_row_is_verified_with_its_own_algorithm(self):
        plan = f"'{URI}' {NAME} {len(PAYLOAD)} SHA512:{hashlib.sha512(PAYLOAD).hexdigest()}"
        result, calls, files = self.run_step(plan=plan)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(files[NAME], PAYLOAD)
        actual = [args for name, args in calls if name == 'apt-get' and '--print-uris' not in args]
        self.assertFalse(any('ForceHash' in arg for args in actual for arg in args))


if __name__ == '__main__':
    unittest.main()
