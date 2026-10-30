# Virtual display comparison

These tools measure the comparison asked for in papi-ux/polaris#795. It sets Polaris
Host Virtual Display on a KWin virtual output against Hermes with Hermes-KMS 0.4.0, on
one AMD/KDE machine with one client and the same stream settings. The first round is
HEVC SDR at a fixed bitrate, at 1440p120 and 4K60.

The scripts only read the machine. Installing Hermes and Hermes-KMS, changing either
host's settings and switching between the hosts are manual steps, and so is putting
the machine back afterwards.

| File | What it does |
|---|---|
| `measure_run.py` | Samples one running stream and writes a JSON receipt for it |
| `compare_runs.py` | Combines the receipts with the client's readings and the photo readings into Markdown tables |
| `pattern.html` | The streamed test page: a moving WebGL scene and a clock |
| `launch_page.sh` | Opens `pattern.html` in its own Firefox profile, fullscreen for the stream or windowed as the reference clock |

## What is measured

| Figure | Source | Notes |
|---|---|---|
| Frame processing latency | Both hosts log `Frame processing latency (min/max/avg)` at debug level for each 20 s window | From the moment the host's capture code has a frame to the moment its packets go to the network, so it includes encoding. Moonlight shows the same figure as host processing latency. |
| Network latencies | The other three periodic lines: `send_batch()`, FEC block and the frame's overall network latency | Same logger as above |
| CPU | `/proc/PID/stat`, every 0.5 s | In percent of one core for the host (its process and children, except the test page), KWin, Xwayland, PipeWire (`pipewire`, `pipewire-pulse`, `wireplumber`), the desktop portals and the test page. Whole-system CPU is in percent of all cores. |
| GPU busy, VCN busy, GPU power | amdgpu's sysfs files | The first amdgpu card, unless `--card` names another |
| VCN busy time per frame | VCN busy share times the target frame interval | Stands in for encode time, which Hermes does not report. Any other video work on the GPU adds to it. |
| Network send rate | `/proc/net/dev` | The busiest interface is the stream: video, FEC and audio |
| Polaris capture timings | `capture_telemetry` lines, written every 600 frames with `linux_capture_profile = enabled` | Polaris only |
| Polaris encode time, frame rate, dropped and duplicate frames | `/api/stats/stream-sse` | Polaris only. One request stays open for the whole run because Polaris logs every web request at debug level. |
| Hermes KMS acquire time | `[capture-metric/...]` lines, every 120 frames | Hermes only. Times the KMS acquire call and nothing else. |
| Client figures | Moonlight's performance overlay, typed into `client.csv` | Frame rates, drops, host processing latency, network round trip, decode, queue and render times |
| Glass to glass | Photos of the host monitor and the client screen together, typed into `photos.csv` | How far the stream's clock trails the host monitor's |

Points to keep in mind when reading the results:

- Each host starts its processing clock when its own capture code has the frame:
  Polaris once the PipeWire buffer is attached, Hermes once the KMS acquire returns.
  Time spent before that, in KWin or PipeWire for Polaris and in the KMS commit for
  Hermes, is not counted in it. Glass to glass is the only figure that covers the whole
  path.
- The capture timings are listed in their own table because the two hosts time
  different steps. They are not compared with each other.
- A process that starts and exits within one 0.5 s sample is only partly counted.
- Debug logging adds a few lines per second to both hosts. Each receipt records the
  log line rate, so a host that logs much more shows it.
- The photo figure is relative. The host monitor's own display delay is subtracted out,
  so it is the delay the stream adds and is the same yardstick for both hosts. Each
  clock moves once per frame of its own display, so single photos vary by a frame or
  so. The median of about 20 photos per host and scenario is what to compare.

## Setup

Only one host can run at a time, because Polaris and Hermes use the same ports. Stop
one before starting the other.

Both hosts need `min_log_level = debug` in their config file for the periodic lines.

Set `encoder = vaapi` and `fec_percentage` the same in both config files. The
defaults differ: Polaris takes 10% of the bitrate for FEC and Hermes 20%, so at
50 Mb/s Polaris's encoder would get about 44 Mb/s and Hermes's about 39. compare_runs
warns when these or a few other stream settings differ between hosts, and a key left
out of one file counts as different.

The VA-API rate control also differs. On AMD, Hermes always uses VBR with a one-frame
buffer, whatever its settings say. Polaris picks CBR with a standard buffer unless
`polaris.conf` has `vaapi_rc = vbr` and `vaapi_strict_rc_buffer = enabled`. Polaris
avoids that pair on radeonsi by default because frames after a scene change can grow
far past the one-frame buffer, so say which way Polaris ran when sharing results.
Each host logs its choice at info level when the stream starts, Polaris as
`VA-API session settings` and Hermes as `Using VBR with single frame VBV size`.

Polaris:

- Settings, Audio/Video: set Where games run to Host Virtual Display and the backend
  to KWin. In `polaris.conf` these are `linux_stream_mode = host_virtual_display` and
  `linux_virtual_display_backend = kwin`.
- `linux_capture_profile = enabled` for the capture timings.
- `adaptive_bitrate_enabled = disabled` and `ai_enabled = disabled`, so the bitrate
  and frame rate stay where the client set them.
- The test client's device: on the Devices page its Host view should show Display mode
  and Stream display mode as Not set, Target bitrate as Client's choice and Adaptive
  bitrate as Off or Not set. Leave Enable HDR on Auto and its Default Space in Spaces
  on Desktop. A saved display mode replaces the mode the client asks for, and a saved
  bitrate caps it. compare_runs warns about both.

Hermes:

- `virtual_display_backend = hermes_kms` in `hermes.conf`, with the `hermes_kms`
  module loaded.
- The test app with its virtual display on and the layout set to Extend.

On both hosts, add an app whose command is the absolute path of `launch_page.sh`
followed by `pattern`. On the client, use the same device, Moonlight version and
settings for every run: the resolution and frame rate of the scenario, HEVC, SDR, a
fixed bitrate, and the performance overlay on. Use standard Moonlight for both hosts.

## Each run

1. Before connecting, open the reference clock on the host monitor with
   `sh launch_page.sh reference`, and the terminal the sampler will run in. Polaris
   moves windows that open during a stream to the stream's screen, so a window opened
   later would land in the stream.
2. Connect from the client and start the app. The page's info line shows the size and
   refresh rate of the screen it is on, which should be the stream's, and the seconds
   since the page opened. If Hermes leaves the page on the host monitor, move it with
   Meta+Shift+Right.
3. Start the sampler from that terminal. It waits 10 s for the stream to settle and
   then measures for 120 s:

   ```sh
   python3 scripts/validation/virtual-display/measure_run.py \
     --host polaris --scenario 1440p120 --run 1 \
     --fps 120 --resolution 2560x1440 --bitrate-mbps 50 \
     --api-key-file ~/.config/polaris/polaris_state.json \
     --out ~/vd-compare/receipts
   ```

   For Hermes, pass `--host hermes` and leave out `--api-key-file`. The host label picks
   the log and config file to read, and `--log` and `--config` override them. The key
   file must be readable only by you. Polaris keeps its key in `polaris_state.json`,
   and that copy wins over an `api_key` line in `polaris.conf`.
4. While it runs, photograph the host monitor and the client screen together with a fast
   shutter, 1/1000 s or faster. Leave out photos where either clock is caught between
   two values. About 7 per run gives the 20 or so per host and scenario over three runs.
5. Note the client's overlay figures in `client.csv`. The overlay shows roughly the last
   second, so read it at the same points in every run and enter the mean. Moonlight-qt
   also logs the same figures for the whole stream as `Global video stats` when the
   stream ends, in its `Moonlight-*.log` in `%TEMP%` on Windows and on stderr on Linux.
   Those include the first seconds of the stream, before the sampler starts.
6. Disconnect once the receipt is written. The sampler exits with status 1 and marks the
   receipt incomplete if it was stopped early.

For the HDR check, use a scenario label of its own, such as `4k60-hdr`, and pass
`--dynamic-range hdr`. Polaris cannot stream HDR from a KWin virtual output, so it
streams SDR when asked for HDR, and the comparison points that out.

## Comparing

```sh
python3 scripts/validation/virtual-display/compare_runs.py ~/vd-compare/receipts \
  --client ~/vd-compare/client.csv --photos ~/vd-compare/photos.csv \
  --json ~/vd-compare/report.json > ~/vd-compare/report.md
```

Each table cell is the mean over runs, with the lowest and highest run in brackets.
Other hosts are set against Polaris where both measured the same thing. The report ends
with the versions each host ran with and a list of things to check before the results
are shared: runs that stopped early, missing timing lines, a stream that did not use
Host Virtual Display, a bitrate that moved, a display mode, frame rate or bitrate other
than the run's, and versions or stream settings that changed between runs or differ
between hosts.

`client.csv` has one row per run. Any reading may be left blank, and lines starting
with `#` are ignored:

```csv
scenario,host,run,received_fps,decoded_fps,rendered_fps,network_drop_pct,jitter_drop_pct,rtt_ms,rtt_variance_ms,decode_ms,queue_ms,render_ms,host_latency_min_ms,host_latency_max_ms,host_latency_avg_ms
1440p120,polaris,1,119.8,119.8,119.7,0,0,1,0,0.9,0.2,8.1,2.1,6.0,3.2
```

`photos.csv` has one row per photo, with both clocks as read. The clocks count seconds
modulo 100, and a pair either side of the wrap is handled:

```csv
scenario,host,run,photo,host_clock,stream_clock
1440p120,polaris,1,IMG_0412,37.482,37.441
```

## Privacy

A receipt holds no host name, user name, client name, address, path or key. It records
the kernel, CPU and GPU models, package versions, DRM connector states and the host
settings listed in `CONFIG_KEYS` that change per-frame work. A setting whose value
contains the home directory, or does not look like a plain value, is recorded as
`(not recorded)`. From Polaris's stats it keeps only the fields in `STATS_NUMBERS` and
`STATS_LABELS` and the mode strings of the display mode decision, so the client's name
and address are dropped. Read a receipt before attaching it anywhere.

## Tests

```sh
python3 -m unittest discover -s scripts/validation/virtual-display -p 'test_*.py'
```
