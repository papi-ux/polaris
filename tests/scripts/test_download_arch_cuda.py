"""Exercise the download gate without networking or package installation."""
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

SCRIPT = Path(__file__).resolve().parents[2] / "scripts/ci/download-arch-cuda.sh"


class DownloadGateTest(unittest.TestCase):
    def run_gate(self, mode):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            commands = {
                "timeout": '#!/bin/sh\nprintf "%s\\n" "$*" >> "$POLARIS_CUDA_TEST_DIR/deadlines"\nshift 2\nexec "$@"\n',
                "sleep": '#!/bin/sh\nprintf "%s\\n" "$*" >> "$POLARIS_CUDA_TEST_DIR/sleeps"\n',
                "pacman": """#!/bin/sh
count=0
if test -f "$POLARIS_CUDA_TEST_DIR/count"; then
  count=$(cat "$POLARIS_CUDA_TEST_DIR/count")
fi
count=$((count + 1))
printf '%s\\n' "$count" > "$POLARIS_CUDA_TEST_DIR/count"
printf '%s\\n' "$*" >> "$POLARIS_CUDA_TEST_DIR/arguments"
case "$POLARIS_CUDA_TEST_MODE" in
  success) exit 0 ;;
  recover) test "$count" -ge 3 ;;
  fail) exit 23 ;;
  timeout) exit 124 ;;
  locking)
    # The real pacman takes the database lock and an attempt killed at the
    # deadline leaves it behind. The first attempt does exactly that; the
    # second resumes and succeeds, but only if the lock was cleared first.
    if test -e "$POLARIS_PACMAN_LOCK"; then
      printf 'error: failed to init transaction (unable to lock database)\n' >&2
      exit 1
    fi
    : > "$POLARIS_PACMAN_LOCK"
    if test "$count" -eq 1; then
      exit 124
    fi
    rm -f "$POLARIS_PACMAN_LOCK"
    exit 0
    ;;
esac
""",
            }
            for name, source in commands.items():
                path = root / name
                path.write_text(source)
                path.chmod(0o755)
            result = subprocess.run(
                ["bash", str(SCRIPT)],
                env=os.environ | {
                    "PATH": str(root) + os.pathsep + os.environ["PATH"],
                    "POLARIS_CUDA_TEST_DIR": str(root),
                    "POLARIS_CUDA_TEST_MODE": mode,
                    "POLARIS_PACMAN_LOCK": str(root / "db.lck"),
                },
                text=True, capture_output=True, timeout=5,
            )
            arguments = (root / "arguments").read_text().splitlines()
            deadlines = (root / "deadlines").read_text().splitlines()
            # All attempts only download from the already configured repositories.
            # They must never refresh databases, install packages, or disable signatures.
            self.assertEqual(arguments, [
                "-Sw --noconfirm --needed --disable-download-timeout cuda"
            ] * len(arguments))
            self.assertEqual(deadlines, [
                "--kill-after=30s 10m pacman " + arguments[0]
            ] * len(arguments))
            sleeps = (root / "sleeps").read_text().splitlines() if (root / "sleeps").exists() else []
            self.assertEqual(sleeps, ["5"] * (len(arguments) - 1))
            return result, len(arguments)

    def test_first_success_stops(self):
        result, attempts = self.run_gate("success")
        self.assertEqual((result.returncode, attempts), (0, 1), result.stderr)

    def test_transient_failure_recovers(self):
        result, attempts = self.run_gate("recover")
        self.assertEqual((result.returncode, attempts), (0, 3), result.stderr)

    def test_persistent_failure_blocks_after_bound(self):
        result, attempts = self.run_gate("fail")
        self.assertEqual((result.returncode, attempts), (23, 3), result.stderr)

    def test_exhausted_timeouts_remain_failures(self):
        result, attempts = self.run_gate("timeout")
        self.assertEqual((result.returncode, attempts), (124, 3), result.stderr)

    def test_a_killed_attempt_does_not_poison_the_retries(self):
        """The bug this guards: a deadline kill leaves the database lock behind.

        pacman is killed mid-transaction holding /var/lib/pacman/db.lck, and the lock outlives
        it, so every later attempt failed instantly with "could not lock database: File exists".
        The retry existed to resume a partial download and could never run, and the log blamed a
        lock while the real cause was a 4.8 GB download that needed more time than one attempt.

        This is what took the v1.4.13-beta.2 release build down.
        """
        result, attempts = self.run_gate("locking")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(attempts, 2, "the second attempt should resume, not die on a stale lock")
        self.assertIn("hit the deadline", result.stderr)
        self.assertIn("clearing the database lock", result.stderr)


if __name__ == "__main__":
    unittest.main()
