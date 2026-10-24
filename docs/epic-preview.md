# Official Epic Linux Preview

This integration is being qualified for Polaris 1.5.0. It imports completed Windows-game
installations from the official Epic Linux Preview and launches them in **Mirror Desktop**.
Epic manages its own sign-in, downloads, updates, Proton runtime and cloud saves.

## Install and open Epic

Use [Epic's official Preview download page](https://launcher.epicgames.com/).
[Epic documents](https://www.epicgames.com/help/c-32735058/c-Trending_0/what-is-the-epic-games-launcher-early-preview-a12104725)
an x86-64 AppImage and a Debian package. Both are the Preview launcher, not a stable release.
Choose the package suited to your distribution; Polaris does not install or convert Epic packages.

Open Epic on the host desktop, sign in yourself, and let the selected game's installation or
update finish. Keep the launcher open before launching through Polaris. Polaris does not read
your Epic account store, sign you in, install a game, select Proton or change your saves.

The first integration supports the already-running **desktop** launcher. It refuses Game Mode,
Private Stream, Host Virtual Display and Spaces instead of opening a game on the wrong display.
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

## Disconnect, End and close permission

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

## When launch or observation is unavailable

- Open Epic on the host desktop and finish any install or update before trying again.
- Use Mirror Desktop. This Preview does not move the launcher into a private compositor.
- An AppImage's mounted internal executable is not a supported standalone launch command.
  Polaris must establish the original launcher backing file; otherwise launch is refused.
  For filename-only FUSE mounts, this also needs the kernel’s `fuse_connection` descriptor
  metadata to match the running AppImage to that exact mount. Older kernels without that
  metadata keep this route unavailable.
- A game may be playable while its exact process cannot be verified. In that case it remains
  unmanaged; the warning is not evidence of software encoding, a broken save or a stopped game.
- Keep using your existing Heroic entry if the official provider is unavailable. Its own setup,
  library, save locations and account data remain separate.
