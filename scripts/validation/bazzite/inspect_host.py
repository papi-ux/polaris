#!/usr/bin/env python3
"""Read-only Bazzite baseline collection; never an installation or readiness gate."""

from __future__ import annotations

import argparse
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path
import pwd
import re
import stat
import subprocess
import sys


SERVICE_PROPERTIES = (
    "LoadState", "ActiveState", "SubState", "MainPID", "InvocationID",
    "FragmentPath", "DropInPaths", "UnitFileState",
)
IDENTITY_PROPERTIES = ("MainPID", "InvocationID", "ActiveState", "SubState")
DEPLOYMENT_FIELDS = (
    "booted", "staged", "checksum", "base-checksum", "version",
    "container-image-reference", "container-image-reference-digest",
    "requested-local-packages", "requested-packages", "pinned",
)
SAFE_ENV = {"PATH": "/usr/sbin:/usr/bin:/sbin:/bin", "LC_ALL": "C"}


def command(argv: list[str]) -> dict:
    try:
        result = subprocess.run(
            argv, capture_output=True, text=True, timeout=30, env=SAFE_ENV,
            check=False,
        )
        return {"returncode": result.returncode, "stdout": result.stdout}
    except (OSError, subprocess.TimeoutExpired) as error:
        # Command arguments, environments and stderr can contain private data.
        return {"returncode": None, "error": type(error).__name__, "stdout": ""}


def service_command(uid: int) -> list[str]:
    if uid <= 0 or (os.geteuid() != 0 and uid != os.geteuid()):
        raise ValueError("Select the normal desktop user's UID, not root or another user")
    argv = ["systemctl", "--user", "show", "polaris.service"]
    argv += ["--property=" + name for name in SERVICE_PROPERTIES]
    argv = [
        "env", "-i", "PATH=" + SAFE_ENV["PATH"], "LC_ALL=C",
        f"XDG_RUNTIME_DIR=/run/user/{uid}",
        f"DBUS_SESSION_BUS_ADDRESS=unix:path=/run/user/{uid}/bus", *argv,
    ]
    if os.geteuid() == 0:
        argv = ["runuser", "--user", pwd.getpwuid(uid).pw_name, "--", *argv]
    return argv


def service_state(uid: int) -> dict:
    result = command(service_command(uid))
    if result["returncode"] != 0:
        return {"available": False, "error": result.get("error", "query_failed")}
    fields = dict(line.split("=", 1) for line in result["stdout"].splitlines() if "=" in line)
    return {"available": True, **{name: fields.get(name, "") for name in SERVICE_PROPERTIES}}


def file_identity(path: Path, *, no_follow: bool = False) -> dict:
    try:
        flags = os.O_RDONLY | os.O_NONBLOCK | os.O_CLOEXEC
        if no_follow:
            flags |= os.O_NOFOLLOW
        fd = os.open(path, flags)
        try:
            before = os.fstat(fd)
            if not stat.S_ISREG(before.st_mode) or before.st_size > 512 * 1024 * 1024:
                return {"available": False, "error": "not_a_bounded_regular_file"}
            digest = hashlib.sha256()
            size = 0
            while chunk := os.read(fd, 1024 * 1024):
                size += len(chunk)
                if size > before.st_size:
                    return {"available": False, "error": "file_changed"}
                digest.update(chunk)
            after = os.fstat(fd)
            fields = ("st_dev", "st_ino", "st_size", "st_mtime_ns", "st_ctime_ns")
            if size != before.st_size or any(getattr(before, x) != getattr(after, x) for x in fields):
                return {"available": False, "error": "file_changed"}
            return {"available": True, "sha256": digest.hexdigest(), "bytes": size}
        finally:
            os.close(fd)
    except OSError as error:
        return {"available": False, "error": type(error).__name__}


def process_identity(pid: int, proc_root: Path = Path("/proc")) -> dict:
    if pid <= 0:
        return {"available": False, "error": "no_running_process"}
    try:
        root = proc_root / str(pid)
        before = (root / "stat").read_text().rsplit(")", 1)[1].split()[19]
        fields = dict(line.split(":", 1) for line in (root / "status").read_text().splitlines() if ":" in line)
        credentials = {name: list(map(int, fields[name].split())) for name in ("Uid", "Gid", "Groups")}
        executable = file_identity(root / "exe")
        try:
            executable["path"] = os.readlink(root / "exe")
        except OSError:
            pass
        after = (root / "stat").read_text().rsplit(")", 1)[1].split()[19]
        if before != after:
            return {"available": False, "error": "process_changed"}
        return {
            "available": True, "pid": pid, "start_ticks": before,
            "credentials": credentials, "executable": executable,
            "capabilities": {name: fields.get(name, "").strip() for name in ("CapEff", "CapPrm", "CapAmb", "NoNewPrivs")},
        }
    except (OSError, KeyError, IndexError, ValueError) as error:
        return {"available": False, "error": type(error).__name__}


def device_metadata(path: Path) -> dict:
    try:
        info = path.lstat()
        return {
            "available": True, "character_device": stat.S_ISCHR(info.st_mode),
            "mode": oct(stat.S_IMODE(info.st_mode)), "uid": info.st_uid, "gid": info.st_gid,
        }
    except OSError as error:
        return {"available": False, "error": type(error).__name__}


def deployment_summary(result: dict) -> dict:
    if result["returncode"] != 0:
        return {"available": False, "error": result.get("error", "query_failed")}
    try:
        data = json.loads(result["stdout"])
        deployments = data["deployments"]
        if not isinstance(deployments, list) or not all(isinstance(d, dict) for d in deployments):
            raise ValueError("Invalid deployment list")
        return {
            "available": True,
            "deployments": [{key: d.get(key) for key in DEPLOYMENT_FIELDS} for d in deployments],
            "transaction_active": data.get("transaction") is not None,
        }
    except (ValueError, KeyError, TypeError):
        return {"available": False, "error": "invalid_deployment_response"}


def assess(before: dict, after: dict, process: dict, uid: int, installed: dict, expected: str | None) -> dict:
    stable = (
        before.get("available") is True and after.get("available") is True
        and all(before.get(k) == after.get(k) for k in IDENTITY_PROPERTIES)
        and before.get("ActiveState") == "active" and before.get("SubState") == "running"
        and re.fullmatch("[0-9a-f]{32}", before.get("InvocationID", "")) is not None
        and process.get("available") is True
        and str(process.get("pid")) == before.get("MainPID")
        and process.get("credentials", {}).get("Uid") == [uid] * 4
    )
    runtime = process.get("executable", {})
    runtime_hash = runtime.get("sha256") if stable and runtime.get("available") else None
    installed_hash = installed.get("sha256") if installed.get("available") else None
    return {
        "service_snapshot": "consistent" if stable else "unavailable_or_changed",
        "runtime_matches_expected": (
            "not_requested" if expected is None else
            "unavailable" if runtime_hash is None else
            "match" if runtime_hash == expected else "mismatch"
        ),
        "usr_bin_matches_running_executable": (
            "unavailable" if runtime_hash is None or installed_hash is None else
            "match" if runtime_hash == installed_hash else "mismatch"
        ),
        "input_access": "not_tested",
        "installed_lifecycle": "not_tested",
        "streaming": "not_tested",
        "scope": "Observation only; matching hashes and device metadata do not establish readiness.",
    }


def collect(uid: int, expected: str | None) -> dict:
    before = service_state(uid)
    pid = int(before.get("MainPID") or "0")
    process = process_identity(pid)
    installed = file_identity(Path("/usr/bin/polaris"))
    deployments = deployment_summary(command(["rpm-ostree", "status", "--json"]))
    package = command(["rpm", "-q", "--qf", "%{NAME}-%{EPOCHNUM}:%{VERSION}-%{RELEASE}.%{ARCH}\n", "polaris"])
    selinux = command(["getenforce"])
    extension = Path("/var/lib/extensions/polaris.raw")
    process_after = process_identity(pid)
    after = service_state(uid)
    process_stable = (
        process.get("available") is True and process_after.get("available") is True
        and all(process.get(k) == process_after.get(k) for k in ("pid", "start_ticks", "credentials", "capabilities", "executable"))
    )
    assessment = assess(before, after, process, uid, installed, expected)
    if not process_stable:
        assessment["service_snapshot"] = "unavailable_or_changed"
        assessment["runtime_matches_expected"] = "unavailable" if expected else "not_requested"
        assessment["usr_bin_matches_running_executable"] = "unavailable"
    return {
        "schema_version": 1, "kind": "polaris-bazzite-host-observation",
        "captured_at_utc": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "collector_uid": os.geteuid(), "service_uid": uid,
        "boot_id": Path("/proc/sys/kernel/random/boot_id").read_text().strip(),
        "service_before": before, "service_after": after,
        "process": process, "process_after": process_after,
        "usr_bin_polaris": installed, "deployments": deployments,
        "installed_rpm": package["stdout"].strip() if package["returncode"] == 0 else None,
        "selinux": selinux["stdout"].strip() if selinux["returncode"] == 0 else None,
        "persistent_polaris_extension": file_identity(extension, no_follow=True),
        "input_nodes": {str(p): device_metadata(p) for p in (Path("/dev/uinput"), Path("/dev/uhid"))},
        "expected_executable_sha256": expected,
        "assessment": assessment,
    }


def write_observation(path: Path, uid: int, result: dict) -> None:
    data = (json.dumps(result, indent=2) + "\n").encode()
    parent_fd = os.open(path.parent, os.O_RDONLY | os.O_DIRECTORY | os.O_NOFOLLOW)
    try:
        info = os.fstat(parent_fd)
        if info.st_uid not in {os.geteuid(), uid} or info.st_mode & 0o077:
            raise ValueError("Output parent must be a private directory owned by the collector or selected user")
        fd = os.open(path.name, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600, dir_fd=parent_fd)
        with os.fdopen(fd, "wb") as output:
            if os.geteuid() == 0:
                os.fchown(output.fileno(), uid, pwd.getpwuid(uid).pw_gid)
            output.write(data)
            output.flush()
            os.fsync(output.fileno())
    finally:
        os.close(parent_fd)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--user-uid", type=int, default=os.geteuid())
    parser.add_argument("--expected-executable-sha256")
    parser.add_argument("--output", type=Path, required=True, help="New private observation file; existing files are never replaced")
    args = parser.parse_args()
    if args.expected_executable_sha256 and not re.fullmatch("[0-9a-f]{64}", args.expected_executable_sha256):
        parser.error("Expected executable SHA256 must be 64 lowercase hexadecimal characters")
    service_command(args.user_uid)  # Validate the selected user before collection.
    result = collect(args.user_uid, args.expected_executable_sha256)
    write_observation(args.output, args.user_uid, result)
    print(json.dumps(result["assessment"]))
    # A successful observation is deliberately not a successful acceptance receipt.
    return 0 if result["assessment"]["service_snapshot"] == "consistent" else 2


if __name__ == "__main__":
    sys.exit(main())
