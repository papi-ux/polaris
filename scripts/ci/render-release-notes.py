#!/usr/bin/env python3
"""Render the GitHub release body a tag publishes from its curated release notes.

docs/release-notes/vX.Y.Z.md is written for the X.Y.Z release and stays that way: the stable gate
(scripts/check-stable-release-notes.py) and the release contract refuse a beta-tester line in it. A
stable tag vX.Y.Z publishes that file byte for byte. A beta or release candidate, vX.Y.Z-beta.N or
vX.Y.Z-rc.N, publishes the same notes before X.Y.Z exists, so its body opens with a short preamble
saying what it is, and Polaris's own download links, which name the X.Y.Z release, name the tag
being read instead. Every other link is left exactly as written. The release job stages these bytes
and holds the published body to the same bytes.
"""

import argparse
import re
import sys
from pathlib import Path


# The tag rule the release job enforces (build.yml, "Bind release tag to source commit"). A tag the
# job would refuse is refused here too, rather than rendered as something it is not.
TAG = re.compile(r"v([0-9]+\.[0-9]+\.[0-9]+)(?:-((beta|rc)\.[0-9]+))?")

# Polaris's own release downloads. Only this exact prefix, followed by the release's own version and
# a slash, moves to the prerelease tag: another repository, another version, a source link or the
# same text inside some other URL is not one of this release's attached files.
OWN_DOWNLOADS = "https://github.com/papi-ux/polaris/releases/download/"
URL = re.compile(r"[A-Za-z][A-Za-z0-9+.-]*:[^\s<>\[\]`\"']+")

PREAMBLE = (
    "> **this is {release}, for people who choose Beta.** Stable {version} isnt published yet, so use "
    "the files attached to this release. also the one-command repository installer still installs "
    "Stable, not this {kind}, and updates from repo.papi-ux.com stay on Stable too. the change list "
    "below describes the planned {version}, thank you for trying it early!\n"
    "\n"
)
KIND = {"beta": "beta", "rc": "RC"}


class Refused(Exception):
    pass


def parse_tag(tag):
    """The release version and prerelease label ('' for stable) a tag names."""
    match = TAG.fullmatch(tag)
    if not match:
        raise Refused(f"release tag must match vMAJOR.MINOR.PATCH, optionally -beta.N or -rc.N: {tag!r}")
    return match.group(1), match.group(2) or ""


def render(tag, prerelease, notes_path):
    """The exact bytes the release for tag publishes, from the notes at notes_path."""
    version, label = parse_tag(tag)
    if prerelease not in ("true", "false"):
        raise Refused(f"prerelease must be true or false, not {prerelease!r}")
    if (prerelease == "true") != bool(label):
        channel = "a prerelease" if prerelease == "true" else "a stable release"
        raise Refused(f"{tag} is being published as {channel}, which its tag does not name")
    notes_path = Path(notes_path)
    if notes_path.name != f"v{version}.md":
        raise Refused(f"{tag} publishes the notes for v{version}, not {notes_path}")
    notes = notes_path.read_bytes()
    if not notes.strip():
        raise Refused(f"missing curated release notes: {notes_path}")
    if not label:
        return notes, 0

    text = notes.decode("utf-8")
    old = f"{OWN_DOWNLOADS}v{version}/"
    moved = 0

    def own_download(match):
        nonlocal moved
        url = match.group()
        if url.startswith(old):
            moved += 1
            return f"{OWN_DOWNLOADS}{tag}/" + url[len(old):]
        return url

    text = URL.sub(own_download, text)
    preamble = PREAMBLE.format(release=f"{version}-{label}", version=version, kind=KIND[label.split(".")[0]])
    return (preamble + text).encode("utf-8"), moved


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--tag", required=True, help="the release tag, vX.Y.Z, vX.Y.Z-beta.N or vX.Y.Z-rc.N")
    parser.add_argument("--prerelease", required=True, help="true or false, as the release job decided it")
    parser.add_argument("--notes", required=True, help="the curated notes, docs/release-notes/vX.Y.Z.md")
    parser.add_argument("--output", help="where to write the body; standard output when omitted")
    arguments = parser.parse_args(argv)

    try:
        body, moved = render(arguments.tag, arguments.prerelease, arguments.notes)
    except (Refused, OSError, UnicodeDecodeError) as error:
        print(f"Refusing to render release notes: {error}", file=sys.stderr)
        return 1

    if arguments.output:
        Path(arguments.output).write_bytes(body)
    else:
        sys.stdout.buffer.write(body)
        sys.stdout.buffer.flush()
    if parse_tag(arguments.tag)[1]:
        print(f"{arguments.tag}: preamble added, {moved} own download links now name {arguments.tag}", file=sys.stderr)
    else:
        print(f"{arguments.tag}: {arguments.notes} unchanged", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
