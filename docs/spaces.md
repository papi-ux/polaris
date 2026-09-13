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

The native host packages and gaming runtime images serve different purposes.
This preview does not provide a supported image for running the entire Polaris
host inside Docker, or an Unraid installation template.

## Preview status

The Spaces tab has host prerequisite checks, Docker installation guidance, and
management for configured Steam spaces. Creating additional spaces from an
existing Steam setup is supported by the development backend.

**First space bootstrap and automatic download of a verified gaming runtime are
still being integrated.** A fresh host cannot yet complete the entire setup from
this page. The page keeps that step incomplete even when Docker is working.
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
