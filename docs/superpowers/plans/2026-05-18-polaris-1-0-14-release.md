# Polaris v1.0.14 Release Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Prepare Polaris `v1.0.14` with release docs, version metadata, and the display-selection hardening fix.

**Architecture:** This is a release-prep change. The implementation keeps code changes scoped to the existing `video` display-selection path, updates the public release narrative in README/changelog, and bumps CMake project version metadata for tagged package builds.

**Tech Stack:** C++17, CMake, GoogleTest/CTest, Markdown, GitHub release workflow.

---

## File Structure

- Modify `src/video.cpp`: keep the display-list guard and `clamp_display_index` helper so empty display lists return early instead of clamping against an invalid range.
- Modify `src/video.h`: expose `clamp_display_index_for_tests`.
- Modify `tests/unit/test_video.cpp`: keep focused tests for empty and out-of-range display selection.
- Modify `CMakeLists.txt`: bump `project(Polaris VERSION 1.0.14 ...)`.
- Modify `README.md`: replace the stale `v1.0.12` "What's New" content with a concise `v1.0.14` public summary.
- Modify `docs/changelog.md`: add the `v1.0.14` release section above `v1.0.12`.

### Task 1: Display-Selection Hardening

**Files:**
- Modify: `src/video.cpp`
- Modify: `src/video.h`
- Test: `tests/unit/test_video.cpp`

- [ ] **Step 1: Confirm the focused tests exist**

The file `tests/unit/test_video.cpp` must contain:

```cpp
TEST(VideoDisplaySelectionTests, RejectsDisplaySwitchWhenDisplayListIsEmpty) {
  EXPECT_EQ(video::clamp_display_index_for_tests(1, 0), std::nullopt);
}

TEST(VideoDisplaySelectionTests, ClampsDisplaySwitchToAvailableDisplayRange) {
  EXPECT_EQ(video::clamp_display_index_for_tests(-1, 3), 0);
  EXPECT_EQ(video::clamp_display_index_for_tests(1, 3), 1);
  EXPECT_EQ(video::clamp_display_index_for_tests(8, 3), 2);
}
```

- [ ] **Step 2: Confirm the implementation exposes the test seam**

`src/video.h` must contain:

```cpp
std::optional<int> clamp_display_index_for_tests(int requested_index, std::size_t display_count);
```

`src/video.cpp` must contain:

```cpp
std::optional<int> clamp_display_index(int requested_index, std::size_t display_count) {
  if (display_count == 0) {
    return std::nullopt;
  }

  const auto max_index = static_cast<int>(std::min(
    display_count - 1,
    static_cast<std::size_t>(std::numeric_limits<int>::max())
  ));
  return std::clamp(requested_index, 0, max_index);
}
```

- [ ] **Step 3: Run the focused video test**

Run:

```bash
ctest --test-dir build --output-on-failure -R test_video
```

Expected: the `test_video` target passes. If `build` does not exist, configure/build the test target first:

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target test_video -j"$(nproc)"
ctest --test-dir build --output-on-failure -R test_video
```

### Task 2: Release Version and Public Docs

**Files:**
- Modify: `CMakeLists.txt`
- Modify: `README.md`
- Modify: `docs/changelog.md`

- [ ] **Step 1: Bump the CMake project version**

Change the top-level project declaration to:

```cmake
project(Polaris VERSION 1.0.14
        DESCRIPTION "Self-hosted game stream host for Moonlight"
        HOMEPAGE_URL "https://github.com/papi-ux/polaris")
```

- [ ] **Step 2: Update README "What's New"**

Replace `## What's New in v1.0.12` with:

```markdown
## What's New in v1.0.14

Polaris `v1.0.14` focuses on Steam launch reliability, encoder/runtime polish, and safer Linux capture setup.

- Steam library launches are more reliable, including direct Steam launch handling and non-default Steam library discovery.
- NVIDIA hosts get NVENC split-frame encoding controls and clearer configuration docs for the prepared FFmpeg path.
- Auto Quality and Adaptive Bitrate respect paired-client bitrate more consistently and avoid unsafe recovery/clamp edge cases.
- AMD telemetry, Linux session fallback behavior, and runtime diagnostics are clearer across the dashboard, logs, and support data.
- Display selection now handles empty display lists safely instead of clamping against an invalid range during capture setup.
- Maintainers get a safer `scripts/dev-clean.sh` workflow for local build and runtime artifact cleanup.

See the [changelog](docs/changelog.md) for the full release history.
```

- [ ] **Step 3: Add changelog entry**

Insert this section after `## Unreleased` in `docs/changelog.md`:

```markdown
## v1.0.14

Patch release focused on Steam launch reliability, encoder/runtime polish, and safer Linux capture setup.

- Improved Steam library launch behavior, including direct Steam launch mode and non-default Steam library discovery
- Added NVIDIA NVENC split-frame encoding support, prepared FFmpeg wiring, configuration validation, and user-facing docs
- Improved Auto Quality and Adaptive Bitrate behavior so paired-client bitrate, recovery profiles, and clamp edge cases are handled more safely
- Added AMD GPU telemetry support and clearer dashboard handling for optional vendor-specific metrics
- Improved Linux unlock fallback, session cleanup, AMD headless DMA-BUF handling, and runtime diagnostics
- Added safe local development cleanup tooling with script coverage and building-guide documentation
- Hardened display selection so capture setup handles empty display lists without clamping against an invalid range
```

- [ ] **Step 4: Run docs reference checks**

Run:

```bash
rg -n "What's New in v1\\.0\\.12|Polaris `v1\\.0\\.12`|VERSION 1\\.0\\.12|## v1\\.0\\.14" README.md docs/changelog.md CMakeLists.txt
```

Expected: no stale README/CMake `1.0.12` release headline/version matches, and `docs/changelog.md` contains exactly one `## v1.0.14` entry.

### Task 3: Validation, Commit, and Publish

**Files:**
- Stage: `CMakeLists.txt`
- Stage: `README.md`
- Stage: `docs/changelog.md`
- Stage: `src/video.cpp`
- Stage: `src/video.h`
- Stage: `tests/unit/test_video.cpp`

- [ ] **Step 1: Run focused release-scope tests**

Run:

```bash
ctest --test-dir build --output-on-failure -R 'test_video|test_adaptive_bitrate|test_process_migration|test_stream'
```

Expected: all selected tests pass. If a test binary is missing, build it first with:

```bash
cmake --build build --target test_video test_adaptive_bitrate test_process_migration test_stream -j"$(nproc)"
```

- [ ] **Step 2: Inspect the final diff**

Run:

```bash
git diff -- CMakeLists.txt README.md docs/changelog.md src/video.cpp src/video.h tests/unit/test_video.cpp
git status -sb
```

Expected: only the release docs/version files and approved display-selection files are modified.

- [ ] **Step 3: Commit release prep**

Run:

```bash
git add CMakeLists.txt README.md docs/changelog.md src/video.cpp src/video.h tests/unit/test_video.cpp
git commit -m "release: prepare Polaris 1.0.14"
```

- [ ] **Step 4: Push branch**

Run:

```bash
git push -u polaris polaris/1-0-14-release
```

- [ ] **Step 5: Create and push the release tag after validation**

Run:

```bash
git tag -a v1.0.14 -m "Polaris v1.0.14"
git push polaris v1.0.14
```

Expected: GitHub Actions starts the release asset workflow for `v1.0.14`.

- [ ] **Step 6: Create or update GitHub release notes**

Use this public release body:

```markdown
# Polaris v1.0.14

Polaris v1.0.14 improves Steam launch reliability, Linux runtime diagnostics, and safety around capture setup edge cases.

## Highlights

- **Better Steam launches**: direct Steam launch mode and non-default Steam library discovery make Polaris library launches more reliable.
- **NVENC split-frame controls**: NVIDIA hosts get split-frame encoding support, prepared FFmpeg wiring, validation, and clearer configuration docs.
- **Safer Auto Quality behavior**: Adaptive Bitrate and optimizer recovery now respect paired-client bitrate and avoid unsafe clamp edge cases.
- **Clearer Linux runtime state**: AMD telemetry, optional vendor metrics, unlock fallback, session cleanup, and headless DMA-BUF diagnostics are easier to understand.
- **Safer display selection**: capture setup now handles empty display lists without clamping against an invalid display range.
- **Maintainer cleanup tooling**: `scripts/dev-clean.sh` provides a safer path for local build and runtime artifact cleanup.

## Release assets

- Arch Linux package
- Fedora 42, Fedora 43, and Fedora 44 RPMs
- Ubuntu 24.04 DEB tester package
```
