# gamescope-polaris patches

Applied in order by `nix/packages/gamescope-polaris/default.nix`.

| # | File | Purpose | Drop when |
|---|------|---------|-----------|
| **01** | `01-pipewire-xbgr-210le-2270.patch` | SPA `xBGR_210LE` + BT.2020/PQ metadata + HDR renegotiate | **[#2270](https://github.com/ValveSoftware/gamescope/pull/2270)** merges |
| 02 | `02-headless-hdr-colorimetry.patch` | Headless real SDR vs HDR EDID/expose | Upstream headless HDR API |
| 03 | `03-pipewire-prefer-dmabuf.patch` | Prefer `SPA_DATA_DmaBuf` when allowed | Upstream if accepted |
| 04 | `04-pipewire-color-mgmt.patch` | `paint_pipewire` screenshot SDR/HDR LUTs | Companion to #2270 |
| **05** | `05-pipewire-rgb10-capture-format-2271.patch` | Black PW frames + R/B swap (NVIDIA) | **[#2271](https://github.com/ValveSoftware/gamescope/pull/2271)** merges |
| **06** | `06-prefer-discrete-gpu-2217.patch` | Headless prefers discrete GPU if unpinned | **[#2217](https://github.com/ValveSoftware/gamescope/pull/2217)** merges |
| **07** | `07-paint-pipewire-eotf-pq.patch` | `EOTF_PQ` when HDR on + SDR-on-HDR defaults | Companion to **#2270** paint path |

```bash
rg -n 'POLARIS-UPSTREAM-REMOVE' nix/patches/gamescope/
```

## 01 = #2270

Vendored from https://github.com/ValveSoftware/gamescope/pull/2270 with
`POLARIS-UPSTREAM-REMOVE` markers only. Delete when #2270 merges.

## 04 + 07 = paint companions to #2270

1. **07** — `outputEncodingEOTF = g_bOutputHDREnabled ? EOTF_PQ : EOTF_Gamma22`
2. **04** — screenshot SDR/HDR LUT sets locked to that EOTF
3. **07** also pins `k_ScreenshotColorMgmtHDR` (`sdrGamutWideness=0`,
   `flSDROnHDRBrightness=203`)

(Raw composite / no-LUT experiment removed — no video for client.)

## 05 = #2271

Not covered by 01/07. Drop when #2271 merges.

## 06 = #2217

Headless device pick without `--prefer-vk-device`. Drop when #2217 merges.
