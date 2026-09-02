# Configuration

Polaris is designed to be configured from the web UI first. The config file is still useful when
you want to script setup, review current values, or recover from a broken UI state.

## Files

| File | Default path | Purpose |
| --- | --- | --- |
| Main config | `~/.config/polaris/polaris.conf` | Host settings |
| App library | `~/.config/polaris/apps.json` | Published apps and launch behavior |
| Runtime state | `~/.config/polaris/polaris_state.json` | Saved UI and session state |

If you change `port` in `polaris.conf`, the web UI moves to `https://localhost:<port + 1>`.

Add installed Steam titles from the web UI's Applications library scan. The Nix modules do not
generate or replace `apps.json`; Polaris stores each imported title with its Steam app id and applies
the selected Linux stream runtime when that title launches. Steam Big Picture remains available as
the browse-first entry and does not require an app id.

## Recommended first settings

```ini
headless_mode = enabled
linux_use_cage_compositor = enabled
linux_prefer_gpu_native_capture = enabled
trusted_subnets = ["10.0.0.0/24"]
encoder = nvenc
nvenc_split_encode_mode = disabled
adaptive_bitrate_enabled = enabled
max_sessions = 2
```

These are the settings behind the recommended Headless Stream mode on a Linux host. Use `encoder = nvenc` on NVIDIA, `encoder = vaapi` on AMD/Intel Mesa VAAPI hosts, and `encoder = software` only as a fallback or diagnostic path.

## Linux display modes

This is the config-file summary. For choosing a mode, what each one feels like in practice, and
AMD/NVIDIA guidance, see [Launch modes and capture paths](launch-modes.md).

| I want | Set `linux_stream_mode` to |
| --- | --- |
| My real desktop, at host resolution | `desktop_display` (Mirror Desktop) |
| An extra display, sized to the client | `host_virtual_display` |
| An isolated game-only session, desktop untouched | `headless_stream` / `windowed_stream` |

Two client-facing notes: Moonlight-protocol clients can request the mirror for a single launch with
`mirrorDesktop=1` on `/launch` (no host reconfiguration), and `headless_mode = enabled` *without*
`linux_use_cage_compositor` derives `host_virtual_display`, not a headless session.

## Common options

| Key | Typical value | What it controls |
| --- | --- | --- |
| `headless_mode` | `enabled` | Request a stream-only session instead of the visible desktop |
| `linux_use_cage_compositor` | `enabled` | Enable Polaris' private stream runtime |
| `linux_prefer_gpu_native_capture` | `enabled` | Prefer DMA-BUF/GPU-resident capture on NVIDIA and AMD-capable stacks; Polaris reports SHM/system-memory fallback truthfully when the compositor or driver cannot provide it |
| `linux_stream_mode` | `headless_stream` | Stream path id for Linux sessions: `headless_stream`, `windowed_stream`, `gamescope_stream`, `host_virtual_display`, `desktop_display`, or `headless_dongle`. Empty derives the path from the legacy booleans above. See [Launch modes and capture paths](launch-modes.md) for choosing, [stream paths](stream-paths.md) for the contract |
| `linux_private_runtime` | `labwc` | Private compositor used by paths that host the session themselves: `labwc` or `gamescope`. Ignored on host paths |
| `headless_swap_mode` | `privacy` | Headless Dongle path only: `privacy` makes the dongle primary and blanks the panel after one-time portal approval is saved (the approval session keeps it on); `off` extends onto the dongle and leaves the panel primary |
| `trusted_subnets` | CIDR list | Enable Trusted Pair on known local networks |
| `headless_gamepad_isolation` | `enabled` | Hide host-connected gamepads from private headless streams; disable only when you intentionally want a wired host controller visible inside the stream |
| `client_gamepad_seat_isolation` | `disabled` | Assign Polaris-created client gamepads to a dedicated Linux seat so other active-seat users do not receive automatic device ACLs |
| `client_keyboard_mouse_seat_isolation` | `disabled` | Assign the virtual keyboard, mouse, touch and pen Polaris creates for clients to a dedicated Linux seat, so a client streaming a private session does not also type and click into the desktop session logged in at the machine |
| `mouse_cursor_visible` | `enabled` | Composite a separately captured host cursor into the stream. Required for DRM/KMS; disable it if the client draws its own cursor and you see two pointers. Portal may embed its cursor independently |
| `back_button_timeout` | `-1` | **Milliseconds**, not seconds, that Back/Select must be held to emulate Home/Guide. `-1` disables it. A small value such as `2` means two *milliseconds*, which turns nearly every Back/Select press into Home and makes the button look broken — use `2000` for two seconds |
| `encoder` | `nvenc` / `vaapi` / `software` | Primary encoder backend |
| `nvenc_split_encode_mode` | `disabled` | Experimental Linux/FFmpeg NVENC split-frame encoding for HEVC/AV1 |
| `adaptive_bitrate_enabled` | `enabled` | Allow mid-stream bitrate adjustment |
| `disconnect_resume_timeout_seconds` | `300` | Seconds to keep an app paused after client disconnect for resume |
| `max_sessions` | `2` | Number of simultaneous sessions or viewers |
| `enable_pairing` | `enabled` | Accept new clients |
| `enable_discovery` | `enabled` | Advertise Polaris over mDNS |
| `stream_audio` | `enabled` | Capture and stream audio |
| `steamgriddb_api_key` | key | Cover art lookups for non-Steam apps |
| `beat_times_lookup` | `enabled` | Ask How Long To Beat about titles missing from the local completion-estimate dataset; disable to keep the host from making those requests |

### Linux client-gamepad access boundary

`headless_gamepad_isolation` controls the **opposite** direction: it hides controllers physically
connected to the host from a private stream. It does not make a client-created virtual controller
disappear from the host kernel.

When `client_gamepad_seat_isolation` is enabled, Polaris marks client gamepads for the bundled host
rules to assign them to the `seat-polaris` seat. This prevents logind from granting the active local
desktop user an automatic `uaccess` ACL. The device nodes remain `root:input` with mode `0660`, so
the Polaris streaming user must belong to the `input` group. The option is disabled by default to
preserve existing virtual-controller identity and local access; enabling it gives isolated gamepads
Polaris-specific device names so current Inputtino uinput backends can enforce the udev policy.

Re-run `sudo -H polaris --setup-host` after upgrading so the installed udev rules understand the
dedicated device names and marker. Existing virtual controller nodes keep their previous access policy
until recreated; stop active streams and restart Polaris after host setup. AppImage users should
re-run the AppImage install action for the same reason.

The `input` group requirement is not optional and there is no fallback: the isolated nodes are
deliberately denied the logind ACL, so an account outside the group cannot open the devices Polaris
just created, and neither can the streamed game. Polaris logs an `input_access:` warning at startup
when seat isolation is enabled and the account it runs as is not a member, and `--setup-host` reports
the same thing. Add the account with `sudo usermod -aG input <user>` and log out and back in;
membership only applies to new sessions. On ostree hosts such as Bazzite the group is defined
in `/usr/lib/group` and `usermod` cannot see it, so use `ujust add-user-to-input-group`
instead. The startup warning prints whichever command applies.

#### Verifying that isolation is applied

Check the device, not the seat list:

```bash
udevadm info -q property -n /dev/input/eventN | grep ID_SEAT
```

`ID_SEAT=seat-polaris` means the rules applied and the device is isolated.

`loginctl list-seats` will keep showing only `seat0`, and that is expected — it is not a sign that
isolation failed. logind only materializes a seat that owns a device tagged `master-of-seat`, which
in practice means a graphics device. `seat-polaris` exists purely as a udev property that keeps
logind from handing the device to the seat0 session, so it never becomes a seat logind lists.

This is a Unix-user boundary, not a same-account process sandbox. Local users who are deliberately
members of `input`, and local applications running under the same Unix account as Polaris, can still
open the virtual controller. For concurrent gaming, run Polaris under a dedicated Unix account and
do not add local desktop users to `input`. Strong same-UID isolation requires a privileged broker,
container/security-domain boundary, or equivalent system-level policy; a per-session Web toggle
cannot provide it safely.

### Linux host and private session input isolation

Polaris can run a private stream session while somebody is using the desktop session logged in at
the machine. Two separate leaks have to be closed for that to work, and they are closed by different
mechanisms because the two directions are not symmetric.

**Client input reaching the host desktop.** The virtual keyboard and mouse Polaris creates are
kernel input devices, so a desktop session at `seat0` receives them along with everything else on
that seat: the client types into the stream and into the desktop at the same time. Enabling
`client_keyboard_mouse_seat_isolation` marks those devices for the bundled udev rules to assign to
the `seat-polaris` seat, which stops logind from handing them to the seat0 session. Device names are
unchanged, so a host compositor already configured to ignore them by name keeps working. This is the
same Unix-user boundary described above: same-account processes and members of `input` are not
isolated.

Host compositors that do not go through logind can ignore the devices by name instead. In sway:

```
input 48879:57005:Polaris_Keyboard_passthrough events disabled
input 48879:57005:Polaris_Mouse_passthrough events disabled
input 48879:57005:Polaris_Mouse_passthrough_(absolute) events disabled
```

**Host devices reaching the private session.** The reverse leak appears when the private compositor
opens the seat's physical devices: the keyboard and mouse on the desk then drive the streamed
session. Polaris generates the private session's `~/.config/labwc-polaris/rc.xml` with a
`<libinput>` block that sets `sendEventsMode` to `no` for every physical device it finds, leaving its
own virtual devices enabled. This needs no configuration and applies to every private session.

Writing your own `rc.xml` there takes over completely: a file without Polaris' generated marker
comment is never overwritten, and Polaris then stops managing input isolation for that session.

### Steam Input and virtual controllers

Strict host-controller isolation exposes the Polaris virtual gamepad to the streamed app while hiding
physical controllers connected to the host. Local Steam Input settings can still claim that virtual
Xbox controller.
For Proton games, Steam then tries to hand the game a replacement controller through `/dev/uinput`,
but the strict sandbox deliberately does not expose that device or dynamically created input nodes.
The result is a controller that works in host-side event tests but is completely dead in the game.

When Doctor reports `steam_input_conflict`, open Steam Settings > Controller and disable Steam Input
for Xbox controllers. Also review the affected game's Controller properties: use Default after
disabling the host-wide Xbox setting, or Disable Steam Input for that game. A per-game Force On
override still triggers the conflict.

This Doctor check is read-only. It reports only aggregate status and counts; it does not expose Steam
account ids, installed app ids, profile filenames, or filesystem paths, and it does not edit Steam
configuration. Close Steam before changing the setting through Steam or by another supported tool so
the running client cannot overwrite the update.

## Linux HDR and Main10

On Linux, treat sessions that log `stream_hdr_enabled=false` as SDR even if the client requests HDR.
Forcing `hdr_mode = 2` can still select a 10-bit HEVC/Main10 or P010 encode path, but that does not
create a true HDR source when the captured display path is SDR and may produce incorrect colors on
some VAAPI stacks.

True Linux HDR requires the active capture path to expose HDR display metadata. Today that means a
KMS/DRM display path with an HDR-capable output reporting `HDR_OUTPUT_METADATA`, plus a client HDR
request and a 10-bit-capable encoder. A valid true HDR session logs:

```text
HDR metadata: available=true usable=true
Color coding: HDR (Rec. 2020 + SMPTE 2084 PQ)
HDR decision: ... display_hdr=true hdr_metadata_available=true stream_hdr_enabled=true
```

If the log says `HDR metadata: available=true usable=false`, Polaris found an HDR
metadata blob but the static metadata is incomplete, such as a custom EDID with a
zero max luminance value. Polaris treats that stream as SDR instead of tagging it
as HDR with unusable metadata.

Headless labwc/wlroots sessions are intentionally treated as SDR until the headless display path can
truthfully provide HDR metadata. In that mode, `hdr_mode = 2` can still be useful to test Main10/P010
encode support, but Polaris will not advertise true HDR to the client without metadata.

For AMD VAAPI hosts, validate SDR first:

```ini
encoder = vaapi
hdr_mode = 0
color_range = 1
```

Then test HEVC Main 8-bit before enabling Main10 or client HDR requests.

## NVIDIA NVENC Encoder

### nvenc_split_encode_mode

Controls FFmpeg's `split_encode_mode` private option for Linux NVENC HEVC and AV1 encoders. Polaris only
passes this option when the selected FFmpeg encoder exposes it; H.264 and native Windows NVENC ignore it.

Recommended values:

| Value | FFmpeg value | Recommendation |
| --- | ---: | --- |
| `disabled` | `15` | Default. Preserves legacy behavior after FFmpeg updates. |
| `auto` | `0` | Let NVIDIA's driver and FFmpeg decide after validating your GPU/driver stack. |
| `2` | `2` | Useful first manual test on multi-NVENC GPUs, especially for 4K120 HEVC/AV1. |
| `forced` | `1` | Experimental. Use only when comparing against `auto` and explicit engine counts. |
| `3` | `3` | Experimental. Use only on GPUs known to expose three usable NVENC engines. |

## AI provider settings

The AI optimizer is optional. Configure it in the web UI if you want connection testing before
saving, or set it directly in `polaris.conf`.

### Anthropic

```ini
ai_enabled = enabled
ai_provider = anthropic
ai_model = claude-haiku-4-5-20251001
ai_auth_mode = subscription
```

### OpenAI

```ini
ai_enabled = enabled
ai_provider = openai
ai_model = gpt-5.4-mini
ai_auth_mode = api_key
ai_api_key = sk-proj-...
```

### Gemini

```ini
ai_enabled = enabled
ai_provider = gemini
ai_model = gemini-2.5-flash
ai_auth_mode = api_key
ai_api_key = YOUR_GEMINI_KEY
```

### Local OpenAI-compatible server

```ini
ai_enabled = enabled
ai_provider = local
ai_model = gpt-oss
ai_auth_mode = none
ai_base_url = http://127.0.0.1:11434/v1
ai_timeout_ms = 60000
```

Large local models can need substantially longer than cloud models for their first response while
weights are loaded. The web UI's local-provider profiles start with a bounded 60-second timeout and
report inference timeout, connection, authentication, missing-model, and response-format failures
separately. Lower the timeout after the model is warm if you prefer faster failure.

## Credential reset

If you lose access to the web UI credentials:

```bash
polaris --creds new-username new-password
```

Run the command as the same user account that runs Polaris, then restart Polaris before signing in
with the new credentials:

```bash
systemctl --user restart polaris
```

If Polaris is running in the foreground, stop it and start it again instead.

## Host setup helper

To re-run the host setup steps explicitly:

```bash
sudo -H polaris --setup-host
```

Optional DRM/KMS setup:

```bash
sudo -H polaris --setup-host --enable-kms
```
