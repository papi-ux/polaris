#!/usr/bin/env python3
import copy
import unittest

from bounded import BuildError
from manifest import build_manifest, build_attestation, dependency_lock, target_lock, toolchain_lock, TOOLS


def ref(name):
    return {'path': name, 'sha256': 'a' * 64}


def inputs():
    return {'schema_version': 1, 'kind': 'polaris-sysext-build-inputs',
            'source': {'commit': 'b' * 40, 'tree': 'c' * 40},
            'candidate': ref('candidate.rpm'),
            'binary': {'path': 'usr/bin/polaris-1.2.3', 'sha256': 'd' * 64, 'version': '1.2.3'},
            'build_receipt': ref('rpm-build.json'), 'dependencies': ref('dependencies.json'),
            'target': ref('target.json'), 'toolchain': ref('tools.json')}


def target():
    return {'schema_version': 1, 'kind': 'polaris-sysext-target', 'variant': 'bazzite',
            'image_digest': 'sha256:' + 'a' * 64, 'ostree_commit': 'b' * 64,
            'architecture': 'x86_64', 'version_id': '44', 'inventory': ref('root-inventory.json'),
            'rpmdb': 'usr/share/rpm', 'policy': 'etc/selinux/targeted/contexts/files/file_contexts'}


def dependencies():
    return {'schema_version': 1, 'kind': 'polaris-sysext-dependencies',
            'target_digests': [target()['image_digest']],
            'keys': [{**ref('fedora-key.asc'), 'fingerprint': 'e' * 40}],
            'packages': [{**ref('runtime.rpm'), 'name': 'runtime', 'nevra': 'runtime-0:1.0-1.x86_64',
                          'architecture': 'x86_64', 'license': 'MIT', 'source_url': 'https://packages.example/runtime-1.0.rpm'}]}


class ManifestTests(unittest.TestCase):
    def test_admits_bound_inputs_and_attestation(self):
        manifest = build_manifest(inputs())
        receipt = {'schema_version': 1, 'kind': 'polaris-rpm-build', 'source': manifest['source'],
                   'candidate': manifest['candidate'], 'binary': manifest['binary'],
                   'nevra': 'polaris-0:1.2.3-1.x86_64', 'architecture': 'x86_64', 'build_mode': 'release'}
        build_attestation(receipt, manifest)
        for field, value in [('architecture', 'aarch64'), ('build_mode', 'debug'),
                             ('source', {'commit': 'f' * 40, 'tree': 'c' * 40}),
                             ('binary', {**manifest['binary'], 'sha256': 'f' * 64})]:
            with self.subTest(field=field), self.assertRaises(BuildError):
                build_attestation({**receipt, field: value}, manifest)

    def test_rejects_ambiguous_paths_fields_and_versions(self):
        for update in ({'schema_version': True}, {'unexpected': 1},
                       {'candidate': ref('../candidate.rpm')}, {'candidate': ref('candidate.raw')},
                       {'dependencies': ref('target.json')},
                       {'source': {'commit': 'x' * 40, 'tree': 'b' * 40}},
                       {'binary': {'path': 'usr/bin/polaris-other', 'sha256': 'a' * 64, 'version': '1.2.3'}}):
            with self.subTest(update=update), self.assertRaises(BuildError):
                build_manifest({**inputs(), **update})

    def test_target_requires_known_family_architecture_and_layout(self):
        target_lock(target())
        for update in ({'variant': 'unknown'}, {'architecture': 'aarch64'}, {'rpmdb': 'var/lib/rpm'},
                       {'policy': '/etc/policy'}, {'version_id': '45'}, {'image_digest': 'latest'}):
            with self.subTest(update=update), self.assertRaises(BuildError):
                target_lock({**target(), **update})

    def test_dependencies_are_unique_and_target_bound(self):
        lock = dependencies()
        dependency_lock(lock, target()['image_digest'])
        for mutate in (lambda v: v['packages'].append(v['packages'][0]),
                       lambda v: v['target_digests'].clear(),
                       lambda v: v['packages'][0].update(source_url='https://user:pass@example.test/file'),
                       lambda v: v['packages'][0].update(source_url='https://example.test/file?token=secret'),
                       lambda v: v['packages'][0].update(architecture='aarch64'),
                       lambda v: v['keys'][0].update(fingerprint='bad')):
            changed = copy.deepcopy(lock)
            mutate(changed)
            with self.assertRaises(BuildError):
                dependency_lock(changed, target()['image_digest'])
        with self.assertRaises(BuildError):
            dependency_lock(lock, 'sha256:' + 'f' * 64)

    def test_toolchain_requires_every_tool(self):
        lock = {'schema_version': 1, 'kind': 'polaris-sysext-toolchain', 'builder_digest': 'sha256:' + 'a' * 64,
                'tools': {name: {'path': 'usr/bin/' + name, 'sha256': 'b' * 64, 'package': name + '-1.0'} for name in TOOLS}}
        toolchain_lock(lock)
        del lock['tools']['rpmkeys']
        with self.assertRaises(BuildError):
            toolchain_lock(lock)


if __name__ == '__main__':
    unittest.main()
