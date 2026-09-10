# Private system-extension assembly

This tool replaces the machine-specific package extraction step used for private
Bazzite validation. It consumes reviewed, already acquired inputs offline. It
does not install an RPM, run package scriptlets, change a service, merge an
extension, or write to the target snapshot.

The standalone release image remains withdrawn. A `PRIVATE_PACKAGE_BUILT` result
is a package assembly result. Clean-base ELF closure, scriptlet omission review,
non-root execution, lifecycle testing, and physical streaming are separate gates
in [the Bazzite validation runbook](../../validation/bazzite/README.md).

## Inputs and execution

Use a disposable Linux builder with a recorded immutable image digest. Run as
root inside that builder, with networking disabled, a read-only source target
snapshot, read-only reviewed inputs, and an empty private output parent. SELinux
label creation must be supported by that environment. A label failure fails the
build; do not disable SELinux or relabel the running host to bypass it.

The operator must independently verify the builder image identity when launching
it and retain that launch evidence. `--builder-digest` is checked against the lock;
it is not an attestation of the container runtime. The tool also checks the bytes
and package identity of every tool it executes. The pinned builder must include
their transitive libraries and configuration. Do not generate a matching lock
from an unexplained mutable environment and call it reproducible.

All input references use `{ "path": "relative/path", "sha256": "..." }`.
JSON documents have exact field sets, integer `schema_version: 1`, and the kinds
below. [manifest.py](manifest.py) is the executable contract; the manifest tests
contain small synthetic examples, not usable package locks.

| Kind | Required binding |
| --- | --- |
| `polaris-sysext-build-inputs` | Git commit and tree, candidate RPM, versioned executable path/hash/version, and references to the four documents below |
| `polaris-rpm-build` | The build producer's exact source, RPM and executable identities, NEVRA, `x86_64`, and `release` build mode |
| `polaris-sysext-dependencies` | Supported target image digests, key files with full primary fingerprints, and each dependency's path/hash/name/NEVRA/architecture/license/credential-free HTTPS acquisition URL |
| `polaris-sysext-target` | Fedora 44 Bazzite variant, image digest, OSTree commit, architecture, target metadata inventory reference, RPMDB and policy paths |
| `polaris-sysext-toolchain` | Immutable builder digest and exact path/hash/package NEVRA for Python, RPM, rpmkeys, rpm2archive, GPG, setfiles, mksquashfs and unsquashfs |

Pin the regular `rpm2archive` executable, with support for `--nocompression
--format=cpio`. The assembler captures its standard output through a bounded pipe.
Modern RPM packages provide `rpm2cpio` as a symlink; tool admission still rejects
symlinks and does not resolve them implicitly.

The target inventory has kind `polaris-sysext-target-metadata`, and `files` maps
relative paths to `{ "sha256": "...", "mode": 420 }` records. Include every
regular file in `usr/share/rpm`, and every `file_contexts*` companion under
`etc/selinux/targeted/contexts/files`. Generate it from a quiescent read-only
snapshot, not a live RPM transaction. Unexpected files or changed bytes are
rejected. This inventory covers package resolution and labeling; it does not
establish the target's ELF library inventory or verify its claimed OCI/OSTree
origin. Retain the export provenance separately.

The build producer's source/RPM attestation must come from the actual build.
Matching fields in a hand-written document cannot establish that an RPM was
compiled from that source. Dependency acquisition and review happen before this
offline tool runs. No default dependency lock, wildcard target, live repository,
or automatically trusted signer is supplied.

With the reviewed input directory and target snapshot mounted at the indicated
paths inside the builder, run:

```sh
python3 scripts/packaging/sysext/assemble.py \
  --inputs /inputs \
  --manifest manifest.json \
  --manifest-sha256 "$REVIEWED_MANIFEST_SHA256" \
  --target-root /target \
  --builder-digest "$VERIFIED_BUILDER_DIGEST" \
  --output /output/private-run
```

`/output` must already exist, be root-owned, and not be writable by other users.
The run directory must not exist. Input reads use no-follow descriptors and are
hashed while being copied into exclusive private staging. All commands have
bounded output and time limits. A signal or failed command prevents a successful
completion receipt. Failed output directories are retained for diagnosis and
must never be reused as a new build destination.

## What is checked

- Every dependency signature is checked against a private keyring containing only
  the locked keys. The private candidate is admitted by its reviewed build
  attestation and hash, without claiming a release signature.
- `rpm --install --test` checks dependency and file conflicts against a private
  copy of the target RPMDB. Base package replacements and packages declaring
  obsoletes are rejected. No repository is consulted. The original target files
  are rehashed, and the private package inventory must remain unchanged.
- Complete CPIO archives are validated before writing. Paths outside `/usr` and
  `/opt`, special nodes, non-root ownership, privileged modes/capabilities,
  writable payloads, malformed hardlinks and cross-package path collisions are
  rejected. Extension metadata is generated in a reserved directory.
- Expanded regular-file bytes, including every reconstructed hardlink copy,
  have a 256 MiB aggregate limit checked before materialization.
- The exact executable hash must match. The admitted file manifest must match
  the written tree. Locked target policy labels the private tree, and the full
  file/label manifest must survive SquashFS extraction unchanged.

The output contains `polaris.raw`, `package-manifest.json`, frozen inputs,
command logs, and `build-receipt.json`. The manifest includes ownership by RPM,
licenses, scriptlet text, content hashes and SELinux labels. The completion
receipt is written last, exclusively and durably. Only a zero exit status plus a
matching successful receipt admits the package result. An image left by a failed
run is not an admitted candidate.

The package result explicitly lists the remaining validation. In particular,
omitted scriptlets can create required caches or other state: inspect their
recorded text and validate runtime behavior before treating the image as usable.
Build twice from clean directories in the same pinned environment and compare
raw hashes before claiming byte reproducibility. Test the same bytes against
both required image families before broadening the compatibility claim. Do not
upload these private images as release or Actions artifacts.

## Tests and CI

```sh
python3 -m unittest discover -s scripts/packaging/sysext -p 'test_*.py'
```

The ordinary source check runs parser, manifest, staging, bounded-child and
completion-failure tests. Linux also tests TERM/HUP at child creation and receipt
commit, and keeps process-group ownership until cleanup finishes. Command
execution uses Linux `waitid`; macOS runs only the portable parser/contract tests.
The existing Fedora RPM job also requires RPM build/signing tools and GPG and
runs real disposable-package transaction and signer tests. These install only
fixture metadata into temporary RPMDBs, then test dependency closure, missing
transitive dependencies, base replacements, obsoletes, scriptlet non-execution,
preservation of the original database, correct/wrong/unsigned package keys and
exclusion of an ambient trusted key. They do not build or publish a raw
artifact. Full assembly and namespace lifecycle integration need the reviewed
candidate/target/toolchain inputs described above.
