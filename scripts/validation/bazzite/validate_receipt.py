#!/usr/bin/env python3
"""Fail-closed validator for Polaris Bazzite lifecycle evidence receipts."""

from __future__ import annotations

import hashlib
import json
import os
import re
import stat
import sys
from datetime import datetime
from pathlib import Path, PurePosixPath
from typing import Any
from urllib.parse import urlsplit

REQUIRED_TARGETS = {
    "bazzite": "amd-intel",
    "bazzite-nvidia-open": "nvidia-open",
}
REQUIRED_PCI_VENDOR_DRIVERS = {
    "bazzite": {
        "1002": {"amdgpu"},
        "8086": {"i915", "xe"},
    },
    "bazzite-nvidia-open": {
        "10de": {"nvidia"},
    },
}
REQUIRED_RESULTS = (
    "artifact_identity",
    "repo_resolution",
    "elf_closure",
    "selinux_enforcing",
    "selinux_avc_clean",
    "install_failure_cleanup",
    "setup_failure_cleanup",
    "install",
    "first_boot",
    "second_boot",
    "removal",
    "deployment_rollback",
    "shared_var_rollback",
    "live_media_recovery",
)
REQUIRED_HARDWARE_RESULTS = ("capture", "encode", "stream")
REQUIRED_DEPLOYMENT_CHECKSUMS = (
    "baseline",
    "first_boot",
    "second_boot",
    "post_rollback",
    "post_removal",
)
SHA256_RE = re.compile(r"[0-9a-f]{64}\Z")
BAZZITE_VERSION_RE = re.compile(r"44\.[0-9]{8}\Z")
UTC_TIMESTAMP_RE = re.compile(r"[0-9]{4}-[0-9]{2}-[0-9]{2}T[0-9]{2}:[0-9]{2}:[0-9]{2}Z\Z")
COMMIT_RE = re.compile(r"[0-9a-f]{40}\Z")
IMAGE_DIGEST_RE = re.compile(r"sha256:[0-9a-f]{64}\Z")
PCI_ID_RE = re.compile(r"[0-9a-f]{4}:[0-9a-f]{4}\Z")


class ReceiptError(ValueError):
    """The evidence receipt is incomplete, inconsistent, or malformed."""


def reject_duplicate_json_members(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise ReceiptError(f"duplicate JSON member: {key}")
        result[key] = value
    return result


def require_mapping(value: Any, path: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ReceiptError(f"{path} must be an object")
    return value


def require_string(mapping: dict[str, Any], key: str, path: str) -> str:
    value = mapping.get(key)
    if not isinstance(value, str) or not value:
        raise ReceiptError(f"{path}.{key} must be a non-empty string")
    return value


def require_sha256(mapping: dict[str, Any], key: str, path: str) -> str:
    value = mapping.get(key)
    if not isinstance(value, str) or not SHA256_RE.fullmatch(value):
        raise ReceiptError(
            f"{path}.{key} must be 64 lowercase hexadecimal characters"
        )
    return value


def require_pass(results: dict[str, Any], gate: str, prefix: str) -> None:
    if gate not in results:
        raise ReceiptError(f"{prefix}.{gate} is missing")
    value = results[gate]
    if value != "pass":
        raise ReceiptError(f"{prefix}.{gate} must be pass, got {value}")


def sha256_relative_file(
    receipt_dir: Path,
    relative_path: PurePosixPath,
    prefix: str,
) -> str:
    directory_flags = os.O_RDONLY | os.O_DIRECTORY | os.O_CLOEXEC | os.O_NOFOLLOW
    file_flags = os.O_RDONLY | os.O_CLOEXEC | os.O_NOFOLLOW | os.O_NONBLOCK
    directory_fds: list[int] = []
    file_fd: int | None = None

    try:
        current_fd = os.open(receipt_dir, directory_flags)
        directory_fds.append(current_fd)
        for component in relative_path.parts[:-1]:
            current_fd = os.open(component, directory_flags, dir_fd=current_fd)
            directory_fds.append(current_fd)

        file_fd = os.open(relative_path.parts[-1], file_flags, dir_fd=current_fd)
        before = os.fstat(file_fd)
        if not stat.S_ISREG(before.st_mode):
            raise ReceiptError(
                f"{prefix}.path does not exist or is not a regular file"
            )

        before_identity = (
            before.st_dev,
            before.st_ino,
            before.st_size,
            before.st_mtime_ns,
            before.st_ctime_ns,
        )
        digest = hashlib.sha256()
        while chunk := os.read(file_fd, 1024 * 1024):
            digest.update(chunk)

        after = os.fstat(file_fd)
        after_identity = (
            after.st_dev,
            after.st_ino,
            after.st_size,
            after.st_mtime_ns,
            after.st_ctime_ns,
        )
        if after_identity != before_identity:
            raise ReceiptError(f"{prefix}.path changed while hashing")
        return digest.hexdigest()
    except ReceiptError:
        raise
    except OSError as error:
        raise ReceiptError(
            f"{prefix}.path does not exist or is not a regular file"
        ) from error
    finally:
        if file_fd is not None:
            os.close(file_fd)
        for directory_fd in reversed(directory_fds):
            os.close(directory_fd)


def require_hashed_file(
    relative: str,
    expected_sha256: str,
    prefix: str,
    receipt_dir: Path,
    *,
    mismatch_prefix: str | None = None,
) -> None:
    if "\x00" in relative:
        raise ReceiptError(f"{prefix}.path must be a safe relative path")

    relative_path = PurePosixPath(relative)
    if (
        not relative_path.parts
        or relative_path.is_absolute()
        or ".." in relative_path.parts
        or "\\" in relative
    ):
        raise ReceiptError(f"{prefix}.path must stay below the receipt directory")

    actual_sha256 = sha256_relative_file(receipt_dir, relative_path, prefix)
    if actual_sha256 != expected_sha256:
        raise ReceiptError(f"{mismatch_prefix or prefix} sha256 mismatch")


def require_gate_evidence(
    evidence: dict[str, Any],
    gate: str,
    prefix: str,
    receipt_dir: Path,
) -> None:
    if gate not in evidence:
        raise ReceiptError(f"{prefix}.{gate} is missing")

    entries = evidence[gate]
    if not isinstance(entries, list) or not entries:
        raise ReceiptError(f"{prefix}.{gate} must be a non-empty array")

    for index, raw_entry in enumerate(entries):
        entry_prefix = f"{prefix}.{gate}[{index}]"
        entry = require_mapping(raw_entry, entry_prefix)
        relative = require_string(entry, "path", entry_prefix)
        expected_sha256 = require_sha256(entry, "sha256", entry_prefix)
        require_hashed_file(
            relative,
            expected_sha256,
            entry_prefix,
            receipt_dir,
        )


def validate_receipt(receipt: Any, receipt_dir: Path) -> dict[str, Any]:
    receipt_dir = receipt_dir.resolve(strict=True)
    root = require_mapping(receipt, "receipt")
    schema_version = root.get("schema_version")
    if type(schema_version) is not int or schema_version != 1:
        raise ReceiptError("schema_version must be integer 1")

    captured_at_utc = require_string(root, "captured_at_utc", "receipt")
    if not UTC_TIMESTAMP_RE.fullmatch(captured_at_utc):
        raise ReceiptError(
            "receipt.captured_at_utc must match YYYY-MM-DDTHH:MM:SSZ exactly"
        )
    try:
        datetime.strptime(captured_at_utc, "%Y-%m-%dT%H:%M:%SZ")
    except ValueError as error:
        raise ReceiptError(
            "receipt.captured_at_utc must match YYYY-MM-DDTHH:MM:SSZ exactly"
        ) from error

    candidate = require_mapping(root.get("candidate"), "candidate")
    kind = require_string(candidate, "kind", "candidate")
    if kind not in {"rpm", "sysext"}:
        raise ReceiptError(f"candidate.kind must be rpm or sysext, got {kind}")
    name = require_string(candidate, "name", "candidate")
    expected_suffix = {"rpm": ".rpm", "sysext": ".raw"}[kind]
    if not name.endswith(expected_suffix):
        raise ReceiptError(
            f"candidate.name must end in {expected_suffix} for kind {kind}"
        )
    candidate_path = require_string(candidate, "path", "candidate")
    if PurePosixPath(candidate_path).name != name:
        raise ReceiptError("candidate.path filename must match candidate.name")
    require_string(candidate, "version", "candidate")

    candidate_sha256 = require_string(candidate, "sha256", "candidate")
    if not SHA256_RE.fullmatch(candidate_sha256):
        raise ReceiptError(
            "candidate.sha256 must be 64 lowercase hexadecimal characters"
        )
    require_hashed_file(
        candidate_path,
        candidate_sha256,
        "candidate",
        receipt_dir,
        mismatch_prefix="candidate.path",
    )

    source_commit = require_string(candidate, "source_commit", "candidate")
    if not COMMIT_RE.fullmatch(source_commit):
        raise ReceiptError(
            "candidate.source_commit must be 40 lowercase hexadecimal characters"
        )

    source_tree = candidate.get("source_tree")
    if not isinstance(source_tree, str) or not COMMIT_RE.fullmatch(source_tree):
        raise ReceiptError(
            "candidate.source_tree must be 40 lowercase hexadecimal characters"
        )

    build_mode = require_string(candidate, "build_mode", "candidate")
    if build_mode != "release":
        raise ReceiptError(
            f"candidate.build_mode must be release, got {build_mode}"
        )

    acquisition_url = require_string(candidate, "acquisition_url", "candidate")
    if any(character.isspace() or ord(character) < 32 for character in acquisition_url):
        raise ReceiptError("candidate.acquisition_url must be an absolute https URL")
    try:
        parsed_url = urlsplit(acquisition_url)
        has_credentials = parsed_url.username is not None or parsed_url.password is not None
    except ValueError as error:
        raise ReceiptError(
            "candidate.acquisition_url must be an absolute https URL"
        ) from error
    if parsed_url.scheme != "https" or not parsed_url.netloc:
        raise ReceiptError("candidate.acquisition_url must be an absolute https URL")
    if has_credentials:
        raise ReceiptError("candidate.acquisition_url must not contain credentials")
    if parsed_url.query or parsed_url.fragment:
        raise ReceiptError(
            "candidate.acquisition_url must not contain query parameters or fragments"
        )

    targets = root.get("targets")
    if not isinstance(targets, list):
        raise ReceiptError("targets must be an array")

    by_variant: dict[str, dict[str, Any]] = {}
    for index, raw_target in enumerate(targets):
        target = require_mapping(raw_target, f"targets[{index}]")
        variant = require_string(target, "variant", f"targets[{index}]")
        if variant in by_variant:
            raise ReceiptError(f"target variant {variant} appears more than once")
        by_variant[variant] = target

    for variant, expected_driver in REQUIRED_TARGETS.items():
        if variant not in by_variant:
            raise ReceiptError(f"required target {variant} is missing")

        target = by_variant[variant]
        driver = require_string(target, "driver", variant)
        if driver != expected_driver:
            raise ReceiptError(
                f"{variant}.driver must be {expected_driver}, got {driver}"
            )

        fedora_version = target.get("fedora_version")
        if type(fedora_version) is not int:
            raise ReceiptError(
                f"{variant}.fedora_version must be integer 44"
            )
        if fedora_version != 44:
            raise ReceiptError(
                f"{variant}.fedora_version must be 44, got {fedora_version}"
            )

        bazzite_version = require_string(target, "bazzite_version", variant)
        if not BAZZITE_VERSION_RE.fullmatch(bazzite_version):
            raise ReceiptError(
                f"{variant}.bazzite_version must match 44.YYYYMMDD exactly"
            )

        image_digest = require_string(target, "image_digest", variant)
        if not IMAGE_DIGEST_RE.fullmatch(image_digest):
            raise ReceiptError(
                f"{variant}.image_digest must be sha256 followed by 64 lowercase hexadecimal characters"
            )

        image_reference = require_string(target, "image_reference", variant)
        expected_image_reference = f"ghcr.io/ublue-os/{variant}@{image_digest}"
        if image_reference != expected_image_reference:
            raise ReceiptError(
                f"{variant}.image_reference must exactly match its variant and image_digest"
            )

        deployments = require_mapping(
            target.get("deployment_checksums"),
            f"{variant}.deployment_checksums",
        )
        deployment_values = {
            deployment: require_sha256(
                deployments,
                deployment,
                f"{variant}.deployment_checksums",
            )
            for deployment in REQUIRED_DEPLOYMENT_CHECKSUMS
        }
        if deployment_values["second_boot"] != deployment_values["first_boot"]:
            raise ReceiptError(
                f"{variant}.deployment_checksums.second_boot must equal first_boot"
            )
        if deployment_values["post_rollback"] != deployment_values["baseline"]:
            raise ReceiptError(
                f"{variant}.deployment_checksums.post_rollback must equal baseline"
            )
        if deployment_values["post_removal"] != deployment_values["baseline"]:
            raise ReceiptError(
                f"{variant}.deployment_checksums.post_removal must equal baseline"
            )
        if (
            kind == "rpm"
            and deployment_values["first_boot"] == deployment_values["baseline"]
        ):
            raise ReceiptError(
                f"{variant}.deployment_checksums.first_boot must differ from baseline for rpm"
            )
        if (
            kind == "sysext"
            and deployment_values["first_boot"] != deployment_values["baseline"]
        ):
            raise ReceiptError(
                f"{variant}.deployment_checksums.first_boot must equal baseline for sysext"
            )

        target_sha256 = require_string(target, "candidate_sha256", variant)
        if target_sha256 != candidate_sha256:
            raise ReceiptError(
                f"{variant}.candidate_sha256 does not match candidate.sha256"
            )

        hardware = require_mapping(target.get("hardware"), f"{variant}.hardware")
        environment = require_string(hardware, "environment", f"{variant}.hardware")
        if environment != "physical":
            raise ReceiptError(
                f"{variant}.hardware.environment must be physical, got {environment}"
            )
        require_string(hardware, "gpu_model", f"{variant}.hardware")
        pci_id = require_string(hardware, "pci_id", f"{variant}.hardware")
        if not PCI_ID_RE.fullmatch(pci_id):
            raise ReceiptError(
                f"{variant}.hardware.pci_id must be four lowercase hex digits, a colon, and four lowercase hex digits"
            )
        vendor = pci_id.split(":", 1)[0]
        vendor_drivers = REQUIRED_PCI_VENDOR_DRIVERS[variant]
        if vendor not in vendor_drivers:
            expected_vendors = " or ".join(sorted(vendor_drivers))
            raise ReceiptError(
                f"{variant}.hardware.pci_id vendor must be {expected_vendors}, got {vendor}"
            )
        kernel_driver = require_string(
            hardware,
            "kernel_driver",
            f"{variant}.hardware",
        )
        allowed_drivers = vendor_drivers[vendor]
        if kernel_driver not in allowed_drivers:
            expected_drivers = " or ".join(sorted(allowed_drivers))
            raise ReceiptError(
                f"{variant}.hardware.kernel_driver must be {expected_drivers}, got {kernel_driver}"
            )

        results = require_mapping(target.get("results"), f"{variant}.results")
        for gate in REQUIRED_RESULTS:
            require_pass(results, gate, f"{variant}.results")

        hardware_results = require_mapping(
            target.get("hardware_results"), f"{variant}.hardware_results"
        )
        for gate in REQUIRED_HARDWARE_RESULTS:
            require_pass(
                hardware_results,
                gate,
                f"{variant}.hardware_results",
            )

        evidence = require_mapping(target.get("evidence"), f"{variant}.evidence")
        for gate in (*REQUIRED_RESULTS, *REQUIRED_HARDWARE_RESULTS):
            require_gate_evidence(
                evidence,
                gate,
                f"{variant}.evidence",
                receipt_dir,
            )

    unknown_targets = sorted(set(by_variant) - set(REQUIRED_TARGETS))
    if unknown_targets:
        raise ReceiptError(
            "unexpected target variants: " + ", ".join(unknown_targets)
        )

    return {
        "status": "ready",
        "candidate_sha256": candidate_sha256,
        "targets": list(REQUIRED_TARGETS),
    }


def main(argv: list[str]) -> int:
    if len(argv) != 2:
        print(f"usage: {Path(argv[0]).name} RECEIPT.json", file=sys.stderr)
        return 2

    try:
        receipt_path = Path(argv[1])
        receipt = json.loads(
            receipt_path.read_text(encoding="utf-8"),
            object_pairs_hook=reject_duplicate_json_members,
        )
        report = validate_receipt(receipt, receipt_path.parent)
    except (OSError, json.JSONDecodeError, ReceiptError) as error:
        print(f"Bazzite validation receipt rejected: {error}", file=sys.stderr)
        return 1

    print(json.dumps(report, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main(sys.argv))
