"""Exercise privileged setup decisions with no host-policy or service changes."""
from contextlib import ExitStack
import copy
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import stat
import sys
import time
import types
import unittest
from unittest.mock import patch

loader = importlib.machinery.SourceFileLoader('spaces_security_setup', str(Path(__file__).with_name('security_setup.py.in')))
spec = importlib.util.spec_from_loader(loader.name, loader)
s = importlib.util.module_from_spec(spec)
loader.exec_module(s)


class Host(s.System):
    def __init__(self):
        super().__init__()
        self.files = {}
        self.installed = {}
        self.active = False
        self.fail = ''
        self.calls = []
        self.compiles = 0

    def read(self, path, optional=False, **kwargs):
        if path not in self.files and not optional:
            raise s.SetupError('missing file')
        return self.files.get(path)

    def write(self, path, value, *args, **kwargs):
        if self.fail == 'journal' and path == self.state / 'state.json':
            raise s.SetupError('journal failed')
        if self.fail == 'rule' and path == self.rule:
            raise s.SetupError('rule failed')
        self.files[path] = value

    def remove(self, path, **kwargs):
        self.files.pop(path, None)

    def modules(self):
        return copy.deepcopy(self.installed)

    def idle(self):
        return not self.active

    def compile(self):
        self.compiles += 1
        result = {}
        for name in s.NAMES:
            value = (s.RELEASE + name).encode()
            self.files[self.state / (name + '.cil')] = value
            result[name] = s.digest(value)
        return result

    def run(self, argv, **kwargs):
        self.calls.append(argv)
        if '-i' in argv:
            if self.fail == 'before_commit':
                raise s.SetupError('before commit')
            self.installed = {Path(path).stem: s.digest(self.files[Path(path)]) for path in argv[4:]}
        if '-r' in argv:
            self.installed = {}
        if self.fail == 'after_commit' and ('-i' in argv or '-r' in argv):
            raise s.SetupError('after commit')
        if self.fail == 'reload' and argv[0] == '/usr/bin/udevadm':
            raise s.SetupError('reload failed')
        return b''

    def state_value(self):
        return s.decode_state(self.files[self.state / 'state.json'])


class Transaction(unittest.TestCase):
    def setUp(self):
        self.contexts = ExitStack()
        self.addCleanup(self.contexts.close)
        self.host = Host()
        self.contexts.enter_context(patch.object(s, 'RELEASE', 'a' * 64))
        self.contexts.enter_context(patch.object(s, 'EXPECTED', {s.RULE.name: s.digest(b'rule')}))
        self.contexts.enter_context(patch.object(s, 'read', self.host.read))
        self.contexts.enter_context(patch.object(s, 'write', self.host.write))
        self.contexts.enter_context(patch.object(s, 'remove_file', self.host.remove))
        self.kernel = self.contexts.enter_context(patch.object(s, 'kernel_ready', return_value=True))
        self.host.files[self.host.data / s.RULE.name] = b'rule'

    def test_install_and_remove_are_explicit_and_idempotent(self):
        h = self.host
        h.change('install')
        self.assertEqual(h.state_value()['phase'], 'installed')
        self.assertEqual(h.files[h.state / 'ready.json'], s.ready_bytes(s.RELEASE))
        self.assertEqual(len(h.installed), 3)
        self.assertEqual(h.files[h.rule], b'rule')
        calls = len(h.calls)
        h.active = True
        h.change('install')
        self.assertEqual(len(h.calls), calls)
        with self.assertRaises(s.SetupError):
            h.change('remove')
        h.active = False
        h.change('remove')
        self.assertEqual(h.installed, {})
        self.assertNotIn(h.rule, h.files)
        self.assertNotIn(h.state / 'ready.json', h.files)
        self.assertEqual(h.state_value()['phase'], 'removed')
        calls = len(h.calls)
        h.change('remove')
        self.assertEqual(len(h.calls), calls)
        h.change('install')
        self.assertEqual(h.state_value()['phase'], 'installed')

    def test_journal_failure_prevents_system_effects(self):
        h = self.host; h.fail = 'journal'
        with self.assertRaises(s.SetupError):
            h.change('install')
        self.assertEqual(h.calls, [])
        self.assertEqual(h.installed, {})
        h.fail = ''; h.change('install')
        self.assertEqual(h.state_value()['phase'], 'installed')

    def test_install_recovers_each_interrupted_effect_without_recompiling(self):
        for failure in ('before_commit', 'after_commit', 'rule', 'reload'):
            with self.subTest(failure=failure):
                h = self.host; h.fail = failure
                with self.assertRaises(s.SetupError):
                    h.change('install')
                self.assertEqual(h.state_value()['phase'], 'installing')
                self.assertNotIn(h.state / 'ready.json', h.files)
                compiles = h.compiles
                h.fail = ''; h.change('install')
                self.assertEqual(h.compiles, compiles)
                self.assertEqual(h.state_value()['phase'], 'installed')
                h.change('remove')

    def test_remove_recovers_committed_policy_and_reload_failures(self):
        for failure in ('after_commit', 'reload'):
            with self.subTest(failure=failure):
                h = self.host; h.change('install'); h.fail = failure
                with self.assertRaises(s.SetupError):
                    h.change('remove')
                self.assertEqual(h.state_value()['phase'], 'removing')
                self.assertNotIn(h.state / 'ready.json', h.files)
                with self.assertRaises(s.SetupError):
                    h.change('install')
                h.fail = ''; h.change('remove')
                self.assertEqual(h.state_value()['phase'], 'removed')
                if failure == 'after_commit':
                    self.assertIn(['/usr/bin/semodule', '-R'], h.calls)

    def test_changed_policy_or_rule_never_gets_adopted(self):
        h = self.host
        for operation in ('install', 'remove'):
            h.installed = {s.NAMES[0]: 'b' * 64}
            with self.assertRaises(s.SetupError):
                h.change(operation)
            h.installed = {}; h.files[h.rule] = b'unowned'
            with self.assertRaises(s.SetupError):
                h.change(operation)
            del h.files[h.rule]
        self.assertEqual(h.calls, [])
        h.change('install')
        calls = len(h.calls)
        h.installed[s.NAMES[0]] = 'b' * 64
        with self.assertRaises(s.SetupError):
            h.change('remove')
        self.assertEqual(len(h.calls), calls)

    def test_active_host_prevents_new_install(self):
        self.host.active = True
        with self.assertRaises(s.SetupError):
            self.host.change('install')
        self.assertEqual(self.host.calls, [])
        self.assertEqual(self.host.compiles, 0)

    def test_saved_candidates_and_external_changes_are_checked_on_retry(self):
        h = self.host; h.fail = 'before_commit'
        with self.assertRaises(s.SetupError):
            h.change('install')
        h.fail = ''
        path = h.state / (s.NAMES[0] + '.cil')
        h.files[path] = b'changed'
        calls = len(h.calls)
        with self.assertRaises(s.SetupError):
            h.change('install')
        self.assertEqual(len(h.calls), calls)
        h.files[h.rule] = b'changed'
        with self.assertRaises(s.SetupError):
            h.change('install')
        with self.assertRaises(s.SetupError):
            h.change('remove')
        self.assertEqual(len(h.calls), calls)

    def test_a_retry_finishes_old_identity_before_upgrading(self):
        h = self.host; h.fail = 'after_commit'
        with self.assertRaises(s.SetupError):
            h.change('install')
        h.fail = ''
        old = h.state_value()['after']
        with patch.object(s, 'RELEASE', 'b' * 64):
            h.change('install')
            self.assertEqual(h.installed, old)
            self.assertNotIn(h.state / 'ready.json', h.files)
            h.change('install')
            self.assertNotEqual(h.installed, old)
            self.assertEqual(h.files[h.state / 'ready.json'], s.ready_bytes('b' * 64))

    def test_kernel_must_confirm_before_readiness(self):
        self.kernel.return_value = False
        with self.assertRaises(s.SetupError):
            self.host.change('install')
        self.assertNotIn(self.host.state / 'ready.json', self.host.files)
        self.kernel.return_value = True
        self.host.change('install')
        self.assertIn(self.host.state / 'ready.json', self.host.files)

    def test_strict_journal_rejects_untrusted_shapes(self):
        h = self.host; h.change('install')
        good = h.state_value()
        bad = [None, [], 1, 'state', {}, dict(good, schema=True), dict(good, phase='anything'),
               dict(good, before={'unknown': 'a' * 64}), dict(good, after={}),
               dict(good, rule_after=10), dict(good, release='../path'), dict(good, extra=True)]
        for value in bad:
            with self.subTest(value=value), self.assertRaises(s.SetupError):
                s.decode_state(json.dumps(value))
        with self.assertRaises(s.SetupError):
            s.decode_state('{"schema":1,"schema":1}')


class Boundaries(unittest.TestCase):
    def test_inventory_rejects_overrides_disabled_and_duplicate_modules(self):
        checksum = 'sha256:' + 'a' * 64
        good = ('200 ' + s.NAMES[0] + ' cil ' + checksum + '\n').encode()
        self.assertEqual(s.inventory(good), {s.NAMES[0]: 'a' * 64})
        for bad in [good.replace(b'200', b'400'), good + good,
                    good.replace(b'cil', b'pp'), good.rstrip() + b' disabled\n',
                    good.replace(checksum.encode(), b'unknown')]:
            with self.assertRaises(s.SetupError):
                s.inventory(bad)
        self.assertEqual(s.inventory(b'100 unrelated pp ignored\n'), {})

    def test_trusted_files_reject_mutable_parents_symlinks_links_and_owners(self):
        file_meta = types.SimpleNamespace(st_mode=stat.S_IFREG | 0o644, st_uid=0, st_nlink=1)
        dir_meta = types.SimpleNamespace(st_mode=stat.S_IFDIR | 0o755, st_uid=0, st_nlink=2)
        path = Path('/safe/file')
        with patch.object(Path, 'lstat', side_effect=lambda p: file_meta if p == path else dir_meta, autospec=True):
            s.trusted(path)
            for key, value in [('st_mode', stat.S_IFLNK | 0o777), ('st_mode', stat.S_IFREG | 0o666),
                               ('st_mode', stat.S_IFREG | 0o4644), ('st_uid', 1000), ('st_nlink', 2)]:
                before = getattr(file_meta, key); setattr(file_meta, key, value)
                with self.assertRaises(s.SetupError):
                    s.trusted(path)
                setattr(file_meta, key, before)
            dir_meta.st_mode = stat.S_IFDIR | 0o777
            with self.assertRaises(s.SetupError):
                s.trusted(path)

    def test_commands_are_bounded_and_have_no_ambient_environment(self):
        h = s.System()
        with patch.object(s, 'trusted'), patch.dict(os.environ, {'SPACES_UNTRUSTED_ENV': 'present'}):
            result = h.run([sys.executable, '-c', 'import os; print(os.getenv("SPACES_UNTRUSTED_ENV", "clean"))'])
            self.assertEqual(result.strip(), b'clean')
            start = time.monotonic()
            with self.assertRaises(s.SetupError):
                h.run([sys.executable, '-c', 'import time; time.sleep(10)'], timeout=0.2)
            self.assertLess(time.monotonic() - start, 3)
            with patch.object(s, 'MAX_FILE', 10), self.assertRaises(s.SetupError):
                h.run([sys.executable, '-c', 'print("x" * 100)'])
            with self.assertRaises(s.SetupError):
                h.run([sys.executable, '-c', 'raise SystemExit(1)'])


if __name__ == '__main__':
    unittest.main()
