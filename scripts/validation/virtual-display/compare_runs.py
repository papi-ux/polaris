#!/usr/bin/env python3
"""Compare virtual display runs from measure_run.py with the client's readings and photo readings."""

from __future__ import annotations

import argparse
import csv
import json
import math
from pathlib import Path
import re
import statistics
import sys


SCHEMA = "polaris-virtual-display-run-v1"
KEY_COLUMNS = ("scenario", "host", "run")
# Moonlight's performance overlay, one row per run. Columns may be left out or left blank.
CLIENT_COLUMNS = (
    "received_fps", "decoded_fps", "rendered_fps", "network_drop_pct", "jitter_drop_pct", "rtt_ms",
    "rtt_variance_ms", "decode_ms", "queue_ms", "render_ms", "host_latency_min_ms",
    "host_latency_max_ms", "host_latency_avg_ms",
)
# One row per photo: the clock on the host monitor and the clock on the client, as read.
PHOTO_COLUMNS = ("scenario", "host", "run", "photo", "host_clock", "stream_clock")
# pattern.html shows seconds modulo 100.
CLOCK_PERIOD_S = 100.0
# A scenario's resolution, or a display mode with its frame rate: 2560x1440, 2560x1440x120.
MODE = re.compile(r"([0-9]+)x([0-9]+)(?:x([0-9]+(?:\.[0-9]+)?))?")
# Settings both hosts read that change the stream. A key a host's file leaves out runs on that
# host's own default, and the defaults differ: fec_percentage is 10 in Polaris and 20 in Hermes.
# So a key set on one host and left out on the other counts as a difference.
MATCHED_SETTINGS = ("encoder", "fec_percentage", "max_bitrate", "minimum_fps_target", "limit_framerate")


def number_at(*keys):
    def get(run: dict) -> float | None:
        value = run
        for key in keys:
            if not isinstance(value, dict) or key not in value:
                return None
            value = value[key]
        return value if isinstance(value, (int, float)) and not isinstance(value, bool) else None
    return get


def scaled(get, factor: float):
    def scale(run: dict) -> float | None:
        value = get(run)
        return None if value is None else value * factor
    return scale


def busiest_send_rate(run: dict) -> float | None:
    name = run.get("summary", {}).get("busiest_interface")
    return number_at("summary", "network_tx_mbps", name, "mean")(run) if name else None


def busiest_entry(section: str, field: str):
    """A field of the capture path that handled the most frames."""
    def get(run: dict) -> float | None:
        entries = run.get("summary", {}).get(section) or {}
        if not entries:
            return None
        entry = max(entries.values(), key=lambda entry: entry.get("frames", 0))
        value = entry.get(field)
        return value if isinstance(value, (int, float)) and not isinstance(value, bool) else None
    return get


def shortfall(run: dict) -> float | None:
    received, target = number_at("client", "received_fps")(run), number_at("target", "fps")(run)
    return 100.0 * (target - received) / target if received is not None and target else None


SHARED = (
    ("Frame processing latency, mean of 20 s averages", "ms", number_at("summary", "periodic", "frame_processing", "avg_ms")),
    ("Frame processing latency, slowest frame", "ms", number_at("summary", "periodic", "frame_processing", "max_ms")),
    ("Frame network latency, mean", "ms", number_at("summary", "periodic", "frame_network", "avg_ms")),
    ("Host process CPU, mean", "% of a core", number_at("summary", "cpu", "host_pct", "mean")),
    ("Host process CPU, p95", "% of a core", number_at("summary", "cpu", "host_pct", "p95")),
    ("KWin CPU, mean", "% of a core", number_at("summary", "cpu", "compositor_pct", "mean")),
    ("PipeWire CPU, mean", "% of a core", number_at("summary", "cpu", "pipewire_pct", "mean")),
    ("Desktop portal CPU, mean", "% of a core", number_at("summary", "cpu", "portal_pct", "mean")),
    ("Test page CPU, mean", "% of a core", number_at("summary", "cpu", "workload_pct", "mean")),
    ("Whole system CPU, mean", "% of all cores", number_at("summary", "cpu", "system_pct", "mean")),
    ("GPU busy, mean", "%", number_at("summary", "gpu", "gpu_busy", "mean")),
    ("VCN busy, mean", "%", number_at("summary", "gpu", "vcn_busy", "mean")),
    ("VCN busy time per target frame", "ms", number_at("summary", "gpu", "vcn_ms_per_target_frame")),
    ("GPU power, mean", "W", number_at("summary", "gpu", "power_w", "mean")),
    ("Network send rate, busiest interface", "Mb/s", busiest_send_rate),
    ("Host log lines", "per s", number_at("summary", "log_lines_per_second")),
)
CLIENT = (
    ("Received frame rate", "fps", number_at("client", "received_fps")),
    ("Received frames short of target", "%", shortfall),
    ("Rendered frame rate", "fps", number_at("client", "rendered_fps")),
    ("Frames dropped by the network", "%", number_at("client", "network_drop_pct")),
    ("Frames dropped by jitter", "%", number_at("client", "jitter_drop_pct")),
    ("Host processing latency, mean", "ms", number_at("client", "host_latency_avg_ms")),
    ("Host processing latency, max", "ms", number_at("client", "host_latency_max_ms")),
    ("Network round trip", "ms", number_at("client", "rtt_ms")),
    ("Decode time", "ms", number_at("client", "decode_ms")),
    ("Frame queue delay", "ms", number_at("client", "queue_ms")),
    ("Render time with V-sync", "ms", number_at("client", "render_ms")),
)
HOST_SPECIFIC = (
    ("Polaris capture, total p50", "us", busiest_entry("polaris_capture", "total_us_p50")),
    ("Polaris capture, total p99 (worst window)", "us", busiest_entry("polaris_capture", "total_us_p99")),
    ("Polaris capture, dispatch p50", "us", busiest_entry("polaris_capture", "dispatch_us_p50")),
    ("Polaris capture, ingest p50", "us", busiest_entry("polaris_capture", "ingest_us_p50")),
    ("Polaris encode time, mean", "ms", number_at("summary", "polaris_stats", "encode_time_ms", "mean")),
    ("Polaris encode time, p95", "ms", number_at("summary", "polaris_stats", "encode_time_ms", "p95")),
    ("Polaris stream frame rate, mean", "fps", number_at("summary", "polaris_stats", "fps", "mean")),
    ("Polaris encoder bitrate, mean", "Mb/s", scaled(number_at("summary", "polaris_stats", "bitrate_kbps", "mean"), 0.001)),
    ("Polaris dropped frames, mean", "%", scaled(number_at("summary", "polaris_stats", "dropped_frame_ratio", "mean"), 100)),
    ("Polaris duplicate frames, mean", "%", scaled(number_at("summary", "polaris_stats", "duplicate_frame_ratio", "mean"), 100)),
    ("Hermes KMS frame acquire, mean", "us", busiest_entry("hermes_capture", "avg_us")),
    ("Hermes KMS frame acquire, slowest", "us", busiest_entry("hermes_capture", "peak_us")),
)
SECTIONS = (
    ("Measured the same way on both hosts", True, SHARED),
    ("Reported by the client", True, CLIENT),
    ("Host-specific figures, not comparable across hosts", False, HOST_SPECIFIC),
)


def run_key(run: dict) -> tuple[str, str, int] | None:
    key = run.get("scenario"), run.get("host"), run.get("run")
    if isinstance(key[0], str) and isinstance(key[1], str) and isinstance(key[2], int) and not isinstance(key[2], bool):
        return key
    return None


def load_receipts(directory: Path, warnings: list[str]) -> dict[tuple[str, str, int], dict]:
    runs = {}
    for path in sorted(directory.glob("*.json")):
        try:
            receipt = json.loads(path.read_text())
        except (OSError, ValueError) as error:
            warnings.append(f"{path.name}: unreadable ({type(error).__name__}), skipped")
            continue
        if not isinstance(receipt, dict) or receipt.get("schema") != SCHEMA:
            warnings.append(f"{path.name}: not a {SCHEMA} receipt, skipped")
            continue
        key = run_key(receipt)
        if key is None:
            warnings.append(f"{path.name}: no scenario, host or run number, skipped")
            continue
        if key in runs:
            warnings.append(f"{path.name}: repeats {key[0]} {key[1]} run {key[2]}, skipped")
            continue
        receipt["file"] = path.name
        runs[key] = receipt
    return runs


def read_rows(path: Path, required: tuple[str, ...], optional: tuple[str, ...] = ()) -> list[dict]:
    """Rows of a CSV that may hold blank lines and # comments, each tagged with its line in the file."""
    with path.open(newline="") as handle:
        kept = [(number, line) for number, line in enumerate(handle, start=1) if line.strip() and not line.lstrip().startswith("#")]
    reader = csv.DictReader(line for _, line in kept)
    columns = [column.strip() for column in reader.fieldnames or []]
    missing = [column for column in required if column not in columns]
    unknown = [column for column in columns if column not in required + optional]
    if missing or unknown:
        problems = ([f"missing {', '.join(missing)}"] if missing else []) + ([f"unknown {', '.join(unknown)}"] if unknown else [])
        raise SystemExit(f"{path.name}: {'; '.join(problems)}")
    rows = []
    for row in reader:
        line = kept[reader.line_num - 1][0]
        row = {key.strip(): (value or "").strip() for key, value in row.items() if key}
        if not all(row.get(column) for column in KEY_COLUMNS):
            raise SystemExit(f"{path.name} line {line}: scenario, host and run are all needed")
        try:
            row["run"] = int(row["run"])
        except ValueError:
            raise SystemExit(f"{path.name} line {line}: run must be a whole number") from None
        row["line"] = line
        rows.append(row)
    return rows


def number(path: Path, row: dict, column: str) -> float | None:
    text = row.get(column, "")
    if not text:
        return None
    try:
        value = float(text)
    except ValueError:
        value = math.nan
    if not math.isfinite(value):
        raise SystemExit(f"{path.name} line {row['line']}: {column} is not a number")
    return value


def attach_client(path: Path, runs: dict, warnings: list[str]) -> None:
    seen = set()
    for row in read_rows(path, KEY_COLUMNS, CLIENT_COLUMNS):
        key = (row["scenario"], row["host"], row["run"])
        if key in seen:
            raise SystemExit(f"{path.name} line {row['line']}: {key[0]} {key[1]} run {key[2]} is listed twice")
        seen.add(key)
        readings = {column: value for column in CLIENT_COLUMNS if (value := number(path, row, column)) is not None}
        if key not in runs:
            warnings.append(f"{path.name} line {row['line']}: no receipt for {key[0]} {key[1]} run {key[2]}")
            runs[key] = {"scenario": key[0], "host": key[1], "run": key[2], "target": {}}
        runs[key]["client"] = readings


def photo_latency_ms(host_clock: float, stream_clock: float) -> float:
    """How far the stream's clock trailed the host monitor's, allowing for the clock wrapping."""
    difference = (host_clock - stream_clock) % CLOCK_PERIOD_S
    if difference > CLOCK_PERIOD_S / 2:
        difference -= CLOCK_PERIOD_S
    return round(difference * 1000, 1)


def read_photos(path: Path, warnings: list[str]) -> list[dict]:
    photos = []
    for row in read_rows(path, PHOTO_COLUMNS):
        host_clock, stream_clock = number(path, row, "host_clock"), number(path, row, "stream_clock")
        if host_clock is None or stream_clock is None:
            warnings.append(f"{path.name} line {row['line']}: a clock is blank, so the photo is left out")
            continue
        photos.append({
            "scenario": row["scenario"], "host": row["host"], "run": row["run"], "photo": row["photo"],
            "latency_ms": photo_latency_ms(host_clock, stream_clock),
        })
    return photos


def nearest_rank(values: list[float], fraction: float) -> float:
    ordered = sorted(values)
    return ordered[max(1, math.ceil(round(len(ordered) * fraction, 9))) - 1]


def aggregate(values: list[float]) -> dict:
    values = [value for value in values if value is not None]
    if not values:
        return {"n": 0}
    return {"n": len(values), "mean": statistics.fmean(values), "min": min(values), "max": max(values)}


def host_order(hosts) -> list[str]:
    return sorted(set(hosts), key=lambda host: (host != "polaris", host))


def parse_mode(text) -> tuple[int, int, float | None] | None:
    """Width, height and frame rate, if given, from 2560x1440 or 2560x1440x120."""
    match = MODE.fullmatch(text) if isinstance(text, str) else None
    if not match:
        return None
    return int(match.group(1)), int(match.group(2)), float(match.group(3)) if match.group(3) else None


def check_run(run: dict, warnings: list[str]) -> None:
    name = run.get("file") or f"{run['scenario']} {run['host']} run {run['run']}"
    if "summary" not in run:
        return
    summary = run["summary"]
    if not run.get("complete", False):
        warnings.append(f"{name}: stopped early")
    if not summary.get("periodic"):
        warnings.append(f"{name}: no host timing lines; was the stream running with min_log_level = debug?")
    processes = summary.get("processes", {})
    if processes.get("host") == 0:
        warnings.append(f"{name}: the host process was not found")
    if processes.get("workload") == 0:
        warnings.append(f"{name}: the test page was not running")
    stats = summary.get("polaris_stats")
    target = run.get("target", {})
    if stats is not None:
        if not stats.get("streaming_records"):
            errors = ", ".join(f"{error} x{count}" for error, count in sorted(stats.get("errors", {}).items()))
            warnings.append(f"{name}: Polaris stats never showed a stream ({errors or 'no errors'})")
        path, mode = stats.get("stream_path_id"), stats.get("stream_display_mode")
        if (path and path != "host_virtual_display") or (not path and mode and "virtual" not in str(mode).lower()):
            warnings.append(f"{name}: Polaris streamed with {mode or path}, not Host Virtual Display")
        if stats.get("adaptive_bitrate_active") is True:
            warnings.append(f"{name}: Polaris adaptive bitrate was active")
        # The encoder's rate sits below the client's setting by the FEC, audio and packet
        # overhead, so only a change during the run says the bitrate was not fixed.
        bitrate = stats.get("bitrate_kbps") or {}
        if bitrate.get("mean") is not None and bitrate.get("max") != bitrate.get("mean"):
            warnings.append(f"{name}: Polaris changed the encoder bitrate during the run")
        # A bitrate saved for the paired device caps what the client asks for.
        saved, asked = number_at("paired_target_bitrate_kbps", "max")(stats), number_at("bitrate_mbps")(target)
        if saved and asked and saved < asked * 1000:
            warnings.append(f"{name}: Polaris has {saved:g} kbps saved as this device's bitrate, below the run's {asked * 1000:g}")
        codec = stats.get("codec")
        if codec and target.get("codec") and str(codec).lower() != str(target["codec"]).lower():
            warnings.append(f"{name}: Polaris encoded {codec}, the run asked for {target['codec']}")
        # The virtual display takes the launch's mode, which a mode saved for the paired device
        # replaces. The stream's width and height stay the client's own whatever the display got.
        size, fps = parse_mode(target.get("resolution")), number_at("fps")(target)
        applied = parse_mode(stats.get("display_mode_applied"))
        if size and applied and (applied[:2] != size[:2] or (fps and applied[2] and abs(applied[2] - fps) > 0.5)):
            saved_mode = " (saved for this device)" if stats.get("display_mode_pinned_by_host") is True else ""
            asked_for = target["resolution"] + (f" at {fps:g} fps" if fps else "")
            warnings.append(f"{name}: Polaris gave the virtual display {stats['display_mode_applied']}{saved_mode}, the run asked for {asked_for}")
        width, height = number_at("width", "max")(stats), number_at("height", "max")(stats)
        if size and width and height and (width, height) != size[:2]:
            warnings.append(f"{name}: Polaris encoded {width:g}x{height:g}, the run asked for {target['resolution']}")
        session_fps = number_at("session_target_fps", "mean")(stats)
        if fps and session_fps and abs(session_fps - fps) > 0.5:
            warnings.append(f"{name}: Polaris aimed for {session_fps:g} fps, the run asked for {fps:g}")
        hdr = stats.get("stream_hdr_enabled")
        if isinstance(hdr, bool) and target.get("dynamic_range") in ("sdr", "hdr") and hdr != (target["dynamic_range"] == "hdr"):
            warnings.append(f"{name}: Polaris streamed {'HDR' if hdr else 'SDR'}, the run asked for {target['dynamic_range'].upper()}")
    if run["host"].startswith("hermes") and not run.get("environment", {}).get("modules", {}).get("hermes_kms", {}).get("loaded"):
        warnings.append(f"{name}: hermes_kms was not loaded")


def environment_rows(runs: list[dict]) -> list[tuple[str, str]]:
    environment = runs[0].get("environment", {})
    packages = environment.get("packages", {})
    modules = environment.get("modules", {})
    gpus = [
        f"{card.get('name', 'unknown')} ({card.get('pci_id', '?')})"
        for card in environment.get("drm", {}).get("cards", [])
        if card.get("driver") in ("amdgpu", "i915", "xe", "nvidia")
    ]

    def module(name: str) -> str:
        entry = modules.get(name, {})
        if not entry.get("loaded"):
            return "not loaded"
        return "loaded" + (f", {entry['version']}" if entry.get("version") else "")

    def package(*names: str) -> str:
        return next((packages[name] for name in names if name in packages), "not installed")

    return [
        ("GPU", "; ".join(gpus) or "unknown"),
        ("Kernel", environment.get("kernel") or "unknown"),
        ("Mesa", package("mesa", "mesa-dri-drivers", "libgl1-mesa-dri")),
        ("KWin", package("kwin")),
        ("PipeWire", package("pipewire")),
        ("Polaris", package("polaris", "Polaris")),
        ("Hermes", package("hermes-streaming", "hermes")),
        ("Hermes-KMS", package("hermes-kms-dkms-git", "hermes-kms-dkms")),
        ("hermes_kms module", module("hermes_kms")),
        ("evdi module", module("evdi")),
    ]


def environment_drift(host: str, runs: list[dict], warnings: list[str]) -> None:
    def fingerprint(run: dict) -> tuple:
        environment = run.get("environment", {})
        return environment.get("kernel"), json.dumps(environment.get("packages", {}), sort_keys=True), json.dumps(run.get("host_settings", {}), sort_keys=True)

    prints = {fingerprint(run) for run in runs if "environment" in run}
    if len(prints) > 1:
        warnings.append(f"{host}: kernel, packages or host settings changed between runs")


def build_report(runs: dict, photos: list[dict], warnings: list[str]) -> dict:
    for key in sorted(runs):
        check_run(runs[key], warnings)
    scenarios = []
    for scenario in sorted({key[0] for key in runs} | {photo["scenario"] for photo in photos}):
        members = [run for key, run in sorted(runs.items()) if key[0] == scenario]
        scenario_photos = [photo for photo in photos if photo["scenario"] == scenario]
        hosts = host_order([run["host"] for run in members] + [photo["host"] for photo in scenario_photos])
        targets = {json.dumps(run.get("target", {}), sort_keys=True) for run in members if run.get("target")}
        if len(targets) > 1:
            warnings.append(f"{scenario}: the runs asked for different targets; see each receipt's target")
        target = next((run["target"] for run in members if run.get("target")), {})
        sections = []
        for title, comparable, metrics in SECTIONS:
            rows = []
            for metric, unit, get in metrics:
                values = {host: aggregate([get(run) for run in members if run["host"] == host]) for host in hosts}
                if any(value["n"] for value in values.values()):
                    rows.append({"metric": metric, "unit": unit, "values": values})
            if rows:
                sections.append({"title": title, "comparable": comparable, "rows": rows})
        glass = {}
        for host in hosts:
            latencies = [photo["latency_ms"] for photo in scenario_photos if photo["host"] == host]
            if latencies:
                glass[host] = {
                    "n": len(latencies), "median": statistics.median(latencies),
                    "p90": nearest_rank(latencies, 0.9), "min": min(latencies), "max": max(latencies),
                }
        scenarios.append({
            "scenario": scenario, "target": target, "hosts": hosts,
            "runs": {host: sum(1 for run in members if run["host"] == host and "summary" in run) for host in hosts},
            "sections": sections, "glass_to_glass_ms": glass,
        })

    environments, settings = {}, {}
    for host in host_order(key[1] for key in runs):
        measured = [run for key, run in sorted(runs.items()) if key[1] == host and "environment" in run]
        if measured:
            environment_drift(host, measured, warnings)
            environments[host] = dict(environment_rows(measured))
            settings[host] = measured[0].get("host_settings") or {}
    # The hosts are meant to share one machine and one desktop, so only their own packages differ.
    for label in ("GPU", "Kernel", "Mesa", "KWin", "PipeWire"):
        if len({rows[label] for rows in environments.values()}) > 1:
            warnings.append(f"{label} differs between hosts: " + ", ".join(f"{host} {rows[label]}" for host, rows in environments.items()))
    for key in MATCHED_SETTINGS:
        values = {host: host_settings.get(key, "not set") for host, host_settings in settings.items()}
        if len(set(values.values())) > 1:
            warnings.append(f"{key} differs between hosts: " + ", ".join(f"{host} {value}" for host, value in values.items()))
    return {"scenarios": scenarios, "environment": environments, "warnings": warnings}


def fmt(value: float | None) -> str:
    if value is None:
        return "n/a"
    magnitude = abs(value)
    # Frame rates near 120 still need their tenths.
    digits = 0 if magnitude >= 1000 else 1 if magnitude >= 10 else 2
    return f"{value:.{digits}f}"


def cell(value: dict) -> str:
    if not value["n"]:
        return "n/a"
    if value["n"] == 1 or value["min"] == value["max"]:
        return fmt(value["mean"])
    return f"{fmt(value['mean'])} ({fmt(value['min'])} to {fmt(value['max'])})"


def difference(first: dict, second: dict) -> str:
    if not first["n"] or not second["n"]:
        return ""
    change = second["mean"] - first["mean"]
    text = ("+" if change > 0 else "") + fmt(change)
    if first["mean"]:
        text += f" ({'+' if change > 0 else ''}{100 * change / abs(first['mean']):.0f}%)"
    return text


def describe_target(target: dict) -> str:
    parts = []
    if target.get("resolution"):
        parts.append(str(target["resolution"]))
    if target.get("fps"):
        parts.append(f"{target['fps']:g} fps")
    if target.get("codec") or target.get("dynamic_range"):
        parts.append(f"{str(target.get('codec', '')).upper()} {str(target.get('dynamic_range', '')).upper()}".strip())
    if target.get("bitrate_mbps"):
        parts.append(f"{target['bitrate_mbps']:g} Mb/s")
    return ", ".join(parts)


def render_markdown(report: dict) -> str:
    out = []
    for scenario in report["scenarios"]:
        hosts = scenario["hosts"]
        heading = scenario["scenario"]
        if described := describe_target(scenario["target"]):
            heading += f": {described}"
        out.append(f"## {heading}\n")
        counts = ", ".join(f"{host} {scenario['runs'][host]}" for host in hosts)
        out.append(f"Runs: {counts}. Each value is the mean over runs, with the lowest and highest run in brackets.\n")
        for section in scenario["sections"]:
            out.append(f"### {section['title']}\n")
            # Every other host is set against the first, which is Polaris when it ran.
            others = hosts[1:] if section["comparable"] else []
            header = ["Metric", *hosts, *(f"{host} vs {hosts[0]}" for host in others)]
            out.append("| " + " | ".join(header) + " |")
            out.append("|---" + "|---:" * (len(header) - 1) + "|")
            for row in section["rows"]:
                cells = [
                    f"{row['metric']} ({row['unit']})", *(cell(row["values"][host]) for host in hosts),
                    *(difference(row["values"][hosts[0]], row["values"][host]) for host in others),
                ]
                out.append("| " + " | ".join(cells) + " |")
            out.append("")
        if glass := scenario["glass_to_glass_ms"]:
            out.append("### Glass to glass, added over the host monitor (photos)\n")
            out.append("| | " + " | ".join(hosts) + " |")
            out.append("|---" + "|---:" * len(hosts) + "|")
            for label, field in (("Photos", "n"), ("Median (ms)", "median"), ("p90 (ms)", "p90"), ("Lowest (ms)", "min"), ("Highest (ms)", "max")):
                values = [
                    "n/a" if host not in glass else str(glass[host][field]) if field == "n" else fmt(glass[host][field])
                    for host in hosts
                ]
                out.append(f"| {label} | " + " | ".join(values) + " |")
            out.append("")
    if report["environment"]:
        hosts = list(report["environment"])
        out.append("## Environment\n")
        out.append("| | " + " | ".join(hosts) + " |")
        out.append("|---" + "|---" * len(hosts) + "|")
        for label in report["environment"][hosts[0]]:
            out.append(f"| {label} | " + " | ".join(report["environment"][host].get(label, "") for host in hosts) + " |")
        out.append("")
    if report["warnings"]:
        out.append("## Check before publishing\n")
        out.extend(f"- {warning}" for warning in report["warnings"])
        out.append("")
    return "\n".join(out)


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("receipts", type=Path, help="directory holding the run receipts")
    parser.add_argument("--client", type=Path, help="CSV of the client overlay readings")
    parser.add_argument("--photos", type=Path, help="CSV of the glass-to-glass photo readings")
    parser.add_argument("--json", type=Path, help="also write the report as JSON")
    args = parser.parse_args(argv)
    if not args.receipts.is_dir():
        parser.error(f"{args.receipts} is not a directory")

    warnings: list[str] = []
    runs = load_receipts(args.receipts, warnings)
    if args.client:
        attach_client(args.client, runs, warnings)
    photos = read_photos(args.photos, warnings) if args.photos else []
    if not runs and not photos:
        parser.error("found no receipts or photo readings")
    report = build_report(runs, photos, warnings)
    print(render_markdown(report))
    if args.json:
        args.json.write_text(json.dumps(report, indent=1) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
