# Desktop Steam and End Session

For Host Virtual Display or Mirror Host Desktop, enable **Keep Desktop Steam Running After End Session** in Settings. Each application can use the host default, keep Steam running, or close Steam started for that stream.

This applies to Polaris's managed Steam launches. Detached commands run when an application starts; they are not exit hooks. Custom prep and undo scripts still run as configured.

Standalone Steam launches can remain open. Delayed launch scripts and other detached commands still stop with the stream.

With the keep option, End Session asks the Steam game this stream launched to close, then removes the stream output and leaves desktop Steam running. A game already open when the stream started is left alone. A restarted game is a different launch and is left alone too. If the game does not close, Polaris retains the stream's output and owner so End Session can be retried after closing the game. It does not force-kill the game.

Without the keep option, Steam started by the stream follows the existing cleanup behavior. Desktop Steam already running before the stream remains desktop-owned. A game started by this stream inside that client is still asked to close.

Private streams keep their existing lifecycle: Disconnect preserves a resumable session; End Session cleans up the private runtime. Game Mode's Steam remains owned by Game Mode. This setting does not control physical monitor sleep and does not restart Steam after End Session.

## Games with several launch options

Some games list more than one way to start in Steam: Hades offers DirectX, Vulkan and 32-bit. Steam shows **Select launch option** for those games and waits, so the game starts only once someone chooses inside the stream. On a handheld that can mean dragging the cursor to **Play**.

Polaris leaves that choice to Steam. No launch command has been confirmed to skip the dialog, and Polaris never changes your Steam settings. Steam's dialog has its own checkbox to remember your selection, and the game's **Properties** in Steam can change that selection later.

Mission Control and the game's `steam_launch.mode_reason` say **Steam may ask** for a title with more than one launch option. Polaris reads this from Steam's `appcache/appinfo.vdf`, without writing to it. It cannot see a choice you told Steam to remember, or an option that needs a beta branch or DLC, so the note can appear for a game that starts straight away.

## Nova controls

Nova's selected PC menu and Command Center offer **Steam** on Android and Linux. Open Steam, Open Big Picture, and Quit Steam use the selected paired host. Status is observed from the host. Quit requires an additional confirmation when a game is running. These controls are unavailable for viewers, another client's session, private runtimes, and Game Mode.

Moonlight users can use the host's keep-running policy without Nova. Play Setup in Nova displays the policy for the selected display mode.

## API

`GET /polaris/v1/host/steam` returns a version 1 status with `state` (`running`, `stopped`, or `unavailable`), `revision`, `game_active`, and `effective_keep_running`. Unknown ownership is unavailable rather than stopped.

`POST /polaris/v1/host/steam/action` accepts only `action` (`open`, `big_picture`, or `quit`), the observed `revision`, and optional boolean `confirm_game_quit`. Both routes require a paired certificate with launch permission. The host serializes actions with session launch and stop, rechecks ownership and revision, and rejects stale status. Quit uses Steam's forwarding client without bootstrapping a replacement Steam process. A successful dispatch is `accepted`; clients then read status to confirm the outcome. Mutations must never be retried automatically.

The game-library `steam_lifecycle` contract reports keep behavior for Host Virtual Display and Mirror Host Desktop. Other display modes retain their own runtime cleanup rules.
