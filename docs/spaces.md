# Spaces

Spaces give each player their own Steam sign-in, installed games, saves, and
settings on one Linux gaming host. Two players can use separate spaces at the
same time. A single player can also use a space on a gaming server without a
monitor, or share one space between a handheld and a TV at different times.

Spaces are optional. If you already stream your usual desktop and games, keep
using the Library in Nova. You do not need Docker for ordinary streaming.

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
budget. Registry publication and clean-host package acceptance remain release gates.
The current runtime also requires the Polaris service account to use UID and
GID 1000. Do not change an existing Linux account's identity to work around this
preview limitation.

## Prepare Docker from Spaces

1. Open the Polaris web interface and select **Spaces**, beside **Devices**.
2. Under **Set up this host**, select **Recheck setup**. Checks run on the PC
   hosting Polaris, even if you opened the page on a phone or another computer.
3. If Docker Engine needs attention, expand **Install Docker step by step**.
   Run the displayed commands in a terminal on the Polaris host. Approve package
   installation with your administrator password in that terminal.
4. Expand **Start Docker and grant access**. Start the system Docker service,
   then grant Docker access to the Linux account running Polaris. The displayed
   access command uses the service's numeric user ID, rather than assuming that
   your terminal account is the same user.
5. Save your work and stop streams before signing out and back in. Start Polaris
   again and select **Recheck setup**. If Polaris runs as a system service, its
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

Spaces presents the repository key, package repository, and installation steps
from the [Docker Ubuntu guide](https://docs.docker.com/engine/install/ubuntu/).
Complete those steps in order, then return to Spaces and recheck access.

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
   sudo -H polaris-spaces-setup install
   ```
4. Reopen Polaris, return to **Spaces**, and select **Recheck setup**.

The helper installs only the dedicated worker policy, reserved controller policy,
version marker, and reserved input rule. It reloads policy and udev rules without
changing SELinux enforcement or relabeling active controllers. It does not
install Steam, change graphics drivers, start Docker, or restart Polaris.
The browser only checks readiness and shows these terminal commands.

If interrupted, repeat the same command to finish the recorded operation. Existing
manually installed Spaces policies or an input rule with no ownership record are
reported for review; the helper will not silently replace them. For other SELinux
distributions, the host must supply the compatible SELinux development interfaces
and container reference policy. System image installations are not supported by
this helper in the preview.

You can inspect readiness with `polaris-spaces-setup status`. To remove only the
policies and input rule owned by the helper, stop Polaris and Spaces first, then
run `sudo -H polaris-spaces-setup remove`. This does not delete player homes. Remove
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
9. Under **Device access**, assign your paired Nova device to the saved Space.
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
5. Under **Device access**, choose that space for a paired device and select
   **Save assignment**. Wait for the saved assignment to be confirmed.
6. Refresh the host's game library in Nova, open the assigned space, and sign
   in through Steam Big Picture. Download a game and check picture, sound,
   controller buttons, and both sticks before a longer session.

A device needs permission to launch apps. Temporary guests cannot be assigned
these persistent spaces. Devices assigned to the same space share that space's
Steam login and saved data, and take turns streaming it. Assign separate spaces
for simultaneous players. In this preview each device has one assigned space;
an owner-controlled choice between multiple allowed spaces is still planned.

For simultaneous Steam play, use separate Steam accounts and ensure each player
has access to the game. Spaces do not change
[Steam's account and library-sharing rules](https://help.steampowered.com/en/faqs/view/054C-3167-DD7F-49D4).

## Spaces and streaming presets

A **space** selects a gaming environment on the Polaris host, including its
Steam sign-in and saves. A **streaming preset** in Nova saves stream settings,
such as resolution, frame rate, and bitrate. Switching a preset does not switch
Steam accounts or create a new space.

**Standard streaming** opens the usual apps on the host. Select it in a device's
assignment to return that device to ordinary streaming.

## Compatibility and troubleshooting

Host package availability and Spaces validation are separate:

| Host packaging | Spaces evidence in this preview |
| --- | --- |
| Fedora RPM | Fedora with NVIDIA has retained physical Steam, controller, and simultaneous-stream evidence |
| Arch package | Docker installation guidance; equivalent physical Spaces acceptance still needed |
| Ubuntu DEB | Docker installation guidance; equivalent physical Spaces acceptance still needed |
| SteamOS package | Native host packaging does not imply Spaces installation support on the system image |

| Spaces capability | Current boundary |
| --- | --- |
| NVIDIA encoding | Exercised in the development Steam runtime |
| AMD encoding | Do not infer Spaces support from native Polaris VA-API support; equivalent runtime acceptance is still needed |
| 120 FPS | Short simultaneous gameplay checks passed; a longer audio-dropout failure remains unresolved |
| Browser Stream | Separate experiment; it does not yet open a space's isolated stream |
| Automatic recovery | Do not apply global host adjustments to a space; isolated telemetry and verified session-scoped repair are still needed |

If **Docker Engine** passes but **Polaris access to Docker** fails, confirm that
the system daemon is running and the Polaris process received the new group
membership. Docker Desktop, remote Docker contexts, and rootless Docker are not
the supported Spaces engine in this preview.

If a graphics or input check fails, use Doctor & Support and resolve the named
host permission or driver issue before trying another launch. Keep SELinux
enabled. Do not add privileged container flags or mount the entire device tree.

If a space stops on its own or loses sound, retain the time of the failure and
the space name for diagnosis. A successful setup check is not evidence that
120 FPS, every game, or every GPU is reliable.
