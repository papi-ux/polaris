"""Read and verify the single-platform OCI artifact without extracting files."""
import hashlib
import gzip
import json
import re
import tarfile


def verify_archive(path, expected_config):
    with tarfile.open(path) as archive:
        members = {}
        for member in archive.getmembers():
            if member.name in members or not (member.isfile() or member.isdir()):
                raise ValueError('duplicate or non-regular OCI archive member')
            if member.name.startswith('/') or '..' in member.name.split('/'):
                raise ValueError('unsafe OCI archive member')
            members[member.name] = member

        def read_json(name):
            member = members[name]
            if not member.isfile() or member.size > 16 * 1024 * 1024:
                raise ValueError('invalid OCI JSON member')
            return json.load(archive.extractfile(member))

        def verify_descriptor(descriptor):
            value = descriptor['digest']
            if not re.fullmatch(r'sha256:[0-9a-f]{64}', value):
                raise ValueError('unsupported OCI digest')
            name = 'blobs/sha256/' + value[7:]
            member = members[name]
            if not member.isfile() or member.size != descriptor['size']:
                raise ValueError('OCI descriptor size mismatch')
            with archive.extractfile(member) as stream:
                actual = hashlib.file_digest(stream, 'sha256').hexdigest()
            if actual != value[7:]:
                raise ValueError('OCI blob checksum mismatch')
            return name

        if read_json('oci-layout') != {'imageLayoutVersion': '1.0.0'}:
            raise ValueError('unsupported OCI layout')
        index = read_json('index.json')
        if index['schemaVersion'] != 2 or len(index['manifests']) != 1:
            raise ValueError('expected a single worker manifest')
        descriptor = index['manifests'][0]
        if descriptor['mediaType'] != 'application/vnd.oci.image.manifest.v1+json':
            raise ValueError('expected an OCI image manifest')
        manifest = read_json(verify_descriptor(descriptor))
        if manifest['schemaVersion'] != 2 or manifest['config']['digest'] != expected_config:
            raise ValueError('exported worker configuration differs from the validated image')
        config = read_json(verify_descriptor(manifest['config']))
        if config['architecture'] != 'amd64' or config['os'] != 'linux':
            raise ValueError('exported worker architecture mismatch')
        rootfs = config.get('rootfs', {})
        diff_ids = rootfs.get('diff_ids', [])
        if rootfs.get('type') != 'layers' or len(diff_ids) != len(manifest['layers']):
            raise ValueError('exported layer count differs from validated configuration')
        for layer, expected in zip(manifest['layers'], diff_ids):
            name = verify_descriptor(layer)
            media_type = layer['mediaType']
            if media_type not in ('application/vnd.oci.image.layer.v1.tar',
                                   'application/vnd.oci.image.layer.v1.tar+gzip'):
                raise ValueError('unsupported OCI layer encoding')
            with archive.extractfile(members[name]) as raw:
                if media_type.endswith('+gzip'):
                    with gzip.GzipFile(fileobj=raw) as stream:
                        actual = hashlib.file_digest(stream, 'sha256').hexdigest()
                else:
                    actual = hashlib.file_digest(raw, 'sha256').hexdigest()
            if expected != 'sha256:' + actual:
                raise ValueError('exported layer contents or order differ from validated configuration')
        return descriptor['digest']
