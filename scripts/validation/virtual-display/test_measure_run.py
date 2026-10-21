import contextlib
import io
import json
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

import measure_run as measure


def stat_line(pid, comm, ppid, utime, stime=0, start=0):
    # Fields after the name, from state onwards: ppid is the second, utime and stime the 12th
    # and 13th, and the start time the 20th.
    fields = ["S", ppid] + [0] * 9 + [utime, stime] + [0] * 6 + [start] + [0] * 5
    return f"{pid} ({comm}) " + " ".join(str(field) for field in fields)


class FakeProc:
    def __init__(self, root):
        self.root = Path(root)

    def process(self, pid, comm, ppid, ticks, start=0, cmdline=""):
        directory = self.root / str(pid)
        directory.mkdir(exist_ok=True)
        (directory / "stat").write_text(stat_line(pid, comm, ppid, ticks, 0, start))
        (directory / "cmdline").write_bytes(cmdline.replace(" ", "\0").encode())

    def system(self, uptime, busy, idle):
        (self.root / "uptime").write_text(f"{uptime:.2f} 0.00\n")
        (self.root / "stat").write_text(f"cpu  {busy} 0 0 {idle} 0 0 0 0 0 0\ncpu0 0 0 0 0 0 0 0 0 0 0\n")


class LogLineTests(unittest.TestCase):
    def test_periodic_lines_from_either_host(self):
        names = {
            "Frame processing latency": "frame_processing",
            "Network: each send_batch() latency": "send_batch",
            "Network: each FEC block latency": "fec_block",
            "Network: frame's overall network latency": "frame_network",
        }
        for text, name in names.items():
            with self.subTest(name=name):
                event = measure.parse_log_line(f"[2026-10-13 10:00:00.000]: Debug: {text} (min/max/avg): 1.10ms/4.20ms/2.35ms")
                self.assertEqual(event, {"kind": "periodic", "name": name, "min_ms": 1.1, "max_ms": 4.2, "avg_ms": 2.35})

    def test_polaris_capture_line_keeps_missing_figures_as_minus_one(self):
        event = measure.parse_log_line(
            "Info: capture_telemetry: transport=dmabuf frames=600 dispatch_us_p50=210 dispatch_us_p99=900 "
            "total_us_p50=260 total_us_p99=1010 source_interval_samples=0 source_interval_us_p50=-1"
        )
        self.assertEqual(event["kind"], "polaris_capture")
        self.assertEqual(event["transport"], "dmabuf")
        self.assertEqual(event["frames"], 600)
        self.assertEqual(event["total_us_p99"], 1010)
        self.assertEqual(event["source_interval_us_p50"], -1)

    def test_hermes_capture_line_with_exponent_and_dotted_label(self):
        event = measure.parse_log_line("Info: [capture-metric/hermes-kms.0] avg=41.5us peak=1.2e+03us over 120 frames")
        self.assertEqual(event, {"kind": "hermes_capture", "label": "hermes-kms.0", "avg_us": 41.5, "peak_us": 1200.0, "frames": 120})

    def test_other_lines_are_ignored(self):
        for line in ("", "Info: Session started", "Frame processing latency (min/max/avg): n/a"):
            with self.subTest(line=line):
                self.assertIsNone(measure.parse_log_line(line))


class ProcParsingTests(unittest.TestCase):
    def test_command_names_with_spaces_and_brackets(self):
        for comm in ("Web Content", "a) (b", "xdg-desktop-por"):
            with self.subTest(comm=comm):
                self.assertEqual(measure.parse_proc_stat(stat_line(7, comm, 3, 40, 2, 900)), (comm, 3, 42, 900))

    def test_truncated_stat_is_rejected(self):
        for text in ("", "7 (sh) S 1 2 3", "no brackets here"):
            with self.subTest(text=text):
                self.assertIsNone(measure.parse_proc_stat(text))

    def test_cpu_totals_count_idle_and_iowait_as_not_busy(self):
        busy, total = measure.parse_cpu_totals("cpu  10 1 5 70 4 2 3 5 9 9\ncpu0 1 1 1 1 1 1 1 1 1 1\n")
        self.assertEqual((busy, total), (26, 100))
        with self.assertRaises(ValueError):
            measure.parse_cpu_totals("cpu0 1 2 3")

    def test_net_dev_reads_sent_bytes_and_skips_loopback(self):
        text = (
            "Inter-|   Receive                                                |  Transmit\n"
            " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n"
            "    lo: 900 9 0 0 0 0 0 0 900 9 0 0 0 0 0 0\n"
            "enp10s0: 1000 10 0 0 0 0 0 0 5000000 4000 0 0 0 0 0 0\n"
        )
        self.assertEqual(measure.parse_net_dev(text), {"enp10s0": 5000000})

    def test_percentile_is_a_measured_value(self):
        self.assertEqual(measure.percentile(list(range(1, 21)), 0.95), 19)
        self.assertEqual(measure.percentile([3, 1, 2], 0.95), 3)
        self.assertIsNone(measure.percentile([], 0.95))


class ProcessSamplerTests(unittest.TestCase):
    def test_groups_and_cpu_shares(self):
        with tempfile.TemporaryDirectory() as directory:
            proc = FakeProc(directory)
            proc.system(100.0, busy=1000, idle=9000)
            proc.process(100, "polaris", 1, 50, start=10)
            proc.process(101, "sh", 100, 10, start=20)
            proc.process(102, "firefox", 100, 30, start=30, cmdline="firefox --profile /run/user/1000/vdpattern-pattern")
            proc.process(103, "Isolated Web Co", 102, 70, start=40)
            proc.process(200, "kwin_wayland", 1, 500, start=5)
            proc.process(201, "Xwayland", 200, 9, start=6)
            proc.process(202, "pipewire", 1, 9, start=6)
            proc.process(203, "wireplumber", 1, 9, start=6)
            proc.process(204, "xdg-desktop-por", 1, 9, start=6)
            proc.process(205, "bash", 1, 9, start=6, cmdline="bash -c echo vdpattern-pattern")
            # The reference clock on the host monitor is not the streamed page.
            proc.process(206, "firefox", 1, 20, start=7, cmdline="firefox --profile /run/user/1000/vdpattern-reference")
            sampler = measure.ProcessSampler("polaris", ["kwin_wayland"], measure.DEFAULT_WORKLOAD, Path(directory))
            sampler.excluded.add(205)
            with patch.object(measure.time, "monotonic", return_value=50.0):
                self.assertIsNone(sampler.sample())

            ticks = measure.CLOCK_TICKS
            proc.system(100.5, busy=1050, idle=9150)
            proc.process(100, "polaris", 1, 75, start=10)
            proc.process(101, "sh", 100, 15, start=20)
            proc.process(102, "firefox", 100, 40, start=30, cmdline="firefox --profile /run/user/1000/vdpattern-pattern")
            proc.process(103, "Isolated Web Co", 102, 110, start=40)
            # Started after the first sample, so all of its time falls in this interval.
            proc.process(104, "Web Content", 102, 4, start=int(100.2 * ticks))
            proc.process(200, "kwin_wayland", 1, 507, start=5)
            proc.process(201, "Xwayland", 200, 10, start=6)
            proc.process(202, "pipewire", 1, 11, start=6)
            proc.process(203, "wireplumber", 1, 10, start=6)
            proc.process(204, "xdg-desktop-por", 1, 12, start=6)
            proc.process(205, "bash", 1, 99, start=6, cmdline="bash -c echo vdpattern-pattern")
            proc.process(206, "firefox", 1, 60, start=7, cmdline="firefox --profile /run/user/1000/vdpattern-reference")
            with patch.object(measure.time, "monotonic", return_value=50.5):
                cpu = sampler.sample()

        def share(used):
            return round(100.0 * used / ticks / 0.5, 2)

        self.assertEqual(cpu["host_pct"], share(25 + 5))
        self.assertEqual(cpu["host_processes"], 2)
        self.assertEqual(cpu["workload_pct"], share(10 + 40 + 4))
        self.assertEqual(cpu["workload_processes"], 3)
        self.assertEqual(cpu["compositor_pct"], share(7))
        self.assertEqual(cpu["xwayland_pct"], share(1))
        self.assertEqual(cpu["pipewire_pct"], share(2 + 1))
        self.assertEqual(cpu["portal_pct"], share(3))
        self.assertEqual(cpu["system_pct"], 25.0)
        self.assertEqual(cpu["system_cores"], round(50 / ticks / 0.5, 3))

    def test_network_rate_in_megabits(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "dev"
            header = "Inter-|\n face |\n"
            path.write_text(header + "enp10s0: 0 0 0 0 0 0 0 0 1000000 0 0 0 0 0 0 0\n")
            sampler = measure.NetworkSampler(path)
            with patch.object(measure.time, "monotonic", return_value=10.0):
                self.assertIsNone(sampler.sample())
            path.write_text(header + "enp10s0: 0 0 0 0 0 0 0 0 7250000 0 0 0 0 0 0 0\nwlan0: 0 0 0 0 0 0 0 0 5 0 0 0 0 0 0 0\n")
            with patch.object(measure.time, "monotonic", return_value=10.5):
                self.assertEqual(sampler.sample(), {"enp10s0": 100.0})


class LogFollowerTests(unittest.TestCase):
    def test_follows_appends_rotation_and_truncation(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "host.log"
            path.write_text("before the run\n")
            follower = measure.LogFollower(path)
            try:
                self.assertEqual(follower.read_lines(), [])
                with path.open("a") as handle:
                    handle.write("one\ntw")
                self.assertEqual(follower.read_lines(), ["one"])
                with path.open("a") as handle:
                    handle.write("o\n")
                self.assertEqual(follower.read_lines(), ["two"])
                path.rename(Path(directory) / "host.log.1")
                path.write_text("fresh\n")
                self.assertEqual(follower.read_lines(), ["fresh"])
                path.write_text("x\n")
                self.assertEqual(follower.read_lines(), ["x"])
                path.unlink()
                self.assertEqual(follower.read_lines(), [])
            finally:
                follower.close()


class StatsTests(unittest.TestCase):
    def test_only_listed_fields_are_kept(self):
        payload = {
            "streaming": True, "client_name": "Living room", "client_ip": "192.0.2.4", "fps": 119.8,
            "encode_time_ms": 2.5, "codec": "hevc", "stream_path_id": "host_virtual_display",
            "capture_cpu_copy": False, "width": True, "display_mode_decision": {"requested": "x"},
        }
        kept = measure.select_stats(payload)
        self.assertEqual(kept, {
            "fps": 119.8, "encode_time_ms": 2.5, "streaming": True, "codec": "hevc",
            "stream_path_id": "host_virtual_display", "capture_cpu_copy": False,
        })

    def test_display_mode_decision_keeps_mode_strings_only(self):
        decision = {"requested": "2560x1440x120", "applied": "1920x1080x59.94", "pinned_by_host": True}
        self.assertEqual(measure.select_stats({"display_mode_decision": decision}), {
            "display_mode_requested": "2560x1440x120", "display_mode_applied": "1920x1080x59.94",
            "display_mode_pinned_by_host": True,
        })
        for decision in ({"requested": "Living room 1080p"}, {"applied": "1920x1080x60 "}, {"pinned_by_host": "yes"}, "1920x1080x60"):
            with self.subTest(decision=decision):
                self.assertEqual(measure.select_stats({"display_mode_decision": decision}), {})

    def test_event_lines(self):
        self.assertEqual(measure.parse_stats_event('data: {"fps": 60, "client_ip": "192.0.2.4"}'), {"fps": 60})
        for line in ("", ": keepalive", "event: stats", "data: {broken", "data: [1, 2]"):
            with self.subTest(line=line):
                self.assertIsNone(measure.parse_stats_event(line))

    def test_only_https_on_this_machine(self):
        for url, allowed in [
            ("https://localhost:47990/api/stats/stream-sse", True),
            ("https://127.0.0.1:47990/", True),
            ("https://[::1]:47990/", True),
            ("http://localhost:47990/", False),
            ("https://localhost.example.com/", False),
            ("https://192.0.2.4:47990/", False),
            ("localhost:47990", False),
        ]:
            with self.subTest(url=url):
                self.assertEqual(measure.loopback_https(url), allowed)


class ApiKeyTests(unittest.TestCase):
    def key_file(self, directory, text, mode=0o600):
        path = Path(directory) / "key"
        path.write_text(text)
        path.chmod(mode)
        return path

    def test_key_forms(self):
        cases = [
            (json.dumps({"username": "u", "api_key": "fromstate"}), "fromstate"),
            ("min_log_level = debug\napi_key = fromconf\n", "fromconf"),
            ("barekey\n", "barekey"),
        ]
        for text, key in cases:
            with self.subTest(key=key), tempfile.TemporaryDirectory() as directory:
                self.assertEqual(measure.read_api_key(self.key_file(directory, text)), key)

    def test_unusable_files_are_refused(self):
        cases = [
            ("barekey\n", 0o644),
            (json.dumps({"username": "u"}), 0o600),
            ("two words\n", 0o600),
        ]
        for text, mode in cases:
            with self.subTest(text=text, mode=oct(mode)), tempfile.TemporaryDirectory() as directory:
                with self.assertRaises(SystemExit):
                    measure.read_api_key(self.key_file(directory, text, mode))


class SettingsTests(unittest.TestCase):
    def test_only_listed_plain_values_are_recorded(self):
        home = str(Path.home())
        text = "\n".join([
            "encoder = vaapi",
            "vaapi_rc = vbr",
            "linux_stream_mode = host_virtual_display",
            "api_key = secret",
            "sunshine_name = desk",
            f"adapter_name = {home}/card",
            "output_name = DP-4 $(odd)",
            "# min_log_level = verbose",
            "min_log_level=debug",
            "not a setting",
        ])
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "host.conf"
            path.write_text(text)
            self.assertEqual(measure.read_settings(path), {
                "encoder": "vaapi", "vaapi_rc": "vbr", "linux_stream_mode": "host_virtual_display",
                "adapter_name": "(not recorded)", "output_name": "(not recorded)", "min_log_level": "debug",
            })
        self.assertEqual(measure.read_settings(Path("/nonexistent/host.conf")), {})


class InventoryTests(unittest.TestCase):
    def test_pci_name_reads_past_comments_inside_a_vendor(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "pci.ids"
            path.write_text(
                "# header\n1002  Advanced Micro Devices, Inc. [AMD/ATI]\n\t1234  Older part\n"
                "# a comment inside the vendor block\n\n\t744c  Navi 31 [Radeon RX 7900 XT/7900 XTX]\n"
                "\t\t1002 0e3b  Subsystem\n10de  NVIDIA Corporation\n\t744c  Not this one\n"
            )
            self.assertEqual(measure.pci_name("1002", "744c", (path,)), "Navi 31 [Radeon RX 7900 XT/7900 XTX]")
            self.assertIsNone(measure.pci_name("1002", "ffff", (path,)))
            self.assertIsNone(measure.pci_name("1002", "744c", (Path(directory) / "missing",)))

    def test_drm_cards_connectors_and_gpu_counters(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            drivers = root / "drivers"
            for name in ("evdi", "amdgpu"):
                (drivers / name).mkdir(parents=True)
            drm = root / "drm"
            for card, driver, vendor, model in (("card0", "evdi", None, None), ("card1", "amdgpu", "0x1002", "0x744c")):
                device = drm / card / "device"
                device.mkdir(parents=True)
                (device / "driver").symlink_to(drivers / driver)
                if vendor:
                    (device / "vendor").write_text(vendor + "\n")
                    (device / "device").write_text(model + "\n")
            amd = drm / "card1" / "device"
            (amd / "gpu_busy_percent").write_text("37\n")
            (amd / "vcn_busy_percent").write_text("12\n")
            (amd / "hwmon" / "hwmon4").mkdir(parents=True)
            (amd / "hwmon" / "hwmon4" / "power1_average").write_text("82000000\n")
            connector = drm / "card1-DP-4"
            connector.mkdir()
            (connector / "status").write_text("connected\n")
            (connector / "enabled").write_text("enabled\n")
            (connector / "modes").write_text("7680x2160\n3840x2160\n")

            with patch.object(measure, "pci_name", return_value="Navi 31"):
                inventory = measure.drm_inventory(drm)
            self.assertEqual(inventory["cards"], [
                {"card": "card0", "driver": "evdi"},
                {"card": "card1", "driver": "amdgpu", "pci_id": "1002:744c", "name": "Navi 31"},
            ])
            self.assertEqual(inventory["connectors"], [
                {"connector": "card1-DP-4", "status": "connected", "enabled": "enabled", "first_mode": "7680x2160"},
            ])
            self.assertEqual(measure.find_gpu(drm), amd)
            self.assertIsNone(measure.find_gpu(drm, card="card7"))
            self.assertEqual(measure.read_gpu(amd), {"gpu_busy": 37, "vcn_busy": 12, "power_w": 82.0})

    def test_module_state_reads_parameters_of_listed_modules_only(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "proc").mkdir()
            (root / "proc" / "modules").write_text("hermes_kms 40960 0 - Live 0x0\namdgpu 1 0 - Live 0x0\n")
            for module, parameter in (("hermes_kms", "initial_enabled"), ("amdgpu", "dc")):
                (root / "module" / module / "parameters").mkdir(parents=True)
                (root / "module" / module / "parameters" / parameter).write_text("0\n")
            (root / "module" / "hermes_kms" / "version").write_text("0.4.0\n")
            state = measure.module_state(("hermes_kms", "amdgpu", "evdi"), root / "module", root / "proc")
        self.assertEqual(state, {
            "hermes_kms": {"loaded": True, "version": "0.4.0", "parameters": {"initial_enabled": "0"}},
            "amdgpu": {"loaded": True},
            "evdi": {"loaded": False},
        })


class SummaryTests(unittest.TestCase):
    def test_summary_of_samples_events_and_stats(self):
        samples = [
            {"t": 0.5, "cpu": {"host_pct": 10.0, "host_processes": 1, "workload_processes": 3, "system_pct": 5.0},
             "gpu": {"gpu_busy": 40, "vcn_busy": 30, "power_w": 90.0}, "net": {"enp10s0": 50.0, "wlan0": 0.1}},
            {"t": 1.0, "cpu": {"host_pct": 20.0, "host_processes": 2, "workload_processes": 3, "system_pct": 7.0},
             "gpu": {"gpu_busy": 60, "vcn_busy": 50, "power_w": 110.0}, "net": {"enp10s0": 70.0}},
        ]
        events = [
            {"kind": "periodic", "name": "frame_processing", "min_ms": 1.0, "max_ms": 6.0, "avg_ms": 3.0},
            {"kind": "periodic", "name": "frame_processing", "min_ms": 0.5, "max_ms": 9.0, "avg_ms": 4.0},
            {"kind": "polaris_capture", "transport": "dmabuf", "frames": 600, "total_us_p50": 200, "total_us_p99": 900, "source_interval_us_p50": -1},
            {"kind": "polaris_capture", "transport": "dmabuf", "frames": 600, "total_us_p50": 300, "total_us_p99": 1500, "source_interval_us_p50": -1},
            {"kind": "hermes_capture", "label": "hermes", "avg_us": 40.0, "peak_us": 900.0, "frames": 120},
            {"kind": "hermes_capture", "label": "hermes", "avg_us": 100.0, "peak_us": 1500.0, "frames": 60},
        ]
        stats = [
            {"t": 0.1, "streaming": False, "fps": 0},
            {"t": 1.1, "streaming": True, "fps": 119.0, "encode_time_ms": 2.0, "codec": "hevc",
             "display_mode_applied": "2560x1440x120"},
            {"t": 2.1, "streaming": True, "fps": 121.0, "encode_time_ms": 3.0, "codec": "hevc",
             "display_mode_applied": "2560x1440x120"},
        ]
        summary = measure.summarize(samples, events, stats, {"URLError": 1}, 120.0, 2.0, 50)

        self.assertEqual(summary["samples"], 2)
        self.assertEqual(summary["log_lines_per_second"], 25.0)
        self.assertEqual(summary["processes"]["host"], 2)
        self.assertEqual(summary["processes"]["workload"], 3)
        self.assertEqual(summary["processes"]["compositor"], 0)
        self.assertEqual(summary["cpu"]["host_pct"], {"mean": 15.0, "p95": 20.0, "max": 20.0})
        self.assertEqual(summary["gpu"]["vcn_busy"]["mean"], 40.0)
        self.assertEqual(summary["gpu"]["vcn_ms_per_target_frame"], round(0.4 * 1000 / 120, 3))
        self.assertEqual(summary["network_tx_mbps"]["wlan0"]["mean"], 0.05)
        self.assertEqual(summary["busiest_interface"], "enp10s0")
        self.assertEqual(summary["periodic"]["frame_processing"], {"windows": 2, "avg_ms": 3.5, "min_ms": 0.5, "max_ms": 9.0})
        self.assertEqual(summary["polaris_capture"]["dmabuf"], {"lines": 2, "frames": 1200, "total_us_p50": 250, "total_us_p99": 1500})
        self.assertEqual(summary["hermes_capture"]["hermes"], {"lines": 2, "frames": 180, "avg_us": 60.0, "peak_us": 1500.0})
        polaris = summary["polaris_stats"]
        self.assertEqual((polaris["records"], polaris["streaming_records"]), (3, 2))
        self.assertEqual(polaris["fps"]["mean"], 120.0)
        self.assertEqual(polaris["codec"], "hevc")
        self.assertEqual(polaris["display_mode_applied"], "2560x1440x120")
        self.assertEqual(polaris["errors"], {"URLError": 1})
        self.assertNotIn("streaming", polaris)

    def test_no_stats_section_without_a_stats_stream(self):
        self.assertNotIn("polaris_stats", measure.summarize([], [], [], {}, 60.0, 0.0, 0))


class CommandLineTests(unittest.TestCase):
    def main(self, *argv):
        with contextlib.redirect_stderr(io.StringIO()), self.assertRaises(SystemExit) as raised:
            measure.main(list(argv))
        return raised.exception.code

    def test_bad_arguments_stop_before_sampling(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "host.log"
            log.write_text("")
            (Path(directory) / "s-polaris-run1.json").write_text("{}")
            common = ["--scenario", "s", "--run", "1", "--fps", "60", "--out", directory, "--log", str(log)]
            with patch.object(measure, "measure") as sampled:
                self.assertEqual(self.main("--host", "polaris", *common), 2)
                self.assertEqual(self.main("--host", "../polaris", *common[:-2], "--log", str(log)), 2)
                self.assertEqual(self.main("--host", "hermes", *common[:-2], "--log", str(log) + ".missing"), 2)
                self.assertEqual(self.main("--host", "hermes", *common, "--interval", "0.01"), 2)
                self.assertEqual(self.main("--host", "hermes", "--run", "1", "--fps", "60"), 2)
                sampled.assert_not_called()

    def test_receipt_is_written_whole(self):
        with tempfile.TemporaryDirectory() as directory:
            log = Path(directory) / "host.log"
            log.write_text("")
            out = Path(directory) / "receipts"
            receipt = {"complete": False}
            with patch.object(measure, "measure", return_value=receipt), contextlib.redirect_stdout(io.StringIO()):
                code = measure.main(["--host", "hermes", "--scenario", "s", "--run", "2", "--fps", "60",
                                     "--out", str(out), "--log", str(log)])
            self.assertEqual(code, 1)
            self.assertEqual(json.loads((out / "s-hermes-run2.json").read_text()), receipt)
            self.assertEqual(sorted(os.listdir(out)), ["s-hermes-run2.json"])


if __name__ == "__main__":
    unittest.main()
