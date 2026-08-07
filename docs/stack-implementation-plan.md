# Audio, Gamescope Patches, and Session Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Align Polaris Linux stream audio routing, `gamescope-polaris` patches, and gamescope session lifecycle into three independent, mergeable PRs.

**Architecture:**  
- **PR1 (audio):** Replace EasyEffects special-cases + re-pin thrash with a host-owned stream capture sink claimed as the WirePlumber default for the session (host-owned stream sink claim).  
- **PR2 (patches):** Replace the fragmented HDR companion set with a focused three-patch surface (10-bit PQ formats + optional cursor composite + version stamp), keeping only Polaris-specific needs that still apply.  
- **PR3 (session):** Make gamescope_stream lifecycle host-owned and reuse-friendly (same-mode reuse, debounced teardown, degrade paths)—the largest change.

**Tech Stack:** C++ (PulseAudio API + optional PipeWire), shell session scripts, Nix gamescope packaging, gtest, shell tests.

**Reference trees (read-only, local):**

**Suggested merge order:** PR2 → PR1 → PR3 (patches first unblocks HDR capture quality; audio is independent user value; session is largest and can land last). PRs must not block each other for review: each ships working software alone.

---

## File map (who owns what)

| Area | Primary files |
|------|----------------|
| Audio policy (cross-platform decision) | `src/audio.cpp`, `src/audio.h` |
| Linux PA/PW capture + routing | `src/platform/linux/audio.cpp` |
| Audio control interface | `src/platform/common.h` (`audio_control_t`) |
| Audio unit tests | `tests/unit/test_audio.cpp` |
| Gamescope package | `nix/packages/gamescope-polaris/default.nix` |
| Gamescope patches | `nix/patches/gamescope/*`, `nix/patches/gamescope/README.md` |
| Nested gamescope ownership | `src/platform/linux/gamescope_process.{h,cpp}` |
| Stream runtime / portal stack | `src/platform/linux/stream_runtime_gamescope.cpp` |
| Session shell | `nix/modules/polaris-gamescope-session.sh` |
| Runtime helpers | `nix/modules/polaris-gamescope-runtime-lib.sh` |
| Session seed conf | `nix/modules/session-lib.nix` |
| Shell tests | `tests/unit/platform/test_gamescope_*.sh` |

---

## PR overview

| PR | Branch (suggested) | Scope | Out of scope |
|----|--------------------|--------|--------------|
| **PR1** | `feat/linux-stream-sink-claim` | Default-sink claim, drop re-pin/EE capture hacks | Session lifecycle, gamescope patches |
| **PR2** | `feat/gamescope-polaris-pf-align` | Patch set + version stamp + package wiring | Audio, session scripts |
| **PR3** | `feat/gamescope-session-lifecycle` | Reuse, restore, wait, ownership harden | Full rewrite to “attach gamescope PW node only” (phase 2 note only) |

---

# PR1 — Linux stream audio: claim default, stop fighting EasyEffects

### Problem

Polaris today:

1. Prefers capturing EasyEffects when it is the host default (FMOD `target.object` “cannot be overridden”).  
2. Otherwise isolates onto `module-null-sink` (`sink-sunshine-*`) **without** changing default, then **re-pins** session processes every 3s.  
3. Session shell sets soft `PULSE_SINK` / `PIPEWIRE_NODE` and historically tried pin loops that thrash WirePlumber.


1. Registers a host-owned **PipeWire `Audio/Sink` stream node** (or equivalent virtual sink).  
2. Claims `default.configured.audio.sink` metadata for the session (refcounted, restore on idle).  
3. WirePlumber’s `linking.follow-default-target` moves apps—no per-process pin loop.

### Target behavior

| Session state | Routing | Capture |
|---------------|---------|---------|
| Stream active | Configured default → Polaris stream sink (`polaris-speaker-*` or existing null sink name) | Monitor / stream of that sink |
| Stream ends | Restore previous configured default (or delete key if none / stale polaris name) | Stop claim |
| Concurrent sessions | Refcount; latest claim wins; last release restores | Same as today max_sessions |
| Explicit `audio_sink=` in conf | Still honored as the claim target (if present) | Capture that sink |
| `host_audio` on | Still claim stream sink for capture; do **not** force host speakers silent unless product already did | Capture stream sink; document host may stay on previous default if claim fails |

**Escape hatch:** `config::audio` or env `POLARIS_STREAM_SINK=0` keeps legacy “capture host default monitor, no claim” for debugging.

### Non-goals (PR1)

- Killing/pausing EasyEffects.  
- Shell-side pin loops.  
- Replacing the entire Pulse stack with pure PipeWire capturer (optional later; claim can target existing null sinks first).

---

### Task 1.1: Document the contract + invert failing tests

**Files:**
- Modify: `tests/unit/test_audio.cpp`
- Modify: `docs/configuration.md` (short Linux audio section) or `docs/troubleshooting.md` only if already covers sinks

- [ ] **Step 1: Rewrite sink-selection tests for claim model**

Delete or flip `CapturesEasyEffectsInsteadOfVirtualIsolation`—EE must **not** be preferred over the virtual stream sink.

```cpp
TEST(AudioSinkSelectionTest, PrefersVirtualStreamSinkOverEasyEffectsDefault) {
  audio_ctx_t ctx = make_ctx_with_nulls(/*host=*/"easyeffects_sink");
  // host_audio=false: session wants isolation via stream sink claim, not capture EE.
  EXPECT_EQ(audio::select_sink_name(ctx, 6, false), ctx.sink.null->surround51);
  EXPECT_FALSE(audio::should_route_session_sink_without_default(ctx, ctx.sink.null->surround51, false));
  // New: claim path, not re-pin path.
  EXPECT_TRUE(audio::should_claim_default_sink(ctx, ctx.sink.null->surround51, false));
}

TEST(AudioSinkSelectionTest, DoesNotEnableRepinWhenClaimingDefault) {
  audio_ctx_t ctx = make_ctx_with_nulls(/*host=*/"alsa_output.pci-0000.analog-stereo");
  auto sink = audio::select_sink_name(ctx, 2, false);
  EXPECT_TRUE(audio::sink_is_virtual(ctx, sink));
  EXPECT_FALSE(audio::should_route_session_sink_without_default(ctx, sink, false));
  EXPECT_TRUE(audio::should_claim_default_sink(ctx, sink, false));
}
```

Keep: explicit `audio_sink` config still wins; host_audio=true still can use host when no isolation needed (define expected policy in test names).

- [ ] **Step 2: Run RED**

```bash
# from build dir used by this repo (adjust if needed)
cmake --build build -j"$(nproc)" --target test_audio 2>/dev/null || true
ctest --test-dir build -R AudioSinkSelection --output-on-failure
```

Expected: FAIL — `should_claim_default_sink` missing / EE still preferred.

---

### Task 1.2: Policy API in `src/audio.{h,cpp}`

**Files:**
- Modify: `src/audio.h`
- Modify: `src/audio.cpp` (`select_sink_name`, `should_route_session_sink_without_default`, capture loop)

- [ ] **Step 1: Add claim decision helpers**

```cpp
// audio.h (near other selection helpers if exported for tests)
bool should_claim_default_sink(const audio_ctx_t &ctx, const std::string &sink, bool host_audio);
bool stream_sink_claim_enabled(); // env POLARIS_STREAM_SINK=0 disables
```

Policy:

1. If `!stream_sink_claim_enabled()` → claim false, keep legacy re-pin only if still needed (prefer off).  
2. If sink empty → claim false.  
3. If capturing a processing sink (should become rare) → claim false (never claim *as* EasyEffects).  
4. If selected sink is virtual/null stream sink and isolation wanted → claim true.  
5. Explicit configured sink → claim true when different from host (so apps follow).

- [ ] **Step 2: Change `select_sink_name`**

Remove priority “2. Host processing sink when host_audio off”. New order:

1. Explicit `config::audio.sink`  
2. Virtual sink by channel count when `ctx.sink.null` and (!host_audio || host unusable)  
3. Host default if usable  
4. Empty / warn

- [ ] **Step 3: Capture loop in `captureThread` / equivalent**

Replace re-pin block:

```cpp
// OLD: route_without_default + route_process_audio_to_sink every 3s
// NEW:
const bool claim = should_claim_default_sink(*ref.get(), sink, host_audio);
if (claim && !ref->sink_flag->exchange(true, std::memory_order_acquire)) {
  if (control->claim_default_sink(sink) != 0) {
    BOOST_LOG(warning) << "Could not claim default sink; capture continues, apps may stay on host output";
  } else {
    ref->restore_sink = true; // release on stop_audio_control
  }
}
// no route_process_audio_to_sink loop
```

On `stop_audio_control`, if `restore_sink`, call `control->release_default_sink()` (not only `set_sink(host)`).

- [ ] **Step 4: Unit tests GREEN** for pure policy.

---

### Task 1.3: Linux implementation — WirePlumber metadata claim

**Files:**
- Modify: `src/platform/common.h` (`audio_control_t`)
- Modify: `src/platform/linux/audio.cpp`
- Optional create: `src/platform/linux/stream_sink_claim.{h,cpp}` if `audio.cpp` is already too large (~1.5k lines)—prefer extract if claim needs PW mainloop code

- [ ] **Step 1: Extend interface**

```cpp
// common.h — default no-ops on non-Linux
virtual int claim_default_sink(const std::string &sink_name) { return 0; }
virtual int release_default_sink() { return 0; }
// Deprecate use of route_process_audio_to_sink for session isolation;
// leave stub for one release or delete if nothing else calls it.
```

- [ ] **Step 2: Implement claim (default-sink claim path)**

Mirror `stream_sink.rs` semantics:

- Metadata object name: `default`  
- Key: `default.configured.audio.sink`  
- Value JSON: `{"name":"<sink_node_name>"}`  
- Stale restore: if previous value contains `polaris-speaker` / `sink-sunshine` prefix left by a crash, restore by **deleting** the key (WP elects automatically)—never restore a ghost.  
- Refcount static ledger (max_sessions ≥ 2).  
- Prefer **PipeWire** metadata API when `POLARIS_BUILD_PIPEWIRE`; fallback: `wpctl set-default <id>` via subprocess is acceptable only if node id resolution is reliable—prefer native PW.  
- Pulse fallback: `pa_context_set_default_sink` is **not** the same as configured metadata under WirePlumber; document that PW path is required for claim reliability.

Name prefix constants:

```cpp
constexpr std::string_view kStreamSinkPrefix = "sink-sunshine"; // existing null sinks
// or migrate new names to polaris-speaker-* in a follow-up; do not rename mid-stream without migration
```

- [ ] **Step 3: Keep null-sink creation**

Continue creating stereo/5.1/7.1 null sinks at control start (existing `module-null-sink`). Claim points default at the selected one. (True PW `Audio/Sink` stream node is a stretch goal; not required for PR1 if claim+null sink works on lea.)

- [ ] **Step 4: Strip re-pin path**

Remove or no-op `route_process_audio_to_sink` body used for EE fight; remove session-env heuristics that exist only for re-pin (`POLARIS_SESSION_AUDIO_SINK` process tree walk) if unused after claim.

---

### Task 1.4: Session shell audio env (minimal)

**Files:**
- Modify: `nix/modules/polaris-gamescope-session.sh` (audio block only)

- [ ] **Step 1: Soft env only**

Keep optional `PULSE_SINK` / `PIPEWIRE_NODE` = virtual sink as a **hint** for children born before claim applies.  
Remove any pin-loop spawn, EasyEffects stop/start, and comments promising re-pin.

- [ ] **Step 2: Do not change port numbers, flake worktree hacks, or session wait logic** (those are PR3 / unrelated).

---

### Task 1.5: Manual verification + PR1 ship checklist

- [ ] Stream with EE running as default: desktop audio after stream ends returns to EE; during stream games/menu audio appears on client.  
- [ ] Stream without EE: claim works; host default restored.  
- [ ] Two quick connect/disconnect cycles: no stuck `sink-sunshine-*` as system default.  
- [ ] `POLARIS_STREAM_SINK=0`: legacy behavior (no claim).  
- [ ] Unit: `ctest -R Audio`  
- [ ] Commit + open PR titled e.g. `fix(linux/audio): claim stream sink as default (drop EE re-pin)`

**PR1 acceptance:** No periodic re-pin; no “capture EasyEffects because FMOD”; claim/release refcounted; tests document policy.

---

# PR2 — Align `gamescope-polaris` HDR patch stack

### Problem


### Target patch surface

| Keep / adopt | Source | Notes |
|--------------|--------|--------|
| **#2217 discrete GPU** | Current `06-…` | Keep if still needed on hybrid lea; mark DROP when #2217 merges |
| **dmabuf multi-type** (`03`) | Current | Keep only if portal path still needs MemPtr+DmaBuf advertise; else drop after capture smoke |
| **Headless SetHDR** (`02`) | Current | Keep if nested/headless SDR/HDR still wrong without it after 0001 lands |

**Drop when superseded by 0001:** fragmented companions `04` + `07` if 0001 already switches LUTs/EOTF on 10-bit negotiation; keep a thin headless-only patch if 0001 doesn’t touch HeadlessBackend.

### Pin strategy

- Track the same Valve master tip as `gamescope-polaris` currently does (or bump together with overlay).  
- Apply order: packaging filters (shaders-path, reaper) → HDR formats → cursor → stamp → optional polaris-only (2217, headless, dmabuf).  
- `installCheck` or `postInstall` grep: `$out/bin/gamescope --version` must contain `+polhdr`.


---

### Task 2.1: Import and rename patches

**Files:**
- Create/replace under `nix/patches/gamescope/`:
  - `10-pipewire-offer-10-bit-BT2020-PQ.patch` (10-bit PQ format negotiation)
  - `11-pipewire-composite-cursor.patch` (optional composite cursor)
  - `12-polaris-stamp-version-polhdrN.patch` (`+polhdr2` version stamp)
- Archive or delete superseded: `01`, `04`, `07` once 10 proves equivalent
- Keep (if still required): `02`, `03`, `06` with updated README

- [ ] **Step 1: Copy patches, change stamp string**

In stamp patch:

```diff
-version_tag = vcs_tag + '+pfhdr2' + ...
+version_tag = vcs_tag + '+polhdr2' + ...
```

Document levels in patch header:

```
+polhdr1  10-bit BT.2020/PQ capture formats
+polhdr2  …and --pipewire-composite-cursor
```

- [ ] **Step 2: `git apply --check` against current `gamescopeRev`**

```bash
rev=$(rg -o 'gamescopeRev = "[^"]+"' nix/packages/gamescope-polaris/default.nix)
# fetch src once, apply each patch with --check
```

Fix hunks until clean on the pinned rev.

---

### Task 2.2: Wire package + README

**Files:**
- Modify: `nix/packages/gamescope-polaris/default.nix`
- Rewrite: `nix/patches/gamescope/README.md`

- [ ] **Step 1: patches list**

```nix
++ [
  ../../patches/gamescope/10-pipewire-offer-10-bit-BT2020-PQ.patch
  ../../patches/gamescope/11-pipewire-composite-cursor.patch
  ../../patches/gamescope/12-polaris-stamp-version-polhdrN.patch
  # optional polaris-only, drop checklist in README:
  # ../../patches/gamescope/02-headless-hdr-colorimetry.patch
  # ../../patches/gamescope/03-pipewire-prefer-dmabuf.patch
  # ../../patches/gamescope/06-prefer-discrete-gpu-2217.patch
];
```

- [ ] **Step 2: postInstall / installCheck**

```bash
"$out/bin/gamescope" --version 2>&1 | grep -q '+polhdr' \
  || { echo "gamescope-polaris: +polhdr marker missing"; exit 1; }
```

(If binary is wrapProgram’d, check the real unwrapped binary path as nixpkgs does.)

- [ ] **Step 3: README table** — purpose, upstream PR links (#2270, cursor, #2217), DROP conditions.

---

### Task 2.3: Host capability probe (minimal)

**Files:**
- Modify: `src/platform/linux/stream_runtime_gamescope.cpp` or gamescope spawn path only if version is already parsed
- Or: document-only in PR2 if no spawn gate exists yet

- [ ] **Step 1:** If Polaris already logs gamescope version, parse `+polhdr` and log capability (HDR capture formats / cursor). Do **not** hard-fail missing stamp in PR2 (break distro packages); warn only.

---

### Task 2.4: Build + smoke

```bash
nix build .#gamescope-polaris -L
# or package attribute used by flake
./result/bin/gamescope --version   # expect +polhdr2
```

Manual: one HDR nested/stream smoke if hardware available.

**PR2 acceptance:** Package builds; version stamp present; README drop list accurate; HDR path no worse than before (ideally cleaner 10-bit negotiation).

---

# PR3 — Gamescope session lifecycle (largest)

### Problem

Polaris nested path is shell-heavy and fail-closed:

- Marker + socket + Xwayland generation checks  
- `wait` can race Steam/portal and tear down early  
- Recovery often forces full stop/reclaim  
- No first-class “same mode reuse” or debounced restore  



### Target behavior (phase 1)

1. **Start once per mode+HDR:** reconnect / second client at same mode reuses nested compositor when marker validates.  
2. **Wait holds stream:** do not exit wait on early Steam probe failure; release on SIGTERM, game exit (appid debounce), or nested gamescope death.  
3. **Stop is debounced / ordered:** stop gamescope → clear markers → never leave half-state that blocks next start; if stop fails, fail-open clean slate (documented).  
4. **Ownership checks match real gamescope:** setsid/Xwayland reparent (session/pgid), wrapProgram `gamescope` vs `.gamescope-wrapped` executable match, ambiguous unix socket inode prefer live listener.  
5. **Degrade path:** if nested spawn fails portal readiness, log actionable error; optional attach-to-existing gamescope-0 only if already owned (no silent capture of foreign session without config).  
6. **Audio in session script:** only soft env; no pin loops (depends on PR1 ideally, but PR3 must not reintroduce them).

### Non-goals (PR3 phase 1)

- Full `gamescope-session-plus` takeover / DM stop / linger (document as phase 2).  
- Replacing portal capture with pure gamescope PW node capture (phase 2; needs encoder path work).  

---

### Task 3.1: Runtime lib ownership fixes (C++ + shell parity)

**Files:**
- Modify: `src/platform/linux/gamescope_process.cpp` (+ header if needed)
- Modify: `nix/modules/polaris-gamescope-runtime-lib.sh`
- Modify: `tests/unit/platform/test_gamescope_process.cpp`
- Modify: `tests/unit/platform/test_gamescope_runtime_shell.sh`

- [ ] **Step 1: Xwayland relatedness via session/pgid**

When gamescope is session leader (`sid == pgid == pid`), treat processes with same sid/pgid as related even if PPID is 1 (double-fork). Mirror in shell `polaris_pid_related_to_root`.

- [ ] **Step 2: Executable match for wrapProgram**

`gamescope` and `.gamescope-wrapped` in same `…/bin` directory are the same generation.

- [ ] **Step 3: Socket inode disambiguation**

Prefer unique pathname; if duplicate rows, single listening state `01` wins; else fail closed.

- [ ] **Step 4: Unit + shell tests GREEN**

```bash
ctest --test-dir build -R gamescope --output-on-failure
bash tests/unit/platform/test_gamescope_runtime_shell.sh
```

---

### Task 3.2: Session `wait` and start recovery

**Files:**
- Modify: `nix/modules/polaris-gamescope-session.sh`
- Modify: `tests/unit/platform/test_gamescope_session_stop_shell.sh` (extend)

- [ ] **Step 1: Rewrite `wait` subcommand**

Contract:

```
trap TERM/INT → exit 0
loop forever:
  if appid set and game was seen and gone ≥ ~15s → kill session steam; exit 0
  if marker exists and gamescope socket gone / marker invalid after once-up → exit 0
  sleep 0.5
never: exit 1 on steam probe flapping in first N seconds
```

- [ ] **Step 2: Start recovery**

If prior session state exists, run `stop`; if stop fails, force clean slate (remove markers, reclaim orphan sockets, best-effort pkill gamescope for this user)—log loudly. Do not leave “complete prior recovery” permanent wedge.

- [ ] **Step 3: Shell tests** for stop cleanup and wait signal handling (mockable with temp POLARIS_PROC_ROOT if fixtures exist).

---

### Task 3.3: Same-mode reuse in stream runtime

**Files:**
- Modify: `src/platform/linux/stream_runtime_gamescope.cpp`
- Tests: add/extend unit tests under `tests/unit/platform/` for pure decision helpers if extracted

- [ ] **Step 1: Extract pure decision helper** (testable without spawning)

```cpp
struct GamescopeReuseKey {
  int width, height, fps_milli;
  bool hdr;
};
bool can_reuse_nested_session(
  const GamescopeReuseKey &want,
  const GamescopeReuseKey &have,
  bool marker_valid,
  bool portal_ready);
```

- [ ] **Step 2: On session start**

If `can_reuse…` true, skip stop+spawn; re-ensure portal stack only.  
If mode/HDR changed, ordered stop then spawn (existing HDR force file logic).

- [ ] **Step 3: Debounce teardown on client disconnect**

Optional 3–5s delay before stopping nested session if product wants quick Moonlight reconnect (support a short reconnect debounce). If too invasive, document as follow-up and only do same-mode reuse within process lifetime.

---

### Task 3.4: Session seed conf hygiene

**Files:**
- Modify: `nix/modules/session-lib.nix` only if defaults are wrong for gamescope_stream

- [ ] Prefer product defaults that match claimed stream sink names after PR1 (`audio_sink` optional; empty means auto virtual).  
- Do not hardcode machine-specific `adapter_name` or local flake paths.

---

### Task 3.5: Integration smoke + PR3 ship

Manual matrix:

| Case | Expect |
|------|--------|
| Cold start gamescope_stream | Nested up, portal attach, stream holds |
| Disconnect/reconnect same mode | Reuse or <2s recovery, no permanent wedge |
| Game exit with appid | Stream ends after debounce |
| Big Picture / no appid | Stream holds until stop/SIGTERM |
| Crash mid-session + restart polaris | Orphan reclaim; next start works |
| HDR on/off flip | Restart nested once; correct force file |

**PR3 acceptance:** No “Response code 2 / wait exited early” race class; ownership checks pass on Nix wrapProgram gamescope; stop always leaves startable state.

---

## Cross-PR dependencies

```text
PR2 (patches) ─────────────────────────────┐
                                           ├──► better HDR on PR3 path
PR1 (audio claim) ─── session shell soft env ┤
                                           │
PR3 (session) ◄──── ideally after PR1 so wait/start never reintroduces pin loops
```

- PR1 + PR3 both touch `polaris-gamescope-session.sh` → rebase carefully; **PR1 only audio block**, **PR3 wait/start/stop/runtime**.  
- PR2 is independent of the other two.

---

## Risks

| Risk | Mitigation |
|------|------------|
| WP metadata claim fails on pure Pulse | Log warn; capture still works; document PW requirement |
| Claim steals default from desktop apps user wanted local | Product choice: stream isolation *is* default claim; `host_audio` / escape hatch |
| Patch 0001 conflicts with remaining headless patch | Apply order + drop redundant companions; `git apply --check` in CI note |
| Session fail-open clean slate kills foreign gamescope | Only pkill when marker recovery failed and sockets are polaris runtime paths; never kill without user-scoped care |
| Scope creep into session-plus/DM | Hard non-goal in PR3 |

---

## Done definition (all three)

1. **Audio:** Session claim/release; no EE capture preference; no re-pin loop; unit tests green.  
2. **Patches:** `gamescope-polaris` builds with `+polhdr*`; README accurate.  
3. **Session:** Reuse + solid wait + ownership parity; shell + C++ tests green; smoke matrix checked on lea.

---

## Execution handoff


**Recommended execution:**

1. **Subagent-driven** — one PR (or one task group) per subagent with review between PRs.  
2. **Inline** — implement PR2 first (smallest), then PR1, then PR3.

**Which approach?** If starting now: implement **PR2 → PR1 → PR3** in that order unless audio pain is blocking daily use (then PR1 first).
