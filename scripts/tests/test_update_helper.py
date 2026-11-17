"""Synthetic only: no helper main, subprocess, network, RPM or host changes."""
import base64
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


def fedora(version='44'):
    return {'id': 'fedora', 'version_id': version, 'arch': 'x86_64'}


def release(version='1.5.1', identity=123, fedora_releases=('44',)):
    tag = 'v' + version
    names = [prefix + 'fedora' + fedora_release + '-x86_64.rpm'
             for fedora_release in fedora_releases for prefix in ('Polaris-', 'Polaris-kms-')]
    return {'id': identity, 'draft': False, 'prerelease': '-' in version, 'tag_name': tag,
            'assets': [{'id': number, 'name': name, 'size': 3, 'digest': 'sha256:' + hashlib.sha256(b'rpm').hexdigest(),
                        'browser_download_url': helper.UPSTREAM + tag + '/' + name, 'updated_at': 'today'}
                       for number, name in enumerate(names, 10)]}


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

    def test_github_trust_is_only_the_ca_bundle_fedora_ships(self):
        # Fedora 44 and 45 keep it here; /etc/pki/tls/certs/ca-bundle.crt is gone from both.
        self.assertEqual(helper.CA_BUNDLE, '/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem')
        with patch.object(helper.ssl, 'create_default_context', return_value='context') as create:
            self.assertEqual(helper.official_tls_context(), 'context')
        create.assert_called_once_with(cafile=helper.CA_BUNDLE)

    def test_only_actual_fedora44_asset_family_is_qualified(self):
        self.assertEqual(helper.qualify_host('ID=fedora\nVERSION_ID="44"', 'x86_64', False), fedora('44'))
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
        self.assertEqual(helper.select_release(data, 'stable', packages(), fedora())['version'], '1.5.1')
        self.assertEqual(helper.select_release(data, 'beta', packages(), fedora())['version'], '1.5.2-beta.1')
        self.assertEqual(len(helper.select_release(data, 'stable', packages(kms=False), fedora())['assets']), 1)

    def test_include_beta_selects_newest_stable_rc_or_beta_without_downgrading_stable(self):
        for values, expected, kind in (([release('1.5.1-rc.2'), release('1.5.1')], '1.5.1', 'stable'),
                                       ([release('1.5.0'), release('1.5.1-rc.1')], '1.5.1-rc.1', 'rc'),
                                       ([release('1.5.1'), release('1.5.2-beta.1')], '1.5.2-beta.1', 'beta')):
            with self.subTest(expected=expected):
                plan = helper.make_plan('beta', fetch=lambda: values, read_installed=packages, host=lambda: fedora())
                self.assertEqual((plan['version'], plan['release_kind']), (expected, kind))
        current = helper.make_plan('beta', fetch=lambda: [release('1.5.1'), release('1.5.1-rc.2')],
                                   read_installed=lambda: packages('1.5.1'), host=lambda: fedora())
        self.assertFalse(current['update_needed'])
        stable = helper.make_plan('beta', fetch=lambda: [release('1.5.1'), release('1.5.1-rc.2')],
                                  read_installed=lambda: packages('1.5.1-rc.2'), host=lambda: fedora())
        self.assertTrue(stable['update_needed'])

    def test_review_binds_release_id_asset_ids_digests_pair_and_installed_state(self):
        original = release()
        guard = helper.select_release([original], 'stable', packages(), fedora())['review_guard']
        for change in ('release', 'asset', 'digest', 'size'):
            altered = copy.deepcopy(original)
            if change == 'release': altered['id'] += 1
            elif change == 'asset': altered['assets'][0]['id'] += 1
            elif change == 'digest': altered['assets'][0]['digest'] = 'sha256:' + 'b' * 64
            else: altered['assets'][0]['size'] += 1
            self.assertNotEqual(helper.select_release([altered], 'stable', packages(), fedora())['review_guard'], guard)
        self.assertNotEqual(helper.select_release([original], 'stable', packages(kms=False), fedora())['review_guard'], guard)
        self.assertNotEqual(helper.select_release([original], 'stable', packages('1.4.9'), fedora())['review_guard'], guard)

    def test_absent_digest_unexpected_url_missing_pair_and_ambiguous_identity_refuse(self):
        for change in ('digest', 'url', 'pair', 'duplicate'):
            value = release()
            if change == 'digest': value['assets'][0].pop('digest')
            elif change == 'url': value['assets'][0]['browser_download_url'] = 'https://example.com/x.rpm'
            elif change == 'pair': value['assets'].pop()
            else: value['assets'].append(copy.deepcopy(value['assets'][0]))
            with self.subTest(change=change), self.assertRaises(helper.Refusal):
                helper.select_release([value], 'stable', packages(), fedora())

    def test_downgrade_and_unknown_partial_installed_state_refuse_plan(self):
        for current in (packages('2.0.0'), {'polaris': {'state': 'partial'}, 'polaris-kms': {'state': 'absent'}},
                        {'polaris': packages()['polaris'], 'polaris-kms': {'state': 'unknown'}}):
            with self.subTest(current=current), self.assertRaises(helper.Refusal):
                helper.make_plan('stable', fetch=lambda: [release()], read_installed=lambda: current, host=lambda: fedora())

    def test_rpm_identity_refuses_name_arch_epoch_version_and_missing_metadata(self):
        asset = helper.select_release([release()], 'stable', packages(), fedora())['assets'][0]
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
        # A host without the vendor key in RPM's database would otherwise add a NOKEY warning to both answers.
        self.assertEqual([argv[:5] for argv in calls], [[helper.RPM, '-qp', '--nosignature', '--nomanifest', '--qf']] * 2)

    def test_download_compares_exact_independent_size_and_digest(self):
        asset = helper.select_release([release()], 'stable', packages(), fedora())['assets'][0]
        for payload, success in ((b'rpm', True), (b'bad', False), (b'rp', False), (b'rpm-extra', False)):
            with self.subTest(payload=payload), tempfile.TemporaryDirectory() as directory:
                with patch.object(helper, 'open_official', return_value=io.BytesIO(payload)):
                    if success: helper.download(asset, Path(directory) / 'asset.rpm')
                    else:
                        with self.assertRaises(helper.Refusal): helper.download(asset, Path(directory) / 'asset.rpm')

    def exercise_install(self, *, fail_transaction=False, fail_preview=False, changed=False, crash=False, obsoletes=False, current=None,
                         fedora_release='44', signature='vendor', key_present=True, import_fails=False, key_lookup_error=False, import_readback_missing=False):
        current = current or packages()
        reviewed = helper.make_plan('stable', fetch=lambda: [release(fedora_releases=(fedora_release,))],
                                    read_installed=lambda: current, host=lambda: fedora(fedora_release))
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
            if argv[0] == helper.sys.executable:
                return (0, '') if signature == 'vendor' else (1, 'unsigned' if signature == 'unsigned' else 'wrong signer')
            if argv[:2] == ['/usr/bin/rpmkeys', '--list']:
                if key_lookup_error: return 2, 'keyring unavailable'
                present = key_present or (not import_readback_missing and any('--import' in c for c in calls))
                return (0, '58017edffa9f803e07ed26f835f13f14faad15cc Vendor public key\n') if present else (1, 'error: key not found: 58017EDFFA9F803E07ED26F835F13F14FAAD15CC\n')
            if argv[:2] == ['/usr/bin/rpmkeys', '--import']:
                return (1, 'key import refused') if import_fails else (0, '')
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
        with tempfile.TemporaryDirectory() as directory, patch.object(helper, 'vendor_key', create=True, return_value=Path('/usr/share/polaris/keys/polaris.gpg')):
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

    def test_mixed_pairs_only_download_and_transact_the_older_member(self):
        for older in helper.PACKAGES:
            with self.subTest(older=older):
                current = packages('1.5.1')
                current[older] = packages()[older]
                result, calls = self.exercise_install(current=current)
                self.assertEqual(result['state'], 'installed')
                self.assertEqual(result['installed'], packages('1.5.1'))
                operations = [argv for argv in calls if '-U' in argv]
                self.assertEqual(len(operations), 2)
                self.assertEqual([arg for arg in operations[0] if arg != '--test'], operations[1])
                files = operations[1][operations[1].index('--') + 1:]
                self.assertEqual(len(files), 1)
                self.assertEqual(Path(files[0]).name, 'Polaris-kms-fedora44-x86_64.rpm' if older == 'polaris-kms' else 'Polaris-fedora44-x86_64.rpm')
                self.assertEqual(len([argv for argv in calls if '-qp' in argv]), 2)
                self.assertFalse(any(flag in operations[1] for flag in ('--force', '--replacepkgs', '--nodeps', '--oldpackage')))

    def test_rpm_test_refusal_preserves_details_without_a_false_dependency_diagnosis(self):
        result, _ = self.exercise_install(fail_preview=True)
        self.assertIn('package state or dependencies may need manual repair', result['message'])
        self.assertIn('missing unrelated dependency', result['message'])
        self.assertIn('will not force a reinstall', result['message'])
        with self.assertRaises(helper.Refusal) as refused:
            helper.make_plan('stable', fetch=lambda: [release()], read_installed=lambda: {
                'polaris': {'state': 'partial', 'versions': ['1.5.0', '1.5.1']}, 'polaris-kms': {'state': 'absent'}}, host=lambda: fedora())
        self.assertIn('repair them, then review again', str(refused.exception))

    def test_fedora45_host_qualifies_with_its_own_release_and_other_releases_still_refuse(self):
        self.assertEqual(helper.qualify_host('ID=fedora\nVERSION_ID="45"', 'x86_64', False), fedora('45'))
        self.assertEqual(helper.qualify_host('ID=fedora\nVERSION_ID=44', 'x86_64', False), fedora('44'))
        for os_release, machine, image in (('ID=fedora\nVERSION_ID=43', 'x86_64', False),
                                           ('ID=fedora\nVERSION_ID=46', 'x86_64', False),
                                           ('ID=fedora\nVERSION_ID=450', 'x86_64', False),
                                           ('ID=fedora\nVERSION_ID=4', 'x86_64', False),
                                           ('ID=bazzite\nID_LIKE=fedora\nVERSION_ID=45', 'x86_64', False),
                                           ('ID=fedora\nVERSION_ID=45', 'aarch64', False),
                                           ('ID=fedora\nVERSION_ID=45', 'x86_64', True)):
            with self.subTest(os=os_release, machine=machine, image=image):
                with self.assertRaises(helper.Refusal) as refused:
                    helper.qualify_host(os_release, machine, image)
                self.assertEqual(refused.exception.code, 'image_based_host' if image else 'family_unqualified')

    def test_assets_and_review_guard_follow_the_actual_host_release(self):
        data = [release(fedora_releases=('44', '45'))]
        plan44 = helper.select_release(data, 'stable', packages(), fedora('44'))
        plan45 = helper.select_release(data, 'stable', packages(), fedora('45'))
        self.assertEqual([asset['name'] for asset in plan44['assets']], ['Polaris-fedora44-x86_64.rpm', 'Polaris-kms-fedora44-x86_64.rpm'])
        self.assertEqual([asset['name'] for asset in plan45['assets']], ['Polaris-fedora45-x86_64.rpm', 'Polaris-kms-fedora45-x86_64.rpm'])
        self.assertTrue(all(asset['url'].endswith('/' + asset['name']) for asset in plan45['assets']))
        self.assertNotEqual(plan44['review_guard'], plan45['review_guard'])
        self.assertEqual(plan45['host'], fedora('45'))
        # The same release and installed state reviewed on the other host must never share a guard.
        self.assertEqual(helper.select_release(data, 'stable', packages(), fedora('45'))['review_guard'], plan45['review_guard'])

    def test_release_without_the_hosts_own_assets_refuses_without_cross_release_fallback(self):
        for host_release, published in (('45', ('44',)), ('44', ('45',))):
            with self.subTest(host=host_release, published=published):
                with self.assertRaises(helper.Refusal) as refused:
                    helper.select_release([release(fedora_releases=published)], 'stable', packages(), fedora(host_release))
                self.assertEqual(refused.exception.code, 'release_unverifiable')
        mixed = release(fedora_releases=('45',))
        mixed['assets'][1]['name'] = 'Polaris-kms-fedora44-x86_64.rpm'
        mixed['assets'][1]['browser_download_url'] = helper.UPSTREAM + 'v1.5.1/Polaris-kms-fedora44-x86_64.rpm'
        with self.assertRaises(helper.Refusal):
            helper.select_release([mixed], 'stable', packages(), fedora('45'))

    def test_make_plan_binds_the_qualified_host_and_refuses_unqualified_host_hints(self):
        plan = helper.make_plan('stable', fetch=lambda: [release(fedora_releases=('44', '45'))],
                                read_installed=packages, host=lambda: fedora('45'))
        self.assertEqual([asset['name'] for asset in plan['assets']], ['Polaris-fedora45-x86_64.rpm', 'Polaris-kms-fedora45-x86_64.rpm'])
        self.assertEqual(plan['host'], fedora('45'))
        for hint in ({}, fedora('46'), {'id': 'fedora', 'version_id': '45', 'arch': 'aarch64'},
                     {'id': 'bazzite', 'version_id': '45', 'arch': 'x86_64'}, {**fedora('45'), 'extra': True}, None):
            with self.subTest(hint=hint):
                with self.assertRaises(helper.Refusal) as refused:
                    helper.make_plan('stable', fetch=lambda: [release(fedora_releases=('44', '45'))],
                                     read_installed=packages, host=lambda: hint)
                self.assertEqual(refused.exception.code, 'family_unqualified')

    def test_install_downloads_and_transacts_the_fedora45_assets_for_a_fedora45_host(self):
        result, calls = self.exercise_install(fedora_release='45')
        self.assertEqual(result['state'], 'installed')
        queried = [Path(argv[-1]).name for argv in calls if '-qp' in argv and 'OBSOLETENAME' not in ''.join(argv)]
        self.assertEqual(sorted(queried), ['Polaris-fedora45-x86_64.rpm', 'Polaris-kms-fedora45-x86_64.rpm'])
        operations = [argv for argv in calls if '-U' in argv and '--test' not in argv]
        self.assertEqual(sorted(Path(path).name for path in operations[0][operations[0].index('--') + 1:]),
                         ['Polaris-fedora45-x86_64.rpm', 'Polaris-kms-fedora45-x86_64.rpm'])

    def test_per_package_query_distinguishes_absent_unknown_and_version(self):
        def run(argv):
            return (0, 'polaris|0|1.5.1~beta.2|1|x86_64\n') if argv[-1] == 'polaris' else (1, 'package polaris-kms is not installed\n')
        current = helper.installed_packages(run=run)
        self.assertEqual(current['polaris']['version'], '1.5.1-beta.2')
        self.assertEqual(current['polaris-kms']['state'], 'absent')
        self.assertEqual(helper.installed_packages(run=lambda argv: (1, 'database error'))['polaris']['state'], 'unknown')
        duplicates = 'polaris|0|1.5.0|1|x86_64\npolaris|0|1.5.1|1|x86_64\n'
        self.assertEqual(helper.installed_packages(run=lambda argv: (0, duplicates))['polaris']['state'], 'partial')


    def test_unsigned_candidate_refuses_before_key_import_or_rpm_transaction(self):
        result, calls = self.exercise_install(signature='unsigned', key_present=False)
        self.assertEqual(result['state'], 'failed')
        self.assertEqual(result['code'], 'signature_untrusted')
        self.assertFalse(result['transaction_attempted'])
        self.assertFalse(any('--import' in x or '-U' in x for x in calls))

    def test_wrong_signer_refuses_even_when_that_key_might_be_trusted_by_the_host(self):
        result, calls = self.exercise_install(signature='foreign')
        self.assertEqual(result['state'], 'failed')
        self.assertEqual(result['code'], 'signature_untrusted')
        self.assertFalse(any('--import' in x or '-U' in x for x in calls))

    def test_vendor_signature_then_absent_key_import_precedes_unchanged_transaction(self):
        result, calls = self.exercise_install(key_present=False)
        self.assertEqual(result['state'], 'installed')
        self.assertTrue(result['signatures_verified'])
        self.assertTrue(result['vendor_key_imported'])
        verify = next(i for i, x in enumerate(calls) if x[0] == helper.sys.executable)
        imported = next(i for i, x in enumerate(calls) if '--import' in x)
        transactions = [(i, x) for i, x in enumerate(calls) if '-U' in x]
        self.assertLess(verify, imported)
        self.assertLess(imported, transactions[0][0])
        self.assertEqual([x for x in transactions[0][1] if x != '--test'], transactions[1][1])
        self.assertEqual(sum('--import' in x for x in calls), 1)
        self.assertEqual(sum('--list' in x for x in calls), 2)
        self.assertNotIn('--nosignature', transactions[1][1])

    def test_vendor_key_present_skips_import_but_still_verifies_candidate(self):
        result, calls = self.exercise_install()
        self.assertEqual(result['state'], 'installed')
        self.assertTrue(result['signatures_verified'])
        self.assertFalse(result['vendor_key_imported'])
        self.assertFalse(result['vendor_key_import_attempted'])
        self.assertEqual(sum(x[0] == helper.sys.executable for x in calls), 1)
        self.assertFalse(any('--import' in x for x in calls))

    def test_key_import_failure_never_starts_transaction_or_claims_import_success(self):
        result, calls = self.exercise_install(key_present=False, import_fails=True)
        self.assertEqual(result['state'], 'failed')
        self.assertEqual(result['code'], 'vendor_key_unavailable')
        self.assertIsNone(result['vendor_key_imported'])
        self.assertTrue(result['vendor_key_import_attempted'])
        self.assertFalse(any('-U' in x for x in calls))

    def test_unknown_system_keyring_is_not_absent_and_never_triggers_import(self):
        result, calls = self.exercise_install(key_present=False, key_lookup_error=True)
        self.assertEqual(result['state'], 'failed')
        self.assertEqual(result['code'], 'vendor_key_unavailable')
        self.assertFalse(any('--import' in x or '-U' in x for x in calls))

    def test_successful_import_with_unconfirmed_readback_remains_failed_and_logged(self):
        result, calls = self.exercise_install(key_present=False, import_readback_missing=True)
        self.assertEqual(result['state'], 'failed')
        self.assertTrue(result['vendor_key_imported'])
        self.assertFalse(result['transaction_attempted'])
        self.assertFalse(any('-U' in x for x in calls))

    def test_shipped_vendor_key_bytes_and_primary_fingerprint_are_pinned(self):
        key = Path(__file__).parents[2] / 'packaging/linux/keys/polaris.gpg'
        raw = key.read_bytes()
        self.assertEqual(hashlib.sha256(raw).hexdigest(), '3387618dcfa246abb5c53abc3a27893791a2de34916fc0954e6195b3a3bb0fe1')
        # The retained vendor key starts with one old-format tag-6/v4 RSA packet.
        # RFC4880's primary fingerprint hashes 0x99 + its two-byte size + body.
        armor = raw.decode().split('\n\n', 1)[1].split('-----END', 1)[0]
        packet = base64.b64decode(''.join(x for x in armor.splitlines() if x and not x.startswith('=')), validate=True)
        self.assertEqual(packet[0], 0x99)
        size = int.from_bytes(packet[1:3], 'big')
        self.assertEqual(packet[3], 4)
        self.assertEqual(hashlib.sha1(packet[:3 + size]).hexdigest().upper(), '58017EDFFA9F803E07ED26F835F13F14FAAD15CC')
        self.assertEqual(helper.VENDOR_FINGERPRINT, '58017EDFFA9F803E07ED26F835F13F14FAAD15CC')


if __name__ == '__main__':
    unittest.main()
