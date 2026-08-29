#!/usr/bin/env python3
"""Diff Polaris' served fields against what a Nova checkout actually reads.

Nova and Polaris ship from separate repositories, so neither build can see the
other. Both address the same JSON by string literal, and both sides fail soft:
Nova defaults a field it cannot find, Polaris never learns that something went
unread. The result is a feature that quietly stops working instead of breaking.

This is the half that needs both trees present, so it is a developer tool rather
than a CI gate. tests/integration/test_nova_contract.cpp covers the half that can
run inside Polaris alone.

    scripts/check-nova-contract.py --nova ~/Documents/github/nova

Exits non-zero when drift is found that is not already recorded under
`known_drift` in docs/nova-contract.json, so it is usable as a gate once a repo
has both checkouts.
"""

from __future__ import annotations

import argparse
import json
import re
import sys
from pathlib import Path

# Nova reads responses through org.json accessors. Anything it asks for by name
# is a field it expects Polaris to serve.
ACCESSORS = "optString|optBoolean|optInt|optLong|optDouble|optJSONObject|optJSONArray|opt|getString|getBoolean|getInt|getLong|has|isNull"


def function_body(source: str, name: str) -> str:
    """The body of one Kotlin function, up to the next declaration at its level.

    Scope has to be the function, and getting this wrong has already produced two
    rounds of false findings. A file-wide scan reports the nested launch_mode and
    mode fields as game fields. Scoping by receiver name is no better here,
    because PolarisGameJsonAdapter.kt gives four different functions a parameter
    called `json` — so artwork manifest and asset fields land in the game set and
    the tool reports drift that does not exist.
    """
    start = re.search(
        rf'^([ \t]*)(?:internal |private |public )?fun {re.escape(name)}\(',
        source,
        re.M,
    )
    if not start:
        raise SystemExit(f"function not found: {name}")
    rest = source[start.end():]
    # Local helper functions are deeper-indented and remain part of the reader
    # scope. Only a sibling declaration at the exact same indentation closes
    # the method whose JSON reads we are auditing.
    indent = re.escape(start.group(1))
    nxt = re.search(
        rf'^{indent}(?:internal |private |public )?fun \w+\(',
        rest,
        re.M,
    )
    return rest[: nxt.start()] if nxt else rest


def reads_for_object(source: str, reader: dict) -> set[str]:
    """Keys read for one contract object."""
    body = function_body(source, reader["function"])
    # `?.` as well as `.`: Nova reaches nullable sub-objects with the safe-call
    # operator, and requiring a bare dot silently reports every one of their
    # fields as unread.
    pattern = re.compile(
        rf'\b{re.escape(reader["receiver"])}\??\.(?:{ACCESSORS})\(\s*"([a-zA-Z_0-9]+)"'
    )
    # PolarisGameJsonAdapter routes every double through its finiteDouble
    # helper, so optDouble never appears against the receiver and the accessor
    # scan above reports those fields as unread. The helper call still names
    # the receiver and the literal key, so count it as the read it is.
    helper = re.compile(
        rf'\bfiniteDouble\(\s*{re.escape(reader["receiver"])}\s*,\s*"([a-zA-Z_0-9]+)"'
    )
    return set(pattern.findall(body)) | set(helper.findall(body))


def polaris_manifest(repo: Path) -> dict:
    return json.loads((repo / "docs" / "nova-contract.json").read_text())


def nova_reads(nova: Path, reader: dict) -> set[str]:
    """Fields Nova reads for one contract object."""
    path = nova / reader["file"]
    if not path.is_file():
        raise SystemExit(f"not found: {path}\nIs --nova pointing at a Nova checkout?")
    return reads_for_object(path.read_text(), reader)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--nova", required=True, type=Path, help="path to a Nova checkout")
    parser.add_argument("--polaris", type=Path, default=Path(__file__).resolve().parent.parent)
    args = parser.parse_args()

    manifest = polaris_manifest(args.polaris)
    drift = manifest.get("known_drift", {})
    known_missing = drift.get("nova_reads_polaris_never_sends", {})
    known_unread = drift.get("polaris_sends_nova_never_reads", {})

    sources: dict[Path, str] = {}
    new_findings = 0

    for name, obj in manifest["objects"].items():
        reader = obj.get("nova_reader")
        if reader is None:
            # Shipped ahead of any Nova consumer (display_planner did this
            # before papi-ux/nova#197; session_timing does it now). Nothing to
            # diff against yet, so report it and move on rather than treating
            # absence as an error.
            served = set(obj["fields"])
            print(f"{name}: Polaris serves {len(served)}, no Nova reader yet [not yet consumed]")
            continue
        path = args.nova / reader["file"]
        if not path.is_file():
            raise SystemExit(f"not found: {path}\nIs --nova pointing at a Nova checkout?")
        if path not in sources:
            sources[path] = path.read_text()

        served = set(obj["fields"])
        read = reads_for_object(sources[path], reader)

        # Not every served field has to be consumed. A host may publish diagnostics a
        # client has no use for, so only fields absent from informational_ok count.
        informational = set(obj.get("informational_ok", []))

        missing = read - served
        unread = served - read - informational

        fresh_missing = sorted(missing - set(known_missing.get(name, [])))
        fresh_unread = sorted(unread - set(known_unread.get(name, [])))
        outstanding = len(missing & set(known_missing.get(name, []))) + len(
            unread & set(known_unread.get(name, []))
        )

        status = "drift" if (fresh_missing or fresh_unread) else "ok"
        print(f"{name}: Polaris serves {len(served)}, Nova reads {len(read)} [{status}]")
        for field in fresh_missing:
            print(f"    DRIFT  Nova reads [{field}] that Polaris never serves")
        for field in fresh_unread:
            print(f"    DRIFT  Polaris serves [{field}] that Nova never reads")
        if outstanding:
            print(f"    {outstanding} known drift field(s) still outstanding")

        new_findings += len(fresh_missing) + len(fresh_unread)

    print()
    print("no new drift" if not new_findings else f"{new_findings} new drift field(s)")
    return 1 if new_findings else 0


if __name__ == "__main__":
    sys.exit(main())
