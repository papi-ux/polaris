import io
import json
import struct
import unittest

from audio_packet_timing import TimingError, analyze


def packet(kind=97, source=53000, destination=40000, ssrc=0, vlan=False):
    rtp = struct.pack("!BBHII", 0x80, kind, 0, 0, ssrc)
    body = rtp + bytes(80 + (12 if kind == 127 else 0))
    udp = struct.pack("!HHHH", source, destination, 8 + len(body), 0) + body
    ip = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(udp), 0, 0, 64, 17, 0,
                     bytes([192, 0, 2, 1]), bytes([192, 0, 2, 2])) + udp
    ethernet = bytes(12) + (b"\x81\x00\x00\x01\x08\x00" if vlan else b"\x08\x00")
    return ethernet + ip


def capture(records, order="<", nano=False, snaplen=96):
    magic = 0xA1B23C4D if nano else 0xA1B2C3D4
    value = struct.pack(order + "IHHIIII", magic, 2, 4, 0, 0, snaplen, 1)
    for ns, frame in records:
        captured = frame[:snaplen]
        sec, fraction = divmod(ns, 1000000000)
        if not nano:
            fraction //= 1000
        value += struct.pack(order + "IIII", sec, fraction, len(captured), len(frame)) + captured
    return value


def read(records, **kwargs):
    return analyze(io.BytesIO(capture(records, **kwargs)), 53000)


class AudioPacketTimingTests(unittest.TestCase):
    def test_known_data_and_parity_wire_cost_and_gaps(self):
        records = [(i * 5000000, packet()) for i in range(4)]
        records += [(15000000, packet(127)), (15000000, packet(127))]
        result = read(records)
        self.assertEqual(result["duration_seconds"], .015)
        self.assertEqual(result["data"]["packets"], 4)
        self.assertEqual(result["data"]["ip_bytes"], 480)
        self.assertEqual(result["fec"]["ip_bytes"], 264)
        self.assertEqual(result["ip_bitrate_kbps"], 396.8)
        self.assertEqual(result["data"]["gaps"]["median_ms"], 5)
        self.assertEqual(result["data"]["gaps"]["max_ms"], 5)
        self.assertEqual(result["fec"]["gaps"]["under_1ms"], 1)

    def test_delayed_burst_is_distinct_from_regular_pacing(self):
        result = read([(ns, packet()) for ns in [0, 5000000, 65000000, 65200000, 70200000]])
        gaps = result["data"]["gaps"]
        self.assertEqual(gaps["median_ms"], 5)
        self.assertEqual(gaps["p99_ms"], 60)
        self.assertEqual(gaps["under_1ms"], 1)
        self.assertEqual(gaps["over_40ms"], 1)

    def test_exact_gap_thresholds_remain_exact_at_epoch_timestamps(self):
        base = 1789327000000000000
        result = read([(base + ns, packet()) for ns in
                       (0, 1000000, 21000000, 61000000)])
        gaps = result["data"]["gaps"]
        self.assertEqual(gaps["under_1ms"], 0)
        self.assertEqual(gaps["over_20ms"], 1)
        self.assertEqual(gaps["over_40ms"], 0)

    def test_invalid_ip_or_udp_lengths_are_rejected(self):
        for offset, value in ((14 + 2, 20), (14 + 20 + 4, 7)):
            frame = bytearray(packet())
            struct.pack_into("!H", frame, offset, value)
            with self.assertRaises(TimingError):
                read([(0, bytes(frame)), (5000000, packet())])

    def test_microsecond_nanosecond_and_endian_formats_agree(self):
        records = [(100000000000, packet()), (100005000000, packet())]
        expected = read(records)
        for order in ("<", ">"):
            for nano in (True, False):
                self.assertEqual(read(records, order=order, nano=nano), expected)

    def test_header_only_capture_preserves_full_datagram_size(self):
        records = [(0, packet()), (5000000, packet())]
        self.assertEqual(read(records, snaplen=54), read(records, snaplen=200))

    def test_vlan_header_does_not_count_as_ip_traffic(self):
        ordinary = read([(0, packet()), (5000000, packet())])
        tagged = read([(0, packet(vlan=True)), (5000000, packet(vlan=True))])
        self.assertEqual(tagged, ordinary)

    def test_summary_has_no_addresses_payload_or_port(self):
        result = json.dumps(read([(0, packet()), (5000000, packet())]))
        for private in ("192.0.2", "53000", "40000", "ssrc", "payload"):
            self.assertNotIn(private, result)

    def test_other_source_ports_are_not_part_of_the_sample(self):
        self.assertEqual(
            read([(0, packet()), (2000000, packet(source=53001)), (5000000, packet())]),
            read([(0, packet()), (5000000, packet())]))

    def test_multiple_destinations_or_ssrc_are_not_aggregated(self):
        for frame in (packet(destination=40001), packet(ssrc=1)):
            with self.assertRaisesRegex(TimingError, "multiple audio flows"):
                read([(0, packet()), (5000000, frame)])

    def test_empty_or_single_packet_cannot_claim_a_rate(self):
        for records in ([], [(0, packet())], [(0, packet()), (0, packet())]):
            with self.assertRaises(TimingError):
                read(records)

    def test_backwards_clock_is_rejected(self):
        with self.assertRaisesRegex(TimingError, "backwards"):
            read([(5000000, packet()), (0, packet())])

    def test_truncated_record_is_not_a_successful_partial_analysis(self):
        data = capture([(0, packet()), (5000000, packet())])
        for value in (data[:10], data[:-1], data + b"x"):
            with self.assertRaises(TimingError):
                analyze(io.BytesIO(value), 53000)

    def test_inconsistent_record_lengths_are_rejected(self):
        data = bytearray(capture([(0, packet()), (5000000, packet())]))
        struct.pack_into("<I", data, 24 + 12, 10)
        with self.assertRaises(TimingError):
            analyze(io.BytesIO(data), 53000)

    def test_selected_flow_requires_complete_fixed_rtp_headers(self):
        for snaplen in (20, 40, 50, 53):
            with self.assertRaises(TimingError):
                read([(0, packet()), (5000000, packet())], snaplen=snaplen)
        for offset, value in ((42, 0x90), (43, 98)):
            frame = bytearray(packet())
            frame[offset] = value
            with self.assertRaises(TimingError):
                read([(0, bytes(frame)), (5000000, packet())])

    def test_fragmented_udp_is_not_silently_omitted(self):
        frame = bytearray(packet())
        struct.pack_into("!H", frame, 14 + 6, 0x2000)
        with self.assertRaisesRegex(TimingError, "fragmented"):
            read([(0, bytes(frame)), (5000000, packet())])

    def test_invalid_source_port_or_capture_format_is_rejected(self):
        data = capture([(0, packet()), (5000000, packet())])
        for port in (0, 65536, True, "53000"):
            with self.assertRaises(TimingError):
                analyze(io.BytesIO(data), port)
        for offset, value in ((0, 0), (20, 276)):
            bad = bytearray(data)
            struct.pack_into("<I", bad, offset, value)
            with self.assertRaises(TimingError):
                analyze(io.BytesIO(bad), 53000)


if __name__ == "__main__":
    unittest.main()
