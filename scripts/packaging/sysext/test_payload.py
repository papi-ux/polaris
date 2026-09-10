#!/usr/bin/env python3
import os
from pathlib import Path
import stat
import subprocess
import sys
import tempfile
import unittest
from unittest.mock import patch

from payload import Entry, PayloadError, materialize, merge_payloads, parse_cpio, with_metadata


def record(name, data=b'', *, mode=stat.S_IFREG | 0o644, uid=0, gid=0, nlink=1, ino=1, crc=False):
    raw = name.encode() + b'\0'
    fields = [ino, mode, uid, gid, nlink, 0, len(data), 0, 0, 0, 0, len(raw), sum(data) & 0xffffffff if crc else 0]
    header = (b'070702' if crc else b'070701') + ''.join(f'{n:08x}' for n in fields).encode()
    value = header + raw
    value += b'\0' * (-len(value) % 4)
    value += data
    return value + b'\0' * (-len(value) % 4)


def archive(*entries):
    return b''.join(entries) + record('TRAILER!!!', mode=0)


class PayloadTests(unittest.TestCase):
    def test_files_links_hardlinks_and_metadata_roundtrip(self):
        content = archive(record('./usr/bin/app', b'payload', mode=stat.S_IFREG | 0o755),
                          record('usr/bin/link', b'app', mode=stat.S_IFLNK | 0o777),
                          record('usr/share/a', b'', nlink=2, ino=99),
                          record('usr/share/b', b'data', nlink=2, ino=99, crc=True))
        entries = with_metadata(merge_payloads([parse_cpio(content)]), 'ID=bazzite\n')
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory) / 'tree'
            old = os.umask(0o077)
            try:
                materialize(entries, root)
            finally:
                os.umask(old)
            self.assertEqual((root / 'usr/bin/link').read_bytes(), b'payload')
            self.assertEqual((root / 'usr/share/a').read_bytes(), b'data')
            self.assertEqual((root / 'usr/lib/extension-release.d/extension-release.polaris').read_text(), 'ID=bazzite\n')
            self.assertEqual(stat.S_IMODE((root / 'usr').stat().st_mode), 0o755)
            self.assertEqual(stat.S_IMODE((root / 'usr/bin/app').stat().st_mode), 0o755)

    def test_truncation(self):
        value = archive(record('usr/a', b'payload'))
        for size in (0, 5, 109, 115, len(value) - 1):
            with self.subTest(size=size), self.assertRaises(PayloadError):
                parse_cpio(value[:size])

    def test_paths_ownership_modes_and_reserved_metadata(self):
        values = [record(name) for name in ('/usr/a', '../usr/a', 'usr/../opt/a', 'usr//a', 'usr/./a',
                  'etc/a', 'var/a', 'usr/lib/extension-release.d', 'usr/lib/extension-release.d/evil')]
        values += [record('usr/a', mode=mode) for mode in (stat.S_IFCHR | 0o600, stat.S_IFIFO | 0o600,
                   stat.S_IFREG | 0o4755, stat.S_IFREG | 0o6755, stat.S_IFREG | 0o666)]
        values += [record('usr/a', uid=1), record('usr/a', gid=1)]
        for value in values:
            with self.subTest(value=value[:30]), self.assertRaises(PayloadError):
                parse_cpio(archive(value))

    def test_escaping_symlink_and_non_directory_ancestor(self):
        for target in (b'/etc/passwd', b'../../etc/passwd', b'', b'bad\0target'):
            with self.subTest(target=target), self.assertRaises(PayloadError):
                parse_cpio(archive(record('usr/link', target, mode=stat.S_IFLNK | 0o777)))
        for first in (record('usr/a', b'/usr/elsewhere', mode=stat.S_IFLNK | 0o777), record('usr/a', b'file')):
            with self.assertRaises(PayloadError):
                parse_cpio(archive(first, record('usr/a/b', b'escape')))

    def test_cross_package_symlink_cannot_write_outside(self):
        with tempfile.TemporaryDirectory() as directory:
            sentinel = Path(directory) / 'sentinel'
            sentinel.write_bytes(b'unchanged')
            packages = [parse_cpio(archive(record('usr/link', b'/usr/outside', mode=stat.S_IFLNK | 0o777))),
                        parse_cpio(archive(record('usr/link/sentinel', b'changed')))]
            with self.assertRaises(PayloadError):
                materialize(merge_payloads(packages), Path(directory) / 'tree')
            self.assertEqual(sentinel.read_bytes(), b'unchanged')
            self.assertFalse((Path(directory) / 'tree').exists())

    def test_duplicates_directory_conflicts_and_metadata_replacement(self):
        with self.assertRaises(PayloadError):
            parse_cpio(archive(record('usr/a'), record('./usr/a')))
        for a, b in [(Entry(stat.S_IFREG | 0o644), Entry(stat.S_IFREG | 0o644)),
                     (Entry(stat.S_IFDIR | 0o755), Entry(stat.S_IFDIR | 0o700))]:
            with self.assertRaises(PayloadError):
                merge_payloads([{'usr/a': a}, {'usr/a': b}])
        with self.assertRaises(PayloadError):
            with_metadata({'usr/lib/extension-release.d': Entry(stat.S_IFLNK | 0o777, b'/usr/outside')}, 'ID=bazzite\n')

    def test_bad_checksum_trailer_and_hardlinks(self):
        invalid = record('usr/a', b'original', crc=True).replace(b'original', b'changed!')
        cases = [archive(invalid), archive(record('usr/a', nlink=2)),
                 archive(record('usr/a', b'a', nlink=2), record('usr/b', b'a', nlink=2)),
                 archive(record('usr/a', nlink=2), record('usr/b', b'a', nlink=3)),
                 archive(record('usr/a')) + b'nonzero']
        for value in cases:
            with self.subTest(value=value[:20]), self.assertRaises(PayloadError):
                parse_cpio(value)

    def test_bounded_payload_and_entries(self):
        with patch('payload.MAX_PAYLOAD', 10), self.assertRaises(PayloadError):
            parse_cpio(archive(record('usr/a')))
        with patch('payload.MAX_ENTRIES', 1), self.assertRaises(PayloadError):
            parse_cpio(archive(record('usr/a'), record('usr/b')))

    def test_optimized_python_keeps_admission_checks(self):
        command = 'from payload import parse_cpio, PayloadError; from test_payload import archive, record\ntry: parse_cpio(archive(record("../escape")))\nexcept PayloadError: raise SystemExit(0)\nraise SystemExit(1)'
        result = subprocess.run([sys.executable, '-O', '-c', command], cwd=Path(__file__).parent, timeout=5)
        self.assertEqual(result.returncode, 0)


if __name__ == '__main__':
    unittest.main()
