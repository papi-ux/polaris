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

**Import games** scans Steam, Lutris, Heroic and the ROM folders you register for installed
titles, keeps entries that are already published visible so you can spot what is new, and lets you stage several candidates
before one import pass. Imported Steam titles keep their app id and take the Linux launch mode
you have selected when they start.

Importing a Lutris or Heroic title also publishes an entry for the launcher itself, once, using the command that exists on this host. Those imports launch straight into a game, so without it there is no way to reach the launcher from a stream to install something or fix a login. An entry you added by hand is recognised and not duplicated.

### ROM folders

Emulator games come in the same way; [Emulators and ROM folders](emulators.md) walks the whole
path, from keys and BIOS files to what happens on End Session. In the import console, add a folder and pick the emulator
that loads it. Polaris lists every file with a matching extension as a candidate, named from its
filename with the region and version tags removed and `Legend of Zelda, The` read as `The Legend
of Zelda`, and skips update and DLC dumps (`[v65536]`, `[UPD]`, `(DLC)`, or a folder called
`updates` or `dlc`). A second dump of the same game, another region say, does not become a second
entry. The folder is remembered, so **Rescan Sources** finds games you add later.

An imported entry launches the emulator straight into the game, fullscreen where the emulator has
a switch for it, with the **Emulated Gamepad Type** set for the platform: a Switch Pro pad for
Eden, a DualSense for the PlayStation emulators. A cover that already sits next to the game
(`<name>.png`, or in a `covers`, `boxart` or `media` folder beside it), in ES-DE's downloaded
media for that system, or among RetroArch's boxarts for it, is copied in at import, so the entry
has art without any key; otherwise covers come from SteamGridDB when an API key is set in Settings,
like any entry without artwork of its own. The emulator itself is published once
next to its games so its own UI stays reachable from a stream.

Polaris looks for each emulator in this order: the emulator file you set on the folder (an
AppImage, say), a binary on the service's `PATH`, then the Flatpak. A folder whose emulator is not
installed still imports; the entries launch once it is. The folder card also names what the
emulator still lacks before a game boots, with the fix: Eden's `prod.keys`, Cemu's `keys.txt`, a
BIOS image for DuckStation or PCSX2, or a Flatpak that has not been allowed to read the folder.
Library health counts the folders that are ready.

| Emulator | Files | Command Polaris writes |
| --- | --- | --- |
| Eden (Nintendo Switch) | nsp, xci, nca, nro, nso | `eden -f -g '<rom>'` |
| Dolphin (GameCube and Wii) | iso, gcm, wbfs, rvz, ciso, gcz, wia, wad, dol, elf | `dolphin-emu -b -e '<rom>'` |
| Cemu (Wii U) | wua, wud, wux, rpx | `Cemu -f -g '<rom>'` |
| DuckStation (PlayStation) | cue, chd, iso, pbp, m3u, img, ecm, mds | `duckstation-qt -batch -fullscreen '<rom>'` |
| PCSX2 (PlayStation 2) | iso, chd, cso, zso, gz, bin, elf | `pcsx2-qt -batch -fullscreen '<rom>'` |
| PPSSPP (PlayStation Portable) | iso, cso, chd, pbp | `PPSSPPSDL '<rom>'`, fullscreen follows PPSSPP's own setting |
| mGBA (Game Boy Advance) | gba, gb, gbc, sgb | `mgba-qt -f '<rom>'` |

A Flatpak runs as `flatpak run <app id>` with the same arguments, and it has to be allowed to read
the folder: Flatseal, or `flatpak override --user --filesystem=<folder> <app id>`. For any other
emulator choose **Custom command**, give the command with `{rom}` where the file goes, for example
`retroarch -f -L ~/.config/retroarch/cores/snes9x_libretro.so {rom}`, and the extensions to look
for. The file path is single-quoted when it is substituted, and `~/` at the start of an argument is
expanded; the command runs without a shell, so nothing else is.

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
| **Image** | The icon, picture, or box image sent to clients: PNG, JPEG or WebP. When unset, Polaris sends its default box image. **Find Cover** searches SteamGridDB through the host for the name you type and shows every game it matches, with its title and year. Picking one saves that poster in Polaris's covers directory and puts its path here; the entry uses it once you save. The search needs the SteamGridDB API key from Settings, General, and says so, with a link to that field, when the key is missing. |
| **Artwork in Nova** | How Nova gets posters, backgrounds, logos and icons for a saved entry, with **Remove artwork** and **Find artwork again**. See [Artwork in Nova](#artwork-in-nova). |
| **Game Category** | A classification hint for Auto Quality, detected from Steam genres on import. |
| **Platform and runtime** | Filled in for titles imported from Heroic. Says what the title installs as and what will execute it, such as Windows through Proton-GE. Left blank when Heroic did not record it. |
| **Emulated Gamepad Type** | Which gamepad to emulate for this app, overriding the Input tab's default. |
| **MangoHud Overlay** | Shows GPU, CPU, temperature, and frametime in the stream from the host side. |

### Artwork in Nova

An entry's image is its poster in Nova. A new image, including a second cover picked into the
same file, replaces the poster the next time Nova loads the library, and clearing the image lets
the next source show.

When Nova shows an entry without artwork of its own, Polaris looks it up. A Steam title gets
Steam's images, and with a SteamGridDB key set in Settings the rest comes from its SteamGridDB game,
found by app id. Any other entry gets SteamGridDB artwork only when a search finds a game with
exactly the entry's name, ignoring case, punctuation and trademark signs. A similar name is never
used, so an entry SteamGridDB does not know keeps its own image. Desktop, Low Res Desktop and any
other entry that streams the desktop never look anything up, and artwork an earlier version
downloaded for them is removed.

**Remove artwork** deletes every picture Polaris downloaded for a saved entry, and any you picked
in Nova, and stops Polaris looking up new ones. The entry's own image stays, and Nova shows it the
next time its library loads. **Find artwork again** turns the lookup back on; it does not bring the
deleted pictures back. Picking artwork in Nova still works after a removal.

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
| **Exit Timeout** | Seconds to wait for every app process to exit gracefully when quitting; five by default. Zero or below terminates immediately. In a private-compositor session the wait is at least two seconds and at most thirty, so an emulator can finish writing its save. |
| **Resolution Scale Factor** | Scales the client-requested resolution: 2000x1000 at 120 percent becomes 2400x1200. Only a value other than 100 percent overrides the client's own factor; the stream mode itself is not affected. |
| **Continue streaming until all app processes exit** | Keeps streaming until every process the app started has ended, instead of stopping when the first one does. |
| **Continue streaming if the application exits quickly** | Detects launcher-type apps that close right after starting something else and treats them as detached. |
| **Terminate on Pause** | Ends the app when the last client disconnects instead of keeping it paused for the resume window. |
| **Mirror the host desktop** | Linux only. Streams the desktop you see on the host whatever launch mode the host uses, and ignores **Always create Virtual Display** for this entry. The bundled Desktop entry has it on, which is why Desktop shows your real desktop on a Private Stream host. Turn it off, or add an entry with no command, to get an empty private session to launch things into. A Desktop Takeover launch ignores it. |
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
