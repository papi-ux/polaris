#!/usr/bin/env python3
"""Refuse stable release notes that still carry text written for a beta.

A beta publishes the notes of the release it precedes: v1.4.13-beta.2 and
v1.4.13 both publish docs/release-notes/v1.4.13.md, and the release workflow
sends that file to GitHub as the page body, byte for byte. Whatever the notes
told beta testers is therefore still on the stable page unless someone takes it
out before the tag, and v1.4.13 went out that way. Its page told stable readers
to use the packages of a prerelease, for a release "which does not exist yet",
and called it matched with a Nova release that was still in beta.

Two kinds of line are refused in the notes of a stable tag:

* the line written for beta testers, which docs/release-notes/TEMPLATE.md gives
  in these words: "While X.Y.Z is in beta, use the packages attached to the
  prerelease you are reading. The install commands below name the final
  release, which does not exist yet." Refused with it are the rewordings that
  say the same thing: that Polaris, a version of it, or the release is still in
  beta or a prerelease ("While the release is in beta", "Until 1.4.14 ships"),
  packages attached to the or this prerelease, and a final or stable release
  that does not exist yet;
* a claim that the release is matched with a Nova release: matched with or to
  Nova, a matched Nova release, a matching Nova release or version, a matched
  pair, or matching or pairing with a named Nova version. Nova is released on
  its own schedule, and nothing at tag time can show that the Nova release
  named is out, which is how 1.4.13 made the claim while Nova 1.4.13 was not.
  A Nova version can still be named for what a feature needs from it, and
  Nova's own beta can be named.

This reads wording, not meaning. A beta line in words it does not know gets
through, which is why the template gives the line exactly and a test reads it
from there.

Prereleases are not checked: the beta line is written for them.

Usage:
    scripts/check-stable-release-notes.py docs/release-notes/vX.Y.Z.md [...]
"""

from pathlib import Path
import re
import sys

VERSION = r"v?\d+\.\d+\.\d+"

# What a beta line calls the release. Nova is left out on purpose: Polaris notes can say that a Nova
# release is still in beta, and the stable 1.4.13 notes do.
SUBJECT = rf"(?:polaris(?:\s+{VERSION})?|{VERSION}|(?:the|this)\s+release)"

BETA = "tells a beta tester what to do while the release is in beta"
PRERELEASE = "sends the reader to the packages of a prerelease"
NOT_YET = "says the release the install commands name does not exist yet"
MATCHED = "calls the release matched with a Nova release, which the tag cannot show is out"

# Each pattern spans line breaks, so rewrapping a paragraph cannot hide a phrase.
REFUSED = tuple(
    (re.compile(pattern, re.IGNORECASE), reason)
    for pattern, reason in (
        (
            rf"\bwhile\s+{SUBJECT}\s+is\s+(?:still\s+)?(?:in\s+beta|a\s+beta|in\s+prerelease|a\s+prerelease)\b",
            BETA,
        ),
        (rf"\buntil\s+{SUBJECT}\s+(?:ships|is\s+(?:out|released|stable)|goes\s+stable)\b", BETA),
        (r"\bthe\s+prerelease\s+you\s+are\s+reading\b", PRERELEASE),
        (r"\battached\s+to\s+(?:the|this)\s+prerelease\b", PRERELEASE),
        # The gap stops at the end of a sentence, but not at the dots inside a version number.
        (
            r"\b(?:final|stable)\s+release\b(?:[^.]|\.(?=\w)){0,60}?\b(?:does\s+not|doesn't)\s+exist\s+yet\b",
            NOT_YET,
        ),
        (r"\bmatched\s+(?:(?:with|to)\s+)?nova\b", MATCHED),
        # Only a matching Nova release or version: the 1.3.8 notes say "Matching Nova client
        # controls are versioned and released independently", which is the opposite claim.
        (rf"\bmatching\s+nova\s+(?:release|version|{VERSION})", MATCHED),
        (r"\bmatched\s+pair\b", MATCHED),
        # Pairing with Nova without a version is how a device pairs, which notes can describe.
        (rf"\b(?:matches|pairs|paired)\s+(?:with\s+)?nova\s+{VERSION}", MATCHED),
    )
)


def findings(text: str) -> list[tuple[int, str, str]]:
    found = []
    for pattern, reason in REFUSED:
        for match in pattern.finditer(text):
            line = text.count("\n", 0, match.start()) + 1
            found.append((line, " ".join(match.group(0).split()), reason))
    return sorted(found)


def main(arguments: list[str]) -> int:
    if not arguments or any(argument.startswith("-") for argument in arguments):
        print("usage: check-stable-release-notes.py NOTES.md [NOTES.md ...]", file=sys.stderr)
        return 2

    failed = False
    for argument in arguments:
        path = Path(argument)
        try:
            text = path.read_text(encoding="utf-8")
        except OSError as error:
            print(f"{path}: cannot read stable release notes: {error.strerror}", file=sys.stderr)
            failed = True
            continue
        for line, phrase, reason in findings(text):
            print(f'{path}:{line}: "{phrase}" {reason}', file=sys.stderr)
            failed = True

    if failed:
        print(
            "check-stable-release-notes: the stable release page is published from these notes as they "
            "are. Take out what was written for the beta, and name a Nova version only for what a "
            "feature needs from it.",
            file=sys.stderr,
        )
        return 1

    print(f"check-stable-release-notes: {len(arguments)} file(s) carry nothing written for a beta")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
