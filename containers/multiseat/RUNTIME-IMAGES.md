# Locked runtime images

The four `linux/amd64` profiles retain their existing Games on Whales launcher
roots. A source-root digest is never a produced-worker digest. Schema 2 in
`images.lock.json` binds each source root to its package lock and names the
location of its produced artifact manifest. Production catalog promotion and
registry publication require separate review and are absent from this job.

## Build and inspect

Use Linux/amd64, Python 3.11 or newer, Git, GNU tar, and rootless Podman. Prepare
inputs with network access, then build with network access disabled:

```sh
python3 containers/multiseat/prepare-inputs.py gamescope
python3 containers/multiseat/build-image.py gamescope
```

Repeat for `steam`, `heroic`, and `lutris`. Both commands accept `--nvidia` for
the driver-matched physical lane. The build command requires a clean commit and materializes sources and locks
from that exact Git object into a private build context. Ignored files and edits
made during a build cannot enter the image or change its later provenance.
Only independently copied and hash-verified locked inputs enter that context.
The CI matrix fetches exact locked inputs, runs integrity tests, builds with
`--network=none --pull=never`, validates dependencies, and exercises the real
session-bus, private-audio, and software-display providers. Dependency skips fail
the job. Worker binaries use the pinned Go toolchain root, only the standard
library, and disabled module-network access.

Each `build/worker-artifacts/<profile>/<default|nvidia>/` contains:

- `worker.oci.tar`, a downloadable image with no registry publication;
- `artifact.json`, binding source revision, profile, architecture, source root,
  dependency locks, validation scope, and file hashes to that archive;
- `packages.tsv`, the final root's complete installed package manifest;
- `sbom.cdx.json`, runtime packages, custom source components, and the declared
  Rust dependency closure (including build/dev dependencies);
- `providers.json`, sanitized names and scope of completed real-provider tests.

The private `providers.log` is retained locally and is excluded from uploads.
`worker_digest` is the exported OCI manifest digest. Podman's local storage
manifest may differ. Export verification hashes every referenced blob, checks
sizes and Linux/amd64 configuration, and requires the same configuration digest
as the validated image. Use the exported digest when importing/promoting that
artifact; no source root or cached local tag establishes its identity.

## Reproducible inputs

Package locks include exact versions, architecture, HTTPS URLs, SHA-256 values,
and full added runtime/build dependency closures resolved inside each immutable
source root. Resolution uses Ubuntu's signed 2026-01-20 snapshot. Historical
snapshot expiry is disabled only during this explicit resolution step; final
builds neither resolve packages nor contact repositories. Every `.deb` is
verified before installation, and `dpkg --audit` must be empty afterward.

`locks/rust.json` pins the compiler archive. `locks/plugin.json` pins the
Wayland plugin revision, Cargo.lock, Rust version, and canonical vendored archive.
Preparation runs that pinned Cargo version in a private toolchain directory;
ambient Cargo is not used. `locks/gamescope.json` pins Gamescope 3.16.19, recursive
submodules, additional Meson wraps, and its canonical source archive. GNU tar
normalizes ordering, ownership, and timestamps. A reconstruction mismatch fails
and preserves the unverified archive for investigation. Final Cargo builds are
frozen/offline; Meson wraps cannot download.

Each root compiles `waylanddisplaysrc` against its own GStreamer development ABI.
The installed system plugin directory also supplies `unixfdsink`, `unixfdsrc`,
`fakesink`, and `videoconvert`. The encoder dependency gate additionally requires
H.264/Opus encoders and decoders, Pulse capture, appsink, and the GL import,
conversion, and download elements. Their presence does not establish that a
particular GPU's DMA-BUF format can be imported or that game frames encode.
The GL plugin and its dependencies use the same signed snapshot and each root's
GStreamer ABI; Steam already includes that package in its pinned source root.
Fixed provider executables, plugin files, and
PipeWire configurations must be trusted regular files; dynamic library checks
must resolve. The custom Gamescope executable is `/usr/bin/gamescope`; the
source root's packaged `/usr/games/gamescope` remains recorded in the package
manifest but is not selected by the provider. The custom source lock and SBOM
identify the executable that the provider uses.

Lock refresh is explicit: run `resolve-packages.sh` in each disposable pinned
root, review `write-package-lock.py` output and source manifests, reconstruct
source archives with the pinned toolchain, and review every changed source or
dependency before rebuilding all four profiles. Never replace a failed hash
with the downloaded value without investigating the difference. Byte identity
does not establish publisher trust or a vulnerability policy. Final OCI metadata
uses the source commit timestamp; input reproducibility does not promise identical
image bytes across different Podman versions or compression implementations.

## NVIDIA physical lane

The optional layer pins NVIDIA 610.57.04 and the official installer archive's
SHA-256. Extraction occurs only inside a disposable build stage. It copies an
explicit set of vendor graphics, CUDA, and codec userspace libraries, SONAME
links, EGL/Vulkan configuration, license, and per-file hashes. Generic GLVND
frontends remain from the root. Kernel modules, firmware, host configuration,
and host installer execution are absent. The host driver must match this version.

Physical provider tests require an explicitly admitted render node and the
matching GPU catalog's device set. Resolve DRM primary/render nodes by their
physical sysfs device identity; numbering alone does not establish a match.
NVIDIA support must never import arbitrary CDI-generated mounts. SELinux remains
enforcing; denied GPU access is a separate admission failure, not permission to
enable blanket container device access or disable labeling.

For SELinux hosts whose ordinary container domain cannot access NVIDIA nodes,
`selinux/polaris_nvidia_worker.te` provides a separate opt-in physical-test domain.
Build it using the host-compatible reference-policy development headers, including
the installed `container_domain_template` interface, in a private working directory:

```sh
make -f /usr/share/selinux/devel/Makefile polaris_nvidia_worker.pp
```

It requires the dedicated input-device type from the input-access policy. Inspect
the expanded policy and verify the module is absent before temporary installation.
Record the installed module checksum from `semodule -l -m`. Explicit physical
containers select `--security-opt=label=type:polaris_nvidia_worker_t` and retain
Podman's fresh MCS categories, private namespaces, dropped capabilities, and exact
catalog devices. Verify the effective process label and distinct MCS categories.
Ordinary `container_t`, existing device labels, and broad device booleans remain
unchanged. The standard template includes file-management permissions; Linux
capability removal and mount admission are essential complementary controls.
After validation, remove only the module matching the recorded checksum, once
all test containers have stopped. No code installs this policy automatically.

The test-only `provider-nvidia-test` stage includes real-provider tests and a
private X11 socket-directory fixture. The worker artifact excludes that test
binary. Run the eight real provider tests with exact device admission, requiring
the nested Gamescope and independent-stack teardown tests to pass. The default
CI lane validates software capture transport; software Vulkan without
`VK_EXT_physical_device_drm` is insufficient for the current Gamescope provider.

## Acceptance and next milestone

Dependency checks establish installed ABI compatibility. Isolated provider tests
establish protocol readiness and owned-resource teardown. The separate physical
input harness must open each seat's exact allocated nodes, receive bounded host
authority events, reject other-seat and singleton nodes, and stop one worker
without affecting the other. Repeat that harness for each produced profile,
including from a user service with its actual supplementary groups.

Providers retain bounded `O_PATH` descriptors for captured artifacts until
child shutdown and cleanup finish, preventing inode reuse from turning a
replacement into an apparently owned artifact. Pins are close-on-exec, deduplicate
aliases, reject symlinks, and fail closed at their descriptor limit. Private
runtime directories remain part of the trust boundary: inode retention does not
make a pathname check followed by unlink atomic against a concurrent writer.

None of these receipts proves successful game streaming. Production `run` still
injects no lifecycle adapters. Virtual-input provider integration, worker-local
encoding, launcher process management, production media routing, seat-aware
status, and real concurrent game streams remain the next milestone. Runtime
startup must also establish the private X11 directory ownership expected by the
provider before production wiring; the isolated tests provide their own fixture.

### Isolated physical game probe

The experimental Gamescope image contains `input-pong-v1`, a small offline game
with keyboard, pointer and controller counters plus a private audio tone. The
opt-in native physical harness can select it with `POLARIS_PHYSICAL_GAME=1`.
This requires the Gamescope profile, newly initialized private profile volumes,
the exact GPU/input catalog, and the separately reviewed NVIDIA SELinux domain
on the matching physical validation lane. The harness applies that fixed domain
only after the normal backend has admitted the worker's mounts and devices.

The worker's `physical-game-probe start|state|finish TOKEN` command accepts a
32-character lowercase hexadecimal token. It requires the existing validated
allocation and authenticated worker health, claims one private probe, and starts
six real providers. Only this explicit probe enables retained-FD compositor
input. Startup is bounded to 90 seconds, total lifetime to 150 seconds, and
providers stop in reverse order. Errors remain errors even when outer worker
destruction subsequently removes the remaining resources.

The harness compares each game's counters before and after host-authority input,
checks the other game's counters stay unchanged, and verifies that the surviving
game keeps advancing after the first stops. These are game/process/input checks.
The probe excludes the encoder, publishes no media readiness, and does not claim
an encoded stream or client playback. Production adapter selection stays off.
