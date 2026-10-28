# Official Epic Linux Preview

This integration is being qualified for Polaris 1.5.0. It imports completed Windows-game
installations from the official Epic Linux Preview and launches them in **Mirror Desktop**.
Epic manages its own sign-in, downloads, updates, Proton runtime and cloud saves.
The explicit labwc **Private Stream** path described below is a source candidate pending
native, integrated UI and owner-controlled runtime qualification. It is not a stable availability
or hardware acceptance claim; imported entries still default to Mirror Desktop.

## Install and open Epic

Use [Epic's official Preview download page](https://launcher.epicgames.com/).
[Epic documents](https://www.epicgames.com/help/c-32735058/c-Trending_0/what-is-the-epic-games-launcher-early-preview-a12104725)
an x86-64 AppImage and a Debian package. Both are the Preview launcher, not a stable release.
Choose the package suited to your distribution; Polaris does not install or convert Epic packages.

Open Epic on the host desktop, sign in yourself, and let the selected game's installation or
update finish. Keep the launcher open before launching through Polaris. Polaris does not read
your Epic account store, sign you in, install a game, select Proton or change your saves.

Mirror Desktop uses the already-running **desktop** launcher. The private candidate starts its
own launcher only after the desktop launcher is completely quit. Game Mode, Host Virtual Display,
Gamescope/private Gamescope and Spaces remain refused instead of receiving a forwarded desktop launch.
Native Linux games are not enabled in this first installed-metadata slice.

## Add a game

In **Library**, scan for games and open **Epic (Preview)**. Select a completed installation and
import it. Polaris rebuilds its launch request from the host's current installed-state metadata
every time; a pending update or ambiguous installation is unavailable until resolved in Epic.

If the same exact title is already imported through Heroic and its catalog identity can be
verified, the scan shows one entry with an explicit **Use Epic Launcher** or **Use Heroic** choice.
Confirming a provider change keeps that entry's UUID, artwork, history and other settings.
Polaris never switches an existing Heroic entry automatically. A name alone is not sufficient
to merge titles, and uncertain catalog data does not authorize a provider change.

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

## Private Stream candidate

This path requires an explicit `headless_stream` selection and the actual **labwc** private
runtime. It never falls back to sending the request to the desktop. Existing entries and host
defaults are not migrated. The corresponding Launch as caption and Private session controls
still need their frontend integration and qualification.

While Epic is running on the host desktop, scan the signed-in Polaris Library or select the
official Epic provider. Polaris retains proof of that exact original launcher executable in
memory; a successful Mirror admission can retain it too. Then quit Epic completely. After a
Polaris restart or launcher update, repeat this setup if the proof is unavailable or stale.
Polaris never guesses an installed path, persists an untrusted executable proof, or reads your
Epic account data. An AppImage's original backing-file proof remains necessary after its mount
disappears.

Before the single private launch request, Polaris checks that no owner Epic launcher process,
requested game or exact title-prefix server is present. Incomplete or unreadable checks refuse.
An unassociated AppImage, even if renamed, can conservatively prevent a complete launcher census;
quit other AppImages or use Mirror Desktop. A missing or unreadable prefix is unknown, not proof
that its server is absent. Finish the game's normal setup in Epic on the desktop yourself first.

The one requested URI is the new private compositor's startup client, in the game's installation
directory with this generation's credentials. A request lower bound is recorded before startup;
it is not an exact URI-dispatch timestamp. The launcher and game must actually stay inside that
private generation and display. An escaped, unreadable, stale or ambiguous game cannot receive
individual close authority. Sampled absence, ancestry and timing cannot prove URI causation or
exclude a later concurrent request from another same-user application.

Status or purposeful End can lazily retain the unique exact requested game, even much later,
only if that process was **born within the first 30 seconds** after the request lower bound.
A slower launch remains unowned. The observation time is not a deadline, so leaving Mission
Control closed does not by itself prevent game-first End. A failed or retired grant never
switches to a replacement. Another title never becomes this requested game's individual close
target. Private status is session-managed and offers no manual adopt/release action.

Disconnect and Resume preserve the private session. **End closes the whole owned private
session**, including its newly started Epic launcher and any additional games opened inside it;
an existing desktop launcher or outside game is not part of that authority. When the exact
requested game is retained, Polaris asks it to close through its pidfd first. Within the existing
five-second Epic close budget, a confirmed exit can leave up to two seconds of launcher grace
before ordinary private-session teardown. This is an allowed grace period, not evidence that
Epic cloud synchronization completed. If the game cannot be retained or graceful exit is not
confirmed, the status says so; ending the session does not prove exact-game graceful closure.
There is no individual game group/prefix/wineserver or forced-kill fallback.

The owner-controlled Alan Wake 2 launch/display/process-tree, sign-in availability,
Disconnect/Resume, End order, launcher settle and save-integrity checks remain later gates.
No source-only result establishes those outcomes.

## When launch or observation is unavailable

- Open Epic on the host desktop and finish any install or update before trying again.
- Use Mirror Desktop if the explicit labwc private candidate or its ownership checks are unavailable.
  A running desktop launcher cannot be moved into the private compositor.
- An AppImage's mounted internal executable is not a supported standalone launch command.
  Polaris must establish the original launcher backing file; otherwise launch is refused.
  For filename-only FUSE mounts, this also needs the kernel’s `fuse_connection` descriptor
  metadata to match the running AppImage to that exact mount. Older kernels without that
  metadata keep this route unavailable.
- A game may be playable while its exact process cannot be verified. In that case it remains
  unmanaged; the warning is not evidence of software encoding, a broken save or a stopped game.
- Keep using your existing Heroic entry if the official provider is unavailable. Its own setup,
  library, save locations and account data remain separate.
