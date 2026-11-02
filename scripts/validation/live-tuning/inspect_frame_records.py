#!/usr/bin/env python3
"""Inspect historical packet records in complete private raw trace 2.3 bytes.

No media/policy/clock/source-binary qualification or control output. Never echo
input paths, raw identities, unrelated event fields or exception messages.
"""
import argparse
import hashlib
import json
import math
import os
import re
import stat
import sys

MAX_BYTES = 256 * 1024 * 1024
MAX_EVENTS = 2_000_000
FOOTER_RESERVE = 65536
U64 = (1 << 64) - 1
I64 = (1 << 63) - 1
SCHEMA = "polaris.packet-historical-receipts-inspection/v1"
HEADER_KEYS = frozenset((
    "type", "schema", "major", "minor", "identity", "queue_capacity",
    "event_budget", "byte_budget", "writer_lifetime_ns", "delivery_semantics",
    "clock_route", "raw_capture_peer_endpoint_facts", "build_source_commit",
    "source_binding_requires_external_binary_receipt", "code_constants"))
CONSTANT_KEYS = frozenset((
    "tick_ns", "liveness_ns", "max_source_age_ns", "burst_frames",
    "max_receipt_uncertainty_ns", "mapping_min_samples", "mapping_max_probe_samples",
    "mapping_min_probe_samples", "mapping_max_uncertainty_ns", "mapping_max_abs_offset_ns",
    "source_staleness_ns", "source_max_live_bracket_ns", "source_max_live_drift_ns",
    "source_max_total_uncertainty_ns", "submission_capacity"))
FOOTER_KEYS = frozenset((
    "type", "major", "identity", "peer_end", "peer_overflow", "attempts", "admitted",
    "event_rows", "lost_contention_or_full", "event_cap_loss", "invalid_event_loss",
    "byte_cap_loss", "duration_cap", "instrumentation_refusals", "content_sha256",
    "bytes_before_footer", "producer_complete", "main_close_result", "qualification",
    "build_source_commit"))
CLOSE_KEYS = frozenset((
    "type", "schema", "major", "main_bytes", "footer_written", "write_errno",
    "main_sha256", "main_close_errno", "receipt_own_close", "durability"))
SETTING_KEYS = (
    "frame01_encoder_lifetime", "frame01_stream_generation", "frame01_application_sequence",
    "frame01_origin_revision", "frame01_configured_encoder_kbps", "frame01_input_pts",
    "frame01_submission_sequence", "frame01_receipt_origin")
WORK_KEYS = (
    "pyrowork_authority", "pyrowork_provenance", "pyrowork_kind", "pyrowork_sequence",
    "pyrowork_handoff_frame_index", "pyrowork_budget_argument_bytes",
    "pyrowork_completed_bitstream_bytes", "pyrowork_packet_copy_bytes")
PACKET_KEYS = frozenset((
    "type", "order", "observed_steady_ns", "packet", "encoder", "submission", "frame_index",
    "frame_timestamp_ns", "encode_done_ns", "source_clock_timestamp", "source_time_paced",
    "record_stage", "frame01_available", "pyrowork_available") + SETTING_KEYS + WORK_KEYS)


class Refusal(Exception):
    """Only static non-identifying reason codes cross the output boundary."""


def need(condition, reason):
    if not condition:
        raise Refusal(reason)


def integer(value, minimum=0, maximum=U64):
    need(type(value) is int and minimum <= value <= maximum, "integer_range_or_type")
    return value


def exact(value, keys):
    need(type(value) is dict and value.keys() == keys, "object_keys")


def _pairs(pairs):
    result = {}
    for key, value in pairs:
        need(key not in result, "duplicate_key")
        result[key] = value
    return result


def _constant(_value):
    raise Refusal("nonfinite_json")


def _json_values(value):
    if type(value) is dict:
        for key, child in value.items():
            key.encode("utf-8")
            _json_values(child)
    elif type(value) is list:
        for child in value:
            _json_values(child)
    elif type(value) is int:
        integer(value, -(1 << 63), U64)
    elif type(value) is float:
        need(math.isfinite(value), "nonfinite_json")
    elif type(value) is str:
        value.encode("utf-8")


def parse(raw):
    try:
        value = json.loads(raw.decode("utf-8"), object_pairs_hook=_pairs, parse_constant=_constant)
        _json_values(value)
        need(type(value) is dict, "object_required")
        return value
    except Refusal:
        raise
    except (ValueError, UnicodeError, RecursionError):
        raise Refusal("malformed_json") from None


def digest(value):
    need(type(value) is str and re.fullmatch(r"[0-9a-f]{64}", value) is not None, "digest_type")


def header(row):
    exact(row, HEADER_KEYS)
    need(row["type"] == "header" and row["schema"] == "polaris.vrr-host-trace", "header_schema")
    need(type(row["major"]) is int and row["major"] == 2 and
         type(row["minor"]) is int and row["minor"] == 3, "unsupported_version")
    need(type(row["identity"]) is dict, "identity_shape")
    need(type(row["queue_capacity"]) is int and row["queue_capacity"] == 512, "queue_capacity")
    integer(row["event_budget"], 1, MAX_EVENTS)
    integer(row["byte_budget"], FOOTER_RESERVE, MAX_BYTES)
    need(type(row["writer_lifetime_ns"]) is int and row["writer_lifetime_ns"] == 1_800_000_000_000,
         "writer_lifetime")
    need(row["delivery_semantics"] == "socket_api_observations_only" and
         row["clock_route"] == "unqualified_requires_G1_G5_and_owner_topology" and
         row["raw_capture_peer_endpoint_facts"] == "epoch_peer_reads_and_footer_peer_snapshots" and
         row["source_binding_requires_external_binary_receipt"] is True, "header_semantics")
    commit = row["build_source_commit"]
    # CMake defines this macro even when COMMIT was unset, yielding an empty
    # string. That is absent provenance, never binary/source qualification.
    need(commit is None or (type(commit) is str and
         (commit == "" or re.fullmatch(r"[0-9a-f]{40}", commit))), "source_commit_shape")
    exact(row["code_constants"], CONSTANT_KEYS)
    for value in row["code_constants"].values():
        integer(value)


def packet(row):
    exact(row, PACKET_KEYS)
    need(row["record_stage"] == "sender_packet_dequeue", "record_stage")
    for key in ("packet", "encoder", "submission"):
        integer(row[key])
    # This is the original raw signed output PTS, including AV_NOPTS_VALUE.
    # Only present immutable records require a nonnegative exact PTS match.
    integer(row["frame_index"], -(1 << 63), I64)
    for key in ("frame_timestamp_ns", "encode_done_ns"):
        if row[key] is not None:
            integer(row[key], 0, I64)
    for key in ("source_clock_timestamp", "source_time_paced", "frame01_available", "pyrowork_available"):
        integer(row[key], 0, 1)
    setting = None
    if row["frame01_available"]:
        for key in ("frame01_encoder_lifetime", "frame01_stream_generation", "frame01_application_sequence",
                    "frame01_origin_revision", "frame01_submission_sequence"):
            integer(row[key], 1)
        integer(row["frame01_configured_encoder_kbps"], 1, (1 << 31) - 1)
        integer(row["frame01_input_pts"], 0, I64)
        need(row["frame01_input_pts"] == row["frame_index"], "setting_pts_mismatch")
        need(row["frame01_receipt_origin"] in ("successful_open", "backend_reconfiguration"), "setting_origin")
        setting = {key.removeprefix("frame01_"): row[key] for key in SETTING_KEYS}
    else:
        need(all(row[key] is None for key in SETTING_KEYS), "absent_setting_payload")
    work = None
    if row["pyrowork_available"]:
        need(row["pyrowork_authority"] == "unknown" and
             row["pyrowork_provenance"] == "wrapper_call_success_nonempty", "work_authority_or_provenance")
        need(row["pyrowork_kind"] in ("imported", "blank_primer", "blank_hdr", "packed", "retained"), "work_kind")
        integer(row["pyrowork_sequence"], 1)
        integer(row["pyrowork_handoff_frame_index"], 0, I64)
        need(row["pyrowork_handoff_frame_index"] == row["frame_index"], "work_pts_mismatch")
        for key in ("pyrowork_budget_argument_bytes", "pyrowork_completed_bitstream_bytes", "pyrowork_packet_copy_bytes"):
            integer(row[key], 1)
        need(row["pyrowork_completed_bitstream_bytes"] == row["pyrowork_packet_copy_bytes"], "work_copy_mismatch")
        work = {key.removeprefix("pyrowork_"): row[key] for key in WORK_KEYS}
    else:
        need(all(row[key] is None for key in WORK_KEYS), "absent_work_payload")
    # A row is its own association. Never join wrappers by local work sequence,
    # or manufacture trace IDs from setting scopes. Zero trace IDs stay absent.
    return {"order": row["order"], "packet": row["packet"] or None,
            "trace_encoder": row["encoder"] or None, "trace_submission": row["submission"] or None,
            "frame_index": row["frame_index"], "frame01": setting, "pyrowork": work}


def inspect_bytes(main, close):
    """Validate both original byte streams before returning any retained rows."""
    try:
        need(type(main) is bytes and 0 < len(main) <= MAX_BYTES, "main_size")
        need(type(close) is bytes and 0 < len(close) <= FOOTER_RESERVE, "close_size")
        need(main.endswith(b"\n") and close.endswith(b"\n"), "truncated_line")
        need(close.count(b"\n") == 1, "close_framing")
        # splitlines would also treat non-LF Unicode/control separators as rows.
        # The actual writer emits LF only; split this exact framing instead.
        lines = main.split(b"\n")
        need(len(lines) <= MAX_EVENTS + 3, "row_limit")
        head = parse(lines[0])
        header(head)
        need(len(main) <= head["byte_budget"], "byte_budget")
        records = []
        content = hashlib.sha256()
        bytes_before_footer = 0
        event_rows = 0
        footer = None
        seen_packets = set()
        for index, body in enumerate(lines[:-1]):
            row = head if index == 0 else parse(body)
            if index == 0:
                pass
            elif row.get("type") == "footer":
                need(index == len(lines) - 2, "footer_not_terminal")
                footer = row
                break
            else:
                need(row.get("type") not in ("header", "close_receipt") and type(row.get("type")) is str and
                     bool(row["type"]), "event_type")
                event_rows += 1
                need(event_rows <= head["event_budget"], "event_budget")
                integer(row.get("order"), 1)
                need(row["order"] == event_rows, "event_order")
                integer(row.get("observed_steady_ns"), 0, I64)
                need(row["type"] not in ("identity_refused", "instrumentation_refused"), "instrumentation_refusal")
                need(len(row) <= 43 + ("writer_peer_snapshot" in row), "event_field_limit")
                for key, value in row.items():
                    if key == "writer_peer_snapshot":
                        need(type(value) is dict and type(row.get("lease_pid")) is int and row["lease_pid"] > 1,
                             "peer_snapshot_shape")
                    else:
                        need(value is None or type(value) in (str, int, float), "event_field_type")
                need(bytes_before_footer + len(body) + 1 <= head["byte_budget"] - FOOTER_RESERVE,
                     "event_byte_reserve")
                if row["type"] == "packet_dequeue":
                    record = packet(row)
                    if row["packet"]:
                        need(row["packet"] not in seen_packets, "duplicate_packet")
                        seen_packets.add(row["packet"])
                    records.append(record)
            content.update(body + b"\n")
            bytes_before_footer += len(body) + 1
        need(footer is not None, "missing_footer")
        exact(footer, FOOTER_KEYS)
        need(type(footer["major"]) is int and footer["major"] == 2, "footer_version")
        need(type(footer["identity"]) is dict and type(footer["peer_end"]) is list and
             len(footer["peer_end"]) <= 64 and all(type(peer) is dict for peer in footer["peer_end"]), "footer_identity_shape")
        need(footer["peer_overflow"] is False and footer["duration_cap"] is False and
             footer["producer_complete"] is True, "producer_incomplete")
        for key in ("attempts", "admitted", "event_rows"):
            integer(footer[key])
            need(footer[key] == event_rows, "event_count")
        for key in ("lost_contention_or_full", "event_cap_loss", "invalid_event_loss", "byte_cap_loss", "instrumentation_refusals"):
            integer(footer[key])
            need(footer[key] == 0, "producer_loss_or_refusal")
        integer(footer["bytes_before_footer"])
        digest(footer["content_sha256"])
        need(footer["bytes_before_footer"] == bytes_before_footer and
             footer["content_sha256"] == content.hexdigest(), "content_integrity")
        need(footer["main_close_result"] == "requires_exclusive_close_receipt" and
             footer["qualification"] == "ungraded" and footer["build_source_commit"] == head["build_source_commit"],
             "footer_semantics")
        receipt = parse(close)
        exact(receipt, CLOSE_KEYS)
        need(receipt["type"] == "close_receipt" and receipt["schema"] == "polaris.vrr-host-trace" and
             type(receipt["major"]) is int and receipt["major"] == 2, "close_schema")
        integer(receipt["main_bytes"])
        integer(receipt["write_errno"])
        integer(receipt["main_close_errno"])
        digest(receipt["main_sha256"])
        need(receipt["footer_written"] is True and receipt["write_errno"] == 0 and receipt["main_close_errno"] == 0,
             "close_failure")
        raw_hash = hashlib.sha256(main).hexdigest()
        need(receipt["main_bytes"] == len(main) and receipt["main_sha256"] == raw_hash, "close_integrity")
        need(receipt["receipt_own_close"] == "not_observable_in_this_file" and
             receipt["durability"] == "no_fsync_claim", "close_semantics")
        need(bool(records), "no_packet_records")
        return {"schema": SCHEMA, "status": "COMPLETE_HISTORICAL_DIAGNOSTIC", "raw_sha256": raw_hash,
                "producer_commit": head["build_source_commit"] or None, "source_binary_binding": "unassessed",
                "other_event_semantics": "unassessed", "current_controller_permission": "unassessed",
                "gpu_completion": "unassessed", "physical_acceptance": False,
                "durability": "no_fsync_claim", "receipt_own_close": "not_observable_in_this_file",
                "work_association": "packet_row_only", "packet_rows": len(records),
                "frame01_records": sum(row["frame01"] is not None for row in records),
                "pyrowork_records": sum(row["pyrowork"] is not None for row in records), "records": records}
    except Refusal as error:
        return {"schema": SCHEMA, "status": "INCONCLUSIVE", "reason": str(error),
                "records": None, "physical_acceptance": False, "source_binary_binding": "unassessed"}


def _read(path, limit):
    need(all(hasattr(os, flag) for flag in ("O_CLOEXEC", "O_NOFOLLOW", "O_NONBLOCK")), "input_platform")
    descriptor = os.open(path, os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK)
    with os.fdopen(descriptor, "rb") as stream:
        info = os.fstat(stream.fileno())
        need(stat.S_ISREG(info.st_mode) and 0 < info.st_size <= limit, "input_file_size_or_type")
        raw = stream.read(limit + 1)
        need(len(raw) <= limit, "input_file_size_or_type")
        return raw


def inspect_files(main_path, close_path):
    try:
        return inspect_bytes(_read(main_path, MAX_BYTES), _read(close_path, FOOTER_RESERVE))
    except (OSError, Refusal, ValueError):
        return {"schema": SCHEMA, "status": "INCONCLUSIVE", "reason": "input_unavailable",
                "records": None, "physical_acceptance": False, "source_binary_binding": "unassessed"}


class _Parser(argparse.ArgumentParser):
    def error(self, _message):
        raise Refusal("arguments")


def main(argv=None):
    parser = _Parser(description="Inspect private raw 2.3 packet history; no media/control qualification.")
    parser.add_argument("trace")
    parser.add_argument("close_receipt")
    try:
        arguments = parser.parse_args(argv)
        result = inspect_files(arguments.trace, arguments.close_receipt)
    except Refusal:
        result = {"schema": SCHEMA, "status": "INCONCLUSIVE", "reason": "arguments", "records": None,
                  "physical_acceptance": False, "source_binary_binding": "unassessed"}
    print(json.dumps(result, separators=(",", ":"), allow_nan=False))
    return 0 if result["status"] == "COMPLETE_HISTORICAL_DIAGNOSTIC" else 1


if __name__ == "__main__":
    sys.exit(main())
