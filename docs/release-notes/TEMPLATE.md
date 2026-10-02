# Polaris vX.Y.Z

One sentence: what kind of release. Name a Nova version for what a feature needs from it, never as the release this one is matched with: Nova ships on its own schedule, and a stable tag is refused while its notes say matched with Nova.

**New**

- One line per user-visible addition. No internal identifiers, no field names, no code paths.

**Fixed**

- One line per fix, said the way the person who hit it would say it.

**Heads up**

- What is not proven yet, and anything that stays deliberately unchanged.
- While X.Y.Z is in beta, use the packages attached to the prerelease you are reading. The install commands below name the final release, which does not exist yet. (Beta notes only, and in these words. Take it out before the stable tag; see the last paragraph.)

<details>
<summary><b>Install</b> (Fedora 44, Arch / CachyOS, Ubuntu 24.04, SteamOS 3.8)</summary>

The four version-pinned install blocks, unchanged in shape from the previous release, followed by the Bazzite and SteamOS guide links.

</details>

**Assets:** the release files, backticked, on one line.

Rules: bullets stay under about twenty words; the changelog (`docs/changelog.md`) is where the detail lives; `scripts/check-public-docs.sh` and the release contract test pin a few user-visible phrases plus the install commands, so keep those when you edit; never an em dash.

A beta publishes the notes of the release it precedes, and the stable page is published from the stable tag's copy of this file as it stands. `scripts/check-stable-release-notes.py` refuses a stable tag whose notes still hold the beta line above or a rewording of it that the check knows (while the release is in beta or a prerelease, until it ships, packages attached to the prerelease, a final release that does not exist yet), or call the release matched with a Nova release. It reads wording, not meaning, so a beta line in other words can get past it: use the words above.
