# Emulators and ROM folders

Polaris can fill your library from a folder of games and launch the emulator
straight into each one. This guide is the whole path: what the emulator needs
before a game boots, how the folder becomes entries with covers, and what to
expect once you press Play. The import mechanics themselves, the preset table
and the custom command form, are in [Add and edit apps](apps.md#rom-folders).

## Before you import

**Install the emulator on the host.** Polaris finds it in this order: the file
you point the folder at (an AppImage in `~/Apps`, say), a binary on the
service's `PATH`, then the Flatpak. A folder whose emulator is missing still
imports; the entries launch once it is installed. Polaris runs as a service
with its own `PATH`, so an emulator that only lives in `~/.local/bin` or a
shell alias is better named as the folder's emulator file.

**Give it its keys or BIOS.** An emulator that cannot decrypt or boot a game
shows a black screen on the stream and says why only on its own window, which
you are not looking at. The folder card in the import console names each
missing piece with the fix:

| Emulator | It needs | Where Polaris looks |
| --- | --- | --- |
| Eden | `prod.keys` (and firmware for some games) | `~/.local/share/eden/keys/`, the Flatpak's `~/.var/app/dev.eden_emu.eden/data/eden/keys/`, or `user/keys/` next to a portable AppImage |
| Cemu | `keys.txt` for encrypted dumps (`wud`, `wux`); `wua` and `rpx` load without it | `~/.local/share/Cemu/`, `~/.config/Cemu/`, the Flatpak's data folder, or next to a portable launcher |
| DuckStation | a PlayStation BIOS image (`.bin`) | `~/.local/share/duckstation/bios/` or the Flatpak's `bios/` |
| PCSX2 | a PlayStation 2 BIOS image | `~/.config/PCSX2/bios/` or the Flatpak's `bios/` |
| Dolphin, PPSSPP, mGBA | nothing | |

**Let a Flatpak read the folder.** A Flatpak emulator only sees what its
permissions allow. Polaris reads the app's metadata and your overrides and,
when the folder is outside them, prints the exact command on the card:

```sh
flatpak override --user --filesystem=/path/to/your/roms dev.eden_emu.eden
```

Flatseal does the same thing with a switch. Recheck the folder afterwards.

**Set up the controller once, inside the emulator.** Polaris gives the stream
a virtual pad that matches the platform: a Switch Pro Controller for Eden, a
DualSense for DuckStation, PCSX2 and PPSSPP, your host default for the rest
(the entry's **Emulated Gamepad Type** changes it). Most emulators map a known
pad automatically. If one does not, open the emulator's own entry from Nova
(Polaris publishes it next to its games) and map the pad in its settings while
the stream is running, so the emulator sees the same device it will see later.

## Import the folder

1. Open **Apps**, then **Import Games**, and under **ROM folders** choose
   **Add folder**. Give the folder, pick the emulator, and add the emulator
   file only when Polaris should run that copy rather than the one on `PATH`
   or the Flatpak. **Custom command** covers any emulator not in the list:
   give the command with `{rom}` where the game file goes and the file
   extensions to look for.
2. The folder card shows what the emulator still lacks. Fix those first; an
   import works either way, but the game will not boot until they are there.
3. Polaris lists every file with a matching extension as a candidate, named
   from its filename with the region and version tags removed, and skips update
   and DLC dumps. Stage what you want and import.
4. **Rescan Sources** later finds games you added since. The folder is
   remembered; **Remove** forgets the folder and keeps the entries you imported.

Each entry launches the emulator straight into its game, fullscreen where the
emulator has a switch for it, and ends the stream when the emulator exits. The
emulator itself gets one entry too, so its own settings and Steam-style shops
stay reachable from a stream.

## Covers

An entry gets a cover without any account or key when one already exists on
the host: next to the game as `<name>.png` or in a `covers`, `boxart` or
`media` folder beside it, in ES-DE's downloaded media for that system, or among
RetroArch's boxarts for it (native or Flatpak). The first match is copied into
Polaris's covers folder at import. Anything else falls back to SteamGridDB when
an API key is set in Settings, like any entry without artwork of its own; a
tidy name helps that search, which is why the tags are stripped.

If you already run ES-DE, let it download media first and then import in
Polaris; you get its covers for free.

## Playing

- **Nova shows what an entry is.** The tile reads the console and the emulator
  ("Nintendo Switch · Eden"), Nova's source filter offers Emulator, and the
  console's Apps page has an Emulators filter.
- **Face buttons.** The virtual Switch Pro maps A to the east button by name,
  so an Xbox-layout pad confirms with its south button in a Switch game. If you
  would rather have positions match, open the game in Nova, then **Play Setup →
  Face buttons → Match positions**. It is remembered per game; the Settings
  flip stays the default for everything else.
- **Saves.** Emulators write saves when they exit, and End Session in a private
  stream sends the emulator a polite request first. It gets the entry's **Exit
  Timeout** to finish (ten seconds for imported entries, at least two, at most
  thirty) before Polaris insists. Raise it in the entry if an emulator needs
  longer; lower it if you prefer a snappier stop and the emulator saves as it
  goes.
- **Quitting the game inside the emulator** ends the stream too, since the
  emulator is the app. Reopen it from Nova.

## Living with other front ends

ES-DE and Polaris can share a ROM tree; Polaris only reads it, and reuses the
covers ES-DE downloaded. Steam ROM Manager shortcuts are Steam's business and
are not imported from `shortcuts.vdf` today; a folder import gives the same
games a direct launch without Steam in the middle. RetroArch works through a
custom command, for example:

```
retroarch -f -L ~/.config/retroarch/cores/snes9x_libretro.so {rom}
```

The command runs without a shell, so `~/` at the start of an argument is
expanded for you and nothing else is; keep paths plain and give the full core
path.

## When it goes wrong

| Symptom | Where to look |
| --- | --- |
| The stream opens and stays black | The folder card: missing keys, BIOS, or a Flatpak that cannot see the folder. Open the emulator's own entry to read its message. |
| The entry launches nothing and the stream ends at once | The emulator is not installed where Polaris looks, or a custom command names a file that is not there. The launch refusal in Nova names the reason when the host knows it. |
| Buttons feel swapped in a Switch game | **Play Setup → Face buttons** for that game. |
| The last save is missing after End Session | Raise the entry's **Exit Timeout**, or save inside the game before leaving. |
| No cover | Put a `<name>.png` next to the game, or set a SteamGridDB key in Settings. |
| Two entries for one game | Two dumps whose names differ once tags are stripped, for example different revisions; remove one entry or move the second dump out of the folder. |
