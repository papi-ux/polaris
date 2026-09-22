"""Regression checks for both NVIDIA userspace ABIs and artifact substitution."""
import hashlib
import json
import os
import pathlib
import struct
import subprocess
import tempfile
import unittest
from unittest import mock

import nvidia_runtime as runtime


def elf(architecture):
    _, elf_class, machine = runtime.ARCHITECTURES[architecture]
    return b'\x7fELF' + bytes([elf_class, 1, 1]) + bytes(9) + struct.pack('<HH', 3, machine)


class NvidiaRuntime(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = pathlib.Path(self.temporary.name)
        self.root.chmod(0o755)
        self.manifest = {'schema': 1, 'driver_version': '610.57.04', 'source': 'image',
                         'architectures': ['amd64', 'i386'], 'files': [], 'symlinks': []}
        for architecture in self.manifest['architectures']:
            directory = pathlib.Path('usr/lib') / runtime.ARCHITECTURES[architecture][0]
            names = set(runtime.REQUIRED_VENDOR) | {'libnvidia-allocator.so.1'}
            names |= {name for name in runtime.CONFIGURATIONS.values() if name}
            for soname in sorted(names):
                relative = directory / (soname + '.610.57.04')
                self.write(relative, elf(architecture))
                self.record(relative, architecture=architecture, soname=soname)
                self.link(directory / soname, relative.name)
            self.link(directory / 'gbm/nvidia-drm_gbm.so', '../libnvidia-allocator.so.1')
            for name in runtime.FRONTENDS:
                self.write(directory / name, elf(architecture))
        for name, soname in runtime.CONFIGURATIONS.items():
            relative = pathlib.Path('usr/share') / name
            self.write(relative, json.dumps({'ICD': {'library_path': soname}}).encode())
            self.record(relative)
        license_path = pathlib.Path('usr/share/licenses/polaris-nvidia/LICENSE')
        self.write(license_path, b'fixture license')
        self.record(license_path)
        self.write('usr/share/polaris/build/nvidia.lock.json', b'{"version":"610.57.04"}')
        self.save()

    def write(self, relative, content):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(content)
        path.chmod(0o644)

    def record(self, relative, **identity):
        self.manifest['files'].append({'path': '/' + str(relative),
                                      'sha256': hashlib.sha256((self.root / relative).read_bytes()).hexdigest(), **identity})

    def link(self, relative, target):
        path = self.root / relative
        path.parent.mkdir(parents=True, exist_ok=True)
        path.symlink_to(target)
        self.manifest['symlinks'].append({'path': '/' + str(relative), 'target': target})

    def save(self):
        self.write('usr/share/polaris/build/nvidia-files.json', json.dumps(self.manifest).encode())

    def verify(self, **kwargs):
        return runtime.verify(self.root, 'steam', owner_uid=os.getuid(), **kwargs)

    def host_layer(self):
        """Rebuild the fixture as a layer that borrows the machine's driver."""
        contract = {'schema': 1, 'contract': 1, 'minimum_driver': '570.00',
                    'library_prefixes': ['libcuda.so.', 'libEGL_nvidia.so.', 'libGLX_nvidia.so.',
                                         'libnvidia-encode.so.', 'libnvidia-allocator.so.'],
                    'architectures': [], 'vendor_files': []}
        # Written beside the lock, not recorded as a packaged file, exactly as
        # the packaging script leaves it.
        self.write('usr/share/polaris/build/nvidia-host-contract.json', json.dumps(contract).encode())
        borrowed = tuple(contract['library_prefixes'])
        self.manifest['source'] = 'host'
        self.manifest['driver_version'] = ''
        self.manifest['files'] = [entry for entry in self.manifest['files']
                                  if not pathlib.PurePosixPath(entry['path']).name.startswith(borrowed)
                                  and 'glvnd/egl_vendor.d' not in entry['path']
                                  and 'vulkan/' not in entry['path']]
        self.manifest['symlinks'] = [entry for entry in self.manifest['symlinks']
                                     if not pathlib.PurePosixPath(entry['path']).name.startswith(borrowed)]
        for entry in list(self.manifest['files']):
            path = self.root / entry['path'].lstrip('/')
            if not path.exists():
                self.manifest['files'].remove(entry)
        for name in ['libcuda.so.1', 'libEGL_nvidia.so.0', 'libGLX_nvidia.so.0',
                     'libnvidia-encode.so.1', 'libnvidia-allocator.so.1']:
            for architecture in self.manifest['architectures']:
                directory = self.root / 'usr/lib' / runtime.ARCHITECTURES[architecture][0]
                (directory / name).unlink(missing_ok=True)
                (directory / (name + '.610.57.04')).unlink(missing_ok=True)
        for name in ['glvnd/egl_vendor.d/10_nvidia.json', 'vulkan/icd.d/nvidia_icd.json',
                     'vulkan/implicit_layer.d/nvidia_layers.json']:
            (self.root / 'usr/share' / name).unlink(missing_ok=True)
        self.save()
        return contract

    def test_a_host_driver_layer_ships_no_borrowed_library(self):
        self.host_layer()

        report = self.verify(link_check=lambda _: None, source='host')

        self.assertEqual(report['source'], 'host')
        self.assertEqual(report['driver_version'], '')

    def test_a_host_driver_layer_that_still_ships_the_driver_is_rejected(self):
        self.host_layer()
        # Put one borrowed library back, which would shadow the machine's copy.
        directory = pathlib.Path('usr/lib') / runtime.ARCHITECTURES['amd64'][0]
        relative = directory / 'libcuda.so.1'
        self.write(relative, elf('amd64'))
        self.record(relative, architecture='amd64', soname='libcuda.so.1')
        self.save()

        with self.assertRaisesRegex(ValueError, 'still ships libcuda.so.1'):
            self.verify(link_check=lambda _: None, source='host')

    def test_an_image_layer_is_not_accepted_as_a_host_driver_layer(self):
        with self.assertRaises(ValueError):
            self.verify(link_check=lambda _: None, source='host')

    def test_both_abis_and_every_vendor_and_frontend_reach_link_check(self):
        calls = []
        result = self.verify(link_check=calls.append)
        self.assertEqual(result['architectures'], ['amd64', 'i386'])
        self.assertEqual(len(calls), sum(result['vendor_library_counts'].values()) + 10)
        self.assertTrue(any('i386-linux-gnu/libEGL.so.1' in str(path) for path in calls))
        self.assertEqual(runtime.architectures('gamescope'), ['amd64'])
        for profile in ['steam', 'heroic', 'lutris']:
            self.assertEqual(runtime.architectures(profile), ['amd64', 'i386'])

    def test_wrong_architecture_even_with_updated_manifest_hash_is_rejected(self):
        entry = next(e for e in self.manifest['files'] if e.get('architecture') == 'i386')
        path = self.root / entry['path'][1:]
        path.write_bytes(elf('amd64'))
        entry['sha256'] = hashlib.sha256(path.read_bytes()).hexdigest()
        self.save()
        with self.assertRaisesRegex(ValueError, 'ELF ABI'):
            self.verify(link_check=lambda _: None)

    def test_elf_endianness_machine_and_object_type_are_checked(self):
        path = self.root / 'sample.so'
        for offset, value in [(5, 2), (16, 2), (18, 62)]:
            content = bytearray(elf('i386'))
            content[offset] = value
            path.write_bytes(content)
            with self.assertRaises(ValueError):
                runtime.elf_identity(path, 'i386')

    def test_manifest_substitution_and_duplicates_are_rejected(self):
        entry = self.manifest['files'][0]
        for mutation in ['hash', 'duplicate', 'version', 'architecture', 'path', 'soname']:
            with self.subTest(mutation=mutation):
                original = json.loads(json.dumps(self.manifest))
                if mutation == 'hash': entry['sha256'] = '0' * 64
                elif mutation == 'duplicate': self.manifest['files'].append(dict(entry))
                elif mutation == 'version': self.manifest['driver_version'] = 'other'
                elif mutation == 'architecture': self.manifest['architectures'] = ['amd64']
                elif mutation == 'path': entry['path'] = '/../outside'
                elif mutation == 'soname': entry['soname'] = '../library.so'
                self.save()
                with self.assertRaises(ValueError): self.verify(link_check=lambda _: None)
                self.manifest = original
                entry = self.manifest['files'][0]

    def test_missing_soname_alias_is_rejected(self):
        self.manifest['symlinks'].pop(0)
        self.save()
        with self.assertRaisesRegex(ValueError, 'SONAME alias'):
            self.verify(link_check=lambda _: None)

    def test_replaced_alias_cannot_leave_packaged_libraries(self):
        entry = self.manifest['symlinks'][0]
        path = self.root / entry['path'][1:]
        path.unlink()
        path.symlink_to('/etc/passwd')
        entry['target'] = '/etc/passwd'
        self.save()
        with self.assertRaises(ValueError): self.verify(link_check=lambda _: None)

    def test_missing_i386_frontend_is_rejected(self):
        (self.root / 'usr/lib/i386-linux-gnu/libEGL.so.1').unlink()
        with self.assertRaises(FileNotFoundError): self.verify(link_check=lambda _: None)

    def test_missing_configuration_is_rejected(self):
        self.manifest['files'] = [e for e in self.manifest['files'] if not e['path'].endswith('nvidia_icd.json')]
        self.save()
        with self.assertRaisesRegex(ValueError, 'configuration'): self.verify(link_check=lambda _: None)

    def test_wrong_loader_target_even_with_updated_hash_is_rejected(self):
        entry = next(e for e in self.manifest['files'] if e['path'].endswith('nvidia_icd.json'))
        path = self.root / entry['path'][1:]
        path.write_text('{"ICD":{"library_path":"/foreign/libGLX_nvidia.so.0"}}')
        entry['sha256'] = hashlib.sha256(path.read_bytes()).hexdigest()
        self.save()
        with self.assertRaisesRegex(ValueError, 'loader points'): self.verify(link_check=lambda _: None)

    def test_writable_gbm_alias_directory_is_rejected(self):
        (self.root / 'usr/lib/i386-linux-gnu/gbm').chmod(0o777)
        with self.assertRaisesRegex(ValueError, 'untrusted NVIDIA directory'): self.verify(link_check=lambda _: None)

    def test_writable_vendor_file_is_rejected(self):
        (self.root / self.manifest['files'][0]['path'][1:]).chmod(0o666)
        with self.assertRaisesRegex(ValueError, 'untrusted NVIDIA file'): self.verify(link_check=lambda _: None)

    def test_zero_exit_does_not_hide_unresolved_dependencies(self):
        for output in ['libwayland-server.so.0 => not found', 'undefined symbol: wl_global_create', '', 'not a dynamic executable']:
            with self.subTest(output=output), mock.patch.object(runtime.subprocess, 'run', return_value=subprocess.CompletedProcess([], 0, output)):
                with self.assertRaisesRegex(ValueError, 'unresolved NVIDIA'): runtime.linked_library('/vendor.so')
        with mock.patch.object(runtime.subprocess, 'run', return_value=subprocess.CompletedProcess([], 1, 'loader')):
            with self.assertRaises(ValueError): runtime.linked_library('/vendor.so')


if __name__ == '__main__':
    unittest.main()
