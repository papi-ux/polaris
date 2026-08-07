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
ACCESSORS = "optString|optBoolean|optInt|optLong|optDouble|optJSONObject|optJSONArray|getString|getBoolean|getInt|getLong"


def function_body(source: str, name: str) -> str:
    """The body of one Kotlin function, up to the next declaration at its level.

    Scope has to be the function, and getting this wrong has already produced two
    rounds of false findings. A file-wide scan reports the nested launch_mode and
    mode fields as game fields. Scoping by receiver name is no better here,
    because PolarisGameJsonAdapter.kt gives four different functions a parameter
    called `json` — so artwork manifest and asset fields land in the game set and
    the tool reports drift that does not exist.
    """
    start = re.search(rf'^\s*(?:internal |private |public )?fun {re.escape(name)}\(', source, re.M)
    if not start:
        raise SystemExit(f"function not found: {name}")
    rest = source[start.end():]
    nxt = re.search(r'^\s*(?:internal |private |public )?fun \w+\(', rest, re.M)
    return rest[: nxt.start()] if nxt else rest


def reads_for_object(source: str, reader: dict) -> set[str]:
    """Keys read for one contract object."""
    body = function_body(source, reader["function"])
    pattern = re.compile(rf'\b{re.escape(reader["receiver"])}\.(?:{ACCESSORS})\(\s*"([a-zA-Z_0-9]+)"')
    return set(pattern.findall(body))


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
    game = manifest["objects"]["game"]
    served = set(game["fields"])
    read = nova_reads(args.nova, game["nova_reader"])

    drift = manifest.get("known_drift", {})
    known_missing = set(drift.get("nova_reads_polaris_never_sends", {}).get("game", []))
    known_unread = set(drift.get("polaris_sends_nova_never_reads", {}).get("game", []))

    # Nova asks, Polaris never answers. Nova defaults these, so the symptom is a
    # blank or "unknown" in the UI rather than an error.
    missing = read - served
    # Polaris answers, Nova never asks. The feature ships and stays invisible.
    unread = served - read

    new_missing = sorted(missing - known_missing)
    new_unread = sorted(unread - known_unread)
    fixed = sorted((known_missing - missing) | (known_unread - unread))

    print(f"game object: Polaris serves {len(served)}, Nova reads {len(read)}")

    for field in new_missing:
        print(f"  DRIFT  Nova reads [{field}] that Polaris never serves")
    for field in new_unread:
        print(f"  DRIFT  Polaris serves [{field}] that Nova never reads")
    for field in fixed:
        print(f"  FIXED  [{field}] is no longer drifting; remove it from known_drift")

    if not (new_missing or new_unread or fixed):
        print("  no new drift")

    if missing & known_missing or unread & known_unread:
        print(f"\n{len(missing & known_missing) + len(unread & known_unread)} known drift field(s) still outstanding")

    return 1 if (new_missing or new_unread) else 0


if __name__ == "__main__":
    sys.exit(main())
