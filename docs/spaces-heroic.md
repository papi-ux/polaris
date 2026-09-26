# Heroic in a Space

A Heroic Space keeps its own Epic, GOG and Amazon sign-ins, installed games
and saves. Open Heroic from Nova to manage that library, then start installed
games from their own tiles in Nova.

This uses the [Spaces preview](spaces.md#preview-limits). Complete
[Host Setup](spaces.md#host-setup) first. The host does not need a separate
Heroic installation. The preview currently allows one active Space per host;
see the preview limits for graphics and audio acceptance. Keep a mouse or
touch controls available: Heroic's menus do not receive controller input in
a Space yet, even though games do.

## Create and open the Space

1. Stop Space streams, then open **Spaces** in the Polaris console.
2. Select **Create a Space**, give it a name and choose **Heroic** as its
   **Launcher**. Use the launchers offered by the page. If Heroic is missing,
   return to Host Setup and use **Recheck Setup**.
3. Wait for creation to finish. The first Heroic Space downloads its gaming
   runtime; the page shows the download and preparation progress. Later Heroic
   Spaces reuse an existing Heroic runtime, but start with their own empty
   player data.
4. In [Device Access](spaces.md#give-a-device-access), allow your paired device
   to open the new Space. Refresh the host's library in Nova and select it
   with **Change Space** if your device has more than one choice.
5. Open the **Heroic** launcher tile. Sign in to the stores you use inside
   Heroic, then install a game there. Your host desktop's Heroic accounts and
   installations are separate from this Space.

## Play an installed game

Let installation finish in Heroic. Return to Nova's library and refresh it;
the Space library can take up to 15 seconds to refresh. Installed Epic, GOG
and Amazon titles appear alongside the Heroic launcher tile. Sideloaded games
added to Heroic's library are also read from that Space.

Select a title and press **Play**. Heroic handles its launch and the runtime
selected for it. Use Heroic's own game settings for Wine or Proton choices;
Nova's **Play Setup** controls the stream's resolution and frame rate. Start
at 60 FPS and check picture, sound and controls before increasing it.

Save in the game before leaving. In this preview, disconnecting ends the
Space's running game. The Space retains installed games, saves and sign-ins;
**Resume** does not promise that a disconnected game stayed running.

## When something is missing

| What you see | What to check |
| --- | --- |
| Only the Heroic tile | Open it and check that the game is installed in this Space, not just owned by the signed-in store account or installed on the host desktop. After installation, wait 15 seconds and refresh Nova's library. |
| A game opens the wrong account | Check the account inside Heroic. The Space name is only a label; it does not select a store account. |
| A controller works in the game but not Heroic | Use touch or a mouse in Heroic's menus. This is a current preview limit. |
| Heroic opens but a game does not start | Open the game's settings in Heroic and check its installation and selected Wine or Proton runtime. A library tile does not establish game compatibility. |
| The Space cannot open | Read its status in Polaris and recheck Host Setup. The preview's capacity and graphics limits also apply to Heroic. |

Heroic's update check starts disabled. Update the Space's gaming runtime
through its card in Polaris when a newer runtime is offered; do not try to
replace the image's Heroic with a package manager inside the Space. See
[Runtime updates](spaces.md#runtime-updates) for progress and recovery.

[Rename, remove and restore](spaces.md#rename-remove-and-restore) explains how
to keep or permanently delete this Space's games, saves and sign-ins.
