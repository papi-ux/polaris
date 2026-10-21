import contextlib
import copy
import io
import json
from pathlib import Path
import tempfile
import unittest

import compare_runs as compare


def receipt(host="polaris", scenario="1440p120", run=1, host_cpu=20.0, **changes):
    base = {
        "schema": compare.SCHEMA, "host": host, "scenario": scenario, "run": run, "complete": True,
        "target": {"fps": 120.0, "resolution": "2560x1440", "bitrate_mbps": 50.0, "codec": "hevc", "dynamic_range": "sdr"},
        "host_settings": {"min_log_level": "debug"},
        "environment": {
            "kernel": "7.2.9-2-cachyos",
            "packages": {"mesa": "26.2.4", "kwin": "6.7.5", "pipewire": "1.6.9", "polaris": "1.4.13"},
            "modules": {"hermes_kms": {"loaded": host == "hermes", "version": "0.4.0"}, "evdi": {"loaded": False}},
            "drm": {"cards": [
                {"card": "card0", "driver": "evdi"},
                {"card": "card1", "driver": "amdgpu", "pci_id": "1002:744c", "name": "Navi 31"},
            ]},
        },
        "summary": {
            "processes": {"host": 1, "compositor": 1, "workload": 3},
            "periodic": {"frame_processing": {"windows": 6, "avg_ms": 3.0, "min_ms": 1.0, "max_ms": 9.0}},
            "cpu": {"host_pct": {"mean": host_cpu, "p95": host_cpu, "max": host_cpu}},
            "gpu": {}, "polaris_capture": {}, "hermes_capture": {},
        },
    }
    if host == "polaris":
        base["summary"]["polaris_stats"] = {
            "records": 120, "streaming_records": 120, "stream_path_id": "host_virtual_display",
            "stream_display_mode": "Host Virtual Display", "codec": "hevc", "stream_hdr_enabled": False,
            "adaptive_bitrate_active": False, "bitrate_kbps": {"mean": 38000.0, "p95": 38000, "max": 38000},
            "paired_target_bitrate_kbps": {"mean": 0.0, "p95": 0, "max": 0},
            "width": {"mean": 2560.0, "p95": 2560, "max": 2560}, "height": {"mean": 1440.0, "p95": 1440, "max": 1440},
            "session_target_fps": {"mean": 120.0, "p95": 120.0, "max": 120.0},
            "display_mode_requested": "2560x1440x120", "display_mode_applied": "2560x1440x120",
            "display_mode_pinned_by_host": False,
        }
    for path, value in changes.items():
        *parents, last = path.split("__")
        target = base
        for parent in parents:
            target = target[parent]
        target[last] = value
    return base


class Receipts:
    def __init__(self, directory):
        self.directory = Path(directory)

    def add(self, run, name=None):
        name = name or f"{run['scenario']}-{run['host']}-run{run['run']}.json"
        (self.directory / name).write_text(json.dumps(run))


def report_for(runs, photos=()):
    warnings = []
    return compare.build_report({compare.run_key(run): copy.deepcopy(run) for run in runs}, list(photos), warnings)


class PhotoTests(unittest.TestCase):
    def test_clock_difference_handles_the_wrap(self):
        self.assertEqual(compare.photo_latency_ms(12.345, 12.301), 44.0)
        self.assertEqual(compare.photo_latency_ms(0.020, 99.980), 40.0)
        self.assertEqual(compare.photo_latency_ms(10.000, 10.010), -10.0)

    def test_photo_rows(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "photos.csv"
            path.write_text(
                "# read from the phone\nscenario,host,run,photo,host_clock,stream_clock\n"
                "4k60,hermes,1,IMG_1,55.500,55.462\n\n4k60,hermes,1,IMG_2,,55.0\n"
            )
            warnings = []
            photos = compare.read_photos(path, warnings)
        self.assertEqual(photos, [{"scenario": "4k60", "host": "hermes", "run": 1, "photo": "IMG_1", "latency_ms": 38.0}])
        self.assertEqual(warnings, ["photos.csv line 5: a clock is blank, so the photo is left out"])


class CsvTests(unittest.TestCase):
    def write(self, directory, text):
        path = Path(directory) / "client.csv"
        path.write_text(text)
        return path

    def test_client_rows_attach_to_their_runs(self):
        with tempfile.TemporaryDirectory() as directory:
            path = self.write(directory, "scenario,host,run,received_fps,decode_ms\n# note\n1440p120,polaris,1,119.5,\n1440p120,hermes,4,118,1.1\n")
            runs = {("1440p120", "polaris", 1): receipt()}
            warnings = []
            compare.attach_client(path, runs, warnings)
        self.assertEqual(runs[("1440p120", "polaris", 1)]["client"], {"received_fps": 119.5})
        self.assertEqual(runs[("1440p120", "hermes", 4)]["client"], {"received_fps": 118.0, "decode_ms": 1.1})
        self.assertEqual(warnings, ["client.csv line 4: no receipt for 1440p120 hermes run 4"])

    def test_bad_files_name_the_line(self):
        cases = {
            "scenario,host,received_fps\n": "missing run",
            "scenario,host,run,fps\n": "unknown fps",
            "scenario,host,run\n\n# x\ns,polaris,one\n": "line 4: run must be a whole number",
            "scenario,host,run,decode_ms\ns,polaris,1,fast\n": "line 2: decode_ms is not a number",
            "scenario,host,run,decode_ms\ns,polaris,1,nan\n": "line 2: decode_ms is not a number",
            "scenario,host,run\ns,,1\n": "line 2: scenario, host and run are all needed",
            "scenario,host,run\ns,polaris,1\ns,polaris,1\n": "line 3: s polaris run 1 is listed twice",
        }
        for text, message in cases.items():
            with self.subTest(message=message), tempfile.TemporaryDirectory() as directory:
                with self.assertRaises(SystemExit) as raised:
                    compare.attach_client(self.write(directory, text), {}, [])
                self.assertIn(message, str(raised.exception))


class ReceiptLoadingTests(unittest.TestCase):
    def test_only_whole_receipts_are_loaded(self):
        with tempfile.TemporaryDirectory() as directory:
            receipts = Receipts(directory)
            receipts.add(receipt())
            receipts.add(receipt(), name="copy.json")
            receipts.add(dict(receipt(), schema="other"), name="other.json")
            receipts.add(dict(receipt(), run="1"), name="text-run.json")
            (Path(directory) / "broken.json").write_text("{")
            (Path(directory) / "notes.txt").write_text("not a receipt")
            warnings = []
            runs = compare.load_receipts(Path(directory), warnings)
        self.assertEqual(list(runs), [("1440p120", "polaris", 1)])
        self.assertEqual(runs[("1440p120", "polaris", 1)]["file"], "1440p120-polaris-run1.json")
        self.assertEqual(warnings, [
            "broken.json: unreadable (JSONDecodeError), skipped",
            "copy.json: repeats 1440p120 polaris run 1, skipped",
            "other.json: not a polaris-virtual-display-run-v1 receipt, skipped",
            "text-run.json: no scenario, host or run number, skipped",
        ])


class ReportTests(unittest.TestCase):
    def test_runs_average_per_host_with_polaris_first(self):
        report = report_for([
            receipt("hermes", host_cpu=10.0),
            receipt(run=1, host_cpu=20.0),
            receipt(run=2, host_cpu=30.0),
        ])
        scenario = report["scenarios"][0]
        self.assertEqual(scenario["hosts"], ["polaris", "hermes"])
        self.assertEqual(scenario["runs"], {"polaris": 2, "hermes": 1})
        rows = {row["metric"]: row for section in scenario["sections"] for row in section["rows"]}
        self.assertEqual(rows["Host process CPU, mean"]["values"]["polaris"], {"n": 2, "mean": 25.0, "min": 20.0, "max": 30.0})
        self.assertNotIn("VCN busy, mean", rows)
        self.assertEqual(report["warnings"], [])

        markdown = compare.render_markdown(report)
        self.assertIn("## 1440p120: 2560x1440, 120 fps, HEVC SDR, 50 Mb/s", markdown)
        self.assertIn("| Metric | polaris | hermes | hermes vs polaris |", markdown)
        self.assertIn("| Host process CPU, mean (% of a core) | 25.0 (20.0 to 30.0) | 10.0 | -15.0 (-60%) |", markdown)
        self.assertIn("| Metric | polaris | hermes |\n", markdown)

    def test_a_third_host_is_also_set_against_polaris(self):
        report = report_for([receipt(), receipt("hermes", host_cpu=10.0), receipt("polaris-kms", host_cpu=22.0)])
        markdown = compare.render_markdown(report)
        self.assertIn("| Metric | polaris | hermes | polaris-kms | hermes vs polaris | polaris-kms vs polaris |", markdown)

    def test_client_shortfall_and_photos(self):
        polaris, hermes = receipt(), receipt("hermes")
        polaris["client"] = {"received_fps": 117.0}
        photos = [
            {"scenario": "1440p120", "host": "polaris", "run": 1, "photo": str(index), "latency_ms": value}
            for index, value in enumerate([30.0, 40.0, 50.0, 60.0])
        ]
        report = report_for([polaris, hermes], photos)
        scenario = report["scenarios"][0]
        rows = {row["metric"]: row for section in scenario["sections"] for row in section["rows"]}
        self.assertEqual(rows["Received frames short of target"]["values"]["polaris"]["mean"], 2.5)
        self.assertEqual(rows["Received frames short of target"]["values"]["hermes"], {"n": 0})
        self.assertEqual(scenario["glass_to_glass_ms"], {"polaris": {"n": 4, "median": 45.0, "p90": 60.0, "min": 30.0, "max": 60.0}})
        markdown = compare.render_markdown(report)
        self.assertIn("| Photos | 4 | n/a |", markdown)
        self.assertIn("| Median (ms) | 45.0 | n/a |", markdown)

    def test_runs_that_need_a_second_look(self):
        bad = receipt(complete=False, summary__periodic={}, summary__processes={"host": 0, "workload": 0})
        bad["summary"]["polaris_stats"].update({
            "streaming_records": 0, "errors": {"http_401": 2}, "stream_path_id": "windowed_stream",
            "stream_display_mode": "Windowed Stream", "adaptive_bitrate_active": True, "codec": "h264",
            "stream_hdr_enabled": True, "bitrate_kbps": {"mean": 30000.0, "p95": 38000, "max": 38000},
            "paired_target_bitrate_kbps": {"mean": 20000.0, "p95": 20000, "max": 20000},
            "width": {"mean": 1920.0, "p95": 1920, "max": 1920},
            "session_target_fps": {"mean": 60.0, "p95": 60.0, "max": 60.0},
            "display_mode_applied": "1920x1080x60", "display_mode_pinned_by_host": True,
        })
        hermes = receipt("hermes", environment__modules={"hermes_kms": {"loaded": False}})
        report = report_for([bad, hermes])
        self.assertEqual(report["warnings"], [
            "1440p120 hermes run 1: hermes_kms was not loaded",
            "1440p120 polaris run 1: stopped early",
            "1440p120 polaris run 1: no host timing lines; was the stream running with min_log_level = debug?",
            "1440p120 polaris run 1: the host process was not found",
            "1440p120 polaris run 1: the test page was not running",
            "1440p120 polaris run 1: Polaris stats never showed a stream (http_401 x2)",
            "1440p120 polaris run 1: Polaris streamed with Windowed Stream, not Host Virtual Display",
            "1440p120 polaris run 1: Polaris adaptive bitrate was active",
            "1440p120 polaris run 1: Polaris changed the encoder bitrate during the run",
            "1440p120 polaris run 1: Polaris has 20000 kbps saved as this device's bitrate, below the run's 50000",
            "1440p120 polaris run 1: Polaris encoded h264, the run asked for hevc",
            "1440p120 polaris run 1: Polaris gave the virtual display 1920x1080x60 (saved for this device), the run asked for 2560x1440 at 120 fps",
            "1440p120 polaris run 1: Polaris encoded 1920x1440, the run asked for 2560x1440",
            "1440p120 polaris run 1: Polaris aimed for 60 fps, the run asked for 120",
            "1440p120 polaris run 1: Polaris streamed HDR, the run asked for SDR",
        ])

    def test_a_size_off_on_one_side_only(self):
        polaris = receipt()
        polaris["summary"]["polaris_stats"].update({
            "display_mode_applied": "2560x1600x120", "height": {"mean": 1600.0, "p95": 1600, "max": 1600},
        })
        self.assertEqual(report_for([polaris])["warnings"], [
            "1440p120 polaris run 1: Polaris gave the virtual display 2560x1600x120, the run asked for 2560x1440 at 120 fps",
            "1440p120 polaris run 1: Polaris encoded 2560x1600, the run asked for 2560x1440",
        ])

    def test_targets_that_match_raise_nothing(self):
        # A saved bitrate above the run's caps nothing, and 119.88 Hz is the 120 asked for.
        polaris = receipt()
        polaris["summary"]["polaris_stats"].update({
            "paired_target_bitrate_kbps": {"mean": 80000.0, "p95": 80000, "max": 80000},
            "session_target_fps": {"mean": 119.88, "p95": 119.88, "max": 119.88},
            "display_mode_applied": "2560x1440x119.88", "display_mode_pinned_by_host": True,
        })
        bare = receipt(run=2)
        for field in ("paired_target_bitrate_kbps", "width", "session_target_fps", "display_mode_applied"):
            del bare["summary"]["polaris_stats"][field]
        self.assertEqual(report_for([polaris, bare])["warnings"], [])

    def test_mode_text(self):
        self.assertEqual(compare.parse_mode("2560x1440"), (2560, 1440, None))
        self.assertEqual(compare.parse_mode("1920x1080x59.94"), (1920, 1080, 59.94))
        for text in ("", "2560", "2560x1440x", "2560 x 1440", "4k60", None, 2560):
            with self.subTest(text=text):
                self.assertIsNone(compare.parse_mode(text))

    def test_settings_that_differ_between_hosts(self):
        report = report_for([
            receipt(host_settings={"min_log_level": "debug", "encoder": "vaapi", "fec_percentage": "20", "qp": "24"}),
            receipt("polaris-mod", host_settings={"min_log_level": "debug", "encoder": "vaapi", "fec_percentage": "20"}),
            receipt("hermes", host_settings={"min_log_level": "debug", "encoder": "vaapi"}),
        ])
        self.assertEqual(report["warnings"], [
            "fec_percentage differs between hosts: polaris 20, hermes not set, polaris-mod 20",
        ])

    def test_versions_that_differ_or_change(self):
        report = report_for([
            receipt(),
            receipt(run=2, environment__kernel="7.3.0"),
            receipt("hermes", environment__packages={"mesa": "26.3.0", "kwin": "6.7.5", "pipewire": "1.6.9"}),
        ])
        self.assertIn("polaris: kernel, packages or host settings changed between runs", report["warnings"])
        self.assertIn("Mesa differs between hosts: polaris 26.2.4, hermes 26.3.0", report["warnings"])
        environment = report["environment"]
        self.assertEqual(environment["polaris"]["GPU"], "Navi 31 (1002:744c)")
        self.assertEqual(environment["hermes"]["hermes_kms module"], "loaded, 0.4.0")
        self.assertEqual(environment["hermes"]["Polaris"], "not installed")
        self.assertEqual(environment["polaris"]["evdi module"], "not loaded")

    def test_different_targets_in_one_scenario(self):
        report = report_for([receipt(), receipt("hermes", target={"fps": 60.0})])
        self.assertIn("1440p120: the runs asked for different targets; see each receipt's target", report["warnings"])


class CommandLineTests(unittest.TestCase):
    def test_markdown_and_json(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "receipts").mkdir()
            receipts = Receipts(root / "receipts")
            receipts.add(receipt())
            receipts.add(receipt("hermes"))
            (root / "client.csv").write_text("scenario,host,run,received_fps\n1440p120,hermes,1,119.9\n")
            (root / "photos.csv").write_text("scenario,host,run,photo,host_clock,stream_clock\n1440p120,hermes,1,a,1.050,1.010\n")
            output = io.StringIO()
            with contextlib.redirect_stdout(output):
                code = compare.main([
                    str(root / "receipts"), "--client", str(root / "client.csv"),
                    "--photos", str(root / "photos.csv"), "--json", str(root / "report.json"),
                ])
            report = json.loads((root / "report.json").read_text())
        self.assertEqual(code, 0)
        self.assertIn("### Reported by the client", output.getvalue())
        self.assertIn("## Environment", output.getvalue())
        self.assertNotIn("## Check before publishing", output.getvalue())
        self.assertEqual(report["scenarios"][0]["glass_to_glass_ms"]["hermes"]["median"], 40.0)

    def test_nothing_to_compare(self):
        with tempfile.TemporaryDirectory() as directory, contextlib.redirect_stderr(io.StringIO()):
            for argv in ([directory], [str(Path(directory) / "missing")]):
                with self.subTest(argv=argv), self.assertRaises(SystemExit) as raised:
                    compare.main(argv)
                self.assertEqual(raised.exception.code, 2)


if __name__ == "__main__":
    unittest.main()
