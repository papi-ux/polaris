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
linux_use_cage_compositor = true
linux_prefer_gpu_native_capture = enabled
trusted_subnets = ["10.0.0.0/24"]
encoder = nvenc
adaptive_bitrate_enabled = enabled
max_sessions = 2
```

These are the settings most people should confirm first on a Linux host.

## Common options

| Key | Typical value | What it controls |
| --- | --- | --- |
| `headless_mode` | `enabled` | Use an isolated invisible runtime instead of the physical desktop |
| `linux_use_cage_compositor` | `true` | Enable the dedicated compositor path |
| `linux_prefer_gpu_native_capture` | `enabled` | Prefer DMA-BUF and other GPU-native capture paths |
| `trusted_subnets` | CIDR list | Enable Trusted Pair on known local networks |
| `encoder` | `nvenc` / `vaapi` / `software` | Primary encoder backend |
| `adaptive_bitrate_enabled` | `enabled` | Allow mid-stream bitrate adjustment |
| `max_sessions` | `2` | Number of simultaneous sessions or viewers |
| `enable_pairing` | `enabled` | Accept new clients |
| `enable_discovery` | `enabled` | Advertise Polaris over mDNS |
| `stream_audio` | `enabled` | Capture and stream audio |
| `steamgriddb_api_key` | key | Cover art lookups for non-Steam apps |

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

## Host setup helper

To re-run the host setup steps explicitly:

```bash
sudo polaris --setup-host
```

Optional DRM/KMS setup:

```bash
sudo polaris --setup-host --enable-kms
```
