#!/usr/bin/env python3
"""Device-free real codec/control check. Does not establish game streaming."""
import argparse
import concurrent.futures
import os
import re
import select
import signal
import struct
import subprocess
import sys
import time
import threading


def check_reference_limit(encoded):
    """Inspect baseline SPS RBSPs, including headers repeated on recovery IDRs."""
    headers = 0
    for nal in re.split(b'\x00\x00\x00?\x01', encoded):
        if not nal or nal[0] & 31 != 7:
            continue
        rbsp = nal[1:].replace(b'\x00\x00\x03', b'\x00\x00')
        assert len(rbsp) >= 4 and rbsp[0] == 66, 'expected baseline SPS'
        bits = ''.join(f'{byte:08b}' for byte in rbsp)
        position = 24

        def ue():
            nonlocal position
            zeroes = 0
            while position < len(bits) and bits[position] == '0':
                zeroes += 1
                position += 1
                assert zeroes <= 31, 'invalid SPS Exp-Golomb value'
            assert position + zeroes < len(bits), 'truncated SPS'
            value = int(bits[position:position + zeroes + 1], 2) - 1
            position += zeroes + 1
            return value

        ue()  # seq_parameter_set_id
        ue()  # log2_max_frame_num_minus4
        order = ue()
        if order == 0:
            ue()  # log2_max_pic_order_cnt_lsb_minus4
        elif order == 1:
            position += 1  # delta_pic_order_always_zero_flag
            ue(); ue()  # signed offsets use the same bit length as ue(v)
            cycle = ue()
            assert cycle <= 255, 'invalid picture order cycle'
            for _ in range(cycle):
                ue()
        else:
            assert order == 2, 'invalid picture order type'
        references = ue()
        assert references == 1, f'client requires one H.264 reference, encoder announced {references}'
        headers += 1
    assert headers >= 2, 'initial and recovery SPS were not both inspected'


def check(executable, invalid=False, render_node=None, peers=None, survivor=False, bitrate=None, invalid_selection=None):
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
        if bitrate is not None:
            selection = bytes([3]) + struct.pack('>I', bitrate)
            # Exercise a fragmented control body, not just one pipe write.
            child.stdin.write(selection[:2]); child.stdin.flush()
            assert not select.select([child.stdout], [], [], 0.03)[0], 'partial selection produced output'
            child.stdin.write(selection[2:]); child.stdin.flush()
            if bitrate == 0 or bitrate > 8000:
                assert child.wait(timeout=4) == 1, 'invalid bitrate accepted'
                return
            kind, confirmed = packet()
            assert kind == 4 and confirmed == struct.pack('>I', bitrate), 'encoder did not confirm the selected rate'
            assert not select.select([child.stdout], [], [], 0.15)[0], 'selection released media before Start'
            if invalid_selection == 'duplicate':
                child.stdin.write(selection); child.stdin.flush()
                assert child.wait(timeout=4) == 1, 'duplicate selection accepted'
                return
        if peers:
            peers['ready'].wait(timeout=25)
        child.stdin.write(bytes([9 if invalid else 1])); child.stdin.flush()
        if invalid:
            assert child.wait(timeout=3) == 1
            return
        if invalid_selection == 'after-start':
            child.stdin.write(bytes([3]) + struct.pack('>I', 1000)); child.stdin.flush()
            assert child.wait(timeout=4) == 1, 'selection after Start accepted'
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
                # Moonlight's audio FEC shards must have equal sizes, including
                # the transition from silence to sound. 128 kbps at 5 ms is 80 bytes.
                assert len(body) - 32 == 80, 'Opus packet size violates the audio FEC contract'
            return kind == 2 and body[1] == 1

        for _ in range(100):
            frame()
        assert counts[2] >= 10 and counts[3] >= 50
        before = counts[2]
        child.stdin.write(bytes([2])); child.stdin.flush()
        deadline = time.monotonic() + 5
        while not frame():
            assert time.monotonic() < deadline, 'requested IDR timed out'
            assert counts[2]-before < 8, 'requested IDR did not arrive promptly'
        if peers and survivor:
            deadline = time.monotonic() + 10
            while not peers['stopped'].is_set():
                assert not peers['failed'].is_set(), 'peer failed before clean stop'
                assert time.monotonic() < deadline, 'peer stop timed out'
                frame()
            assert not peers['failed'].is_set(), 'peer stop failed'
            before = counts[2]
            audio_before = counts[3]
            deadline = time.monotonic() + 5
            while counts[2] - before < 30:
                assert time.monotonic() < deadline, 'survivor video timed out'
                frame()
            assert counts[3] > audio_before, 'audio stopped with the peer'
        child.send_signal(signal.SIGTERM)
        assert child.wait(timeout=3) == 0
        if peers and not survivor:
            peers['stopped'].set()
        check_reference_limit(encoded_video)
        decoded = subprocess.run(['gst-launch-1.0', '-q', 'fdsrc', 'fd=0', '!', 'h264parse', '!',
                                  'openh264dec', '!', 'videoconvert', '!',
                                  'video/x-raw,format=I420,width=640,height=480', '!',
                                  'fdsink', 'fd=1', 'sync=false'], input=encoded_video,
                                 stdout=subprocess.PIPE, timeout=10, check=True)
        expected = counts[2] * 640 * 480 * 3 // 2
        assert len(decoded.stdout) == expected, f'decoded bytes {len(decoded.stdout)} expected {expected} for {counts[2]} frames'

        print(f'synthetic H.264 decode, Opus packets, ack gate, IDR and clean stop passed (video target {bitrate or 8000} kbps)', flush=True)
    except BaseException:
        if peers:
            peers['failed'].set()
            peers['ready'].abort()
        raise
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
    parser.add_argument('--two-seats', action='store_true')
    args = parser.parse_args()
    if args.two_seats:
        peers = {'ready': threading.Barrier(2), 'stopped': threading.Event(), 'failed': threading.Event()}
        with concurrent.futures.ThreadPoolExecutor(max_workers=2) as pool:
            futures = [pool.submit(check, args.executable, render_node=args.render_node,
                                   peers=peers, survivor=survivor, bitrate=4000 if survivor else 1000) for survivor in (False, True)]
            for future in futures:
                future.result()
        print('two concurrent encoder sessions, IDRs and 30 survivor frames after peer stop passed')
    else:
        check(args.executable, render_node=args.render_node)
        check(args.executable, invalid=True, render_node=args.render_node)
        for bitrate in (1000, 4000, 8000, 0, 8001):
            check(args.executable, render_node=args.render_node, bitrate=bitrate)
        check(args.executable, render_node=args.render_node, bitrate=4000, invalid_selection='duplicate')
        check(args.executable, render_node=args.render_node, invalid_selection='after-start')
