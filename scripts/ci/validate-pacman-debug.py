#!/usr/bin/env python3
"""Check that the debug archive belongs to the host and KMS packages being released."""

import argparse
import re
import subprocess
import tempfile
from pathlib import Path


def package_info(path, expected_name):
    text = subprocess.check_output(["tar", "-xOf", str(path), ".PKGINFO"], text=True)
    fields = {}
    for key in ("pkgname", "pkgver", "arch"):
        values = [line[len(key) + 3:] for line in text.splitlines() if line.startswith(key + " = ")]
        if len(values) != 1 or not values[0]:
            raise ValueError(f"{expected_name}: expected one {key} field")
        fields[key] = values[0]
    if fields["pkgname"] != expected_name or fields["arch"] != "x86_64":
        raise ValueError(f"expected {expected_name} for x86_64")
    return fields


def members(path):
    return subprocess.check_output(["tar", "-tf", str(path)], text=True).splitlines()


def build_id(archive, member, destination, *, require_symbols=False):
    # Read one exact member into a private file; never extract archive paths.
    with destination.open("wb") as output:
        subprocess.run(["tar", "-xOf", str(archive), member], stdout=output, check=True)
    # Detached symbols can retain an empty PT_INTERP, which readelf diagnoses on
    # stderr even when note parsing succeeds. Preserve failures, not that warning.
    result = subprocess.run(["readelf", "--wide", "--section-headers", "--notes", str(destination)],
                            text=True, capture_output=True, check=True)
    notes = result.stdout
    if require_symbols and not re.search(r"\]\s+\.(?:debug|zdebug)_info\s+PROGBITS\s", notes):
        raise ValueError("host's detached symbols contain no DWARF debug information")
    ids = set(re.findall(r"Build ID: ([0-9a-f]+)", notes))
    if len(ids) != 1:
        raise ValueError("expected one ELF build ID")
    return ids.pop()


def validate(main, kms, debug):
    main_info = package_info(main, "polaris")
    kms_info = package_info(kms, "polaris-kms")
    debug_info = package_info(debug, "polaris-debug")
    if len({info["pkgver"] for info in (main_info, kms_info, debug_info)}) != 1:
        raise ValueError("main, KMS and debug package versions differ")

    hosts = [name for name in members(main) if re.fullmatch(r"(?:\./)?usr/bin/polaris-[0-9][^/]*", name)]
    if len(hosts) != 1:
        raise ValueError("expected one versioned host binary in polaris")
    host = hosts[0].removeprefix("./")
    symbol_path = f"usr/lib/debug/{host}.debug"
    symbols = [name for name in members(debug) if name.removeprefix("./") == symbol_path]
    if len(symbols) != 1:
        raise ValueError("debug package is missing the host's detached symbols")
    helpers = [name for name in members(kms) if name.removeprefix("./") == "usr/libexec/polaris/polaris-kms"]
    if len(helpers) != 1:
        raise ValueError("expected one KMS helper binary")

    with tempfile.TemporaryDirectory(prefix="polaris-debug-check-") as temporary:
        directory = Path(temporary)
        host_id = build_id(main, hosts[0], directory / "host")
        symbol_id = build_id(debug, symbols[0], directory / "symbols", require_symbols=True)
        kms_id = build_id(kms, helpers[0], directory / "kms")
    if host_id != symbol_id or host_id != kms_id:
        raise ValueError("host, KMS helper and detached symbols have different ELF build IDs")
    return main_info["pkgver"], host_id


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--main", required=True, type=Path)
    parser.add_argument("--kms", required=True, type=Path)
    parser.add_argument("--debug", required=True, type=Path)
    args = parser.parse_args()
    try:
        version, identity = validate(args.main, args.kms, args.debug)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Debug package validation failed: {error}\n")
    print(f"polaris-debug {version}: host/KMS ELF build ID {identity} matches")


if __name__ == "__main__":
    main()
