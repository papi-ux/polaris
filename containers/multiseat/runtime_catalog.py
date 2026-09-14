#!/usr/bin/env python3
"""Prepare a reviewable catalog candidate from verified build and registry bytes.

This does not publish an image or modify the catalog trusted by Polaris.
"""
import argparse
import hashlib
import json
import pathlib
import re
import tarfile

from oci_archive import verify_archive

REQUIRED_PROVIDER_TESTS = {
    'TestRealSessionBusAuthenticatesAndCleansUp',
    'TestRealEncoderBridgeKeepsTwoSeatsIndependent',
    'TestRealPrivateAudioGraphRoutesExactlyAndCleansUp',
    'TestRealAudioReadinessFailureCleansPartialArtifacts',
    'TestRealPrivateAudioGraphsRemainIndependent',
    'TestRealPrivateAudioPolicyRejectsWrongTargetsAndReplacement',
    'TestRealPrivateAudioPolicyDeathRetiresProvider',
    'TestRealDisplayCaptureProducesFrameAndCleansUp',
    'TestRealDisplayCapturesRemainIndependent',
}


def checksum(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def unique(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError('duplicate metadata key')
        result[key] = value
    return result


def metadata(path):
    if path.is_symlink() or not path.is_file() or path.stat().st_size > 1024 * 1024:
        raise ValueError('metadata must be a bounded regular file')
    return json.loads(path.read_bytes(), object_pairs_hook=unique)


def is_digest(value):
    return isinstance(value, str) and re.fullmatch(r'sha256:[0-9a-f]{64}', value) is not None


def validate_catalog(catalog):
    if (not isinstance(catalog, dict) or set(catalog) != {'schema', 'runtimes'} or
            type(catalog['schema']) is not int or catalog['schema'] != 1 or
            not isinstance(catalog['runtimes'], list) or len(catalog['runtimes']) > 16):
        raise ValueError('unsupported runtime catalog')
    ids, digests = set(), set()
    for entry in catalog['runtimes']:
        expected = {'id', 'profile', 'variant', 'platform', 'media_contract', 'uid', 'gid',
                    'source_revision', 'registry_digest', 'config_digest', 'nvidia_driver'}
        if not isinstance(entry, dict) or set(entry) != expected:
            raise ValueError('unexpected runtime fields')
        strings = expected - {'media_contract', 'uid', 'gid'}
        if any(not isinstance(entry[key], str) for key in strings):
            raise ValueError('runtime identity fields must be strings')
        if (not re.fullmatch(r'[a-z0-9][a-z0-9-]{0,63}', entry['id']) or
                entry['id'] in ids or entry['registry_digest'] in digests or
                entry['profile'] != 'steam' or entry['platform'] != 'linux/amd64' or
                not re.fullmatch(r'[0-9a-f]{40}', entry['source_revision']) or
                not is_digest(entry['registry_digest']) or not is_digest(entry['config_digest']) or
                any(type(entry[key]) is not int or entry[key] != value
                    for key, value in [('media_contract', 1), ('uid', 1000), ('gid', 1000)])):
            raise ValueError('incompatible or ambiguous runtime identity')
        driver = entry['nvidia_driver']
        if not ((entry['variant'] == 'default' and not driver) or
                (entry['variant'] == 'nvidia' and 3 <= len(driver) <= 32 and
                 re.fullmatch(r'[0-9]+(?:\.[0-9]+)+', driver))):
            raise ValueError('unsupported runtime driver variant')
        ids.add(entry['id'])
        digests.add(entry['registry_digest'])
    return catalog


def prepare_candidate(directory, registry_digest, registry_manifest):
    directory = pathlib.Path(directory)
    artifact = metadata(directory / 'artifact.json')
    if (any(type(artifact[key]) is not int or artifact[key] != value
            for key, value in [('schema', 1), ('media_contract', 1), ('owner_uid', 1000), ('owner_gid', 1000)]) or
            artifact['development'] is not False or
            artifact['build_engine'] != 'docker' or artifact['profile'] != 'steam' or
            artifact['platform'] != 'linux/amd64' or artifact['media_contract'] != 1 or
            artifact['owner_uid'] != 1000 or artifact['owner_gid'] != 1000 or
            artifact['validation']['dependencies'] != 'passed' or
            artifact['validation']['session_bus_audio_display'] != 'passed'):
        raise ValueError('artifact is not a compatible validated Docker Steam runtime')
    required = {'worker.oci.tar', 'worker.docker.tar', 'providers.json', 'packages.tsv', 'sbom.cdx.json'}
    if artifact['variant'] == 'nvidia':
        required.update({'nvidia-files.json', 'nvidia-runtime.json'})
    if not required <= artifact['files'].keys():
        raise ValueError('artifact is missing required evidence')
    for name, receipt in artifact['files'].items():
        if not re.fullmatch(r'[a-z0-9][a-z0-9.-]{0,63}', name):
            raise ValueError('unsafe artifact filename')
        path = directory / name
        if (path.is_symlink() or not path.is_file() or set(receipt) != {'sha256', 'bytes'} or
                type(receipt['bytes']) is not int or receipt['bytes'] <= 0 or
                path.stat().st_size != receipt['bytes'] or checksum(path) != receipt['sha256']):
            raise ValueError('artifact file differs from its build receipt')
    config_digest = artifact['worker_config_digest']
    if not is_digest(config_digest) or verify_archive(directory / 'worker.oci.tar', config_digest) != artifact['worker_digest']:
        raise ValueError('worker export differs from the validated build')
    providers = metadata(directory / 'providers.json')
    tests = providers['tests']
    if (providers['schema'] != 1 or providers['result'] != 'passed' or
            providers['worker_config_digest'] != config_digest or providers['variant'] != artifact['variant'] or
            not isinstance(tests, list) or len(tests) != len(REQUIRED_PROVIDER_TESTS) or
            set(tests) != REQUIRED_PROVIDER_TESTS):
        raise ValueError('provider evidence is incomplete or belongs to another worker')
    if not is_digest(registry_digest) or 'sha256:' + hashlib.sha256(registry_manifest).hexdigest() != registry_digest:
        raise ValueError('registry manifest bytes do not match the published digest')
    if len(registry_manifest) > 65536:
        raise ValueError('registry manifest is oversized')
    manifest = json.loads(registry_manifest, object_pairs_hook=unique)
    if (manifest['schemaVersion'] != 2 or manifest['mediaType'] not in {
            'application/vnd.oci.image.manifest.v1+json', 'application/vnd.docker.distribution.manifest.v2+json'} or
            manifest['config']['digest'] != config_digest):
        raise ValueError('registry publication is not the validated worker configuration')
    with tarfile.open(directory / 'worker.oci.tar') as archive:
        config_member = archive.getmember('blobs/sha256/' + config_digest[7:])
        if (type(manifest['config']['size']) is not int or manifest['config']['size'] != config_member.size or
                manifest['config']['mediaType'] not in {'application/vnd.oci.image.config.v1+json',
                                                       'application/vnd.docker.container.image.v1+json'}):
            raise ValueError('registry configuration descriptor differs from the exported worker')
        config = json.load(archive.extractfile(config_member))
    labels = config['config']['Labels']
    if (labels['org.opencontainers.image.source'] != 'https://github.com/papi-ux/polaris' or
            labels['org.opencontainers.image.revision'] != artifact['source_revision'] or
            labels['io.polaris.multiseat.profile'] != 'steam' or
            labels['io.polaris.multiseat.architecture'] != 'linux/amd64' or
            labels['io.polaris.multiseat.media-contract'] != '1' or
            config['config']['Entrypoint'] != ['/usr/bin/polaris-seat-worker'] or config['config']['Cmd'] != ['run'] or
            any(config['config'].get(key) for key in ['Volumes', 'ExposedPorts', 'OnBuild'])):
        raise ValueError('exported worker labels or launch configuration are incompatible')
    layers = manifest['layers']
    if (not isinstance(layers, list) or len(layers) != len(config['rootfs']['diff_ids']) or
            any(not is_digest(layer['digest']) or type(layer['size']) is not int or layer['size'] <= 0 or
                layer['mediaType'] not in {'application/vnd.oci.image.layer.v1.tar',
                                          'application/vnd.oci.image.layer.v1.tar+gzip',
                                          'application/vnd.docker.image.rootfs.diff.tar.gzip'}
                for layer in layers)):
        raise ValueError('registry layer descriptors are incompatible')
    driver = labels.get('io.polaris.multiseat.nvidia.driver', '')
    if artifact['variant'] == 'nvidia':
        report = metadata(directory / 'nvidia-runtime.json')
        if (report['result'] != 'passed' or report['driver_version'] != driver or
                report['manifest_sha256'] != checksum(directory / 'nvidia-files.json')):
            raise ValueError('NVIDIA evidence differs from the worker driver')
    entry = {'id': 'steam-' + artifact['variant'] + '-' + registry_digest[7:23],
             'profile': 'steam', 'variant': artifact['variant'], 'platform': 'linux/amd64',
             'media_contract': 1, 'uid': 1000, 'gid': 1000, 'source_revision': artifact['source_revision'],
             'registry_digest': registry_digest, 'config_digest': config_digest, 'nvidia_driver': driver}
    return validate_catalog({'schema': 1, 'runtimes': [entry]})


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest='command', required=True)
    validate = sub.add_parser('validate')
    validate.add_argument('catalog', type=pathlib.Path)
    prepare = sub.add_parser('prepare')
    prepare.add_argument('artifact', type=pathlib.Path)
    prepare.add_argument('registry_digest')
    prepare.add_argument('registry_manifest', type=pathlib.Path)
    args = parser.parse_args()
    if args.command == 'validate':
        validate_catalog(metadata(args.catalog))
    else:
        if args.registry_manifest.stat().st_size > 65536:
            parser.error('registry manifest is oversized')
        print(json.dumps(prepare_candidate(args.artifact, args.registry_digest,
                                          args.registry_manifest.read_bytes()), indent=2))


if __name__ == '__main__':
    main()
