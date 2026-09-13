# Multiseat audio timing observations

On 2026-09-13, a bounded comparison exercised PEAK DX12 in its offline airport
through Nova on an RP6. Each interval lasted 300 seconds with periodic
controller camera input. The stream used 1920x1080 at 60 FPS, H.264, a 4000 kbps
stream budget, and stereo 48 kHz Opus with 5 ms packets at 128 kbps. The selected
video target was 3067 kbps.

These observations concern an unpaused loaded scene. They are not continuous
human gameplay, an audible assessment, or a latency benchmark.

## Changes and identities

The original worker image came from Polaris
`1c25a2eb6e5ba5e9e73fdd00c70bf21f57f46b0e`. The candidate came from
`343cf53cc4f641a4215692f527c016811f8f7108`. The candidate asks Pulse capture
for 5000 microseconds, matching the Opus packet duration. Its real backend
reported 5333 microseconds and a 197333 microsecond capture buffer. The
original requested 10000 microseconds. The capture buffer is not a measure of
end-to-end audio latency.

The unchanged native host binary came from `1c25a2eb6e5ba5e9e73fdd00c70bf21f57f46b0e`.
The two initial comparisons used Nova
`6031e49e412290ab8ce32f4680b801d5355149f6`, which adds playback diagnostics.
A third used `13b8a62b30c48512c384a7852adc9cd7ec2eb64a`, which requests
Android audio priority on the dedicated native playback thread. The live
priority was -16; the original was 0. AudioTrack's reported buffer remained
480 frames.

All three 60 FPS comparison APKs used `-PnovaNativeDebugChecks=false`. The packaged native library
hash was unchanged across those Nova comparisons. The candidate worker's
committed-source build passed all nine real provider checks.

## Results

| Observation | Original capture | 5 ms capture request | Capture plus audio priority |
| --- | ---: | ---: | ---: |
| Timed gameplay interval | 300 s | 300 s | 300 s |
| Pending audio warnings / queue skips | 9 | 0 | 4 |
| Underruns at first and last interval reports | 3 to 17 | 6 to 14 | 2 to 3 |
| Maximum AudioTrack write in contained windows | 15.104 ms | 16.048 ms | 8.966 ms |
| Maximum time outside callback | 32.041 ms | 9.011 ms | 32.409 ms |
| Short writes / write errors | 0 / 0 | 0 / 0 | 0 / 0 |
| Unrecoverable video / decoder watchdog messages | 0 / 0 | 0 / 0 | 0 / 0 |
| Host audio data gap p95 | 10.594 ms | 5.610 ms | 5.733 ms |
| Host audio data gap p99 | 10.688 ms | 6.372 ms | 6.366 ms |
| Host audio data gap maximum | 11.775 ms | 8.038 ms | 7.412 ms |
| Host audio data gaps below 1 ms | 4853 | 595 | 514 |

Each host header capture covered about 124 seconds within the gameplay
interval. None reported kernel capture drops. The data packet analysis excludes
FEC packets; combined audio traffic was about 336 kbps at the IP layer, including
encryption, headers and FEC.

The priority interval's four queue skips occurred in one early spike. Its
underrun count stayed at 3 afterward. This single sequential comparison does
not establish a causal reduction in underruns or eliminate the remaining
backlog. Startup still included roughly 50 ms AudioTrack writes and queue
skips outside the timed gameplay interval.

The ten second playback reports overlap interval boundaries. Only fully
contained reports contribute the write and callback maxima above. The underrun
endpoints are the first and last reports inside the interval, not exact
start and finish counters. Host and device clock offsets were measured.

## Isolated capture check

A separate disposable container had no network, GPU, input, host audio or
Steam volume access. It used a private PipeWire null sink and a synthetic tone
with the same Pulse capture and Opus encoding chain. Three 20 second runs
compared the original 10 ms request, the 5 ms request, and the original again.
Analysis excluded the first two seconds of each run.

| Capture request | Data gap p95 | Data gap p99 | Data gap maximum | Gaps below 1 ms |
| --- | ---: | ---: | ---: | ---: |
| 10 ms | 10.719 ms | 10.773 ms | 10.941 ms | 1800 |
| 5 ms | 5.446 ms | 5.487 ms | 8.314 ms | 225 |
| 10 ms repeat | 10.701 ms | 10.752 ms | 10.853 ms | 1800 |

The running graph used a 256 frame quantum at 48 kHz in all three runs.
The repeat supports the observed capture batching effect. The exact backend
latency is logged because the requested value may be rounded.

## Interpretation and remaining work

The capture change reduces paired packet bursts at host egress. Host capture
timestamps do not prove packet arrival times at the handheld, audible quality,
or the cause of a client queue spike. The priority comparison keeps a small
audio buffer and narrows the observed write delays, but longer repeats and
audible acceptance remain necessary.

PEAK returned to Steam and completed cloud sync after each connection.
Cleanup retained all three profile homes, retired the test workers and IPC,
and restored the original SELinux policy with enforcement and the normal
service active. Raw logs, captures, screenshots and detailed identities remain
private.

GStreamer documents requested and actual capture timing in
[GstAudioBaseSrc](https://gstreamer.freedesktop.org/documentation/audio/gstaudiobasesrc.html).
Android defines application write buffer underruns in
[AudioTrack](https://developer.android.com/reference/android/media/AudioTrack#getUnderrunCount()).
Neither counter is an end-to-end measurement.

## Higher bitrate at 120 FPS

The same encoder image and Nova audio priority build then ran PEAK's offline
airport at 1920x1080x120. An 8000 kbps stream budget selected a 6507 kbps video
target. The worker received 120000 millihertz, the decoder was configured for
120 FPS, and sampled overlays showed 120 FPS. This establishes delivered stream
cadence, not a count of unique game-rendered frames.

The 600 second interval logged no unrecoverable video frames or decoder
watchdog messages, but it had 2420 pending audio warnings. Across the first and
last playback reports, AudioTrack underruns rose from 14 to 829. Fully contained
windows recorded 2392 queue skips, a 9.040 ms maximum write and a 110.805 ms
maximum gap outside the callback. The approximately 124 second host capture
had no reported kernel drops; its longest audio data gap was 7.556 ms. The
high bitrate audio result failed.

A subsequent 300 second 120 FPS comparison at 4000 kbps logged four pending
audio warnings near a report boundary. Its 29 fully contained playback windows
had no queue skips, a 9.225 ms maximum write and a 0.970 ms maximum callback
idle gap. The first and last underrun reports both read 18. There were no
unrecoverable video frames or watchdog messages. The host capture's maximum
audio data gap was 7.412 ms with no reported kernel drops. Image quality was
visibly lower at this budget. This sequential comparison is insufficient to
attribute the earlier failure to bitrate.

For further diagnosis, Nova's optional receive observer was built from
`15555ac1eab3ca72edb0bc1d1d7964ab604225cc` with
`-PnovaAudioReceiveDiagnostics=true -PnovaNativeDebugChecks=false`.
It observes the existing audio socket call without editing the vendored core.
Its native library differs from the ordinary priority APK, and its results
must be recorded separately. Application receive times still include kernel
and thread scheduling; they are not radio arrival timestamps.

The instrumented 300 second repeat at 8000 kbps logged no pending audio
warnings, queue skips, unrecoverable video frames or decoder watchdog messages.
The first and last underrun reports were both zero. Fully contained playback
windows had an 8.843 ms maximum write and a 0.913 ms maximum callback idle gap.

The 29 full receive windows had no socket timeouts or errors. Their longest
audio data gap was 22.154 ms, with one gap over 20 ms. The 12 receive windows
fully contained in the host capture had a 19.862 ms maximum data gap and no
gaps over 20 ms. The approximately 124 second host capture had an 11.929 ms
maximum data gap and no reported kernel drops. Receive timing did not reproduce
the earlier large callback gaps, so it did not identify their cause. This
instrumented repeat does not supersede the failed ordinary-build observation.

## Two simultaneous 120 FPS seats

The same instrumented Nova connection then ran alongside a local Moonlight
client streaming Control Ultimate Edition from a second Steam profile. Both
clients requested 1920x1080 at 120 FPS with an 8000 kbps budget. Both workers
used the candidate image, received 120000 millihertz, and retained distinct
profile storage and their original identities for a 300 second interval.

Both games stayed unpaused in loaded scenes with periodic bounded input.
Sampled overlays showed 120 FPS on the RP6 and about 120 FPS decoded and
rendered by the local client. The final local sample was 119.90 FPS with zero
reported network or jitter frame drops. The RP6 final sample reported 120 FPS,
a 119 FPS one percent low and 6.0 ms decode time. These are delivered stream
observations, not a count of unique frames rendered by either game.

Nova logged no pending audio warnings, unrecoverable video frames or decoder
watchdog messages during the interval. Its 29 fully contained playback windows
had no queue skips, a 9.041 ms maximum write and a 1.136 ms maximum callback
idle gap. The first and last underrun counters were both 2. Those two underruns
appeared during second-seat startup before the measured interval.

The 29 full receive windows reported no socket errors, timeouts or data gaps
over 20 ms; their longest data gap was 16.192 ms. The roughly 124 second host
capture had no reported kernel drops and a 9.408 ms maximum audio data gap.
The host capture covered only the handheld's audio flow.

RP6 input used its controller device. Control input was sent to its verified
seat keyboard, so this does not validate transport from a second physical
controller. The test was a bounded scene observation, not continuous human
play or a combat benchmark. The earlier failed ordinary 120 FPS, 8000 kbps
audio interval remains a release blocker; the quiet diagnostic repeats do not
identify or fix its cause.

Both games exited through their own menus and Steam showed cloud sync up to
date for both. Requested cleanup completed with host exit zero, no remaining
test workers or IPC, all three profile homes retained, the original SELinux
policy restored in enforcing mode, and the normal service active. Nova's
previous 60 FPS preference was restored. An ordinary build from
`15555ac1eab3ca72edb0bc1d1d7964ab604225cc` was installed with both diagnostic
flags false, preserving application data. Its native library hash matched the
ordinary priority build.
