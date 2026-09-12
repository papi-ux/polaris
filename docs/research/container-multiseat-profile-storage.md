# Saved multiseat profiles

The private profile catalog joins persistent Docker storage, a pinned runtime
image, a typed workload, and paired device assignments. It supplies the existing
controller factory through `production_controller_options_t::profile_catalog`.
Production activation remains off by default. The
[profile launch integration](container-multiseat-launch-integration.md) connects
an explicitly configured host to this catalog. The ordinary streaming path does
not read it, and an enabled controller with an empty catalog remains inert.

## Administrative command

Run these commands as the ordinary Polaris service user. Use an absolute catalog
path inside a private directory owned by that user. This early subcommand does
not start a streaming host or initialize its unrelated user configuration.
Keep the persistent catalog outside the controller's dedicated IPC authority
directory, which admits only worker authority records.

```sh
polaris --multiseat-profiles init "$catalog_path"
polaris --multiseat-profiles list "$catalog_path"
polaris --multiseat-profiles create "$catalog_path" "Living room" "$worker_image_id"
polaris --multiseat-profiles assign "$catalog_path" "$profile_id" "$paired_device_id"
polaris --multiseat-profiles unassign "$catalog_path" "$paired_device_id"
```

`worker_image_id` is a locally built immutable `sha256:` image ID with 64 lowercase
hex digits. Create currently provisions the implemented Gamescope
`input-pong-v1` validation workload. The image must identify itself as the
Gamescope runtime and must not declare implicit volumes. No image is pulled.
Current images require the service user's UID and GID to both be 1000; other
identities fail before provisioning.

Create prints opaque profile and volume identifiers. Friendly names and paired
device identifiers remain in the private catalog. Assigning a device does not
pair it or grant streaming permission: the existing authenticated launch path
still enforces pairing, revocation, and launch permissions. Multiple devices may
share a profile; the registry permits only one active seat for that profile.
Different profiles receive different volumes. A device must be explicitly
unassigned before moving it to another profile.

## Storage transaction

The version 1 JSON catalog records the service UID and GID and a bounded array of
profiles. Each profile has exactly `id`, `name`, `volume`, `family`, `image`,
`target`, and `clients`. Duplicate object keys, unknown fields, unsupported
versions, duplicate profile or volume identities, conflicting device assignments,
mutable image references, and invalid workload identifiers are rejected.
Limits are 4 MiB, 4096 profiles, 4096 devices per profile, and 65536 devices total.

Catalog access uses the existing private state file implementation: owned regular
files, private permissions, no symlink or hardlink admission, a cross process
exclusive lock, and an atomic durable replacement. Initialization never replaces
an existing file. A controller keeps the lock until shutdown proves that streams,
workers, and input resources are closed. An incomplete shutdown retains the lock.
Administrative operations fail promptly while a controller owns it. Manual file
edits bypass this cooperative lock and are unsupported while the controller runs.

The controller rejects mixing a saved catalog with inline profiles, workloads,
or routes. Saved storage uses the same local Docker socket and explicit Docker
and runc executables as provisioning. The factory derives backend storage,
workload allowlists, and paired device routes from the one snapshot, then checks
the service UID and GID before constructing input dependencies.

## Fresh volume provisioning

Create holds the catalog transaction while it validates the whole proposed
catalog, admits the local Linux Docker Engine and immutable image, and proves a
random opaque volume name absent from a successful bounded inventory. It creates
a local driver volume with no driver options and an opaque ownership label, then
checks that identity before and after initialization.

The initializer mounts only that volume with `volume-nocopy`. It has no network,
GPU, input devices, host directories, or Docker socket. It uses runc, the host
user namespace, a read only root filesystem, no new privileges, bounded resources,
and only CHOWN and FOWNER capabilities. Fixed Python code requires an empty,
root owned directory, changes only its root ownership to 1000:1000 and mode 0700,
syncs the directory, and verifies the result. It never recursively changes or
adopts an existing home. The catalog is published only after successful completion.

A failure or crash after volume creation can leave a labeled orphan volume or an
initializer container. No automatic deletion occurs. A returned error reports
the opaque volume and initializer names when available. After a crash, inspect
resources labeled `io.polaris.multiseat.profile`; compare their opaque IDs with
the catalog before any manual recovery. If persistence reports uncertain
durability, the replacement may already be visible: read the catalog back before
retrying. Do not delete storage based only on the command's failure status.

## Remaining integration

This provides administrative storage and controller ingestion. Explicit host
configuration, controller ownership, reconciliation, and HTTP launch activation
are implemented behind the default-off profile launch setting. Settings UI and
pairing UI assignments remain separate work.
Steam, Heroic, and Lutris have typed catalog families and built runtime images,
but their game launch adapters and provisioning network policies remain pending.
The catalog does not claim real game or client playback acceptance.

The runtime images use Polaris builds from official Ubuntu. Required notices and
source provenance for retained third party dependencies remain intact.
