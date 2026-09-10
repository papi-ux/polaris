# Install on Bazzite

The supported Polaris installation on Bazzite is the matching Fedora RPM layered
with `rpm-ostree`. Install the package, reboot, run host setup, and start the user
service. Bazzite 44 uses `Polaris-fedora44-x86_64.rpm` from the
[latest Polaris release](https://github.com/papi-ux/polaris/releases/latest).

The RPM installation is supported. Capture and game compatibility still depend
on the GPU, driver, and host session; [Validation status](#validation-status)
records those limits. The withdrawn standalone `Polaris-sysext-x86_64.raw` image
is not part of this installation process.

## Before you install

Use a terminal on the Bazzite host, initially in Desktop Mode. Check the Fedora
base and the booted deployment:

```bash
rpm -E %fedora
rpm-ostree status
```

Use an RPM matching that Fedora version and architecture. If no matching asset
exists in the release, wait for a compatible release rather than installing an
RPM built for another Fedora version. If Polaris is already installed, follow
[Update](#update) instead of the fresh-install command.

Polaris and Sunshine use the same default GameStream ports. Stop and disable the
Sunshine service you actually use before starting Polaris. Common unit names are
`sunshine.service`, `homebrew.sunshine.service`, and
`app-dev.lizardbyte.app.Sunshine.service`; inspect your user services first:

```bash
systemctl --user list-unit-files '*sunshine*' '*Sunshine*'
# Example for a native Sunshine user service:
systemctl --user disable --now sunshine.service
```

If you installed the withdrawn system extension, complete
[its removal](#system-extension-withdrawn) before layering the RPM.

## Install

### 1. Download and stage the package

For Bazzite 44 on x86_64:

```bash
rpm_name="Polaris-fedora44-x86_64.rpm"
curl --fail --location --output "./${rpm_name}" \
  "https://github.com/papi-ux/polaris/releases/latest/download/${rpm_name}" &&
sudo rpm-ostree install "./${rpm_name}"
```

Continue only after the transaction succeeds. Inspect the pending deployment:

```bash
rpm-ostree status
```

The downloaded RPM appears under `LocalPackages`. It is still layered onto the
host; `LayeredPackages` lists packages requested from repositories by name.
Local RPMs do not update automatically when Bazzite updates. See
[Bazzite's package-layering documentation](https://docs.bazzite.gg/Installing_and_Managing_Software/rpm-ostree/).

Save your work, then reboot separately:

```bash
systemctl reboot
```

### 2. Set up the host after reboot

Confirm the new deployment is booted, then run setup as your normal desktop user
through `sudo`:

```bash
rpm -q polaris
sudo -H polaris --setup-host
```

Follow any input-access instructions printed by setup. If it reports missing
input-group membership, complete [Controller and input group](#controller-and-input-group)
and reboot or sign out and back in before continuing. A successful root device
check alone does not establish that the running user service can use input.

Start the packaged service:

```bash
systemctl --user enable --now polaris
systemctl --user is-active polaris
```

Normal Private Stream and portal capture do not require a manual binary copy or
`cap_sys_admin`. Use [Optional DRM/KMS capture](#optional-drmkms-capture) only when
you intentionally need that backend.

### 3. Open the console and pair

- **Fresh install:** open `https://127.0.0.1:47990/#/welcome` and create the
  web account.
- **Upgrade or reinstall:** open `https://127.0.0.1:47990/#/login` with your existing
  account. Package changes preserve credentials, paired devices, settings, and
  the library in `~/.config/polaris`.

Pair Nova or Moonlight from the console. If you forgot the existing web password,
use [credential reset](troubleshooting.md#web-ui-credentials); an update does not
require repeating first-run signup.

## First stream

Start with a modest client profile such as 1920×1080 at 60 FPS. Select the launch
mode for the app you intend to use:

- **Private Stream:** Polaris starts a separate compositor for the game. The
  app, capture, and supported input devices must all belong to that session.
- **Mirror Desktop:** captures the logged-in desktop. A Desktop app stream in
  this mode proves desktop capture, not a private game session.

Check the active mode, capture path, and encoder in Mission Control after the
client connects. A preferred Private Stream setting does not prove the active
session used it. Stop and inspect the launch result if the mode is unexpected.

For Private Stream, let Polaris create and select its Wayland socket. Do not
export `WAYLAND_DISPLAY` manually or add display-switch scripts for the initial
setup. SHM/CPU fallback can still provide a working stream; it is a performance
characteristic, not proof of a failed session. See
[launch modes and capture paths](launch-modes.md).

## Headless boot and Deck images

To keep Polaris available after reboot, before desktop login, or across switches
between Desktop Mode and Game Mode:

```bash
sudo -H polaris --setup-host --enable-headless-boot
systemctl --user enable --now polaris
```

This enables lingering for your account and attaches the user service to
`default.target`. Run it through `sudo` from your normal account so setup targets
the right user. To undo that startup policy, use
`sudo -H polaris --setup-host --disable-headless-boot`.

Verify after reboot or a mode switch:

```bash
systemctl --user is-active polaris
journalctl --user -u polaris --since "10 minutes ago" --no-pager
```

Service availability and capture availability are separate. Mirror Desktop
needs a graphical session. Steam already running in the host's Game Mode can
also affect Private Stream or Gamescope Stream launch ownership; boot setup
does not resolve those conflicts. Follow
[Handhelds and Game Mode](handhelds.md) for the current capture limits.

If you use the machine as an always-available streaming host, review automatic
suspend in Plasma's power settings. Lingering keeps the user service alive; it
does not keep a suspended computer reachable. A disconnected SSH session or a
locked screen is not by itself evidence that Polaris crashed.

## Controller and input group

Run this if setup reports that the account needs the `input` group:

```bash
ujust add-user-to-input-group
```

Bazzite can define this group in `/usr/lib/group`, so a plain
`usermod -aG input` may not find it. The Bazzite command handles that layout.
Reboot after changing the group to ensure both your login and a lingering user
manager inherit it. Check from the new session:

```bash
id -nG
sudo -H polaris --setup-host
systemctl --user restart polaris
```

Account-database membership and the supplementary groups of an already-running
service are different. If input is still unavailable, inspect the actual service
process rather than repeatedly adding the account to the group:

```bash
polaris_pid="$(systemctl --user show polaris -p MainPID --value)"
if [ "$polaris_pid" -gt 0 ]; then
  grep '^Groups:' "/proc/${polaris_pid}/status"
fi
ls -l /dev/uinput /dev/uhid
```

Keep SELinux enforcing. If logs show an input denial after group access is
correct, report the denial and package version; do not disable SELinux or grant
world-writable device permissions.

## Update

Download the newer matching RPM, then replace the old local package request in
one transaction:

```bash
rpm_name="Polaris-fedora44-x86_64.rpm"
curl --fail --location --output "./${rpm_name}" \
  "https://github.com/papi-ux/polaris/releases/latest/download/${rpm_name}" &&
sudo rpm-ostree install --uninstall=polaris "./${rpm_name}"
```

Use this for an existing Polaris layer. A fresh install uses the command in
[Install](#install). The explicit replacement avoids the local-RPM
`cannot install both polaris-...` / `conflicting requests` error. Do not remove
unrelated layers or reset your configuration to resolve that error.

After a successful transaction, inspect `rpm-ostree status`, save your work, and
reboot. The booted deployment is marked `●`; a pending version above it is not
running yet. The dashboard may therefore show the old version until reboot.

After reboot:

```bash
rpm -q polaris
sudo -H polaris --setup-host
systemctl --user restart polaris
systemctl --user is-active polaris
```

If you previously installed a KMS runtime copy, refresh it using the next section
before restarting. A copy under `/usr/local` is outside the deployment and does
not change automatically with an RPM update or rollback. Check
`systemctl --user cat polaris` if the service still runs an older binary.

## Optional DRM/KMS capture

Use this section only for explicit DRM/KMS capture, including a controlled Game
Mode capture test. On composefs-backed Bazzite deployments, adding a capability
to the packaged binary under `/usr` can fail even as root. Use a writable runtime
copy for that backend:

```bash
systemctl --user stop polaris
polaris_binary="$(readlink -f "$(command -v polaris)")"
sudo install -D -m 0755 "$polaris_binary" /usr/local/bin/polaris-kms &&
sudo setcap cap_sys_admin+ep /usr/local/bin/polaris-kms &&
getcap /usr/local/bin/polaris-kms
```

Continue only if the copy succeeded and `getcap` reports `cap_sys_admin=ep`:

```bash
printf '[Service]\nExecStart=\nExecStart=/usr/local/bin/polaris-kms\n' \
  | systemctl --user edit --stdin --drop-in=10-bazzite-kms.conf polaris
systemctl --user daemon-reload
systemctl --user start polaris
```

Repeat the copy and capability steps after every package update or rollback.
`/usr/local` maps into writable `/var/usrlocal`, which is shared across
rpm-ostree deployments. The capability grants access needed by KMS; it does not
select a capture backend or validate Game Mode streaming.

To return to the packaged executable, stop Polaris, remove only the
`10-bazzite-kms.conf` drop-in you created, reload the user manager, and restart:

```bash
systemctl --user stop polaris
rm -f ~/.config/systemd/user/polaris.service.d/10-bazzite-kms.conf
systemctl --user daemon-reload
systemctl --user start polaris
sudo rm -f /usr/local/bin/polaris-kms
```

Other custom service overrides may still select another binary; inspect
`systemctl --user cat polaris` rather than removing unrelated overrides.

## Roll back

Select the previous deployment in the boot menu, or stage it explicitly:

```bash
sudo rpm-ostree rollback
rpm-ostree status
```

Save your work and reboot. Refresh any optional KMS copy from the now-booted
package. Your Polaris configuration and other `/var` contents are shared across
deployments; deployment rollback does not restore those files.

## Uninstall

```bash
systemctl --user disable --now polaris
sudo rpm-ostree uninstall polaris
rpm-ostree status
```

Reboot after the transaction succeeds. If you used the optional KMS copy, remove
that copy and its dedicated drop-in. Keep `~/.config/polaris` to preserve your
account, paired devices, and settings for reinstalling.

You can then re-enable the Sunshine service you used previously. Run one host
at a time because their default ports overlap.

## System Extension (withdrawn)

The standalone `Polaris-sysext-x86_64.raw` release image was withdrawn on
September 5, 2026 because its runtime dependencies were incomplete. It remains
withdrawn. Validation of newer private candidates does not make cached release
images usable, and the RPM's supported status does not promote the extension.

An old extension at `/var/lib/extensions/polaris.raw` persists across deployment
rollbacks. If the host reaches Desktop Mode, a TTY, or SSH, remove that specific
extension before installing the RPM:

```bash
systemctl --user disable --now polaris 2>/dev/null || true
sudo systemd-sysext unmerge &&
sudo rm -f /var/lib/extensions/polaris.raw &&
sudo systemd-sysext refresh
systemctl --user daemon-reload
sudo systemd-sysext status
```

The final status must not list `polaris`. Unmerge temporarily affects all active
system extensions; refresh restores the remaining installed extensions. For a
host that cannot reach login, ask in the
[Polaris Matrix room](https://matrix.to/#/#polaris:papi-ux.com) with the boot
symptom before attempting recovery. Switching deployments alone does not remove
an extension stored in `/var`.

## Validation status

| Area | Current scope |
|:-----|:--------------|
| Fedora 44 RPM on Bazzite | Supported installation, with explicit update and rollback steps |
| NVIDIA / KDE Plasma Wayland Desktop Mode | Service, pairing, capture, input isolation, and reconnect evidence exists; a successful desktop stream does not establish private-game acceptance |
| Bazzite Deck / Steam Game Mode | Boot-independent service setup is available; end-to-end Game Mode game streaming still needs hardware validation |
| AMD / Intel Bazzite hosts | Use the matching Fedora RPM; driver-specific capture, encoding, and Game Mode behavior need additional hardware coverage |
| Standalone system extension | Withdrawn; separate package, SELinux, lifecycle, and physical validation gates remain |
| Container multiseat | Separate development work; production activation remains off |

The earlier NVIDIA Desktop Mode baseline used `bazzite-nvidia-open:stable`
`44.20260430`. It was a Plasma Desktop image, not a Game Mode-capable Deck image.
More recent candidate testing does not certify every released package, GPU, or
Steam launch path. See [Compatibility](compatibility.md) and
[the system-extension validation requirements](../scripts/validation/bazzite/README.md).

## Troubleshooting

- **Host disappears after reboot or leaving Desktop Mode:** verify the user
  service and [headless boot setup](#headless-boot-and-deck-images), then check
  whether the computer suspended or the network disconnected.
- **Old version after update:** check the booted deployment, then the service's
  `ExecStart` and any `/usr/local` copy. Do not repeat first-run signup.
- **Black screen or unexpected Mirror Desktop:** inspect the active launch mode
  and capture decision. A physical connector name alone does not diagnose an
  app-routing failure; use the session's actual backend and compositor records.
- **KMS capability warning while portal/private capture works:** use the normal
  installation. Apply the optional capability only for explicit KMS capture.
- **Input reaches the physical desktop during Private Stream:** stop the session
  and report the input-routing details as an isolation issue.

For a report, include the Bazzite image/Fedora version, Desktop or Game Mode,
GPU/driver, Polaris package version, active launch/capture mode, client and
requested resolution/FPS. These local checks help identify the installed and
running components:

```bash
rpm-ostree status
rpm -q polaris
systemctl --user status polaris --no-pager
systemctl --user cat polaris
journalctl --user -u polaris --since "10 minutes ago" --no-pager
command -v polaris grim labwc wlr-randr
```

Review logs before sharing them and remove credentials, pairing material, and
private network or account details.
