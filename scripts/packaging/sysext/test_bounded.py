#!/usr/bin/env python3
import hashlib
import io
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time
import unittest
from unittest.mock import patch

from bounded import BuildError, copy_verified, run, strict_json


class BoundedTests(unittest.TestCase):
    def test_duplicate_and_nonfinite_json(self):
        for data in ('{"a":1,"a":2}', '{"a":{"b":1,"b":2}}', '{"a":NaN}', '{'):
            with self.subTest(data=data), self.assertRaises(BuildError):
                strict_json(data)
        self.assertEqual(strict_json('{"source":{"commit":"abc"}}'), {'source': {'commit': 'abc'}})

    def test_descriptor_copy_and_wrong_identity(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'a').write_bytes(b'candidate')
            fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY)
            try:
                output = io.BytesIO()
                digest = hashlib.sha256(b'candidate').hexdigest()
                self.assertEqual(copy_verified(fd, 'a', digest, output), 9)
                self.assertEqual(output.getvalue(), b'candidate')
                for name, expected, limit in [('a', '0' * 64, 100), ('a', digest, 2), ('../a', digest, 100), ('./a', digest, 100)]:
                    with self.subTest(name=name, expected=expected), self.assertRaises(BuildError):
                        copy_verified(fd, name, expected, io.BytesIO(), limit=limit)
            finally:
                os.close(fd)

    def test_symlink_ancestors_and_fifo_rejected_without_blocking(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / 'actual').mkdir()
            (root / 'actual/a').write_bytes(b'a')
            (root / 'link').symlink_to('actual/a')
            (root / 'parent').symlink_to('actual')
            os.mkfifo(root / 'fifo')
            fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY)
            try:
                for name in ('link', 'parent/a', 'fifo'):
                    with self.subTest(name=name), self.assertRaises((OSError, BuildError)):
                        copy_verified(fd, name, hashlib.sha256(b'a').hexdigest(), io.BytesIO())
            finally:
                os.close(fd)

    def test_input_mutation_is_rejected(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            source = root / 'a'
            source.write_bytes(b'candidate')
            fd = os.open(root, os.O_RDONLY | os.O_DIRECTORY)
            original_read = os.read
            def change(open_fd, count):
                data = original_read(open_fd, count)
                if data:
                    source.write_bytes(b'different')
                return data
            try:
                with patch('bounded.os.read', side_effect=change), self.assertRaises(BuildError):
                    copy_verified(fd, 'a', hashlib.sha256(b'candidate').hexdigest(), io.BytesIO())
            finally:
                os.close(fd)

    def test_command_bounds_and_failures(self):
        with tempfile.TemporaryDirectory() as directory:
            good = run([sys.executable, '-c', 'print("ok")'], cwd=directory)
            self.assertEqual(good['stdout'], b'ok\n')
            for code, kwargs in [('import os; os.write(1,b"x"*1000000)', {'stdout_limit': 1024}),
                                 ('import os; os.write(2,b"x"*1000000)', {'stderr_limit': 1024}),
                                 ('import time; time.sleep(30)', {'timeout': 0.1}),
                                 ('raise SystemExit(2)', {})]:
                started = time.monotonic()
                with self.subTest(code=code), self.assertRaises((BuildError, subprocess.TimeoutExpired)):
                    run([sys.executable, '-c', code], cwd=directory, **kwargs)
                self.assertLess(time.monotonic() - started, 5)

    def test_exited_parent_does_not_leave_pipe_child(self):
        with tempfile.TemporaryDirectory() as directory:
            pid_file = Path(directory) / 'pid'
            code = 'import os,time,pathlib; pid=os.fork(); pathlib.Path("pid").write_text(str(pid)) if pid else time.sleep(30)'
            with self.assertRaises(BuildError):
                run([sys.executable, '-c', code], cwd=directory, timeout=0.2)
            child_pid = int(pid_file.read_text())
            # Killed descendants may briefly remain zombies until adopted/reaped.
            deadline = time.monotonic() + 2
            while True:
                if sys.platform == 'linux':
                    try:
                        gone = Path('/proc/' + str(child_pid) + '/stat').read_text().rsplit(') ', 1)[1][0] == 'Z'
                    except FileNotFoundError:
                        gone = True
                else:
                    result = subprocess.run(['ps', '-o', 'stat=', '-p', str(child_pid)], capture_output=True, text=True)
                    gone = result.returncode != 0 or result.stdout.strip().startswith('Z')
                if gone or time.monotonic() >= deadline:
                    break
                time.sleep(0.01)
            self.assertTrue(gone, 'owned descendant survived bounded shutdown')


if __name__ == '__main__':
    unittest.main()
