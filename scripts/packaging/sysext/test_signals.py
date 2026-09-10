#!/usr/bin/env python3
"""External termination at child admission and receipt publication boundaries."""
import os
from pathlib import Path
import signal
import subprocess
import sys
import tempfile
import time
import unittest


def exited(pid):
    try:
        return Path('/proc/' + str(pid) + '/stat').read_text().rsplit(') ', 1)[1][0] == 'Z'
    except FileNotFoundError:
        return True


@unittest.skipUnless(sys.platform == 'linux', 'Linux signal/process ownership tests')
class SignalTests(unittest.TestCase):
    def assert_stopped(self, pid):
        deadline = time.monotonic() + 2
        while not exited(pid) and time.monotonic() < deadline:
            time.sleep(0.01)
        self.assertTrue(exited(pid), 'owned process survived cleanup')

    def test_termination_between_popen_and_ownership_assignment(self):
        for termination in (signal.SIGTERM, signal.SIGHUP):
            with self.subTest(signal=termination), tempfile.TemporaryDirectory() as directory:
                code = '''import os,signal,sys,pathlib
import bounded
def interrupted(*_): raise bounded.BuildError('interrupted')
signal.signal(signal.SIGTERM, interrupted)
signal.signal(signal.SIGHUP, interrupted)
original = bounded.subprocess.Popen
def after_creation(*args, **kwargs):
    child = original(*args, **kwargs)
    pathlib.Path(sys.argv[1], 'pid').write_text(str(child.pid))
    os.kill(os.getpid(), int(sys.argv[2]))
    return child
bounded.subprocess.Popen = after_creation
try: bounded.run([sys.executable, '-c', 'import time; time.sleep(30)'], cwd=sys.argv[1])
except bounded.BuildError: raise SystemExit(0)
raise SystemExit(2)
'''
                result = subprocess.run([sys.executable, '-c', code, directory, str(int(termination))],
                                        cwd=Path(__file__).parent, timeout=5, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assert_stopped(int((Path(directory) / 'pid').read_text()))

    def test_external_termination_reaps_leader_and_descendant(self):
        for termination in (signal.SIGTERM, signal.SIGHUP):
            with self.subTest(signal=termination), tempfile.TemporaryDirectory() as directory:
                code = '''import pathlib,signal,sys
from bounded import run, BuildError
def interrupted(*_): raise BuildError('interrupted')
signal.signal(signal.SIGTERM, interrupted)
signal.signal(signal.SIGHUP, interrupted)
payload = "import os,pathlib,time; child=os.fork(); pathlib.Path('pids').write_text(str(os.getpid())+' '+str(child)) if child else None; time.sleep(30)"
try: run([sys.executable, '-c', payload], cwd=sys.argv[1])
except BuildError: raise SystemExit(0)
raise SystemExit(2)
'''
                wrapper = subprocess.Popen([sys.executable, '-c', code, directory], cwd=Path(__file__).parent,
                                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
                try:
                    pids = Path(directory) / 'pids'
                    deadline = time.monotonic() + 3
                    while not pids.exists() and time.monotonic() < deadline:
                        time.sleep(0.01)
                    self.assertTrue(pids.exists())
                    os.kill(wrapper.pid, termination)
                    _, error = wrapper.communicate(timeout=5)
                    self.assertEqual(wrapper.returncode, 0, error)
                    for pid in pids.read_text().split():
                        self.assert_stopped(int(pid))
                finally:
                    if wrapper.poll() is None:
                        wrapper.kill()
                        wrapper.wait()
                    wrapper.stdout.close()
                    wrapper.stderr.close()

    def test_signal_during_link_revokes_completion_marker(self):
        for termination in (signal.SIGTERM, signal.SIGHUP):
            with self.subTest(signal=termination), tempfile.TemporaryDirectory() as directory:
                code = '''import os,signal,sys,pathlib
import assemble
def interrupted(*_): raise assemble.BuildError('interrupted')
signal.signal(signal.SIGTERM, interrupted)
signal.signal(signal.SIGHUP, interrupted)
original = os.link
def linked(*args, **kwargs):
    original(*args, **kwargs)
    os.kill(os.getpid(), int(sys.argv[2]))
assemble.os.link = linked
try: assemble.finish_receipt(pathlib.Path(sys.argv[1]), {'verdict':'PRIVATE_PACKAGE_BUILT'})
except assemble.BuildError: raise SystemExit(0)
raise SystemExit(2)
'''
                result = subprocess.run([sys.executable, '-c', code, directory, str(int(termination))],
                                        cwd=Path(__file__).parent, timeout=5, capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertFalse((Path(directory) / 'build-receipt.json').exists())
