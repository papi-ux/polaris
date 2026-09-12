# Docker worker backend

Docker is the default container engine for the multiseat implementation. This
choice serves deployment compatibility. It makes no claim of better frame rate,
encode time, or input latency than Podman. The normal single-device experience
and the production multiseat activation gate remain unchanged.

## Host and worker boundary

The initial target is a native Linux host with a local, rootful Docker Engine
and runc. The Polaris controller runs as its ordinary nonroot user with
operator-authorized access to that daemon. Docker daemon access grants broad
host authority; this is a different controller trust model from rootless Podman.
There is no new privilege broker in this change.

Workers run with the controller's numeric UID, primary GID, and captured
supplementary groups. They use `--userns=host` so those IDs retain their host
meaning, including when daemon user namespace remapping is configured. This
does not share the host PID, IPC, network, mount, or UTS namespaces. Rootless
Docker is rejected until its device and UID mappings have their own acceptance.

The backend explicitly selects a local Unix socket and clears inherited Docker
context, host, and client configuration. Workers never receive that socket.
Worker roots are read only, capabilities are dropped, privilege escalation is
disabled, networking is disabled, and runtime temporary filesystems have bounded
sizes and explicit ownership. The worker owns a mode-0700 `/tmp` so its
Gamescope provider can safely initialize the private X11 socket directory. Image tags, privileged workers, broad input-device
mounts, and implicit NVIDIA runtime/CDI injection are refused.

The existing GPU and input authority still admits exact character devices for
each seat generation. Docker inspection checks numeric users and groups,
namespaces, capabilities, device permissions, private binds, profile volumes,
temporary filesystems, health commands, bounded logs, and executed routing
metadata. Cleanup continues to target the inspected container ID and generation
even when the runtime, GPU, or input group is no longer available.

The shared adapter retains an explicit Podman option to reproduce older
acceptance receipts. Those receipts do not validate the new Docker path.

## Images and profile state

`prepare-inputs.py` and `build-image.py` default to Docker. Both accept
`--engine=podman` for the earlier image lane. Docker builds use the local default
Buildx builder, require the locked base images to be present, and disable
networking for build instructions. `--pull=false` is Docker's cache preference;
it is not a daemon-wide network sandbox for registry metadata resolution.

Docker artifacts include `worker.docker.tar` for `docker image load`, plus a
verified OCI archive for portable provenance. Conversion preserves the exact
configuration and ordered layer contents. `worker_digest` remains the OCI
manifest digest; `worker_config_digest` identifies Docker's immutable image ID.
For an offline Docker import, use the full `sha256:<64 hex>` value recorded as
`worker_reference`. A registry `name@sha256:<64 hex>` reference is also accepted.
An image ID is checked against the actual inspected container image, not only
its labels. Loading an archive does not promote it into the production catalog.

The current worker image includes UID/GID 1000, and the required provider image
checks run as `1000:1000` with no capabilities. Docker does not add a passwd
entry for an arbitrary numeric `--user`. The streaming worker rejects a UID
missing from its image before starting providers because D-Bus requires that
account. General UID provisioning remains a deployment prerequisite for other
host users; the tests do not establish that support.

Profile volumes must be precreated with Docker's local driver and no driver
options. The worker requires their root to be mode 0700 and owned by its UID.
The operator must initialize a new volume before acceptance; a missing or
redirected volume is rejected, and an incorrectly owned root fails worker startup.
For a fresh volume and an already verified worker image:

```sh
docker volume create pv-seat-a
# Set worker_image to artifact.json's worker_reference after verifying/loading it.
docker run --rm --pull=never --network=none --read-only --user=0:0 \
  --cap-drop=all --cap-add=CHOWN --cap-add=FOWNER \
  --security-opt=no-new-privileges \
  --mount=type=volume,src=pv-seat-a,dst=/profile,volume-nocopy \
  --entrypoint=/bin/sh "$worker_image" \
  -c 'chown "$1:$2" /profile && chmod 0700 /profile' -- "$(id -u)" "$(id -g)"
```

This bounded initialization command runs only for a new profile volume. Seat
workers themselves always run as the nonroot controller user. Repeat with a
distinct volume for the second seat.

## Acceptance

The physical harness defaults to `POLARIS_PHYSICAL_ENGINE=docker`, with
`POLARIS_PHYSICAL_DOCKER`, `POLARIS_PHYSICAL_RUNC`, and
`POLARIS_PHYSICAL_DOCKER_SOCKET` overrides. Its existing exact GPU catalog,
image, private IPC parent, and two-volume requirements still apply. Retain
diagnostics with `--gtest_output=xml:<private receipt path>` because
`RecordProperty` values are absent from ordinary console output.

The optional NVIDIA physical lane explicitly selects the existing reviewed
`polaris_nvidia_worker_t` SELinux domain. Policy installation is a separate
operator step; labeling and exact device restrictions remain in force.

Unit tests establish command and reconciliation behavior. A temporary Docker
create/inspect check and a device-free container check establish CLI field
shapes and writable private runtime ownership. They do not establish AMD or
NVIDIA game streaming, hardware encoding, latency, or Unraid compatibility.
The physical game probe still skips the new encoder provider. Its bounded
codec observation is separate from continuous worker media and client playback.

Polaris still runs on the host in this design. Choosing Docker for seat workers
does not yet provide an Unraid application template or a supported Polaris
controller container. Those need a separate host, input, GPU, and lifecycle
acceptance lane.

References: [Docker run](https://docs.docker.com/reference/cli/docker/container/run/),
[daemon access](https://docs.docker.com/engine/security/protect-access/),
[user namespace remapping](https://docs.docker.com/engine/security/userns-remap/),
[Buildx](https://docs.docker.com/reference/cli/docker/buildx/build/), and
[image save](https://docs.docker.com/reference/cli/docker/image/save/).
