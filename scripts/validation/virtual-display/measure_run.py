#!/usr/bin/env python3
"""Sample one stream for the virtual display comparison; reads the host and changes nothing."""

from __future__ import annotations

import argparse
from collections import Counter
from datetime import datetime, timezone
import ipaddress
import json
import math
import os
from pathlib import Path
import platform
import re
import signal
import ssl
import statistics
import subprocess
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request


SCHEMA = "polaris-virtual-display-run-v1"
SAFE_ENV = {"PATH": "/usr/sbin:/usr/bin:/sbin:/bin", "LC_ALL": "C"}
CLOCK_TICKS = os.sysconf("SC_CLK_TCK")
PROC = Path("/proc")
DRM = Path("/sys/class/drm")
SYS_MODULE = Path("/sys/module")
LABEL = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}")

DEFAULT_LOGS = {
    "polaris": "~/.config/polaris/polaris.log",
    "hermes": "~/.config/hermes/hermes.log",
}
DEFAULT_CONFIGS = {
    "polaris": "~/.config/polaris/polaris.conf",
    "hermes": "~/.config/hermes/hermes.conf",
}
DEFAULT_STATS_URL = "https://localhost:47990/api/stats/stream-sse"
# The streamed page's browser profile, as launch_page.sh names it. The reference clock on the
# host monitor runs under vdpattern-reference and is not part of the test page.
DEFAULT_WORKLOAD = "vdpattern-pattern"

# CPU time is reported for these groups as well as the host. Kernel command names stop at 15
# characters, so the portal processes all share one prefix.
GROUPS = ("host", "compositor", "xwayland", "pipewire", "portal", "workload")
PIPEWIRE = frozenset({"pipewire", "pipewire-pulse", "wireplumber"})
PORTAL_PREFIX = "xdg-desktop-por"

# Polaris and Hermes write these through the same periodic logger, every 20 seconds at debug
# level, so they are the host timings both report in the same way.
PERIODIC_LINE = re.compile(
    r"(Frame processing latency|Network: each send_batch\(\) latency|"
    r"Network: each FEC block latency|Network: frame's overall network latency)"
    r" \(min/max/avg\): ([0-9.]+)ms/([0-9.]+)ms/([0-9.]+)ms"
)
PERIODIC_NAMES = {
    "Frame processing latency": "frame_processing",
    "Network: each send_batch() latency": "send_batch",
    "Network: each FEC block latency": "fec_block",
    "Network: frame's overall network latency": "frame_network",
}
# Polaris with linux_capture_profile = enabled: one line per 600 captured frames, at info level.
POLARIS_CAPTURE_LINE = re.compile(r"capture_telemetry: transport=(\w+)((?: \w+=-?[0-9]+)+)")
POLARIS_CAPTURE_P50 = ("dispatch_us_p50", "ingest_us_p50", "total_us_p50", "source_interval_us_p50")
POLARIS_CAPTURE_P99 = ("dispatch_us_p99", "ingest_us_p99", "total_us_p99", "source_interval_us_p99")
# Hermes: one line per 120 frames, at info level. It times the KMS acquire call and nothing else.
NUMBER = r"([0-9.]+(?:e[+-]?[0-9]+)?)"
HERMES_CAPTURE_LINE = re.compile(
    r"\[capture-metric/([\w.-]+)\] avg=" + NUMBER + r"us peak=" + NUMBER + r"us over ([0-9]+) frames"
)

# The stats payload also names the client and its address. A receipt keeps these fields only.
STATS_NUMBERS = (
    "fps", "requested_client_fps", "session_target_fps", "encode_target_fps", "capture_source_fps",
    "runtime_reported_refresh_hz", "bitrate_kbps", "effective_launch_bitrate_kbps",
    "paired_target_bitrate_kbps", "adaptive_target_bitrate_kbps", "encode_time_ms",
    "duplicate_frame_ratio", "dropped_frame_ratio", "avg_frame_age_ms", "frame_interval_error_ms",
    "width", "height", "latency_ms", "packet_loss", "gpu_usage", "idr_requests_total",
    "invalidate_ref_frames_requests_total",
)
STATS_LABELS = (
    "streaming", "codec", "encoder_backend", "dynamic_range", "stream_hdr_enabled", "color_coding",
    "stream_chroma", "stream_display_mode", "stream_path_id", "runtime_backend", "capture_path", "capture_transport",
    "capture_residency", "capture_format", "capture_cpu_copy", "capture_gpu_native", "capture_pacing",
    "pacing_policy", "capture_backend_opened", "capture_backend_route", "encode_target_residency",
    "encode_target_format", "adaptive_bitrate_enabled", "adaptive_bitrate_active", "optimization_source",
)
# From the launch's display_mode_decision: the mode the client asked for, the mode the virtual
# display was given, and whether a mode saved for the paired device replaced the request. The
# stream's width and height are the client's own size whatever the display was given, so only
# these say what was captured. Only mode strings such as 2560x1440x120 are kept.
DISPLAY_MODE_LABELS = ("display_mode_requested", "display_mode_applied", "display_mode_pinned_by_host")
DISPLAY_MODE = re.compile(r"[0-9]{1,5}x[0-9]{1,5}(?:x[0-9]{1,4}(?:\.[0-9]{1,3})?)?")

# Settings that change the work done per frame. Names, paths, addresses and keys in the same
# files are never read into a receipt.
CONFIG_KEYS = (
    "adapter_name", "capture", "encoder", "hevc_mode", "av1_mode", "fec_percentage", "qp", "min_threads",
    "max_bitrate", "minimum_fps_target", "limit_framerate", "double_refreshrate", "headless_mode",
    "min_log_level", "isolated_virtual_display_option", "lan_encryption_mode", "wan_encryption_mode",
    "output_name", "amd_usage", "amd_rc", "amd_quality", "amd_preanalysis", "amd_vbaq", "amd_coder",
    "amd_enforce_hrd", "adaptive_bitrate_enabled", "adaptive_bitrate_min", "adaptive_bitrate_max",
    "ai_enabled", "benchmark_mode_enabled", "color_range", "hdr_mode", "headless_max_refresh_rate",
    "linux_auto_manage_displays", "linux_capture_profile", "linux_prefer_gpu_native_capture",
    "linux_primary_output", "linux_private_runtime", "linux_streaming_output", "linux_stream_mode",
    "linux_use_cage_compositor", "linux_virtual_display_backend",
    "mouse_cursor_visible", "recording_enabled", "recording_replay_buffer", "vk_quality", "vk_rc_mode",
    "vk_tune", "virtual_display_backend", "hermes_kms_isolated_sessions", "hermes_kms_multi_output",
    "hermes_kms_session_compositor", "vaapi_quality", "vaapi_rc", "vaapi_blbrc", "vaapi_strict_rc_buffer",
    "default_scale_factor", "gamescope_backend", "appliance_mode",
)
CONFIG_VALUE = re.compile(r"[\w.:/,+ -]{0,120}")

PACKAGES = (
    "mesa", "mesa-dri-drivers", "libgl1-mesa-dri", "kwin", "plasma-workspace", "kscreen", "pipewire",
    "polaris", "Polaris", "hermes-streaming", "hermes", "hermes-kms-dkms-git", "hermes-kms-dkms",
    "evdi-dkms", "firefox", "google-chrome", "google-chrome-stable", "discord",
)
MODULES = ("amdgpu", "i915", "xe", "nvidia", "evdi", "hermes_kms", "vkms")
MODULE_PARAMETERS = ("evdi", "hermes_kms")
PCI_IDS = (Path("/usr/share/hwdata/pci.ids"), Path("/usr/share/misc/pci.ids"))


def run(argv: list[str]) -> str | None:
    """Stdout of a read-only query, or None when the tool is missing or hangs."""
    try:
        result = subprocess.run(
            argv, capture_output=True, text=True, timeout=30, env=SAFE_ENV, check=False,
        )
    except (OSError, subprocess.TimeoutExpired):
        return None
    return result.stdout


def read_text(path: Path) -> str | None:
    try:
        return path.read_text(errors="replace").strip()
    except OSError:
        return None


def read_int(path: Path) -> int | None:
    text = read_text(path)
    try:
        return int(text) if text is not None else None
    except ValueError:
        return None


def mean(values: list[float]) -> float | None:
    return round(statistics.fmean(values), 3) if values else None


def percentile(values: list[float], fraction: float) -> float | None:
    """Nearest-rank percentile, so the value reported is one that was measured."""
    if not values:
        return None
    ordered = sorted(values)
    return ordered[max(1, math.ceil(round(len(ordered) * fraction, 9))) - 1]


def describe(values: list[float]) -> dict | None:
    return {"mean": mean(values), "p95": percentile(values, 0.95), "max": max(values)} if values else None


def parse_log_line(line: str) -> dict | None:
    if match := PERIODIC_LINE.search(line):
        return {
            "kind": "periodic", "name": PERIODIC_NAMES[match.group(1)],
            "min_ms": float(match.group(2)), "max_ms": float(match.group(3)),
            "avg_ms": float(match.group(4)),
        }
    if match := POLARIS_CAPTURE_LINE.search(line):
        fields = dict(item.split("=", 1) for item in match.group(2).split())
        return {
            "kind": "polaris_capture", "transport": match.group(1),
            **{name: int(value) for name, value in fields.items()},
        }
    if match := HERMES_CAPTURE_LINE.search(line):
        return {
            "kind": "hermes_capture", "label": match.group(1), "avg_us": float(match.group(2)),
            "peak_us": float(match.group(3)), "frames": int(match.group(4)),
        }
    return None


def parse_proc_stat(text: str) -> tuple[str, int, int, int] | None:
    """Command name, parent PID, CPU ticks (user + system) and start tick from /proc/PID/stat."""
    opening, closing = text.find("("), text.rfind(")")
    if opening < 0 or closing < opening:
        return None
    fields = text[closing + 2:].split()
    if len(fields) < 20:
        return None
    return text[opening + 1:closing], int(fields[1]), int(fields[11]) + int(fields[12]), int(fields[19])


def parse_cpu_totals(text: str) -> tuple[int, int]:
    """Busy and total ticks across every CPU, from the first line of /proc/stat."""
    for line in text.splitlines():
        if line.startswith("cpu "):
            values = [int(value) for value in line.split()[1:9]]
            idle = values[3] + values[4]
            return sum(values) - idle, sum(values)
    raise ValueError("/proc/stat has no cpu line")


def parse_net_dev(text: str) -> dict[str, int]:
    """Bytes sent so far by each interface other than loopback, from /proc/net/dev."""
    sent = {}
    for line in text.splitlines()[2:]:
        name, _, counters = line.partition(":")
        fields = counters.split()
        if len(fields) >= 16 and name.strip() != "lo":
            sent[name.strip()] = int(fields[8])
    return sent


def own_lineage(proc: Path = PROC) -> set[int]:
    """This sampler and its ancestors, whose command lines can repeat the workload match."""
    lineage, pid = set(), os.getpid()
    while pid > 1 and pid not in lineage:
        lineage.add(pid)
        parsed = parse_proc_stat(read_text(proc / str(pid) / "stat") or "")
        if not parsed:
            break
        pid = parsed[1]
    return lineage


class ProcessSampler:
    """CPU use of the host, the desktop's streaming services and the test page between two reads."""

    def __init__(self, host: str, compositors: list[str], workload_match: str, proc: Path = PROC):
        self.host = host
        self.compositors = set(compositors)
        self.workload_match = workload_match
        self.proc = proc
        self.excluded = own_lineage(proc)
        self.commands: dict[tuple[int, int], str] = {}
        self.previous: dict[tuple[int, int], int] | None = None
        self.previous_uptime = 0.0
        self.previous_cpu = (0, 0)
        self.previous_time = 0.0

    def command(self, pid: int, start: int) -> str:
        key = (pid, start)
        if key not in self.commands:
            try:
                raw = (self.proc / str(pid) / "cmdline").read_bytes()
            except OSError:
                raw = b""
            self.commands[key] = raw.replace(b"\0", b" ").decode(errors="replace")
        return self.commands[key]

    def table(self) -> dict[int, tuple[str, int, int, int]]:
        table = {}
        for entry in os.scandir(self.proc):
            if not entry.name.isdigit():
                continue
            parsed = parse_proc_stat(read_text(self.proc / entry.name / "stat") or "")
            if parsed:
                table[int(entry.name)] = parsed
        return table

    def groups(self, table: dict[int, tuple[str, int, int, int]]) -> dict[str, set[int]]:
        result: dict[str, set[int]] = {name: set() for name in GROUPS}
        children: dict[int, list[int]] = {}
        hosts, pages = [], []
        for pid, (name, parent, _, start) in table.items():
            children.setdefault(parent, []).append(pid)
            if name == self.host:
                hosts.append(pid)
            elif name in self.compositors:
                result["compositor"].add(pid)
            elif name == "Xwayland":
                result["xwayland"].add(pid)
            elif name in PIPEWIRE:
                result["pipewire"].add(pid)
            elif name.startswith(PORTAL_PREFIX):
                result["portal"].add(pid)
            if self.workload_match and pid not in self.excluded and self.workload_match in self.command(pid, start):
                pages.append(pid)

        def descendants(roots: list[int]) -> set[int]:
            found, pending = set(), list(roots)
            while pending:
                pid = pending.pop()
                if pid not in found:
                    found.add(pid)
                    pending.extend(children.get(pid, ()))
            return found

        # The page's content processes are children of the browser that matched. The host's
        # helpers count as the host, except a page the host launched as the streamed app.
        result["workload"] = descendants(pages)
        result["host"] = descendants(hosts) - result["workload"]
        return result

    def sample(self) -> dict | None:
        now = time.monotonic()
        uptime = float((read_text(self.proc / "uptime") or "0").split()[0])
        busy, total = parse_cpu_totals(read_text(self.proc / "stat") or "")
        table = self.table()
        ticks = {(pid, entry[3]): entry[2] for pid, entry in table.items()}
        result = None
        if self.previous is not None:
            elapsed = now - self.previous_time
            result = {}
            for name, members in self.groups(table).items():
                used = 0
                for pid in members:
                    key = (pid, table[pid][3])
                    if key in self.previous:
                        used += max(0, ticks[key] - self.previous[key])
                    elif key[1] / CLOCK_TICKS >= self.previous_uptime:
                        # Started inside this interval, so all of its time belongs to it.
                        used += ticks[key]
                result[name + "_pct"] = round(100.0 * used / CLOCK_TICKS / elapsed, 2)
                result[name + "_processes"] = len(members)
            busy_ticks, total_ticks = busy - self.previous_cpu[0], total - self.previous_cpu[1]
            result["system_pct"] = round(100.0 * busy_ticks / total_ticks, 2) if total_ticks > 0 else None
            result["system_cores"] = round(busy_ticks / CLOCK_TICKS / elapsed, 3)
        self.previous = ticks
        self.previous_uptime = uptime
        self.previous_cpu = (busy, total)
        self.previous_time = now
        return result


class NetworkSampler:
    """Send rate of each interface between two reads, in megabits per second."""

    def __init__(self, path: Path = PROC / "net" / "dev"):
        self.path = path
        self.previous: dict[str, int] | None = None
        self.previous_time = 0.0

    def sample(self) -> dict | None:
        now = time.monotonic()
        sent = parse_net_dev(read_text(self.path) or "")
        result = None
        if self.previous is not None and now > self.previous_time:
            elapsed = now - self.previous_time
            result = {
                name: round((count - self.previous[name]) * 8 / elapsed / 1_000_000, 3)
                for name, count in sent.items()
                if name in self.previous and count >= self.previous[name]
            }
        self.previous, self.previous_time = sent, now
        return result


def driver_name(device: Path) -> str | None:
    try:
        return os.path.basename(os.readlink(device / "driver"))
    except OSError:
        return None


def find_gpu(drm: Path = DRM, card: str | None = None) -> Path | None:
    """The device directory of the named card, or of the first amdgpu card with busy counters."""
    if card:
        device = drm / card / "device"
        return device if device.is_dir() else None
    cards = [path for path in drm.glob("card[0-9]*") if re.fullmatch(r"card[0-9]+", path.name)]
    for path in sorted(cards, key=lambda path: int(path.name[4:])):
        device = path / "device"
        if driver_name(device) == "amdgpu" and (device / "gpu_busy_percent").exists():
            return device
    return None


def read_gpu(device: Path) -> dict:
    result = {}
    for name in ("gpu_busy_percent", "mem_busy_percent", "vcn_busy_percent"):
        value = read_int(device / name)
        if value is not None:
            result[name.removesuffix("_percent")] = value
    for hwmon in sorted((device / "hwmon").glob("hwmon*")):
        for name in ("power1_average", "power1_input"):
            value = read_int(hwmon / name)
            if value is not None:
                result["power_w"] = round(value / 1_000_000, 2)
                return result
    return result


class LogFollower:
    """New lines of the host log, following a rotation or truncation to the new file."""

    MAX_READ = 8 * 1024 * 1024

    def __init__(self, path: Path):
        self.path = path
        self.handle = None
        self.inode = None
        self.partial = b""
        self.open(at_end=True)

    def open(self, at_end: bool) -> None:
        if self.handle:
            self.handle.close()
        self.handle = open(self.path, "rb")
        self.inode = os.fstat(self.handle.fileno()).st_ino
        self.partial = b""
        if at_end:
            self.handle.seek(0, os.SEEK_END)

    def read_lines(self) -> list[str]:
        try:
            info = os.stat(self.path)
        except OSError:
            return []
        if info.st_ino != self.inode or info.st_size < self.handle.tell():
            self.open(at_end=False)
        data = self.partial + self.handle.read(self.MAX_READ)
        lines = data.split(b"\n")
        self.partial = lines.pop()
        return [line.decode(errors="replace") for line in lines]

    def close(self) -> None:
        if self.handle:
            self.handle.close()


def loopback_https(url: str) -> bool:
    parsed = urllib.parse.urlsplit(url)
    if parsed.scheme != "https" or not parsed.hostname:
        return False
    if parsed.hostname == "localhost":
        return True
    try:
        return ipaddress.ip_address(parsed.hostname).is_loopback
    except ValueError:
        return False


def read_api_key(path: Path) -> str:
    """The key from a private file: Polaris's polaris_state.json, an api_key line, or the key alone."""
    if path.stat().st_mode & 0o077:
        raise SystemExit(f"{path.name} can be read by other users; run chmod 600 on it first")
    text = path.read_text()
    try:
        state = json.loads(text)
    except ValueError:
        state = None
    if isinstance(state, dict):
        # Polaris generates the key into its credentials file, and that copy wins over polaris.conf.
        if isinstance(state.get("api_key"), str) and state["api_key"]:
            return state["api_key"]
        raise SystemExit(f"{path.name} holds no api_key")
    for line in text.splitlines():
        if match := re.fullmatch(r"\s*api_key\s*=\s*(\S+)\s*", line):
            return match.group(1)
    tokens = text.split()
    if len(tokens) == 1:
        return tokens[0]
    raise SystemExit(f"{path.name} holds neither a bare key nor an api_key line")


def select_stats(payload: dict) -> dict:
    result = {}
    for name in STATS_NUMBERS:
        value = payload.get(name)
        if isinstance(value, (int, float)) and not isinstance(value, bool):
            result[name] = value
    for name in STATS_LABELS:
        value = payload.get(name)
        if isinstance(value, (str, int, float, bool)):
            result[name] = value
    decision = payload.get("display_mode_decision")
    if isinstance(decision, dict):
        for part in ("requested", "applied"):
            value = decision.get(part)
            if isinstance(value, str) and DISPLAY_MODE.fullmatch(value):
                result[f"display_mode_{part}"] = value
        if isinstance(decision.get("pinned_by_host"), bool):
            result["display_mode_pinned_by_host"] = decision["pinned_by_host"]
    return result


def parse_stats_event(line: str) -> dict | None:
    """The kept fields from one 'data:' line of Polaris's stats event stream."""
    if not line.startswith("data:"):
        return None
    try:
        payload = json.loads(line[5:])
    except ValueError:
        return None
    return select_stats(payload) if isinstance(payload, dict) else None


class RefuseRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


class StatsStream:
    """Polaris's stream stats, pushed once a second over one loopback request for the whole run.

    Polaris logs every request it serves at debug level, so one long request keeps polling out
    of the log that is being compared.
    """

    def __init__(self, url: str, key: str):
        if not loopback_https(url):
            raise SystemExit("--stats-url must be an https address on this machine")
        # The web UI certificate is self-signed, and the request never leaves this machine. No
        # proxy is used, so the key cannot be handed to one.
        context = ssl.create_default_context()
        context.check_hostname = False
        context.verify_mode = ssl.CERT_NONE
        self.opener = urllib.request.build_opener(
            urllib.request.ProxyHandler({}), urllib.request.HTTPSHandler(context=context), RefuseRedirect(),
        )
        self.request = urllib.request.Request(
            url, headers={"Authorization": "Bearer " + key, "Accept": "text/event-stream"},
        )
        self.records: list[dict] = []
        self.errors: Counter = Counter()
        self.lock = threading.Lock()
        self.stopping = threading.Event()
        self.thread = threading.Thread(target=self.follow, name="polaris-stats", daemon=True)

    def start(self) -> None:
        self.thread.start()

    def follow(self) -> None:
        while not self.stopping.is_set():
            try:
                with self.opener.open(self.request, timeout=5) as response:
                    for raw in response:
                        if self.stopping.is_set():
                            return
                        record = parse_stats_event(raw.decode(errors="replace").strip())
                        if record is not None:
                            with self.lock:
                                self.records.append({"at": time.monotonic(), **record})
            except urllib.error.HTTPError as error:
                # A refused key or a redirect to the login page will not fix itself, and retrying
                # would only fill the host log.
                self.note(f"http_{error.code}")
                return
            except (OSError, ValueError) as error:
                self.note(type(error).__name__)
            self.stopping.wait(1)

    def note(self, error: str) -> None:
        with self.lock:
            self.errors[error] += 1

    def latest(self) -> dict | None:
        with self.lock:
            return self.records[-1] if self.records else None

    def stop(self) -> None:
        self.stopping.set()
        self.thread.join(timeout=3)

    def collected(self, begin: float, end: float) -> tuple[list[dict], dict]:
        with self.lock:
            records = [
                {"t": round(record["at"] - begin, 3), **{k: v for k, v in record.items() if k != "at"}}
                for record in self.records if begin <= record["at"] <= end
            ]
            return records, dict(self.errors)


def read_settings(path: Path) -> dict[str, str]:
    """The listed frame-path settings that the host's config file sets explicitly."""
    settings = {}
    home = str(Path.home())
    for line in (read_text(path) or "").splitlines():
        key, separator, value = line.partition("=")
        key, value = key.strip(), value.strip()
        if separator and key in CONFIG_KEYS:
            settings[key] = value if CONFIG_VALUE.fullmatch(value) and home not in value else "(not recorded)"
    return settings


def package_versions(names: tuple[str, ...]) -> dict[str, str]:
    queries = (
        ["pacman", "-Q", *names],
        ["rpm", "-q", "--qf", "%{NAME} %{VERSION}-%{RELEASE}\n", *names],
        ["dpkg-query", "-W", "-f", "${Package} ${Version}\n", *names],
    )
    for argv in queries:
        output = run(argv)
        if output is None:
            continue
        versions = {}
        for line in output.splitlines():
            parts = line.split()
            if len(parts) == 2 and parts[0] in names:
                versions[parts[0]] = parts[1]
        return versions
    return {}


def module_state(names: tuple[str, ...], sys_module: Path = SYS_MODULE, proc: Path = PROC) -> dict:
    loaded = {line.split()[0] for line in (read_text(proc / "modules") or "").splitlines() if line.strip()}
    result = {}
    for name in names:
        entry: dict = {"loaded": name in loaded}
        if entry["loaded"]:
            for field in ("version", "srcversion"):
                if value := read_text(sys_module / name / field):
                    entry[field] = value
            if name in MODULE_PARAMETERS:
                parameters = {}
                for parameter in sorted((sys_module / name / "parameters").glob("*")):
                    value = read_text(parameter)
                    if value is not None:
                        parameters[parameter.name] = value
                entry["parameters"] = parameters
        result[name] = entry
    return result


def pci_name(vendor: str, device: str, paths: tuple[Path, ...] = PCI_IDS) -> str | None:
    """The marketing name from the system's pci.ids, without running lspci."""
    for path in paths:
        try:
            handle = path.open(errors="replace")
        except OSError:
            continue
        with handle:
            in_vendor = False
            for line in handle:
                # Comments also sit inside vendor blocks, so they must not end one.
                if line.startswith("#") or not line.strip():
                    continue
                if not line.startswith("\t"):
                    in_vendor = line.startswith(vendor + " ")
                elif in_vendor and line.startswith("\t" + device + " "):
                    return line.strip()[len(device):].strip()
    return None


def drm_inventory(drm: Path = DRM) -> dict:
    cards, connectors = [], []
    try:
        entries = sorted(drm.iterdir(), key=lambda path: path.name)
    except OSError:
        entries = []
    for path in entries:
        if re.fullmatch(r"card[0-9]+", path.name):
            device = path / "device"
            entry: dict = {"card": path.name, "driver": driver_name(device)}
            vendor, model = read_text(device / "vendor"), read_text(device / "device")
            if vendor and model:
                vendor, model = vendor.removeprefix("0x"), model.removeprefix("0x")
                entry["pci_id"] = f"{vendor}:{model}"
                if name := pci_name(vendor, model):
                    entry["name"] = name
            cards.append(entry)
        elif re.fullmatch(r"card[0-9]+-.+", path.name):
            modes = read_text(path / "modes")
            connectors.append({
                "connector": path.name, "status": read_text(path / "status"),
                "enabled": read_text(path / "enabled"), "first_mode": modes.splitlines()[0] if modes else None,
            })
    return {"cards": cards, "connectors": connectors}


def os_name() -> str | None:
    for line in (read_text(Path("/etc/os-release")) or "").splitlines():
        if line.startswith("PRETTY_NAME="):
            return line.split("=", 1)[1].strip('"')
    return None


def cpu_model() -> str | None:
    for line in (read_text(PROC / "cpuinfo") or "").splitlines():
        if line.startswith("model name"):
            return line.split(":", 1)[1].strip()
    return None


def environment(extra_packages: list[str]) -> dict:
    # No host name, user name, address, monitor serial or path is recorded.
    return {
        "kernel": platform.release(),
        "os": os_name(),
        "cpu": cpu_model(),
        "desktop": os.environ.get("XDG_CURRENT_DESKTOP", ""),
        "session_type": os.environ.get("XDG_SESSION_TYPE", ""),
        "drm": drm_inventory(),
        "modules": module_state(MODULES),
        "packages": package_versions(tuple(dict.fromkeys(PACKAGES + tuple(extra_packages)))),
    }


def summarize(samples: list[dict], events: list[dict], stats_records: list[dict], stats_errors: dict,
              fps: float, seconds: float, log_lines: int) -> dict:
    summary: dict = {
        "samples": len(samples),
        "log_lines_per_second": round(log_lines / seconds, 2) if seconds else None,
        "processes": {
            name: max((sample["cpu"].get(name + "_processes", 0) for sample in samples if "cpu" in sample), default=0)
            for name in GROUPS
        },
    }

    cpu = {}
    for key in [name + "_pct" for name in GROUPS] + ["system_pct", "system_cores"]:
        values = [sample["cpu"][key] for sample in samples if sample.get("cpu", {}).get(key) is not None]
        if values:
            cpu[key] = describe(values)
    summary["cpu"] = cpu

    gpu: dict = {}
    for key in ("gpu_busy", "mem_busy", "vcn_busy", "power_w"):
        values = [sample["gpu"][key] for sample in samples if key in sample.get("gpu", {})]
        if values:
            gpu[key] = describe(values)
    if "vcn_busy" in gpu and fps:
        gpu["vcn_ms_per_target_frame"] = round(gpu["vcn_busy"]["mean"] / 100 * 1000 / fps, 3)
    summary["gpu"] = gpu

    network: dict = {}
    for name in sorted({name for sample in samples for name in sample.get("net", {})}):
        values = [sample["net"].get(name, 0.0) for sample in samples if "net" in sample]
        network[name] = describe(values)
    if network:
        summary["network_tx_mbps"] = network
        summary["busiest_interface"] = max(network, key=lambda name: network[name]["mean"])

    periodic = {}
    for name in PERIODIC_NAMES.values():
        windows = [event for event in events if event["kind"] == "periodic" and event["name"] == name]
        if windows:
            periodic[name] = {
                "windows": len(windows), "avg_ms": mean([window["avg_ms"] for window in windows]),
                "min_ms": min(window["min_ms"] for window in windows),
                "max_ms": max(window["max_ms"] for window in windows),
            }
    summary["periodic"] = periodic

    polaris_capture = {}
    for transport in sorted({event["transport"] for event in events if event["kind"] == "polaris_capture"}):
        lines = [event for event in events if event["kind"] == "polaris_capture" and event["transport"] == transport]
        entry = {"lines": len(lines), "frames": sum(line.get("frames", 0) for line in lines)}
        for field in POLARIS_CAPTURE_P50:
            # -1 means the window had no samples for that figure.
            values = [line[field] for line in lines if line.get(field, -1) >= 0]
            if values:
                entry[field] = statistics.median(values)
        for field in POLARIS_CAPTURE_P99:
            values = [line[field] for line in lines if line.get(field, -1) >= 0]
            if values:
                entry[field] = max(values)
        polaris_capture[transport] = entry
    summary["polaris_capture"] = polaris_capture

    hermes_capture = {}
    for label in sorted({event["label"] for event in events if event["kind"] == "hermes_capture"}):
        lines = [event for event in events if event["kind"] == "hermes_capture" and event["label"] == label]
        frames = sum(line["frames"] for line in lines)
        hermes_capture[label] = {
            "lines": len(lines), "frames": frames,
            "avg_us": round(sum(line["avg_us"] * line["frames"] for line in lines) / frames, 3) if frames else None,
            "peak_us": max(line["peak_us"] for line in lines),
        }
    summary["hermes_capture"] = hermes_capture

    if stats_records or stats_errors:
        streaming = [record for record in stats_records if record.get("streaming") is True]
        stats: dict = {"records": len(stats_records), "streaming_records": len(streaming)}
        if stats_errors:
            stats["errors"] = stats_errors
        for name in STATS_NUMBERS:
            values = [record[name] for record in streaming if name in record]
            if values:
                stats[name] = describe(values)
        for name in STATS_LABELS + DISPLAY_MODE_LABELS:
            values = [record[name] for record in streaming if name in record and name != "streaming"]
            if values:
                stats[name] = Counter(values).most_common(1)[0][0]
        summary["polaris_stats"] = stats
    return summary


class StopFlag:
    def __init__(self):
        self.requested = False
        signal.signal(signal.SIGINT, self.request)
        signal.signal(signal.SIGTERM, self.request)

    def request(self, *_):
        self.requested = True


def progress(elapsed: float, sample: dict, events: list[dict], stats: dict | None) -> str:
    cpu, gpu = sample.get("cpu", {}), sample.get("gpu", {})
    parts = [f"[{elapsed:5.0f}s]"]
    for key, name in (("host_pct", "host"), ("compositor_pct", "kwin"), ("workload_pct", "page")):
        if cpu.get(key) is not None:
            parts.append(f"{name} {cpu[key]:.0f}%")
    for key, name in (("gpu_busy", "gpu"), ("vcn_busy", "vcn")):
        if key in gpu:
            parts.append(f"{name} {gpu[key]}%")
    if net := sample.get("net"):
        parts.append(f"tx {max(net.values()):.1f} Mb/s")
    if stats and "fps" in stats:
        parts.append(f"fps {stats['fps']:.1f}")
    if stats and "encode_time_ms" in stats:
        parts.append(f"encode {stats['encode_time_ms']:.2f} ms")
    latency = [event for event in events if event.get("name") == "frame_processing"]
    if latency:
        parts.append(f"processing avg {latency[-1]['avg_ms']:.2f} ms")
    return " ".join(parts)


def measure(args: argparse.Namespace, log_path: Path, config_path: Path) -> dict:
    sampler = ProcessSampler(args.host_process or args.host, args.compositor, args.workload_match)
    network = NetworkSampler()
    gpu = find_gpu(card=args.card)
    stats = StatsStream(args.stats_url, read_api_key(args.api_key_file)) if args.api_key_file else None
    log = LogFollower(log_path)
    stop = StopFlag()
    started = datetime.now(timezone.utc)

    groups = sampler.groups(sampler.table())
    for name, text in (("host", f"host process {sampler.host!r}"), ("workload", f"page matching {args.workload_match!r}")):
        if not groups[name]:
            print(f"warning: no {text} is running", file=sys.stderr)
    if gpu is None:
        print("warning: no amdgpu card with busy counters; GPU figures are left out", file=sys.stderr)
    if stats is not None:
        stats.start()

    # Lines and CPU time from the warm-up are dropped, so the run starts on a settled stream.
    warmup_end = time.monotonic() + args.warmup
    while not stop.requested and time.monotonic() < warmup_end:
        log.read_lines()
        time.sleep(min(0.25, max(0.0, warmup_end - time.monotonic())))
    log.read_lines()
    sampler.sample()
    network.sample()

    samples, events, log_lines = [], [], 0
    begin = time.monotonic()
    next_sample, next_report = begin + args.interval, begin + 10
    while not stop.requested and next_sample <= begin + args.duration + 1e-6:
        time.sleep(max(0.0, next_sample - time.monotonic()))
        now = time.monotonic()
        elapsed = round(now - begin, 3)
        sample: dict = {"t": elapsed}
        if cpu := sampler.sample():
            sample["cpu"] = cpu
        if net := network.sample():
            sample["net"] = net
        if gpu is not None:
            sample["gpu"] = read_gpu(gpu)
        lines = log.read_lines()
        log_lines += len(lines)
        for line in lines:
            if event := parse_log_line(line):
                events.append({"t": elapsed, **event})
        samples.append(sample)
        if now >= next_report:
            print(progress(elapsed, sample, events, stats.latest() if stats else None), file=sys.stderr)
            next_report += 10
        next_sample += args.interval
    end = time.monotonic()
    log.close()

    stats_records, stats_errors = [], {}
    if stats is not None:
        stats.stop()
        stats_records, stats_errors = stats.collected(begin, end)
    seconds = round(end - begin, 3)
    return {
        "schema": SCHEMA,
        "host": args.host,
        "scenario": args.scenario,
        "run": args.run,
        "target": {
            "fps": args.fps, "resolution": args.resolution, "bitrate_mbps": args.bitrate_mbps,
            "codec": args.codec, "dynamic_range": args.dynamic_range,
        },
        "started_utc": started.isoformat(timespec="seconds"),
        "ended_utc": datetime.now(timezone.utc).isoformat(timespec="seconds"),
        "warmup_s": args.warmup,
        "measured_s": seconds,
        "interval_s": args.interval,
        "complete": not stop.requested and seconds >= args.duration - args.interval,
        "log_file": log_path.name,
        "host_settings": read_settings(config_path),
        "environment": environment(args.package),
        "summary": summarize(samples, events, stats_records, stats_errors, args.fps, seconds, log_lines),
        "samples": samples,
        "events": events,
        "polaris_stats": stats_records,
    }


def label(text: str) -> str:
    if not LABEL.fullmatch(text):
        raise argparse.ArgumentTypeError("use letters, digits, dot, dash or underscore")
    return text


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", type=label, required=True, help="host label, such as polaris or hermes")
    parser.add_argument("--scenario", type=label, required=True, help="settings label, such as 1440p120")
    parser.add_argument("--run", type=int, required=True, help="run number within the scenario")
    parser.add_argument("--fps", type=float, required=True, help="frame rate the client asked for")
    parser.add_argument("--resolution", default="", help="resolution the client asked for, such as 2560x1440")
    parser.add_argument("--bitrate-mbps", type=float, help="bitrate set in the client")
    parser.add_argument("--codec", default="hevc", help="codec the client asked for")
    parser.add_argument("--dynamic-range", choices=("sdr", "hdr"), default="sdr")
    parser.add_argument("--out", type=Path, required=True, help="directory for the receipt")
    parser.add_argument("--log", type=Path, help="host log to follow (defaults by host label)")
    parser.add_argument("--config", type=Path, help="host config to record settings from (defaults by host label)")
    parser.add_argument("--host-process", help="process name of the host (defaults to the host label)")
    parser.add_argument("--compositor", action="append", default=None, help="compositor process name (kwin_wayland)")
    parser.add_argument("--workload-match", default=DEFAULT_WORKLOAD, help="text in the test browser's command line")
    parser.add_argument("--card", help="DRM card to read GPU counters from (first amdgpu card)")
    parser.add_argument("--api-key-file", type=Path, help="Polaris's polaris_state.json, or another private file with the key")
    parser.add_argument("--stats-url", default=DEFAULT_STATS_URL)
    parser.add_argument("--duration", type=float, default=120.0)
    parser.add_argument("--warmup", type=float, default=10.0)
    parser.add_argument("--interval", type=float, default=0.5)
    parser.add_argument("--package", action="append", default=[], help="another package whose version to record")
    parser.add_argument("--force", action="store_true", help="replace an existing receipt")
    args = parser.parse_args(argv)
    args.compositor = args.compositor or ["kwin_wayland"]
    if args.interval < 0.1 or args.duration < args.interval or args.warmup < 0:
        parser.error("need --interval >= 0.1, --duration >= --interval and --warmup >= 0")

    log_path = (args.log or Path(DEFAULT_LOGS.get(args.host, ""))).expanduser()
    if not log_path.is_file():
        parser.error(f"host log {log_path.name or '(none)'} not found; pass --log")
    config_path = (args.config or Path(DEFAULT_CONFIGS.get(args.host, ""))).expanduser()
    target = args.out / f"{args.scenario}-{args.host}-run{args.run}.json"
    if target.exists() and not args.force:
        parser.error(f"{target.name} already exists; pass --force to replace it")

    receipt = measure(args, log_path, config_path)
    args.out.mkdir(parents=True, exist_ok=True)
    partial = target.with_suffix(".json.partial")
    partial.write_text(json.dumps(receipt, indent=1) + "\n")
    partial.replace(target)
    print(f"wrote {target}" + ("" if receipt["complete"] else " (stopped early, marked incomplete)"))
    return 0 if receipt["complete"] else 1


if __name__ == "__main__":
    sys.exit(main())
