# Add and edit apps

The **Library** page holds every entry Polaris publishes to clients: the Desktop entry, Steam Big
Picture, and the games and launchers you add. This guide covers the page itself and every field
in the app editor. Artwork sources and fixes are on
[Library sources and artwork](https://papi-ux.com/docs/library-and-artwork/).

## The Library page

**Quick Launch** ranks favourite, recent, and launch-ready apps first so the list feels familiar
from the couch, while still surfacing entries that need attention.

**Published apps** is the list clients see. Drag rows to reorder what appears first. Reordering,
like any change to the published list, makes Polaris rebuild it, which can interrupt a running
session. Open an entry to edit it or to export its `.art` launcher file for front ends that want a
direct launch.

**Import games** scans Steam, Lutris, and Heroic for installed titles, keeps entries that are
already published visible so you can spot what is new, and lets you stage several candidates
before one import pass. Imported Steam titles keep their app id and take the Linux launch mode
you have selected when they start.

**Library health** shows import coverage and the host context the library depends on. Keep
entries short and recognisable on a handheld screen, use per-app overrides only where a launcher,
tool, or game needs them, and export `.art` entries when you want favourite launches in another
front end.

## The app editor

Saving writes the launcher profile immediately; there is no separate apply step.

### Client entry

| Field | What it does |
| --- | --- |
| **Application Name** | The name shown on Moonlight and Nova. |
| **Image** | The icon, picture, or box image sent to clients. PNG only; when unset, Polaris sends its default box image. |
| **Game Category** | A classification hint for Auto Quality, detected from Steam genres on import. |
| **Platform and runtime** | Filled in for titles imported from Heroic. Says what the title installs as and what will execute it, such as Windows through Proton-GE. Left blank when Heroic did not record it. |
| **Emulated Gamepad Type** | Which gamepad to emulate for this app, overriding the Input tab's default. |
| **MangoHud Overlay** | Shows GPU, CPU, temperature, and frametime in the stream from the host side. |

### Command path

| Field | What it does |
| --- | --- |
| **Command** | The main application to start. Leave it blank to publish an entry that starts nothing, such as the Desktop. |
| **Working Directory** | Passed to the process; some applications look for their configuration there. Defaults to the parent directory of the command. |
| **Output** | A file that receives the command's output. Ignored when unset. |
| **Detached Commands** | Commands run in the background alongside the app. |

### Prep and state commands

| Field | What it does |
| --- | --- |
| **Command Preparations** | Commands run before the app starts and undone after the session ends. If any preparation fails, the launch is aborted. |
| **Resume/Pause Commands** | The do command runs when the first client connects to an idle app; the undo command runs when the last client disconnects. Clean up in undo what do sets up. |
| **Global prep and state commands** | Per-app switches that include or exclude the host-wide commands from the General tab for this app. |
| **Allow client prepare commands** | Whether the commands a paired device carries may run when this app starts. |

### Runtime behavior

| Field | What it does |
| --- | --- |
| **Exit Timeout** | Seconds to wait for every app process to exit gracefully when quitting; five by default. Zero or below terminates immediately. |
| **Resolution Scale Factor** | Scales the client-requested resolution: 2000x1000 at 120 percent becomes 2400x1200. Only a value other than 100 percent overrides the client's own factor; the stream mode itself is not affected. |
| **Continue streaming until all app processes exit** | Keeps streaming until every process the app started has ended, instead of stopping when the first one does. |
| **Continue streaming if the application exits quickly** | Detects launcher-type apps that close right after starting something else and treats them as detached. |
| **Terminate on Pause** | Ends the app when the last client disconnects instead of keeping it paused for the resume window. |
| **Close desktop Steam for private launches** | When desktop Steam is running as a private stream starts, quits it and waits for it to exit instead of refusing the launch. Unsaved state in that Steam session is lost. |
| **Per Client App Identity** | Gives the app a separate identity per client, so one app can carry different virtual display configurations for different devices. |
| **Use App Identity** | Creates virtual displays under the app's own identity instead of the client's, so each app gets its own display configuration. |
| **Always create Virtual Display** | Creates a virtual display whenever this app starts, regardless of what the client asked for. Needs the virtual display driver on Windows hosts. |
| **Enforce Virtual Display Primary** | Makes the virtual display primary when the app starts. Kept on by default; known broken on Windows 11 24H2. |

### Environment variables

Every command the app runs receives a set of environment variables describing the session:
client, resolution, frame rate, and the like. The editor lists them under the Reference section.
Variables starting with `SUNSHINE_` are kept for compatibility with tools written for that host;
their `POLARIS_` twins are the current names. `SUNSHINE_CLIENT_FPS` carries a fractional value for
fractional refresh rates; if a script cannot read a floating-point number there, enable **ENVVAR
compatibility mode** on the Advanced tab. `POLARIS_CLIENT_FPS` is always fractional.
