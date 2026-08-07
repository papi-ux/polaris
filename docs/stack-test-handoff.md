# Stack test handoff — audio / gamescope / session

**Date:** 2026-08-07  
**Status:** Ready for human rebuild + smoke testing on lea

## Branches (stack)

```text
master
  └── feat/gamescope-polaris-pf-align     PR2 patches (+polhdr)
        └── feat/linux-stream-sink-claim PR1 audio claim
              └── feat/gamescope-session-lifecycle  PR3 session  ← TEST THIS
```

| Branch | Commit (tip of layer) | Scope |
|--------|------------------------|--------|
| `feat/gamescope-polaris-pf-align` | `8d48956` | gamescope-polaris patches |
| `feat/linux-stream-sink-claim` | `a83c4da` | default-sink claim, no EE re-pin |
| **`feat/gamescope-session-lifecycle`** | 60c966a | wait/ownership/recovery |

**Local tip checkout (all three layers):**

```bash
cd /home/luxus/projects/polaris
git checkout feat/gamescope-session-lifecycle
git log --oneline master..HEAD
```

## What changed (summary)

1. **PR2 — gamescope patches**  
   - `10`/`11`/`12` 10-bit PQ + optional cursor + `+polhdr2` stamp  
   - Archived old `01`/`04`/`07`; kept headless, dmabuf, #2217  
   - `installCheck` requires `+polhdr` in `--version`

2. **PR1 — audio**  
   - Prefer `sink-sunshine-*` over EasyEffects as capture target  
   - **Claim** that sink as Pulse default for the session (refcounted restore)  
   - No 3s re-pin loop when claiming; `POLARIS_STREAM_SINK=0` restores legacy re-pin  
   - Session shell soft `PULSE_SINK` only to virtual sink (never EE)

3. **PR3 — session**  
   - C++ Xwayland relatedness via session/pgid  
   - Runtime lib: wrapProgram exe match, socket listening disambiguation, tree holds inode  
   - `wait` holds until SIGTERM / game exit debounce / nested gone (no early Steam probe fail)  
   - Start recovery fail-open clean slate if stop fails

## Automated checks already run

```bash
bash tests/unit/platform/test_gamescope_runtime_shell.sh   # PASS
POLARIS_SOURCE_DIR=$PWD bash tests/unit/platform/test_gamescope_session_stop_shell.sh  # PASS after test update
```

C++ unit tests need a local rebuild (no pre-existing build dir in this worktree).

## Rebuild for testing

Pick your usual path, e.g.:

```bash
# NixOS module / flake path you already use for polaris-stream + gamescope-polaris
nix build .#gamescope-polaris -L   # verify +polhdr2 if attribute exists
# or: rebuild host config that pulls gamescope-polaris + polaris from this worktree
```

Confirm:

```bash
gamescope --version 2>&1 | grep polhdr   # expect +polhdr2 after package install
```

## Smoke matrix (you run)

| # | Case | Expect |
|---|------|--------|
| 1 | Stream with EasyEffects as host default | Client hears game/desktop stream audio; after stream, desktop default returns to EE (or WP auto-elect) |
| 2 | Stream without EE | Claim virtual sink; restore host default after stop |
| 3 | Connect → disconnect → reconnect same mode | Stream holds; no permanent “complete prior recovery” wedge |
| 4 | Launch game with appid | Wait ends ~15s after game process gone |
| 5 | Big Picture / no appid | Wait holds until Moonlight/Polaris SIGTERM |
| 6 | HDR session (if hardware) | Nested/idle HDR; no washed PQ/SDR hybrid if patch stack applied |
| 7 | `POLARIS_STREAM_SINK=0` | Legacy no-claim path (debug only) |

## Escape hatches

- `POLARIS_STREAM_SINK=0` — disable default claim  
- Do **not** kill EasyEffects; claim should move streams via WirePlumber follow-default  

## Known gaps (honest)

- Claim uses Pulse `set_default_sink` (WirePlumber-compatible); if claim fails, capture still runs with a warning  
- True PW `Audio/Sink` stream node (not null sink) not implemented — still `module-null-sink` + claim  
- Same-mode **process-lifetime** reuse already existed in `stream_runtime_gamescope` (no multi-minute debounced idle restore)  
- Stack not pushed / PRs not opened yet — local branches only  
- `nix build` gamescope-polaris not run in this session (heavy); installCheck will enforce stamp at build time  

## Workflow


## Open PRs later (optional)

```bash
# after push
gh pr create --base master --head feat/gamescope-polaris-pf-align ...
gh pr create --base feat/gamescope-polaris-pf-align --head feat/linux-stream-sink-claim ...
gh pr create --base feat/linux-stream-sink-claim --head feat/gamescope-session-lifecycle ...
# or: gh stack if init'd on these branches
```
