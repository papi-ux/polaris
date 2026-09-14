# Runtime publication evidence

Spaces uses a native Polaris installation to supervise isolated Docker runtimes.
An image artifact is a candidate until its package setup, license review and
physical gameplay checks are complete. A signed build identifies its source and
bytes; it does not assert that a game, GPU or distribution was tested.

## Build and scan

Commit the reviewed source, then run on Linux amd64:

```sh
python3 containers/multiseat/prepare-inputs.py steam --nvidia
python3 containers/multiseat/build-image.py steam --nvidia
python3 containers/multiseat/audit-runtime.py \
  build/worker-artifacts/steam/nvidia \
  --source-revision "$(git rev-parse HEAD)" \
  --cache build/runtime-audit-cache \
  --output build/worker-artifacts/steam/nvidia/audit
```

The scan output directory must be new. Keep earlier receipts under a separate
validation directory. Use `--offline` only when both digest-pinned scanner images
and a current Grype database have already been downloaded.

The audit verifies the OCI archive and scans both its installed filesystem and
the declared dependency closure. The first scan discovers embedded Go libraries
and Ubuntu source-package metadata. The second includes vendored Rust and other
build inputs which a binary scan cannot reliably recover. Declared dependencies
include build and development inputs; their presence is not proof of runtime
reachability. Preserve both reports rather than discarding one to lower the count.

Scanner containers have no host service socket, GPU, input device or network
during analysis. The separate database update can use the network. A missing,
invalid, future-dated or more than five-day-old database fails the audit. High and
Critical matches block signing, including matches without available fixes. Other
matches remain in the report and still need review before publication.

## Dependency updates and notices

The package resolver explicitly includes already installed Ubuntu base packages
when resolving against the signed snapshot. Downloading only new dependencies
would otherwise leave some base packages without available security updates.
Final image builds install only checksum-verified inputs with network access off.

The pinned Ubuntu root contains an unmanaged Pebble executable. Polaris uses its
own process startup and does not invoke Pebble. Initialization removes only the
reviewed executable hash and rejects changed bytes, symlinks and hard links.
The prepared root is copied into a fresh image layer so removed executables and
downloaded package archives are absent from the delivered layer history.

The display plugin source lock distinguishes the upstream Cargo.lock hash from
the reviewed patched hash. `prepare-inputs.py` applies the committed dependency
patch before vendoring, then verifies the complete reconstructed archive. The
patch updates slab, tracing-subscriber and rand, along with logging dependencies
required by tracing-subscriber. The existing upstream source identity and license
notices remain intact.

Images retain Polaris and Go license texts under `/usr/share/licenses`, along
with the existing display, Gamescope, codec and NVIDIA notices. The display
plugin's vendored notices and declared license metadata are retained in
`polaris-seat-display/dependencies/index.json`. This complete vendor inventory
also includes dependencies used only during development or compilation.
Where a crate omits a standalone notice, reviewed supplemental texts come from
its exact recorded upstream source revision. The collector verifies that revision
and each notice hash before copying them. `AUTHORS` files are retained too because
some projects put their license grants there. Two current crates, drm-fourcc and
input-event-codes-sys, declare licenses but provide no standalone notice at their
recorded revisions; their metadata remains visible for distribution review.

Before distributing an image, review the actual notices and preserve matching
source, patches and build scripts for all components whose terms require them.
The source locks identify custom inputs; Ubuntu binary packages also need their
corresponding source packages. A notice index or a URL list alone does not close
that obligation. Review the included NVIDIA agreement for the intended binary
redistribution and hardware/deployment scope. Do not remove third-party
provenance to change product branding. See [ownership](OWNERSHIP.md).

## Signing and verification

The **Multiseat runtime images** workflow builds and scans without signing
permissions. On a manual run of `master`, select `attest` to enable a separate
signing job after all selected images pass. That job downloads artifacts from the
same workflow run, rechecks archive hashes, source revision, scan output hashes,
scanner locks and database age, then signs build provenance with GitHub OIDC and
Sigstore. Pull requests and feature branches cannot run this signing job.

Download both the runtime artifact and signature bundle. Verify the archive with
GitHub CLI, including the expected repository and workflow:

```sh
gh attestation verify worker.oci.tar \
  --repo papi-ux/polaris \
  --signer-workflow papi-ux/polaris/.github/workflows/multiseat-images.yml \
  --source-ref refs/heads/master --source-digest REVIEWED_COMMIT \
  --deny-self-hosted-runners
```

Also inspect the attested source revision and compare it to the reviewed commit.
The signed `artifact.json` binds the exported archive, dependency records and
provider receipt. The signed audit files bind both scan scopes. Recheck locally:

```sh
python3 containers/multiseat/audit-runtime.py runtime-directory \
  --verify --source-revision REVIEWED_COMMIT
```

Neither signing nor a successful audit uploads a Docker runtime, edits the runtime
catalog, enables Spaces, or publishes a native package. Public registry admission
still requires a verified immutable pull, completed source/license review, and
physical receipts for the exact candidate. Retain separate results for gameplay,
audio, controller input, concurrent streams and each supported frame rate.
