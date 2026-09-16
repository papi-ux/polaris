"""Summarize one captured Moonlight audio flow without emitting addresses or payloads.

Reads IPv4 audio from classic Ethernet PCAP, including header-only captures and VLAN tags.
This measures submission at the capture point, not delivery or audible quality.
No packet capture, device access, network calls or configuration changes occur.
"""

import argparse
from collections import defaultdict
import json
import math
from pathlib import Path
import statistics
import struct


class TimingError(ValueError):
    """The capture cannot establish timing for one audio flow."""


def _read(stream, size):
    value = stream.read(size)
    if len(value) != size:
        raise TimingError("capture is truncated")
    return value


def _records(stream):
    header = _read(stream, 24)
    formats = {
        b"\xd4\xc3\xb2\xa1": ("<", 1000),
        b"\xa1\xb2\xc3\xd4": (">", 1000),
        b"\x4d\x3c\xb2\xa1": ("<", 1),
        b"\xa1\xb2\x3c\x4d": (">", 1),
    }
    if header[:4] not in formats:
        raise TimingError("classic PCAP is required")
    order, scale = formats[header[:4]]
    major, minor, _, _, snaplen, link = struct.unpack(order + "HHIIII", header[4:])
    if (major, minor) != (2, 4) or link != 1 or not 1 <= snaplen <= 1048576:
        raise TimingError("Ethernet PCAP version 2.4 with bounded records is required")
    while True:
        record = stream.read(16)
        if not record:
            return
        if len(record) != 16:
            raise TimingError("capture record header is truncated")
        sec, fraction, captured, original = struct.unpack(order + "IIII", record)
        if captured > snaplen or captured > original or fraction * scale >= 1000000000:
            raise TimingError("capture record metadata is invalid")
        yield sec * 1000000000 + fraction * scale, _read(stream, captured), original


def _audio(frame, original, source_port):
    if len(frame) < 14:
        raise TimingError("Ethernet header is truncated")
    offset = 14
    protocol = struct.unpack("!H", frame[12:14])[0]
    for _ in range(2):
        if protocol not in (0x8100, 0x88A8):
            break
        if len(frame) < offset + 4:
            raise TimingError("VLAN header is truncated")
        protocol = struct.unpack("!H", frame[offset + 2:offset + 4])[0]
        offset += 4
    if protocol != 0x0800:
        return None
    if len(frame) < offset + 20:
        raise TimingError("IPv4 header is truncated")
    ip = frame[offset:]
    ihl = (ip[0] & 15) * 4
    total = struct.unpack("!H", ip[2:4])[0]
    if ip[0] >> 4 != 4 or ihl < 20 or len(ip) < ihl or total < ihl:
        raise TimingError("IPv4 header is invalid")
    if ip[9] != 17:
        return None
    # A noninitial fragment has no UDP source port. Reject rather than silently
    # omit audio fragments from a supposedly complete timing observation.
    if struct.unpack("!H", ip[6:8])[0] & 0x3FFF:
        raise TimingError("fragmented UDP requires a separate reassembly analysis")
    if len(ip) < ihl + 8:
        raise TimingError("UDP header is truncated")
    source, destination, length, _ = struct.unpack("!HHHH", ip[ihl:ihl + 8])
    if source != source_port:
        return None
    if length < 20 or total != ihl + length or original < offset + total:
        raise TimingError("audio datagram lengths are inconsistent")
    body = ip[ihl + 8:]
    if len(body) < 12:
        raise TimingError("audio RTP header is truncated")
    # Moonlight audio uses a fixed RTP header. Parity's FEC extension follows it.
    if body[0] != 0x80 or body[1] not in (97, 127):
        raise TimingError("selected UDP flow is not fixed-header Moonlight audio")
    flow = (ip[12:16], ip[16:20], source, destination, body[8:12])
    return flow, body[1], total


def _gaps(times):
    if len(times) < 2:
        return None
    gaps = sorted((b - a) / 1000000 for a, b in zip(times, times[1:]))
    def percentile(fraction):
        return gaps[math.ceil(len(gaps) * fraction) - 1]
    return {
        "median_ms": round(statistics.median(gaps), 6),
        "p95_ms": round(percentile(.95), 6),
        "p99_ms": round(percentile(.99), 6),
        "max_ms": round(gaps[-1], 6),
        "under_1ms": sum(gap < 1 for gap in gaps),
        "over_20ms": sum(gap > 20 for gap in gaps),
        "over_40ms": sum(gap > 40 for gap in gaps),
    }


def analyze(stream, source_port):
    if type(source_port) is not int or not 1 <= source_port <= 65535:
        raise TimingError("a valid UDP source port is required")
    flow = None
    times = defaultdict(list)
    ip_bytes = defaultdict(int)
    first = last = None
    count = 0
    for timestamp, frame, original in _records(stream):
        audio = _audio(frame, original, source_port)
        if audio is None:
            continue
        identity, kind, size = audio
        if flow is not None and identity != flow:
            raise TimingError("multiple audio flows found; capture one destination")
        flow = identity
        if last is not None and timestamp < last:
            raise TimingError("capture timestamps move backwards")
        first = timestamp if first is None else first
        last = timestamp
        times[kind].append(timestamp)
        ip_bytes[kind] += size
        count += 1
        if count > 2000000:
            raise TimingError("capture exceeds the bounded analysis size")
    if first is None or last == first or len(times[97]) < 2:
        raise TimingError("at least two audio data packets over a positive interval are required")
    seconds = (last - first) / 1000000000
    return {
        "schema": 1,
        "observation": "audio_at_capture_point",
        "duration_seconds": round(seconds, 6),
        "ip_bitrate_kbps": round(sum(ip_bytes.values()) * 8 / seconds / 1000, 3),
        "data": {"packets": len(times[97]), "ip_bytes": ip_bytes[97], "gaps": _gaps(times[97])},
        "fec": {"packets": len(times[127]), "ip_bytes": ip_bytes[127], "gaps": _gaps(times[127])},
        "limits": [
            "Capture-point timestamps do not establish client delivery or audible quality.",
            "IP bitrate includes IP, UDP, RTP, encryption and FEC; it excludes link-layer overhead.",
            "Capture drops must be checked in the capture tool receipt.",
            "Intervals include scheduling at the capture point and are not end-to-end latency.",
        ],
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("capture", type=Path)
    parser.add_argument("--source-port", required=True, type=int)
    args = parser.parse_args()
    try:
        with args.capture.open("rb") as stream:
            result = analyze(stream, args.source_port)
    except OSError:
        parser.error("capture could not be read")
    except TimingError as error:
        parser.error(str(error))
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    main()
