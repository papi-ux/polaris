from pathlib import Path
import subprocess
import sys
import tempfile
import textwrap
import unittest


ROOT = Path(__file__).resolve().parents[2]
GATE = ROOT / "scripts/check-stable-release-notes.py"

# The v1.4.13 notes as the betas published them, which the stable page then repeated.
BETA_NOTES = textwrap.dedent(
    """\
    # Polaris v1.4.13

    Turning DRM/KMS capture back off is one command now instead of three. Polaris 1.4.13 is matched with Nova 1.4.13; existing settings, paired devices and Spaces keep working.

    **Heads up**

    - While 1.4.13 is in beta, use the packages attached to the prerelease you are reading. The install commands below name the final release, which does not exist yet.
    """
)


class StableReleaseNotesGate(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="polaris-stable-notes-")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)

    def notes(self, text, name="v9.9.9.md"):
        path = self.root / name
        path.write_text(text, encoding="utf-8")
        return path

    def run_gate(self, *paths):
        return subprocess.run(
            [sys.executable, str(GATE), *map(str, paths)],
            capture_output=True,
            text=True,
            check=False,
        )

    def test_refuses_what_the_betas_told_their_testers(self):
        result = self.run_gate(self.notes(BETA_NOTES))
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn('v9.9.9.md:7: "While 1.4.13 is in beta" tells a beta tester', result.stderr)
        self.assertIn('v9.9.9.md:7: "the prerelease you are reading" sends the reader', result.stderr)

    def test_refuses_a_matched_nova_release(self):
        result = self.run_gate(self.notes(BETA_NOTES))
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn('v9.9.9.md:3: "matched with Nova" calls the release matched', result.stderr)
        # The form the 1.4.2 to 1.4.7 notes used carries a v, and it is the same claim.
        result = self.run_gate(self.notes("Stability update, matched with Nova v1.4.2.\n"))
        self.assertEqual(result.returncode, 1, result.stderr)

    def test_a_rewrapped_paragraph_cannot_hide_a_phrase(self):
        wrapped = "- While 1.4.14\n  is in beta, use the packages attached to the\n  prerelease you are reading.\n"
        result = self.run_gate(self.notes(wrapped))
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn(':1: "While 1.4.14 is in beta"', result.stderr)
        self.assertIn(':2: "the prerelease you are reading"', result.stderr)
        result = self.run_gate(self.notes("Polaris 1.4.14 is matched\nwith Nova 1.4.14.\n"))
        self.assertEqual(result.returncode, 1, result.stderr)

    def test_the_other_ways_to_say_it_are_refused_too(self):
        for sentence in (
            "While this release is in beta, install it by hand.",
            "While Polaris 1.4.14 is still in beta, keep your repository on stable.",
            "while v1.4.14 is in beta, nothing updates by itself.",
        ):
            with self.subTest(sentence=sentence):
                result = self.run_gate(self.notes(sentence + "\n"))
                self.assertEqual(result.returncode, 1, result.stderr)

    def test_the_template_beta_line_is_refused(self):
        # The template gives beta notes their line in exact words and says a stable tag is refused
        # while it is still there. Reading the line from the template keeps that promise true: a
        # rewording of the template that the gate does not know fails here, not on a stable page.
        template = (ROOT / "docs/release-notes/TEMPLATE.md").read_text(encoding="utf-8")
        lines = [line for line in template.splitlines() if "X.Y.Z is in beta" in line]
        self.assertEqual(len(lines), 1, "the template gives the beta line once")
        result = self.run_gate(self.notes(lines[0].replace("X.Y.Z", "1.4.14") + "\n"))
        self.assertEqual(result.returncode, 1, result.stderr)
        for phrase in (
            "While 1.4.14 is in beta",
            "the prerelease you are reading",
            "final release, which does not exist yet",
        ):
            self.assertIn(f':1: "{phrase}"', result.stderr)

    def test_rewordings_of_the_beta_line_are_refused(self):
        # Each of these tells a stable reader to go and find a prerelease, and each got past the
        # first version of this gate, which knew only the words 1.4.13 used.
        for sentence, phrase in (
            (
                "While the release is in beta, use the packages attached to this prerelease.",
                "While the release is in beta",
            ),
            (
                "While 1.4.14 is a beta, use the packages attached to the prerelease.",
                "While 1.4.14 is a beta",
            ),
            (
                "While v1.4.14 is still a prerelease, use the packages attached here.",
                "While v1.4.14 is still a prerelease",
            ),
            (
                "While Polaris is in beta, nothing updates by itself.",
                "While Polaris is in beta",
            ),
            (
                "Until 1.4.14 ships, use the packages attached to this prerelease.",
                "Until 1.4.14 ships",
            ),
            (
                "Use the packages attached to this prerelease.",
                "attached to this prerelease",
            ),
            (
                "During the beta, the install commands below name the final release, which does not exist yet.",
                "final release, which does not exist yet",
            ),
            (
                "The commands name the stable release 1.4.14, which does not exist yet.",
                "stable release 1.4.14, which does not exist yet",
            ),
        ):
            with self.subTest(sentence=sentence):
                result = self.run_gate(self.notes(sentence + "\n"))
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn(f':1: "{phrase}"', result.stderr)

    def test_other_ways_to_call_it_a_matched_release_are_refused(self):
        for sentence, phrase in (
            ("Polaris 1.4.14 is matched to Nova 1.4.14.", "matched to Nova"),
            ("Polaris 1.4.14 and Nova 1.4.14 are a matched pair.", "matched pair"),
            ("Install the matching Nova release with it.", "matching Nova release"),
            ("Pair it with the matching Nova 1.4.14.", "matching Nova 1.4.14"),
            ("It ships as the matched Nova release does.", "matched Nova"),
            ("Polaris 1.4.14 matches Nova 1.4.14.", "matches Nova 1.4.14"),
            ("Polaris 1.4.14 pairs with Nova 1.4.14.", "pairs with Nova 1.4.14"),
        ):
            with self.subTest(sentence=sentence):
                result = self.run_gate(self.notes(sentence + "\n"))
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn(f':1: "{phrase}" calls the release matched', result.stderr)

    def test_naming_nova_for_what_a_feature_needs_is_fine(self):
        text = textwrap.dedent(
            """\
            # Polaris v9.9.9

            Nova 1.4.13 is still in beta, and its stable release follows; existing settings keep working.

            - Nova for Android asks for it in its 1.4.13 beta, when PyroWave is chosen as the video codec.
            - With Nova 1.4.9, Play Setup can move Steam Big Picture between your Desktop and a Space.
            - If this host ran 1.4.13-beta.2 or beta.3, reinstall this release over it.
            - The Nova for Linux Alpha Flatpak attached to those betas is built without the decoder.
            - While Nova 1.4.14 is in beta, PyroWave needs its beta build.
            - Until Nova 1.4.14 ships, the codec picker stays hidden.
            - A host now pairs with Nova without a PIN, and a crash testers reported during the beta is fixed.
            - A config folder the first start needs, which does not exist yet on a new install, is created.
            - Matching Nova client controls are versioned and released independently.
            """
        )
        result = self.run_gate(self.notes(text))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stderr, "")

    def test_a_missing_file_is_refused_rather_than_passed(self):
        result = self.run_gate(self.root / "v9.9.9.md")
        self.assertEqual(result.returncode, 1)
        self.assertIn("cannot read stable release notes", result.stderr)

    def test_every_file_is_checked(self):
        clean = self.notes("# Polaris v9.9.9\n", "clean.md")
        stale = self.notes(BETA_NOTES, "stale.md")
        result = self.run_gate(clean, stale)
        self.assertEqual(result.returncode, 1, result.stderr)
        self.assertIn("stale.md:", result.stderr)
        self.assertNotIn("clean.md:", result.stderr)

    def test_no_notes_is_a_usage_error(self):
        self.assertEqual(self.run_gate().returncode, 2)

    def test_the_corrected_v1_4_13_notes_pass(self):
        # This copy is not what the v1.4.13 page is published from. The release job reads the notes
        # from the tag's own checkout, and the v1.4.13 tag (e7bbad30) still carries what the betas
        # said, so the page says what shipped only through a body posted by hand, and a rerun of
        # that tag's release job would put the old text back. This copy is what the next release's
        # notes and contract test start from, so it has to pass the gate their stable tag will face.
        result = self.run_gate(ROOT / "docs/release-notes/v1.4.13.md")
        self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == "__main__":
    unittest.main()
