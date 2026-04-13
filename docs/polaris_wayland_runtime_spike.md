# Polaris Wayland Runtime Spike

This is the first proof-of-concept plan for a Polaris-owned Linux runtime.

The objective is to stop treating third-party compositors as the permanent
architecture and prove that Polaris can own the frame lifecycle end to end.

## Minimum Viable Scope

The spike should implement only the pieces required to validate the model:

- one hidden or headless output
- one hosted app at a time
- fullscreen placement only
- Xwayland support if required by the hosted app
- frame export through the shared frame metadata contract
- direct telemetry through the shared runtime and capture stats pipeline

Anything beyond that should wait:

- arbitrary window management
- polished input routing
- advanced layout logic
- desktop integration features

## Required Architecture

The spike should be built around these explicit seams:

- session runtime:
  - launch
  - stop
  - health
  - runtime metadata
- frame source:
  - acquire next frame
  - report transport
  - report residency
  - report format
- converter:
  - GPU path first
  - CPU compatibility fallback only where unavoidable
- encoder handoff:
  - compatible with the shared `frame_t` contract

## Success Criteria

The spike is only useful if it demonstrates all of the following:

- Polaris can create and own the runtime without labwc or gamescope
- the runtime can expose frames with the same metadata contract used elsewhere
- telemetry can report startup, capture, convert, and encode behavior
- the hosted app can render without relying on the main desktop session

## Evaluation Questions

The spike should answer these before it grows:

- can Polaris keep frames GPU-resident from capture through encoder handoff?
- can it avoid the SHM fallback that currently hurts true headless operation?
- can it host enough real applications to justify deeper investment?
- what Linux-only code remains, and what can be shared with a future Windows
  runtime layer?

## Expected Deliverable

The spike should end with:

- a runnable prototype backend behind an explicit experimental switch
- runtime-state logging wired into `stream_stats`
- a short measurement summary against labwc
- a decision:
  - continue toward a maintained Polaris runtime
  - stop and keep the shared-core work while relying on external compositors
