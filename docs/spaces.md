# Spaces

A Space is a separate Steam sign-in, game library and set of saves on one Linux
gaming PC. Two players can use their own Spaces at the same time, one person
can keep a Space on a server without a monitor, or a handheld and a TV can share
one Space at different times.

Spaces are a preview. Two limits shape everything below: the host runs one
Space at a time, and the gaming runtime is not published for download yet, so
the catalog in **Host Setup** stays empty until it is.

Spaces are optional. If you stream your usual desktop and games today, keep
using the Library in Nova; nothing here is required for that, and you do not
need Docker. [Spaces or regular streaming](spaces-or-regular.md) puts the two
side by side.

![Spaces with player cards and a default Space for a handheld](images/spaces/spaces-overview.png)

Example with sample player and device names.

## What you need

| Component | Purpose |
| --- | --- |
| Polaris RPM, Arch package or DEB | Runs the host, pairing, settings and streaming |
| Docker Engine on that host | Runs the isolated gaming environments |
| Polaris gaming runtime image | Steam and the software that runs inside each Space, downloaded by Docker from GitHub Container Registry |
| Nova on your device | Opens the stream and sends your controls |

Steam, its 32-bit libraries and the gaming userspace live inside the runtime
image, so the host does not need Steam installed. The host still needs Polaris,
Docker, its GPU driver and input permissions. An NVIDIA image names the host
driver version it was built for; the current preview image targets NVIDIA
610.57.04.

The native packages ship the setup UI, the controller, the host policy files
and the `polaris-spaces-setup` helper. There is no supported image that runs
the whole Polaris host inside Docker, and no Unraid template.

## Host Setup

Open **Spaces** in the Polaris console, beside **Devices**, and expand **Host
Setup**. It runs eight checks on the PC hosting Polaris, whichever device you
opened the page from, and each failing check links the section of this guide
that fixes it. Select **Recheck Setup** after every terminal step. A configured
host keeps the section collapsed.

Two checks can be fixed from the page itself. **Polaris access to Docker**
offers **Give Polaris access to Docker**, and **Spaces security support** offers
**Install security support**. Either button opens a password prompt on the
screen of the PC that runs Polaris, so someone signed in at that PC's desktop
approves the change. From another device you can start the request, but only
someone at the PC can approve it. The terminal steps under each check do the
same thing.

Polaris asks for one change at a time and closes the prompt if nobody approves
it within five minutes. While a change runs, Space launches and changes to
Spaces wait. Polaris does not ask, and says why, on an image based system, when
the native package's helper or its polkit policy is missing, when nobody is
signed in at the PC's desktop, or while a Space, a stream or the first Space
setup is still active.

## Prepare Docker from Spaces

If **Docker Engine** passes and **Polaris access to Docker** needs attention,
select **Give Polaris access to Docker** and approve the password prompt on the
Polaris host's screen. Polaris starts the system Docker service, sets it to
start with the PC, and adds the account Polaris runs as to the `docker` group. A Polaris that is already running
keeps the groups it started with, so the check then asks you to restart the PC.
After the restart, select **Recheck Setup**.

From a terminal on the Polaris host instead:

1. If **Docker Engine** needs attention, install it with your distribution's
   steps below.
2. Start the system Docker service, then grant Docker access to the Linux
   account running Polaris. For a service installation that account may differ
   from your terminal account; grant it to the service account.
3. Save your work, stop streams and restart the PC, so Polaris starts with the
   new group.
4. Select **Recheck Setup**.

Docker group access gives that account administrator-level control over the
host. Grant it only to a trusted account; Docker's
[post-installation instructions](https://docs.docker.com/engine/install/linux-postinstall/)
explain why.

If **Docker Engine** passes but **Polaris access to Docker** fails, confirm that
the system daemon is running and that the Polaris process received the new
group membership. Docker Desktop, remote Docker contexts and rootless Docker
are not the supported engine in this preview.

### Fedora

```sh
sudo dnf config-manager addrepo --from-repofile https://download.docker.com/linux/fedora/docker-ce.repo
sudo dnf install docker-ce docker-ce-cli containerd.io docker-buildx-plugin docker-compose-plugin
sudo systemctl enable --now docker
```

If you already have a container engine or hit a package conflict, read the
[Docker Fedora guide](https://docs.docker.com/engine/install/fedora/) before
replacing packages. Spaces never removes existing container software.

### Arch Linux

```sh
sudo pacman -Syu docker
```

If the update installed a new kernel, save your work and restart the host
before starting Docker; signing out alone does not load the new kernel, and
Docker can fail to start when the running kernel no longer has its network
modules. Then:

```sh
sudo systemctl enable --now docker
```

The [Arch Docker documentation](https://wiki.archlinux.org/title/Docker) covers
distribution-specific configuration.

### Ubuntu

Follow the [Docker Ubuntu guide](https://docs.docker.com/engine/install/ubuntu/)
to add Docker's repository key and package repository, then install the engine.
Return to Spaces and select **Recheck Setup**.

### Bazzite, SteamOS and other system images

The package commands above do not apply, and the preview does not automate
Docker on these systems. Use your distribution's supported installation method,
and do not disable filesystem protection to follow instructions written for
another distribution.

## Gaming runtime account

The current runtime requires the Linux account running Polaris to have user and
group ID 1000, which is the first account created on most installations. The
**Gaming runtime account** check says whether this host qualifies.

Do not change an existing account's ID to work around this; it breaks file
ownership across the whole account. Run Polaris under an account that already
has that ID, or wait for a runtime that lifts the limit.

## Controller access

Polaris creates virtual controllers, keyboards and mice for a Space through
`/dev/uinput` and `/dev/uhid`. When **Controller and input access** fails,
run the host setup that grants them:

```sh
sudo -H polaris --setup-host
```

then restart Polaris and select **Recheck Setup**. **Doctor & Support** shows
the same access with its reason.

## Graphics access

A Space needs a render node the Polaris account can open. When **Graphics
device access** fails, check that the GPU driver is installed and that the
account is in the group that owns `/dev/dri/renderD*` (usually `render` or
`video`); **Doctor & Support** names the node it tried. Hardware encoding is
checked again when the Space starts.

## Prepare Spaces security support

On hosts with SELinux, the **Spaces security support** check verifies that
SELinux is enforcing and that the dedicated policies and input rule are
installed at the matching version. Hosts without SELinux need only the Steam
seccomp file, which the native package includes.

On a mutable Fedora installation:

1. Install the tools that compile against your host policy, in a terminal on
   the Polaris host:
   ```sh
   sudo dnf install selinux-policy-devel container-selinux make
   ```
2. Finish your games and stop Space streams.
3. Select **Install security support** on the check and approve the password
   prompt on the Polaris host's screen. Polaris stays open, and the check is
   read again when the helper finishes.

Without anyone at the host's desktop, run the helper from a terminal instead:

1. Quit Polaris. If it runs as a service, stop the service first.
2. Run the helper from the native package:
   ```sh
   sudo -H /usr/bin/polaris-spaces-setup install
   ```
3. Start Polaris, return to **Spaces** and select **Recheck Setup**.

The helper installs the worker policy, the reserved controller policy, a
version marker and the reserved input rule. It reloads policy and udev rules
without changing enforcement or relabeling controllers, and it does not
install Steam, change drivers, start Docker or restart Polaris. If it is
interrupted, run the same command again. Policies you installed by hand, and an
input rule that differs from the packaged one, are reported with the command
that clears them rather than replaced. An input rule identical to the packaged
one is adopted.

If the helper refuses, its message names what it found and prints the command
that clears it:

- **"Spaces setup found SELinux modules it does not manage"** lists each copy of
  a module with one of the helper's names that sits at another priority, is
  disabled or was installed another way, for example a copy installed by hand,
  and prints the command that removes those copies, such as
  `sudo semodule -X 400 -r polaris_multiseat_input polaris_nvidia_worker`. Run
  it, check `sudo semodule -lfull | grep polaris_`, then run the install again.
  libsemanage may print "Failed!" while removing; trust the list, not the
  message.
- **"Spaces setup found SELinux modules at priority 200 that it has no record
  of installing"** means the policies stayed installed after the helper's
  records in `/var/lib/polaris/spaces-security` were deleted. Remove them with
  the printed command and run the install again.
- **"Quit Polaris and stop Spaces streams before changing security setup."**
  names each Polaris process still running as `name (pid N)`, a second
  instance included, a Space that is still running with its process count,
  and the Spaces input devices still present. `systemctl status PID` shows
  which service started a process. Stop it and retry. From the button the
  message starts **"Stop Spaces streams and quit any other Polaris"**: the
  Polaris that asked stays open, and everything else still counts.
- **"is not owned by this setup and differs from the rule this package ships"**
  means `/etc/udev/rules.d/97-polaris-multiseat-input.rules` was placed there by
  hand or by an older build. Move it aside as the message shows and run the
  install again. A rule identical to the one the package ships is adopted
  instead: the install says so, records it as its own, and `remove` deletes it
  later like any rule the helper installed.
- **"changed after this setup installed it"** or **"is missing"** means a
  policy or the input rule the helper installed was changed or deleted since.
  The message prints the command that restores the installed copy from
  `/var/lib/polaris/spaces-security`. Run it, then the same helper command
  again.

`polaris-spaces-setup status` shows readiness. `sudo -H /usr/bin/polaris-spaces-setup remove`
removes only what the helper owns, after Polaris and every Space have stopped;
it never deletes player homes. Remove the policies before uninstalling the
package if you no longer need them: package removal alone leaves them installed with
nothing left to remove them, and [Uninstall Polaris, or start over](uninstall.md)
has the full order. Package installation and removal never
change live SELinux policy. Other SELinux distributions need the compatible
development interfaces and the container reference policy; system images are
not supported by the helper in the preview.

## Download the gaming runtime

The **Gaming runtime** check picks the runtime this PC needs from the ones this
Polaris build approves: the NVIDIA runtime built for the NVIDIA driver loaded on
this PC, otherwise the runtime for AMD and Intel graphics. It asks Docker for
that exact image and verifies it against the build. Downloading it here is
optional; preparing your first Space downloads it too.

- **Waiting for runtime** means this build has no approved gaming runtime yet.
  Nothing on this PC can change that, so the check stays out of the count.
- **Not downloaded** means the runtime this PC needs is not on it yet. Once the
  host checks above pass, select **Download**. The download is several
  gigabytes and shows no percentage. You can leave Spaces and come back; the
  host keeps the download running. **Stop download** ends it, and Docker keeps
  verified layers for the next attempt.
- **Checked** means the runtime is downloaded and verified. Preparing your
  first Space then starts without downloading it again.
- **Needs attention** without a button means no runtime in this build fits
  this PC. When the check names an NVIDIA driver version, install that version,
  restart the PC, then recheck. When the build's runtime is for other graphics
  than this PC has, wait for a build that includes one for yours.
- **Needs attention** with **Retry download** means Docker did not finish
  answering, or holds an image under the approved reference that does not
  match this build. Retry. If it keeps failing, check that Docker is running
  and open **Doctor & Support**.

A download runs on the host like first-Space setup, one job at a time. It never
prepares a Steam home or changes Spaces configuration, and Polaris never
removes or replaces an image. If Polaris restarts during a download, select
**Download** again; Docker reuses the layers it kept. On a host whose Spaces
are already configured, the check still reports the runtime but offers no
download.

## Prepare your first Space

Once the host checks pass and a runtime is offered:

1. Under **Set up your first space**, enter a name such as **Living room**.
   If more than one runtime is offered, choose the variant for your graphics
   hardware; an NVIDIA variant names the host driver it needs.
2. Select **Download and prepare**. If the **Gaming runtime** check shows
   **Checked**, preparation starts without a download. Otherwise the download
   is several gigabytes. You can leave Spaces and come back; the host keeps the
   job. Polaris cannot show a percentage for it.
3. **Stop setup** is available while the runtime downloads; Docker keeps
   verified layers for the next attempt. Once Steam home preparation begins,
   wait for it to finish.
4. When the home is prepared, choose its **Graphics card** and select **Enable
   Spaces**. Polaris saves its configuration without starting a game. This
   first setup allows one Space at a time; hosts configured by hand keep their
   own budget.
5. Save any running game, then select **Restart Polaris and finish setup**.
   Restarting disconnects every stream. Reconnect and return to **Spaces**.
6. Under **Default Space**, assign your paired Nova device to the new Space.

**Steam home prepared** means storage was saved. **Configuration saved** means
a restart is still required. Neither is a game or controller test yet; that
comes in [Play in a Space](#play-in-a-space).

## Recover an interrupted setup

- **Reconnect to setup** re-reads the job without changing it. Use it first
  after a dropped connection.
- **Retry setup** checks the original request and its saved home. It never
  creates a second home or copies another player's Steam sign-in.
- If Polaris restarts, the job does not resume on its own. Return to Spaces
  and retry it.
- If the runtime you started with is no longer offered by this build, the job
  cannot be retried; your player data stays where it is.
- If the setup journal could not be secured, Spaces shows **recovery
  required** and names the retained image and reference. Polaris keeps
  uncertain resources for recovery instead of deleting or adopting them.
  Restart Polaris after saving your work; if it persists, open **Doctor &
  Support** and keep the existing player data while diagnosing.
- If no accessible graphics card matches the runtime, the prepared home waits.
  Fix [graphics access](#graphics-access) and recheck.

Configuration is retried with the same graphics selection. A changed or
inaccessible GPU needs attention; Polaris does not silently choose another.

## Add a Space on a configured host

1. Stop every Space stream. Creating, renaming, removing or restoring a Space
   and changing any device's access all reload the host's Space catalog, so
   they wait for the streams to end.
2. Select **Create a space** and give it a recognisable name, a player or a
   room.
3. If asked, choose an existing Steam setup. The new Space reuses its runtime
   configuration and gets its own storage and no copied sign-in.
4. Wait for the creation to be confirmed. After a dropped connection use
   **Check creation status** or **Retry creation**; both check the same
   request. Returning to Spaces in the same browser tab restores an unfinished
   request without submitting it again.
5. Give a device access, below, then refresh the library in Nova.

## Give a device access

- **Default Space** on the Spaces page lists the handhelds, TVs and computers
  paired with Polaris. Choose the Space each device opens first and select
  **Save assignment**. Devices assigned to the same Space share its sign-in and
  saves and take turns streaming it; give simultaneous players separate Spaces
  and separate Steam accounts.
- **Device Access** on a Space card allows more devices into that Space. A
  device with more than one permitted Space picks between them in Nova with
  **Change Space**.
- **Desktop** is this PC's usual desktop and apps. Under Default Space it
  clears all of a device's Space access; under **Desktop Access** it becomes
  one more choice beside the device's Spaces.
- A device needs permission to launch apps; temporary guests cannot be
  assigned a Space. Rename an unfamiliar device in **Devices**.

Spaces do not change
[Steam's account and library sharing rules](https://help.steampowered.com/en/faqs/view/054C-3167-DD7F-49D4).

## Play in a Space

1. Open the host in Nova. **Playing in** in the library toolbar names your
   Space; **Change Space** lists the others this device may use. With one
   permitted Space the library opens straight into it, and the host remembers
   each device's last choice.
2. Select **Steam Big Picture**, sign in and install a game. A Space's name is
   a label, not proof of which Steam account is signed in; check or switch the
   account inside Steam.
3. Return to the library and refresh it; a newly installed title can take up
   to 15 seconds to appear. Choose a game and press **Play**. Start at 60 FPS,
   check picture, sound and both sticks, then raise the target.

Each Space shows a status: ready, starting, playing (yours), in use (another
device), stopping, or unavailable when the host cannot offer it. A Space that
reads ready but cannot open because the host is at its limit says so before
you press, and a refused launch tells you what to change.

**Save before you leave.** In this preview, disconnecting ends the Space's
running game. **Leave Space** asks you to confirm and keeps saves, installed
games and the Steam sign-in; a dropped connection ends the session the same
way. **Resume** is offered only for a session this device owns and does not
promise a disconnected game kept running. Other Spaces keep running.

Nova's **Play Setup** saves resolution and frame rate on the device for the
next Space launch. Nova offers up to 240 FPS where the display supports it; a
selected rate is a target the host must sustain with every intended Space
running, so start at 60 and raise it. Switching a streaming preset never
switches accounts or Spaces.

## Rename, remove and restore

**Rename** on a Space card changes its name without touching its account or
files; refresh the library in Nova afterwards.

**Remove Space** asks what happens to the Space's games and saves. Other Spaces
stay available either way.

- **Archive** is already selected. Devices lose access, and installed games,
  saves, settings and the Steam sign-in stay on the host, so archiving frees no
  disk space. **Restore** under **Archived Spaces** brings the Space back
  without its device access; assign devices again.
- **Remove for good** deletes the Space with its installed games, saves,
  settings and sign-in, and frees their disk space. Type the Space's name
  exactly as it is shown to confirm. It cannot be undone or restored. An
  archived Space offers **Remove for good** beside **Restore**.

Like every change to Spaces, removing for good needs every Space stream to end
first. Polaris deletes only the storage it made for that Space, and only
through Docker: if the storage is not the one Polaris created, or Docker does
not answer, nothing is removed and the page says why. If Docker stops partway,
the Space stays under **Archived Spaces**, the page names the Docker volume
that may still hold its games and saves, and **Remove for good** again
finishes the job. The last Space can be archived but not removed for good,
because Polaris makes a new Space from an existing one; create another Space
first.

## Check sound and stuttering

If sound crackles or drops, note the time, the Space, the game, the frame rate,
the bitrate and whether the device is on Wi-Fi or Ethernet, and whether picture
or controls paused too. Compare the same game over Ethernet or another access
point, with downloads and builds paused on the host, changing one thing at a
time, and compare one Space with the intended number of players. Low latency
and zero reported packet loss do not rule out brief audio pauses; the preview
has recorded both host scheduling stalls and wireless delays. Keep the notes
for **Doctor & Support**.

## When something goes wrong

- A refused launch in Nova names the reason and the fix: the Space is in use
  on another device, this device already has a Space running, every Space
  slot or the encoder is taken, no Space is assigned or selected, or the
  assignment changed. Do what it says, then try again.
- A failing host check links its section above. **Doctor & Support** covers
  controller and graphics access with the host's own evidence.
- If a Space stops on its own or loses sound, keep the time and the Space name
  for diagnosis. A passing setup check does not prove that 120 FPS, every
  game or every GPU is reliable.
- Keep SELinux enabled, and do not add privileged container flags or mount
  the whole device tree.

## Preview limits

One Space at a time, no runtime download until an image is published, handheld
audio still under investigation, NVIDIA exercised and AMD not yet, Docker on
system images not automated. The measured results and the exact boundaries are
in the [preview status report](research/container-multiseat-preview-status.md),
which links the acceptance and audio reports it summarises.
