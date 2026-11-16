# Official Epic Linux Preview

This integration is new in Polaris 1.5.0. It imports completed Windows-game installations from
the official Epic Linux Preview and launches them in **Mirror Desktop**. Epic manages its own
sign-in, downloads, updates, Proton runtime and cloud saves. With a native package, a game can also run in labwc
**Private Stream** when you choose it for that game, as described below; imported entries still
default to Mirror Desktop. Hands-on checks so far cover one game on one host in each mode: in
Private Stream, a Retroid Pocket 6 launched it with controller isolation on, its controller input
reached the game, and End closed the whole private session. Keyboard input, play past the game's
opening screens, saves and cloud sync, Disconnect and Resume in Private Stream, and Epic's AppImage
build in a private session haven't been checked.

## Install and open Epic

Use [Epic's official Preview download page](https://launcher.epicgames.com/).
[Epic documents](https://www.epicgames.com/help/c-32735058/c-Trending_0/what-is-the-epic-games-launcher-early-preview-a12104725)
an x86-64 AppImage and a Debian package. Both are the Preview launcher, not a stable release.
Choose the package suited to your distribution; Polaris does not install or convert Epic packages.

The [Epic Games Launcher package on Flathub](https://flathub.org/en/apps/com.epicgames.launcher)
is a community-maintained package of Epic's official Preview client. The package is not
published by Epic, and its listing does not establish Epic endorsement. Its
[pinned manifest](https://github.com/flathub/com.epicgames.launcher/blob/8f2584ce007ac9c7f09b7756c79f489380754418/com.epicgames.launcher.yml)
fetches Epic's own Debian package with a pinned SHA256 hash and adds UMU. This provenance
check is not a full binary or security audit. Updates come through Flatpak, with Epic's
self-updater disabled by the package.

Its Polaris route is limited to **Mirror Desktop**: discovery/import and URI dispatch are
implemented, but exact-source Linux checks and deployed Flatpak Mirror/URI forwarding,
streaming, End, Disconnect and Resume validation remain pending. Native/AppImage hands-on
results above do not qualify this route.

Polaris checks the default user or default system installation's exact x86-64 branch and
deployment against an already-running desktop instance. Multiple user/system deployments,
multiple active branches, missing or unreadable metadata, and a launcher still running an
older deployment are unavailable until resolved. Named system installations and custom
installation roots are not supported in this slice. Game files may use an unchanged,
canonical path under `~/.var/app/com.epicgames.launcher/` or `~/Games/`. A completed game
using an existing Documents portal export of exactly `~/Games/` is also supported by the
source implementation when its mapping can be independently verified. Other portal origins,
external folders and guessed path translations are unavailable. Polaris reads the app-private
`data/com.epicgames.eglv2/installations.jsonl`, without reading account data or migrating saves.

Discovery and import describe current completed installation metadata and a canonical host
directory. They do not establish that Epic can see that directory, that the game is running,
or that streaming is ready. Epic's folder picker may already provide access to `~/Games/`
through a Documents export; that route does not require a new broad filesystem override or
moving/redownloading the game. For direct folder access, the owner makes the chosen folder
accessible to Epic themselves. Let the installation finish in Epic: an incomplete or pending
update remains unavailable even when its download is progressing normally.
Polaris does not grant Flatpak permissions, export a portal folder, copy or move a game,
or choose a different installation when the selected path is inaccessible.

Before streaming, confirm that the completed game opens normally from Epic itself. Readable
Documents metadata and files do not prove that Proton can use the portal directory as its
working directory. On one recorded Epic 0.113.0 / GE-Proton 10-34 setup, two Alan Wake 2
launch captures failed at `protonfixes/util.py:909`, where `os.getcwd()` encountered a deleted
FUSE working directory. Re-entering the same directory repaired it only temporarily.
Direct access to the existing `~/Games/` folder, correcting the existing installation's path
to its real location, and a normal launcher restart let the owner open the game without
moving or redownloading it or migrating old saves. This observation does not identify the
underlying portal defect or establish a fix for every setup.

If this limitation affects you, check access to the specific existing game folder and its
configured installation path with the package or launcher maintainer; restart Epic normally
once the launcher and game are idle. Limit any access change to that chosen folder.
Polaris does not repair these settings or choose a fallback path. Opening a game inside
Epic does not validate Polaris Flatpak URI forwarding, streaming, End/Resume, game ownership
or release readiness; the Flatpak route still needs its own validation.

At launch, Polaris checks the selected folder and launch file through the independently
verified running desktop launcher's process root. For a direct path, the actual host and
sandbox device, inode and file identities must agree at the same absolute path; symlinks, redirected
ancestors, unreadable or missing views, and different same-spelling files are refused.
Those identities are retained across stream preparation and checked again immediately
before the URI. A closed or changed desktop instance cannot provide a mapping, and a
permission setting or host directory alone never establishes one. These are sampled
read-only prerequisites, with no game-control authority or guarantee of future launch success.

For the Documents route, Polaris reads only the current Documents service's version,
mount point, exact Games export ID/origin and Epic's existing read/write grant. It binds
those facts to the matching live Epic sandbox's app-domain `fuse.portal` mount and the
portal's read-only backing-path property for the selected folders and launch file. FUSE
inodes differ from native inodes; they are retained separately, never treated as native
equality. The grant, service, mount, instance and both file views are checked again before
dispatch. Missing, revoked, redirected or changed facts are unavailable, with no direct-path
or native-provider fallback. Nothing is exported or granted by Polaris, and mappings are
not saved in the imported app entry. This route still needs deployed-package validation.

Keep that matching Epic Flatpak open on the desktop before launching. Flatpak **Private
Stream**, game adoption and close-game permission are unavailable. An attempted private
launch is refused before startup and never forwarded to the desktop. **End** still ends
the stream, leaving the desktop game and launcher running; **Disconnect** and **Resume**
also leave them running. A successful URI command does not prove game ownership or streaming
acceptance. The setup checklist explains this package limit and cannot turn a metadata-only
Flatpak scan into a native setup receipt or Private Stream readiness.

Open Epic on the host desktop, sign in yourself, and let the selected game's installation or
update finish. Keep the launcher open before launching through Polaris. Polaris does not read
your Epic account store, sign you in, install a game, select Proton or change your saves.

Mirror Desktop uses the already-running **desktop** launcher. For native packages, Private Stream starts its
own launcher only after the desktop launcher is completely quit. Game Mode, Host Virtual Display,
Gamescope/private Gamescope and Spaces remain refused instead of receiving a forwarded desktop launch.
Native Linux Epic games aren't supported yet.

## Add a game

In **Library**, scan for games and open **Epic (Preview)**. Select a completed installation and
import it. Polaris rebuilds its launch request from the host's current installed-state metadata
every time; a pending update or ambiguous installation is unavailable until resolved in Epic.

If the same exact title is already imported through Heroic and its catalog identity can be
verified, the scan shows one entry with an explicit **Use Epic Launcher** or **Use Heroic** choice.
Confirming a provider change keeps that entry's UUID, artwork, history and other settings.
Polaris never switches an existing Heroic entry automatically. A name alone is not sufficient
to merge titles, and uncertain catalog data does not authorize a provider change.

## Library setup checklist

In the Linux **Library → Import Games** panel, expand **Epic Private Stream setup**.
The entry is available even with no discovered Epic titles. Opening it reads setup status;
it does not scan, launch, switch providers or change staged imports. **Rescan Sources** remains
an explicit action. Choose an official Epic game in the checklist's separate picker to check
that installation without changing which games are staged for import.

The ordered rows distinguish three facts:

1. **Epic observed open on this PC** is a desktop observation. After a successful scan it
   can remain historical; it does not claim that Epic is still open.
2. **Scanned while Epic was open** requires the host's in-memory receipt for a successful
   official metadata scan with matching desktop-launcher observations at both ends. These
   samples do not prove continuous presence. Provider choice, Mirror Desktop discovery,
   imported titles and browser storage cannot create this receipt. A failed rescan can show
   a clearly marked **Previous successful scan (historical)**, never crediting that failure.
3. **Epic closed: check Private Stream setup** is a current sample, separate from that
   history. With no selected title it says **Epic is closed; choose a game to check its setup**.
   The current host reports prerequisite support as unknown, so a selected game is never
   labelled ready by this implementation. Its Windows setup-folder and runtime checks still
   run: an unreadable folder, a busy runtime and a sampled absent runtime remain distinct. The checklist cannot confirm sign-in, cloud saves,
   encoder startup or future launch success. **Polaris checks again when you launch.** A
   missing checklist receipt does not add an admission restriction to existing launch paths.

Complete, Needs action, Can't check and Checking are text, with a single polite live summary.
Native buttons and the game picker support keyboard, touch and client controller mapping;
Escape closes the checklist and returns focus to its entry. Controls stay present while a
check runs. There is no looping animation. The checklist uses the current theme and respects
reduced motion and forced colors; Aurora remains the default. Physical controller and TalkBack
behavior still require owner device checks.

While visible, one passive GET runs at a time. The next read starts five seconds after completion;
failures back off to at most thirty seconds. The transport deadline is eight seconds, and current
facts expire within ten seconds of request start (earlier for an already aged host sample).
Closing/hiding/unmounting, signing out, changing host, scanning, changing provider, or selecting
another title discards current facts and aborts the read. Returning requires a fresh sample.
Old requests, retired host epochs and older revisions cannot restore current facts. No scan,
launch or other mutation is automatically retried; browser storage holds no setup truth flags.

| Checklist cause | What to do |
| --- | --- |
| Epic has not been observed / launcher proof required | Open Epic on this PC, select **Rescan Sources**, then quit Epic completely. Repeat after Polaris restarts. |
| Scan required / no receipt | Scan explicitly with Epic open. Merely selecting a provider or importing a title does not complete the scan row. |
| Metadata unavailable / latest source scan failed | Finish installs or updates in Epic and check the source warnings, then rescan. Any prior successful scan is historical. |
| Launcher unobserved or changed during the scan | Open the same desktop Epic launcher and rescan after its update settles. No new receipt was created. |
| Setup changed / rescan superseded | Wait for the current source/provider change to finish and check again. Old replies are discarded. |
| Polaris restarted / setup epoch changed | Repeat the open → scan → quit setup on this PC. Receipts are memory-only. |
| Epic changed or updated / retained proof changed | Open the updated launcher on the desktop, rescan, then quit it. An old receipt cannot validate the new file. |
| Epic still open | Quit it yourself on the host desktop, then **Check again**. Checks never close a program. |
| Choose a game | Select an official Epic installation in the checklist's picker. This does not alter import staging. |
| Selected-game support unknown | Selected-title Windows setup-folder and runtime diagnostics still run. A sampled absent game/runtime is reported separately; this host cannot confirm all Private Stream prerequisites. Use Mirror Desktop or rely on the independent actual-launch checks; no ready claim is made. |
| Private Stream prerequisites unavailable | Use Mirror Desktop or review the host's explicit labwc Private Stream configuration. |
| Epic Flatpak: Mirror Desktop only | Keep the matching Flatpak open on the desktop and choose Mirror Desktop. Private Stream and game-control permission are unavailable; End ends the stream and leaves the game running. |
| Windows setup folder unknown (N3) | For a first play on this PC, play once in Mirror Desktop, then close the game and Epic. If played before, finish any update and check that the folder is readable. This is not proof the game was never played. |
| Installation unavailable or changed | Complete the install/update in Epic, rescan and select the current installation again. |
| Game or its exact runtime still running | Close that game yourself and check again. No process-group or wineserver cleanup is performed by the checklist. |
| Launcher check unresolved (generic N4) | Retry, or use Mirror Desktop. An unresolved census alone does not identify another AppImage or program; the checklist does not name or terminate one. |
| Active/changing stream / session-busy rescan | Wait until the stream is idle. Use the existing session controls for an active stream. |
| Another setup check in progress | Wait briefly and check again. A coalesced read does not mean a stream is active. |
| Checks paused / stale sample | Return to the visible checklist or **Check again** for a fresh sample. Historical scan evidence is not a live closed/ready badge. |
| Offline / request timed out / host read failed | Check the connection and host availability, then retry. These outcomes never mean Epic is closed. |
| Sign-in required (401/403) | Sign in to this PC's Polaris console. Signing out clears visible host facts. |
| Host sign-in state unavailable | Reconnect to the console and retry when the host can read its credential state. |
| Live setup unsupported (including an older host's 404) | Follow the static open → scan → quit instructions; live checkmarks are unavailable. |
| Malformed or unknown-version setup response | Check host/console version compatibility. Missing fields, unknown versions or unknown enum values cannot authorize readiness. |

The checklist's line for whether Epic is closed reads **Can't check** even when Epic is closed,
because the host only looks for a running launcher when a launch asks. Polaris checks everything
again when you launch.

## Mirror Desktop: Disconnect, End and close permission

Games are **unmanaged by default**. Disconnect and Resume in Nova or Moonlight leave the game
running. End Session, or **Disconnect Client** in Mission Control, ends the session and leaves
an unmanaged game running too; close it on the host when you are finished.

While the stream is open, the signed-in **host web console** can offer **Allow close on End
Session** in Mission Control. **Yes, allow close on End Session** grants permission for the exact game instance
currently observed, including one you may have started outside Polaris. This is your explicit
permission, not a claim that the earlier launch request started that process. A paired client's
launch permission does not grant this access.

Granting permission does not close the game immediately. With permission granted, **End Session**
in the streaming client, or **Disconnect Client** in Mission Control, can ask that exact instance
to close. **Turn off close on End** revokes permission without sending a signal. Disconnect/Resume in
the streaming client keep the grant for the same instance;
replacement, restart or a changed process cannot silently inherit it. If identity cannot be
verified or exit is not confirmed, close the game on the host. Polaris does not escalate to a
process-group, launcher, prefix, wineserver or forced-kill fallback.

If a status check cannot verify the game, confirmation pauses with **Yes** disabled. It resumes
only when the same game is verified again. A changed game or session needs a new confirmation;
refused requests remain visible instead of being silently retried.

Some desktop processes restrict inspection. When this happens, the console shows how many
could not be inspected before you confirm. Permission still concerns only the verified game
instance, not every game on the desktop. A change in that inspection context invalidates the
permission; Polaris will not guess a replacement or close an uninspected process.

## Private Stream

This path requires an explicit `headless_stream` selection and the actual **labwc** private
runtime. It never falls back to sending the request to the desktop. Existing entries and host
defaults are not migrated. Mission Control shows the private session's state without consent
actions.

While Epic is running on the host desktop, scan the signed-in Polaris Library or select the
official Epic provider. Polaris retains proof of that exact original launcher executable in
memory; a successful Mirror admission can retain it too. Then quit Epic completely. Repeat this
after every Polaris restart or Epic update.
Polaris never guesses an installed path, persists an untrusted executable proof, or reads your
Epic account data. An AppImage's original backing-file proof remains necessary after its mount
disappears.

Before the single private launch request, Polaris checks that no owner Epic launcher process,
requested game or exact title-prefix server is present. Incomplete or unreadable checks refuse.

Other programs starting and exiting while it checks do not refuse. It answers only once it has
judged every process of yours that a fresh look at `/proc` still shows, by the same rules, so a
program started during the check is checked too, and one that starts running something else is
checked again. A process the kernel proves has exited or is exiting is skipped, and another user's
needs no judgement. It still refuses when a process that could be Epic or the game changes or exits during
the check, when a process's absence isn't proven, and when so much keeps starting that sixteen
looks at `/proc` never find nothing new. A check is only ever a sample: a launch after it is not
excluded. The same rule applies while a session runs, so a program exiting during a status check
or End never loses the session's claim on its game.

Some session and system processes hide their details even from your own account, such as
`systemd --user`, KWin, the polkit agent, `ssh-agent` and SSH logins. Polaris sets such a process
aside only when the details any process can read (its name, start time, parent chain and systemd
unit) show that it belongs to your user manager, a service that manager started, or a login
session. For a service, your user manager must also report a unit your system installed: not
transient, and defined only by root-owned files in `/usr/lib/systemd/user` or `/etc/systemd/user`.
That is a property of the paths the manager reports for the unit's fragment and every drop-in:
each must be a root-owned regular file there, at a path not reached through a link. A unit started
with `systemd-run`, one whose reported fragment is in your own `~/.config/systemd/user`, and one
a drop-in of yours changes refuse whatever they are called, as does any manager query that fails
or changes during the check. systemd resolves an alias to its unit before reporting these paths,
so a link of your own to a genuine vendor unit is judged as that vendor unit; the rule does not
show that only root chose how that program was started. Each query has its own one-second
deadline. A unit is asked about once when the census places a process in it and again at the
census's end, so the bound is per unit, not one second for the whole admission, and a stalled
query refuses. Every one of these facts is checked again at the end of the census; a process that
has exited by then needs no role. Any other hidden process refuses. If that judgement is ever
wrong, a hidden desktop Epic receives the
request and the game opens on the desktop: Private Stream shows the game as unverified, End
closes only the private session, and nothing outside it is signalled.

An unassociated AppImage, even if renamed, can conservatively prevent a complete launcher census;
quit other AppImages or use Mirror Desktop. A running copy of the retained launcher counts
whatever path it runs by. Play a game once in Mirror Desktop before its first Private Stream:
until it has run, it has no prefix, and a missing or unreadable prefix is unknown, not proof that
its server is absent. Finish the game's normal setup in Epic on the desktop first.

The one requested URI is the new private compositor's startup client, in the game's installation
directory with this generation's credentials. An AppImage cannot mount itself inside
`headless_gamepad_isolation`'s sandbox, which has no `/dev/fuse`, so when host controllers are being
hidden, Polaris mounts the retained AppImage with its own runtime first, outside the sandbox, and
starts only its `AppRun` inside. The URI reaches only that launcher, never the mount step. If the
mount fails, nothing is launched and the stream stays black until End, with the reason in the
startup diagnostics. A retained runtime that doesn't offer the mount refuses before the session
starts. A request lower bound is recorded before startup;
it is not an exact URI-dispatch timestamp. The launcher and game must actually stay inside that
private generation and display. An escaped, unreadable, stale or ambiguous game cannot receive
individual close authority. Sampled absence, ancestry and timing cannot prove URI causation or
exclude a later concurrent request from another same-user application.

Status or purposeful End can lazily retain the unique exact requested game, even much later,
only if that process was **born within the first 30 seconds** after the request lower bound.
A slower launch remains unowned. The observation time is not a deadline, so leaving Mission
Control closed does not by itself prevent game-first End. A failed or retired grant never
switches to a replacement. Another title never becomes this requested game's individual close
target. Private status is session-managed and offers no manual adopt/release action. Until the
30 seconds pass, a game not seen yet reads as waiting; after them it reads as unverified, since
it may have opened on the desktop, while a game born inside the window can still be retained.

Disconnect and Resume preserve the private session. **End closes the whole owned private
session**, including its newly started Epic launcher and any additional games opened inside it; an
existing desktop launcher or outside game is not part of that authority. A process carrying the
session's token counts only inside its supervisor tree or Polaris's own cgroup; one elsewhere is
left running and logged as `left pid=<pid> running`, once per sweep however the sweep ends. When
the exact requested game is retained, Polaris asks it to close through its pidfd first. Within the
existing five-second Epic close budget, a confirmed exit can leave up to two seconds of launcher
grace before ordinary private-session teardown. This is an allowed grace period, not evidence that
Epic cloud synchronization completed. If the game cannot be retained or graceful exit is not
confirmed, the status says so; ending the session does not prove exact-game graceful closure.
There is no individual game group/prefix/wineserver or forced-kill fallback.

An AppImage launcher's mount outlasts the launcher: Polaris ends the mount's two processes only
after the launcher has gone, one first and then, if they haven't both exited within a quarter of a
second, both. Ending them together makes the mount's daemon die before it unmounts, leaving a dead
`/tmp/.mount_*` directory until reboot. If the private app stop is skipped because the compositor
has already gone, or Polaris itself stops, that can still happen; it is harmless to a later launch.

The owner-controlled Alan Wake 2 launch/display/process-tree, sign-in availability,
Disconnect/Resume, End order, launcher settle and save-integrity checks remain later gates.
No source-only result establishes those outcomes.

## When launch or observation is unavailable

- Open Epic on the host desktop and finish any install or update before trying again.
- Use Mirror Desktop if Private Stream or its ownership checks are unavailable.
  A running desktop launcher cannot be moved into the private compositor.
- Under `headless_gamepad_isolation`, Epic can't relaunch itself from inside the sandbox, since its
  AppImage needs FUSE to start again. If Epic updates itself during a Private Stream, the launcher
  is likely to end and the stream to go black until End; let Epic update on the host desktop first.
  The update changes the file, so the next launch needs Epic opened once on the desktop again.
- An AppImage's mounted internal executable is not a supported standalone launch command.
  Polaris must establish the original launcher backing file; otherwise launch is refused.
  For filename-only FUSE mounts, this also needs the kernel’s `fuse_connection` descriptor
  metadata to match the running AppImage to that exact mount. Older kernels without that
  metadata keep this route unavailable.
- A game may be playable while its exact process cannot be verified. In that case it remains
  unmanaged; the warning is not evidence of software encoding, a broken save or a stopped game.
- Keep using your existing Heroic entry if the official provider is unavailable. Its own setup,
  library, save locations and account data remain separate.
