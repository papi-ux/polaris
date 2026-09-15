# Spaces

Spaces give each player their own Steam sign-in, installed games, saves, and
settings on one Linux gaming host. Two players can use separate spaces at the
same time. A single player can also use a space on a gaming server without a
monitor, or share one space between a handheld and a TV at different times.

Spaces are optional. If you already stream your usual desktop and games, keep
using the Library in Nova. You do not need Docker for ordinary streaming.

![Spaces with player cards and a default Space for a handheld](images/spaces/spaces-overview.png)

Example with sample player and device names.

## Using the Spaces page

The page keeps everyday controls together. Each card shows its Space name,
current activity, permitted devices, **Rename**, and **Remove Space**.
Expand **Device Access** on a card to change its permitted devices. Expand
**Default Space** to choose which Space each device opens first. Pair and rename
devices in **Devices**.

Open **Host Setup** for current prerequisite checks and first Space preparation.
Checks that need attention link directly to the relevant section of this guide.
Use **Recheck Setup** after completing the terminal steps. Healthy configured
hosts keep this section collapsed.

The **Spaces Guide** link at the top of the page opens this document, including
installation, Steam accounts, first gameplay, troubleshooting, and removal.

## Your first game

1. **Check Host.** Open **Spaces → Host Setup** in Polaris. Follow the checks for
   Docker, graphics, controller access, and security on the Linux PC running
   Polaris. Once configured, Host Setup is collapsed; reopen it to recheck.
2. **Prepare Space.** Download the offered runtime and create a Space with a
   recognizable player or room name, such as **Alex’s Space** or **Living Room**.
   Under **Device Access**, allow your paired handheld or TV. Choose its
   **Default Space** if it can access more than one.
3. **Sign In To Steam.** Open the host’s Library in Nova. **Playing In** in the library toolbar
   shows your current Space. Use **Change Space** when another permitted Space
   is available, then select **Steam Big Picture → Open Steam Big Picture**.
   Sign in through Steam and install a game. A Space name is a label, not proof
   of which Steam account is signed in; check or switch accounts inside Steam.
4. **Test Controls And Sound.** Return to Nova’s library and refresh it after
   installing games. Choose a game and select **Play**. Start at 60 FPS, check
   picture, sound, both sticks, and buttons, then increase settings if desired.
   Use **Play Setup → Change Space** to choose another permitted Space where
   that title is installed. Changing a Space does not copy games or saves.

With one permitted Space, Nova opens its library without a mandatory player
selection screen. The device remembers its selected Space. Ordinary desktop
streaming keeps its existing Library flow.

**Save before leaving.** In this preview, disconnecting ends the Space’s running
Steam/game session. **Leave Space** asks you to confirm and keeps saved files,
installed games, and Steam sign-in. An interrupted connection may also end the
session. **Resume** is offered only for an existing matching session; it does
not promise that a disconnected game stayed running. Other Spaces keep running.

While opening a Space, Nova reports the worker startup request and observed
stream connection stages. It does not estimate download progress or claim that
Steam is ready without a signal. If launch fails, the screen shows the host’s
explanation when available, **View Details**, and **Back To Library** so the next
attempt checks the Space again.

## What to install

Install the Polaris host package for your Linux distribution, and Nova on the
device you will play on. For Spaces, install Docker Engine on the Polaris host
as well. Polaris manages a separate gaming container for each active space.

| Component | Purpose |
| --- | --- |
| Polaris RPM, Arch package, or DEB | Runs the host, pairing, settings, and streaming services |
| Docker Engine on that host | Runs the isolated gaming environments |
| Polaris gaming runtime image | Contains the launcher and the software used inside a space |
| Nova Android app | Opens the stream and sends your controls |

The [packages repository](https://github.com/papi-ux/packages) distributes signed
Fedora and Arch host packages through `repo.papi-ux.com`. The approved gaming
runtime is an OCI image downloaded by Docker from GitHub Container Registry.
Native packages include the setup UI, controller, host policy files, and the
`polaris-spaces-setup` terminal helper;
the image supplies the software running inside each Space. These are coordinated
release artifacts, not interchangeable installation choices.
This preview does not provide a supported image for running the entire Polaris
host inside Docker, or an Unraid installation template.

Steam, its 32-bit libraries, and the gaming userspace belong inside the runtime
image. You do not need a host Steam installation to use a Space. The host still
needs Polaris's native dependencies, Docker, GPU drivers, and input permissions.
NVIDIA userspace in the image must also match the supported host driver version.
The current preview image targets NVIDIA 610.57.04. A clean Arch installation
without host Steam libraries still needs physical acceptance before we describe
that complete setup as tested.

## Preview status

The Spaces tab has host prerequisite checks, Docker installation guidance, and
management for configured Steam spaces. Creating additional spaces from an
existing Steam setup is supported by the development backend.

**First space preparation now has a persistent background job.** It connects the
verified runtime download to a new private Steam home, with progress, stop and
retry controls in Spaces. The preview catalog is still empty until a runtime
completes publication and review, so this build shows that the download is
unavailable. The next step now selects a detected graphics card, saves Spaces
configuration and offers an explicit restart. The initial configuration permits
one active Space; simultaneous Spaces still need a separately reviewed graphics
budget. Fresh Fedora package and Docker installation passed in a VM without GPU
passthrough; gameplay was checked on an existing NVIDIA host. Registry publication
and first runtime download remain release gates. A fresh NVIDIA graphics
installation has not been validated.
The current runtime also requires the Polaris service account to use UID and
GID 1000. Do not change an existing Linux account's identity to work around this
preview limitation.

## Prepare Docker from Spaces

1. Open the Polaris web interface and select **Spaces**, beside **Devices**.
2. Under **Host Setup**, select **Recheck Setup**. Checks run on the PC
   hosting Polaris, even if you opened the page on a phone or another computer.
3. If Docker Engine needs attention, open **Docker Setup Guide** and follow your
   distribution's steps below in a terminal on the Polaris host. Approve package
   installation with your administrator password in that terminal.
4. Start the system Docker service, then grant Docker access to the Linux account
   running Polaris. For a service installation, that account may differ from
   your terminal account; grant access to the service account.
5. Save your work and stop streams before signing out and back in. Start Polaris
   again and select **Recheck Setup**. If Polaris runs as a system service, its
   administrator may need to restart that service to refresh group membership.
6. Resolve any controller or graphics access checks through **Doctor & Support**.
   These checks verify device access; a working game stream is a separate test.

Docker group access gives that account administrator-level control over the
host. Grant it only to a trusted account. See Docker's
[post-installation instructions](https://docs.docker.com/engine/install/linux-postinstall/).

### Fedora

On a regular Fedora installation, add Docker's repository and install the engine:

```sh
sudo dnf config-manager addrepo --from-repofile https://download.docker.com/linux/fedora/docker-ce.repo
sudo dnf install docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin
sudo systemctl enable --now docker
```

If you already have a container engine or encounter a package conflict, review
the [Docker Fedora guide](https://docs.docker.com/engine/install/fedora/) before
replacing packages. The Spaces guide does not remove existing container software.

### Arch Linux

On a regular Arch installation, update the system and install Docker:

```sh
sudo pacman -Syu docker
```

If the update installed a new kernel, save your work and restart the host before
starting Docker. Signing out alone does not load the new kernel. Docker can fail
to start when the running kernel no longer has its matching network modules.
After restarting, continue with:

```sh
sudo systemctl enable --now docker
```

Use the [Arch Docker documentation](https://wiki.archlinux.org/title/Docker) for
distribution-specific configuration. Schedule the system update when you have
saved your work and stopped games.

### Ubuntu

Follow the [Docker Ubuntu guide](https://docs.docker.com/engine/install/ubuntu/)
to add Docker's repository key and package repository, then install the engine.
Complete those steps in a terminal on the Polaris host, then return to Spaces
and select **Recheck Setup**.

### Bazzite, SteamOS, and other system images

Regular Fedora or Arch package commands do not apply to these systems. The
preview does not automate their Docker installation. Use your distribution's
supported installation method, and do not disable filesystem protection to
follow instructions intended for another distribution.

## Prepare Spaces security support

On hosts with SELinux, the **Spaces security support** check verifies that
SELinux is enforcing and that the matching dedicated policies and input rule
are installed. A missing file, an older policy version, or a failed check keeps
first-space preparation unavailable. Hosts without SELinux only need the
matching Steam seccomp file included in the native package.

On a mutable Fedora installation:

1. Install the development tools used to compile against your host policy:
   ```bash
   sudo dnf install selinux-policy-devel container-selinux make
   ```
2. Finish your games, stop Spaces streams, and quit Polaris. If Polaris runs as a
   service, stop that service first.
3. Run the helper included in the matching native Polaris package:
   ```bash
   sudo -H /usr/bin/polaris-spaces-setup install
   ```
4. Reopen Polaris, return to **Spaces**, and select **Recheck Setup**.

The helper installs only the dedicated worker policy, reserved controller policy,
version marker, and reserved input rule. It reloads policy and udev rules without
changing SELinux enforcement or relabeling active controllers. It does not
install Steam, change graphics drivers, start Docker, or restart Polaris.
The browser checks readiness and links to these terminal instructions.

If interrupted, repeat the same command to finish the recorded operation. Existing
manually installed Spaces policies or an input rule with no ownership record are
reported for review; the helper will not silently replace them. For other SELinux
distributions, the host must supply the compatible SELinux development interfaces
and container reference policy. System image installations are not supported by
this helper in the preview.

You can inspect readiness with `polaris-spaces-setup status`. To remove only the
policies and input rule owned by the helper, stop Polaris and Spaces first, then
run `sudo -H /usr/bin/polaris-spaces-setup remove`. This does not delete player homes. Remove
owned policies before uninstalling the native package if you no longer need them.
Native package installation and removal do not activate or remove live SELinux
policy automatically.


## Prepare your first Steam home

Once this build offers an approved gaming runtime:

1. Complete the host checks above.
2. Under **Set up your first space**, enter a name such as **Living room**.
   If more than one runtime is offered, choose the variant for your graphics
   hardware. An NVIDIA variant names its required host driver version.
3. Select **Download and prepare**. Allow space and bandwidth for a download of
   several gigabytes. You can leave Spaces and return; the host keeps the job.
4. If the connection drops, select **Reconnect to setup** before retrying.
   **Retry setup** checks the original request and its saved home. It does not
   create a second home or copy another player's Steam login.
5. **Stop setup** is available while obtaining the runtime. Docker may retain
   verified layers for another attempt. Once Steam home preparation begins, wait
   for it to finish. Polaris keeps uncertain resources for recovery instead of
   deleting or adopting them.
6. If Polaris restarts, return to Spaces and explicitly retry the interrupted
   job. A restart does not automatically resume downloads or provisioning.

7. Once the home is prepared, choose its detected **Graphics card**, then select
   **Enable Spaces**. Polaris saves its configuration without starting a game.
   This first setup permits one Space at a time. Existing manually configured
   hosts retain their own simultaneous-session budgets.
8. Save any running game, then select **Restart Polaris and finish setup**.
   Restarting disconnects active streams. Reconnect to Polaris and return to
   **Spaces**.
9. Under **Default Space**, assign your paired Nova device to the saved Space.
   Refresh the host library in Nova, open the Space, and sign in through Steam
   Big Picture. Keyboard, controller and sound should be checked in your game.

If configuration is interrupted, retry the same graphics selection. Polaris
preserves the home and refuses to replace existing controller settings. A changed
or inaccessible GPU requires attention; Polaris does not silently choose another.
The native package must install its matching Steam seccomp policy. SELinux hosts
also need the dedicated Spaces worker and input policy installed with the
[terminal helper](#prepare-spaces-security-support). The browser does not change
SELinux policy.

**Steam home prepared** means storage has been saved. **Configuration saved**
means a restart is required. Neither is a successful game or controller test.
If setup repeatedly cannot finish, open **Doctor & Support** and retain the
existing player data while diagnosing the failure.

## Add a space on a configured host

1. Stop active space streams before changing assignments or creating a space.
2. In **Spaces**, select **Create a space** and give it a recognizable name.
3. If prompted, choose an existing Steam setup. Polaris reuses its runtime
   configuration; the new space gets separate storage and no copied Steam login.
4. Wait for creation to be confirmed. If the connection is interrupted, use
   **Check creation status** or **Retry creation**. Retrying checks the same
   request. Returning to Spaces in the same browser tab restores an unfinished
   request without submitting it again automatically. If browser storage is
   unavailable, keep the form open as instructed until the result is confirmed.
5. Under **Default Space**, choose that space for a paired device and select
   **Save assignment**. Wait for the saved assignment to be confirmed.
6. Refresh the host's game library in Nova, open the assigned space, and sign
   in through Steam Big Picture. Download a game and check picture, sound,
   controller buttons, and both sticks before a longer session.

A device needs permission to launch apps. Temporary guests cannot be assigned
these persistent spaces. Devices assigned to the same space share that space's
Steam login and saved data, and take turns streaming it. Assign separate spaces
for simultaneous players. Devices with more than one permitted Space can use **Change Space** in Nova.
To allow another Space, expand **Device Access** on its card and select the device.
Wait for the saved access to be confirmed before returning to Nova.

For simultaneous Steam play, use separate Steam accounts and ensure each player
has access to the game. Spaces do not change
[Steam's account and library-sharing rules](https://help.steampowered.com/en/faqs/view/054C-3167-DD7F-49D4).

## Spaces and streaming presets

A **space** selects a gaming environment on the Polaris host, including its
Steam sign-in and saves. A **streaming preset** in Nova saves stream settings,
such as resolution, frame rate, and bitrate. Switching a preset does not switch
Steam accounts or create a new space.

Nova offers frame rates up to **240 FPS** when the client display supports them.
Choose a rate your host can sustain while all intended Spaces are running.
A selectable rate is a target, not a measured result: the game, GPU, encoder,
network, decoder, and display all affect delivery. Start at 60 FPS, then increase
the target and check gameplay, sound, and frame pacing.

**This PC’s desktop and apps** opens the usual apps on the host. Select it under
Default Space to clear all Space access for that device and return to ordinary streaming.

## Compatibility and troubleshooting

Host package availability and Spaces validation are separate:

| Host packaging | Spaces evidence in this preview |
| --- | --- |
| Fedora RPM | Fresh Fedora 44 VM package, Docker, security setup, reboot, and removal checks passed without a GPU; an existing Fedora NVIDIA host has Steam, controller, and simultaneous-stream evidence |
| Arch package | Docker installation guidance; equivalent physical Spaces acceptance still needed |
| Ubuntu DEB | Docker installation guidance; equivalent physical Spaces acceptance still needed |
| SteamOS package | Native host packaging does not imply Spaces installation support on the system image |

| Spaces capability | Current boundary |
| --- | --- |
| NVIDIA encoding | Exercised in the development Steam runtime |
| AMD encoding | Do not infer Spaces support from native Polaris VA-API support; equivalent runtime acceptance is still needed |
| 60 FPS | Two 1080p streams completed 15 minutes; Shield Ethernet listening passed, while RP6 Wi-Fi audio underruns remain unresolved |
| 120 FPS | Two streams stayed connected through a 15 minute test with one at 120 FPS; uneven presentation and audio gaps remain unresolved |
| Browser Stream | Separate experiment; it does not yet open a space's isolated stream |
| Automatic recovery | Do not apply global host adjustments to a space; isolated telemetry and verified session-scoped repair are still needed |

If **Docker Engine** passes but **Polaris access to Docker** fails, confirm that
the system daemon is running and the Polaris process received the new group
membership. Docker Desktop, remote Docker contexts, and rootless Docker are not
the supported Spaces engine in this preview.

If a graphics or input check fails, use Doctor & Support and resolve the named
host permission or driver issue before trying another launch. Keep SELinux
enabled. Do not add privileged container flags or mount the entire device tree.

The [September 15 acceptance report](research/container-multiseat-acceptance-20260915.md)
records fresh Fedora installation, simultaneous 60 FPS streams, Shield listening,
and Space reopening. The [sustained streaming report](research/container-multiseat-sustained-streaming.md)
records earlier frame rate measurements. Both reports state their remaining limits.

If a space stops on its own or loses sound, retain the time of the failure and
the space name for diagnosis. A successful setup check is not evidence that
120 FPS, every game, or every GPU is reliable.

### Check Sound And Stuttering

For a comparison, set the device's display settings in Polaris to 1920 × 1080
at 60 FPS. In Nova, open **Stream Settings**, choose **Auto** for **Frame Rate**
and **Device Settings** for **Resolution**. Check the game during play with one
Space, then repeat with the other intended Spaces running.

If sound crackles, drops out, or falls behind:

1. Note the time, Space name, game, frame rate, bitrate, and whether the client
   uses Wi-Fi or Ethernet. Note whether picture or controls also paused.
2. Save your game before reconnecting. The current runtime ends the game session
   when its stream disconnects.
3. Compare the same game and settings using Ethernet on the same client, when
   available. Otherwise, try another access point. Change one thing at a time.
4. Repeat with heavy downloads, builds, and updates paused on the host. Then
   compare one Space with the intended number of simultaneous players.
5. If the problem remains, retain the observations for **Doctor & Support**.

A low average latency or zero reported video packet loss does not rule out brief
audio delivery pauses. The preview has recorded both host scheduling stalls and
delays on wireless client paths. A quiet wired comparison helps narrow the
investigation; it does not identify a specific router or prove every client is
reliable. Audio reliability remains under investigation.

See the [audio timing investigation](research/container-multiseat-audio-timing.md)
for measured results and their limits.

## Names, device access, and archiving

Give a space a player or room name, such as Alex or Living room. In **Spaces**,
use **Rename** on its card to change that name without changing its Steam account
or files. Refresh Library in Nova afterward to see the new name.

**Default Space** lists the handhelds, TVs, and computers paired with Polaris.
Choose a space for each device and save the assignment. **This PC’s desktop and
apps** clears all of that device’s Space grants and uses the host’s usual desktop session. Rename an unfamiliar device in
**Devices**. Nova’s **Streaming presets** change picture quality and performance;
they do not select a Steam account or space.

To archive a Space:

1. Stop space streams and wait for cleanup.
2. Select **Remove Space** on its card, then confirm the displayed space name.
3. The space moves to **Archived Spaces**. Devices lose access to it. Other allowed Spaces remain available;
   devices with none return to the host’s usual desktop and apps.

Removal keeps installed games, saves, settings, and Steam sign-in on the host.
It does not free disk space or delete Docker volumes. Select **Restore** under
**Archived Spaces** to use it again, then assign its devices. Restoring never
restores device permissions or assignments automatically. You can restore the
last archived Space or create another one using the retained runtime setup.

## Choose And Check Spaces In Nova

Nova opens the selected Space's game library. **Playing In** names the Space in
the same toolbar as the selected game, **Options**, and **System**. Select
**Change Space** to see the Spaces allowed for that paired device. Choosing
does not start a game or change another device's selection. The host remembers
each device's last choice across restarts; Default Space is used when there is
no saved permitted choice. Select **Steam Big Picture** to sign in or install
games, or select an installed game's poster to play it.

Older hosts without the Space library use a compatibility screen with
**Your Space** and **Open Space**. See the
[Nova Spaces guide](https://github.com/papi-ux/nova/blob/master/docs/spaces.md)
for the library, Play Setup, and compatibility flows.

**Ready To Play** means the Space is idle. **In Use** means another device is using
it. **Starting** and **Stopping** mean Nova must wait before opening. An active
Space owned by this device can offer **Resume Space** after checking its session.

The current gaming runtime ends its game session when the stream disconnects.
Reopening uses the same Steam home, installed games, and saved data, but starts a
new session. Save your game before disconnecting; returning to the running game
later is not supported by this runtime.

Finish your own stream and wait for cleanup before switching Spaces. Other devices
can keep streaming while you choose. Changing access or defaults still requires all
Space streams to stop because it reloads the host's permission catalog.

Status refreshes while Nova's Space screen is open and is checked again before
opening. If it cannot be verified, Nova keeps opening and switching unavailable
until a fresh check succeeds. Older hosts without this API retain the single-Space
flow. Library game settings stay associated with that game and environment on the device.

For administrators: catalogs with explicit Desktop Access use schema 4. Catalogs with extra Space access grants use schema 3. Archived-only
catalogs use schema 2; catalogs with neither use schema 1. Older builds reject new
schemas instead of guessing their permissions. Selections live in a private file
beside the catalog with a `.selections` suffix. A selection is never an access
grant: it is revalidated against the catalog before use. Keep the catalog,
selections, and volume backups together.

## Choose A Player And A Game In Nova

A Space is a saved gaming environment for a person or purpose. Name it **Alex’s Space**, **Family Space**, or another name your players recognize. Its name identifies the environment in Nova; it does not sign into a Steam account. Each Space keeps its own Steam sign-in, installed games, settings, and saves.

1. In Polaris, open **Spaces**, create a Space, and name it for the player.
2. Under that Space’s **Device Access**, allow the paired handhelds or TVs that may use it. A device can access more than one Space. One device can play in a given Space at a time.
3. In Nova, select your computer. **Spaces Available** identifies computers offering Spaces to your paired device.
4. Open **Library**, then use **Playing In** to choose your Space. The same Nova artwork library shows games installed in that environment.
5. Choose **Steam Big Picture** to sign in or install games in Steam Big Picture. After installing, return to Nova and refresh the library. Library reads are cached briefly, so a newly installed title may take up to 15 seconds to appear.
6. Choose a game, review **Play Setup**, and press **Play**. **Play In** shows the environments available to your device and whether that title can launch there. Choosing another player’s Space uses that Space’s Steam sign-in and saves.
7. End your stream before changing environments. Your selected Space is remembered for the next visit.

**Desktop** uses the host computer’s usual account and applications. Devices assigned to Spaces gain that choice only when the host owner enables **Desktop Access** in Polaris. Granting access to a Space alone does not grant desktop access.

The Steam library currently detects completed installations in the Space’s standard Steam home. Compatibility tools such as Proton are excluded. External library folders are not imported from host paths. If a library cannot be read, **Steam Big Picture** remains available and Nova does not substitute the desktop’s games. Older hosts retain the original **Open Space** screen until they support the Space library API.

Steam artwork is fetched through the paired Polaris host and cached separately from desktop artwork. Use **Options → Update Artwork Library** to check the selected Space's game artwork and retry missing or invalid images. **Steam Big Picture** keeps its bundled launcher artwork. Space artwork editing and pinned game shortcuts are not available in this first library version.

### Space status and networking

Polaris Space cards show **Available**, **Starting On**, **Playing On**, or
**Stopping On**, with the paired device name when known. Status refreshes while
Spaces is open. **Status Unavailable** means the host did not provide a current
activity report; it does not imply that a Space is free. Device Access shows
which devices may use a Space, not which Steam account is signed in.

Each running Steam Space has a private Docker address on its own bridge
network. Nova connects to the Polaris host address and selects a Space by name;
there is no extra IP address to enter for each player. Outbound connections
normally share the host network’s public internet address.
