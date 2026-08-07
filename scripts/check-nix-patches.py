#!/usr/bin/env python3
"""Check the vendored nix patch stacks against what the packages declare.

nix/ is the one directory in this repo that no other CI job looks at, so a
change here could land with every check green and nothing having read it. A
malformed hunk header is enough to break the whole stack: nixpkgs' patchPhase
applies the declared list in order and stops at the first failure, so one patch
miscounting its own body by a single line takes every patch after it down too.

Offline (the default) this checks three properties that need neither network
nor nix:

  * every hunk header's declared line counts match the hunk body, so a
    hand-edited patch cannot claim a size it does not have;
  * every patch on disk is either declared by a package or parked under
    archive/, so a superseded patch is not left looking live;
  * every declared patch exists.

With --apply it also fetches each package's pinned upstream source and applies
that package's declared list, in the declared order, with patch(1) -- the same
tool nixpkgs' patchPhase uses. patch(1) rather than `git apply` on purpose:
patch tolerates the line offsets a slightly drifted upstream produces, git
apply refuses them, and it is patch's behaviour the build depends on.

Only the patches this repo vendors are applied. A package may also carry
patches inherited from nixpkgs; resolving those needs nix, so a conflict
between the two is left to the job that actually builds the package.

Usage:
  scripts/check-nix-patches.py            # offline, instant
  scripts/check-nix-patches.py --apply    # also fetch pinned sources and apply
"""

from __future__ import annotations

import argparse
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
PACKAGES_DIR = REPO_ROOT / "nix" / "packages"
PATCHES_DIR = REPO_ROOT / "nix" / "patches"

# Where a package parks a patch it no longer applies. Files here are documented
# history, not dead weight, so they are exempt from the "declared" rule.
ARCHIVE_DIR_NAME = "archive"

HUNK_HEADER = re.compile(r"^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@")
INDEX_LINE = re.compile(r"^index [0-9a-f]+\.\.[0-9a-f]+")
GIT_VERSION = re.compile(r"^\d+(\.\d+)+")
NIX_STRING = r'"([^"]*)"'


class Failure(Exception):
    """A check failed with a message worth printing verbatim."""


# --------------------------------------------------------------------------
# Reading what the packages declare
# --------------------------------------------------------------------------


def declared_patches(nix_source: str) -> list[str]:
    """Patch paths a package declares, in the order it declares them.

    Order is the point: gamescope-polaris applies 10, 11, 12, 02, 03, 06, and
    sorting those by number would apply a stack that has never been tested in
    that shape. Each entry sits alone on its line, so matching whole lines
    keeps a path mentioned in a comment from counting as a declaration.
    """
    paths = []
    for line in nix_source.splitlines():
        stripped = line.strip()
        if stripped.startswith("../../patches/") and stripped.endswith(".patch"):
            paths.append(stripped)
    return paths


def brace_block(source: str, start: int) -> str:
    """The { ... } block beginning at or after `start`, braces balanced."""
    open_brace = source.index("{", start)
    depth = 0
    for index in range(open_brace, len(source)):
        if source[index] == "{":
            depth += 1
        elif source[index] == "}":
            depth -= 1
            if depth == 0:
                return source[open_brace : index + 1]
    raise Failure("unbalanced braces in nix source")


def pinned_source(nix_source: str) -> dict[str, str] | None:
    """The GitHub owner/repo/rev a package pins, or None if it fetches nothing.

    polaris-stream builds from this repo rather than a fetched tarball, so a
    package with no fetchFromGitHub src is normal and not an error.
    """
    marker = "src = fetchFromGitHub"
    if marker not in nix_source:
        return None
    block = brace_block(nix_source, nix_source.index(marker))

    pinned = {}
    for field in ("owner", "repo", "rev"):
        match = re.search(rf"\b{field}\s*=\s*([^;]+);", block)
        if not match:
            raise Failure(f"fetchFromGitHub src has no {field}")
        value = match.group(1).strip()
        literal = re.fullmatch(NIX_STRING, value)
        if literal:
            pinned[field] = literal.group(1)
            continue
        # An identifier instead of a literal: gamescope-polaris hoists its rev
        # into a let-binding so the version comment and the src cannot drift.
        binding = re.search(rf"^\s*{re.escape(value)}\s*=\s*{NIX_STRING};", nix_source, re.M)
        if not binding:
            raise Failure(f"fetchFromGitHub {field} is `{value}`, which is not defined in this file")
        pinned[field] = binding.group(1)
    return pinned


def packages() -> list[tuple[str, Path, str]]:
    """(name, default.nix path, source text) for every nix/packages entry."""
    found = []
    for default_nix in sorted(PACKAGES_DIR.glob("*/default.nix")):
        found.append((default_nix.parent.name, default_nix, default_nix.read_text()))
    if not found:
        raise Failure(f"no packages found under {PACKAGES_DIR}")
    return found


# --------------------------------------------------------------------------
# Structural check: does every hunk header describe its own body?
# --------------------------------------------------------------------------


def strip_format_patch_signature(lines: list[str]) -> list[str]:
    """Drop the `-- \\n<git version>` trailer git format-patch appends.

    It is only two lines, but the first of them starts with '-' and would
    otherwise be counted as a removal in the final hunk.
    """
    end = len(lines)
    while end > 0 and lines[end - 1] == "":
        end -= 1
    if end >= 2 and lines[end - 2] == "-- " and GIT_VERSION.match(lines[end - 1]):
        return lines[: end - 2]
    return lines


def hunk_body_ends(lines: list[str], index: int) -> bool:
    """Whether `lines[index]` is past the end of the hunk body it follows."""
    line = lines[index]
    if line.startswith("diff --git ") or HUNK_HEADER.match(line) or INDEX_LINE.match(line):
        return True
    # A file header, not a removal: only a `--- ` immediately followed by a
    # `+++ ` is one, which keeps a patch that genuinely removes a line reading
    # `-- ` from being cut short here.
    if line.startswith("--- ") and index + 1 < len(lines) and lines[index + 1].startswith("+++ "):
        return True
    return False


def measure_hunk(lines: list[str], start: int) -> tuple[int, int, int]:
    """Count the old and new side of the hunk body starting at `start`.

    Returns (old, new, index of the first line past the body). A wholly empty
    line counts as context: some editors eat the single leading space of a
    blank context line, patch(1) accepts the result, so counting has to too.
    """
    old = new = 0
    index = start
    while index < len(lines):
        if hunk_body_ends(lines, index):
            break
        line = lines[index]
        if line == "" or line[0] == " ":
            old += 1
            new += 1
        elif line[0] == "-":
            old += 1
        elif line[0] == "+":
            new += 1
        elif line[0] == "\\":
            pass  # "\ No newline at end of file" belongs to neither side.
        else:
            break
        index += 1
    return old, new, index


def check_patch_structure(patch_path: Path) -> list[str]:
    """Every hunk header in one patch file, checked against its own body."""
    lines = strip_format_patch_signature(patch_path.read_text().splitlines())
    relative = patch_path.relative_to(REPO_ROOT)
    problems = []
    hunks = 0

    index = 0
    while index < len(lines):
        header = HUNK_HEADER.match(lines[index])
        if not header:
            index += 1
            continue
        hunks += 1
        header_line = index + 1  # 1-based, for an error a reader can jump to
        # An omitted count means one line, per the unified diff format.
        declared_old = int(header.group(2)) if header.group(2) is not None else 1
        declared_new = int(header.group(4)) if header.group(4) is not None else 1
        actual_old, actual_new, index = measure_hunk(lines, index + 1)

        if (declared_old, declared_new) != (actual_old, actual_new):
            problems.append(
                f"{relative}:{header_line}: `{header.group(0)}` claims "
                f"{declared_old} old / {declared_new} new lines, "
                f"body has {actual_old} old / {actual_new} new"
            )

    if hunks == 0:
        problems.append(f"{relative}: no hunks found -- is this a patch file?")
    return problems


# --------------------------------------------------------------------------
# Applying the stacks against pinned upstream sources
# --------------------------------------------------------------------------


def fetch_source(pinned: dict[str, str], into: Path) -> Path:
    """Download and extract a pinned GitHub revision. Returns the source root."""
    url = f"https://codeload.github.com/{pinned['owner']}/{pinned['repo']}/tar.gz/{pinned['rev']}"
    tarball = into / "source.tar.gz"
    print(f"    fetching {pinned['owner']}/{pinned['repo']}@{pinned['rev'][:12]}")
    with urllib.request.urlopen(url, timeout=120) as response, tarball.open("wb") as out:
        shutil.copyfileobj(response, out)
    with tarfile.open(tarball) as archive:
        archive.extractall(into, filter="data")
    roots = [entry for entry in into.iterdir() if entry.is_dir()]
    if len(roots) != 1:
        raise Failure(f"expected one directory in the {pinned['repo']} tarball, found {len(roots)}")
    return roots[0]


def apply_stack(source_root: Path, patch_paths: list[Path]) -> list[str]:
    """Apply patches in order, reporting the first that does not apply."""
    problems = []
    for patch_path in patch_paths:
        relative = patch_path.relative_to(REPO_ROOT)
        # --batch so a patch that cannot find its target fails instead of
        # prompting for a filename and hanging a CI job forever.
        result = subprocess.run(
            ["patch", "-p1", "--batch", "--no-backup-if-mismatch", "-i", str(patch_path)],
            cwd=source_root,
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            output = (result.stdout + result.stderr).strip()
            problems.append(f"{relative}: does not apply\n{indent(output)}")
            problems.append(
                "        every later patch in this stack is untested: patchPhase "
                "stops at the first failure"
            )
            break
        print(f"    applied {relative.name}")
    return problems


def indent(text: str, prefix: str = "        ") -> str:
    return "\n".join(prefix + line for line in text.splitlines())


# --------------------------------------------------------------------------
# Checks
# --------------------------------------------------------------------------


def check_declarations(all_declared: dict[str, list[str]]) -> list[str]:
    """Patches on disk against patches packages declare, in both directions."""
    problems = []
    declared_paths = {
        (REPO_ROOT / "nix" / "packages" / package / declared).resolve()
        for package, paths in all_declared.items()
        for declared in paths
    }

    for declared in sorted(declared_paths):
        if not declared.exists():
            problems.append(f"{declared.relative_to(REPO_ROOT)}: declared by a package but not on disk")

    for on_disk in sorted(PATCHES_DIR.rglob("*.patch")):
        if ARCHIVE_DIR_NAME in on_disk.relative_to(PATCHES_DIR).parts:
            continue
        if on_disk.resolve() not in declared_paths:
            problems.append(
                f"{on_disk.relative_to(REPO_ROOT)}: on disk but no package applies it "
                f"-- move it to {ARCHIVE_DIR_NAME}/ or declare it"
            )
    return problems


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument(
        "--apply",
        action="store_true",
        help="also fetch each pinned upstream source and apply its stack",
    )
    args = parser.parse_args()

    problems: list[str] = []
    all_declared: dict[str, list[str]] = {}

    for name, default_nix, source in packages():
        try:
            all_declared[name] = declared_patches(source)
        except Failure as failure:
            problems.append(f"{default_nix.relative_to(REPO_ROOT)}: {failure}")

    print("Checking patch declarations")
    problems += check_declarations(all_declared)

    print("Checking hunk headers against hunk bodies")
    patch_files = sorted(PATCHES_DIR.rglob("*.patch"))
    if not patch_files:
        problems.append(f"{PATCHES_DIR.relative_to(REPO_ROOT)}: no patches found")
    for patch_path in patch_files:
        problems += check_patch_structure(patch_path)
    print(f"  {len(patch_files)} patch files")

    if args.apply:
        for name, default_nix, source in packages():
            declared = all_declared.get(name) or []
            if not declared:
                continue
            print(f"Applying the {name} stack")
            try:
                pinned = pinned_source(source)
            except Failure as failure:
                problems.append(f"{default_nix.relative_to(REPO_ROOT)}: {failure}")
                continue
            if pinned is None:
                print(f"    {name} pins no upstream source, nothing to apply against")
                continue
            patch_paths = [(default_nix.parent / declared_path).resolve() for declared_path in declared]
            missing = [path for path in patch_paths if not path.exists()]
            if missing:
                continue  # already reported by the declaration check
            with tempfile.TemporaryDirectory(prefix="polaris-nix-patches-") as workdir:
                try:
                    source_root = fetch_source(pinned, Path(workdir))
                except Failure as failure:
                    problems.append(f"{name}: {failure}")
                    continue
                problems += apply_stack(source_root, patch_paths)

    if problems:
        print("\nFAIL", file=sys.stderr)
        for problem in problems:
            print(f"  {problem}", file=sys.stderr)
        return 1

    print("\nOK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
