#!/usr/bin/env python3
"""Build, check and export one locked worker profile without network access."""
import argparse
import contextlib
import hashlib
import json
import pathlib
import re
import shutil
import subprocess
import tarfile
import tempfile
import tomllib
import urllib.parse

from oci_archive import verify_archive

HERE = pathlib.Path(__file__).resolve().parent
REPO = HERE.parent.parent


def run(arguments, **kwargs):
    return subprocess.run(arguments, cwd=REPO, check=True, **kwargs)


def output(arguments):
    return subprocess.check_output(arguments, cwd=REPO, text=True)


def digest(path):
    with path.open('rb') as stream:
        return hashlib.file_digest(stream, 'sha256').hexdigest()


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n')


@contextlib.contextmanager
def materialized_context(revision, profile_id, nvidia):
    # An ignored Go file can still be compiled by COPY *.go. Archive the exact
    # committed sources/locks; neither ignored files nor concurrent edits enter.
    with tempfile.TemporaryDirectory(prefix='polaris-worker-context-') as temporary:
        context = pathlib.Path(temporary)
        archive_path = context / 'source.tar'
        with archive_path.open('wb') as stream:
            run(['git', 'archive', revision, 'multiseat_worker', 'containers/multiseat'], stdout=stream)
        with tarfile.open(archive_path) as archive:
            for member in archive.getmembers():
                name = pathlib.PurePosixPath(member.name)
                if name.is_absolute() or '..' in name.parts or not (member.isfile() or member.isdir()):
                    raise ValueError('build source must contain only regular files and directories')
                destination = context / name
                if member.isdir():
                    destination.mkdir(parents=True, exist_ok=True)
                else:
                    destination.parent.mkdir(parents=True, exist_ok=True)
                    with archive.extractfile(member) as source, destination.open('wb') as target:
                        shutil.copyfileobj(source, target)
                    destination.chmod(member.mode & 0o755)
        archive_path.unlink()
        here = context / 'containers/multiseat'
        images = json.loads((here / 'images.lock.json').read_text())
        profile = next(p for p in images['runtime_profiles'] if p['id'] == profile_id)
        packages = json.loads((here / profile['dependency_lock']).read_text())
        inputs = []
        for role in ['runtime', 'build']:
            for package in packages[role]:
                filename = package['filename']
                if pathlib.Path(filename).name != filename:
                    raise ValueError('unsafe locked input filename')
                inputs.append((pathlib.Path(profile_id) / role / filename, package['sha256']))
        for name in ['rust', 'plugin', 'gamescope'] + (['nvidia'] if nvidia else []):
            lock = json.loads((here / images['dependency_locks'][name]).read_text())
            filename = pathlib.Path(name + '.tar')
            if name in ['rust', 'nvidia']:
                filename = pathlib.Path(pathlib.PurePosixPath(lock['url']).name)
                if name == 'rust':
                    filename = pathlib.Path('toolchains') / filename
            inputs.append((filename, lock['sha256']))
        for filename, expected in inputs:
            source = REPO / 'build/runtime-inputs' / filename
            if source.is_symlink() or not source.is_file():
                raise ValueError('cached input must be a regular file')
            destination = context / 'build/runtime-inputs' / filename
            destination.parent.mkdir(parents=True, exist_ok=True)
            shutil.copyfile(source, destination)
            if digest(destination) != expected:
                raise ValueError('copied input differs from committed lock: ' + str(filename))
        yield context


def sbom(packages, profile, revision, context):
    here = context / 'containers/multiseat'
    components = []
    components.append({'type': 'application', 'name': 'polaris-input-pong',
                       'version': revision, 'bom-ref': 'polaris-input-pong',
                       'properties': [{'name': 'polaris:workload-id', 'value': 'input-pong-v1'},
                                      {'name': 'polaris:source-sha256', 'value': digest(here / 'workloads/input-pong.c')}],
                       'externalReferences': [{'type': 'vcs', 'url': 'https://github.com/papi-ux/polaris/blob/' + revision + '/containers/multiseat/workloads/input-pong.c'}]})
    for name in ['capture-input.c', 'seat-input.c', 'seat-input.h', 'game-status.c', 'encoded-game-check.c']:
        components.append({'type': 'file', 'name': 'polaris-input-provider/' + name,
                           'version': revision, 'bom-ref': 'polaris-input-provider/' + name,
                           'hashes': [{'alg': 'SHA-256', 'content': digest(here / 'providers' / name)}]})
    for line in packages.splitlines():
        name, version, architecture = line.split('\t')
        purl = 'pkg:deb/ubuntu/' + urllib.parse.quote(name, safe='') + '@' + urllib.parse.quote(version, safe='') + '?arch=' + architecture
        components.append({'type': 'library', 'name': name, 'version': version, 'purl': purl, 'bom-ref': purl})
    for name in ['plugin', 'gamescope']:
        lock = json.loads((here / 'locks' / (name + '.json')).read_text())
        components.append({'type': 'application' if name == 'gamescope' else 'library',
                           'name': name, 'version': lock['revision'], 'bom-ref': name,
                           'externalReferences': [{'type': 'vcs', 'url': lock['url'] + '/tree/' + lock['revision']}],
                           'properties': [{'name': 'polaris:source-archive-sha256', 'value': lock['sha256']}]})
    # Retain the complete reviewed Rust closure, including declared build/dev
    # inputs; this is dependency provenance, not a claim that every crate links.
    with tarfile.open(context / 'build/runtime-inputs/plugin.tar') as archive:
        source = tomllib.loads(archive.extractfile('./Cargo.lock').read().decode())
    for package in source['package']:
        name, version = package['name'], package['version']
        reference = 'rust:' + name + '@' + version
        component = {'type': 'library', 'name': name, 'version': version, 'bom-ref': reference,
                     'properties': [{'name': 'polaris:scope', 'value': 'declared plugin build closure'}]}
        if package.get('source', '').startswith('registry+'):
            component['purl'] = 'pkg:cargo/' + name + '@' + version
        if 'checksum' in package:
            component['hashes'] = [{'alg': 'SHA-256', 'content': package['checksum']}]
        components.append(component)
    return {'bomFormat': 'CycloneDX', 'specVersion': '1.6', 'version': 1,
            'metadata': {'component': {'type': 'application', 'name': 'polaris-worker-' + profile,
                                       'version': revision, 'bom-ref': 'worker'}},
            'components': components}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('profile', choices=['gamescope', 'steam', 'heroic', 'lutris'])
    parser.add_argument('--nvidia', action='store_true')
    args = parser.parse_args()
    revision = output(['git', 'rev-parse', 'HEAD']).strip()
    if output(['git', 'status', '--porcelain']):
        parser.error('commit the reviewed source before producing acceptance artifacts')
    epoch = output(['git', 'show', '-s', '--format=%ct', revision]).strip()
    with materialized_context(revision, args.profile, args.nvidia) as context:
        build_artifact(args, revision, epoch, context)


def build_artifact(args, revision, epoch, context):
    here = context / 'containers/multiseat'
    images = json.loads((here / 'images.lock.json').read_text())
    profile = next(p for p in images['runtime_profiles'] if p['id'] == args.profile)
    packages = json.loads((here / profile['dependency_lock']).read_text())
    if images['schema'] != 2 or images['platform'] != 'linux/amd64' or packages['source_root'] != profile['reference']:
        raise ValueError('source root, dependency lock and architecture must agree')
    variant = 'nvidia' if args.nvidia else 'default'
    artifact = REPO / 'build/worker-artifacts' / args.profile / variant
    artifact.mkdir(parents=True, exist_ok=True)
    image = 'localhost/polaris-worker-' + args.profile + '-' + variant + ':' + revision[:12]
    command = ['podman', 'build', '--network=none', '--pull=never', '--platform=linux/amd64',
               '--format=oci', '--timestamp=' + epoch, '-f', str(here / 'Containerfile'),
               '--build-arg', 'RUNTIME_PROFILE=' + args.profile,
               '--build-arg', 'RUNTIME_IMAGE=' + profile['reference'],
               '--build-arg', 'GO_BUILDER_IMAGE=' + images['builder']['reference'],
               '--build-arg', 'POLARIS_REVISION=' + revision]
    run(command + ['--target', 'worker-nvidia' if args.nvidia else 'worker', '-t', image, str(context)])
    inspected = json.loads(output(['podman', 'image', 'inspect', image]))[0]
    labels = inspected['Labels']
    if inspected['Architecture'] != 'amd64' or inspected['Os'] != 'linux' or labels.get('org.opencontainers.image.revision') != revision or labels.get('io.polaris.multiseat.profile') != args.profile:
        raise ValueError('produced worker identity does not match the build')
    config_digest = 'sha256:' + inspected['Id'].removeprefix('sha256:')

    # CI covers all device-free real providers and their independent teardown.
    # Gamescope hardware acceptance is a separate required physical receipt.
    provider_image = image + '-providers'
    run(command + ['--target', 'provider-nvidia-test' if args.nvidia else 'provider-test', '-t', provider_image, str(context)])
    provider_inspected = json.loads(output(['podman', 'image', 'inspect', provider_image]))[0]
    worker_layers = inspected['RootFS']['Layers']
    if provider_inspected['RootFS']['Layers'][:len(worker_layers)] != worker_layers:
        raise ValueError('provider test image does not extend the produced worker filesystem')
    test_command = ['podman', 'run', '--rm', '--network=none', '--cap-drop=all',
                    '--security-opt=no-new-privileges', provider_image, '-test.v',
                    '-test.run=^TestReal(SessionBus|PrivateAudio|AudioReadiness|Display)', '-test.timeout=2m']
    with (artifact / 'providers.log').open('w') as log:
        run(test_command, stdout=log, stderr=subprocess.STDOUT)
    results = (artifact / 'providers.log').read_text()
    names = re.findall(r'^--- PASS: ([A-Za-z0-9_]+)', results, re.MULTILINE)
    required = {
        'TestRealSessionBusAuthenticatesAndCleansUp',
        'TestRealPrivateAudioGraphRoutesExactlyAndCleansUp',
        'TestRealAudioReadinessFailureCleansPartialArtifacts',
        'TestRealPrivateAudioGraphsRemainIndependent',
        'TestRealDisplayCaptureProducesFrameAndCleansUp',
        'TestRealDisplayCapturesRemainIndependent',
    }
    if '--- SKIP:' in results or set(names) != required or len(names) != len(required):
        raise ValueError('real provider tests skipped or did not all execute')
    write_json(artifact / 'providers.json', {'schema': 1, 'result': 'passed', 'tests': names,
                                            'variant': variant, 'worker_config_digest': config_digest,
                                            'provider_config_digest': 'sha256:' + provider_inspected['Id'].removeprefix('sha256:'),
                                            'scope': 'isolated session bus, audio and software display; no game stream'})
    package_manifest = output(['podman', 'run', '--rm', '--network=none', '--read-only',
                               '--cap-drop=all', '--security-opt=no-new-privileges',
                               '--entrypoint=/usr/bin/cat', image, '/usr/share/polaris/build/packages.tsv'])
    (artifact / 'packages.tsv').write_text(package_manifest)
    bill = sbom(package_manifest, args.profile, revision, context)
    if args.nvidia:
        nvidia = json.loads((here / 'locks/nvidia.json').read_text())
        bill['components'].append({'type': 'library', 'name': 'nvidia-graphics-userspace',
                                   'version': nvidia['version'], 'bom-ref': 'nvidia-userspace',
                                   'properties': [{'name': 'polaris:source-archive-sha256', 'value': nvidia['sha256']}]})
    write_json(artifact / 'sbom.cdx.json', bill)
    run(['podman', 'save', '--format=oci-archive', '-o', str(artifact / 'worker.oci.tar'), image])
    # Podman may rewrite the manifest when exporting. Only the verified digest
    # inside this downloadable archive identifies the delivered worker artifact.
    worker_digest = verify_archive(artifact / 'worker.oci.tar', config_digest)
    lock_files = [here / 'images.lock.json', here / profile['dependency_lock']]
    lock_files += [here / path for name, path in images['dependency_locks'].items() if args.nvidia or name != 'nvidia']
    write_json(artifact / 'artifact.json', {
        'schema': 1, 'source_revision': revision, 'profile': args.profile,
        'platform': 'linux/amd64', 'variant': variant, 'source_root': profile['reference'],
        'worker_digest': worker_digest, 'worker_config_digest': config_digest,
        'dependency_locks': {str(path.relative_to(context)): digest(path) for path in lock_files},
        'files': {name: {'sha256': digest(artifact / name)} for name in ['worker.oci.tar', 'packages.tsv', 'sbom.cdx.json', 'providers.json']},
        'validation': {'dependencies': 'passed', 'session_bus_audio_display': 'passed',
                       'nested_compositor': 'physical receipt required', 'input': 'physical receipt required',
                       'game_streaming': 'not exercised', 'production_activation': False},
        'development': False, 'publication': 'downloadable artifact; no registry or production catalog mutation',
    })
    print(artifact.relative_to(REPO))


if __name__ == '__main__':
    main()
