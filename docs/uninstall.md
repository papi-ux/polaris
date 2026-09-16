# Uninstall Polaris, or start over

Two reasons bring people here: you are done with Polaris, or you want a clean slate before
installing it again. The steps are the same. The last one decides which of the two you get.

The package manager removes what the package installed. It does not remove what Polaris created
while running, what host setup wrote outside the package, or what you added yourself. This page
lists all of it, in the order that works.

## Before you start

1. Finish your games and end every stream from the client.
2. Quit Polaris. If it runs as a service:
   ```bash
   systemctl --user disable --now polaris
   ```
   If you start it from the desktop or the tray, quit it from the tray icon.
3. Check that nothing is left running:
   ```bash
   pgrep -a polaris
   ```
   It should print nothing. A second instance you forgot about, for example a validation build,
   counts too.

## 1. Remove the Spaces security setup first

Do this while the package is still installed. The helper is a package file, and nothing else
knows how to remove the policies it installed.

```bash
sudo -H /usr/bin/polaris-spaces-setup remove
```

It removes only the SELinux policies and the input rule it installed, and it never touches player
homes. If it refuses, [Prepare Spaces security support](spaces.md#prepare-spaces-security-support)
explains each message.

Docker keeps the gaming runtime image and each Space's Steam home after the package is gone, and
Polaris never deletes a home on its own. The homes are Docker volumes with opaque names, listed by
`docker volume ls` next to your other volumes. Leave them if you might come back; the games and
saves live there. Remove one with `docker volume rm` only when you are sure which Space it belongs
to and want its games and saves gone for good. The runtime image is safe to remove at any time:

```bash
docker image ls 'ghcr.io/papi-ux/polaris-worker-steam'
```

Hosts without Spaces skip this section.

## 2. Turn off boot start, if you enabled it

Also before the package goes, because it needs the binary:

```bash
sudo -H polaris --setup-host --disable-headless-boot
```

If the package is already gone, remove the boot hook by hand instead:

```bash
rm -f ~/.config/systemd/user/default.target.wants/polaris.service
```

Lingering stays on because other services may rely on it. Turn it off yourself if nothing else
needs your session at boot:

```bash
loginctl disable-linger "$USER"
```

## 3. Remove the package

Fedora:

```bash
sudo dnf remove polaris
```

Ubuntu:

```bash
sudo apt remove polaris
```

Arch Linux:

```bash
sudo pacman -R polaris
```

SteamOS:

```bash
sudo steamos-readonly disable
sudo pacman -Rns polaris
sudo steamos-readonly enable
```

Bazzite:

```bash
sudo rpm-ostree uninstall polaris
```

Then reboot into the new deployment.

The package takes its binaries with it, including `polaris-spaces-setup`, plus the user service
unit, the udev rules and modules-load configuration under `/usr/lib`, the desktop entries and
`/usr/share/polaris`. The `uinput` and `uhid` kernel modules stay loaded until the next reboot,
which is harmless. The KMS capture capability lives on the binary and leaves with it.

## 4. What the package does not take with it

Check each of these. On a host that only ever ran the packaged Polaris, most of them are absent.

- **Rules copied into `/etc`.** Older host setups copied the udev rules and modules-load
  configuration into `/etc`. The package manager does not know about those copies.
  ```bash
  sudo rm -f /etc/udev/rules.d/60-polaris.rules /etc/modules-load.d/60-polaris.conf
  sudo udevadm control --reload-rules
  ```
- **`/usr/share/polaris` still present.** The package manager leaves the folder when something
  else wrote into it, for example web assets deployed from a source build or an `.rpmsave` file.
  Once the package is gone, remove it:
  ```bash
  sudo rm -rf /usr/share/polaris
  ```
- **The Spaces security state.** The helper keeps a small record under `/var/lib/polaris`. Remove
  it after step 1:
  ```bash
  sudo rm -rf /var/lib/polaris
  ```
- **KWin screencast permissions, on KDE hosts.** Polaris registers itself for KWin's screencast
  permission by writing `~/.local/share/applications/dev.polaris-stream.app.Polaris.kwin.<id>.desktop`,
  one per binary path, and never removes them, so they pile up across upgrades. Remove them:
  ```bash
  rm -f ~/.local/share/applications/dev.polaris-stream.app.Polaris.kwin.*.desktop
  ```
- **Installs from source.** If you ever installed a source build, its files live under `/usr/local`
  and no package manager knows them: `/usr/local/bin/polaris*`,
  `/usr/local/share/applications/dev.polaris-stream.app.Polaris*.desktop` (this is what keeps
  "Polaris" in the application launcher after the package is gone), `/usr/local/share/polaris`,
  `/usr/local/lib/systemd/user/polaris.service`, and the icons under
  `/usr/local/share/icons/hicolor/scalable/apps/polaris.svg` and
  `/usr/local/share/icons/hicolor/scalable/status/polaris-*.svg`. Because `/usr/local/share` comes
  before `/usr/share` in `XDG_DATA_DIRS`, stale icons there shadow the packaged ones, so the panel
  and the launcher keep showing an old Polaris icon even after a reinstall. Remove all of it, then
  on KDE run `kbuildsycoca6 --noincremental` so the launcher forgets the entry, and restart the
  panel or log out and in:
  ```bash
  sudo rm -rf /usr/local/bin/polaris* /usr/local/share/applications/dev.polaris-stream.app.Polaris*.desktop \
    /usr/local/share/polaris /usr/local/lib/systemd/user/polaris.service \
    /usr/local/share/icons/hicolor/scalable/apps/polaris.svg \
    /usr/local/share/icons/hicolor/scalable/status/polaris-*.svg
  kbuildsycoca6 --noincremental
  systemctl --user restart plasma-plasmashell.service
  ```
- **Your own additions.** Unit overrides under `~/.config/systemd/user/`, udev rules you wrote for
  a virtual display, firewall openings for the Polaris ports, or an extra binary you copied into
  `/usr/bin` while testing. Only you know about these; `ls /usr/bin/polaris*` and
  `ls /etc/udev/rules.d/ | grep polaris` find the common ones.
- **Steam Input.** If you applied the Doctor fix that turns off Steam's Xbox configuration
  support, Steam keeps that setting after Polaris is gone. Turn it back on in Steam under
  Settings, Controller, if you want it;
  [Steam Input and virtual controllers](configuration.md#steam-input-and-virtual-controllers)
  explains what it does.
- **Bazzite's optional KMS copy.** If you made the writable copy of the binary for KMS capture,
  remove that copy and its unit drop-in as the [Bazzite guide](bazzite.md#uninstall) describes.

## 5. Your data

Everything Polaris created while running lives in one folder, `~/.config/polaris`:

| Files | What they hold | Keep them if |
| --- | --- | --- |
| `polaris.conf` | host settings: capture, modes, HDR, ports | you want the same setup back |
| `polaris_state.json`, `credentials/` | the web UI account and every paired device | you do not want to pair every device again |
| `apps.json`, `library_sources.json` | the library and its ROM folders | you want the library back |
| `device_db.json`, `client_profiles.json` | per-device tuning | you tuned devices by hand |
| `covers/`, `artwork/` | downloaded box art | never needed; they come back |
| `polaris.log*` | logs | you plan to report a problem |

Back the folder up before deciding. It is small:

```bash
tar czf ~/polaris-config-$(date +%F).tgz -C ~ .config/polaris
```

To keep Polaris off the machine for good, or to install it again as if for the first time:

```bash
rm -rf ~/.config/polaris
```

To install again and keep your account, devices and library, leave the folder alone. The next
install finds it and shows the login page instead of the welcome page.

## 6. Check

```bash
pgrep -a polaris
ls -d /usr/bin/polaris* /usr/share/polaris /etc/udev/rules.d/*polaris* \
  /etc/modules-load.d/*polaris* /var/lib/polaris ~/.config/polaris 2>/dev/null
ls -d /usr/local/bin/polaris* /usr/local/share/applications/dev.polaris* \
  /usr/local/share/icons/hicolor/*/*/polaris* ~/.local/share/applications/dev.polaris* 2>/dev/null
systemctl --user status polaris
```

Everything but the last line prints nothing, and the last says the unit could not be found.
Anything that still shows up belongs to one of the sections above.

## 7. Install again

Follow the [quickstart](quickstart.md) or your distribution's page. Two things to know:

- With `~/.config/polaris` gone, the console opens on the welcome page and asks for a new account.
  With the folder kept, it opens on the login page and your devices are still paired.
  [Web UI credentials](troubleshooting.md#web-ui-credentials) covers both.
- KMS capture needs its capability again after every install or upgrade. Doctor says so once
  KMS capture is configured; the fix is one command:
  ```bash
  sudo -H polaris --setup-host --enable-kms
  ```
  then restart Polaris.
