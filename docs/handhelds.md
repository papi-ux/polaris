# Handhelds and Game Mode

A Steam Deck, ROG Ally, Legion Go or similar device runs two kinds of session. Desktop Mode is a
normal KDE Plasma desktop. Game Mode is gamescope running Steam full screen, with no desktop
underneath it. Polaris treats the two very differently today, and this page says exactly what
works so you do not have to find out from a client that reports the host offline.

This applies to SteamOS, Bazzite deck images, CachyOS handheld edition, ChimeraOS, Nobara's
handheld edition, and any host that installs a gamescope Steam session next to its desktop.

## What works today

| Situation | Status |
|---|---|
| Streaming from Desktop Mode | Works as the distro guide describes: [SteamOS](steamos.md), [Bazzite](bazzite.md), [Arch and CachyOS](arch.md). |
| Polaris staying reachable after switching to Game Mode | Works once Polaris starts at boot. See below. |
| Streaming what Game Mode shows, or launching games into it | Not supported yet. Tracked in [#626](https://github.com/papi-ux/polaris/issues/626). |

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

## What Game Mode itself needs

Streaming from inside Game Mode is a separate piece of work, and none of the current stream paths
do it:

- Mirror Desktop looks for a desktop compositor to capture, and a gamescope session has none.
- Private Stream and Gamescope Stream start their own session and their own Steam, and refuse to
  fight the Steam that Game Mode already has open.

Two capture routes already exist in Polaris that a Game Mode session could use: the PipeWire node
gamescope exports on its own, and DRM/KMS capture of whatever is on screen. Which one holds up on
real hardware is the open question, and nobody on the project has a Game Mode host to answer it
with. Progress lives in [#626](https://github.com/papi-ux/polaris/issues/626).

## Help validate Game Mode streaming

If you have a Game Mode host and ten minutes, this probe answers the open question without any new
code. It uses KMS capture, which is how Sunshine streams a Steam Deck in Game Mode, and it needs
the `cap_sys_admin` capability on the Polaris binary, which `--enable-kms` grants and
`sudo setcap -r "$(readlink -f "$(command -v polaris)")"` removes again.

1. From Desktop Mode, make Polaris boot independent and allow KMS capture:

   ```bash
   sudo -H polaris --setup-host --enable-headless-boot --enable-kms
   ```

2. In `~/.config/polaris/polaris.conf`, set the capture backend and the Mirror Desktop path, then
   restart the service:

   ```ini
   capture = kms
   linux_stream_mode = desktop_display
   ```

   ```bash
   systemctl --user restart polaris
   ```

3. Schedule the evidence dump, which fires while Game Mode is active:

   ```bash
   systemd-run --user --on-active=120 --collect bash -c '
     pw-cli ls Node > ~/game-mode-nodes.txt 2>&1
     systemctl --user status polaris --no-pager -l > ~/game-mode-polaris.txt 2>&1'
   ```

4. Switch to Game Mode. Within two minutes, connect a client to the **Desktop** entry and keep
   trying for a minute, whether or not video appears.

5. Switch back to Desktop Mode and collect the results:

   ```bash
   journalctl --user -u polaris --since "20 minutes ago" --no-pager \
     | grep -iE 'kms|capture|encoder|portal|display|session|error|warn' > ~/game-mode-journal.txt
   ```

6. Post `~/game-mode-nodes.txt`, `~/game-mode-polaris.txt` and `~/game-mode-journal.txt` on
   [#626](https://github.com/papi-ux/polaris/issues/626) with the device, distro and image, GPU,
   client, and Polaris version. Say whether the client got video, audio, and input.

The node list tells us whether Game Mode's gamescope exports its PipeWire stream on your host,
which decides the capture route. The journal tells us whether KMS capture and the encoder came up
at all.

## Reporting handheld results

Include the device model, distro and image, Desktop Mode or Game Mode, GPU, client, package
filename, Polaris version, and the relevant journal lines. Keep package and startup results
separate from gameplay, display rate, suspend, and update persistence results.
