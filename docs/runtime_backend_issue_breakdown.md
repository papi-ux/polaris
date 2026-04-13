# Polaris Runtime Backend Issue Breakdown

This is the execution plan for the architecture described in
[runtime_backend_architecture.md](./runtime_backend_architecture.md).

The intent is to move Polaris toward a shared streaming core with replaceable
Linux and Windows backends without blocking current Linux shipping work.

## Phase A: Define shared contracts and isolate current seams

### Issue A1: Add shared runtime state and frame metadata types

Goal:

- Introduce backend-neutral metadata for runtime state, frame transport, frame residency, and frame format.

Primary files:

- `src/platform/common.h`
- `src/video.h`
- `src/video.cpp`
- new `src/core/frame/*` or equivalent staging header if introduced incrementally

Deliverables:

- runtime state type:
  - requested headless
  - effective headless
  - override active
  - backend name
- frame metadata type:
  - transport (`shm`, `dmabuf`, `dxgi_duplication`, `wgc`, `unknown`)
  - residency (`cpu`, `gpu`)
  - format (`bgra8`, `nv12`, `p010`, etc.)
- adapters from existing `platf::img_t` and platform backends into the new metadata

Acceptance:

- Linux and Windows backends can report transport and residency without changing capture behavior yet.

### Issue A2: Split `platf::display_t` responsibilities conceptually

Goal:

- Stop treating the display backend as the owner of session runtime, capture loop, buffer pool, and encoder preparation all at once.

Primary files:

- `src/platform/common.h`
- `src/video.cpp`
- `src/process.cpp`

Deliverables:

- define conceptual interfaces or adapter classes for:
  - session runtime
  - frame source
  - encoder import/conversion
- wrap current display backends with adapters instead of rewriting them

Acceptance:

- `video.cpp` no longer depends on hidden runtime policy decisions inside display backends.

### Issue A3: Add shared policy state for runtime selection

Goal:

- Centralize logic that currently decides headless vs windowed vs GPU-native override.

Primary files:

- `src/process.cpp`
- `src/video.cpp`
- `src/config.{h,cpp}`

Deliverables:

- policy input model:
  - requested headless mode
  - encoder device type
  - `linux_prefer_gpu_native_capture`
  - backend capabilities
- policy result model:
  - chosen runtime mode
  - chosen capture source
  - override active

Acceptance:

- current Linux override logic is expressed through a reusable policy structure rather than inline branching.

## Phase B: Observability and telemetry

### Issue B1: Surface runtime mode and transport in logs

Goal:

- Make runtime behavior explicit in logs for all platforms.

Primary files:

- `src/process.cpp`
- `src/platform/linux/cage_display_router.cpp`
- `src/platform/linux/wlgrab.cpp`
- Windows logging touchpoints as needed

Deliverables:

- log:
  - requested headless
  - effective headless
  - override active
  - backend name
  - actual capture transport

Acceptance:

- a single stream session log makes it obvious whether Polaris is truly headless and whether it is on `DMA-BUF`, `SHM`, `DXGI`, or another path.

### Issue B2: Surface runtime mode and transport in the web UI

Goal:

- Expose the effective runtime state to users during or before streaming.

Primary files:

- `src_assets/common/assets/web/views/DashboardView.vue`
- `src_assets/common/assets/web/views/ConfigView.vue`
- `src_assets/common/assets/web/public/assets/locale/en.json`
- backend API source files that provide session state

Deliverables:

- dashboard/runtime status card or section showing:
  - requested headless
  - effective runtime mode
  - override active
  - actual capture transport

Acceptance:

- users can see why Polaris chose windowed labwc and whether the session is actually GPU-native.

### Issue B3: Move Linux capture profiling behind shared telemetry

Goal:

- Keep the current Linux profiler work, but make it feed a shared telemetry contract.

Primary files:

- `src/platform/linux/wlgrab.cpp`
- new telemetry types or `src/stream_stats.*`
- `src/video.cpp`

Deliverables:

- per-frame timing event model for:
  - dispatch time
  - ingest time
  - total capture time
  - convert time
  - encode time
- transport tag attached to telemetry events

Acceptance:

- future Windows and experimental backends can report into the same telemetry system.

## Phase C: Decouple frame conversion from capture

### Issue C1: Introduce a backend-neutral frame contract into the encode pipeline

Goal:

- Make `video.cpp` consume a richer frame description than raw `platf::img_t`.

Primary files:

- `src/video.cpp`
- `src/video.h`
- `src/platform/common.h`

Deliverables:

- frame wrapper or new frame type used at the encode boundary
- explicit metadata for:
  - location
  - format
  - ownership
  - timestamp

Acceptance:

- `video.cpp` can reason about GPU vs CPU frames without backend-specific assumptions.

### Issue C2: Introduce explicit converter stages

Goal:

- Separate capture from color conversion and memory migration.

Primary files:

- `src/video.cpp`
- `src/platform/linux/cuda.*`
- `src/platform/linux/vaapi.*`
- Windows conversion paths under `src/platform/windows/`

Deliverables:

- converter stage abstraction for:
  - CPU conversion
  - GPU conversion
  - format conversion
  - device migration

Acceptance:

- a frame can stay GPU-resident through conversion when the backend and encoder support it.

### Issue C3: Reduce dependence on CPU-style `img_t`

Goal:

- Stop forcing all backends through a CPU-image mental model.

Primary files:

- `src/platform/common.h`
- `src/video.cpp`
- encoder integration files

Deliverables:

- allow GPU-native frame submission path that does not require a CPU pixel buffer
- keep `img_t` only as a compatibility container where needed

Acceptance:

- zero-copy or near-zero-copy paths no longer look like special cases bolted onto a CPU-first pipeline.

## Phase D: Linux backend work

### Issue D1: Finish Linux runtime observability

Goal:

- Complete the current headless optimization work by surfacing effective mode and transport everywhere needed.

Primary files:

- `src/process.cpp`
- `src/platform/linux/cage_display_router.*`
- `src/platform/linux/wlgrab.cpp`
- dashboard/API/UI files

Acceptance:

- this closes the current `0c` bucket cleanly.

### Issue D2: Labwc startup latency and output mode improvements

Goal:

- Remove fixed sleeps/polling and reduce startup cost.

Primary files:

- `src/platform/linux/cage_display_router.cpp`
- `src/process.cpp`

Deliverables:

- event-driven readiness if possible
- preset output mode handling where useful

Acceptance:

- reduced time to first frame on Linux labwc sessions.

### Issue D3: Buffer pooling and GPU BGRA->NV12 conversion

Goal:

- Eliminate repeat allocation and keep conversion on the GPU where possible.

Primary files:

- `src/platform/linux/wlgrab.cpp`
- `src/platform/linux/cuda.*`
- `src/video.cpp`

Acceptance:

- fewer CPU copies
- reduced p99 capture+convert latency

### Issue D4: DMA-BUF to CUDA zero-copy path

Goal:

- Convert the Linux GPU-native fast path from “GPU import plus extra staging” to direct encoder-friendly submission where possible.

Primary files:

- `src/platform/linux/wlgrab.cpp`
- `src/platform/linux/cuda.*`
- `src/nvenc/*`
- `src/video.cpp`

Acceptance:

- measured reduction in conversion and handoff overhead on NVIDIA.

### Issue D5: Prototype `gamescope` backend

Goal:

- Evaluate whether `gamescope` closes enough of the Linux gap to justify a maintained optional backend.

Primary files:

- new Linux backend files under `src/platform/linux/`
- minimal integration points in `src/process.cpp` and `src/video.cpp`

Deliverables:

- prototype runtime backend
- documented measurements:
  - startup latency
  - first frame latency
  - transport reliability
  - compatibility
- spike plan:
  - [gamescope_backend_spike.md](./gamescope_backend_spike.md)

Acceptance:

- clear go/no-go decision on whether `gamescope` is worth maintaining.

### Issue D6: Polaris-owned Wayland runtime spike

Goal:

- Start the long-term Linux backend with the smallest viable proof of concept.

Scope:

- hidden or headless output
- simple fullscreen app hosting
- frame export path
- direct telemetry

Acceptance:

- spike proves whether Polaris can own the frame lifecycle end-to-end on Linux.
- spike plan:
  - [polaris_wayland_runtime_spike.md](./polaris_wayland_runtime_spike.md)

## Phase E: Windows backend alignment

### Issue E1: Map DXGI/WGC capture into shared frame metadata

Goal:

- Make Windows report the same residency/transport/format data as Linux.

Primary files:

- `src/platform/windows/display.h`
- `src/platform/windows/display_*.cpp`
- shared frame metadata types

Acceptance:

- Windows capture paths can report transport and GPU residency through the same contracts as Linux.

### Issue E2: Align D3D11 textures with shared converter and encoder contracts

Goal:

- Reuse shared pipeline logic instead of letting Windows keep a one-off GPU path.

Primary files:

- `src/nvenc/nvenc_d3d11*.{h,cpp}`
- `src/video.cpp`
- shared conversion interfaces

Acceptance:

- Windows NVENC submission works through the shared frame/encoder contract.

### Issue E3: Define Windows runtime abstraction

Goal:

- Match Linux session/runtime separation on Windows.

Primary files:

- `src/process.cpp`
- `src/platform/windows/*`

Deliverables:

- Windows runtime reports:
  - runtime name
  - effective mode
  - capture source
  - transport type

Acceptance:

- Linux and Windows session backends are selected through the same high-level orchestration path.

## Suggested execution order

Recommended order for the next several sessions:

1. `A1` shared metadata types
2. `A3` shared runtime policy model
3. `B1` runtime mode and transport logging
4. `B2` UI surfacing of effective mode and transport
5. `B3` shared telemetry sink for profiler data
6. `C1` backend-neutral frame contract at the encode boundary
7. `D2` through `D4` Linux fast-path work
8. `D5` `gamescope` prototype
9. `E1` and `E2` Windows alignment
10. `D6` Polaris-owned Wayland runtime spike

## Immediate next issue

If only one issue should be picked up next, it should be:

### `A1 + B1` combined

Reason:

- It is small enough to land incrementally.
- It improves Linux immediately.
- It creates the first shared seam needed for Windows.
- It makes all later profiler and backend work easier to reason about.
