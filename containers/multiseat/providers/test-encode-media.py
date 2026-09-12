#!/usr/bin/env python3
"""Device-free real codec/control check. Does not establish game streaming."""
import argparse
import os
import select
import signal
import struct
import subprocess
import sys
import time


def check(executable, invalid=False, render_node=None):
    arguments = ['--self-test-gpu', render_node] if render_node else ['--self-test']
    child = subprocess.Popen([executable, *arguments], stdin=subprocess.PIPE,
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def read_exact(size):
        body = b''
        deadline = time.monotonic() + 20
        while len(body) < size:
            if not select.select([child.stdout], [], [], max(0, deadline-time.monotonic()))[0]:
                raise RuntimeError('encoder packet timed out')
            part = os.read(child.stdout.fileno(), size-len(body))
            if not part:
                raise RuntimeError('encoder packet ended early')
            body += part
        return body

    def packet():
        header = read_exact(12)
        assert header[:4] == b'PME1' and header[5:8] == bytes(3)
        length, = struct.unpack('>I', header[8:])
        assert 0 < length <= 16*1024*1024
        return header[4], read_exact(length)

    try:
        kind, contract = packet()
        assert kind == 1 and len(contract) == 32 and contract[:3] == bytes([1, 1, 66])
        assert struct.unpack('>HHII', contract[4:16]) == (640, 480, 60000, 1000)
        assert contract[20:24] == bytes([1, 2, 0x13, 0x88])
        assert not select.select([child.stdout], [], [], 0.15)[0], 'media before start'
        child.stdin.write(bytes([9 if invalid else 1])); child.stdin.flush()
        if invalid:
            assert child.wait(timeout=3) == 1
            return
        counts = {2: 0, 3: 0}
        indices = {2: -1, 3: -1}
        encoded_video = bytearray()

        def frame():
            kind, body = packet()
            assert kind in counts and len(body) > 32 and body[0] == 1 and body[2:8] == bytes(6)
            index, capture, encoded = struct.unpack('>QQQ', body[8:32])
            assert index > indices[kind] and capture == 0 and encoded > 0
            indices[kind] = index
            counts[kind] += 1
            if kind == 2:
                if counts[2] == 1:
                    assert body[1] == 1, 'first video frame must be an IDR'
                encoded_video.extend(body[32:])
            else:
                assert body[1] == 0 and len(body) <= 1432
            return kind == 2 and body[1] == 1

        for _ in range(100):
            frame()
        assert counts[2] >= 10 and counts[3] >= 50
        before = counts[2]
        child.stdin.write(bytes([2])); child.stdin.flush()
        while not frame():
            assert counts[2]-before < 8, 'requested IDR did not arrive promptly'
        child.send_signal(signal.SIGTERM)
        assert child.wait(timeout=3) == 0
        subprocess.run(['gst-launch-1.0', '-q', 'fdsrc', 'fd=0', '!', 'h264parse', '!',
                        'openh264dec', '!', 'fakesink', 'sync=false'], input=encoded_video,
                       timeout=10, check=True)
        print('synthetic H.264 decode, Opus packets, ack gate, IDR and clean stop passed', flush=True)
    finally:
        if child.poll() is None:
            child.kill()
            child.wait()
        diagnostic = child.stderr.read().decode(errors='replace')
        if diagnostic:
            print(diagnostic[-2000:], file=sys.stderr)
        child.stdin.close(); child.stdout.close(); child.stderr.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('executable')
    parser.add_argument('--render-node')
    args = parser.parse_args()
    check(args.executable, render_node=args.render_node)
    check(args.executable, invalid=True, render_node=args.render_node)
