# Spaces Sustained Streaming Validation

September 14, 2026. Two simultaneous game streams completed 15 minutes at 60 FPS,
followed by 15 minutes with the handheld requesting 120 FPS. No worker restarted
or disconnected unexpectedly. Audio reliability did not pass. These measurements
do not establish release readiness.

## Configuration

The native host was revision `5acba5bb027525a69e3e035b0d1112da5c4f375f`, with binary
SHA-256 `f8dbae29cacd675b0e6478598d7cb38fc89b581b4c18c3d728f8209f111831ac`.
The worker image was
`sha256:c089c4c20720979f5875847cb02f52d719b9ccc91f29a5c70ef08cc4125eccaa`,
built from worker revision `091bd3dd840a60737517052458470b8e9660b322`.
The ordinary Nova baseline was revision
`c405be2e6620bd5064aa011727774e00108dde33`, APK SHA-256
`e8ef8d6dfcf67e0a2f32929ca4e70caefa7bd1246c503e410a12059a4e77b3f5`.
Native debug loss injection and optional audio receive diagnostics were disabled.

The host used Fedora 44, an Intel Core i9-14900K, an NVIDIA GeForce RTX 4090, and
driver 610.57.04. SELinux remained Enforcing with the previously validated scoped
worker policy candidate. The client was a Retroid Pocket 6 running Android 13
over 5 GHz Wi-Fi. A local Moonlight window streamed Control at 60 FPS using a
software decoder. PEAK streamed to the handheld. Both requested 1920 by 1080,
H.264, stereo audio, and 8000 kbps. Nova's existing frame-pacing preference was
Low Latency. The worker used separate retained Steam homes.

Control was loaded into an existing saved scene and PEAK into its offline airport.
Small controller camera movements and seat keyboard movements occurred every
30 seconds. This exercised loaded scenes rather than a fixed game benchmark or
a continuous combat workload. Only the handheld input traversed a physical
client controller path. The local comparison used the verified seat keyboard.
Existing unrelated host applications remained running; no builds ran during the
two measured intervals.

## Ordinary Build Results

| Measurement | Both At 60 FPS | Control At 60, Handheld At 120 FPS |
| --- | ---: | ---: |
| Timed interval | 900 seconds | 900 seconds |
| Worker identity checks | 180, unchanged | 180, unchanged |
| Handheld incoming FPS, eight overlay samples | 59.82 to 60.35 | 119.11 to 120.60 |
| Handheld rendered FPS, eight overlay samples | 58.07 to 60.35 | 114.93 to 120.10 |
| Handheld reported network frame loss | 0% in all eight samples | 0% in all eight samples |
| Handheld decode time, sampled range | 7.87 to 9.43 ms | 5.46 to 5.86 ms |
| Mean GPU utilization | 88.09% | 89.45% |
| Mean encoder utilization | 5.97% | 8.82% |
| Maximum GPU temperature | 59 C | 59 C |
| Audio queue skips in fully contained windows | 132 | 26 |
| Audio underrun counters at first/last retained window ends | 10 / 66 | 6 / 21 |
| Maximum playback callback idle time | 81.677 ms | 34.530 ms |
| Maximum captured host audio packet gap | 12.625 ms | 10.588 ms |
| Captured host audio gaps above 20 ms | 0 | 0 |
| Worker SELinux denials | 0 | 0 |

Each audio column contains 89 fully contained ten-second windows, approximately
890 seconds. Underrun endpoints are window ends, not exact interval boundaries.
The host captures retained only Ethernet, IPv4, UDP, and RTP headers. Each covered
approximately 899 seconds, had no data sequence discontinuity, and reported zero
kernel packet drops. Capture shutdown left approximately the final second outside
the retained packet records. No audio payload was captured.

Android SurfaceFlinger presentation timestamps provided a separate pacing view.
The 60 FPS observation covered 687 seconds and 41,093 presentations, with median
16.733 ms and 99th percentile 25.103 ms intervals. The 120 FPS observation covered
889 seconds and 92,289 presentations, with median 8.367 ms and 99th percentile
16.738 ms intervals. Polls overlapped without a missing timestamp-buffer interval.
The latter cadence was uneven despite incoming stream FPS near 120. Presentation
timestamps are not source-game FPS, unique rendered game frames, or an input-latency
measurement. Periodic screenshots and timestamp polling were observers during
these intervals. The first audio stalls preceded the presentation monitor.

## Isolation And Navigation

Disconnecting the handheld retired only its worker. Reopening created a new
worker using the same retained Steam home; it did not resume the previous running
game. The current worker capability advertises disconnect-and-resume as unsupported.
Control retained the same worker and start time throughout these transitions.

The handheld selected Control's occupied Space using the D-pad and A. Nova showed
In Use and disabled opening. Selecting PEAK again left Control running and allowed
a new PEAK stream. Selection did not grant access to the third, unassigned Space.

The test exposed a navigation defect: Done from Stream Settings returned to the
older detail screen without the chooser and current status. Nova commit
`6a01466ce8554bd23ee73402413a45ce0bb8bb8c` returns settings opened from Library
directly to Library's selected Space. Eighty-one focused existing tests passed.
Touch Done, controller B, and Android Back all passed on the handheld. The ordinary
APK read-back matched SHA-256
`0c3ee408f4acf12d51390697f79a63023874cf23bfd5f543a926acef562b383e`.
Its native streaming library remained unchanged at
`8f1e444a13f8254ae086f728ad37fc9c76c0dfee966549c6d805adbc9d95cbf0`.

## Matched Audio Arrival Diagnostic

A separate 300 second run used the navigation fix and the optional audio receive
observer, with both games loaded and both streams requesting 60 FPS at 8000 kbps.
The diagnostic APK SHA-256 was
`065c0f7107b88c7ff71b333acd6f82d2c3ffe71acdc6c1a994096859684004de`;
its native library was
`52cbdfd4cc854a45bca75677eaff87732ea629c6c8602026ff7323bd39323f19`.
Native debug loss injection remained disabled. An earlier aborted interval that
started during game loading was excluded. No build ran during the measured run.

The 29 contained receive windows covered 290.041 seconds and 58,009 audio data
packets. Every packet had a valid kernel timestamp. There were no socket timeouts,
receive errors, kernel ordering errors, or kernel timestamp ages above 20 ms.
Eight gaps exceeded 20 ms at both application receive and kernel arrival. Matching
each event's preceding and current RTP sequence to the host capture gave:

| Measurement | Eight Matched Events |
| --- | ---: |
| Largest application receive gap | 65.902 ms |
| Largest kernel arrival gap | 65.889 ms |
| Largest gap between the same packet pair at host egress | 5.317 ms |
| Largest kernel timestamp age when Nova observed these packets | 0.481 ms |
| RTP sequence step | 1 for every pair |

The whole host capture had a 10.137 ms maximum data gap, no sequence discontinuity,
and zero kernel capture drops. Contained playback windows still recorded 29 queue
skips, with underrun endpoints rising from 12 to 28. No short writes, write errors,
or writes over 20 ms occurred. Both workers remained unchanged and the worker
domain recorded no SELinux denials.

These matched events locate the additional delay between host egress and Android
kernel arrival. They do not show a Nova receive thread or AudioTrack write stall,
and do not distinguish the access point, radio, client driver, or another part of
that path. Socket timestamps are not radio arrival timestamps. Comparing the same
packet pair avoids treating a host/device wall-clock offset as one-way latency.
The observer's maximum measured cost was 0.612 ms, so this instrumented run remains
separate from the ordinary-build baseline.

The stream already held Android's high performance and low latency Wi-Fi locks,
and captured audio used DSCP 48. No access point, Wi-Fi power setting, or audio
buffer was changed. Earlier [host egress stalls under uncontrolled build load](container-multiseat-audio-timing.md#kernel-receive-timing-and-host-egress-comparison-september-14)
remain a distinct observation; this run does not explain every prior failure.

## Balanced Pacing Comparison At 120 FPS

The ordinary navigation-fix APK was restored and its installed hash verified
before a separate 300 second run. Nova used Balanced pacing, with PEAK freshly
loaded into its offline airport at 120 FPS and Control continuing at 60 FPS.
Resolution, codec, stream budgets, worker image, and ordinary native library
remained the same. No build ran during this interval.

| Android Presentation Measurement | Earlier Low Latency Run | Balanced Run |
| --- | ---: | ---: |
| Timestamp coverage | 889.454 seconds | 287.143 seconds |
| Presentations retained | 92,289 | 34,076 |
| Mean presentations per second over coverage | 103.758 | 118.669 |
| Median interval | 8.367 ms | 8.366 ms |
| 99th percentile interval | 16.738 ms | 8.371 ms |
| Maximum interval | 83.679 ms | 41.830 ms |
| Intervals above 12.5 ms | 13,859 | 240 |
| Missing timestamp-buffer overlap | 0 | 0 |

Both workers remained unchanged across all 60 identity samples. Mean GPU load
was 89.58%, encoder load 8.87%, and maximum GPU temperature 59 C. There were no
worker SELinux denials. The 29 contained playback windows recorded no queue skips,
short writes, write errors, or writes above 20 ms, but underrun endpoints rose
from 1 to 3. The maximum callback idle time was 21.117 ms. The host capture had
a 12.197 ms maximum audio data gap, no sequence discontinuities, and zero kernel
capture drops.

Balanced produced steadier presentation in this run. This sequential comparison
uses different observation durations and a fresh PEAK launch; it is not a repeated,
randomized benchmark or a measurement of the latency tradeoff. The quieter audio
interval does not establish that pacing repairs the arrival stalls. Existing user
preferences and product defaults are retained pending a longer comparison and
human acceptance.

## Cleanup

Both games exited through their own menus, and Steam showed cloud sync up to date
for each. The comparison client, temporary host, test workers, and IPC stopped;
the host exited with code zero. All three retained Steam homes remained present.
The original worker policy was restored with SELinux still Enforcing, and the
temporary seccomp installation was removed. The regular Polaris service retained
its original process, and unrelated containers remained running.

Nova's original global FPS, overlay, and Low Latency pacing preferences were
restored and verified. The ordinary navigation-fix APK remained installed with
its hash verified again after cleanup. No receive diagnostic build remained on
the handheld. Private logs, captures, screenshots, scripts, and receipts were
retained separately from this report.

## Remaining Gates

Audio arrival jitter and recovery remain open. A quieter interval at 120 FPS is
not an audio repair or evidence that a higher frame rate fixes the problem.
Sustained, evenly presented 120 FPS, human audible acceptance, and two independent
physical controller transports remain separate acceptance requirements.
Browser streaming, AMD Spaces acceptance, other distributions, package publication,
and unattended resumable game sessions are not established by this run.

The [pacing and network followup](container-multiseat-pacing-network.md) completes
the equal duration comparison in reversed order and records a separate alternative
network trial. Balanced again presented more evenly, while the degraded direct
wireless path did not resolve the audio cause. The report also measures a 240 FPS
request under two game workloads; accepting the request did not mean sustaining it.
Stage timing and a wired handheld comparison are the next diagnostic steps.
Runtime publication and clean-host installation acceptance remain necessary before
admitting an image to the download catalog.
