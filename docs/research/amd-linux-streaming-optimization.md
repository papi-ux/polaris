# AMD Linux Streaming Optimization Research

This document captures the public research and repo seams for improving Polaris and Nova on AMD/Linux hosts. It intentionally records generalized technical patterns only; do not add private support-bundle contents, IPs, hostnames, pairing identities, screenshots, or private community thread details here.

## Baseline

- AMD/Linux baseline support should assume **Mesa VA-API** first.
- Conservative Headless Stream can legitimately use **SHM/system-memory capture** and still be a valid, stable baseline.
- GPU-native DMA-BUF capture is an optimization path, not a promise. Treat `linux_prefer_gpu_native_capture = enabled` as a request that may still fall back.
- KMS/DRM capture can reduce compositor-copy overhead on some AMD hosts, but it changes the privilege/safety posture and must remain an advanced opt-in path.
- Nova should consume Polaris host truth and explain host-side capture facts separately from Android decoder/network facts.

## Public Sources

- Sunshine docs: <https://docs.lizardbyte.dev/projects/sunshine/latest/>
  - Sunshine publicly lists Linux VAAPI support for AMD/Intel, while AMF is not the normal Linux baseline in the public compatibility table.
- Sunshine Linux platform overview: <https://deepwiki.com/LizardByte/Sunshine/8.2-linux-platform-implementation>
  - Linux capture implementations choose among KMS/DRM, Wayland/PipeWire-style capture, and hardware acceleration paths such as VA-API/CUDA/Vulkan interop.
  - KMS requires privileged DRM framebuffer access; do not recommend it as blind first-line troubleshooting.
- Foundation/Sunshine Linux capture notes: <https://deepwiki.com/qiin2333/foundation-sunshine/4.1.2-linux-capture-methods>
  - Linux capture can produce frames in system RAM or GPU VRAM depending on capture method and encoder requirements.
  - Wayland capture depends on compositor protocol support and DMA-BUF surface descriptors.
- Arch Wiki hardware video acceleration: <https://wiki.archlinux.org/title/Hardware_video_acceleration>
  - AMD open drivers support VA-API through Mesa, Vulkan Video through RADV, and AMF as a separate framework/runtime path.
- Wayland DMA-BUF protocol: <https://wayland.app/protocols/linux-dmabuf-unstable-v1>
  - DMA-BUF import depends on device/modifier compatibility and can fail even when the protocol exists.
- CachyOS AMD Sunshine field writeup: <https://thenets.org/posts/game-streaming-cachyos-amd-sunshine/>
  - AMD community setups commonly converge on Mesa VAAPI, explicit `/dev/dri/renderD*` adapter selection, `vainfo` profile checks, and KMS only after baseline validation.
- Mesa RADV docs: <https://docs.mesa3d.org/drivers/radv.html>
  - RADV is the Mesa Vulkan driver for modern AMD GPUs and Steam Deck-class systems.
- AMD AMF SDK: <https://github.com/GPUOpen-LibrariesAndSDKs/AMF>
  - Linux AMF exists, but runtime packaging/driver coupling makes it a future spike rather than the default AMD/Linux support path.

## Product Principles

1. **Mesa VAAPI first.** Optimize the common open-driver path before proprietary or exotic alternatives.
2. **Capture truth beats optimistic labels.** Surface requested GPU-native, actual DMA-BUF GPU capture, SHM fallback, encoder upload, render-node mismatch, and cross-GPU risk distinctly.
3. **KMS is advanced opt-in.** It may help performance, but evidence and safety explanation come first.
4. **Render-node selection is first-class.** Mixed-GPU systems need explicit capture device vs encoder adapter reporting.
5. **HEVC is the safe AMD default.** AV1 should require capability and stability evidence, especially for handheld clients.
6. **Nova must stay host-aware.** Host VAAPI/SHM warnings are different from Android MediaCodec decoder warnings and network warnings.
7. **Experimental capture paths must be measurable and reversible.** Probe results, fallback reasons, and support-bundle data should explain what happened without requiring a crash archaeology degree.

## Polaris Seams

- `src/platform/linux/wlgrab.cpp`
  - Chooses RAM/SHM capture, wlroots DMA-BUF capture, and experimental `ext-image-copy-capture` paths.
  - Good home for probe/fallback logging and render-node safety decisions.
- `src/platform/linux/vaapi.cpp` / `src/platform/linux/vaapi.h`
  - Own VAAPI vendor/profile/entrypoint/rate-control decisions.
  - Good home for structured capability summaries such as H.264, HEVC Main, HEVC Main10, and AV1 encode availability.
- `src/stream_stats.cpp` / `src/stream_stats.h`
  - Already exports capture transport, capture residency, encode target residency, reason strings, and capture decisions.
  - Good home for a nested `linux_gpu_profile` object shared by diagnostics and APIs.
- `src/nvhttp.cpp` / `src/nvhttp.h`
  - Own `/polaris/v1/session/status`, `/polaris/v1/stream-policy`, optimizer, and launch-policy payloads.
  - Good home for API-level host truth consumed by Nova.
- Web UI:
  - `src_assets/common/assets/web/views/DashboardView.vue`
  - `src_assets/common/assets/web/views/TroubleshootingView.vue`
  - `src_assets/common/assets/web/configs/tabs/AudioVideo.vue`
  - `src_assets/common/assets/web/diagnostics-export.js`
  - `src_assets/common/assets/web/public/assets/locale/en.json`

## Nova Seams

- `app/src/main/java/com/papi/nova/api/PolarisApiClient.kt`
  - Parse new Polaris host-profile fields.
- `app/src/main/java/com/papi/nova/api/PolarisSessionStatus.kt`
  - Model host capture/encoder truth.
- `app/src/main/java/com/papi/nova/api/PolarisClientSettings.kt`
  - Carry effective policy truth when Polaris includes it in client settings.
- `app/src/main/java/com/papi/nova/ui/NovaHudUiState.kt`
  - Command Center/HUD labels such as `VAAPI + SHM fallback`, `VAAPI + GPU-native`, and `render-node mismatch`.
- `app/src/main/java/com/papi/nova/ui/StreamPolicyUiState.kt`
  - Human-readable launch/policy summaries.
- `app/src/main/java/com/papi/nova/manager/ClientRuntimeSnapshot.kt`
  - Keep client decoder/display telemetry separate from host GPU/capture telemetry.

## Proposed `linux_gpu_profile` Shape

The exact API shape should be locked by tests before implementation, but the useful fields are:

```json
{
  "encoder_api": "vaapi",
  "encoder_adapter": "/dev/dri/renderD128",
  "capture_device": "/dev/dri/renderD128",
  "adapter_matches_capture_device": true,
  "gpu_native_requested": false,
  "gpu_native_attempted": false,
  "gpu_native_succeeded": false,
  "vaapi_vendor": "Mesa Gallium",
  "vaapi_profiles": {
    "h264_encode": true,
    "hevc_encode": true,
    "hevc_main10_encode": true,
    "av1_encode": false
  }
}
```

Start with facts Polaris already knows (`encoder_api`, adapter paths, requested/succeeded booleans) before adding live VAAPI profile probing.

## Evidence Checklist for AMD Reports

Ask for one fresh stream attempt and either a support bundle or equivalent host-side evidence:

```bash
polaris --version
vainfo
grep -E 'headless_mode|linux_use_cage_compositor|linux_prefer_gpu_native_capture|encoder|adapter_name' ~/.config/polaris/polaris.conf
journalctl --user -u polaris --since '15 minutes ago' --no-pager
curl -skS https://127.0.0.1:47984/polaris/v1/session/status
curl -skS https://127.0.0.1:47984/polaris/v1/stream-policy
```

Interpretation rules:

- `SHM/system-memory` is not automatically a bug on conservative Headless Stream.
- If GPU-native was requested but capture remains SHM, report that as “requested but unavailable/fell back,” not as user error.
- If capture device and encoder adapter differ, suspect render-node mismatch before blaming the client.
- If FPS target is 120 but delivery lands around 60, split negotiation/target facts from actual delivery facts.
- If AV1 appears during pacing instability, recommend HEVC until capability and stability are proven.

## Open Questions

- Should Polaris serialize `linux_gpu_profile` from `stream_stats::stats_t`, from `nvhttp` policy builders, or from a shared helper used by both?
- Can VAAPI profile probing be deterministic enough for unit tests, or should CI rely on source guards and live smoke evidence?
- Which VAAPI vendor strings need stricter AMD-specific rate-control behavior?
- Should ext-image-copy DMA-BUF probe results be cached per compositor socket and render node?
- Should Nova recommend a conservative AMD profile directly, or show the host truth and let Polaris optimizer own policy changes?
- Which hardware lane validates first: Steam Deck/Bazzite APU, RDNA2 dGPU, RDNA3 dGPU with AV1, or mixed render-node host?

## Release Lane Recommendation

1. Add this research artifact.
2. Add Polaris `linux_gpu_profile` truth in stream stats and v1 APIs.
3. Add AMD-aware Polaris web/support-bundle guidance.
4. Add Nova parser/model support and concise HUD/Command Center labels.
5. Spike VAAPI profile/rate-control policy only after truth surfaces are available.
6. Spike experimental AMD GPU-native capture probing only after fallback reasons are visible.
7. Update public docs/community reply kit with conservative baseline, `vainfo`, render-node selection, SHM meaning, and KMS caveats.
