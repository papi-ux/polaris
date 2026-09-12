# Polaris runtime ownership

Polaris owns its container construction, seat admission, GPU selection, input
authority, authenticated IPC, audio policy, encoder provider, media routing and
teardown. The images use an official Ubuntu distribution root with explicit
package closures. No other streaming project's image or entrypoint is needed.
This describes implementation ownership, not measured performance superiority.

| Component | Current source and boundary |
| --- | --- |
| Runtime base | Official Ubuntu 26.04 LTS image, digest pinned in `images.lock.json` |
| Steam | Snapshot-pinned `steam-installer` and its explicit amd64/i386 dependencies |
| Heroic | Publisher's pinned 2.22.1 `.deb`, recorded separately from Ubuntu packages in the SBOM |
| Lutris | Snapshot-pinned package, Wine and explicit i386 graphics dependencies |
| Session supervision and media | Polaris source in this repository |
| Nested compositor | Source-built Gamescope, pinned with recursive dependencies |
| Outer display | Retained third-party `gst-wayland-display`, identified by `locks/plugin.json` |
| Host virtual input | Retained Inputtino submodule with reviewed Polaris patches |

The outer display plugin and Inputtino are Games on Whales dependencies. They
remain identified accurately in source locks, submodule metadata and notices.
The display binary ships its MIT notice. A recorded Polaris patch sets the
virtual display model to `Polaris`, including after mode changes; this branding
change does not replace the underlying component. Replacing images does not replace
those libraries, and renaming a dependency does not make it independent.

A Polaris display replacement must preserve real DMA-BUF production, allocation
of the exact GPU, capture dimensions and timing, input delivery only to the
selected seat, socket identity checks and independent teardown. It must pass
the provider tests and simultaneous physical media harness before selection.
An input replacement must additionally preserve controller reports, feedback,
thread ownership, per-seat device identity and the ordinary single-user path.
Existing source history and required copyright notices remain intact.

## Product acceptance still required

The images install launcher packages without signing into accounts, accepting
user agreements, downloading a game or executing its first launch. Steam's
installer downloads the client on first use. Heroic and Lutris may download
runners. The [Steam adapter](../../docs/research/container-multiseat-steam.md)
now provisions a dedicated profile bridge and supervises Big Picture or a typed
game ID. The validation workload retains networking disabled. Host profile
activation is implemented behind an explicit setting; real launcher acceptance,
Heroic and Lutris adapters, and assignment UI remain pending.
The NVIDIA launcher images package amd64 and i386 vendor libraries with the
matching generic graphics dependencies. Build checks verify both ELF ABIs,
file hashes, SONAME links and dynamic dependencies. The Gamescope validation
image uses only amd64. Real 32 bit game execution remains an acceptance gate.

Passing image/provider checks proves dependency presence and isolated provider
behavior. Physical GPU testing must use the exact produced image. Client
playback and latency measurements are required before making compatibility or
performance claims. Production activation stays default-off during this work.
