from contextlib import ExitStack
import hashlib
from pathlib import Path
import tempfile
import unittest
from verify_package import verify


class Package(unittest.TestCase):
    def setUp(self):
        self.contexts = ExitStack()
        self.addCleanup(self.contexts.close)
        self.root = Path(self.contexts.enter_context(tempfile.TemporaryDirectory()))
        source = Path(__file__).resolve().parents[2]
        self.data = self.root / 'usr/share/polaris/multiseat/security'
        self.data.mkdir(parents=True)
        expected = {}
        for name in ('polaris_multiseat_input.cil', 'polaris_nvidia_worker.te', '97-polaris-multiseat-input.rules'):
            value = (source / 'containers/multiseat/selinux' / name).read_bytes()
            (self.data / name).write_bytes(value)
            expected[name] = hashlib.sha256(value).hexdigest()
        identity = '1:' + ':'.join(expected.values())
        self.release = hashlib.sha256(identity.encode()).hexdigest()
        marker = 'polaris_spaces_' + self.release[:32] + '_t'
        value = ('(type ' + marker + ')\n(roletype object_r ' + marker + ')\n').encode()
        (self.data / 'polaris_spaces_version.cil').write_bytes(value)
        expected['polaris_spaces_version.cil'] = hashlib.sha256(value).hexdigest()
        self.helper = self.root / 'usr/bin/polaris-spaces-setup'
        self.helper.parent.mkdir(parents=True)
        template = (source / 'scripts/spaces/security_setup.py.in').read_text()
        keys = ['INPUT', 'WORKER', 'RULE', 'VERSION']
        for key, checksum in zip(keys, expected.values()):
            template = template.replace('@POLARIS_SPACES_' + key + '_SHA@', checksum)
        template = template.replace('@POLARIS_SPACES_SECURITY_DIR@', '/usr/share/polaris/multiseat/security')
        template = template.replace('@POLARIS_SPACES_SECURITY_RELEASE@', self.release)
        template = template.replace('@POLARIS_SPACES_SECURITY_MARKER@', marker)
        self.helper.write_text(template); self.helper.chmod(0o755)
        seccomp = (source / 'containers/multiseat/seccomp/steam.json').read_bytes()
        (self.data.parent / ('steam-seccomp-' + hashlib.sha256(seccomp).hexdigest() + '.json')).write_bytes(seccomp)
        policy = (source / 'packaging/linux/dev.polaris-stream.app.Polaris.policy.in').read_text()
        for key, value in {'POLARIS_PUBLISHER_WEBSITE': 'https://papi-ux.com/polaris/', 'POLARIS_DESKTOP_ICON': 'polaris',
                           'POLARIS_POLKIT_ACTION_PREFIX': 'dev.polaris-stream.app.polaris',
                           'CMAKE_INSTALL_FULL_BINDIR': '/usr/bin'}.items():
            policy = policy.replace('@' + key + '@', value)
        self.assertNotIn('@', policy)
        self.policy = self.root / 'usr/share/polkit-1/actions/dev.polaris-stream.app.Polaris.policy'
        self.policy.parent.mkdir(parents=True)
        self.policy.write_text(policy)

    def test_matching_inert_payload(self):
        self.assertEqual(verify(self.root), self.release)

    def test_polkit_actions_only_run_the_packaged_helper_and_always_ask(self):
        original = self.policy.read_text()
        changes = [
            ('<allow_active>auth_admin</allow_active>', '<allow_active>auth_admin_keep</allow_active>'),
            ('<allow_active>auth_admin</allow_active>', '<allow_active>yes</allow_active>'),
            ('<allow_any>no</allow_any>', '<allow_any>auth_admin</allow_any>'),
            ('<allow_inactive>no</allow_inactive>', '<allow_inactive>auth_admin</allow_inactive>'),
            ('>/usr/bin/polaris-spaces-setup<', '>/usr/bin/bash<'),
            ('>docker-access<', '>remove<'),
            ('.spaces-docker-access"', '.spaces-anything"'),
            ('<annotate key="org.freedesktop.policykit.exec.argv1">install</annotate>', ''),
            ('</policyconfig>', '<action id="dev.polaris-stream.app.polaris.extra"><description>x</description><message>x</message>'
             '<defaults><allow_any>no</allow_any><allow_inactive>no</allow_inactive><allow_active>auth_admin</allow_active></defaults>'
             '<annotate key="org.freedesktop.policykit.exec.path">/usr/bin/polaris-spaces-setup</annotate>'
             '<annotate key="org.freedesktop.policykit.exec.argv1">remove</annotate></action></policyconfig>'),
        ]
        for old, new in changes:
            self.assertEqual(original.count(old), 1 if old != '<allow_active>auth_admin</allow_active>' and
                             old != '<allow_any>no</allow_any>' and old != '<allow_inactive>no</allow_inactive>' and
                             old != '>/usr/bin/polaris-spaces-setup<' else 2, old)
            self.policy.write_text(original.replace(old, new, 1))
            with self.assertRaises(AssertionError, msg=new):
                verify(self.root)
        self.policy.write_text(original)
        self.assertEqual(verify(self.root), self.release)
        self.policy.unlink()
        with self.assertRaises(AssertionError):
            verify(self.root)

    def test_changed_payload_or_helper_cannot_pass(self):
        for path in self.data.iterdir():
            old = path.read_bytes(); path.write_bytes(b'changed')
            with self.assertRaises(AssertionError):
                verify(self.root)
            path.write_bytes(old)
        self.helper.write_text(self.helper.read_text().replace(self.release, 'a' * 64))
        with self.assertRaises(AssertionError):
            verify(self.root)

    def test_missing_helper_or_seccomp_cannot_pass(self):
        self.helper.chmod(0o644)
        with self.assertRaises(AssertionError):
            verify(self.root)
        self.helper.chmod(0o755)
        for path in self.data.parent.glob('steam-seccomp-*.json'):
            path.unlink()
        with self.assertRaises(AssertionError):
            verify(self.root)

    def test_package_cannot_install_live_input_rule_or_readiness(self):
        for name in ('etc/udev/rules.d/97-polaris-multiseat-input.rules',
                     'usr/lib/udev/rules.d/97-polaris-multiseat-input.rules',
                     'var/lib/polaris/spaces-security/ready.json'):
            path = self.root / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_text('active')
            with self.assertRaises(AssertionError):
                verify(self.root)
            path.unlink()


if __name__ == '__main__':
    unittest.main()
