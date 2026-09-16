#!/usr/bin/env python3
"""Bounded physical receipt decoder using the system's OpenH264 GStreamer plugin."""
import hashlib
import json
import os
import pathlib
import selectors
import subprocess
import sys
import time

def decode(path, expected):
    assert 0 < expected <= 20000
    descriptor = os.open(path, os.O_RDONLY | os.O_NOFOLLOW)
    with os.fdopen(descriptor, 'rb') as source:
        child = subprocess.Popen([
            '/usr/bin/gst-launch-1.0', '-q', 'fdsrc', 'fd=0', '!', 'h264parse', '!',
            'openh264dec', '!', 'videoconvert', '!',
            'video/x-raw,format=I420,width=1920,height=1080', '!',
            'fdsink', 'fd=1', 'sync=false',
        ], stdin=source, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
           env={'LC_ALL': 'C', 'PATH': '/usr/bin:/bin'})
        try:
            pending = bytearray()
            size = 1920 * 1080 * 3 // 2
            count = changed = 0
            previous = None
            deadline = time.monotonic() + 20
            with selectors.DefaultSelector() as poll:
                poll.register(child.stdout, selectors.EVENT_READ)
                while True:
                    assert time.monotonic() < deadline, 'decoder output timed out'
                    if not poll.select(0.2):
                        continue
                    chunk = os.read(child.stdout.fileno(), 1024 * 1024)
                    if not chunk:
                        break
                    pending.extend(chunk)
                    while len(pending) >= size:
                        digest = hashlib.blake2s(pending[:1920 * 1080:8]).digest()
                        if previous is not None and digest != previous:
                            changed += 1
                        previous = digest
                        count += 1
                        assert count <= expected, 'decoder produced extra frames'
                        del pending[:size]
            assert child.wait(timeout=2) == 0, 'decoder failed'
            result = {'decoded_frames': count, 'changing_frames': changed}
            print(json.dumps(result), flush=True)
            assert not pending and count == expected, 'decoded frame count mismatch'
        finally:
            if child.poll() is None:
                child.terminate()
                try:
                    child.wait(timeout=1)
                except subprocess.TimeoutExpired:
                    child.kill()
                    child.wait(timeout=1)
            child.stdout.close()

if __name__ == '__main__':
    assert len(sys.argv) == 3
    decode(pathlib.Path(sys.argv[1]), int(sys.argv[2]))
