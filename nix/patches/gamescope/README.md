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

## Superseded (removed from package)

| Old | Why gone |
|-----|----------|
| `01-pipewire-xbgr-210le-2270.patch` | Superseded by **10** (full 10-bit PQ offer + paint) |
| `04-pipewire-color-mgmt.patch` | Folded into **10** paint path |
| `07-paint-pipewire-eotf-pq.patch` | Folded into **10** |

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
