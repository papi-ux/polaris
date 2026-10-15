# Mission Control

**Mission Control** is the host's cockpit: what is streaming, how well, and the one safe thing to
do about it. A stage shows the running or next title with its artwork, its main action and the
host's readings. When a Space session is running or Spaces needs attention, the Spaces card
appears above it.

## Space sessions

On Linux, the Spaces card appears while a Space session is starting, running, or stopping,
while Spaces is changing, or when it needs attention. A healthy, idle Spaces stays on its own
page. The card lists reported sessions by Space, launcher, device, and state:
**Starting**, **Running**, or **Stopping**. Device names come from the paired device list;
an unknown device is labeled without exposing its identifier. Assigning a device to a Space
does not make it an active session. A running session is a host report, not confirmation that
video, sound, or input works on that device.

The attention badge points to controller problems, runtime driver mismatches, unsupported
launchers, failed runtime changes, or an unsuccessful refresh. **Open Spaces** takes you to
the existing controls. Mission Control does not repair, stop, move, or create a Space.

Activity refreshes about every ten seconds while the page is visible, with backoff on errors.
An unsuccessful refresh leaves the last report visibly marked out of date. Older hosts that
omit activity show it as unknown. Disabled Spaces has no card or continuing activity poll.
The list shows up to 100 sessions and a count for any others.

Desktop Preview is paused when a Space is starting, running, or stopping, or when its activity
cannot be verified. An already open preview closes when the next report detects a Space.
Opening a Linux preview first rechecks activity; it does not resume automatically when a Space
ends. The preview endpoint captures the host display, so it cannot represent a Space's video.
Any separate host stream telemetry remains labeled as such; Space video and input must be
checked on the connected device.

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

**Display Preview** shows a low-rate view of the host capture display, inside the browser.
Expand it to inspect a frame; hide it to save host work. The preview is diagnostic only and does
not change the stream.

**Disconnect Client** ends the owner's session after a confirmation; the app stays paused for the
resume window so the client can come back.

## While the host is idle

The stage features the next title: the most recently launched one, framed with its cover over a
softened copy of it. **Launch** starts it. When a launch check needs attention, the stage says so
and **Open priority fix** comes first. The readings along its lower edge show readiness, paired
devices, the last session, the mode new sessions start in, the GPU and Live Tuning.

**Recent games** is the shelf under the stage, with up to eight titles. Select a poster to feature
it on the stage without starting anything; **Launch** on the poster, shown when you point at it
or move to it with the keyboard and always on a touch screen, starts that title. **Library**
opens the full library. **Host vitals** shows GPU temperature, load, encoder load, and VRAM as
gauges; a gauge without data is not drawn rather than showing a placeholder. The host's route,
with its displays, audio and paired devices, is in **Diagnostics**.

## Stage artwork

The poster on the stage is the entry's own image. Behind it, the stage shows the title's background
from the artwork cache Nova uses, and its logo stands in for the title text; without a background,
a softened copy of the poster stands in, and a logo too dark to read on the stage keeps the text.
The host serves both at `GET /api/apps/<uuid>/artwork/hero` and `GET /api/apps/<uuid>/artwork/logo`,
so the console only ever loads images from the host. When a Steam title has no background or logo
yet, the request answers without one and the host fetches the missing images from Steam once, in
the background; the next view shows them. A title Steam has no
image for is asked again at most once an hour. Nothing is fetched for an entry after **Remove
artwork** or for a Desktop entry, and the console never contacts SteamGridDB.

## In Nova's Host Console

Nova opens this console inside itself, as Host Console. There it shows in the theme Nova uses,
for that visit only; your saved theme does not change, and a theme you pick in the console saves
as usual. **Launch** and Play on the shelf hand the title to Nova, which streams it to the device
in your hands, instead of only starting it on the host. The guide links are hidden there; any
other link to a page outside the console opens in the device's browser.

## Session history

Two lists can appear here. This browser keeps its own list of sessions it watched, with grade,
client, codec, resolution, duration, and averages. When that list is empty, the host's recorded
outcomes per device and game are shown instead; those are the outcomes Doctor and Auto recovery
read. **Clear history** empties both after a confirmation that names what goes away.

## Where the numbers come from

Stream metrics are host telemetry published once per second; Space activity has the separate
refresh interval described above. The same stream evidence feeds
Doctor & Support, so if the two disagree, reload the page before suspecting the host. Session
Snapshot on Doctor & Support shows the same session in more detail, field by field.
