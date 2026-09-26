#!/usr/bin/env python3
"""Pin the phase timer the SteamOS build reports through.

The value of this file is that a build which fails mid-phase still says which phase, and that the
numbers reaching the receipt artifact are the real ones. A timer that silently recorded nothing, or
that lost the phase in flight when the build died, would read as a successful instrumentation change
while telling us nothing, which is the whole failure mode it exists to remove.
"""

import pathlib
import subprocess
import tempfile
import unittest

ROOT = pathlib.Path(__file__).resolve().parents[2]
HELPER = ROOT / "scripts/ci/phase-timings.sh"


def run(script, clock=None, counter=None):
    """Run a snippet with the helper sourced. `clock` replaces date +%s with a scripted sequence.

    The position in that sequence is kept in a file rather than a variable, because the helper reads
    the clock through a command substitution, which runs in a subshell: an incremented variable would
    be discarded and every phase would measure zero seconds. Found by this test failing that way.
    """
    preamble = ". %s\n" % HELPER
    if clock is not None:
        ticks = " ".join(str(t) for t in clock)
        preamble += (
            "PHASE_TEST_TICKS=(%s)\n"
            "PHASE_TEST_COUNTER=%s\n"
            "printf 0 > \"$PHASE_TEST_COUNTER\"\n"
            "phase_timings_now() {\n"
            "  local i\n"
            "  i=\"$(cat \"$PHASE_TEST_COUNTER\")\"\n"
            "  printf '%%s' \"${PHASE_TEST_TICKS[$i]}\"\n"
            "  printf '%%s' \"$((i + 1))\" > \"$PHASE_TEST_COUNTER\"\n"
            "}\n" % (ticks, counter)
        )
    done = subprocess.run(
        ["bash", "-c", preamble + script],
        capture_output=True,
        text=True,
    )
    return done


class PhaseTimingsTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.addCleanup(self.dir.cleanup)
        self.out = pathlib.Path(self.dir.name) / "timings.txt"

    def sh(self, script, clock=None):
        return run(script, clock=clock, counter=str(pathlib.Path(self.dir.name) / "clock"))

    def test_the_helper_exists_and_is_executable_shell(self):
        self.assertTrue(HELPER.is_file(), "%s is missing" % HELPER)
        done = subprocess.run(["bash", "-n", str(HELPER)], capture_output=True, text=True)
        self.assertEqual(done.returncode, 0, done.stderr)

    def test_records_elapsed_seconds_per_phase(self):
        done = self.sh(
            "phase_timings_init %s\n"
            "phase first\n"
            "phase second\n"
            "phase_timings_finish\n" % self.out,
            clock=[100, 110, 135],
        )
        self.assertEqual(done.returncode, 0, done.stderr)
        rows = [line.split("\t") for line in self.out.read_text().splitlines()]
        self.assertEqual(rows, [["first", "10"], ["second", "25"]])

    def test_a_phase_in_flight_is_not_lost_when_the_build_dies(self):
        """The reason the timer is a function rather than inline echoes: cleanup can flush it."""
        done = self.sh(
            "phase_timings_init %s\n"
            "trap 'phase_timings_finish' EXIT\n"
            "phase mirror-sync\n"
            "phase pacstrap\n"
            "false\n" % self.out,
            clock=[0, 30, 90, 90],
        )
        self.assertNotEqual(done.returncode, 0, "the snippet was supposed to fail")
        rows = [line.split("\t") for line in self.out.read_text().splitlines()]
        self.assertEqual(rows, [["mirror-sync", "30"], ["pacstrap", "60"]])
        self.assertIn("pacstrap", done.stdout)

    def test_emits_collapsible_groups_so_one_step_is_navigable(self):
        done = self.sh(
            "phase_timings_init %s\nphase alpha\nphase beta\nphase_timings_finish\n" % self.out,
            clock=[0, 1, 2],
        )
        self.assertEqual(done.stdout.count("::group::"), 2, done.stdout)
        self.assertEqual(done.stdout.count("::endgroup::"), 2, done.stdout)
        # Opened before closed, or the log nests wrongly and every phase folds into the first.
        self.assertLess(done.stdout.index("::group::"), done.stdout.index("::endgroup::"))

    def test_annotates_each_phase_with_its_duration(self):
        done = self.sh(
            "phase_timings_init %s\nphase pacstrap\nphase_timings_finish\n" % self.out,
            clock=[0, 412],
        )
        self.assertIn("::notice title=SteamOS phase::pacstrap took 412s", done.stdout)

    def test_the_summary_totals_the_phases(self):
        done = self.sh(
            "phase_timings_init %s\nphase a\nphase b\nphase_timings_finish\n" % self.out,
            clock=[0, 60, 200],
        )
        self.assertIn("total", done.stdout)
        self.assertRegex(done.stdout, r"total\s+200s")

    def test_finishing_twice_is_harmless(self):
        """cleanup runs on EXIT, and the script may also finish explicitly on the success path."""
        done = self.sh(
            "phase_timings_init %s\nphase only\nphase_timings_finish\nphase_timings_finish\n" % self.out,
            clock=[0, 5, 5],
        )
        self.assertEqual(done.returncode, 0, done.stderr)
        rows = [line.split("\t") for line in self.out.read_text().splitlines()]
        self.assertEqual(rows, [["only", "5"]], "a second finish must not append a phantom row")

    def test_init_truncates_a_stale_file(self):
        self.out.write_text("stale\t999\n")
        done = self.sh(
            "phase_timings_init %s\nphase fresh\nphase_timings_finish\n" % self.out,
            clock=[0, 1],
        )
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertNotIn("stale", self.out.read_text())

    def test_sourcing_alone_writes_nothing(self):
        """The SteamOS script sources this before its mount checks have all run."""
        done = self.sh("printf 'sourced\\n'")
        self.assertEqual(done.returncode, 0, done.stderr)
        self.assertEqual(done.stdout.strip(), "sourced")
        self.assertFalse(self.out.exists())


class SteamosWiringTest(unittest.TestCase):
    """The helper is only useful if the SteamOS build actually reports through it."""

    def setUp(self):
        self.script = (ROOT / "scripts/ci/run-steamos-build.sh").read_text()

    def test_the_steamos_build_sources_and_initializes_the_timer(self):
        self.assertIn("scripts/ci/phase-timings.sh", self.script)
        self.assertIn("phase_timings_init", self.script)

    def test_the_expensive_phases_are_named(self):
        # These are the candidates for caching, so each has to be separable in the numbers.
        for name in ("mirror-sync", "pacstrap", "polaris-build"):
            self.assertIn("phase %s" % name, self.script, "phase %s is not reported" % name)

    def test_cleanup_flushes_the_phase_in_flight(self):
        """Without this a failing build reports every completed phase and not the one that broke."""
        cleanup = self.script[self.script.index("cleanup() {"):]
        cleanup = cleanup[: cleanup.index("\n}")]
        self.assertIn("phase_timings_finish", cleanup)

    def test_timings_land_where_the_receipts_upload_collects_them(self):
        # The workflow uploads /output/steamos3.8-*.txt, so this name needs no workflow change.
        self.assertIn("/output/steamos3.8-phase-timings.txt", self.script)


if __name__ == "__main__":
    unittest.main()
