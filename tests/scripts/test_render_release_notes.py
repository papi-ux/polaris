"""The body a release tag publishes, rendered from its curated notes.

scripts/ci/render-release-notes.py turns docs/release-notes/vX.Y.Z.md into the GitHub release body for
a tag. These tests hold a stable tag to the file's own bytes and a beta or release candidate to a
preamble plus its own download links and nothing else, refuse a tag the release job would refuse, and
run the release job's staging step against a stand-in gh to show that what it stages and what it
compares the published body with are the renderer's bytes.
"""

import os
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import textwrap
import unittest


ROOT = Path(__file__).resolve().parents[2]
RENDERER = ROOT / "scripts/ci/render-release-notes.py"
WORKFLOW = Path(os.environ.get("POLARIS_TEST_RELEASE_WORKFLOW", ROOT / ".github/workflows/build.yml"))
NOTES = ROOT / "docs/release-notes/v1.5.0.md"
OWN = "https://github.com/papi-ux/polaris/releases/download/"

RC_PREAMBLE = (
    "> **this is 1.5.0-rc.1, for people who choose Beta.** Stable 1.5.0 isnt published yet, so use the "
    "files attached to this release. also the one-command repository installer still installs Stable, "
    "not this RC, and updates from repo.papi-ux.com stay on Stable too. the change list below describes "
    "the planned 1.5.0, thank you for trying it early!\n"
    "\n"
)

# Each line names one link. Only the first block is Polaris's own download for the release.
LINKS = textwrap.dedent(
    """\
    https://github.com/papi-ux/polaris/releases/download/v1.5.0/Polaris-fedora44-x86_64.rpm
    wget --output-document=./x.rpm https://github.com/papi-ux/polaris/releases/download/v1.5.0/Polaris-fedora45-x86_64.rpm &&
    [the Arch package](https://github.com/papi-ux/polaris/releases/download/v1.5.0/Polaris-arch-x86_64.pkg.tar.zst)
    <https://github.com/papi-ux/polaris/releases/download/v1.5.0/Polaris-ubuntu24.04-x86_64.deb>
    `https://github.com/papi-ux/polaris/releases/download/v1.5.0/Polaris-steamos3.9-x86_64.pkg.tar.zst`
    "https://github.com/papi-ux/polaris/releases/download/v1.5.0/SHA256SUMS"
    ---
    https://github.com/papi-ux/nova/releases/download/v1.5.0/Nova-Android-arm64-v8a.apk
    https://github.com/papi-ux/polaris/releases/download/v1.4.13/Polaris-fedora44-x86_64.rpm
    https://github.com/papi-ux/polaris/releases/download/v1.5.0-beta.2/Polaris-fedora44-x86_64.rpm
    https://github.com/papi-ux/polaris/releases/download/v1.5.0.1/Polaris-fedora44-x86_64.rpm
    https://github.com/papi-ux/polaris/releases/download/v1.5.00/Polaris-fedora44-x86_64.rpm
    https://github.com/papi-ux/polaris/releases/download/v11.5.0/Polaris-fedora44-x86_64.rpm
    https://github.com/papi-ux/polaris/releases/download/v1.5.0
    https://github.com/papi-ux/polaris/releases/tag/v1.5.0
    https://github.com/papi-ux/polaris/releases/latest/download/Polaris-fedora44-x86_64.rpm
    https://github.com/papi-ux/polaris/blob/v1.5.0/docs/steamos.md
    https://github.com/papi-ux/polaris/archive/refs/tags/v1.5.0.tar.gz
    https://github.com/papi-ux/polaris-packages/releases/download/v1.5.0/x.rpm
    https://github.com/Papi-UX/Polaris/releases/download/v1.5.0/x.rpm
    http://github.com/papi-ux/polaris/releases/download/v1.5.0/x.rpm
    https://www.github.com/papi-ux/polaris/releases/download/v1.5.0/x.rpm
    https://example.com/out?to=https://github.com/papi-ux/polaris/releases/download/v1.5.0/x.rpm
    https://example.com/mirror/https://github.com/papi-ux/polaris/releases/download/v1.5.0/x.rpm
    https://example.com/#https://github.com/papi-ux/polaris/releases/download/v1.5.0/x.rpm
    https://example.com/?to=(https://github.com/papi-ux/polaris/releases/download/v1.5.0/x.rpm)
    ftp://example.com/#https://github.com/papi-ux/polaris/releases/download/v1.5.0/x.rpm
    https://repo.papi-ux.com/install.sh
    """
)


def run_renderer(*arguments):
    return subprocess.run(
        [sys.executable, str(RENDERER), *map(str, arguments)],
        capture_output=True,
        check=False,
        timeout=30,
    )


def workflow_tag_pattern():
    """The tag rule the release job enforces, read out of the workflow rather than restated."""
    match = re.search(r'release_tag" =~ (\^\S+\$) \]\]', WORKFLOW.read_text())
    if match is None:
        raise AssertionError("the release tag test is no longer a bash regex comparison")
    return match.group(1)


class RenderReleaseNotes(unittest.TestCase):
    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="polaris-render-notes-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)

    def notes(self, content, name="v1.5.0.md"):
        path = self.root / name
        path.write_bytes(content if isinstance(content, bytes) else content.encode())
        return path

    def render(self, tag, notes=NOTES, prerelease=None):
        if prerelease is None:
            prerelease = "true" if "-" in tag else "false"
        return run_renderer("--tag", tag, "--prerelease", prerelease, "--notes", notes)

    def test_a_stable_tag_publishes_the_notes_byte_for_byte(self):
        canonical = NOTES.read_bytes()
        result = self.render("v1.5.0")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, canonical)

        output = self.root / "body.md"
        result = run_renderer("--tag", "v1.5.0", "--prerelease", "false", "--notes", NOTES, "--output", output)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(output.read_bytes(), canonical)

        # Nothing a stable body carries is touched: its own download links, trailing spaces, carriage
        # returns, a missing final newline and bytes outside UTF-8.
        awkward = (LINKS.replace("\n", "\r\n", 3) + "trailing space \n\xe9t\xe9".encode("latin-1").decode("latin-1")).encode("latin-1")
        result = self.render("v1.5.0", self.notes(awkward))
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(result.stdout, awkward)

    def test_a_release_candidate_gets_the_preamble_and_only_its_own_download_links(self):
        canonical = NOTES.read_text(encoding="utf-8")
        result = self.render("v1.5.0-rc.1")
        self.assertEqual(result.returncode, 0, result.stderr)
        body = result.stdout.decode("utf-8")

        self.assertTrue(body.startswith(RC_PREAMBLE), body[:600])
        rest = body[len(RC_PREAMBLE):]
        own = f"{OWN}v1.5.0/"
        self.assertEqual(canonical.count(own), 6, "the canonical notes name six download links of their own")
        self.assertEqual(rest, canonical.replace(own, f"{OWN}v1.5.0-rc.1/"))
        self.assertNotIn(own, body)
        self.assertEqual(rest.count(f"{OWN}v1.5.0-rc.1/"), 6)
        self.assertIn(f"6 own download links now name v1.5.0-rc.1", result.stderr.decode())

        # Every other link survives exactly, in order.
        url = re.compile(r"https?://[^\s)<>`\"']+")
        others = lambda text: [link for link in url.findall(text) if not link.startswith(f"{OWN}v1.5.0")]
        self.assertEqual(others(rest), others(canonical))
        self.assertGreater(len(others(canonical)), 50)

    def test_the_preamble_says_what_a_beta_reader_needs(self):
        body = self.render("v1.5.0-rc.1").stdout.decode("utf-8")
        preamble = body.split("\n\n", 1)[0]
        for claim in (
            "this is 1.5.0-rc.1, for people who choose Beta",
            "Stable 1.5.0 isnt published yet",
            "use the files attached to this release",
            "the one-command repository installer still installs Stable, not this RC",
            "the change list below describes the planned 1.5.0",
        ):
            self.assertIn(claim, preamble)
        self.assertNotIn("—", preamble)
        self.assertNotIn("–", preamble)
        # The preamble is the only addition: the release's own heading still opens the notes.
        self.assertEqual(body.split("\n\n", 1)[1].splitlines()[0], "# Polaris v1.5.0")

    def test_only_polaris_own_exact_download_links_move(self):
        own, others = LINKS.split("---\n")
        result = self.render("v1.5.0-rc.2", self.notes(LINKS))
        self.assertEqual(result.returncode, 0, result.stderr)
        body = result.stdout.decode("utf-8")
        self.assertTrue(body.startswith("> **this is 1.5.0-rc.2, for people who choose Beta.**"))
        moved = own.replace(f"{OWN}v1.5.0/", f"{OWN}v1.5.0-rc.2/")
        self.assertEqual(moved.count("/download/v1.5.0-rc.2/"), 6)
        self.assertEqual(body.split("\n\n", 1)[1], moved + "---\n" + others)

    def test_a_beta_tag_is_rendered_the_same_way(self):
        result = self.render("v1.5.0-beta.3")
        self.assertEqual(result.returncode, 0, result.stderr)
        body = result.stdout.decode("utf-8")
        self.assertTrue(body.startswith("> **this is 1.5.0-beta.3, for people who choose Beta.**"))
        self.assertIn("still installs Stable, not this beta,", body.split("\n\n", 1)[0])
        self.assertEqual(body.count(f"{OWN}v1.5.0-beta.3/"), 6)
        self.assertNotIn(f"{OWN}v1.5.0/", body)

    def test_an_unknown_tag_shape_is_refused(self):
        for tag in (
            "1.5.0", "v1.5", "v1.5.0.1", "v1.5.0-rc", "v1.5.0-rc.", "v1.5.0-rc.1.2", "v1.5.0rc.1",
            "v1.5.0-RC.1", "v1.5.0-alpha.1", "v1.5.0-rc-1", "v1.5.0 ", " v1.5.0", "v1.5.0-rc.1\n",
            "refs/tags/v1.5.0", "V1.5.0", "",
        ):
            with self.subTest(tag=tag):
                output = self.root / "body.md"
                result = run_renderer("--tag", tag, "--prerelease", "true", "--notes", NOTES, "--output", output)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn(b"release tag must match", result.stderr)
                self.assertEqual(result.stdout, b"")
                self.assertFalse(output.exists(), "a refused tag writes no body")

    def test_the_renderer_accepts_exactly_the_tags_the_release_job_accepts(self):
        workflow = re.compile(workflow_tag_pattern())
        for tag in (
            "v1.5.0", "v1.5.0-rc.1", "v1.5.0-rc.49", "v1.5.0-beta.2", "v10.20.30-rc.99",
            "v1.5.0-rc", "v1.5.0-alpha.1", "v1.5.0-rc.1.2", "v1.5", "1.5.0", "v1.5.0-beta",
        ):
            with self.subTest(tag=tag):
                accepted = workflow.search(tag) is not None
                notes = self.notes("# notes\n", f"v{tag[1:].split('-')[0]}.md") if accepted else NOTES
                result = self.render(tag, notes)
                self.assertEqual(result.returncode == 0, accepted, result.stderr)

    def test_a_tag_its_notes_and_its_channel_must_agree(self):
        refusals = (
            # Another release's notes.
            ("v1.5.0-rc.1", "true", ROOT / "docs/release-notes/v1.4.13.md", b"publishes the notes for v1.5.0"),
            # A stable tag published as a prerelease, and a release candidate published as stable.
            ("v1.5.0", "true", NOTES, b"which its tag does not name"),
            ("v1.5.0-rc.1", "false", NOTES, b"which its tag does not name"),
            ("v1.5.0-rc.1", "yes", NOTES, b"prerelease must be true or false"),
            # Empty and missing notes.
            ("v1.5.0-rc.1", "true", self.notes(" \n"), b"missing curated release notes"),
            ("v1.5.0", "false", self.root / "absent" / "v1.5.0.md", b"No such file"),
        )
        for tag, prerelease, notes, message in refusals:
            with self.subTest(tag=tag, prerelease=prerelease, notes=notes):
                result = run_renderer("--tag", tag, "--prerelease", prerelease, "--notes", notes)
                self.assertEqual(result.returncode, 1, result.stderr)
                self.assertIn(message, result.stderr)
                self.assertEqual(result.stdout, b"")


# A stand-in for gh that keeps one release's body in a state directory. create and edit store the
# --notes-file they are given; view reports whether the release exists and prints the stored body,
# or, with STUB_GH_SERVE set, that file instead, as GitHub would if it held some other body.
STUB_GH = textwrap.dedent(
    """\
    #!/bin/sh
    state="$STUB_GH_STATE"
    [ "$1" = release ] || exit 2
    action="$2"
    shift 2
    case "$action" in
      view)
        [ -f "$state/body" ] || exit 1
        case " $* " in
          *" --json body "*)
            if [ -n "${STUB_GH_SERVE:-}" ]; then cat "$STUB_GH_SERVE"; else cat "$state/body"; fi
            printf '\\n'
            ;;
        esac
        ;;
      create|edit)
        while [ $# -gt 0 ]; do
          if [ "$1" = --notes-file ]; then
            printf '%s %s\\n' "$action" "$2" >> "$state/calls"
            cp "$2" "$state/body"
          fi
          shift
        done
        ;;
      *) exit 2 ;;
    esac
    """
)


def stage_step():
    workflow = WORKFLOW.read_text()
    step = workflow.split("      - name: Stage curated GitHub release\n", 1)[1].split("\n      - name:", 1)[0]
    return step, textwrap.dedent(step.split("        run: |\n", 1)[1])


class StagedReleaseBody(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        # The step expands an empty "${channel[@]}" under set -u for a stable tag, which bash before
        # 4.4 calls unbound. The release runner's bash is 5.
        version = subprocess.run(
            ["bash", "-c", 'echo "${BASH_VERSINFO[0]} ${BASH_VERSINFO[1]}"'], capture_output=True, text=True, check=True
        ).stdout.split()
        if tuple(map(int, version)) < (4, 4):
            raise RuntimeError(f"the release staging step needs bash 4.4 or later, and bash here is {'.'.join(version)}")

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="polaris-stage-notes-")
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.checkout = self.root / "checkout"
        (self.checkout / "scripts/ci").mkdir(parents=True)
        (self.checkout / "docs/release-notes").mkdir(parents=True)
        shutil.copyfile(RENDERER, self.checkout / "scripts/ci/render-release-notes.py")
        shutil.copyfile(NOTES, self.checkout / "docs/release-notes/v1.5.0.md")
        self.state = self.root / "gh"
        self.state.mkdir()
        tools = self.root / "bin"
        tools.mkdir()
        (tools / "gh").write_text(STUB_GH)
        (tools / "gh").chmod(0o755)
        self.runner_temp = self.root / "runner-temp"
        self.runner_temp.mkdir()
        self.environment = dict(
            os.environ,
            PATH=f"{tools}{os.pathsep}{os.environ['PATH']}",
            STUB_GH_STATE=str(self.state),
            RUNNER_TEMP=str(self.runner_temp),
            GITHUB_OUTPUT=str(self.root / "github-output"),
        )
        self.environment.pop("STUB_GH_SERVE", None)

    def stage(self, tag, prerelease, **environment):
        _, body = stage_step()
        return subprocess.run(
            ["bash", "-c", body],
            cwd=self.checkout,
            env=dict(self.environment, POLARIS_PACKAGE_REF_NAME=tag, IS_PRERELEASE=prerelease, **environment),
            capture_output=True,
            text=True,
            timeout=60,
        )

    def rendered(self, tag, prerelease):
        result = run_renderer("--tag", tag, "--prerelease", prerelease, "--notes", NOTES)
        self.assertEqual(result.returncode, 0, result.stderr)
        return result.stdout

    def test_create_edit_and_the_comparison_all_name_the_rendered_file(self):
        step, _ = stage_step()
        self.assertIn('rendered_notes="${RUNNER_TEMP}/release-notes-${POLARIS_PACKAGE_REF_NAME}.md"', step)
        self.assertIn("python3 scripts/ci/render-release-notes.py", step)
        self.assertIn('--output "$rendered_notes"', step)
        self.assertEqual(step.count('--notes-file "$rendered_notes"'), 2, "create and edit both stage the rendered body")
        self.assertNotIn('--notes-file "$release_notes"', step)
        self.assertIn('expected_notes="$(cat "$rendered_notes")"', step)
        self.assertNotIn('cat "$release_notes"', step)

    def test_a_release_candidate_stages_and_compares_the_rendered_body(self):
        result = self.stage("v1.5.0-rc.1", "true")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.state / "body").read_bytes(), self.rendered("v1.5.0-rc.1", "true"))
        rendered_path = self.runner_temp / "release-notes-v1.5.0-rc.1.md"
        self.assertEqual((self.state / "calls").read_text(), f"create {rendered_path}\n")
        self.assertIn("publish_draft=true", (self.root / "github-output").read_text())

    def test_a_rerun_takes_the_edit_path_with_the_same_body(self):
        # A rerun finds the release already staged, here with the undecorated notes on it.
        shutil.copyfile(NOTES, self.state / "body")
        result = self.stage("v1.5.0-rc.1", "true")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.state / "body").read_bytes(), self.rendered("v1.5.0-rc.1", "true"))
        rendered_path = self.runner_temp / "release-notes-v1.5.0-rc.1.md"
        self.assertEqual((self.state / "calls").read_text(), f"edit {rendered_path}\n")

    def test_a_stable_tag_stages_the_notes_unchanged(self):
        result = self.stage("v1.5.0", "false")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual((self.state / "body").read_bytes(), NOTES.read_bytes())

    def test_a_published_body_other_than_the_rendered_one_stops_the_release(self):
        # GitHub holding the raw notes under a release candidate is a mismatch, not a pass.
        result = self.stage("v1.5.0-rc.1", "true", STUB_GH_SERVE=str(NOTES))
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("Published release notes do not match", result.stderr)
        self.assertIn("rendered from docs/release-notes/v1.5.0.md", result.stderr)
        # And the decorated body under a stable tag is one too.
        decorated = self.root / "decorated.md"
        decorated.write_bytes(self.rendered("v1.5.0-rc.1", "true"))
        result = self.stage("v1.5.0", "false", STUB_GH_SERVE=str(decorated))
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("Published release notes do not match", result.stderr)

    def test_a_refused_render_stages_nothing(self):
        result = self.stage("v1.5.0-rc.1", "false")
        self.assertEqual(result.returncode, 1, result.stdout)
        self.assertIn("Refusing to render release notes", result.stderr)
        self.assertFalse((self.state / "calls").exists())


if __name__ == "__main__":
    unittest.main()
