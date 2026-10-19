# Install on SteamOS 3.8 and 3.9

Polaris provides a dedicated x86_64 package for each supported SteamOS release: `Polaris-steamos3.8-x86_64.pkg.tar.zst` for SteamOS 3.8 and `Polaris-steamos3.9-x86_64.pkg.tar.zst` for SteamOS 3.9. The 3.8 package is built against Valve's versioned SteamOS 3.8 package repositories and the 3.9 package against the SteamOS 3.9 ones. These packages are not built against rolling Arch Linux; the rolling Arch package is not a supported substitute on SteamOS.

Install the package for the release your Deck runs, which `VERSION_ID` in `/etc/os-release` names. Each package depends on the exact Boost libraries of its own release, so pacman refuses the other release's package instead of installing a Polaris that cannot start.

## Validation Status

The SteamOS 3.8 package is proven on a Steam Deck OLED on SteamOS 3.8.16, in Desktop Mode and in Game Mode. In Game Mode the host streams the Game Mode screen: a Steam title launched from a client opens there, a controller reaches it as a virtual DualSense, touch lands where it is aimed, and ending the session closes only the title the stream opened. The Steam Deck LCD, OLED 90 Hz behavior, suspend and resume, and persistence across SteamOS updates are not certified yet; those claims need their own hardware evidence. The SteamOS 3.9 package was exercised on a docked Steam Deck OLED on SteamOS 3.9.2. [Leave Game Mode Before Launch](#leave-game-mode-before-launch) checks covered an opted-in launch from Moonlight on a PC and five minutes of picture, sound and controls, including reconnects, on an earlier beta.2 build. After the fixes, a separate focused End check confirmed automatic return to Game Mode on the updated build. These streams ran H.264 at about 60 fps through the Gamescope portal; HEVC, 120 fps and HDR on the Deck were not checked. [Handhelds and Game Mode](handhelds.md) explains what a Deck can do in each mode and how to keep Polaris reachable when it returns to Game Mode.

Continuous integration builds and validates each package inside a clean root of its own SteamOS release, bootstrapped from Valve's repositories. A clean root has none of the packages a shipped SteamOS image already carries, so that gate proves the package builds and its libraries resolve. It does not prove the install transaction is conflict-free on a device, and it did not catch the `libdisplay-info` conflict described under Stream Paths on SteamOS.

SteamOS uses a read-only root by default. Polaris installation and `sudo -H polaris --setup-host` must run while the root is writable. Read-only mode must be restored before the user service starts.

## Install

Open a terminal in Desktop Mode. On SteamOS 3.8, run:

```bash
wget --output-document=./Polaris-steamos3.8-x86_64.pkg.tar.zst https://github.com/papi-ux/polaris/releases/latest/download/Polaris-steamos3.8-x86_64.pkg.tar.zst &&
(
set -e
trap 'sudo steamos-readonly enable' EXIT
sudo steamos-readonly disable || exit $?
sudo pacman-key --init || exit $?
sudo pacman-key --populate || exit $?
sudo pacman -Sy || exit $?
sudo pacman -U ./Polaris-steamos3.8-x86_64.pkg.tar.zst || exit $?
sudo -H polaris --setup-host --enable-headless-boot || exit $?
sudo steamos-readonly enable || exit $?
trap - EXIT
) &&
systemctl --user enable --now polaris
```

On SteamOS 3.9, use the matching package from `v1.5.0-beta.2`. This command
requires that prerelease to have been published. Beta releases do not replace
the latest stable release, so `releases/latest/download` does not select it.

```bash
wget --output-document=./Polaris-steamos3.9-x86_64.pkg.tar.zst https://github.com/papi-ux/polaris/releases/download/v1.5.0-beta.2/Polaris-steamos3.9-x86_64.pkg.tar.zst &&
(
set -e
trap 'sudo steamos-readonly enable' EXIT
sudo steamos-readonly disable || exit $?
sudo pacman-key --init || exit $?
sudo pacman-key --populate || exit $?
sudo pacman -Sy || exit $?
sudo pacman -U ./Polaris-steamos3.9-x86_64.pkg.tar.zst || exit $?
sudo -H polaris --setup-host --enable-headless-boot || exit $?
sudo steamos-readonly enable || exit $?
trap - EXIT
) &&
systemctl --user enable --now polaris
```

The `EXIT` trap attempts to restore read-only mode if disabling the root, package installation, host setup, or explicit restoration fails. The user service starts only after the package and setup steps succeed and read-only mode is restored.

`--enable-headless-boot` starts Polaris at boot, before anyone signs in, so it keeps running when the Deck switches between Desktop Mode and Game Mode. Without it a Deck that boots into Game Mode has no Polaris until someone opens Desktop Mode. `sudo -H polaris --setup-host --disable-headless-boot` turns it off again. [Handhelds and Game Mode](handhelds.md#keep-polaris-reachable-across-mode-switches) explains what it changes.

With headless boot enabled, the Polaris user service skips its desktop tray so closing the compositor cannot end the host through the tray's GTK display connection. This also applies when the service runs the KMS helper. Use the web console to control that service. Direct launches and ordinary desktop services without headless boot still honor your system tray setting.

SteamOS ships without an initialized pacman keyring, so the first `pacman` transaction that has to download anything fails signature verification. `pacman-key --init` creates `/etc/pacman.d/gnupg`, and `pacman-key --populate` imports every keyring already on the image, which on SteamOS means the Arch and Holo keyrings. Both steps are one-time and idempotent, and both write to the root filesystem, so they run after `steamos-readonly disable`.

`pacman -Sy` refreshes the sync databases so `pacman -U` can resolve the handful of Polaris dependencies a stock SteamOS image does not carry. Do not substitute `-Syu`. A full upgrade replaces Valve's pinned packages with rolling Arch versions and is not supported on SteamOS.

Installing v1.3.9 or earlier additionally needs `--assume-installed labwc=0.9.0` on the `pacman -U` step. Those packages declared a `labwc` dependency that pulls in a library downgrade which breaks Desktop Mode. See Troubleshooting below.

**Fresh install:** open `https://localhost:47990/#/welcome`, create the web UI account, and pair Nova, Moonlight, or another GameStream-compatible client.

**Upgrade or reinstall:** open `https://localhost:47990/#/login` and use the existing account. Pacman package operations preserve credentials, pairing keys, settings, and the library under `~/.config/polaris`; reinstalling after a SteamOS update does not reset the web account. If needed, follow the [credential reset](troubleshooting.md#web-ui-credentials) instead of returning to Welcome.

## Connect a Client

Nova and Moonlight will not find a Steam Deck on their own. SteamOS ships its network announcement
service, avahi, with publishing turned off, so the Deck never says it is there. Add it by its address
instead: in Nova open **Servers**, select **Add Server**, and enter the Deck's address. Polaris writes
the address to its log when it starts, in a line that begins "This host does not allow network
announcements", and **Settings > Internet** on the Deck shows it too. Then pair as usual.

## Stream Paths on SteamOS

Polaris defaults to Mirror Desktop, which streams the visible Desktop Mode session through the portal and needs nothing beyond this package. Gamescope Stream is also available, because SteamOS ships gamescope.

The Private Stream paths need `labwc` on `PATH`, and labwc cannot be installed on SteamOS 3.8 without breaking the system. labwc 0.9.0 in `extra-3.8.1x` depends on wlroots0.19, which requires `libdisplay-info.so.2` from libdisplay-info 0.2.0. SteamOS installs libdisplay-info 0.3.0 from `holo-3.8.1x`, which provides `libdisplay-info.so.3`, and KWin, Mesa, and Vulkan link against that. Installing labwc therefore downgrades a library the desktop depends on and breaks Desktop Mode. labwc has not been tried on SteamOS 3.9.

Polaris does not depend on labwc for this reason, and links nothing from it. Private Stream cards are greyed out on SteamOS and name the missing `labwc` tool; a pre-existing Private Stream configuration also fails closed at launch. Use Mirror Desktop or Gamescope Stream instead.

## Leave Game Mode Before Launch

An app can opt in to leaving Game Mode before its own private launch with `"leave-game-mode-before-launch": true`. It is off for every app unless set, and it never applies to resume, watch-only, input-only, profile or Space launches. This build implements the switch on Linux builds with GIO. It has had limited hands-on checks on a Steam Deck OLED on SteamOS 3.9.2, described under [Validation Status](#validation-status); that is not certification.

The switch needs SteamOS Manager 26.4.1, which ships with SteamOS 3.9. SteamOS 3.8 carries 26.2.1, so on SteamOS 3.8 an opted-in launch is refused before anything changes.

Whether anything is needed comes only from typed records: this account's `gamescope-session.service` as its own user manager reports it. If that unit is not loaded, inactive or failed, nothing is needed. If it is active, the launch must leave Game Mode. If it is anything else, or the records cannot be read, an opted-in launch is refused rather than treated as "not in Game Mode". Builds that cannot read the records (Linux without GIO) refuse opted-in launches. Platforms without Steam Game Mode need nothing.

On every launch Polaris proves the following, without starting any service. If any of it cannot be read, or reads otherwise, the launch is refused with a fixed `prelaunch_*` code before anything changes:

- `/usr/bin/steamosctl` is a root-owned regular file that nobody else can write, under root-owned directories.
- Exactly one `steamos-manager` package record is installed. It names itself, is release 26.4.1 (the only release whose session switch was audited), and owns both `/usr/bin/steamosctl` and `/usr/lib/steamos-manager`.
- SteamOS Manager on this account's own user bus:
  - its name's owner is this account, running `/usr/lib/steamos-manager` in `steamos-manager.service` under this account's user manager;
  - its introspection, parsed as D-Bus XML, declares `SessionManagement1` once, with `SwitchToDesktopMode` and `SwitchToGameMode` each declared once and without arguments;
  - the name is still owned by that same connection and process afterwards.
- logind and the user manager:
  - logind is bound by its bus name's owner: root, in `systemd-logind.service`;
  - this account's user manager is bound the same way: this account, in `user@UID.service/init.scope`;
  - every read goes to those unique names, and they must still own their names, with the same processes, afterwards.
- Two complete passes over the records agree field by field: the seat's active session, the session listing, every session record, the leader's start time, and each unit's state and generation. Within them, logind shows one active, local graphical session on seat0. That session belongs to this account, is its only graphical session, and was started by SDDM autologin. `graphical-session.target` is running. A property repeated in a record, of any type, is refused.
- Polaris survives the switch:
  - `polaris.service` is active and is this very process's unit (its control group);
  - none of its stop dependencies names the session's units;
  - the complete closure of units that stopping `graphical-session.target` stops does not contain it (RequiredBy, ConsistsOf, BoundBy, RequisiteOf and PropagatesStopTo, transitively, within 128 units);
  - headless boot is enabled.

Polaris then runs exactly `steamosctl switch-to-desktop-mode`, with only this account's bus address, `PATH` and `LANG` in its environment. The command runs as a child Polaris owns: in its own process group, with stdio on `/dev/null`, and every other descriptor closed by `close_range`. Where that is unavailable, descriptors are closed up to a bound proven before fork. If neither is possible, the command is not started. It is bounded, stopped and reaped. Its exit 0 means only that the request was sent. The launch continues once the old session and its target generation have both ended, a new desktop session of the same account is bound, and the stream's route is ready.

If the launch fails before it reaches its client, Polaris returns that same desktop to Game Mode. It does the same when the client ends the session on request. Either way the desktop must still be the one this launch opened. Before submitting the return, Polaris rechecks its current service cgroup, the full stop dependency closure and headless boot readiness; a launch-time proof cannot authorize a later stop. Polaris then proves a new Game Mode session afterwards. The return after a session ends is queued under the session lock but runs on one worker owned by Polaris, outside lifecycle locks. From the moment it is queued until its outcome is recorded, no launch or stream setup of any app is admitted. A teardown that happens while a launch is being admitted does not return the host at all. If Polaris starts stopping, no new switch starts, including from a coordinator first created after the stop began.

If the switch was stopped after it started, or its result could not be bound, the receipt records the outcome as unknown and nothing is undone. The switch takes no session argument, so a session change between Polaris's last check and SteamOS Manager acting on it cannot be prevented, only detected afterwards. Such a change is never adopted or rolled back.

A launch spends at most 60 seconds leaving Game Mode and 25 more rolling back. A return after the session ends takes at most 45 seconds. Console and Browser Stream launches have no cancellation of their own, and their receipts say so (`cancellation_transport: unsupported`).

The app list shows readiness within one second without starting a service. A positive answer is kept for up to five seconds, and only while the boot, the command, each package record's `desc` and `files`, the bus and the manager's connection are unchanged. A refusal or a timeout answers the rest of the list for two seconds and is never shown as ready. Launches never use these kept answers.

### What is proved where

Source and native fixtures cover all of the following:

- the rules above over fixture records;
- a GDBus peer server that stands in for logind, the user manager, the bus daemon and SteamOS Manager;
- a `/bin/sh` stand-in for `steamosctl`;
- descriptor closure, process group termination, the lifecycle gate's fence, and stop handling.

The hardware checks exercised the following SteamOS assumptions on one Steam Deck OLED on SteamOS 3.9.2. They did not exercise every cancellation and failure permutation covered by the fixtures above, or check other devices and SteamOS releases:

- Game Mode and Desktop Mode are distinct logind sessions started by `sddm-autologin`.
- `gamescope-session.service` marks Game Mode.
- SteamOS Manager 26.4.1's switch stops `graphical-session.target` as its sources describe.
- The cgroup paths of logind, the user manager and `steamos-manager.service`.
- `/proc/<pid>/exe` of the manager is readable by the same account.
- The installed systemd's dependency properties and stop propagation support the session switch.

## Update

Download the new package for your SteamOS release, `Polaris-steamos3.8-x86_64.pkg.tar.zst` or `Polaris-steamos3.9-x86_64.pkg.tar.zst`, and repeat its failure-safe install command. The package manager replaces the prior Polaris files, host setup refreshes required integration, and read-only mode is restored before service startup.

A SteamOS operating-system update may remove packages layered into the mutable root. If Polaris disappears after an OS update, return to Desktop Mode and reinstall the current package for the SteamOS release the Deck now runs. After an update from SteamOS 3.8 to 3.9, that is the 3.9 package. Do not substitute the rolling Arch package.

The release also carries `Polaris-kms-steamos3.8-x86_64.pkg.tar.zst` and `Polaris-kms-steamos3.9-x86_64.pkg.tar.zst`, the optional DRM/KMS capture helper for each release. Mirror Desktop, Gamescope Stream and Game Mode streaming do not need it, and it has not been validated on a Deck. If you installed it, download the one for your SteamOS release from the same release each time and add it to the `pacman -U` line, for example `./Polaris-kms-steamos3.8-x86_64.pkg.tar.zst`, including the reinstall after a SteamOS update. On a host that ran 1.4.13-beta.2 or beta.3, the Polaris package named alone replaces Polaris but leaves the beta's helper in place.

After Polaris restarts, return to `https://localhost:47990/#/login` with the existing web credentials.

## Remove and Roll Back

Turn off boot start, stop the service, remove the package while the root is writable, and restore read-only mode even if removal fails:

```bash
sudo -H polaris --setup-host --disable-headless-boot
systemctl --user disable --now polaris
(
set -e
trap 'sudo steamos-readonly enable' EXIT
sudo steamos-readonly disable || exit $?
sudo pacman -Rns polaris || exit $?
sudo steamos-readonly enable || exit $?
trap - EXIT
)
```

If `polaris-kms` is installed, use this block instead. It runs `--disable-kms`, which points the user service back at the ordinary binary, while the root is writable, as every `--setup-host` run on SteamOS needs. Then it removes both packages, since removing `polaris` alone is refused while the helper depends on it:

```bash
sudo -H polaris --setup-host --disable-headless-boot
systemctl --user disable --now polaris
(
set -e
trap 'sudo steamos-readonly enable' EXIT
sudo steamos-readonly disable || exit $?
sudo -H polaris --setup-host --disable-kms || exit $?
sudo pacman -Rns polaris polaris-kms || exit $?
sudo steamos-readonly enable || exit $?
trap - EXIT
)
```

Package removal does not automatically delete user configuration under `~/.config/polaris`. Keep that directory if you plan to reinstall, or remove it separately only after backing up any settings you need.

For a clean slate, or to remove what the package leaves behind, see
[Uninstall Polaris, or start over](uninstall.md).

## Troubleshooting

### `keyring is not writable` or `required key missing from keyring`

```
warning: Public keyring not found; have you run 'pacman-key --init'?
downloading required keys...
error: keyring is not writable
error: required key missing from keyring
error: failed to commit transaction (unexpected error)
```

The pacman keyring was never initialized, or the root filesystem is still read-only. Run `sudo steamos-readonly disable`, then `sudo pacman-key --init` and `sudo pacman-key --populate`, then retry. Nothing was installed when this error appears, so it is safe to rerun the whole install block.

### pacman offers to downgrade `libdisplay-info`

Answer no and let the transaction abort. A downgrade from 0.3.0 to 0.2.0 breaks KWin, Mesa, and Vulkan, which takes Desktop Mode with it. It means labwc or another wlroots0.19 consumer entered the transaction.

Polaris no longer declares that dependency. If you are installing v1.3.9 or earlier, add `--assume-installed labwc=0.9.0` to the package step so pacman treats it as already satisfied:

```bash
sudo pacman -U --assume-installed labwc=0.9.0 ./Polaris-steamos3.8-x86_64.pkg.tar.zst
```

Polaris does not use labwc on SteamOS either way, so nothing is lost by skipping it.

## Reporting SteamOS Results

Include the SteamOS version, device model, Desktop Mode or Game Mode, GPU, client, package filename, and relevant Polaris logs. Clearly separate package and startup success from gameplay, display-rate, suspend, and OS-update persistence results.
