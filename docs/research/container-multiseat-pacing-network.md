# Spaces Pacing And Network Validation

September 14, 2026. This followup repeats the earlier 120 FPS pacing comparison
with equal 15 minute intervals and reversed order: Balanced, then Low Latency.
Audio reliability remains a separate acceptance requirement. Presentation
timestamps do not measure source game FPS or input latency.

Balanced again presented more evenly than Low Latency on the tested handheld.
The audio delay remains unresolved. A 240 FPS request was accepted, but Control
averaged 132.6 observed encoded frames per second with PEAK running alongside it.

## Configuration And Method

The ordinary Nova APK was revision
`6a01466ce8554bd23ee73402413a45ce0bb8bb8c`, SHA-256
`0c3ee408f4acf12d51390697f79a63023874cf23bfd5f543a926acef562b383e`.
Its native library was unchanged from the previous ordinary build. Receive
diagnostics and native debug loss injection were disabled for the pacing runs.
Polaris source HEAD was `4a65c7f572b3ec697e9eeb08d463a2931f1b9a2a`;
changes since the pinned `5acba5bb027525a69e3e035b0d1112da5c4f375f` native build
were documentation only. Host binary SHA-256 was
`f8dbae29cacd675b0e6478598d7cb38fc89b581b4c18c3d728f8209f111831ac`.
Both workers used image
`sha256:c089c4c20720979f5875847cb02f52d719b9ccc91f29a5c70ef08cc4125eccaa`,
from worker revision `091bd3dd840a60737517052458470b8e9660b322`.

The host used Fedora 44, an Intel Core i9-14900K, an RTX 4090, and driver
610.57.04. SELinux remained Enforcing. The Retroid Pocket 6 ran Android 13 and
used its existing 5 GHz Wi-Fi connection during the pacing measurements.
PEAK requested 1920 by 1080, H.264, 120 FPS, stereo audio, and 8000 kbps.
A local Moonlight client continued streaming Control at the same resolution,
codec, and bitrate, with 60 FPS and a software decoder.

PEAK was freshly launched into its offline airport before each condition.
Control retained the same worker and loaded saved scene throughout the pair.
That scene was freshly loaded for this followup, so comparisons with the earlier
report do not have an identical Control camera position. Small PEAK controller
camera movements and Control seat keyboard movements occurred every 30 seconds.
Periodic screenshots verified that the games remained loaded and unpaused.
This was not a fixed game benchmark, continuous combat, or validation of two
independent physical controller transports. No builds or network changes ran
during the measured pacing intervals.

SurfaceFlinger actual presentation timestamps were polled every half second.
The analysis checks overlapping timestamp buffers and reports the observation's
own coverage. Nova's rendered FPS counter counts frames submitted to the surface;
it is not a substitute for displayed cadence. Fully contained audio windows are
reported separately from startup and shutdown. Underrun endpoints are counters
at window ends, not exact measurement boundaries. Host captures retain only
Ethernet, IPv4, UDP, and RTP headers, with a 54 byte snapshot limit.

## Pacing Results

| Measurement | Balanced | Low Latency |
| --- | ---: | ---: |
| Timed interval | 900 seconds | 900 seconds |
| Worker identity samples | 180, unchanged | 180, unchanged |
| Presentation coverage | 899.974 seconds | 899.958 seconds |
| Presentation intervals | 106,647 | 93,568 |
| Mean displayed frames per second | 118.500 | 103.969 |
| Median frame interval | 8.368 ms | 8.368 ms |
| 99th percentile frame interval | 8.372 ms | 16.739 ms |
| Maximum frame interval | 58.564 ms | 75.292 ms |
| Intervals above 12.5 ms | 887 | 13,887 |
| Missing timestamp buffer overlap | 0 | 0 |
| Mean GPU utilization | 88.66% | 88.77% |
| Mean encoder utilization | 8.99% | 8.88% |
| Maximum GPU temperature | 60 C | 61 C |
| Audio queue skips in contained windows | 3 | 6 |
| Underrun counters at first / last retained window ends | 11 / 12 | 4 / 6 |
| Maximum playback callback idle time | 33.259 ms | 42.086 ms |
| Maximum captured host audio data gap | 19.834 ms | 13.134 ms |
| Captured host audio gaps above 20 ms | 0 | 0 |
| Worker SELinux denials | 0 | 0 |

Each audio result contains 89 fully contained windows, approximately 890 seconds.
Neither had short writes, write errors, or writes exceeding 20 ms. Each host
header capture covered approximately 899 seconds with no sequence discontinuity
and zero kernel capture drops. The approximately final second of each interval
was outside the flushed packet records. All 30 scheduled input cycles were issued in each run.

The equal duration comparison reproduces steadier presentation with Balanced.
It remains a sequential observation on one handheld, not randomized evidence
across devices. It does not measure the input latency tradeoff or establish that
pacing repairs audio. Product defaults were not changed.

## Alternative Network Path

Two direct 5 GHz access point preflights failed before the pacing measurements.
The first raced radio startup; the second reached the supplicant and failed to
start access point functionality. Each temporary connection was removed and the
host's original radio and wired default route were restored. The handheld's
network was unchanged during those attempts. No failed setup interval is treated
as a network performance measurement.

The diagnostic pair used the same diagnostic APK, SHA-256
`065c0f7107b88c7ff71b333acd6f82d2c3ffe71acdc6c1a994096859684004de`,
with native library SHA-256
`52cbdfd4cc854a45bca75677eaff87732ea629c6c8602026ff7323bd39323f19`.
Receive observation was enabled and debug loss injection remained disabled.
PEAK requested 60 FPS in both conditions, with the same codec, resolution,
bitrate, Low Latency pacing, and loaded offline airport. Control retained the
same 60 FPS worker and scene. The direct condition used a temporary 2.4 GHz
access point on the host's unused radio. Temporary firewall rules admitted
only the preview ports from the handheld address. The host retained its wired
default route. Android marked the direct connection as lacking internet access;
the game and streaming host retained their wired internet connection.

This comparison changes the access point, host network interface, and radio
band together. It can identify a difference between these paths, but cannot isolate
the router, interference, power saving, or either network driver's behavior.
The software timestamps bound delays between the host capture point and the
Android kernel receive point. They are not wire or radio hardware timestamps.
See the [kernel timestamping documentation](https://docs.kernel.org/networking/timestamping.html).

| Measurement | Normal 5 GHz Wi-Fi | Direct 2.4 GHz AP |
| --- | ---: | ---: |
| Timed interval | 300 seconds | 300 seconds |
| Workers unchanged | Yes | Yes |
| Contained playback windows | 29 | 29 |
| Audio queue skips | 43 | 1479 |
| Underrun counters at window ends | 7 / 28 | 53 / 360 |
| Receive data gaps above 20 ms | 12 | 1143 |
| Maximum Android kernel receive gap | 67.546 ms | 346.527 ms |
| Maximum captured host audio data gap | 10.351 ms | 9.785 ms |
| Captured host audio gaps above 20 ms | 0 | 0 |
| Worker SELinux denials | 0 | 0 |

The normal connection reproduced the earlier delay: ten window maximum events
matched consecutive RTP sequence pairs with 61.6 to 67.6 ms between application
receives, while those same packet pairs were captured at the host 4.512 to
6.023 ms apart. All retained receive packets had kernel timestamps. The matched
kernel ages were at most 0.235 ms. This places those delays after the host
capture point and before Android kernel reception, without identifying the
responsible link or driver.

The direct path was substantially worse and is not a clean reference link.
A periodic screenshot showed 53.46 percent video frame loss, and the Android
presentation observation averaged approximately 17.9 frames per second over
288.073 seconds. Its 29 window maximum receive events matched captured packet
pairs, including six nonconsecutive pairs with sequence steps of 6 to 19.
Those pairs must not be described as consecutive host packets. The 23
consecutive pairs were captured 1.514 to 5.461 ms apart. Host captures had no
sequence discontinuities or kernel capture drops in either condition.

Neither condition had audio short writes, write errors, or writes over 20 ms.
All ten scheduled input cycles were issued in each condition. Presentation polling began later
in the direct interval, and its narrower coverage is reported explicitly.
A wired handheld comparison remains necessary to separate radio behavior from
other causes; this experiment does not establish faulty PC hardware or a
router defect.

## 240 FPS Validation Scope

The requested extension targets the local Control stream at 1920 by 1080,
H.264, 8000 kbps, and 240 FPS while PEAK continues on the handheld at 60 FPS.
The ordinary Nova APK was restored for this phase. The local Moonlight client
uses a software decoder. The host desktop remains at its original 60 Hz mode.
The RP6 advertises only 60 and 120 Hz display modes, so this test does not bypass
Nova's display capability guard or establish physical 240 Hz presentation.
Worker configuration and observed incoming frame rates are reported
separately from requested settings. A 74 byte local loopback capture retains
Ethernet, IPv4, UDP, RTP, and video frame headers without picture payload.
The analysis deduplicates picture start headers by encoded frame identifier,
checks header layout and identifier continuity, and reports its own coverage.
Loopback records may represent UDP segmentation offload aggregates; they are
not physical wire packets. These counts measure observed encoded frame starts,
not proof that every complete frame was decoded. Neither source game FPS nor
input latency is measured by this test.

The worker received a 240000 millihertz display contract. Before gameplay,
the local client displayed 240.26 incoming and decoded FPS at Control's title
screen. Both games were loaded and unpaused during the measured 300 second
interval. Control was freshly loaded from its checkpoint; its camera and
position differed from the preceding 60 FPS comparison stream.

| Measurement | Result |
| --- | ---: |
| Requested Control stream rate | 240 FPS |
| Unique encoded frame identifiers | 39,791 |
| Frame start coverage | 299.993 seconds |
| Mean encoded frame start rate | 132.637 FPS |
| Full second frame count range | 120 to 140 |
| Frame identifier discontinuities | 0 |
| 99th percentile encoded frame start interval | 11.951 ms |
| Mean GPU utilization | 90.82% |
| Mean encoder utilization | 9.32% |
| Mean CPU utilization | 56.05% |
| Maximum GPU temperature | 60 C |
| Worker identity samples | 60, unchanged |
| Worker SELinux denials | 0 |
| PEAK audio queue skips | 28 |
| PEAK underrun counters at window ends | 16 / 30 |

The video capture had no invalid headers or kernel capture drops. A periodic
local client screenshot showed 131.78 incoming and decoded FPS and zero network
frame loss. This supports the measured delivery rate but is not a continuous
decoder trace. All ten scheduled input cycles were issued. The handheld continued receiving
approximately 60 FPS in periodic screenshots, while its retained audio windows
still showed queue skips and underruns. Its host audio capture had no gap above
20 ms, no sequence discontinuities, and no kernel capture drops.

The 240 FPS request is supported by the tested host contract, but this two game
workload did not sustain 240 encoded frames per second. The observation does not
separate game rendering, compositor capture, scheduling, and encoding costs.
Resource utilization percentages alone do not establish the limiting stage.
A followup needs stage timing and a fixed scene, followed by a physical 240 Hz
client test. Product defaults and Nova's RP6 display limit were unchanged.

## Research And Host Health

Android documents that when several MediaCodec buffers target one display
refresh, the surface shows the last buffer and drops the others. Nova's Low
Latency path submits decoded output using the current timestamp, whereas its
Balanced path uses Choreographer callbacks. This is a plausible mechanism for
the submitted versus displayed frame difference, not a demonstrated causal
trace. A followup should correlate decoder output, surface submission, and
actual presentation before changing the pacing policy. See the
[MediaCodec surface timing contract](https://developer.android.com/reference/android/media/MediaCodec#releaseOutputBuffer(int,%20long)).

The host journal also contained corrected PCIe receiver errors on a root port
leading to a Samsung 990 PRO SSD. The GPU and Ethernet controller use other
ports. The root port's fatal and nonfatal uncorrectable counters were zero.
The SSD reported no critical warning, media errors, or error log entries, with
100 percent spare capacity and 9 percent used. These observations do not
establish a failing SSD or connect that link's errors to the audio stalls.
No storage stress test, firmware update, PCIe setting change, or error
suppression was performed. The kernel distinguishes recoverable link errors
from uncorrectable errors in its
[PCIe AER guide](https://www.kernel.org/doc/html/next/PCI/pcieaer-howto.html).

## Cleanup And Remaining Gates

All five timed intervals completed, totaling 45 minutes. The games exited
through their menus and Steam showed cloud sync up to date. Both local comparison
clients and the temporary host stopped. The host and runner exited with code zero;
no test workers remained. All three Steam homes were retained and the empty IPC
directory was removed. The original worker policy hash was restored with SELinux
Enforcing, and the temporary seccomp file was removed. The normal Polaris service
retained its original process.

The temporary access point and its seven runtime firewall rules were removed
and absence verified. The host radio returned to disabled with its original
wired default route. The handheld forgot only the test network and rejoined its
original 5 GHz Wi-Fi. The ordinary Nova APK was restored and its installed hash
verified again. The original global FPS, overlay, and pacing preferences were
restored and verified, along with the 120 FPS Space setting. Private diagnostics,
headers, screenshots, and receipts are retained outside the repository.

Audio reliability, the input latency tradeoff, sustained 240 FPS, and physical
240 Hz presentation remain open. Next, correlate decode, surface submission,
and presentation for the pacing issue; use a wired handheld comparison for
audio; and measure game rendering, compositor capture, and encoding stages in a
fixed scene for the 240 FPS shortfall. These observations do not admit a runtime
image for publication or establish clean host installation acceptance.
