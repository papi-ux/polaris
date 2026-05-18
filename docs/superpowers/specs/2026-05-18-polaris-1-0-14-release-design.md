# Polaris v1.0.14 Release Design

## Context

Polaris `v1.0.13` is the latest published release. The local `master` branch contains the post-`v1.0.13` release work, and the current workspace also has an uncommitted display-selection hardening fix in `src/video.cpp`, `src/video.h`, and `tests/unit/test_video.cpp`.

`v1.0.14` should be prepared as a patch release that includes both the already-committed post-`v1.0.13` changes and the display-list guard currently in the workspace.

## Release Scope

The release should cover:

- Steam and library launch polish, including direct Steam launch behavior and multi-library scanning improvements.
- NVENC split-frame encoding support and supporting configuration/docs updates.
- Safer adaptive bitrate and optimizer behavior around paired-client bitrate, recovery profiles, and clamp handling.
- AMD telemetry and Linux runtime diagnostics improvements.
- Login/session cleanup and fallback improvements for Linux streaming sessions.
- Developer cleanup tooling for safe local build/runtime artifact cleanup.
- Display-selection hardening so capture setup handles an empty display list without clamping against an invalid range.

## Documentation Updates

The README should replace the stale `v1.0.12` "What's New" section with a concise `v1.0.14` summary. It should focus on user-visible release value rather than listing every implementation detail.

The changelog should add a `v1.0.14` section above `v1.0.12`, keep `Unreleased` empty, and preserve the existing release-history style.

## Versioning

`CMakeLists.txt` should be bumped to `1.0.14` as part of release prep. The release tag should be `v1.0.14`.

## Testing

Run focused checks for the changed release scope:

- Unit tests covering the display-selection helper and nearby video behavior.
- Unit tests touched by the post-`v1.0.13` changes when practical.
- Documentation/release reference checks if available.

If full release packaging is too expensive locally, rely on GitHub Actions release workflows after the tag is pushed and report that packaging validation is CI-owned.

## Publish Flow

Use the repository branch convention, not `codex/*`. Commit the implementation and docs with an intentional release-prep message, push to the `polaris` remote, and create the `v1.0.14` tag only after local validation is complete.

The GitHub release notes should use a polished public summary with highlights, impact, release assets, and any paired Nova guidance if applicable.
