# Spaces Acceptance, September 15, 2026

## Result

Two configured Steam Spaces completed a simultaneous 15 minute test at 1080p
and 60 FPS. Control Ultimate Edition ran on a Shield connected by Ethernet;
PEAK ran on a Retroid Pocket 6 connected by Wi-Fi. The listener reported clean
Shield audio. The RP6 continued to accumulate audio underruns.

A fresh Fedora 44 VM also passed native package installation, Docker
installation, setup security checks, reboot checks, and removal. It had no GPU
passthrough. First runtime download and fresh NVIDIA gameplay installation
remain untested.

These results advance the Spaces preview. They do not close the RP6 audio
reliability investigation or establish a complete first installation from
published release artifacts.

## Builds And Conditions

| Item | Tested value |
| --- | --- |
| Polaris candidate | `f99dd59bde6eb59ff2f62a01e01a01506971ba7d` |
| Native host and Steam NVIDIA runtime source | `1e7c847d06bc84e62591825a0d3fe9fb225f9cc6` |
| Nova candidate | `ccbefcf6ba1ed5381177729e9dbf811acdc16b16` |
| Installed ordinary Nova source | `06f549e8613cb051840fe95496897cc368c8b085` |
| Runtime image configuration digest | `sha256:cbf7d00e145e8b30082be97ba06bfc4988e9a91b5d8f941a8ae69a922ed26d1d` |
| Host graphics | NVIDIA RTX 4090, driver 610.57.04 |
| Each requested stream | 1920 × 1080, 60 FPS, H.264, SDR, 8 Mbps, stereo |
| Client receive diagnostics | Disabled |

Each candidate differs from its physically tested source only in documentation.
The image configuration digest identifies the tested local image; it is not a
published registry manifest or a download instruction.

Both games remained in loaded scenes with 30 cycles of bounded input during the
900 second interval. All 180 worker samples retained the original two worker
identities. RP6 input passed through its controller device. Control input was
sent directly to its seat keyboard, so this run does not validate a physical
Shield controller's transport. Sampled overlays showed approximately 60 FPS;
they do not measure unique game frames or input to photon latency.

## Audio Measurements

There were 89 fully contained playback windows per client, covering approximately
890 seconds. Counter endpoints are the first and last reports within the
interval, not exact boundary readings.

| Measurement | Shield Ethernet | RP6 Wi-Fi |
| --- | ---: | ---: |
| Underrun counters | 1 to 1 | 4 to 107 |
| Queue skips | 9 | 250 |
| Short writes / write errors | 0 / 0 | 0 / 0 |
| AudioTrack writes longer than 20 ms | 0 | 0 |
| Maximum AudioTrack write | 8.699 ms | 9.129 ms |
| Maximum time outside playback callback | 3.458 ms | 43.172 ms |
| Maximum pending audio | 40 ms | 65 ms |
| Reported AudioTrack buffer | 512 frames | 480 frames |
| Host audio data gap p99 | 6.920 ms | 6.756 ms |
| Maximum host audio data gap | 12.531 ms | 11.952 ms |
| Host audio data gaps over 20 ms | 0 | 0 |
| Nonconsecutive captured audio data sequences | 0 | 0 |

The host header capture covered approximately 899 seconds per flow and reported
zero kernel capture drops. FEC packets were excluded from data gap calculations.
Clock estimates at both ends were applied conservatively to exclude boundary
playback reports. The retained logs contained no matches for the analyzer's
unrecoverable video, decoder watchdog, fatal exception, or connection termination
warning set during the interval.

The listener reported clean Shield audio. Listening duration was not measured,
so this acceptance applies to the portion heard. The initial underrun was
already present before the interval, and nine queue skips remain in diagnostics.

Regular host egress alongside long RP6 callback gaps does not identify the access
point, client Wi-Fi, kernel delivery, or application scheduling as the cause.
The ordinary client build lacks receive timing observations. The devices also
ran different games and used different audio buffers. Earlier host scheduling
stalls remain separate evidence; this quieter interval does not disprove them.

The next useful comparison keeps the RP6, game, and stream settings unchanged
while using Ethernet or another access point. The available USB cable did not
provide a wired streaming comparison through the device's normal settings. No
buffering, queue, QoS, or scheduling change is established as a fix by this run.

## Reopening A Space

Backgrounding Nova retired the RP6 worker while the Shield worker remained
unchanged. Opening the same Space created a new worker using the retained Steam
home. PEAK loaded its offline airport and responded to controller camera input.
Control remained visible in gameplay on the Shield during this exercise.

This verifies reopening and isolation between the two Spaces. The current
runtime ends the running game on disconnect. It retains installed games and
saved data, but does not preserve the game in memory for a later reconnect.

Both games were exited through their menus, and Steam reported cloud sync up
to date. Temporary stream preferences and the test frame rate override were
restored and checked. The preview shut down with no workers or IPC left behind,
and all four existing player homes were retained. SELinux remained enforcing;
the audit search found no AVC or USER_AVC records from startup through cleanup.

## Fresh Fedora Installation

A new Fedora 44 cloud VM installed Docker Engine 29.8.0 and the actual candidate
CI RPM. The RPM SHA-256 was
`25cb96c0fcf7f9e86101adb5684bf1a9baeeaf3a232c7c16d82d1af14a30bcea`.

All eight installed security checks passed, including input access restrictions,
policy ownership, and interrupted operation recovery. Setup APIs rejected
unauthenticated requests and cookie requests without CSRF protection. A real
guest reboot was verified, and the checks passed again. The owned policy and
native package were removed afterward, with SELinux still enforcing.

The VM correctly reported missing GPU readiness. The RPM reported CUDA disabled,
and the runtime download catalog remained unpublished. This is acceptance of
native package and Docker installation on a fresh Fedora system. It does not
establish fresh NVIDIA gameplay, a successful runtime download, signed public
package installation, or equivalent Arch and Ubuntu results.

## Release Boundaries

At the recorded candidate heads, Polaris native CI, all four runtime profile
jobs, and Nova build CI completed successfully. The Steam runtime job passed
after one retry at the same source revision. Its first combined provider test
build failed without a diagnostic identifying the failing test; that failure
remains unexplained.

Final source review and integration, version and artifact binding, merge CI,
signing, runtime publication and catalog admission, and public installation
readback remain release preparation work. RP6 audio reliability remains open.
These measurements do not establish sustained 120 or 240 FPS gameplay.

See the [Spaces setup guide](../spaces.md),
[audio timing investigation](container-multiseat-audio-timing.md), and
[sustained streaming report](container-multiseat-sustained-streaming.md) for setup,
earlier measurements, and their separate limits. Raw captures, logs, screenshots,
and machine identifiers remain private.
