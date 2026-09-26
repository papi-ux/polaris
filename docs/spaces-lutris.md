# Lutris in a Space

A Lutris Space has its own launcher library, accounts, game installations and
saves. Install and configure games in Lutris, then open their tiles from Nova.
It does not reuse the Lutris library on the host's regular desktop.

This uses the [Spaces preview](spaces.md#preview-limits). Complete
[Host Setup](spaces.md#host-setup) first; the runtime supplies Lutris inside
the Space. The preview currently allows one active Space per host; see the
preview limits for graphics and audio acceptance. Have touch or a mouse
available for launcher setup. A game's controller support depends on that
game and its runner.

## Create and open the Space

1. Stop Space streams and open **Spaces** in the Polaris console.
2. Select **Create a Space**, name it and choose **Lutris** as its
   **Launcher**. If Lutris is not offered, return to Host Setup and select
   **Recheck Setup**.
3. Wait for creation to finish. The first Lutris Space downloads its gaming
   runtime. Later Lutris Spaces reuse an existing Lutris runtime, with fresh
   player data and sign-ins.
4. Give your paired device access in
   [Device Access](spaces.md#give-a-device-access). Refresh Nova's library and
   choose the new Space using **Change Space** when more than one is available.
5. Open the **Lutris** launcher tile. Sign in to any accounts you need, install
   a game and configure its runner inside Lutris. The Space's label does not
   select an account or copy settings from another Space.

## Play an installed game

Finish installing the game in Lutris, then return to Nova's library and
refresh it. The library can take up to 15 seconds to refresh. Polaris lists
installed games from this Space's Lutris library; games hidden in Lutris stay
out of Nova's list.

Choose the game's tile and press **Play**. Lutris uses its saved configuration
for that game. Change the runner or Wine settings in Lutris; change streaming
resolution and frame rate in Nova's **Play Setup**. Start at 60 FPS and check
picture, sound and controls before raising the target.

Save before leaving: a disconnect ends the running game in this preview.
Installed games, saves and launcher sign-ins remain in the Space. **Resume**
only applies to a session your device still owns, and does not mean a
disconnected game kept running.

## When something is missing

| What you see | What to check |
| --- | --- |
| Only the Lutris tile | Open Lutris and confirm that the game is installed in this Space. An account's available games and the host desktop's installations are separate. Wait 15 seconds after installation and refresh Nova. |
| One installed game is absent | Check whether it is hidden in Lutris. If you just changed the library, exit Lutris normally, save any running game before leaving the Space, then refresh Nova. |
| A tile opens Lutris but the game fails | Check the game's installation, runner and launch settings in Lutris. Appearing in the library does not guarantee that a game is compatible. |
| Setup needs a pointer | Use touch or a mouse in the launcher. Test the game's own controller input after it starts. |
| The Space cannot open | Read its reported status and use **Recheck Setup** in Polaris. The same preview capacity and graphics limits apply to Lutris. |

Use the Space card in Polaris for [runtime updates](spaces.md#runtime-updates).
The runtime supplies the launcher; the Space keeps the player's installed
games and configuration. Follow
[Rename, remove and restore](spaces.md#rename-remove-and-restore) when archiving
a Space or deleting its data.
