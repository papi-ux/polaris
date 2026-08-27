# Linux stream paths (plugin contract)

This is the developer contract. User-facing guidance on choosing a mode lives in [Launch modes and capture paths](launch-modes.md).

Polaris models each user-facing Linux streaming option as a **stream path**: a stable id plus three orthogonal concerns.

| Concern | Meaning | Examples |
|---------|---------|----------|
| **Runtime** | Who owns app paint | `labwc`, `gamescope`, none (host) |
| **Capture** | How frames are taken | `wlroots`, `portal`, `kms`, `evdi`, `auto` |
| **Topology** | Host display layout policy | `leave_alone`, `host_virtual`, `swap_primary` |

Config key: `linux_stream_mode = <path id>`. Legacy booleans (`headless_mode`, `linux_use_cage_compositor`, `linux_prefer_gpu_native_capture`) still map to/from primary paths.

## Built-in path ids

| Id | Runtime | Capture | Topology | Status |
|----|---------|---------|----------|--------|
| `headless_stream` | labwc | wlroots | leave_alone | Available (Private Stream) |
| `windowed_stream` | labwc | wlroots | leave_alone | Available (GPU-native preference) |
| `desktop_display` | none | portal | leave_alone | Available (Mirror Desktop / external gamescope) |
| `host_virtual_display` | none | auto | host_virtual | Available |
| `gamescope_stream` | gamescope | portal | leave_alone | **Available** when `gamescope` is on PATH (attach idle or spawn owned) |
| `family_isolated` | — | — | — | **Not registered** until PR #226 wires it (id constant kept for conf parse) |
| `headless_evdi` | — | — | — | **Not registered** until EVDI path wires it (id constant kept for conf parse) |
| `headless_dongle` | none | portal (default; kms optional) | swap_primary | **Available** when `linux_streaming_output` + `linux_primary_output` + auto_manage are set (privacy swap via kscreen-doctor; host ScreenCast after topology prepare) |

Source of truth: `src/platform/linux/stream_path.{h,cpp}` registry.

## Render device (labwc runtime)

The `labwc` paths (`headless_stream`, `windowed_stream`) choose the private
compositor's wlroots render device differently, and it matters on a multi-GPU
host:

- `headless_stream` runs wlroots on the **headless** backend, which owns DRM
  device selection outright — left alone it grabs the first render node it
  enumerates, not necessarily the configured GPU. The runtime therefore pins
  `WLR_RENDER_DRM_DEVICE` to `adapter_name` (the same `/dev/dri/renderD*` used
  for capture/encode) when `adapter_name` is an accessible device path; with
  no usable `adapter_name` it pins to `platf::default_render_device()`, a
  sysfs heuristic that prefers the discrete GPU (an NVIDIA driver — nvidia or
  nouveau — or a ≥1 GiB dedicated pool from amdgpu VRAM / Intel lmem, then
  the larger pool, then the boot display). The VAAPI encoder resolves the
  VAAPI-safe variant of the same default (NVIDIA-bound nodes excluded — no VA
  driver exists there) in place of its old literal `renderD128` fallback, so
  the compositor and the encoder agree on the card either way (issues #354,
  #367). Known gap: an Intel Arc dGPU without lmem sysfs ranks as integrated —
  set `adapter_name` on such hosts.
- `windowed_stream` runs wlroots as a **nested wayland** client of the host
  compositor and inherits its render device from the parent's dmabuf feedback.
  The device is deliberately **not** forced there — overriding it could mismatch
  the parent and break buffer sharing.

Source of truth: `labwc_process_environment_value` in
`src/platform/linux/cage_display_router.cpp`.

## Adding a new path (checklist)

1. **Register** a `stream_path::descriptor_t` in `stream_path::registry()` with a stable id.
2. **Runtime** (if the path needs a private compositor):
   - Implement `stream_runtime::stream_runtime_t` (see `stream_runtime_labwc.cpp`).
   - Extend `stream_runtime::acquire()` for the new `runtime_kind_e`.
3. **Capture** (if not covered by existing portal/kms/wlroots paths):
   - Add grab backend + wire via `capture_kind_e` negotiation in platform init — do not hard-code capture inside the path id switch in `process.cpp`.
4. **Topology** (if rearranging host outputs):
   - Implement prepare/restore hooks keyed by `topology_kind_e` (swap primary / host virtual), callable from session prep — not as ad-hoc booleans.
5. **Policy facade**: `stream_display_policy` maps path → legacy booleans for one release cycle.
6. **UI**: Audio/Video path cards read the same ids; mark `available: false` until the runtime works.
7. **Stats**: set `runtime_backend` + `stream_path_id` via `stream_stats::update_runtime_state` (or rely on policy `backend_name` when idle).
8. **Tests**: selection ↔ legacy round-trip; unavailable apply rejects; launch contract lists only available primary paths.

## Module map (keep boundaries)

| Module | Owns |
|--------|------|
| `stream_path` | Path ids + runtime/capture/topology vocabulary |
| `stream_display_policy` | resolve/apply + legacy bool bridge (one release cycle) |
| `stream_runtime` | Private compositor lifecycle (labwc adapter, gamescope). **Only** `stream_runtime_labwc.cpp` may include `cage_display_router`. |
| `session_media` | **Only** ordered media teardown + post-HTTP stop worker |
| `portal_session` / `portal_grab` | ScreenCast session + process-wide media cache (`release_global_capture`) |
| `pipewire_capture` | PW stream format/copy/dtor |
| `display_topology` | Dongle prepare/restore (kscreen) |
| `process` | App launch + nested kill **after** `session_media` |

Stop callers must not invent a parallel order: confighttp / terminate_impl → `session_media::prepare_for_stop()` → optional `proc::terminate`.

## What not to do

- Do not add a fourth boolean to encode a new mode.
- Do not special-case gamescope/EVDI only inside `cage_display_router` — go through `stream_runtime`.
- Do not call `portal::release_global_capture` from HTTP handlers (use `session_media`).
- Do not report `runtime_backend` empty — use `portal`, `host`, `gamescope`, `labwc`, etc.

## Relation to community PR #226

[Headless Streaming Display](https://github.com/papi-ux/polaris/pull/226) introduces EVDI grab, display swap, Family Mode isolation, and `headless_source` / `headless_swap_mode`. Those map cleanly onto:

- paths `headless_evdi`, `headless_dongle`, `family_isolated`
- topology `swap_primary` + capture `evdi`/`kms`
- optional per-app override (Family Mode) on top of the labwc runtime

Integrate by filling the reserved registry slots and implementing runtime/capture/topology hooks — not by inventing parallel config trees.

## Relation to gamescope

**Shipped on this branch:** `gamescope_stream` is available when `gamescope` is on PATH. `stream_runtime_gamescope` attaches to idle `gamescope-0` (or starts `polaris-gamescope-idle` / spawns owned headless) and wraps app launches into that runtime. Capture stays portal/PipeWire-oriented; nested WSI remains a presentation sub-option (e.g. optional Steam Big Picture via `polaris-gamescope-session`), not a top-level path.

Non-NixOS helper install: [`scripts/install/README.md`](../scripts/install/README.md). Optional private ScreenCast bus is host/packaging-specific.

**Still residual (not path-registry work):** clean stop under load, idle preview without a live stream, and multimode conf helpers that must preserve `browser_streaming`.
