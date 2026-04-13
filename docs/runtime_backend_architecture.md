# Polaris Runtime Backend Architecture

## Goal

Design Polaris as a shared streaming core with replaceable platform runtimes.

This keeps the current Linux `labwc` path shippable, but stops Linux compositor details from becoming the product architecture. It also creates a realistic path to a future Windows build that reuses the same session, frame, telemetry, and encoder logic.

## Why Now

Polaris already has meaningful cross-platform pieces:

- Session launch and app orchestration in [src/process.cpp](../src/process.cpp)
- Frame and encode pipeline in [src/video.cpp](../src/video.cpp)
- Platform capture abstractions in [src/platform/common.h](../src/platform/common.h)
- Existing Windows GPU capture paths in [src/platform/windows/display.h](../src/platform/windows/display.h)
- Existing Linux Wayland capture paths in [src/platform/linux/wlgrab.cpp](../src/platform/linux/wlgrab.cpp)

The current problem is not lack of code. It is that `platf::display_t` and `platf::img_t` currently mix several responsibilities:

- session/runtime ownership
- capture transport
- buffer lifetime
- conversion staging
- encoder handoff

That coupling makes Linux headless optimization harder and will make a future Windows build harder.

## Architectural Direction

Split Polaris into two layers:

1. Shared core
- session orchestration
- frame pipeline
- conversion pipeline
- encoder abstraction
- input/audio abstractions
- telemetry and profiling
- policy engine

2. Platform backends
- Linux runtime backend
- Windows runtime backend
- optional experimental backends such as `gamescope`

The core should know whether a frame is GPU-resident or CPU-resident, what format it is in, and what conversions are required. It should not care whether the frame came from DMA-BUF, DXGI duplication, Windows Graphics Capture, SHM, or a Polaris-owned compositor.

## Recommended Role For Existing And Future Backends

| Backend | Role |
|---|---|
| `labwc/wlroots` | Shipping Linux backend and compatibility path |
| `gamescope` | Experimental Linux backend for evaluation and learning |
| Polaris-owned Wayland runtime | Long-term Linux backend |
| DXGI / WGC runtime | Long-term Windows backend |

`gamescope` is worth prototyping, but it should not define the long-term architecture. The long-term architecture should be the shared core plus backend contracts.

## Module Map

### Shared core

Recommended new modules:

- `src/core/session/`
- `src/core/frame/`
- `src/core/encode/`
- `src/core/input/`
- `src/core/audio/`
- `src/core/telemetry/`
- `src/core/policy/`

### Platform backends

Recommended platform layout:

- `src/platform/linux/runtime/`
- `src/platform/linux/capture/`
- `src/platform/windows/runtime/`
- `src/platform/windows/capture/`

The current code does not need a big-bang move. The first step is to define backend-neutral interfaces and adapt existing code behind them.

## Current To Target Mapping

| Current area | Current file(s) | Target responsibility |
|---|---|---|
| App/session launch | `src/process.cpp`, `src/process.h` | `core::session::orchestrator_t` plus backend `session_runtime_t` |
| Frame capture | `src/platform/*/display*`, `src/platform/linux/wlgrab.cpp` | backend `frame_source_t` |
| Frame object | `platf::img_t` in `src/platform/common.h` | `core::frame::frame_t` |
| Display abstraction | `platf::display_t` in `src/platform/common.h` | split into `session_runtime_t`, `frame_source_t`, and backend factories |
| Encode device conversion | `platf::encode_device_t` | `frame_converter_t` and encoder import contract |
| Stream pipeline | `src/video.cpp`, `src/video.h` | `core::encode::pipeline_t` |
| Input injection | `src/input.cpp`, `src/platform/*/input.cpp` | `input_sink_t` |
| Audio capture | `src/audio.cpp`, `src/platform/*/audio.cpp` | `audio_source_t` |
| Runtime policy | `src/process.cpp`, `src/video.cpp`, config flags | `policy_engine_t` |

## Core Contracts

These contracts are the important part of the design. They should be defined before large new Linux-only optimizations harden the wrong seams.

### Frame metadata and ownership

```cpp
namespace core::frame {
  enum class residency_e {
    cpu,
    gpu,
  };

  enum class transport_e {
    shm,
    dmabuf,
    dxgi_duplication,
    wgc,
    d3d11_shared,
    internal,
    unknown,
  };

  enum class format_e {
    bgra8,
    rgba16f,
    nv12,
    p010,
    yuv444p16,
    unknown,
  };

  struct native_handle_t {
    platf::mem_type_e device_type = platf::mem_type_e::unknown;
    void *resource = nullptr;
    int fd = -1;
  };

  struct frame_t {
    residency_e residency = residency_e::cpu;
    transport_e transport = transport_e::unknown;
    format_e format = format_e::unknown;
    int width = 0;
    int height = 0;
    int row_pitch = 0;
    std::optional<std::chrono::steady_clock::time_point> timestamp;
    native_handle_t native;
    uint8_t *cpu_data = nullptr;
  };
}
```

The important rule is that `frame_t` is descriptive. It tells the pipeline what the frame is and where it lives. It does not assume a specific capture stack.

### Session runtime

```cpp
namespace core::session {
  struct launch_request_t {
    std::string app_id;
    std::string display_name;
    bool requested_headless = false;
    bool prefer_gpu_native_capture = true;
    video::config_t video_config;
  };

  struct runtime_state_t {
    bool requested_headless = false;
    bool effective_headless = false;
    bool gpu_native_override_active = false;
    std::string runtime_name;
  };

  class session_runtime_t {
  public:
    virtual ~session_runtime_t() = default;
    virtual int start(const launch_request_t &request) = 0;
    virtual void stop() = 0;
    virtual runtime_state_t state() const = 0;
  };
}
```

This removes compositor policy from `process.cpp` and makes runtime decisions reportable in a backend-neutral way.

### Frame source

```cpp
namespace core::frame {
  class frame_source_t {
  public:
    virtual ~frame_source_t() = default;
    virtual platf::capture_e start() = 0;
    virtual platf::capture_e next(frame_t &frame, std::chrono::milliseconds timeout) = 0;
    virtual void stop() = 0;
  };
}
```

This replaces the current idea that a display backend must also own the whole streaming lifecycle.

### Converter

```cpp
namespace core::encode {
  struct conversion_request_t {
    core::frame::format_e target_format;
    platf::mem_type_e target_device;
  };

  class frame_converter_t {
  public:
    virtual ~frame_converter_t() = default;
    virtual bool supports(const core::frame::frame_t &src, const conversion_request_t &request) const = 0;
    virtual int convert(core::frame::frame_t &frame, const conversion_request_t &request) = 0;
  };
}
```

This is the seam where future Linux `BGRA -> NV12` GPU conversion and future Windows D3D11 texture conversion can share policy while keeping platform code separate.

### Encoder session

This should evolve from the existing `video::encode_session_t`.

```cpp
namespace core::encode {
  class encoder_session_t {
  public:
    virtual ~encoder_session_t() = default;
    virtual int submit(core::frame::frame_t &frame) = 0;
    virtual void request_idr() = 0;
    virtual bool update_bitrate(int kbps) = 0;
  };
}
```

The existing NVENC, VAAPI, and software paths can adapt to this. The goal is to pass ownership and format information without forcing everything through a CPU-centric image contract.

### Audio, input, telemetry

```cpp
namespace core::audio {
  class audio_source_t {
  public:
    virtual ~audio_source_t() = default;
    virtual platf::capture_e sample(std::vector<float> &frame_buffer) = 0;
  };
}

namespace core::input {
  class input_sink_t {
  public:
    virtual ~input_sink_t() = default;
    virtual void mouse_move(int dx, int dy) = 0;
    virtual void key_event(uint16_t code, bool release, uint8_t flags) = 0;
  };
}

namespace core::telemetry {
  struct frame_event_t {
    core::frame::transport_e transport = core::frame::transport_e::unknown;
    bool gpu_native = false;
    std::chrono::microseconds capture_time {};
    std::chrono::microseconds convert_time {};
    std::chrono::microseconds encode_time {};
  };

  class telemetry_sink_t {
  public:
    virtual ~telemetry_sink_t() = default;
    virtual void on_frame(const frame_event_t &event) = 0;
  };
}
```

The current profiler work on Linux should feed this contract rather than staying trapped inside `wlgrab.cpp`.

## Backend Sketches

### Linux backend: shipping path

Backend name:

- `wlroots_runtime_t`

Internal pieces:

- `labwc_session_runtime_t`
- `wlroots_screencopy_source_t`
- `portal_window_capture_source_t`

This backend keeps today’s shipping path:

- app launch inside labwc
- headless or windowed mode based on policy
- DMA-BUF or SHM depending on actual transport
- existing NVENC / VAAPI encode paths

### Linux backend: long-term path

Backend name:

- `polaris_wayland_runtime_t`

Internal pieces:

- minimal output management
- Xwayland support
- input routing
- GPU render target ownership
- direct export to CUDA / VAAPI compatible surfaces

This is the path that can eventually provide true invisible headless plus deterministic GPU-native capture.

### Linux backend: experimental path

Backend name:

- `gamescope_runtime_t`

Purpose:

- evaluate startup cost
- evaluate capture/export behavior
- evaluate whether it closes enough of the gap to delay a custom compositor

`gamescope` should be treated as an experiment or optional backend, not the architectural center of Polaris.

### Windows backend

Backend name:

- `windows_desktop_runtime_t`

Internal pieces:

- `dxgi_duplication_source_t`
- `wgc_source_t`
- D3D11 or D3D12 native texture ownership
- shared NVENC / AMF / QSV encoder contract

Polaris already has strong raw material here:

- DXGI and D3D11 device abstractions in `src/platform/windows/display.h`
- existing NVENC D3D11 paths under `src/nvenc/`

The design goal is to map those resources into `core::frame::frame_t` rather than building a Windows-only encode pipeline.

## Runtime Policy Engine

The current Linux-specific GPU-native override should become a generic policy engine.

Inputs:

- requested headless mode
- preferred performance mode
- encoder device type
- backend capabilities
- actual capture transport

Outputs:

- chosen runtime mode
- chosen capture source
- conversion plan
- telemetry state exposed to UI and logs

This is where product logic belongs. It should not stay scattered through `process.cpp`, `video.cpp`, and backend-specific code.

## Observability Requirements

The shared architecture should always expose:

- requested headless mode
- effective runtime mode
- backend name
- capture transport
- residency of submitted frames
- number of CPU copies per frame
- capture, convert, and encode timings

That information should feed both logs and UI.

## Migration Plan

### Phase A: define contracts and adapt current code

- add backend-neutral frame metadata and telemetry types
- split `platf::display_t` responsibilities conceptually, even if adapters still wrap it
- move runtime policy decisions behind a shared policy layer

### Phase B: make current Linux and Windows paths report through shared telemetry

- pipe Linux capture profiler into shared telemetry
- map Windows DXGI/WGC paths into the same frame metadata
- surface runtime mode and transport in UI

### Phase C: reduce coupling in the encode pipeline

- make `video.cpp` consume backend-neutral `frame_t`
- isolate conversion staging from capture staging
- remove assumptions that every frame must become a CPU-style `img_t`

### Phase D: add new runtime backends

- `gamescope_runtime_t` as experimental Linux backend
- `polaris_wayland_runtime_t` as long-term Linux backend
- `windows_desktop_runtime_t` under the same core contracts

## Concrete Near-Term Work

The next useful steps are:

1. Add shared enums and metadata for frame transport, residency, and runtime state.
2. Expose `requested_headless`, `effective_headless`, `gpu_native_override_active`, and transport in logs and UI.
3. Move current Linux profiler output behind a shared telemetry sink.
4. Wrap current Linux and Windows display paths with adapters instead of rewriting them immediately.
5. Only after those seams exist, continue deeper work on GPU conversion and zero-copy paths.

## Decision

Polaris should keep shipping on the current Linux `labwc` path, but the architecture should immediately move toward a shared core plus replaceable runtime backends.

That is the best path if Polaris is meant to become a serious Linux-and-Windows streaming platform.
