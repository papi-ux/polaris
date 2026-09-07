# Fractional stream timing

Capture intervals and encoder timebases share the requested rational stream
rate. Legacy integral rates and the existing whole-frame budgeting fields stay
available; they no longer determine fractional capture cadence.

Polaris/Nova's explicit millihertz requests remain exact. A request of 59940
represents 2997/50 Hz. It is not silently changed to 60000/1001. For the standard
GameStream whole-FPS request, an optional
`x-nv-video[0].clientRefreshRateX100` hint can supply fractional precision when it
lies strictly within the requested whole rate's rounding interval. Standard
NTSC hints normalize to their rational rates, including 5994 to 60000/1001 and
2397/2398 to 24000/1001. A 12000 display hint accompanying a 60 FPS stream is
ignored. Warp requests and launch-display rates retain their separate meaning;
the encoder limiter uses the launch rate only when the existing limit setting
calls for it.

PipeWire capture records the requested rate as part of capture reuse identity
and logs both requested and negotiated rates. Its initial format offer uses
variable `framerate` and a `maxFramerate` range. KWin versions before 6.8 prefer
variable capture; confirmed 6.8 and newer prefer the requested rate. An unknown
version keeps the variable policy. The version query has an owner-scoped
cancellable and a single 500 ms deadline covering D-Bus acquisition and query.

Matching compatibility offers follow every primary format, accepting fixed-only
producers through a framerate range without `maxFramerate`. Primary offers retain
literal variable framerate so a producer's preferred fixed rate cannot displace
variable capture when both are available. Both passes preserve format ordering,
modifiers, and HDR/SDR constraints.

A producer that rejects the maximum-rate offer before negotiating may retry
once using a legacy framerate-only offer. Retry retains the same node, core,
formats, HDR policy, and DMA-BUF restrictions. It rechecks stop, negotiation,
and launch cancellation after acquiring the PipeWire loop, before replacing the
stream. Session teardown interrupts startup negotiation before waiting for its
start owner to release the admission fence.

Fixed negotiated rates take precedence over a variable maximum. A producer
running at the requested rate or slower delivers frames through the existing
event-driven wait. Faster or variable production uses a cancellable deadline in
the consumer wait, retaining the latest available frame; the PipeWire callback
thread never sleeps to pace capture. Lower negotiated rates are reported rather
than silently relabeled. SPA buffers explicitly satisfy eight-byte alignment.

Regression coverage includes integer/NTSC/explicit-millihertz rates, inconsistent
client display hints, Warp and launch limits, generated SPA properties, actual
PipeWire loop contention during stop and retry, owner cancellation, teardown
fences, and KWin query cancellation/deadline. Native and sanitizer results are
recorded with the candidate. Physical producer negotiation and measured capture
cadence are additional acceptance evidence, not implied by these tests.

`tools/tests/pipewire_rate_harness.py` starts a private PipeWire graph and a
GStreamer synthetic producer, then measures 181 frames through the actual native
capture consumer. It removes its processes and private runtime on exit. A Linux
run measured 60.0008 FPS from a fixed 60 Hz producer, 59.9993 FPS for a 60 FPS
request from a fixed 120 Hz producer, and 59.9423 FPS for 60000/1001. These results
validate synthetic producer negotiation and consumer pacing; they do not establish
desktop-compositor cadence or game-stream performance.
