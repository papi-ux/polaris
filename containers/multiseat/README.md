# Multiseat worker image inputs

This directory defines an offline-reviewable image recipe. It does not enable
multiseat or make the current Polaris process a container controller.

`images.lock.json` contains the only accepted build and runtime inputs. Every
reference names an immutable OCI index digest; moving tags are intentionally
absent. The plain Gamescope-capable base and the Steam, Heroic, and Lutris
variants all receive the same static `polaris-seat-worker` binary. Keeping the
launchers separate avoids multiplying package and credential state inside one
large image.

The build stage has no module or package download step. The worker uses only
the Go standard library, disables CGO and module-network access, runs its tests,
then emits a static Linux/amd64 binary. A future image job must select a runtime
reference from the lock, build at an exact Polaris revision, record the
resulting image digest, and hand only that final digest to the Podman backend.

These locks establish immutable byte identity, not publisher trust. No
signature, attestation, SBOM, vulnerability policy, or license bundle is
claimed by this spike. A publishable image must add those gates and retain the
resolved upstream manifests as provenance evidence before any lock refresh.

The current entrypoint is intentionally a supervisor and IPC proof. It owns
private control and media sockets, mutual authentication, health state, and
shutdown. It does not yet start Gamescope, Steam, Heroic, Lutris, audio,
capture, encoding, or virtual input. Treating a healthy supervisor as a
streaming-capable worker before those adapters exist would be a false gate.

The container retains `--network=none`. Beneath a pre-created mode-0700
runtime root, the controller exclusively creates one inode-fenced,
mode-0700 authority directory per exact worker generation. Its `ipc` child is
mounted read-write for the two Unix sockets, while its `auth` child is mounted
read-only and contains the controller-generated mode-0600 capability file.
The capability is never placed in argv, environment, labels, or container
inspection metadata, and the worker cannot rewrite it through either mount.
