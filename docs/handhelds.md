# Handhelds and Game Mode

A Steam Deck, ROG Ally, Legion Go or similar device runs two kinds of session. Desktop Mode is a
normal KDE Plasma desktop. Game Mode is gamescope running Steam full screen, with no desktop
underneath it. Polaris treats the two differently, and this page says exactly what works in each
so you do not have to find out from a client that reports the host offline.

This applies to SteamOS, Bazzite deck images, CachyOS handheld edition, ChimeraOS, Nobara's
handheld edition, and any host that installs a gamescope Steam session next to its desktop.

## What works today

| Situation | Status |
|---|---|
| Streaming from Desktop Mode | Works as the distro guide describes: [SteamOS](steamos.md), [Bazzite](bazzite.md), [Arch and CachyOS](arch.md). |
| Polaris staying reachable after switching to Game Mode | Works once Polaris starts at boot. See below. |
| Streaming what Game Mode shows | Works, with keyboard and mouse. See [Streaming from Game Mode](#streaming-from-game-mode). |
| Launching a Steam title into Game Mode from a client | Works. The launch goes to the Steam that is running Game Mode. |
| A Private Stream while Game Mode is running | Not possible. Game Mode owns the one screen and the one Steam. The configured mode comes back in Desktop Mode. |

## Why Polaris disappears in Game Mode

The packaged user service starts with desktop autostart, which only a desktop session fires. A
gamescope session never runs desktop autostart, so:

- a host that boots straight into Game Mode has no Polaris until someone opens Desktop Mode, and
- a Polaris started from a terminal or from the application menu ends with the desktop session
  when the host switches back to Game Mode.

Either way the client sees the host go offline. Host setup now recognises a Game Mode session on
the host and prints the fix, the stats API reports it under `game_mode_host` and `boot_readiness`,
and the console shows a notice while Game Mode is running.

## Keep Polaris reachable across mode switches

Install the package for your distro from its guide, then make the service independent of the
session:

```bash
sudo -H polaris --setup-host --enable-headless-boot
systemctl --user enable --now polaris
```

The first command enables lingering for your account and hooks the Polaris user service into
`default.target`, so it starts at boot before any login. Run it from Desktop Mode's terminal or
over SSH, as your normal user through sudo. `--disable-headless-boot` undoes the hook later.

Verify after a reboot. Over SSH from another machine:

```bash
systemctl --user is-active polaris
journalctl --user -u polaris --since "10 minutes ago" --no-pager
```

Without SSH, schedule the check from Desktop Mode, switch to Game Mode, wait two minutes, and read
the file after switching back:

```bash
systemd-run --user --on-active=90 --collect \
  bash -c 'systemctl --user is-active polaris > ~/polaris-game-mode.txt 2>&1'
```

With that in place, streaming from Desktop Mode works exactly as before, and the host no longer
drops off the client list when it returns to Game Mode.

## Streaming from Game Mode

While Game Mode is running the host has one screen, and it belongs to the session's gamescope.
There is no desktop to build a private display beside, and the Steam that is running is the
session itself. So every stream from the host shows the Game Mode screen, whatever stream mode is
configured. There is nothing to set: Polaris notices the session, holds the configured mode, and
gives it back when the host returns to Desktop Mode. The console and `--setup-host` say so while it
lasts.

What that stream is made of:

- **Video** comes from the PipeWire node gamescope exports for its own screen. Polaris attaches to
  that node directly, and falls back to the ScreenCast portal for gamescope, which SteamOS ships,
  if the direct attach fails. Leave `capture` unset: a host still set to `capture = kms` from the
  earlier Game Mode probe takes the picture another way, and the limits below do not describe it.
- **Keyboard and mouse** go in through the session's libei socket, the same way Steam's own
  streaming reaches it.
- **Controllers** arrive as the same virtual pad as on any host, and Game Mode's Steam picks it up
  the way it picks up a pad that was just plugged in, toast included. On SteamOS the account at the
  screen may already create one, so an Xbox style pad works with no setup at all. An emulated
  DualSense needs the input setup from your distro guide (`sudo -H polaris --setup-host`), and so
  may touch. Touch and pen go to the host the usual way, since the session's gamescope reads the
  host's devices like the built-in panel.
- **Audio** follows the default sink, which Polaris points at its own while a stream is up and
  points back when it ends.
- **A Steam title** launched from a client is handed to the Steam that is running Game Mode, and
  Game Mode brings it to the front. A host configured for Private Stream does not ask the player
  to close Steam first, because there is no other Steam to close. A title that is already open on
  the device is joined as it is and not launched a second time.
- **Ending the session** closes the title the stream opened and leaves Game Mode alone. On a
  desktop host, ending a Steam title's stream closes the Steam that the stream opened. In Game Mode
  that Steam is the session, so only the title is asked to close, and Steam stays as it is. That
  happens only when someone ends the session on purpose: End Session on the client, or Close App or
  Disconnect in the console. A title that was already open on the device when the stream started
  is left open. A client that drops, a paused session that times out, or a Polaris that restarts
  closes nothing, so the game is where the player left it.

Known limits:

- The picture has no mouse pointer. gamescope draws its pointer on the device's own screen and
  does not put it in the stream it exports.
- gamescope sends a frame only when the window in front draws. When a stream starts on a screen
  that is standing still, Polaris asks that window to draw once so the first frame arrives. The
  Steam overlay opened over a still app can lag until the app draws again.
- The stream is the session's resolution, 1280x800 on a Steam Deck, scaled to what the client asked
  for. HDR is not carried.

Proven on a Steam Deck OLED on SteamOS 3.8.16 with gamescope 3.16.23: the Game Mode screen, a Steam
title launched from the client at 60 fps with audio, keyboard, mouse and a controller, that title
closed again when the session ended, and Game Mode still running afterwards. Touch, an emulated
DualSense, Bazzite, and hosts whose gamescope has no ScreenCast portal still want field results.

## Reporting Game Mode results

If you have a Game Mode host, connect a client to the **Desktop** entry while Game Mode is running,
then launch a Steam title from the client. Afterwards collect the lines that say what happened:

```bash
journalctl --user -u polaris --since "20 minutes ago" --no-pager \
  | grep -iE 'game_mode|portal|gamescope|EI virtual|encoder|error|warn' > ~/game-mode-journal.txt
```

Post that file on [#626](https://github.com/papi-ux/polaris/issues/626) with the device, distro and
image, GPU, client, and Polaris version. Say whether the client got video, audio, keyboard and
mouse, and a controller, and whether Game Mode was still running after the stream ended.

## Reporting handheld results

Include the device model, distro and image, Desktop Mode or Game Mode, GPU, client, package
filename, Polaris version, and the relevant journal lines. Keep package and startup results
separate from gameplay, display rate, suspend, and update persistence results.
