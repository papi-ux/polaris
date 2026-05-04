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

## Recommended first settings

```ini
headless_mode = enabled
linux_use_cage_compositor = enabled
linux_prefer_gpu_native_capture = disabled
trusted_subnets = ["10.0.0.0/24"]
encoder = nvenc
adaptive_bitrate_enabled = enabled
max_sessions = 2
```

These are the settings behind the recommended Headless Stream mode on a Linux host.

## Linux display modes

Headless Stream starts apps inside Polaris' private labwc compositor. It is intentionally isolated
from your normal KDE, GNOME, or wlroots desktop, so the built-in Desktop entry can be empty if no
desktop shell or app is launched inside that runtime.

Use Desktop Display mode when you want to stream the visible host desktop session. Use Headless
Stream when you want a stream-only runtime that leaves the host desktop layout alone.

## Common options

| Key | Typical value | What it controls |
| --- | --- | --- |
| `headless_mode` | `enabled` | Request a stream-only session instead of the visible desktop |
| `linux_use_cage_compositor` | `enabled` | Enable Polaris' private stream runtime |
| `linux_prefer_gpu_native_capture` | `disabled` | Keep Headless Stream as the first validation path; enable only after testing GPU-native capture on your stack |
| `trusted_subnets` | CIDR list | Enable Trusted Pair on known local networks |
| `encoder` | `nvenc` / `vaapi` / `software` | Primary encoder backend |
| `adaptive_bitrate_enabled` | `enabled` | Allow mid-stream bitrate adjustment |
| `max_sessions` | `2` | Number of simultaneous sessions or viewers |
| `enable_pairing` | `enabled` | Accept new clients |
| `enable_discovery` | `enabled` | Advertise Polaris over mDNS |
| `stream_audio` | `enabled` | Capture and stream audio |
| `steamgriddb_api_key` | key | Cover art lookups for non-Steam apps |

## Linux HDR and Main10

On Linux, treat sessions that log `stream_hdr_enabled=false` as SDR even if the client requests HDR.
Forcing `hdr_mode = 2` can still select a 10-bit HEVC/Main10 or P010 encode path, but that does not
create a true HDR source when the captured display path is SDR and may produce incorrect colors on
some VAAPI stacks.

True Linux HDR requires the active capture path to expose HDR display metadata. Today that means a
KMS/DRM display path with an HDR-capable output reporting `HDR_OUTPUT_METADATA`, plus a client HDR
request and a 10-bit-capable encoder. A valid true HDR session logs:

```text
HDR metadata: available=true
Color coding: HDR (Rec. 2020 + SMPTE 2084 PQ)
HDR decision: ... display_hdr=true hdr_metadata_available=true stream_hdr_enabled=true
```

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
```

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
sudo polaris --setup-host
```

Optional DRM/KMS setup:

```bash
sudo polaris --setup-host --enable-kms
```
