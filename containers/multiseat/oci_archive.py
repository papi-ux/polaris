"""Read and verify the single-platform OCI artifact without extracting files."""
import hashlib
import gzip
import io
import json
import re
import tarfile


def docker_config_digest(path, image_id):
    """Bind the saved config to either classic Docker or containerd image IDs."""
    if not re.fullmatch(r'sha256:[0-9a-f]{64}', image_id):
        raise ValueError('invalid inspected Docker image ID')
    with tarfile.open(path) as archive:
        members = {}
        for member in archive.getmembers():
            if (member.name in members or not (member.isfile() or member.isdir()) or
                    member.name.startswith('/') or '..' in member.name.split('/')):
                raise ValueError('unsafe or duplicate Docker archive member')
            members[member.name] = member

        def read(name):
            member = members.get(name)
            if member is None or not member.isfile() or member.size > 16 * 1024 * 1024:
                raise ValueError('missing or invalid Docker image metadata')
            return archive.extractfile(member).read()

        manifest = json.loads(read('manifest.json'))
        if not isinstance(manifest, list) or len(manifest) != 1:
            raise ValueError('expected one saved Docker image')
        config = read(manifest[0]['Config'])
        checksum = 'sha256:' + hashlib.sha256(config).hexdigest()
        if checksum == image_id:
            return checksum
        # With the containerd store, Id identifies an OCI manifest or index.
        # Verify the complete descriptor chain; labels alone cannot bind bytes.
        current = image_id
        expected_size = None
        for _ in range(4):
            payload = read('blobs/sha256/' + current[7:])
            if ('sha256:' + hashlib.sha256(payload).hexdigest() != current or
                    expected_size is not None and len(payload) != expected_size):
                raise ValueError('Docker image descriptor differs from inspected ID')
            node = json.loads(payload)
            if node.get('schemaVersion') != 2:
                raise ValueError('unsupported Docker image descriptor')
            if 'config' in node:
                if node['config']['digest'] != checksum or node['config']['size'] != len(config):
                    raise ValueError('saved config differs from inspected Docker image')
                return checksum
            children = node.get('manifests', [])
            if len(children) != 1:
                raise ValueError('expected a single-platform Docker image index')
            current, expected_size = children[0]['digest'], children[0]['size']
            if not re.fullmatch(r'sha256:[0-9a-f]{64}', current):
                raise ValueError('unsupported Docker descriptor digest')
        raise ValueError('Docker descriptor chain is too deep')


def docker_to_oci(source, destination, expected_config, epoch=0):
    """Copy a single Docker save image into OCI without changing config/layers.

    Never extract paths. The existing verifier checks the config digest and
    ordered uncompressed layer hashes before this export is accepted.
    """
    with tarfile.open(source) as saved, tarfile.open(destination, 'w') as result:
        members = {}
        for member in saved.getmembers():
            if (member.name in members or not (member.isfile() or member.isdir()) or
                    member.name.startswith('/') or '..' in member.name.split('/')):
                raise ValueError('unsafe or duplicate Docker archive member')
            members[member.name] = member

        def regular(name):
            member = members.get(name)
            if member is None or not member.isfile():
                raise ValueError('Docker manifest references a missing or non-regular member')
            return member

        def read_json(name):
            member = regular(name)
            if member.size > 16 * 1024 * 1024:
                raise ValueError('oversized Docker metadata')
            return json.load(saved.extractfile(member))

        written = set()

        def write(name, stream, size):
            if name in written:
                return
            member = tarfile.TarInfo(name)
            member.size, member.mtime, member.mode = size, epoch, 0o644
            result.addfile(member, stream)
            written.add(name)

        def copy_blob(name, media_type):
            member = regular(name)
            with saved.extractfile(member) as stream:
                checksum = hashlib.file_digest(stream, 'sha256').hexdigest()
            with saved.extractfile(member) as stream:
                write('blobs/sha256/' + checksum, stream, member.size)
            return {'mediaType': media_type, 'digest': 'sha256:' + checksum, 'size': member.size}

        manifest = read_json('manifest.json')
        if not isinstance(manifest, list) or len(manifest) != 1:
            raise ValueError('expected one Docker worker image')
        manifest = manifest[0]
        config = copy_blob(manifest['Config'], 'application/vnd.oci.image.config.v1+json')
        if config['digest'] != expected_config:
            raise ValueError('Docker worker configuration differs from the validated image')
        layers = []
        for name in manifest['Layers']:
            with saved.extractfile(regular(name)) as stream:
                compressed = stream.read(2) == b'\x1f\x8b'
            layers.append(copy_blob(name, 'application/vnd.oci.image.layer.v1.tar' + ('+gzip' if compressed else '')))
        payload = json.dumps({'schemaVersion': 2, 'mediaType': 'application/vnd.oci.image.manifest.v1+json',
                              'config': config, 'layers': layers}, separators=(',', ':')).encode()
        checksum = hashlib.sha256(payload).hexdigest()
        write('blobs/sha256/' + checksum, io.BytesIO(payload), len(payload))
        descriptor = {'mediaType': 'application/vnd.oci.image.manifest.v1+json',
                      'digest': 'sha256:' + checksum, 'size': len(payload)}
        for name, data in [('oci-layout', {'imageLayoutVersion': '1.0.0'}),
                           ('index.json', {'schemaVersion': 2, 'manifests': [descriptor]})]:
            payload = json.dumps(data, separators=(',', ':')).encode()
            write(name, io.BytesIO(payload), len(payload))
    return verify_archive(destination, expected_config)


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
