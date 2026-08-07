# gamescope-polaris patches

Applied in order by `nix/packages/gamescope-polaris/default.nix`.

Polaris HDR capture patch surface for gamescope PipeWire streaming.

| # | File | Purpose | Drop when |
|---|------|---------|-----------|
| **10** | `10-pipewire-offer-10-bit-BT2020-PQ.patch` | Offer SPA 10-bit BT.2020/PQ formats; paint switches on negotiated format | Upstream HDR PW formats land |
| **11** | `11-pipewire-composite-cursor.patch` | Optional `--pipewire-composite-cursor` | Upstream cursor composite lands |
| **12** | `12-polaris-stamp-version-polhdrN.patch` | Banner `+polhdr2` capability stamp | Functional patches upstream; version floors suffice |
| 02 | `02-headless-hdr-colorimetry.patch` | Headless real SDR vs HDR EDID/expose | Proven redundant with 10 on headless |
| 03 | `03-pipewire-prefer-dmabuf.patch` | Advertise DmaBuf\|MemFd\|MemPtr | Portal path no longer needs multi-type |
| **06** | `06-prefer-discrete-gpu-2217.patch` | Headless prefers discrete GPU if unpinned | **[#2217](https://github.com/ValveSoftware/gamescope/pull/2217)** merges |

## Superseded (kept in `archive/`)

Retired patches move to `archive/`; they are history, not candidates. A patch
sitting next to the live ones without being applied reads as live, so
`scripts/check-nix-patches.py` treats that as an error.

| Old | Why gone |
|-----|----------|
| `01-pipewire-xbgr-210le-2270.patch` | Superseded by **10** (full 10-bit PQ offer + paint) |
| `04-pipewire-color-mgmt.patch` | Folded into **10** paint path |
| `07-paint-pipewire-eotf-pq.patch` | Folded into **10** |

## Checking the stack

```bash
scripts/check-nix-patches.py          # hunk headers, declarations — instant
scripts/check-nix-patches.py --apply  # + apply to the pinned gamescope rev
```

The order above is the order `default.nix` declares, and the order matters:
`patchPhase` stops at the first failure, so a patch that does not apply takes
every patch after it with it. `patch -p1` is what the check and the build both
use — `git apply` refuses the line offsets a drifted upstream produces.

## Capability stamp

```text
+polhdr1  10-bit BT.2020/PQ capture formats
+polhdr2  …and --pipewire-composite-cursor
```

```bash
gamescope --version   # expect +polhdr2
rg -n 'POLARIS-UPSTREAM-REMOVE|polhdr' nix/patches/gamescope/
```

## References

- ValveSoftware/gamescope#2270, #2217, #2126
