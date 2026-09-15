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


## Kernel receive timing and host egress comparison, September 14

The audio failure is still open. A diagnostic run located several large gaps
before audio left the host. It did not establish a repair or explain every
previous failure.

The worker image and native host remained at `091bd3dd`; the temporary enforcing
worker policy was from `22b56f64`. The first Android observer was built from
`8423c75a` with a retained source patch. That patch added a Linux socket timestamp
query to the existing optional audio observer. The final observer in Nova
`d63390c4` additionally records the preceding RTP sequence. A separate physical
connection verified those final fields. No receive wrapper is compiled into an
ordinary build.

A loopback probe running with the debug app's permissions distinguished
immediate reads from deliberate 40 ms delayed reads. On the real stream,
`SIOCGSTAMPNS` provided timestamps for every returned audio data packet in the
contained measurement windows. An external Android packet capture was not
available without additional device privileges.

The comparison used PEAK's unpaused offline airport scene, periodic controller
camera movement, a 1080p H.264 stream requested at 120 FPS and 8 Mbps, and the
existing stereo 48 kHz, 5 ms Opus contract. This is stream delivery evidence;
it does not measure unique game frames or input to photon latency.

| Measurement | Ten minute gameplay interval |
| --- | --- |
| Fully contained playback and receive windows | 59 each |
| Contained playback time | 590.219 seconds |
| Audio queue skips | 24 |
| Short writes, write errors, writes over 20 ms | 0, 0, 0 |
| Maximum AudioTrack write | 9.079 ms |
| Maximum time outside playback callback | 51.201 ms |
| Underruns at first and last window ends | 18 to 40 |
| Returned audio data packets with valid kernel timestamps | 118,025 |
| Maximum application receive gap | 76.928 ms |
| Maximum kernel arrival gap | 76.890 ms |
| Maximum kernel timestamp age at observation | 3.502 ms |
| Worker domain SELinux denials | 0 |

The complete connection also retained an 83 ms receive gap outside those
contained windows. Matching the current packet's RTP sequence to host egress
showed an 83.184 ms host gap and an 83.187 ms device kernel gap; Nova read that
packet about 0.199 ms after its kernel timestamp. Another event had a 76.779 ms
host gap, a 76.890 ms kernel gap, and a 0.137 ms kernel age. Those events were
already delayed on the host. Enlarging the Android audio buffer would not
remove their source.

The host capture retained Ethernet, IPv4, UDP and RTP headers with a 54 byte
snapshot limit. It covered 899.573 seconds, including startup and exit, and
contained 179,879 audio data packets. Tcpdump reported zero kernel drops. The
first observer recorded the current sequence only, so its comparison uses the
preceding captured host data packet, rather than proving the client's preceding
sequence. The final observer closes that diagnostic limitation for future runs.

Heavy compiler and linker activity was observed on the host during several
stalls. Both the host and worker cgroups showed CPU pressure, with no configured
CPU quota and no recorded quota throttling. Gaps became smaller after the
compiler processes ended, although a later burst still occurred. This is an
uncontrolled load observation, not proof that a particular build caused all
stalls. A controlled host load comparison is the next experiment. The worker's
Pulse capture, shared audio/video output pipe, media reader, and audio sender
need to be distinguished before changing scheduling or queue behavior.

The full connection also recorded a 48.593 ms AudioTrack write during startup.
It remains a separate observation from the large host egress gaps. Host/device
clock offsets were measured at both ends; they differed by about 26 ms across
the connection. Absolute one way delay estimates based on a single offset are
therefore unsuitable here. Monotonic application gaps and consecutive kernel
and host packet gaps provide the event comparison above.

Nova `b4e4e123` also presents the versioned Spaces response with explicit
`live_tuning: null` as **Fixed bitrate**, rather than indefinitely waiting for a
setting. Missing, malformed and unrecognized tuning status retains the unknown
state. The final APK displayed the corrected label on the physical client at
120 FPS. The focused UI, live tuning and API parsing suites passed 98 tests.
The observer built for ARM64, ARMv7 and x86-64. The ordinary ARM64 build's native
library exactly matched the original native library with diagnostics disabled.

The game exited through its controller menu. All profile homes were retained.
The temporary host and workers stopped, original client APK and display
preferences were restored, and the original enforcing worker policy was
restored. The normal Polaris service was not restarted. No runtime image was
signed, published or admitted to the download catalog by this experiment.


## Sustained Two Stream Followup, September 14

The [sustained streaming report](container-multiseat-sustained-streaming.md)
records two ordinary-build 15 minute intervals and a separate receive diagnostic.
Both game workers stayed running, but audio queue skips remained. Eight matched
RTP packet pairs in the diagnostic left the host within 5.317 ms and arrived at
Android's kernel as much as 65.889 ms apart. Nova read those packets within
0.481 ms of their kernel timestamps. These events locate delay after host egress;
they do not identify a particular network component or supersede the earlier host
stalls. Audio reliability remains an acceptance gate.

The [equal duration pacing and network comparison](container-multiseat-pacing-network.md)
reproduces delayed Android kernel arrival on the normal wireless path while host
packet spacing remains regular. A temporary direct wireless path was worse and
cannot serve as a clean reference. It also records the requested 240 FPS extension
and its measured delivery shortfall. Audio reliability and causal isolation remain
open; a wired handheld comparison is still needed.

## Wired Android Reference, September 15

A simultaneous four minute network probe sent 48,000 generated UDP datagrams to
each of two Wi-Fi handhelds and an Ethernet Shield. Each flow used 200 packets
per second, a 94 byte payload and CS6 priority. All 144,000 packets arrived
exactly once. The host retained software transmit timestamps and each client
recorded kernel and immediate application receive timestamps.

| Measurement | First Wi-Fi handheld | Second Wi-Fi handheld | Ethernet Shield |
| --- | ---: | ---: | ---: |
| Maximum kernel receive gap | 33.346 ms | 60.112 ms | 6.140 ms |
| Kernel receive gaps above 20 ms | 3 | 4 | 0 |
| Maximum driver transmit gap | 5.233 ms | 5.244 ms | 5.300 ms |
| Missing or duplicate packets | 0 | 0 | 0 |

Three consecutive packet pairs had matching 21 to 33 ms receive gaps on both
handhelds while the Shield received the corresponding pairs about 5 ms apart.
A separate 60 ms gap affected only the second handheld. Matching transmit
timestamps across all three flows were within 0.304 ms throughout the test.
Both wireless flows reordered their first two packets at startup; the delayed
pairs above exclude those reordered packets.

This strengthens the wireless branch as the lead for those pauses. It does not
distinguish the access point, radio conditions or client Wi-Fi behavior. It also
does not supersede the earlier host scheduling stalls. The handhelds were awake
on the same access point without active Wi-Fi locks; the Shield was asleep on
Ethernet. These were generated probes, not game streams. Different hardware,
power states and the absence of streaming load limit the comparison.

The next component comparison needs the same handheld on Ethernet or another
access point. The [Linux timestamping contract](https://docs.kernel.org/networking/timestamping.html)
places software transmit timestamps before physical transmission and receive
timestamps after driver delivery into the kernel. They do not measure wire or
radio arrival times. Comparisons use gaps within each clock, not absolute one
way latency.

A subsequent ordinary Nova build from
`06f549e8613cb051840fe95496897cc368c8b085` opened Control on the Shield using the
native host and Steam NVIDIA image from
`1e7c847d06bc84e62591825a0d3fe9fb225f9cc6`. The image configuration digest was
`sha256:cbf7d00e145e8b30082be97ba06bfc4988e9a91b5d8f941a8ae69a922ed26d1d`.
The stream requested 1080p60 and 8 Mbps and reached a saved gameplay scene.
Its 333 second session included startup and menus. Thirty two full playback
reports after the first report covered 320.083 seconds, with two queue skips,
no short writes or write errors, and an 8.116 ms maximum write. The first and
last reported underrun counters both read 2. The initial report separately
contained 17 queue skips and those two underruns.

The Shield became unavailable for testing during normal household use. Nova
lost foreground focus and shut down its stream. The sustained two-client
gameplay interval, reconnect exercise, physical Shield controller check and
audible acceptance were not completed. This partial observation is not an
audio pass. The second Space reached PEAK gameplay on the handheld and
responded to input from its controller device. Neither observation closes the
remaining audio reliability gate.
