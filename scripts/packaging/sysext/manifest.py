#!/usr/bin/env python3
"""Strict package-only system-extension build contracts; no release gate reuse."""
from __future__ import annotations

import re
from urllib.parse import urlsplit

from bounded import require, relative_path

HEX = re.compile(r'[0-9a-f]+\Z')
TOOLS = {'rpm', 'rpmkeys', 'rpm2cpio', 'mksquashfs', 'unsquashfs', 'setfiles', 'gpg', 'python3'}


def object_keys(value, keys, label):
    require(isinstance(value, dict) and set(value) == set(keys), 'unexpected fields in ' + label)
    return value


def text(value, label):
    require(isinstance(value, str) and value and '\0' not in value and '\n' not in value and '\r' not in value, 'invalid ' + label)
    return value


def digest(value, width=64):
    require(isinstance(value, str) and len(value) == width and HEX.fullmatch(value) is not None, 'invalid digest')
    return value


def file_ref(value):
    object_keys(value, {'path', 'sha256'}, 'file reference')
    relative_path(value['path'])
    digest(value['sha256'])
    return value


def source(value):
    object_keys(value, {'commit', 'tree'}, 'source')
    for item in value.values():
        digest(item, 40)


def versioned(value, kind):
    require(type(value.get('schema_version')) is int and value['schema_version'] == 1, 'unsupported schema version')
    require(value.get('kind') == kind, 'incorrect document kind')


def package(value):
    object_keys(value, {'path', 'sha256', 'name', 'nevra', 'architecture', 'license', 'source_url'}, 'package')
    file_ref({key: value[key] for key in ('path', 'sha256')})
    require(value['path'].endswith('.rpm'), 'package is not an RPM')
    require(value['architecture'] in ('x86_64', 'noarch'), 'unsupported package architecture')
    require(re.fullmatch(r'[a-zA-Z0-9][a-zA-Z0-9+_.-]*', text(value['name'], 'package name')) is not None, 'invalid package name')
    require(text(value['nevra'], 'NEVRA').startswith(value['name'] + '-') and value['nevra'].endswith('.' + value['architecture']), 'inconsistent NEVRA')
    text(value['license'], 'package license')
    url = urlsplit(text(value['source_url'], 'package source URL'))
    require(url.scheme == 'https' and url.hostname and not (url.username or url.password or url.query or url.fragment), 'package source must be credential-free HTTPS')


def build_manifest(value):
    object_keys(value, {'schema_version', 'kind', 'source', 'candidate', 'binary', 'build_receipt', 'dependencies', 'target', 'toolchain'}, 'build manifest')
    versioned(value, 'polaris-sysext-build-inputs')
    source(value['source'])
    file_ref(value['candidate'])
    require(value['candidate']['path'].endswith('.rpm'), 'candidate is not an RPM')
    binary = object_keys(value['binary'], {'path', 'sha256', 'version'}, 'binary')
    digest(binary['sha256'])
    relative_path(binary['path'])
    require(binary['path'].startswith('usr/bin/polaris-'), 'unexpected Polaris executable path')
    require(re.fullmatch(r'[A-Za-z0-9.+_-]+', text(binary['version'], 'binary version')) is not None, 'invalid binary version')
    require(binary['path'] == 'usr/bin/polaris-' + binary['version'], 'binary filename/version mismatch')
    for key in ('build_receipt', 'dependencies', 'target', 'toolchain'):
        file_ref(value[key])
    refs = [value[key]['path'] for key in ('candidate', 'build_receipt', 'dependencies', 'target', 'toolchain')]
    require(len(refs) == len(set(refs)), 'duplicate input destinations')
    return value


def build_attestation(value, manifest):
    object_keys(value, {'schema_version', 'kind', 'source', 'candidate', 'binary', 'nevra', 'architecture', 'build_mode'}, 'RPM build attestation')
    versioned(value, 'polaris-rpm-build')
    require(value['source'] == manifest['source'] and value['candidate'] == manifest['candidate'] and value['binary'] == manifest['binary'], 'RPM build attestation mismatch')
    require(value['architecture'] == 'x86_64' and value['build_mode'] == 'release', 'incompatible RPM build')
    require(text(value['nevra'], 'candidate NEVRA').startswith('polaris-') and value['nevra'].endswith('.x86_64'), 'invalid candidate NEVRA')
    return value


def dependency_lock(value, target_digest):
    object_keys(value, {'schema_version', 'kind', 'target_digests', 'keys', 'packages'}, 'dependency lock')
    versioned(value, 'polaris-sysext-dependencies')
    require(isinstance(value['target_digests'], list) and value['target_digests'], 'missing dependency target binding')
    for target in value['target_digests']:
        require(isinstance(target, str) and target.startswith('sha256:'), 'invalid target digest')
        digest(target[7:])
    require(target_digest in value['target_digests'], 'dependency lock targets a different image')
    require(isinstance(value['keys'], list) and 0 < len(value['keys']) <= 16, 'invalid package signing key set')
    for key in value['keys']:
        object_keys(key, {'path', 'sha256', 'fingerprint'}, 'signing key')
        file_ref({field: key[field] for field in ('path', 'sha256')})
        digest(key['fingerprint'], 40)
    require(isinstance(value['packages'], list) and len(value['packages']) <= 256, 'invalid dependency count')
    for item in value['packages']:
        package(item)
    for key in ('path', 'name', 'nevra'):
        names = [item[key] for item in value['packages']]
        require(len(names) == len(set(names)), 'duplicate dependency ' + key)
    paths = [item['path'] for item in [*value['keys'], *value['packages']]]
    require(len(paths) == len(set(paths)), 'duplicate locked input path')
    return value


def target_lock(value):
    object_keys(value, {'schema_version', 'kind', 'variant', 'image_digest', 'ostree_commit', 'architecture', 'version_id', 'inventory', 'rpmdb', 'policy'}, 'target lock')
    versioned(value, 'polaris-sysext-target')
    require(value['variant'] in ('bazzite', 'bazzite-nvidia-open'), 'unsupported image family')
    require(value['architecture'] == 'x86_64' and value['version_id'] == '44', 'unsupported target')
    require(isinstance(value['image_digest'], str) and value['image_digest'].startswith('sha256:'), 'invalid target digest')
    digest(value['image_digest'][7:])
    digest(value['ostree_commit'])
    file_ref(value['inventory'])
    require(value['rpmdb'] == 'usr/share/rpm', 'unsupported RPMDB layout')
    require(value['policy'] == 'etc/selinux/targeted/contexts/files/file_contexts', 'unsupported SELinux policy layout')
    return value


def toolchain_lock(value):
    object_keys(value, {'schema_version', 'kind', 'builder_digest', 'tools'}, 'toolchain lock')
    versioned(value, 'polaris-sysext-toolchain')
    require(isinstance(value['builder_digest'], str) and value['builder_digest'].startswith('sha256:'), 'invalid builder digest')
    digest(value['builder_digest'][7:])
    require(isinstance(value['tools'], dict) and set(value['tools']) == TOOLS, 'incomplete toolchain lock')
    for tool, record in value['tools'].items():
        object_keys(record, {'path', 'sha256', 'package'}, 'tool')
        require(re.fullmatch(r'usr/(?:s?bin)/' + re.escape(tool) + (r'(?:\.[0-9]+)?' if tool == 'python3' else r'2?' if tool == 'gpg' else ''), record['path']) is not None, 'unexpected tool executable path')
        digest(record['sha256'])
        text(record['package'], 'tool package identity')
    return value
