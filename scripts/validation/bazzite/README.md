# Bazzite validation gate

Publication remains disabled for the standalone Polaris system extension. This directory defines the evidence required before that decision can be reconsidered. Passing structural image checks is not enough.

The supported Fedora 44 RPM path remains separate. Its dependencies may be resolved by `rpm-ostree`; a standalone system extension must carry every runtime dependency not already guaranteed by the target image.

## Required targets

Every candidate is bound to one exact SHA-256 digest and one exact source commit. The same bytes must pass both current Fedora 44 targets:

- `bazzite` for the AMD and Intel image family
- `bazzite-nvidia-open` for the NVIDIA open-kernel-module image family

VM evidence may satisfy package, boot, reboot, removal, rollback, shared `/var`, recovery, and SELinux lifecycle gates. It cannot satisfy capture, encode, or streaming gates. Those require the matching physical GPU family.

## Safety invariants

1. Use an isolated VM, snapshot, or dedicated validation host. Never experiment on a recovered community machine.
2. Pin the installer checksum, target image digest, candidate digest, source commit, and every booted deployment checksum in the receipt.
3. Keep candidate artifacts in a private validation path. Do not upload them to releases or Actions artifacts.
4. Validate candidate identity before any package-manager or extension operation.
5. For RPM validation, stage with `rpm-ostree install` and inspect the pending deployment before rebooting separately.
6. For a future system-extension candidate, prove ELF closure and execution before copying anything into `/var/lib/extensions`.
7. `setup-host` runs only after the installed deployment boots, package identity matches, every ELF dependency resolves, and SELinux is enforcing.
8. Never automate a reboot in the same command that stages a deployment. Capture state first, then reboot as a separate controlled transition.
9. Failed installation or host setup must leave no enabled service, persistent extension, pending deployment, stale staged package, or unexplained host file.
10. An ostree rollback does not imply `/var` rollback. Test and record shared `/var` behavior explicitly.

## Lifecycle sequence

### 0. Artifact identity

Record the artifact filename, SHA-256, RPM NEVRA or extension metadata, source commit, target image digest, and acquisition URL. The `artifact_identity` result passes only when every later stage references the same candidate digest.

### 1. Dependency and ELF closure

For an RPM candidate, run a package-manager transaction simulation against each target inventory and record the exact dependency transaction as `repo_resolution`. After layering and booting, inventory every ELF executable and shared library from the candidate and require zero unresolved `DT_NEEDED` entries for `elf_closure`.

For a system-extension candidate, `elf_closure` must pass using only the extension payload plus libraries guaranteed by the exact target image. Repository availability does not repair a standalone image.

### 2. Clean baseline

Boot the exact Fedora 44 target and record:

- Bazzite image reference and immutable digest
- current and rollback deployment checksums
- `getenforce` output
- package absence
- service and host-integration file absence
- `/var/lib/extensions` inventory
- a unique shared-`/var` marker

The baseline contributes to `selinux_enforcing` and anchors all later diffs.

### 3. Failure injection before persistence

Exercise two bounded failures on a disposable snapshot:

1. Reject a wrong candidate digest before invoking `rpm-ostree` or `systemd-sysext`.
2. Force the installation command to fail and verify that no pending deployment, persistent extension, enabled service, or staged candidate remains.

Record `install_failure_cleanup` only after the complete before/after state diff passes.

Then force `polaris --setup-host` to fail after the package has booted but before service startup. Verify that partial udev, module-load, capability, and service state is removed. Record that result as `setup_failure_cleanup`.

### 4. Install and first boot

Stage the candidate without rebooting. Confirm the current deployment is unchanged and the pending deployment is bound to the expected candidate digest. Record `install` only after that inspection.

Reboot as a separate transition. Record `first_boot` only when the expected deployment is booted, package identity matches, the executable starts for a non-streaming probe, `elf_closure` still passes, and `selinux_enforcing` remains true.

Only then run host setup. Capture all files, capabilities, udev state, modules, service state, and audit events it changes.

### 5. Second boot and SELinux

Reboot again without altering the deployment. Record `second_boot` only when the same deployment and candidate digest return healthy.

Exercise the installed binary and host integration. Query AVC and USER_AVC events from the bounded test window. `selinux_avc_clean` passes only when there are no unexplained denials and enforcing mode stayed active for the whole window.

### 6. Deployment rollback and shared `/var`

Keep the unique `/var` marker, then roll back to the clean deployment and reboot. Record `deployment_rollback` only when the package and immutable `/usr` changes are gone.

Record `shared_var_rollback` only after proving the marker persisted across rollback and documenting the state of `/var/lib/extensions`. For a system-extension candidate, prove that rollback alone does not remove the persistent image, then remove it through the tested recovery path before the next boot.

### 7. Removal

Return to the candidate deployment if needed, remove the layered package or validation extension, and reboot. Record `removal` only when the package, service, capabilities, host-integration files, pending deployment, staged candidate, and persistent extension are all absent or explicitly restored to their baseline state.

### 8. Live-media recovery

From a separate live environment, mount the guest or validation host's Btrfs-backed system volume using the same layout exposed by Bazzite. Locate shared `/var`, remove only the validation candidate, flush writes, unmount cleanly, and boot the installed system. Record `live_media_recovery` only after the normal deployment boots and the exact removed path remains absent.

### 9. Physical driver validation

On matching physical hardware for each target, record all three `hardware_results` as pass:

- `capture` - the expected display and capture path initializes
- `encode` - the expected hardware encoder is selected and produces frames
- `stream` - a bounded client session connects, renders, accepts input, and disconnects cleanly

Do not translate VM rendering, software encoding, or package-manager success into physical GPU evidence.

## Receipt validation

A final receipt must set every required result to the literal string `pass`. Values such as `skip`, `blocked`, `pending`, `not-applicable`, or `fail` are intentionally rejected.

### Receipt identity contract

The receipt root uses `schema_version: 1` as a JSON integer and records `captured_at_utc` as an exact `YYYY-MM-DDTHH:MM:SSZ` timestamp using ASCII digits.

The `candidate` object records:

- `kind` as `rpm` or `sysext`, with a matching `.rpm` or `.raw` filename
- `name`, `version`, and the declared artifact SHA-256
- `candidate.path`, a candidate file path relative to the receipt directory; the validator hashes these exact bytes and requires the basename to match `name`
- `source_commit` and `source_tree` as exact 40-character lowercase Git object IDs
- `build_mode` as the literal `release`
- `acquisition_url` as a stable absolute HTTPS URL with no embedded credentials, query parameters or fragments

Each target records:

- `bazzite_version` in exact `44.YYYYMMDD` form
- `image_reference` as `ghcr.io/ublue-os/<variant>@<image_digest>`, bound to the target's immutable digest
- `deployment_checksums` for `baseline`, `first_boot`, `second_boot`, `post_rollback`, and `post_removal`; `second_boot` must equal `first_boot`, while `post_rollback` and `post_removal` must equal `baseline`
- `first_boot` must differ from `baseline` for RPM candidates, proving the layered deployment actually booted
- `first_boot` must equal `baseline` for system extensions, which must not alter the OSTree deployment
- `hardware.environment` as the literal `physical`, plus `gpu_model`, `pci_id`, and `kernel_driver`; the standard image accepts AMD vendor `1002` with `amdgpu` or Intel vendor `8086` with `i915`/`xe`, while NVIDIA-open requires vendor `10de` with `nvidia`
- every lifecycle `results` and `hardware_results` value as the literal `pass`
- `evidence`, mapping every required lifecycle and hardware gate to one or more `{path, sha256}` records

JSON parsing rejects duplicate JSON members at every nesting level. Candidate and evidence paths are relative to the receipt directory. Absolute paths, `..` traversal, backslash paths, symlinks, missing files, non-regular files including FIFOs, and SHA-256 mismatches are rejected without blocking. Hashing uses descriptor-relative opens with `O_NOFOLLOW` and `O_NONBLOCK`, hashes the opened descriptor, verifies its metadata did not change during the read, and never reuses a pathname hash cache. One evidence file may support several gates, but reviewers must verify that its contents actually prove each referenced gate.

Run:

```bash
python3 scripts/validation/bazzite/validate_receipt.py RECEIPT.json
```

A `ready` result proves that the receipt is complete, internally consistent, and hash-bound to local files. It does not authorize uploading, publishing, or restoring the standalone system-extension release path. Publication remains disabled until an explicit review decision after all evidence is inspected.

The validator requires these target results:

- `artifact_identity`
- `repo_resolution`
- `elf_closure`
- `selinux_enforcing`
- `selinux_avc_clean`
- `install_failure_cleanup`
- `setup_failure_cleanup`
- `install`
- `first_boot`
- `second_boot`
- `removal`
- `deployment_rollback`
- `shared_var_rollback`
- `live_media_recovery`

It also requires `capture`, `encode`, and `stream` hardware results for both target families. A validator success means the receipt is complete and internally consistent. Reviewers must still inspect the referenced immutable evidence before any publication decision.
