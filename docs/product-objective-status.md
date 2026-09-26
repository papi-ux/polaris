# Polaris product objective status

Source audit: 2026-09-26, staging
`319339a1dc083c0031ef47858d443afb9135c7b8`. Imported issues retain earlier reports;
read the current source and linked review candidate before starting another
implementation. An open PR is not a merged feature, release or physical result.

The immediate product priority is dependable Polaris and Nova Linux operation:
installation, ownership/session correctness, playable media, recovery and input,
then the remaining product and installed-device acceptance. Nova Linux covers
laptops, desktops and handhelds; Steam Deck is a device profile. Nordstern owns
measurement and acceptance contracts, not a second implementation of host APIs.

## Current review queue

All numbers below are Polaris Gitea issues/PRs. Each row names the bounded slice
and what it does not close. Reuse these candidates rather than duplicating them.

| Objective | Existing candidate | Remaining boundary |
|---|---|---|
| #90/#109 inventory failure after a worker disappears | #121 retries the vanished-worker inventory case | Preserve other failures; physical runtime acceptance remains separate |
| #111 stale per-client ping timeout | #122 initializes the deadline before control publication | A proved publication race is fixed; the reported physical reconnect scenario still needs acceptance |
| #113 artwork-removal and host-admin races | #124 prevents publication after removal; #126 reserves admission before Host Setup | Review both distinct corrections; neither is a replacement for all concurrent-authorization tests |
| #64 AppImage removal | #127 revokes capabilities applied during installation | Real installation/removal acceptance; other packaging policies are separate |
| #25/#114 Heroic discovery and metadata | #128 adds Amazon/sideload discovery; #130 serves platform/runtime metadata | Earlier imported claims that Heroic has no identity/artwork are historical; preserve existing support. Review together, then validate actual launcher/runtime paths |
| #5 CGNAT policy | #131 applies WAN policy to shared IPv4 space | Explicit compatibility review for LAN-only access/encryption; it does not change explicit trusted-pair decisions |
| #24 capture-size visibility | #133 reports source size per stream | Observation only; native-size capture/performance behavior and device acceptance remain open |
| #102 optional import covers | #135 includes #132/#134 lifetime prerequisites | Existing candidate, not a new assignment; preserve manual artwork and verify live provider behavior separately |
| #95 Spaces Doctor | #136 adds read-only setup/runtime findings | Does not supply Space-scoped Auto Fix, rollback or full #58 acceptance |
| #96 Mission Control for Spaces | #137 lists Space sessions and observations | Activity observations do not prove video/input, isolation or sustained streaming |
| #88 Spaces modifiers and UTF-8 | #139 includes #138 keyboard lifetime and implements synthetic modifiers/repeats | UTF-8 remains unimplemented; physical launcher/game input remains unaccepted |
| #82 Vulkan LTO | #140 isolates volk and generated shader symbols | Full strict-LTO link/fixtures exist; physical encode and package installation remain separate |
| #85 Heroic/Lutris guides | #141 documents existing setup/runtime update routes | Documentation does not accept NVIDIA/AMD/Intel, distribution, audio or controller combinations |
| #112 HTTP responsiveness | #142 moves bounded cover search/choices off the HTTP thread | Cover selection and other slow handlers remain; do not describe this as general concurrent HTTP completion |
| #68 debug packages | #143 retains matching Arch/SteamOS debug assets | Actual package publication/installation and full SteamOS acceptance remain open |

PR #135 already contains #132/#134. PR #139 already contains #138. These
prerequisites remain review history, not additional feature deliveries. Current
staging already includes installation docs #123, sanitizer ccache #125 and the
Virtual Display artwork #129; do not propose those changes again.

The current combined candidate applies all twenty implementation PRs to the
audited staging without conflicts. Retained integration proof covers the strict
Release LTO host link, 1,317 native tests and 17 input/Vulkan fixtures. Application
web sources preserve the earlier 1,215-test/lint/build proof; current-base package
checks pass 12 real-ELF tests and 41 packaging contracts. These are isolated
combined-source results, not protected-branch CI or physical acceptance. Each PR
retains its exact-head checks and narrower limits.

## Work that remains distinct

- Spaces #53-#59 remain installation, GPU/distribution, sustained audio/pacing,
  reversible scoped recovery and UID/GID acceptance. Host Doctor/Mission Control
  additions do not close those gates. Normal single-device behavior and
  authorization/isolation requirements remain in force.
- Server-container #98, sidecar #99 and two-container spike #100 remain design and
  deployment work. The existing default Docker worker backend runs Spaces; it
  does not supply a complete Polaris controller/server image. Reconcile these as
  one deployment design before creating overlapping implementations. They follow
  product correctness and Nova Linux readiness in the present work order.
- Mixed PyroWave/H.264 capture #78, GPU/HDR paths #77/#79/#80, queue integration
  #81 and physical codec/display results require their own current-source and
  hardware evidence. An upstream bump, build or historical headless VERIFIED row
  does not prove those outcomes.
- Installation/upgrade/device rows, full Watch Stream and launcher/runtime QA
  remain open. Keep failed results and exact installed artifact identities.
Nova owns Linux pairing, discovery, native media/input, windows, rates and UI.
Those implementations already exist; host changes should supply a demonstrated
missing contract or correctness fix. Nightly's September 17 source observations
must not override the later Linux scope or reopen completed product work.

Before implementing an imported issue, identify its present source, candidate
PR, remaining gap and acceptance owner. Narrow fixes do not close broad issues
unless every exit criterion is evidenced. Keep protected merges, publication and
physical acceptance separate from this planning reconciliation.
