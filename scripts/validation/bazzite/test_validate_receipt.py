#!/usr/bin/env python3
import copy
import hashlib
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

SCRIPT = Path(__file__).with_name("validate_receipt.py")
ROOT = Path(__file__).resolve().parents[3]
CANDIDATE_CONTENT = b"private Polaris release candidate\n"
SHA = hashlib.sha256(CANDIDATE_CONTENT).hexdigest()
CANDIDATE_PATH = "candidate/Polaris-fedora44-x86_64.rpm"
SYSEXT_CANDIDATE_PATH = "candidate/Polaris-sysext-x86_64.raw"
EVIDENCE_CONTENT = b"immutable bazzite validation evidence\n"
EVIDENCE_PATH = "evidence/validation.log"
EVIDENCE_SHA = hashlib.sha256(EVIDENCE_CONTENT).hexdigest()
COMMON_GATES = (
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
HARDWARE_GATES = ("capture", "encode", "stream")
ALL_GATES = COMMON_GATES + HARDWARE_GATES


def passing_receipt() -> dict:
    def target(variant: str, driver: str, image_digest: str) -> dict:
        return {
            "variant": variant,
            "driver": driver,
            "fedora_version": 44,
            "bazzite_version": "44.20260902",
            "image_digest": image_digest,
            "image_reference": f"ghcr.io/ublue-os/{variant}@{image_digest}",
            "candidate_sha256": SHA,
            "deployment_checksums": {
                "baseline": "d" * 64,
                "first_boot": "e" * 64,
                "second_boot": "e" * 64,
                "post_rollback": "d" * 64,
                "post_removal": "d" * 64,
            },
            "hardware": {
                "environment": "physical",
                "gpu_model": (
                    "NVIDIA GeForce RTX 4090"
                    if driver == "nvidia-open"
                    else "AMD Radeon RX 7900 XTX"
                ),
                "pci_id": "10de:2684" if driver == "nvidia-open" else "1002:744c",
                "kernel_driver": "nvidia" if driver == "nvidia-open" else "amdgpu",
            },
            "results": {gate: "pass" for gate in COMMON_GATES},
            "hardware_results": {
                "capture": "pass",
                "encode": "pass",
                "stream": "pass",
            },
            "evidence": {
                gate: [{"path": EVIDENCE_PATH, "sha256": EVIDENCE_SHA}]
                for gate in ALL_GATES
            },
        }

    return {
        "schema_version": 1,
        "captured_at_utc": "2026-09-06T01:17:10Z",
        "candidate": {
            "kind": "rpm",
            "name": "Polaris-fedora44-x86_64.rpm",
            "path": CANDIDATE_PATH,
            "sha256": SHA,
            "version": "1.4.3-1",
            "source_commit": "1" * 40,
            "source_tree": "2" * 40,
            "build_mode": "release",
            "acquisition_url": "https://validation.invalid/polaris/candidate",
        },
        "targets": [
            target("bazzite", "amd-intel", "sha256:" + "b" * 64),
            target("bazzite-nvidia-open", "nvidia-open", "sha256:" + "c" * 64),
        ],
    }


class ReceiptValidatorTests(unittest.TestCase):
    def run_validator(
        self,
        receipt: dict,
        *,
        candidate_content: bytes | None = CANDIDATE_CONTENT,
        candidate_relative_path: str = CANDIDATE_PATH,
        evidence_content: bytes = EVIDENCE_CONTENT,
        evidence_fifo: bool = False,
    ) -> subprocess.CompletedProcess[str]:
        return self.run_serialized_receipt(
            json.dumps(receipt),
            candidate_content=candidate_content,
            candidate_relative_path=candidate_relative_path,
            evidence_content=evidence_content,
            evidence_fifo=evidence_fifo,
        )

    def run_serialized_receipt(
        self,
        serialized_receipt: str,
        *,
        candidate_content: bytes | None = CANDIDATE_CONTENT,
        candidate_relative_path: str = CANDIDATE_PATH,
        evidence_content: bytes = EVIDENCE_CONTENT,
        evidence_symlink_target: Path | None = None,
        evidence_fifo: bool = False,
    ) -> subprocess.CompletedProcess[str]:
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            if candidate_content is not None:
                candidate_path = root / candidate_relative_path
                candidate_path.parent.mkdir(parents=True)
                candidate_path.write_bytes(candidate_content)
            evidence_path = root / EVIDENCE_PATH
            evidence_path.parent.mkdir(parents=True)
            if evidence_fifo:
                os.mkfifo(evidence_path)
            elif evidence_symlink_target is None:
                evidence_path.write_bytes(evidence_content)
            else:
                evidence_path.symlink_to(evidence_symlink_target)
            path = root / "receipt.json"
            path.write_text(serialized_receipt, encoding="utf-8")
            return subprocess.run(
                ["python3", str(SCRIPT), str(path)],
                text=True,
                capture_output=True,
                check=False,
                timeout=3,
            )

    def test_accepts_complete_matrix(self) -> None:
        result = self.run_validator(passing_receipt())
        self.assertEqual(result.returncode, 0, result.stderr)
        report = json.loads(result.stdout)
        self.assertEqual(report["status"], "ready")
        self.assertEqual(report["candidate_sha256"], SHA)
        self.assertEqual(report["targets"], ["bazzite", "bazzite-nvidia-open"])

    def test_rejects_non_integer_schema_version(self) -> None:
        for value in (True, 1.0):
            with self.subTest(value=value):
                receipt = passing_receipt()
                receipt["schema_version"] = value
                result = self.run_validator(receipt)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("schema_version must be integer 1", result.stderr)

    def test_rejects_duplicate_json_members(self) -> None:
        serialized = json.dumps(passing_receipt()).replace(
            '"schema_version": 1',
            '"schema_version": 99, "schema_version": 1',
            1,
        )
        result = self.run_serialized_receipt(serialized)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("duplicate JSON member: schema_version", result.stderr)

    def test_rejects_missing_gate_evidence(self) -> None:
        receipt = passing_receipt()
        del receipt["targets"][0]["evidence"]["first_boot"]
        result = self.run_validator(receipt)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("bazzite.evidence.first_boot is missing", result.stderr)

    def test_rejects_tampered_evidence_bytes(self) -> None:
        result = self.run_validator(passing_receipt(), evidence_content=b"tampered\n")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("bazzite.evidence.artifact_identity[0] sha256 mismatch", result.stderr)

    def test_rejects_evidence_path_escape(self) -> None:
        receipt = passing_receipt()
        receipt["targets"][0]["evidence"]["artifact_identity"][0]["path"] = "../outside.log"
        result = self.run_validator(receipt)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(
            "bazzite.evidence.artifact_identity[0].path must stay below the receipt directory",
            result.stderr,
        )

    def test_rejects_symlinked_evidence(self) -> None:
        with tempfile.TemporaryDirectory() as directory:
            external = Path(directory) / "outside.log"
            external.write_bytes(EVIDENCE_CONTENT)
            result = self.run_serialized_receipt(
                json.dumps(passing_receipt()),
                evidence_symlink_target=external,
            )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(
            "bazzite.evidence.artifact_identity[0].path does not exist or is not a regular file",
            result.stderr,
        )

    def test_hashing_uses_descriptor_relative_nofollow_opens_without_a_path_cache(self) -> None:
        source = SCRIPT.read_text(encoding="utf-8")
        self.assertIn("os.O_NOFOLLOW", source)
        self.assertIn("dir_fd=", source)
        self.assertIn("os.fstat", source)
        self.assertNotIn("hash_cache", source)
        self.assertNotIn('resolved_path.open("rb")', source)

    def test_rejects_fifo_evidence_without_blocking(self) -> None:
        result = self.run_validator(passing_receipt(), evidence_fifo=True)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("does not exist or is not a regular file", result.stderr)

    def test_rejects_nul_in_evidence_path_without_a_traceback(self) -> None:
        receipt = passing_receipt()
        receipt["targets"][0]["evidence"]["artifact_identity"][0]["path"] = (
            "evidence/validation.log\x00outside"
        )
        result = self.run_validator(receipt)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("must be a safe relative path", result.stderr)
        self.assertNotIn("Traceback", result.stderr)

    def test_rejects_missing_required_gate(self) -> None:
        receipt = passing_receipt()
        del receipt["targets"][0]["results"]["live_media_recovery"]
        result = self.run_validator(receipt)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("bazzite.results.live_media_recovery is missing", result.stderr)

    def test_rejects_nonpassing_gate(self) -> None:
        for value in ("fail", "blocked", "skip", "pending"):
            with self.subTest(value=value):
                receipt = passing_receipt()
                receipt["targets"][1]["results"]["selinux_avc_clean"] = value
                result = self.run_validator(receipt)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(
                    f"bazzite-nvidia-open.results.selinux_avc_clean must be pass, got {value}",
                    result.stderr,
                )

    def test_rejects_hardware_gate_that_was_not_exercised(self) -> None:
        receipt = passing_receipt()
        receipt["targets"][1]["hardware_results"]["stream"] = "not-applicable"
        result = self.run_validator(receipt)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(
            "bazzite-nvidia-open.hardware_results.stream must be pass, got not-applicable",
            result.stderr,
        )

    def test_rejects_virtual_hardware_evidence(self) -> None:
        receipt = passing_receipt()
        receipt["targets"][0]["hardware"]["environment"] = "vm"
        result = self.run_validator(receipt)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("bazzite.hardware.environment must be physical, got vm", result.stderr)

    def test_rejects_missing_gpu_model(self) -> None:
        receipt = passing_receipt()
        del receipt["targets"][0]["hardware"]["gpu_model"]
        result = self.run_validator(receipt)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("bazzite.hardware.gpu_model must be a non-empty string", result.stderr)

    def test_rejects_wrong_kernel_driver_for_variant(self) -> None:
        receipt = passing_receipt()
        receipt["targets"][1]["hardware"]["kernel_driver"] = "nouveau"
        result = self.run_validator(receipt)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(
            "bazzite-nvidia-open.hardware.kernel_driver must be nvidia, got nouveau",
            result.stderr,
        )

    def test_rejects_amd_pci_vendor_for_nvidia_target(self) -> None:
        receipt = passing_receipt()
        receipt["targets"][1]["hardware"]["pci_id"] = "1002:744c"
        result = self.run_validator(receipt)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(
            "bazzite-nvidia-open.hardware.pci_id vendor must be 10de, got 1002",
            result.stderr,
        )

    def test_rejects_nvidia_pci_vendor_for_standard_target(self) -> None:
        receipt = passing_receipt()
        receipt["targets"][0]["hardware"]["pci_id"] = "10de:2684"
        result = self.run_validator(receipt)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(
            "bazzite.hardware.pci_id vendor must be 1002 or 8086, got 10de",
            result.stderr,
        )

    def test_rejects_malformed_gpu_pci_id(self) -> None:
        receipt = passing_receipt()
        receipt["targets"][0]["hardware"]["pci_id"] = "1002"
        result = self.run_validator(receipt)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(
            "bazzite.hardware.pci_id must be four lowercase hex digits, a colon, and four lowercase hex digits",
            result.stderr,
        )

    def test_rejects_missing_required_variant(self) -> None:
        receipt = passing_receipt()
        receipt["targets"] = receipt["targets"][:1]
        result = self.run_validator(receipt)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("required target bazzite-nvidia-open is missing", result.stderr)

    def test_rejects_duplicate_variant(self) -> None:
        receipt = passing_receipt()
        receipt["targets"].append(copy.deepcopy(receipt["targets"][0]))
        result = self.run_validator(receipt)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("target variant bazzite appears more than once", result.stderr)

    def test_rejects_fedora_version_other_than_44(self) -> None:
        receipt = passing_receipt()
        receipt["targets"][0]["fedora_version"] = 42
        result = self.run_validator(receipt)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("bazzite.fedora_version must be 44, got 42", result.stderr)

    def test_rejects_non_integer_fedora_version(self) -> None:
        receipt = passing_receipt()
        receipt["targets"][0]["fedora_version"] = 44.0
        result = self.run_validator(receipt)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("bazzite.fedora_version must be integer 44", result.stderr)

    def test_rejects_missing_bazzite_version(self) -> None:
        receipt = passing_receipt()
        del receipt["targets"][0]["bazzite_version"]
        result = self.run_validator(receipt)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("bazzite.bazzite_version must be a non-empty string", result.stderr)

    def test_rejects_bazzite_version_with_non_ascii_digits(self) -> None:
        receipt = passing_receipt()
        receipt["targets"][0]["bazzite_version"] = "44.٢٠٢٦٠٩٠٢"
        result = self.run_validator(receipt)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("must match 44.YYYYMMDD exactly", result.stderr)

    def test_rejects_image_reference_that_does_not_bind_the_digest(self) -> None:
        receipt = passing_receipt()
        receipt["targets"][1]["image_reference"] = (
            "ghcr.io/ublue-os/bazzite-nvidia-open@sha256:" + "f" * 64
        )
        result = self.run_validator(receipt)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(
            "bazzite-nvidia-open.image_reference must exactly match its variant and image_digest",
            result.stderr,
        )

    def test_rejects_missing_booted_deployment_checksum(self) -> None:
        receipt = passing_receipt()
        del receipt["targets"][0]["deployment_checksums"]["second_boot"]
        result = self.run_validator(receipt)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(
            "bazzite.deployment_checksums.second_boot must be 64 lowercase hexadecimal characters",
            result.stderr,
        )

    def test_rejects_second_boot_from_a_different_deployment(self) -> None:
        receipt = passing_receipt()
        receipt["targets"][0]["deployment_checksums"]["second_boot"] = "f" * 64
        result = self.run_validator(receipt)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(
            "bazzite.deployment_checksums.second_boot must equal first_boot",
            result.stderr,
        )

    def test_rejects_rollback_that_does_not_restore_the_baseline_deployment(self) -> None:
        receipt = passing_receipt()
        receipt["targets"][0]["deployment_checksums"]["post_rollback"] = "f" * 64
        result = self.run_validator(receipt)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(
            "bazzite.deployment_checksums.post_rollback must equal baseline",
            result.stderr,
        )

    def test_rejects_removal_that_does_not_restore_the_baseline_deployment(self) -> None:
        receipt = passing_receipt()
        receipt["targets"][0]["deployment_checksums"]["post_removal"] = "f" * 64
        result = self.run_validator(receipt)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(
            "bazzite.deployment_checksums.post_removal must equal baseline",
            result.stderr,
        )

    def test_rejects_rpm_first_boot_that_does_not_change_the_deployment(self) -> None:
        receipt = passing_receipt()
        for target in receipt["targets"]:
            baseline = target["deployment_checksums"]["baseline"]
            target["deployment_checksums"]["first_boot"] = baseline
            target["deployment_checksums"]["second_boot"] = baseline
        result = self.run_validator(receipt)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(
            "bazzite.deployment_checksums.first_boot must differ from baseline for rpm",
            result.stderr,
        )

    def test_rejects_sysext_that_changes_the_ostree_deployment(self) -> None:
        receipt = passing_receipt()
        receipt["candidate"]["kind"] = "sysext"
        receipt["candidate"]["name"] = "Polaris-sysext-x86_64.raw"
        receipt["candidate"]["path"] = SYSEXT_CANDIDATE_PATH
        result = self.run_validator(
            receipt,
            candidate_relative_path=SYSEXT_CANDIDATE_PATH,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(
            "bazzite.deployment_checksums.first_boot must equal baseline for sysext",
            result.stderr,
        )

    def test_accepts_sysext_without_an_ostree_deployment_change(self) -> None:
        receipt = passing_receipt()
        receipt["candidate"]["kind"] = "sysext"
        receipt["candidate"]["name"] = "Polaris-sysext-x86_64.raw"
        receipt["candidate"]["path"] = SYSEXT_CANDIDATE_PATH
        for target in receipt["targets"]:
            baseline = target["deployment_checksums"]["baseline"]
            target["deployment_checksums"]["first_boot"] = baseline
            target["deployment_checksums"]["second_boot"] = baseline
        result = self.run_validator(
            receipt,
            candidate_relative_path=SYSEXT_CANDIDATE_PATH,
        )
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_rejects_malformed_candidate_digest(self) -> None:
        receipt = passing_receipt()
        receipt["candidate"]["sha256"] = "abc123"
        result = self.run_validator(receipt)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("candidate.sha256 must be 64 lowercase hexadecimal characters", result.stderr)

    def test_rejects_missing_candidate_path(self) -> None:
        receipt = passing_receipt()
        del receipt["candidate"]["path"]
        result = self.run_validator(receipt)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("candidate.path must be a non-empty string", result.stderr)

    def test_rejects_tampered_candidate_bytes(self) -> None:
        result = self.run_validator(
            passing_receipt(),
            candidate_content=b"tampered candidate\n",
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("candidate.path sha256 mismatch", result.stderr)

    def test_rejects_candidate_path_escape(self) -> None:
        receipt = passing_receipt()
        receipt["candidate"]["path"] = "../Polaris-fedora44-x86_64.rpm"
        result = self.run_validator(receipt)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(
            "candidate.path must stay below the receipt directory",
            result.stderr,
        )

    def test_rejects_candidate_path_filename_mismatch(self) -> None:
        receipt = passing_receipt()
        receipt["candidate"]["name"] = "renamed.rpm"
        result = self.run_validator(receipt)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("candidate.path filename must match candidate.name", result.stderr)

    def test_rejects_missing_source_tree(self) -> None:
        receipt = passing_receipt()
        del receipt["candidate"]["source_tree"]
        result = self.run_validator(receipt)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(
            "candidate.source_tree must be 40 lowercase hexadecimal characters",
            result.stderr,
        )

    def test_rejects_nonrelease_build_mode(self) -> None:
        receipt = passing_receipt()
        receipt["candidate"]["build_mode"] = "debug"
        result = self.run_validator(receipt)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("candidate.build_mode must be release, got debug", result.stderr)

    def test_rejects_candidate_acquisition_url_with_credentials(self) -> None:
        receipt = passing_receipt()
        receipt["candidate"]["acquisition_url"] = "https://user:secret@example.com/build"
        result = self.run_validator(receipt)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("candidate.acquisition_url must not contain credentials", result.stderr)

    def test_rejects_candidate_acquisition_url_with_query_parameters(self) -> None:
        receipt = passing_receipt()
        receipt["candidate"]["acquisition_url"] = "https://example.com/build?token=secret"
        result = self.run_validator(receipt)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn(
            "candidate.acquisition_url must not contain query parameters or fragments",
            result.stderr,
        )

    def test_rejects_filename_that_does_not_match_candidate_kind(self) -> None:
        receipt = passing_receipt()
        receipt["candidate"]["kind"] = "sysext"
        result = self.run_validator(receipt)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("candidate.name must end in .raw for kind sysext", result.stderr)

    def test_rejects_missing_utc_capture_timestamp(self) -> None:
        receipt = passing_receipt()
        del receipt["captured_at_utc"]
        result = self.run_validator(receipt)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("receipt.captured_at_utc must be a non-empty string", result.stderr)

    def test_rejects_unknown_candidate_kind(self) -> None:
        receipt = passing_receipt()
        receipt["candidate"]["kind"] = "zip"
        result = self.run_validator(receipt)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("candidate.kind", result.stderr)

    def test_rejects_target_digest_drift(self) -> None:
        receipt = passing_receipt()
        receipt["targets"][1]["candidate_sha256"] = "d" * 64
        result = self.run_validator(receipt)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("bazzite-nvidia-open.candidate_sha256 does not match candidate.sha256", result.stderr)

    def test_workflow_runs_receipt_validator_tests_once(self) -> None:
        workflow = (ROOT / ".github/workflows/build.yml").read_text(encoding="utf-8")
        command = "python3 scripts/validation/bazzite/test_validate_receipt.py"
        self.assertEqual(workflow.count(command), 1)

    def test_rejects_noncanonical_utc_capture_timestamp(self) -> None:
        receipt = passing_receipt()
        receipt["captured_at_utc"] = "2026-9-6T1:2:3Z"
        result = self.run_validator(receipt)
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("must match YYYY-MM-DDTHH:MM:SSZ exactly", result.stderr)

    def test_runbook_documents_receipt_identity_fields(self) -> None:
        runbook = (SCRIPT.parent / "README.md").read_text(encoding="utf-8")
        for field in (
            "captured_at_utc",
            "candidate.path",
            "source_tree",
            "build_mode",
            "acquisition_url",
            "bazzite_version",
            "image_reference",
            "deployment_checksums",
            "hardware.environment",
            "gpu_model",
            "pci_id",
            "kernel_driver",
            "evidence",
        ):
            with self.subTest(field=field):
                self.assertIn(f"`{field}`", runbook)
        self.assertIn("relative to the receipt directory", runbook)
        self.assertIn("query parameters or fragments", runbook)
        self.assertIn("duplicate JSON members", runbook)
        self.assertIn("`second_boot` must equal `first_boot`", runbook)
        self.assertIn(
            "`post_rollback` and `post_removal` must equal `baseline`",
            runbook,
        )
        self.assertIn("descriptor-relative", runbook)
        self.assertIn("`O_NOFOLLOW`", runbook)
        self.assertIn("ASCII digits", runbook)
        self.assertIn("FIFOs", runbook)
        self.assertIn(
            "`first_boot` must differ from `baseline` for RPM",
            runbook,
        )
        self.assertIn(
            "`first_boot` must equal `baseline` for system extensions",
            runbook,
        )
        for vendor in ("1002", "8086", "10de"):
            with self.subTest(vendor=vendor):
                self.assertIn(f"`{vendor}`", runbook)

    def test_runbook_covers_every_required_gate_and_target(self) -> None:
        runbook = (SCRIPT.parent / "README.md").read_text(encoding="utf-8")
        for gate in COMMON_GATES:
            with self.subTest(gate=gate):
                self.assertIn(f"`{gate}`", runbook)
        for target in ("bazzite", "bazzite-nvidia-open"):
            with self.subTest(target=target):
                self.assertIn(f"`{target}`", runbook)
        self.assertIn("Publication remains disabled", runbook)
        self.assertIn("`setup-host` runs only after the installed deployment boots", runbook)


if __name__ == "__main__":
    unittest.main()
