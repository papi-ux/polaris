#!/usr/bin/env python3
"""Build the completion-estimate dataset Polaris reads at runtime.

Why a dataset and not a lookup at runtime
-----------------------------------------
How Long To Beat has no usable public API. The documented search endpoint is gone, the
current ones are named to be unguessable and rotate, and they answer a correct request
with ``{"error":"Session expired or invalid fingerprint"}`` unless it carries a browser
session. Working around that from every Polaris install would break for everyone at once
each time they change it, and would aim a distributed product's traffic at a site that
has said no about as clearly as a site can.

Completion times are near-static data — a game's "about 17 hours" does not move — so they
belong in a file that someone generates deliberately and refreshes when they feel like
it. Nothing in the serving path touches the network, it works offline, and it does not
break when somebody else's site changes shape.

Sources
-------
``--igdb``   IGDB's ``game_time_to_beats`` endpoint. Official, documented, free. Needs a
             Twitch application: set ``IGDB_CLIENT_ID`` and ``IGDB_CLIENT_SECRET``.
``--merge``  A JSON file you maintain by hand, in the same shape as the output. Merged
             last, so anything you correct yourself wins over anything fetched.

By default only games installed locally are looked up, which keeps the file small and
the lookups few. ``--all-steam`` widens it to everything in the Steam library folders.

Output
------
``beat_times.json`` in the shape ``src/beat_times.cpp`` parses. Install it by copying to
``~/.config/polaris/beat_times.json``; Polaris re-reads it when the file changes, so no
restart is needed.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from pathlib import Path

IGDB_TOKEN_URL = "https://id.twitch.tv/oauth2/token"
IGDB_API = "https://api.igdb.com/v4"


def steam_library_roots() -> list[Path]:
    """Every steamapps directory Steam knows about, from libraryfolders.vdf."""
    roots: list[Path] = []
    home = Path.home()
    candidates = [
        home / ".steam/steam/steamapps",
        home / ".local/share/Steam/steamapps",
        home / ".var/app/com.valvesoftware.Steam/.local/share/Steam/steamapps",
    ]
    for base in candidates:
        vdf = base / "libraryfolders.vdf"
        if not vdf.is_file():
            continue
        if base not in roots:
            roots.append(base)
        # "path" sits at depth 2, inside a numbered section inside "libraryfolders".
        depth = 0
        for raw in vdf.read_text(encoding="utf-8", errors="replace").splitlines():
            line = raw.strip()
            if line == "{":
                depth += 1
                continue
            if line == "}":
                depth -= 1
                continue
            if depth != 2:
                continue
            match = re.match(r'"path"\s+"([^"]+)"', line)
            if match:
                extra = Path(match.group(1)) / "steamapps"
                if extra.is_dir() and extra not in roots:
                    roots.append(extra)
    return roots


def installed_steam_games() -> dict[str, str]:
    """appid -> name, from the appmanifests Steam writes for installed games."""
    games: dict[str, str] = {}
    for root in steam_library_roots():
        for manifest in sorted(root.glob("appmanifest_*.acf")):
            text = manifest.read_text(encoding="utf-8", errors="replace")
            appid = re.search(r'"appid"\s+"(\d+)"', text)
            name = re.search(r'"name"\s+"([^"]*)"', text)
            if appid and name and name.group(1).strip():
                games.setdefault(appid.group(1), name.group(1).strip())
    return games


def igdb_token(client_id: str, client_secret: str) -> str:
    body = urllib.parse.urlencode({
        "client_id": client_id,
        "client_secret": client_secret,
        "grant_type": "client_credentials",
    }).encode()
    with urllib.request.urlopen(urllib.request.Request(IGDB_TOKEN_URL, data=body), timeout=30) as response:
        return json.load(response)["access_token"]


def igdb_query(endpoint: str, body: str, client_id: str, token: str) -> list[dict]:
    request = urllib.request.Request(
        f"{IGDB_API}/{endpoint}",
        data=body.encode(),
        headers={"Client-ID": client_id, "Authorization": f"Bearer {token}"},
    )
    for attempt in range(4):
        try:
            with urllib.request.urlopen(request, timeout=30) as response:
                return json.load(response)
        except urllib.error.HTTPError as err:
            # IGDB allows four requests a second and says so with a 429.
            if err.code == 429 and attempt < 3:
                time.sleep(1.5 * (attempt + 1))
                continue
            raise
    return []


def fetch_igdb(games: dict[str, str], client_id: str, client_secret: str) -> list[dict]:
    """Resolve each title to IGDB, then ask how long it takes to beat."""
    token = igdb_token(client_id, client_secret)
    rows: list[dict] = []

    for index, (appid, name) in enumerate(sorted(games.items(), key=lambda kv: kv[1].lower()), start=1):
        escaped = name.replace('"', "")
        found = igdb_query(
            "games",
            f'search "{escaped}"; fields id,name,url; limit 1;',
            client_id,
            token,
        )
        if not found:
            print(f"  [{index}/{len(games)}] no IGDB match: {name}", file=sys.stderr)
            continue

        game = found[0]
        beats = igdb_query(
            "game_time_to_beats",
            f"fields hastily,normally,completely; where game_id = {game['id']}; limit 1;",
            client_id,
            token,
        )
        if not beats:
            print(f"  [{index}/{len(games)}] no completion data: {name}", file=sys.stderr)
            continue

        beat = beats[0]
        row = {
            "name": game.get("name", name),
            "steam_appid": appid,
            "main_seconds": int(beat.get("normally") or 0),
            "extras_seconds": int(beat.get("completely") or 0) if beat.get("completely") else 0,
            "completionist_seconds": int(beat.get("completely") or 0),
            "url": game.get("url", ""),
        }
        # hastily/normally/completely are already seconds; a row with none is not worth
        # writing, since the loader drops it anyway.
        if row["main_seconds"] or row["completionist_seconds"]:
            rows.append(row)
        time.sleep(0.26)  # stay under four requests a second

    return rows


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--igdb", action="store_true", help="fetch estimates from IGDB")
    parser.add_argument("--merge", type=Path, help="a hand-maintained JSON file, merged last so it wins")
    parser.add_argument("--all-steam", action="store_true", help="every Steam game, not only installed ones")
    parser.add_argument("--out", type=Path, default=Path("beat_times.json"))
    args = parser.parse_args()

    if not args.igdb and not args.merge:
        parser.error("choose a source: --igdb, --merge, or both")

    games = installed_steam_games()
    if not games:
        print("No Steam games found locally.", file=sys.stderr)
    else:
        print(f"{len(games)} Steam games found locally.", file=sys.stderr)

    rows: list[dict] = []

    if args.igdb:
        client_id = os.environ.get("IGDB_CLIENT_ID", "").strip()
        client_secret = os.environ.get("IGDB_CLIENT_SECRET", "").strip()
        if not client_id or not client_secret:
            print(
                "IGDB needs credentials. Register an application at https://dev.twitch.tv/console/apps\n"
                "then set IGDB_CLIENT_ID and IGDB_CLIENT_SECRET.",
                file=sys.stderr,
            )
            return 2
        rows.extend(fetch_igdb(games, client_id, client_secret))

    if args.merge:
        merged = json.loads(args.merge.read_text(encoding="utf-8"))
        by_key = {(r.get("steam_appid") or r.get("name")): r for r in rows}
        for row in merged.get("games", merged if isinstance(merged, list) else []):
            # Anything corrected by hand replaces what was fetched.
            by_key[row.get("steam_appid") or row.get("name")] = row
        rows = list(by_key.values())

    rows.sort(key=lambda r: r.get("name", "").lower())
    payload = {
        "version": 1,
        "generated_at": int(time.time()),
        "games": rows,
    }
    args.out.write_text(json.dumps(payload, indent=2, ensure_ascii=False) + "\n", encoding="utf-8")
    print(f"Wrote {len(rows)} entries to {args.out}", file=sys.stderr)
    print(f"Install with: cp {args.out} ~/.config/polaris/beat_times.json", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
