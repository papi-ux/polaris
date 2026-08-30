# Fix a bad stream with Doctor

When a game feels wrong, keep the stream open and let Doctor measure it. Doctor separates network,
host, and client evidence, tells you what is actually confirmed, and offers the safest next step it
can perform. You should not need to translate graphs before getting back to the game.

Open **Command Center** in Nova or **Mission Control** in the Polaris web UI while the affected
stream is still running.

## Read the verdict

Doctor reports one of four plain outcomes:

| Verdict | What it means |
|---|---|
| **Network** | Fresh media-loss or round-trip evidence points to the connection. Control-channel observations alone do not prove a media problem. |
| **Host** | Capture cadence, frame pacing, encode time, or the host render path is missing the stream target. |
| **Client** | Received, decoded, or rendered evidence points to the playback device when those measurements are available. |
| **No confirmed issue** | The available evidence does not support blaming one stage. Unavailable measurements stay unknown instead of becoming a guess. |

Static menus and repeated frames do not by themselves prove a pacing fault. Doctor waits for useful
coverage and keeps warning evidence visible instead of calling the stream stable beside it.

## Pick the offered action

Doctor uses a small action vocabulary so the button says what will happen:

- **Auto Fix** changes one reversible setting in the current stream. In this release that means one
  guarded bitrate step backed by fresh network evidence. Doctor verifies the encoder and the next
  evidence window, then restores the previous live target if verification fails.
- **Recheck** gathers a fresh read-only measurement. It does not change the stream.
- **Manual** explains the next check when Polaris cannot safely act for you.
- **Undo** restores the previous live bitrate while the same stream generation still owns it.

A change that needs a new stream is not an Auto Fix. Fresh-launch experiments are a separate future
**Run a trial** workflow and are not enabled in this release.

## Follow the result

After Auto Fix, Doctor keeps the outcome visible:

| State | What to do |
|---|---|
| **Watching** | Keep playing for a few seconds while Doctor measures the changed stream. |
| **Verified** | The target metric improved without a guardrail regression. Keep playing. |
| **Restored** | Verification failed and the previous live target was restored. Review the next guidance. |
| **Needs attention** | Polaris stopped rather than stacking another guess. Reconnect if encoder restoration could not be confirmed, then follow the manual guidance. |

Auto Fix is scoped to the active owner, app, and stream generation. A reconnect, second controller,
or newer manual bitrate choice retires the old receipt instead of letting it mutate a different
stream.

## Launch preset is separate

**Auto**, **Quality**, **High FPS**, and **Stability** are deterministic launch presets. They resolve
the next launch from your explicit choice, paired-client settings, and current host capabilities.
Doctor history and AI output do not silently rewrite them, and Doctor never changes Private Stream,
Host Virtual Display, or Mirror Desktop topology.

## Optional AI explanation

AI is optional. When configured, it can turn Doctor's structured evidence into a shorter plain-
language explanation. The AI response is informational: it cannot define the action, target
bitrate, confidence used by Doctor, or next-launch policy. Deterministic Polaris code remains the
only authority for the button and its settings.

If the verdict still does not match what you see, copy the diagnostics from Command Center or export
a redacted support bundle from Polaris before ending the stream. That preserves the measurements
needed to investigate the real bottleneck.
