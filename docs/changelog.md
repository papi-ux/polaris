# Changelog

## Polaris Notes

### 1.0.0 - 2026-04-20

- Prepared Polaris for the first public `v1.0.0` release line with refreshed public-facing docs, release metadata, and repo hygiene checks.
- Tightened the public repo surface rules so maintainer-local files and session checkpoint notes are blocked from the tracked tree.
- Landed the current Linux-first Polaris product surface:
  security hardening, reliability diagnostics, AI-guided stability feedback, and the redesigned operator-focused web console.

### 2026-04-17

- Reworked the AI optimizer feedback loop:
  recommendations now carry richer cache/source metadata, session history separates latest-session results from rolling trend data, and poor outcomes invalidate cached recommendations more aggressively.
- Fixed session grading so quality is evaluated against the actual target FPS rather than a fixed 60 FPS assumption.
- Normalized impossible runtime recommendations earlier, including disabling `virtual_display` automatically when headless `labwc` owns the stream output.
- Improved the web UI around AI status, settings search/jump/highlight, incident deduplication, pairing flow density, and Mission Control quick-fix links.
- Updated Linux Steam library launching so Gamepad UI stays in the isolated stream runtime and Steam titles stop bouncing back onto the host desktop during Polaris-driven launches.
- Refreshed the public Nova companion-app handoff so Obtainium points cleanly at Nova's public Android release asset.
- Polished the public AI Transparency copy and release-facing docs for the coordinated April 17 checkpoint.

@htmlonly
<script type="module" src="https://md-block.verou.me/md-block.js"></script>
<md-block
  hmin="2"
  src="https://raw.githubusercontent.com/LizardByte/Sunshine/changelog/CHANGELOG.md">
</md-block>
@endhtmlonly

<div class="section_buttons">

| Previous                              |                          Next |
|:--------------------------------------|------------------------------:|
| [Getting Started](getting_started.md) | [Docker](../DOCKER_README.md) |

</div>

<details style="display: none;">
  <summary></summary>
  [TOC]
</details>
