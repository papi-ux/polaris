#!/usr/bin/env python3
"""Independent producer-shaped fixtures; no host/media/policy acceptance."""
import contextlib
import hashlib
import io
import json
import os
import tempfile
import unittest

import inspect_frame_records as reader


# Literal fixture contracts are independent of the reader's key lists/mappers.
# Synthetic identities are private input only, never expected output.
def header():
    return {
        "type": "header", "schema": "polaris.vrr-host-trace", "major": 2, "minor": 3,
        "identity": {"private_fixture_marker": "PRIVATE_HOST_PROCESS_PEER"},
        "queue_capacity": 512, "event_budget": 2_000_000, "byte_budget": 268435456,
        "writer_lifetime_ns": 1800000000000,
        "delivery_semantics": "socket_api_observations_only",
        "clock_route": "unqualified_requires_G1_G5_and_owner_topology",
        "raw_capture_peer_endpoint_facts": "epoch_peer_reads_and_footer_peer_snapshots",
        "build_source_commit": "1" * 40, "source_binding_requires_external_binary_receipt": True,
        "code_constants": {
            "tick_ns": 11112, "liveness_ns": 500000000, "max_source_age_ns": 100000000,
            "burst_frames": 2, "max_receipt_uncertainty_ns": 150000,
            "mapping_min_samples": 8, "mapping_max_probe_samples": 64,
            "mapping_min_probe_samples": 2, "mapping_max_uncertainty_ns": 50000,
            "mapping_max_abs_offset_ns": 1000000, "source_staleness_ns": 100000000,
            "source_max_live_bracket_ns": 20000, "source_max_live_drift_ns": 50000,
            "source_max_total_uncertainty_ns": 150000, "submission_capacity": 64}}


def packet(order=1, identity=901, pts=5, revision=11):
    return {
        "type": "packet_dequeue", "order": order, "observed_steady_ns": 1000 + order,
        "packet": identity, "encoder": 902, "submission": 903, "frame_index": pts,
        "frame_timestamp_ns": None, "encode_done_ns": None,
        "source_clock_timestamp": 0, "source_time_paced": 0,
        "record_stage": "sender_packet_dequeue", "frame01_available": 1,
        "frame01_encoder_lifetime": 7, "frame01_stream_generation": 8,
        "frame01_application_sequence": 9, "frame01_origin_revision": revision,
        "frame01_configured_encoder_kbps": 10000, "frame01_input_pts": pts,
        "frame01_submission_sequence": 10, "frame01_receipt_origin": "backend_reconfiguration",
        "pyrowork_available": 0, "pyrowork_authority": None, "pyrowork_provenance": None,
        "pyrowork_kind": None, "pyrowork_sequence": None, "pyrowork_handoff_frame_index": None,
        "pyrowork_budget_argument_bytes": None, "pyrowork_completed_bitstream_bytes": None,
        "pyrowork_packet_copy_bytes": None}


def missing(order=1, identity=901):
    row = packet(order, identity)
    row.update(frame01_available=0, frame01_encoder_lifetime=None, frame01_stream_generation=None,
               frame01_application_sequence=None, frame01_origin_revision=None,
               frame01_configured_encoder_kbps=None, frame01_input_pts=None,
               frame01_submission_sequence=None, frame01_receipt_origin=None)
    return row


def work(kind="packed", order=1, identity=901):
    row = missing(order, identity)
    row.update(pyrowork_available=1, pyrowork_authority="unknown",
               pyrowork_provenance="wrapper_call_success_nonempty", pyrowork_kind=kind,
               pyrowork_sequence=1, pyrowork_handoff_frame_index=5,
               pyrowork_budget_argument_bytes=2, pyrowork_completed_bitstream_bytes=3,
               pyrowork_packet_copy_bytes=3)
    return row


def line(value):
    return json.dumps(value, separators=(",", ":"), allow_nan=False).encode("utf-8") + b"\n"


def seal(rows=None, head=None, footer_updates=None, close_updates=None):
    head = header() if head is None else head
    rows = [packet()] if rows is None else rows
    content = line(head) + b"".join(row if type(row) is bytes else line(row) for row in rows)
    footer = {
        "type": "footer", "major": 2, "identity": {}, "peer_end": [], "peer_overflow": False,
        "attempts": len(rows), "admitted": len(rows), "event_rows": len(rows),
        "lost_contention_or_full": 0, "event_cap_loss": 0, "invalid_event_loss": 0,
        "byte_cap_loss": 0, "duration_cap": False, "instrumentation_refusals": 0,
        "content_sha256": hashlib.sha256(content).hexdigest(), "bytes_before_footer": len(content),
        "producer_complete": True, "main_close_result": "requires_exclusive_close_receipt",
        "qualification": "ungraded", "build_source_commit": head["build_source_commit"]}
    footer.update(footer_updates or {})
    main = content + line(footer)
    close = {
        "type": "close_receipt", "schema": "polaris.vrr-host-trace", "major": 2,
        "main_bytes": len(main), "footer_written": True, "write_errno": 0,
        "main_sha256": hashlib.sha256(main).hexdigest(), "main_close_errno": 0,
        "receipt_own_close": "not_observable_in_this_file", "durability": "no_fsync_claim"}
    close.update(close_updates or {})
    return main, line(close)


class FrameRecordInspector(unittest.TestCase):
    def complete(self, streams):
        result = reader.inspect_bytes(*streams)
        self.assertEqual(result["status"], "COMPLETE_HISTORICAL_DIAGNOSTIC", result)
        self.assertFalse(result["physical_acceptance"])
        self.assertEqual(result["source_binary_binding"], "unassessed")
        return result

    def refuse(self, streams):
        result = reader.inspect_bytes(*streams)
        self.assertEqual(result["status"], "INCONCLUSIVE", result)
        self.assertIsNone(result["records"])
        self.assertNotIn("PRIVATE_HOST_PROCESS_PEER", json.dumps(result))
        return result

    def test_complete_producer_shape_and_exact_historical_values(self):
        main, close = seal([packet(), work(order=2, identity=904), missing(3, 905)])
        result = self.complete((main, close))
        self.assertEqual(result["raw_sha256"], hashlib.sha256(main).hexdigest())
        self.assertEqual((result["packet_rows"], result["frame01_records"], result["pyrowork_records"]), (3, 1, 1))
        self.assertEqual(result["records"][0]["frame01"], {
            "encoder_lifetime": 7, "stream_generation": 8, "application_sequence": 9,
            "origin_revision": 11, "configured_encoder_kbps": 10000, "input_pts": 5,
            "submission_sequence": 10, "receipt_origin": "backend_reconfiguration"})
        self.assertEqual(result["records"][1]["pyrowork"]["budget_argument_bytes"], 2)
        self.assertEqual(result["records"][1]["pyrowork"]["completed_bitstream_bytes"], 3)
        self.assertEqual(result["records"][1]["pyrowork"]["authority"], "unknown")
        self.assertIsNone(result["records"][2]["frame01"])
        self.assertIsNone(result["records"][2]["pyrowork"])
        self.assertEqual(result["current_controller_permission"], "unassessed")
        self.assertNotIn("PRIVATE_HOST_PROCESS_PEER", json.dumps(result))

    def test_queued_r1_after_r2_is_valid_history_not_currentness(self):
        result = self.complete(seal([packet(1, 901, 6, 12), packet(2, 904, 5, 11)]))
        self.assertEqual([row["frame01"]["origin_revision"] for row in result["records"]], [12, 11])
        self.assertEqual(result["work_association"], "packet_row_only")

    def test_missing_build_commit_does_not_become_binary_source_proof(self):
        for commit in (None, ""):
            head = header()
            head["build_source_commit"] = commit
            result = self.complete(seal(head=head))
            self.assertIsNone(result["producer_commit"])
            self.assertEqual(result["source_binary_binding"], "unassessed")

    def test_missing_records_do_not_borrow_trace_ids_and_zero_ids_stay_missing(self):
        absent = missing()
        positive = packet(2, 0)
        positive.update(encoder=0, submission=0)
        result = self.complete(seal([absent, positive]))
        self.assertIsNone(result["records"][0]["frame01"])
        self.assertEqual(result["records"][0]["trace_encoder"], 902)
        self.assertIsNone(result["records"][1]["trace_encoder"])
        self.assertIsNone(result["records"][1]["trace_submission"])
        self.assertIsNone(result["records"][1]["packet"])
        self.assertEqual(result["records"][1]["frame01"]["encoder_lifetime"], 7)

    def test_negative_raw_output_pts_keeps_both_records_unavailable(self):
        for pts in (-1, -(1 << 63)):
            absent = missing()
            absent["frame_index"] = pts
            result = self.complete(seal([absent]))
            self.assertEqual(result["records"][0]["frame_index"], pts)
            self.assertIsNone(result["records"][0]["frame01"])
            self.assertIsNone(result["records"][0]["pyrowork"])
        self.refuse(seal([dict(packet(), frame_index=-1, frame01_input_pts=-1)]))
        self.refuse(seal([dict(work(), frame_index=-1, pyrowork_handoff_frame_index=-1)]))

    def test_both_origins_all_work_kinds_and_repeated_wrapper_local_sequences(self):
        rows = [work(kind, index + 1, 901 + index) for index, kind in enumerate(
            ("imported", "blank_primer", "blank_hdr", "packed", "retained"))]
        opened = packet(6, 906)
        opened["frame01_receipt_origin"] = "successful_open"
        rows.append(opened)
        result = self.complete(seal(rows))
        self.assertEqual([row["pyrowork"]["kind"] for row in result["records"][:5]],
                         ["imported", "blank_primer", "blank_hdr", "packed", "retained"])
        self.assertTrue(all(row["pyrowork"]["sequence"] == 1 for row in result["records"][:5]))
        self.assertEqual(result["records"][5]["frame01"]["receipt_origin"], "successful_open")

    def test_unsupported_versions_and_header_contract(self):
        for key, value in (("major", 1), ("minor", 2), ("minor", 4), ("minor", True),
                           ("schema", "old-v1"), ("queue_capacity", 511),
                           ("event_budget", 2000001), ("byte_budget", 268435457),
                           ("writer_lifetime_ns", 0), ("source_binding_requires_external_binary_receipt", False)):
            with self.subTest(key=key, value=value):
                head = header()
                head[key] = value
                self.refuse(seal(head=head))

    def test_malformed_duplicate_nonfinite_and_non_utf8_json(self):
        raw = line(packet())
        for malformed in (b"{\n", raw.replace(b'"packet":901', b'"packet":901,"packet":902'),
                          raw.replace(b'"packet":901', b'"packet":NaN'),
                          raw.replace(b'"packet":901', b'"packet":1e999'),
                          raw.replace(b'"packet":901', b'"packet":"\xff"'),
                          raw.replace(b'"packet":901', b'"packet":"\\ud800"')):
            with self.subTest(raw=malformed[:10]):
                self.refuse(seal([malformed]))
        main, close = seal()
        self.refuse((main, close.replace(b'"major":2', b'"major":2,"major":2')))
        head = header()
        head["identity"] = {"nested": {"marker": "private"}}
        main, close = seal(head=head)
        self.refuse((main.replace(b'"marker":"private"', b'"marker":"private","marker":"private"'), close))

    def test_packet_integer_ranges_and_bool_float_string_types(self):
        for key, value in (("packet", True), ("encoder", -1), ("submission", 1 << 64),
                           ("frame_index", -1), ("frame_timestamp_ns", -1), ("encode_done_ns", 1.0),
                           ("source_clock_timestamp", 2), ("source_time_paced", True),
                           ("frame01_origin_revision", "11"), ("frame01_application_sequence", 0),
                           ("frame01_encoder_lifetime", None), ("frame01_stream_generation", 0),
                           ("frame01_submission_sequence", 0), ("frame01_configured_encoder_kbps", 0),
                           ("frame01_configured_encoder_kbps", 1 << 31), ("frame01_input_pts", 1 << 63)):
            with self.subTest(key=key, value=value):
                row = packet()
                row[key] = value
                self.refuse(seal([row]))

    def test_availability_and_explicit_null_payloads_are_consistent(self):
        for row in (dict(packet(), frame01_available=0), dict(missing(), frame01_available=1),
                    dict(work(), pyrowork_available=0), dict(missing(), pyrowork_available=1),
                    dict(missing(), frame01_receipt_origin="successful_open"),
                    dict(missing(), pyrowork_sequence=0), dict(packet(), frame01_available=True)):
            with self.subTest(row=row):
                self.refuse(seal([row]))

    def test_invalid_enums_pts_and_copy_relationships_are_refused(self):
        for row in (dict(packet(), frame01_receipt_origin="current"), dict(packet(), frame01_input_pts=6),
                    dict(work(), pyrowork_authority="current"), dict(work(), pyrowork_kind="other"),
                    dict(work(), pyrowork_provenance="gpu_complete"), dict(work(), pyrowork_handoff_frame_index=6),
                    dict(work(), pyrowork_packet_copy_bytes=4), dict(work(), pyrowork_sequence=0),
                    dict(work(), pyrowork_budget_argument_bytes=0), dict(work(), pyrowork_completed_bitstream_bytes=True)):
            with self.subTest(row=row):
                self.refuse(seal([row]))

    def test_missing_extra_and_wrong_stage_packet_fields_are_refused(self):
        row = packet()
        del row["frame01_receipt_origin"]
        for changed in (row, dict(packet(), invented_authority=1), dict(packet(), record_stage="encode_submit")):
            self.refuse(seal([changed]))

    def test_event_order_duplicate_packet_and_invalid_flat_values_are_refused(self):
        for rows in ([packet(2)], [packet(), packet(2)], [packet(), packet(1, 904)],
                     [{"type": "other", "order": 1, "observed_steady_ns": 1, "bad": True}, packet(2)],
                     [{"type": "other", "order": 1, "observed_steady_ns": 1, "bad": {}}, packet(2)]):
            self.refuse(seal(rows))
        row = {"type": "other", "order": 1, "observed_steady_ns": 1}
        row.update({"field" + str(index): 0 for index in range(41)})
        self.refuse(seal([row, packet(2)]))

    def test_truncated_missing_nonterminal_and_duplicate_footer_are_refused(self):
        main, close = seal()
        parts = main.splitlines(keepends=True)
        for damaged in (main[:-1], b"".join(parts[:-1]), main + parts[-1],
                        b"".join((parts[0], parts[-1], parts[1])), main + b"\n"):
            self.refuse((damaged, close))
        self.refuse((main, close[:-1]))
        self.refuse((main, close + b"\n"))

    def test_content_hash_byte_count_and_footer_count_mismatch_are_refused(self):
        for changes in ({"content_sha256": "0" * 64}, {"bytes_before_footer": 1},
                        {"attempts": 2}, {"admitted": 0}, {"event_rows": 0},
                        {"major": 3}, {"build_source_commit": "2" * 40}, {"qualification": "passed"}):
            self.refuse(seal(footer_updates=changes))
        main, close = seal()
        self.refuse((main.replace(b'"packet":901', b'"packet":902'), close))

    def test_every_loss_cap_refusal_or_overflow_is_inconclusive(self):
        for key in ("lost_contention_or_full", "event_cap_loss", "invalid_event_loss", "byte_cap_loss",
                    "instrumentation_refusals"):
            with self.subTest(key=key):
                self.refuse(seal(footer_updates={key: 1}))
        for changes in ({"producer_complete": False}, {"duration_cap": True}, {"peer_overflow": True}):
            self.refuse(seal(footer_updates=changes))
        for kind in ("identity_refused", "instrumentation_refused"):
            self.refuse(seal([{"type": kind, "order": 1, "observed_steady_ns": 1}, packet(2)]))

    def test_close_failure_hash_count_schema_and_semantics_are_refused(self):
        for changes in ({"footer_written": False}, {"write_errno": 5}, {"main_close_errno": 5},
                        {"main_sha256": "0" * 64}, {"main_sha256": None}, {"main_bytes": 1},
                        {"major": 3}, {"minor": 3}, {"schema": "other"},
                        {"receipt_own_close": "success"}, {"durability": "fsync"}):
            with self.subTest(changes=changes):
                self.refuse(seal(close_updates=changes))
        main, _close = seal()
        self.refuse((main, b""))

    def test_empty_collection_budget_and_row_bounds_are_refused(self):
        self.refuse(seal([]))
        for key, value in (("event_budget", 0), ("byte_budget", 65535), ("byte_budget", 65536)):
            head = header()
            head[key] = value
            self.refuse(seal(head=head))
        head = header()
        head["event_budget"] = 1
        self.refuse(seal([packet(), packet(2, 904)], head=head))
        self.refuse((b"", b"{}\n"))

    def test_file_input_and_cli_failures_do_not_echo_private_paths(self):
        with tempfile.TemporaryDirectory(prefix="private-frame-fixture-") as directory:
            main, close = seal()
            main_path = os.path.join(directory, "private-raw.jsonl")
            close_path = main_path + ".close.json"
            for name, contents in ((main_path, main), (close_path, close)):
                with open(name, "wb") as output:
                    output.write(contents)
            self.assertEqual(reader.inspect_files(main_path, close_path)["status"], "COMPLETE_HISTORICAL_DIAGNOSTIC")
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(reader.main([main_path, close_path + ".missing"]), 1)
            self.assertNotIn(directory, output.getvalue())
            self.assertEqual(json.loads(output.getvalue())["reason"], "input_unavailable")
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                self.assertEqual(reader.main([main_path]), 1)
            self.assertNotIn(directory, output.getvalue())
            if hasattr(os, "symlink"):
                linked = os.path.join(directory, "linked")
                os.symlink(main_path, linked)
                self.assertEqual(reader.inspect_files(linked, close_path)["status"], "INCONCLUSIVE")


if __name__ == "__main__":
    unittest.main(verbosity=2)
