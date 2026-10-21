"""Run the real Ubuntu install step with fake APT/curl, never host package tools."""
import hashlib
import json
import os
from pathlib import Path
import runpy
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch


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
if name == 'stat':
    print(Path(args[-1]).stat().st_size + config.get('binding_size_delta', 0))
    sys.exit(0)
if name == 'sha256sum':
    print(config.get('binding_digest', hashlib.sha256(Path(args[-1]).read_bytes()).hexdigest())
          + '  ' + args[-1])
    sys.exit(0)
if name == 'dpkg-deb':
    print({'Package': 'polaris-kms', 'Version': '1.5.0', 'Architecture': 'amd64',
           'Depends': 'polaris (= 1.5.0), libcap2-bin, passwd'}[args[-1]])
    sys.exit(0)
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
        if config.get('local_row', False):
            # Captured APT2.8.3 local-only schema: file:/absolute/path, cache
            # name, size, trailing empty hash. DestFile need not equal basename.
            local = next(arg for arg in args if arg.startswith('./artifacts/'))
            p = Path(local).resolve()
            row = [config.get('local_uri', 'file:' + str(p)),
                   config.get('local_cache_name', p.stem + '_1.5.0_amd64.deb'),
                   str(p.stat().st_size + config.get('local_row_size_delta', 0))]
            if 'local_row_hash' in config:
                row.append(config['local_row_hash'].replace('{SHA256}', hashlib.sha256(p.read_bytes()).hexdigest()))
            # APT always quotes the URI, even when it contains no spaces.
            local_line = "'" + row[0] + "' " + ' '.join(row[1:])
            if len(row) == 3:
                local_line += ' '
            plan = (plan + '\n' + local_line if config.get('local_row_after')
                    else local_line + '\n' + plan)
            if config.get('repeat_local_row'):
                plan = local_line + '\n' + plan
            if config.get('extra_local_row'):
                plan += "\n'file:/tmp/not-the-allowed.deb' extra_1_all.deb 1 "
        # Actual APT 2.8.3 print-uris defaults to MD5 for compatibility.
        if 'Acquire::ForceHash=SHA256' not in args:
            plan = re.sub(r'SHA(?:256|512):[0-9a-f]+',
                          'MD5Sum:' + hashlib.md5(bytes.fromhex(config['payload'])).hexdigest(), plan)
        print(plan)
        if config.get('mutate_local_during_plan'):
            p = root / 'artifacts/polaris.deb'
            p.write_bytes(b'x' * p.stat().st_size)
        sys.exit(config.get('plan_exit', 0))
    if '--no-download' in args:
        # APT's offline pass needs the local package in its canonical archive
        # cache too: merely passing an absolute local argv does not acquire it.
        if config.get('local_row', False):
            p = Path(args[-1]).resolve()
            cached = root / 'cache' / (p.stem + '_1.5.0_amd64.deb')
            if not cached.is_file() or cached.stat().st_size != p.stat().st_size:
                print("E: Internal Error, Pathname to install is not absolute '"
                      + cached.name + "'", file=sys.stderr)
                sys.exit(100)
            with (root / 'local-installs.jsonl').open('a') as f:
                f.write(json.dumps({'argument': args[-1], 'cache': cached.name,
                                    'sha256': hashlib.sha256(cached.read_bytes()).hexdigest(),
                                    'regular': cached.is_file(), 'symlink': cached.is_symlink(),
                                    'nlink': cached.stat().st_nlink,
                                    'mode': cached.stat().st_mode & 0o777}) + '\n')
        for filename in config['required_cache']:
            p = root / 'cache' / filename
            if not p.is_file() or p.stat().st_size != len(bytes.fromhex(config['payload'])):
                print('Required exact payload unavailable in cache', file=sys.stderr)
                sys.exit(100)
        if config.get('mutate_local_during_install'):
            p = Path(args[-1])
            p.write_bytes(b'x' * p.stat().st_size)
        if config.get('replace_local_cache_during_install'):
            cached.rename(root / 'retained-staged-copy.deb')
            cached.write_bytes(b'foreign replacement')
        print('Fake install used only the acquired exact cache bytes')
        sys.exit(config.get('install_exit', 0))
    print('Err: snapshot payload 503 Service Unavailable', file=sys.stderr)
    sys.exit(100)
elif name == 'curl':
    location = 'snapshot' if args[-1].startswith('https://snapshot.ubuntu.com/') else 'archive'
    response = config[location]
    if config.get('mutate_local_during_fetch'):
        p = root / 'artifacts/polaris.deb'
        p.write_bytes(b'x' * p.stat().st_size)
    if config.get('mutate_local_cache_during_fetch'):
        p = root / 'cache/polaris_1.5.0_amd64.deb'
        if not p.exists():
            p.write_bytes(b'x' * (root / 'artifacts/polaris.deb').stat().st_size)
    if (int(args[args.index('--max-time') + 1]) < response.get('seconds', 0)
            or ('--speed-time' in args and
                response.get('stall_seconds', 0) >= int(args[args.index('--speed-time') + 1]))):
        Path(args[args.index('--output') + 1]).write_bytes(b'partial')
        print('000', end='')
        sys.exit(28)
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


def local_smoke_step():
    """Use the automatic DEB smoke's actual two install calls and metadata assertions."""
    text = WORKFLOW.read_text()
    start = text.index('          snapshot_sources=', text.index('dpkg-deb --info "$deb_path"'))
    end = text.index('          helper=/usr/libexec/polaris/polaris-kms', start)
    return '\n'.join(line[10:] if line else '' for line in text[start:end].splitlines())


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
            self.assertFalse(any(p.name.startswith(('.polaris-payload-', '.polaris-local-'))
                                 for p in (root / 'cache').iterdir()))
            for name, args in calls:
                if name == 'curl':
                    self.assertIn('--proto', args)
                    self.assertEqual(args[args.index('--proto') + 1], '=https')
                    size = int(args[args.index('--max-filesize') + 1])
                    self.assertEqual(int(args[args.index('--max-time') + 1]),
                                     min(600, 60 + (size + 65535) // 65536))
                    self.assertEqual(args[args.index('--speed-limit') + 1], '1024')
                    self.assertEqual(args[args.index('--speed-time') + 1], '30')
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


class LocalDebSmokeTests(unittest.TestCase):
    def run_smoke(self, *, config_changes=None, before=None, bound_path=None, inspect=None):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for child in ('cache/partial', 'bin', 'runner/polaris-ubuntu-source-parts',
                          'runner/polaris-ubuntu-lists/partial', 'artifacts'):
                (root / child).mkdir(parents=True)
            (root / 'runner/polaris-ubuntu-snapshot.sources').write_text('owned fixture')
            for name in ('polaris', 'polaris-kms'):
                (root / 'artifacts' / (name + '.deb')).write_bytes(PAYLOAD + name.encode())
            (root / 'cache/partial/retained.partial').write_bytes(b'original apt partial')
            (root / 'cache/unrelated.keep').write_bytes(b'preserve unrelated cache state')
            if before is not None:
                before(root)
            original_cache_names = {p.name for p in (root / 'cache').iterdir()}
            for command in ('sudo', 'apt-get', 'curl', 'stat', 'sha256sum', 'dpkg-deb'):
                path = root / 'bin' / command
                path.write_text(f'#!{sys.executable}\n' + FAKE)
                path.chmod(0o755)
            plan = f"'{URI}' {NAME} {len(PAYLOAD)} SHA256:{hashlib.sha256(PAYLOAD).hexdigest()}"
            config = {'plan': plan, 'payload': PAYLOAD.hex(), 'required_cache': [NAME], 'local_row': True,
                      'snapshot': {'status': '503', 'exit': 22, 'body': ''},
                      'archive': {'status': '200', 'exit': 0, 'body': PAYLOAD.hex()}}
            config.update(config_changes or {})
            (root / 'config.json').write_text(json.dumps(config))
            script = local_smoke_step().replace('/var/tmp/polaris-ubuntu-archives', str(root / 'cache'))
            script = script.replace('scripts/ci/fetch-ubuntu-snapshot.py',
                                    str(ROOT / 'scripts/ci/fetch-ubuntu-snapshot.py'))
            if bound_path is not None:
                script = script.replace('--local-deb "./$deb_path"', f'--local-deb "{bound_path}"')
            result = subprocess.run(
                ['bash', '--noprofile', '--norc', '-e', '-o', 'pipefail', '-c', script],
                cwd=root, env=os.environ | {
                    'PATH': str(root / 'bin') + os.pathsep + os.environ['PATH'],
                    'RUNNER_TEMP': str(root / 'runner'), 'UBUNTU_APT_SNAPSHOT': STAMP,
                    'FAKE_APT_ROOT': str(root), 'deb_path': 'artifacts/polaris.deb',
                    'kms_deb_path': 'artifacts/polaris-kms.deb', 'expected_package_version': '1.5.0'},
                capture_output=True, text=True, timeout=10)
            calls_path = root / 'calls.jsonl'
            calls = [json.loads(line) for line in calls_path.read_text().splitlines()] if calls_path.exists() else []
            self.assertEqual((root / 'artifacts/polaris-kms.deb').read_bytes(), PAYLOAD + b'polaris-kms')
            self.assertFalse(any(p.name.startswith(('.polaris-payload-', '.polaris-local-'))
                                 for p in (root / 'cache').iterdir()))
            self.assertEqual((root / 'cache/unrelated.keep').read_bytes(), b'preserve unrelated cache state')
            self.assertEqual((root / 'cache/partial/retained.partial').read_bytes(), b'original apt partial')
            added = {p.name for p in (root / 'cache').iterdir()} - original_cache_names
            allowed = {NAME}
            if config.get('mutate_local_cache_during_fetch') or config.get('replace_local_cache_during_install'):
                allowed.add('polaris_1.5.0_amd64.deb')  # deliberately foreign fixture, never delete it
            self.assertTrue(added <= allowed, added)
            if inspect is not None:
                inspect(root)
            return result, calls

    def test_both_local_smoke_calls_prefetch_runtime_dependencies(self):
        def inspect(root):
            observations = [json.loads(line) for line in (root / 'local-installs.jsonl').read_text().splitlines()]
            self.assertEqual(len(observations), 2)
            for name, row in zip(('polaris', 'polaris-kms'), observations):
                self.assertEqual(row['sha256'], hashlib.sha256(PAYLOAD + name.encode()).hexdigest())
                self.assertEqual(row['cache'], name + '_1.5.0_amd64.deb')
                self.assertTrue(row['regular'])
                self.assertFalse(row['symlink'])
                self.assertEqual(row['nlink'], 1)
                self.assertEqual(row['mode'], 0o644)
                self.assertFalse((root / 'cache' / row['cache']).exists())
        result, calls = self.run_smoke(inspect=inspect)
        self.assertEqual(result.returncode, 0, result.stderr)
        installs = [args for name, args in calls if name == 'apt-get' and '--no-download' in args]
        self.assertEqual(len(installs), 2)
        self.assertEqual([args[-1] for args in installs],
                         ['./artifacts/polaris.deb', './artifacts/polaris-kms.deb'])
        plans = [args for name, args in calls if name == 'apt-get' and '--print-uris' in args]
        self.assertEqual(len(plans), 2)
        for plan, install in zip(plans, installs):
            self.assertIn('Acquire::ForceHash=SHA256', plan)
            self.assertNotIn('Acquire::ForceHash=SHA256', install)
            self.assertEqual(plan[plan.index('install'):plan.index('--print-uris')],
                             install[install.index('install'):])

    def test_occupied_local_cache_refuses_even_valid_bytes_without_overwrite(self):
        original = {}
        def before(root):
            for name in ('polaris', 'polaris-kms'):
                p = root / 'cache' / (name + '_1.5.0_amd64.deb')
                p.write_bytes(PAYLOAD + name.encode())
                original[name] = p.stat().st_ino
        def inspect(root):
            for name, inode in original.items():
                p = root / 'cache' / (name + '_1.5.0_amd64.deb')
                self.assertEqual(p.stat().st_ino, inode)
                self.assertEqual(p.read_bytes(), PAYLOAD + name.encode())
        result, calls = self.run_smoke(before=before, inspect=inspect)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('already occupied', result.stderr)
        self.assertFalse(any(name == 'curl' or (name == 'apt-get' and '--no-download' in args)
                             for name, args in calls))

    def test_invalid_local_cache_refuses_without_overwrite_or_install(self):
        for kind in ('size', 'hash', 'symlink', 'hardlink'):
            with self.subTest(kind=kind):
                def before(root):
                    p = root / 'cache/polaris_1.5.0_amd64.deb'
                    foreign = root / 'foreign.deb'
                    foreign.write_bytes(b'x' * len(PAYLOAD + b'polaris'))
                    if kind == 'symlink':
                        p.symlink_to(foreign)
                    elif kind == 'hardlink':
                        os.link(foreign, p)
                    else:
                        p.write_bytes(b'x' if kind == 'size' else foreign.read_bytes())
                def inspect(root):
                    p = root / 'cache/polaris_1.5.0_amd64.deb'
                    self.assertEqual(p.read_bytes(), b'x' if kind == 'size' else b'x' * len(PAYLOAD + b'polaris'))
                    self.assertEqual(p.is_symlink(), kind == 'symlink')
                    self.assertEqual((root / 'foreign.deb').read_bytes(), b'x' * len(PAYLOAD + b'polaris'))
                result, calls = self.run_smoke(before=before, inspect=inspect)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(any(name == 'curl' or (name == 'apt-get' and '--no-download' in args)
                                     for name, args in calls))

    def test_local_cache_slot_created_during_fetch_refuses_without_overwrite(self):
        def inspect(root):
            self.assertEqual((root / 'cache/polaris_1.5.0_amd64.deb').read_bytes(),
                             b'x' * len(PAYLOAD + b'polaris'))
        result, calls = self.run_smoke(config_changes={'mutate_local_cache_during_fetch': True}, inspect=inspect)
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(any(name == 'curl' for name, _ in calls))
        self.assertFalse(any(name == 'apt-get' and '--no-download' in args for name, args in calls))

    def test_failed_install_or_source_change_cleans_only_its_staged_copy(self):
        for changes in ({'install_exit': 100}, {'mutate_local_during_install': True}):
            with self.subTest(changes=changes):
                result, calls = self.run_smoke(config_changes=changes)
                self.assertNotEqual(result.returncode, 0)
                self.assertEqual(sum(name == 'apt-get' and '--no-download' in args
                                     for name, args in calls), 1)
                if 'install_exit' in changes:
                    self.assertIn('exit status 100', result.stderr)
                else:
                    self.assertIn('SHA256 mismatch', result.stderr)

    def test_replaced_staged_inode_refuses_cleanup_of_foreign_file(self):
        def inspect(root):
            self.assertEqual((root / 'cache/polaris_1.5.0_amd64.deb').read_bytes(), b'foreign replacement')
            self.assertEqual((root / 'retained-staged-copy.deb').read_bytes(), PAYLOAD + b'polaris')
        result, calls = self.run_smoke(config_changes={'replace_local_cache_during_install': True},
                                       inspect=inspect)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn('Replaced local APT cache entry; refusing cleanup', result.stderr)
        self.assertEqual(sum(name == 'apt-get' and '--no-download' in args for name, args in calls), 1)

    def test_atomic_publication_race_never_overwrites_a_new_cache_occupant(self):
        module = runpy.run_path(str(ROOT / 'scripts/ci/fetch-ubuntu-snapshot.py'))
        original_link = os.link
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / 'local.deb'
            source.write_bytes(PAYLOAD)
            name = 'polaris_1.5.0_amd64.deb'
            binding = (source, len(PAYLOAD), 'SHA256:' + hashlib.sha256(PAYLOAD).hexdigest())
            def race(source_path, destination):
                Path(destination).write_bytes(b'foreign race winner')
                return original_link(source_path, destination)
            with patch.object(os, 'link', side_effect=race), self.assertRaises(FileExistsError):
                with module['cache_local_deb'](binding, name, root):
                    self.fail('The raced cache entry must never reach the consumer')
            self.assertEqual((root / name).read_bytes(), b'foreign race winner')
            self.assertEqual(source.read_bytes(), PAYLOAD)
            self.assertFalse(any(p.name.startswith('.polaris-local-') for p in root.iterdir()))

    def test_extra_local_uri_or_unsafe_cache_filename_refuses_before_fetch(self):
        for changes in ({'extra_local_row': True}, {'local_cache_name': NAME},
                        {'local_cache_name': NAME, 'local_row_after': True},
                        {'local_cache_name': '../escape.deb'}, {'local_cache_name': '.hidden.deb'},
                        {'local_cache_name': 'polaris_1%2fescape_amd64.deb'},
                        {'local_cache_name': 'polaris_1%00_amd64.deb'}):
            with self.subTest(changes=changes):
                result, calls = self.run_smoke(config_changes=changes)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(any(name == 'curl' for name, _ in calls))

    def test_caller_binding_rejects_wrong_preplan_size_or_sha256(self):
        for changes in ({'binding_size_delta': 1}, {'binding_digest': '0' * 64}):
            with self.subTest(changes=changes):
                result, calls = self.run_smoke(config_changes=changes)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(any(name in ('apt-get', 'curl') for name, _ in calls))

    def test_local_symlink_nonfile_or_multiple_links_refuse_before_planning(self):
        def symlink(root):
            p = root / 'artifacts/polaris.deb'
            p.rename(root / 'original.deb')
            p.symlink_to(root / 'original.deb')
        def directory(root):
            p = root / 'artifacts/polaris.deb'
            p.unlink()
            p.mkdir()
        def hardlink(root):
            os.link(root / 'artifacts/polaris.deb', root / 'other.deb')
        for before in (symlink, directory, hardlink):
            with self.subTest(kind=before.__name__):
                result, calls = self.run_smoke(before=before)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(any(name in ('apt-get', 'curl') for name, _ in calls))

    def test_local_same_size_change_during_plan_refuses_real_install(self):
        result, calls = self.run_smoke(config_changes={'mutate_local_during_plan': True})
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(any(name == 'apt-get' and '--no-download' in args for name, args in calls))

    def test_local_binding_must_match_the_actual_install_argument(self):
        result, calls = self.run_smoke(bound_path='./artifacts/polaris-kms.deb')
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(any(name in ('apt-get', 'curl') for name, _ in calls))

    def test_local_same_size_change_during_fetch_refuses_real_install(self):
        result, calls = self.run_smoke(config_changes={'mutate_local_during_fetch': True})
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(any(name == 'apt-get' and '--no-download' in args for name, args in calls))

    def test_local_install_still_refuses_tampered_remote_dependency(self):
        result, calls = self.run_smoke(config_changes={
            'archive': {'status': '200', 'exit': 0, 'body': (b'x' * len(PAYLOAD)).hex()}})
        self.assertNotEqual(result.returncode, 0)
        self.assertTrue(any(name == 'curl' for name, _ in calls))
        self.assertFalse(any(name == 'apt-get' and '--no-download' in args for name, args in calls))

    def test_local_row_hash_if_present_must_equal_preplan_sha256(self):
        result, _ = self.run_smoke(config_changes={'local_row_hash': 'SHA256:{SHA256}'})
        self.assertEqual(result.returncode, 0, result.stderr)
        for digest in ('SHA256:' + '0' * 64, 'MD5Sum:' + '0' * 32, 'SHA1:' + '0' * 40):
            with self.subTest(digest=digest):
                result, calls = self.run_smoke(config_changes={'local_row_hash': digest})
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(any(name == 'curl' for name, _ in calls))

    def test_local_row_wrong_size_duplicate_or_unlisted_uri_refuses_before_fetch(self):
        for changes in ({'local_row_size_delta': 1}, {'repeat_local_row': True},
                        {'local_uri': 'file:/tmp/not-the-allowed.deb'},
                        {'local_uri': 'https://foreign.example/local.deb'}):
            with self.subTest(changes=changes):
                result, calls = self.run_smoke(config_changes=changes)
                self.assertNotEqual(result.returncode, 0)
                self.assertFalse(any(name == 'curl' for name, _ in calls))

    def test_local_argument_may_resolve_to_strong_snapshot_row(self):
        # Actual ROOT xtrans-copy fixture selected its same-version repository
        # row rather than a local row. Do not require a local row to be printed.
        result, _ = self.run_smoke(config_changes={'local_row': False})
        self.assertEqual(result.returncode, 0, result.stderr)


class DownloadBudgetTests(unittest.TestCase):
    def test_large_slow_payload_succeeds_but_stalled_transfer_is_bounded(self):
        cases = [(14400000, {'seconds': 160}, 0), (14400000, {'stall_seconds': 31}, 1),
                 (1024 * 1024 * 1024, {'seconds': 590}, 0)]
        for size, response, expected in cases:
            with self.subTest(size=size, response=response), tempfile.TemporaryDirectory() as directory:
                root = Path(directory)
                (root / 'bin').mkdir()
                curl = root / 'bin/curl'
                curl.write_text(f'#!{sys.executable}\n' + FAKE)
                curl.chmod(0o755)
                (root / 'config.json').write_text(json.dumps({
                    'snapshot': {'status': '200', 'exit': 0, 'body': PAYLOAD.hex(), **response}}))
                # Exercise production download(), with curl alone replaced. No wall-clock sleep/network.
                script = ('import runpy,sys; from pathlib import Path; '
                          'module=runpy.run_path(sys.argv[1]); '
                          'module["download"](sys.argv[2], Path(sys.argv[3]), int(sys.argv[4]))')
                result = subprocess.run([
                    sys.executable, '-c', script, str(ROOT / 'scripts/ci/fetch-ubuntu-snapshot.py'),
                    URI, str(root / 'payload.deb'), str(size)], capture_output=True, text=True, timeout=10,
                    env=os.environ | {'FAKE_APT_ROOT': str(root),
                                      'PATH': str(root / 'bin') + os.pathsep + os.environ['PATH']})
                self.assertEqual(result.returncode, expected, result.stderr)
                calls = [json.loads(line) for line in (root / 'calls.jsonl').read_text().splitlines()]
                self.assertEqual(len(calls), 1)
                args = calls[0][1]
                self.assertLessEqual(int(args[args.index('--max-time') + 1]), 600)
                self.assertEqual(args[args.index('--speed-limit') + 1], '1024')
                self.assertEqual(args[args.index('--speed-time') + 1], '30')


if __name__ == '__main__':
    unittest.main()
