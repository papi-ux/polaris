# Gamescope Backend Spike

This document defines the smallest useful `gamescope` experiment for Polaris.

The goal is not to replace labwc immediately. The goal is to answer one
question with measurements:

Can `gamescope` deliver enough of the Linux headless and GPU-native path to be
worth maintaining as an optional backend?

## Scope

The spike should stay narrow:

- launch a single fullscreen app inside `gamescope`
- export a dedicated Wayland socket for Polaris capture
- report runtime state through the existing runtime metadata path
- capture through the same shared frame metadata and telemetry pipeline used by
  labwc

Out of scope for the first spike:

- desktop-style window management
- multi-window composition policies
- production-grade config UX
- long-term migration of all labwc behavior

## Integration Points

Minimum integration is:

- add a Linux runtime backend selector that can resolve to `labwc` or
  `gamescope`
- implement a `gamescope_display_router` with the same shape as the current
  labwc router:
  - `start()`
  - `stop()`
  - `is_running()`
  - `get_wayland_socket()`
  - `runtime_state()`
- feed the selected backend into:
  - `process.cpp`
  - `stream_stats`
  - dashboard runtime-path surfacing

## Measurements

Collect the same numbers as labwc:

- compositor startup latency
- socket-ready latency
- first captured frame latency
- first encoded packet latency
- actual capture transport:
  - `dmabuf`
  - `shm`
- actual capture residency:
  - `gpu`
  - `cpu`
- p50 and p99 capture timings with `linux_capture_profile`

## Compatibility Matrix

Run at least these combinations:

- NVIDIA + KDE + NVENC
- AMD + KDE + VAAPI
- Xwayland title
- native Wayland title
- app with detached launcher

## Go/No-Go Criteria

`gamescope` is worth keeping only if it meets all of these:

- startup is not materially slower than labwc
- DMA-BUF reliability is better than or equal to labwc on the target stack
- Polaris can still make headless-vs-GPU-native policy decisions cleanly
- app compatibility is good enough for the top launch paths

It is a no-go if any of these hold:

- transport still falls back to SHM in the important NVIDIA path often enough
  that the backend adds maintenance without improving the product
- detached launch behavior becomes materially worse
- startup or teardown becomes meaningfully less stable than labwc

## Deliverable

The spike is successful when Polaris can launch a `gamescope` session, stream
it through the existing pipeline, and produce a written measurement summary
against the current labwc backend.
