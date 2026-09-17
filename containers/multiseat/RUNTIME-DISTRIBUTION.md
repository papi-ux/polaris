# Spaces runtime distribution

Polaris remains a native host package. A Steam worker image supplies each
space's isolated launcher, compositor, input and media providers. Downloading
that image does not start a space or change any Steam home.

## Current boundary

The host now has a verified runtime acquisition command:

```sh
polaris --spaces-runtime list
polaris --spaces-runtime install RUNTIME_ID
polaris --spaces-runtime create-first CATALOG RUNTIME_ID REQUEST_ID NAME
```

This is an implementation and validation interface. The guided Spaces job now
calls the same runtime and first-home backend outside the HTTP request thread.
`create-first` obtains an approved runtime and prepares one new private Steam
home plus its owned network and catalog entry. Use a stable UUID request ID and
the same name on retries. An existing catalog cannot be replaced; a matching
retry confirms the same home and preserves later device assignments. It never
adopts an orphaned volume after an uncertain failure. Controller configuration,
GPU selection and restart-based configuration are now a separate guided step;
device assignment uses the existing authenticated controller API after restart.
The shipped catalog is currently empty because no runtime has completed the
publication and catalog admission process below. An unknown runtime fails
before any Docker command. Do not fill the catalog with a guessed digest,
mutable tag, local image ID, or CI artifact download URL.

The installer only uses the system Docker Engine through its local Unix socket.
It clears inherited Docker contexts, credentials and configuration. The only
registry is `ghcr.io/papi-ux/polaris-worker-steam`, addressed by an approved
manifest digest. Installation verifies the resulting configuration digest,
source revision, platform, media contract, launcher, implicit mounts and NVIDIA
driver variant. This follows Docker's
[immutable digest pull interface](https://docs.docker.com/reference/cli/docker/image/pull/).

A successful result means the image is available. It does not prove controller,
GPU, audio, game launch, concurrent streaming or sustained 120 FPS acceptance.
The current runtime identity is still UID/GID 1000. Do not change host user IDs
to satisfy it.

If downloading is interrupted, rerun the same runtime ID. Docker can reuse
verified layers; Polaris re-inspects the complete image on every attempt. It
does not claim byte-level resume or return success from a stale progress record.
The command can wait up to 30 minutes. The web flow uses a host-owned worker,
never an HTTP request or stream owner thread.

## Guided setup job

Authenticated clients read `GET /api/spaces/setup/job` and submit bounded JSON
to `POST /api/spaces/setup/job`. Start accepts only `operation`, `request_id`,
`runtime_id` and `name`; cancel accepts only `operation` and `request_id`.
Activate accepts only `operation`, the prepared `request_id`, and a discovered
`gpu_id`. It cannot change the runtime, profile identity or player home.
Image references, shell commands, paths, GPU devices and controller settings
cannot be supplied through this endpoint.

The main process owns and joins the setup worker. A private cross-process lease
admits one setup owner. Its bounded journal records the request, approved
registry/configuration digests and stage before effects. Repeated requests must
match that identity. Reloading the page reads progress; restarting Polaris marks
unfinished work interrupted and requires an explicit retry. An unavailable
catalog or an existing local controller configuration cannot bootstrap a home.

The installer uses a cancellation-aware host adapter. Its child-process output
drain is bounded per pass so continuous output cannot starve a stop request.
Stopping a download kills the owned Docker CLI; the daemon may finish retaining
layers. It does not prune images or stop other containers. Once home preparation
begins, browser cancellation is fenced. Process shutdown still interrupts host
commands; the existing provisioning transaction retains uncertain resources and
only a durable catalog confirmation can report the home prepared.

A journal durability failure freezes that owner until restart and secure
read-back. No later request overwrites uncertain state. First-home storage is
server-owned at `spaces-profiles.json` under Polaris's application-data directory.
Activation records its exact GPU selection before writing controller configuration.
Discovery pairs render and primary nodes through physical sysfs identity, matches
NVIDIA device minors through the kernel driver, and requires access to every
explicit node. Only one physical GPU is admitted by this first setup. Its initial
seat and encoder budgets are one; this is a conservative admission limit, not a
claim about hardware throughput. Existing configured hosts are not migrated.

The controller catalog is written privately and is immutable on retry. The final
native-configuration patch is the commit point, serialized with other settings
writes and preserving unrelated values. Activation rechecks the prepared profile,
local image, NVIDIA host-driver version, compiled seccomp file and installed
SELinux worker type. Failure preserves player data and requires an explicit retry.
The running process does not install a second controller. An explicit restart
loads configuration through the existing production factory, rechecks physical
GPU identity and recreates only its bound private IPC root after host reboot.
A failed managed startup leaves the web interface available for diagnosis.
Device assignment remains under the existing paired-client authorization path.

The page distinguishes a prepared home, saved configuration awaiting restart and
an available controller. None of these states establishes gameplay acceptance.

## Native packages and container registry

`papi-ux/packages` publishes signed RPM and pacman repositories from stable
Polaris release assets; it does not rebuild them. It remains responsible for
native package signatures, metadata and public read-back. The Polaris native
build packages the Spaces UI/controller and its exact Steam seccomp file.
The dedicated SELinux policies still require a reviewed host integration and
installation path before clean-host setup can be called complete.

The worker image belongs in GitHub Container Registry. Keep its provider receipts,
immutable digest, anonymous read-back and compiled host catalog admission with
the runtime release procedure below. Do not put Docker archives in the dnf or
pacman repository or treat a signed host package as proof of image publication.

## Lab builds

To test Spaces end to end before a runtime is published, keep everything on one machine: run a
registry bound to localhost, copy the exact built OCI archive into it with `skopeo copy` (a
`docker push` re-encodes layers, which the catalog preparation then rejects), prepare a candidate
against that registry's manifest bytes, and configure a Polaris build with
`-DPOLARIS_SPACES_RUNTIME_REPOSITORY=localhost:5000/polaris-worker-steam` and
`-DPOLARIS_SPACES_RUNTIME_CATALOG_FILE=/path/to/lab-catalog.json`. CMake warns that it is a lab
build. Never ship one: release workflows do not set either option, and a unit test fails if one
does.

## Catalog admission

1. Build committed, reviewed source with `multiseat-images.yml`. Use
   `steam_only=true` to validate one Steam variant. `nvidia=true` adds the locked
   NVIDIA userspace variant. This workflow exports artifacts and never publishes.
2. Retain the Docker archive, OCI archive, package manifest, SBOM and all provider
   receipts. Every file in `artifact.json` has a size and SHA-256. All nine real,
   device-free provider tests must pass for the exact worker configuration.
3. After publication is authorized, publish that exact image to the Polaris
   registry using a separate reviewed release operation. Do not rebuild under
   the same identity. Use GitHub's documented
   [Container registry workflow authentication](https://docs.github.com/en/packages/working-with-a-github-packages-registry/working-with-the-container-registry).
   Verify anonymous access so a gamer does not need registry credentials.
4. Fetch the exact single-platform registry manifest bytes and its immutable
   digest. Do not reformat the JSON or confuse the exported OCI digest with the
   registry digest; publication can change the manifest's representation.
5. Prepare a candidate without modifying the trusted catalog:

   ```sh
   python3 containers/multiseat/runtime_catalog.py prepare \
     ARTIFACT_DIRECTORY REGISTRY_DIGEST registry-manifest.json > runtime-candidate.json
   ```

   The tool verifies all file receipts, OCI blobs and ordered layer contents,
   provider coverage, registry/configuration identity and NVIDIA evidence. The
   candidate is evidence for review; it does not authenticate its own publisher.
6. Independently review the exact source, build run, registry publication and
   hardware acceptance scope. Admit the candidate into `runtime-catalog.json`
   through the normal host source review and release process. The catalog is
   compiled into Polaris, so downloaded metadata cannot add trust roots.
7. Validate the host package and runtime together. First-space setup must still
   validate account identity, physical GPU ownership, fresh private storage,
   input access and configuration activation before offering a playable space.

```sh
python3 containers/multiseat/runtime_catalog.py validate containers/multiseat/runtime-catalog.json
python3 -m unittest discover -s containers/multiseat -p 'test_*.py'
```

The Python checks require Python 3.11 or newer and do not use Docker or hardware.
Native `SpacesRuntime.*` tests cover bounded commands, identity mismatches,
interrupted downloads, retry and refusal to create containers or player storage.
`SpacesSetupService.*` covers concurrent requests, stale cancellation,
shutdown, durable recovery, changed runtime identity and owner exclusion.
