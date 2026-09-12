# Steam runtime sandbox policy

Steam's runtime creates a nested user namespace and mount sandbox. The Docker
default policy blocks those operations when the outer worker drops all
capabilities. Steam workers therefore use the fixed `steam.json` policy.
Gamescope validation workers retain Docker's default policy.

`docker-default.json` is an unchanged copy from the Moby profiles revision and
SHA-256 recorded in `upstream.json`. `LICENSE.moby` preserves its Apache 2.0
license. Polaris adds only these rules:

* Allow `unshare` when its flags include `CLONE_NEWUSER`.
* Allow `clone` with `CLONE_NEWUSER` on the supported x86 image architectures.
* Allow `mount`, `umount2`, `pivot_root`, and `chroot`. The kernel still requires
  the corresponding capability in the namespace where the operation acts.

The outer worker remains nonroot with all capabilities dropped, no new
privileges, a read only root filesystem, private namespaces, and enforcing
SELinux. The policy keeps the upstream default deny action and other syscall
restrictions, including the AF_ALG restriction. It does not grant `setns`,
unrestricted namespace creation, or host mount authority. Enabling nested user
namespaces exposes additional kernel interfaces; the host must remain patched.

CMake embeds the exact policy bytes and installs a copy under the configured
system data directory using its SHA-256 in the filename. The controller checks
that the file and every parent directory are owned by root, cannot be written
by group or others, and contain no symlinks. Missing or changed bytes refuse
launch before Docker runs. Recovery requires exactly the compacted policy in
Docker's inspected security options. A changed or missing installation does
not prevent stopping an already owned worker.

Use the matching native installation when testing Steam. An arbitrary policy
path, a writable copy in a checkout, or `seccomp=unconfined` is not accepted.
The policy is a host package asset, separate from the locked worker image.

Run the offline provenance and permission checks with:

```sh
python3 containers/multiseat/test_seccomp_profile.py
```

To refresh upstream, fetch a reviewed commit's exact default policy and
license, update `upstream.json`, and reapply only the listed changes. Serialize
`steam.json` with sorted keys, two space indentation, and a final newline so
Docker CLI compaction and recovery serialization agree. Review the full
upstream permission delta, rerun the native launch and recovery tests, and
repeat real Steam sandbox and denied host operation checks. An upstream
refresh changes the installed filename and requires the matching binary.
