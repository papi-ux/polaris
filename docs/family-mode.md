# Family Mode (isolated per-app sessions) — Linux

Family Mode lets you stream a game in a **fully isolated session** so someone can play it
on another device **without taking over your PC**. The game runs in its own nested
compositor (`labwc`) — its own display, its own audio, its own gamepad — so your desktop
stays yours: you keep using your monitor, mouse, keyboard, and speakers while the game
streams elsewhere.

Typical use: one powerful PC in the house, a game streamed to the living-room TV or a
laptop, while you keep working at the desk. Two people, one machine, independent sessions.

> Family Mode is a **Linux/KDE** feature. It is opt-in per app and does not change how any
> of your other apps launch.

---

## Requirements

| Component | Why | Install (openSUSE) |
| --- | --- | --- |
| `labwc` | The nested compositor the isolated session runs in | `sudo zypper install labwc` |
| `swaybg` *(optional)* | Draws a splash backdrop over the brief black gap before the game paints. Without it you get a black backdrop until the first frame — harmless, just less polished. | `sudo zypper install swaybg` |
| A GPU with working Vulkan (AMD/Intel) or GLES2 (NVIDIA) | Off-screen rendering for the isolated compositor | already present on a gaming system |

The web UI's **Session Readiness** panel (AudioVideo tab) shows at a glance whether these
are present, so you don't have to read the log to find out.

---

## Enabling it for an app

1. Open the Polaris web UI → **Applications**.
2. Add or edit the app you want to isolate (a specific game, or **Steam Big Picture** as a
   universal launcher — see below).
3. Turn on **Isolated session (Family Mode)** (`isolated-session`). This automatically sets
   the app's Display Source to `isolated`.
4. Save. Launch it from Moonlight as usual.

When it launches you'll keep your desktop; the game appears only in the stream.

---

## Launching a whole library: Steam Big Picture

Instead of adding every game, point Family Mode at **Steam Big Picture** as the app
command (`steam -gamepadui`). Because Family Mode isolates a *session* (not a single
process), any game you launch from within Big Picture runs **inside the same isolated
session** and is captured automatically. Configure once, play your whole library.

Two things to know about Steam specifically:

- **Exit your desktop Steam first.** Steam enforces a single instance per user, and the
  isolated session shares your Steam data. If your desktop Steam is running, the isolated
  one can't start cleanly. (A future enhancement — giving the session its own Steam data
  root — would remove this.)
- **Quitting closes Steam gracefully.** When the session ends, Polaris issues
  `steam -shutdown` (Steam's own clean shutdown — it closes the running game and syncs
  Steam Cloud) before tearing the session down, so Steam won't report a "fatal error" on
  the next launch.

---

## How it stays isolated (what to expect)

- **Display:** the game renders into the nested `labwc` compositor, which Polaris captures
  directly. Your physical monitor is never touched — it is not disabled, mirrored, or
  turned into a second screen. A window open on your real desktop stays on your real
  desktop.
- **Audio:** the session uses stream-only audio, so the game's sound goes to the remote
  client, not your speakers.
- **Gamepad:** controllers are isolated to the session; they don't leak into your desktop.
- **Display arrangement modes** (the AudioVideo "Streaming display arrangement" setting,
  Mirror / Privacy / Extended) do **not** apply to Family Mode — an isolated session owns
  its own off-screen display and never rearranges your monitors.

---

## Troubleshooting

- **Black screen for a moment at launch** — the game hasn't painted its first frame yet.
  Install `swaybg` for a splash backdrop instead of black.
- **Session won't start / Steam does nothing** — make sure your desktop Steam is fully
  exited first (see above).
- **`labwc` not found** — install it (`sudo zypper install labwc`); the Session Readiness
  panel will show it as missing.
- **Non-Steam launcher (Lutris/Heroic/Bottles) doesn't close cleanly on quit** — most apps
  handle the standard exit signal fine. If a particular launcher needs a special shutdown,
  add a per-app **prep-command "undo"** with its clean-shutdown command (this is how
  Polaris handles Steam automatically).

---

## Notes

- Family Mode is exempt from the global display-swap logic, so it never dims or rearranges
  your monitor — even if you have a headless/dongle streaming arrangement configured.
- It composes with the rest of the Linux streaming stack (EVDI/dongle, capture-follow),
  but does not require any of it — a plain `labwc` install is enough.
