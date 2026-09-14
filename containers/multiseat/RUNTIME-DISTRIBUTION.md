# Spaces runtime distribution

Polaris remains a native host package. A Steam worker image supplies each
space's isolated launcher, compositor, input and media providers. Downloading
that image does not start a space or change any Steam home.

## Current boundary

The host now has a verified runtime acquisition command:

```sh
polaris --spaces-runtime list
polaris --spaces-runtime install RUNTIME_ID
```

This is an implementation and validation interface. The guided Spaces download
job and first-space configuration transaction still need to call this backend.
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
The command can wait up to 30 minutes and must run on the setup job worker,
never on an HTTP request or stream owner thread. It must not be wired into the
web flow until the job's cancellation and restart lifecycle is implemented.

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
