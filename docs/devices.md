# Pair and manage devices

The **Devices** page is where clients get paired and where each paired device keeps its access
level, display profile, and per-device automation. This page explains the three pairing routes,
what the access presets allow, and what the device editor changes.

## Pairing routes

| Route | For | How it works |
|---|---|---|
| **Nova QR** | Nova on Android | Generate a passphrase; Nova scans a QR code that carries the host address, the PIN, and the pairing context. The fastest path for Nova on your network. |
| **Trusted Network** | Devices on a subnet you control | Any device inside a listed trusted subnet pairs without a PIN. Enable it under **Settings, Network, Trusted Subnet Auto-Pairing** and list the subnets in CIDR form. Off by default. |
| **Manual PIN** | Standard Moonlight clients | The client shows a four-digit PIN; enter it here and choose **Send**. The classic Moonlight flow, described step by step in [Play with Moonlight](moonlight.md). |

Every route applies the access preset selected under **Access for this device** at pairing time.
For Nova QR and Manual PIN, **Temporary device authorization** keeps the certificate only in
memory. Polaris revokes it after that device's final stream disconnect, and a Polaris restart also
removes it, so the device must be paired again before it can reconnect. Temporary authorization is
useful for a guest device; it does not weaken the selected access preset while the device is live.

## Access presets

New devices get **Game Control** unless you choose otherwise. Existing devices keep their saved
access until you edit it.

| Preset | Can | Cannot |
|---|---|---|
| **Viewer Access** | Watch an existing stream | Browse the library, launch games, send input |
| **Browse & Watch** | List the library and join an existing stream | Launch games, send input |
| **Game Control** | Browse, launch, and control games with controller, keyboard, mouse, touch, and pen | Read or set the clipboard, transfer files, run server commands |
| **Full Control** | Everything, including clipboard, file transfer, and server commands | Nothing withheld; use only for devices you fully trust |

**Fine-Tune Permissions** in the editor exposes the individual permissions behind the presets:
list apps, view streams, launch apps, clipboard read and set, server command, and the five input
kinds. A device whose permissions match no preset shows as
**Custom Access**.

A device with Browse & Watch that tries to start a stream gets a permission-denied answer; the
[troubleshooting entry](troubleshooting.md#paired-client-gets-permission-denied-403-when-starting-a-stream)
walks through it.

## The saved-devices list

Each card shows the device name, whether it is connected, its access preset, the client family
(Nova or a standard client), and whether its display profile matches the recommendation for that
device. Below that, five tiles summarise permissions, display profile, client commands, when it was
paired, and when it was last seen.

The card's actions: **Wake** sends a Wake-on-LAN packet when the device has a MAC address in its
profile, **Disconnect** ends its active session, **Edit Access** opens the editor, and the trash
control unpairs the single device. **Unpair All** at the top removes every device.

## Editing a device

**Edit Access** opens a dialog with three areas.

**Host view** is read from the host, not from the form. It shows what Polaris actually applies for
this device: display mode, target bitrate, stream display mode, adaptive bitrate, and the resume
window, each with its source and whether it is applied live or at the next launch. A host without
the settings projection says so instead of guessing.

**Identity & Access** holds the device name, the access preset buttons, the fine-tune permissions,
and switches for legacy app ordering, a virtual display on every connection (Windows hosts), client
commands, and temporary authorization. Changing a saved device to temporary removes it from the
durable pairing store; changing it back to permanent saves it again.

**Display Profile** overrides what the client asks for:

- **Display Mode Override**, as `WxHxFPS`, makes Polaris ignore the client's requested mode and
  configure displays to this value. Leave it blank for automatic matching.
- **Output Name** pins the device to a specific host output.
- **Color Range** forces limited or full range when a client reports it wrong.
- **WoL MAC Address** enables the Wake action.
- **Enable HDR** requests an HDR-capable configuration for this device; see
  [Linux HDR and Main10](configuration.md#linux-hdr-and-main10) for the host side.

**AI Suggest** proposes a profile from the device database or, when AI is configured, from the
provider. It only fills the form; nothing is saved until you choose **Save**.

**Client Commands**, shown when client commands are allowed, runs host-side commands when this
device connects (**do**) and disconnects (**undo**). Commands run detached.

## Stale profile aliases

Renaming or re-pairing a device can leave a display profile behind under the old name. When that
happens a **Stale profile aliases** section appears at the bottom of the page listing them;
**Remove stale** deletes the aliases without unpairing any real device.
