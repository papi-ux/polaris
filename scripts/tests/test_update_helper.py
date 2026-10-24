"""Synthetic only: no helper main, subprocess, network, RPM or host changes."""
import copy
import hashlib
import importlib.util
import io
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

SPEC = importlib.util.spec_from_file_location('update_helper', Path(__file__).parents[1] / 'update_helper.py')
helper = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(helper)


def packages(version='1.5.0', kms=True):
    values = {name: {'state': 'installed', 'version': version, 'rpm_version': version.replace('-', '~'),
                     'release': '1', 'arch': 'x86_64'} for name in helper.PACKAGES}
    if not kms:
        values['polaris-kms'] = {'state': 'absent'}
    return values


def release(version='1.5.1', identity=123):
    tag = 'v' + version
    return {'id': identity, 'draft': False, 'prerelease': '-' in version, 'tag_name': tag,
            'assets': [{'id': number, 'name': name, 'size': 3, 'digest': 'sha256:' + hashlib.sha256(b'rpm').hexdigest(),
                        'browser_download_url': helper.UPSTREAM + tag + '/' + name, 'updated_at': 'today'}
                       for number, name in enumerate(('Polaris-fedora44-x86_64.rpm', 'Polaris-kms-fedora44-x86_64.rpm'), 10)]}


class HelperTests(unittest.TestCase):
    def setUp(self):
        # Fail the test rather than accidentally reaching any effectful production seam.
        self.no_process = patch.object(helper.subprocess, 'Popen', side_effect=AssertionError('real process forbidden'))
        self.no_network = patch.object(helper, 'open_official', side_effect=AssertionError('real network forbidden'))
        self.no_process.start()
        self.no_network.start()
        self.addCleanup(self.no_process.stop)
        self.addCleanup(self.no_network.stop)

    def test_closed_arguments_and_exact_bounded_stdin(self):
        self.assertEqual(helper.arguments(['install', 'beta', '1.5.1-beta.2']), ('install', 'beta', '1.5.1-beta.2'))
        for argv in ([], ['install', 'stable'], ['install', 'stable', '../x'], ['install', 'stable', '1.5.1', '/tmp/a'],
                     ['install', '--url', '1.5.1'], ['shell'], ['status', 'stable'], ['plan', 'stable', '1.5.1']):
            with self.subTest(argv=argv), self.assertRaises(helper.Refusal):
                helper.arguments(argv)
        self.assertEqual(helper.read_review_guard(io.StringIO('a' * 64 + '\n')), 'a' * 64)
        for raw in ('', 'a' * 64, 'a' * 64 + '\nextra', 'A' * 64 + '\n', 'x\n', '{}\n'):
            with self.subTest(raw=raw), self.assertRaises(helper.Refusal):
                helper.read_review_guard(io.StringIO(raw))

    def test_only_actual_fedora44_asset_family_is_qualified(self):
        self.assertEqual(helper.qualify_host('ID=fedora\nVERSION_ID="44"', 'x86_64', False)['version_id'], '44')
        for os_release, machine, image in (('ID=bazzite\nID_LIKE=fedora\nVERSION_ID=44', 'x86_64', False),
                                           ('ID=fedora\nVERSION_ID=43', 'x86_64', False),
                                           ('ID=fedora\nVERSION_ID=44', 'aarch64', False),
                                           ('ID=ubuntu\nVERSION_ID=24.04', 'x86_64', False),
                                           ('ID=fedora\nVERSION_ID=44', 'x86_64', True)):
            with self.subTest(os=os_release), self.assertRaises(helper.Refusal):
                helper.qualify_host(os_release, machine, image)

    def test_channels_are_host_selected_independently_and_drafts_are_ignored(self):
        draft = release('9.0.0'); draft['draft'] = True
        data = [release('1.5.2-beta.1'), draft, release('1.5.0'), release(), release('1.5.1-beta.2')]
        self.assertEqual(helper.select_release(data, 'stable', packages())['version'], '1.5.1')
        self.assertEqual(helper.select_release(data, 'beta', packages())['version'], '1.5.2-beta.1')
        self.assertEqual(len(helper.select_release(data, 'stable', packages(kms=False))['assets']), 1)

    def test_review_binds_release_id_asset_ids_digests_pair_and_installed_state(self):
        original = release()
        guard = helper.select_release([original], 'stable', packages())['review_guard']
        for change in ('release', 'asset', 'digest', 'size'):
            altered = copy.deepcopy(original)
            if change == 'release': altered['id'] += 1
            elif change == 'asset': altered['assets'][0]['id'] += 1
            elif change == 'digest': altered['assets'][0]['digest'] = 'sha256:' + 'b' * 64
            else: altered['assets'][0]['size'] += 1
            self.assertNotEqual(helper.select_release([altered], 'stable', packages())['review_guard'], guard)
        self.assertNotEqual(helper.select_release([original], 'stable', packages(kms=False))['review_guard'], guard)
        self.assertNotEqual(helper.select_release([original], 'stable', packages('1.4.9'))['review_guard'], guard)

    def test_absent_digest_unexpected_url_missing_pair_and_ambiguous_identity_refuse(self):
        for change in ('digest', 'url', 'pair', 'duplicate'):
            value = release()
            if change == 'digest': value['assets'][0].pop('digest')
            elif change == 'url': value['assets'][0]['browser_download_url'] = 'https://example.com/x.rpm'
            elif change == 'pair': value['assets'].pop()
            else: value['assets'].append(copy.deepcopy(value['assets'][0]))
            with self.subTest(change=change), self.assertRaises(helper.Refusal):
                helper.select_release([value], 'stable', packages())

    def test_downgrade_and_unknown_partial_installed_state_refuse_plan(self):
        for current in (packages('2.0.0'), {'polaris': {'state': 'partial'}, 'polaris-kms': {'state': 'absent'}},
                        {'polaris': packages()['polaris'], 'polaris-kms': {'state': 'unknown'}}):
            with self.subTest(current=current), self.assertRaises(helper.Refusal):
                helper.make_plan('stable', fetch=lambda: [release()], read_installed=lambda: current, host=lambda: {})

    def test_rpm_identity_refuses_name_arch_epoch_version_and_missing_metadata(self):
        asset = helper.select_release([release()], 'stable', packages())['assets'][0]
        for metadata in ('other|0|1.5.1|1|x86_64\n', 'polaris|0|1.5.1|1|aarch64\n',
                         'polaris|1|1.5.1|1|x86_64\n', 'polaris|0|1.5.0|1|x86_64\n', ''):
            with self.subTest(metadata=metadata), self.assertRaises(helper.Refusal):
                helper.validate_package(Path('/private/a.rpm'), asset, '1.5.1', run=lambda argv: (0, metadata))
        calls = []
        def obsoleting(argv):
            calls.append(argv)
            return (0, 'unrelated-package\n') if 'OBSOLETENAME' in ''.join(argv) else (0, 'polaris|0|1.5.1|1|x86_64\n')
        with self.assertRaises(helper.Refusal) as raised:
            helper.validate_package(Path('/private/a.rpm'), asset, '1.5.1', run=obsoleting)
        self.assertEqual(raised.exception.code, 'transaction_not_closed')

    def test_download_compares_exact_independent_size_and_digest(self):
        asset = helper.select_release([release()], 'stable', packages())['assets'][0]
        for payload, success in ((b'rpm', True), (b'bad', False), (b'rp', False), (b'rpm-extra', False)):
            with self.subTest(payload=payload), tempfile.TemporaryDirectory() as directory:
                with patch.object(helper, 'open_official', return_value=io.BytesIO(payload)):
                    if success: helper.download(asset, Path(directory) / 'asset.rpm')
                    else:
                        with self.assertRaises(helper.Refusal): helper.download(asset, Path(directory) / 'asset.rpm')

    def exercise_install(self, *, fail_transaction=False, fail_preview=False, changed=False, crash=False, obsoletes=False):
        current = packages()
        reviewed = helper.make_plan('stable', fetch=lambda: [release()], read_installed=lambda: current, host=lambda: {})
        calls = []
        reads = []
        count = 0
        def plan(channel):
            nonlocal count
            count += 1
            value = copy.deepcopy(reviewed)
            if changed and count == 2: value['review_guard'] = 'c' * 64
            return value
        def run(argv, **kwargs):
            calls.append(argv)
            if '-qp' in argv:
                if 'OBSOLETENAME' in ''.join(argv): return (0, 'unrelated\n' if obsoletes else '')
                name = 'polaris-kms' if 'Polaris-kms-' in argv[-1] else 'polaris'
                return 0, name + '|0|1.5.1|1|x86_64\n'
            if '--test' in argv: return (1, 'missing unrelated dependency') if fail_preview else (0, '')
            if crash: raise OSError('synthetic lost RPM status')
            return (1, 'synthetic package transaction stopped') if fail_transaction else (0, '')
        def read_installed():
            reads.append(True)
            return {'polaris': {'state': 'unknown'}, 'polaris-kms': {'state': 'absent'}} if crash else packages('1.5.1')
        with tempfile.TemporaryDirectory() as directory:
            result = helper.install('stable', '1.5.1', reviewed['review_guard'], Path(directory), plan=plan,
                                    fetch_asset=lambda asset, path: path.write_bytes(b'rpm'), run=run,
                                    read_installed=read_installed)
        self.assertEqual(len(reads), 1)
        return result, calls

    def test_actual_transaction_is_non_resolving_and_same_files_as_preview(self):
        result, calls = self.exercise_install()
        self.assertEqual(result['state'], 'installed')
        operations = [argv for argv in calls if '-U' in argv]
        self.assertEqual(len(operations), 2)
        self.assertEqual([arg for arg in operations[0] if arg != '--test'], operations[1])
        self.assertEqual(operations[1][:5], [helper.RPM, '-U', '--nomanifest', '--noscripts', '--notriggers'])
        self.assertEqual(len(operations[1][operations[1].index('--') + 1:]), 2)
        for flag in ('--nodeps', '--oldpackage', '--replacefiles', '--replacepkgs', '--force'):
            self.assertNotIn(flag, operations[1])

    def test_changed_release_obsoletes_and_dependency_failures_never_attempt_transaction(self):
        for kw in ({'changed': True}, {'fail_preview': True}, {'obsoletes': True}):
            with self.subTest(kw=kw):
                result, calls = self.exercise_install(**kw)
                self.assertEqual(result['state'], 'failed')
                self.assertFalse(result['transaction_attempted'])
                self.assertFalse(any('-U' in argv and '--test' not in argv for argv in calls))

    def test_failed_or_lost_transaction_rereads_actual_state_without_old_version_promise(self):
        failed, _ = self.exercise_install(fail_transaction=True)
        self.assertTrue(failed['transaction_attempted'])
        self.assertEqual(failed['installed']['polaris']['version'], '1.5.1')
        crashed, _ = self.exercise_install(crash=True)
        self.assertTrue(crashed['transaction_attempted'])
        self.assertEqual(crashed['installed'], {'polaris': {'state': 'unknown'}, 'polaris-kms': {'state': 'absent'}})

    def test_per_package_query_distinguishes_absent_unknown_and_version(self):
        def run(argv):
            return (0, 'polaris|0|1.5.1~beta.2|1|x86_64\n') if argv[-1] == 'polaris' else (1, 'package polaris-kms is not installed\n')
        current = helper.installed_packages(run=run)
        self.assertEqual(current['polaris']['version'], '1.5.1-beta.2')
        self.assertEqual(current['polaris-kms']['state'], 'absent')
        self.assertEqual(helper.installed_packages(run=lambda argv: (1, 'database error'))['polaris']['state'], 'unknown')
        duplicates = 'polaris|0|1.5.0|1|x86_64\npolaris|0|1.5.1|1|x86_64\n'
        self.assertEqual(helper.installed_packages(run=lambda argv: (0, duplicates))['polaris']['state'], 'partial')


if __name__ == '__main__':
    unittest.main()
