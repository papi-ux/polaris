# Mission Control

**Mission Control** is the host's cockpit: what is streaming, how well, and the one safe thing to
do about it. It has two layouts, one while a stream is live and one while the host is idle.

## While a stream is live

The header names the client that owns the session, or how many clients are watching, along with
the running app and how long it has been up. The pill beside it is the capture path in use, in the
same words the Audio/Video page uses; [Launch modes and capture paths](launch-modes.md#check-what-your-stream-is-using)
explains how to read it.

**Live summary strip**:

| Tile | What it shows |
| --- | --- |
| **Quality** | A letter grade and score for the current stream, from the same evidence Doctor reads. |
| **Latency** | Round trip to the client, in milliseconds. |
| **FPS** | Frames encoded per second against the session target. |
| **Loss** | Packet loss on the video path. |
| **Bitrate** | The live encoder bitrate. With Auto Quality on, this is the value the host is actually sending, not the client's request. |
| **Encode** | Encoder time per frame. |

The small pills at the end of the strip count dropped frames, duplicated frames, and frames that
missed their target interval. Live charts under the tiles pause when reduced motion is on.

**Doctor** sits under the strip: a headline, a confidence chip, the Auto Quality state, a
recommendation, and at most one safe action with a confirmation. What the verdicts and actions
mean is in [Fix a bad stream with Doctor](doctor.md).

**Display Preview** shows a low-rate view of what the client is receiving, inside the browser.
Expand it to inspect a frame; hide it to save host work. The preview is diagnostic only and does
not change the stream.

**Disconnect Client** ends the owner's session after a confirmation; the app stays paused for the
resume window so the client can come back.

## While the host is idle

The readiness card tells you whether the host can stream right now and, when it cannot, the first
thing to fix. **Host vitals** shows GPU temperature, load, encoder load, and VRAM as gauges; a
gauge without data is not drawn rather than showing a placeholder. **Quick Launch** lists recent
games so you can start the next session from the host.

## Session history

Two lists can appear here. This browser keeps its own list of sessions it watched, with grade,
client, codec, resolution, duration, and averages. When that list is empty, the host's recorded
outcomes per device and game are shown instead; those are the outcomes Doctor and Auto recovery
read. **Clear history** empties both after a confirmation that names what goes away.

## Where the numbers come from

Everything on the page is host telemetry published once per second. The same evidence feeds
Doctor & Support, so if the two disagree, reload the page before suspecting the host. Session
Snapshot on Doctor & Support shows the same session in more detail, field by field.
