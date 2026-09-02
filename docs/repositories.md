# Package repositories

Polaris publishes dnf and pacman repositories, so an upgrade is part of the
host's normal update rather than a download and an exact filename.

```bash
sudo dnf upgrade          # Fedora, Bazzite
sudo pacman -Syu          # Arch, CachyOS
```

Adding the repository is a one-time step. Downloading a release package by hand
still works and is still supported; the repository is an easier path to the same
package, not a different one.

## After install or upgrade

**Fresh install:** open `https://localhost:47990/#/welcome` and create the web UI
account. **Upgrade or reinstall:** open `https://localhost:47990/#/login` and use
the existing account. Repository upgrades and package replacement intentionally
preserve credentials, pairing keys, settings, and the library under
`~/.config/polaris`; they do not turn the host back into a first-run installation.
Use the [credential reset](troubleshooting.md#web-ui-credentials) if the existing
account is no longer known.

## Fedora

```bash
sudo rpm --import https://repo.papi-ux.com/polaris.gpg
sudo curl --location --output /etc/yum.repos.d/polaris.repo \
  https://repo.papi-ux.com/fedora/polaris.repo
sudo dnf install polaris
sudo -H polaris --setup-host
systemctl --user restart polaris
```

After that, `sudo dnf upgrade` carries Polaris with everything else.

The first `dnf` command against the repository asks you to accept the signing
key, showing fingerprint `58017EDFFA9F803E07ED26F835F13F14FAAD15CC`. That is
expected: `rpm --import` populates rpm's keyring, and dnf keeps its own record
of which repositories it trusts. Accepting once is enough.

It also means the first non-interactive use finds nothing rather than failing —
a script on a fresh host should run `sudo dnf -y makecache` before relying on
the repository.

## Bazzite and other ostree hosts

The same repository works, layered rather than installed:

```bash
sudo rpm --import https://repo.papi-ux.com/polaris.gpg
sudo curl --location --output /etc/yum.repos.d/polaris.repo \
  https://repo.papi-ux.com/fedora/polaris.repo
rpm-ostree install polaris
systemctl reboot
```

Layered packages are updated by `rpm-ostree upgrade`, so Polaris follows the
image update instead of needing to be re-layered from a downloaded RPM.

Seat isolation still needs the input group, which on ostree hosts lives in
`/usr/lib/group`. See the [Bazzite guide](bazzite.md#controller-and-input-group).

## Arch and CachyOS

```bash
curl -fsSL https://repo.papi-ux.com/polaris.gpg | sudo pacman-key --add -
sudo pacman-key --lsign-key 58017EDFFA9F803E07ED26F835F13F14FAAD15CC
```

Then append to `/etc/pacman.conf`:

```ini
[polaris]
SigLevel = Required DatabaseRequired
Server = https://repo.papi-ux.com/arch/$arch
```

```bash
sudo pacman -Sy polaris
sudo -H polaris --setup-host
systemctl --user restart polaris
```

`SigLevel = Required DatabaseRequired` is deliberate. Without it pacman falls
back to the checksum recorded in the database and reports `Validated By: SHA-256
Sum` — it never looks at the signature, so anyone who can rewrite the database
rewrites the checksum with it.

## SteamOS

SteamOS is not served by a repository and will not be. The rootfs is read-only
and pacman state does not survive a SteamOS update, so a repository would
promise upgrades it cannot deliver. Follow the [SteamOS guide](steamos.md).

## Ubuntu

Not yet served by a repository. The Ubuntu package remains the download-and-
install path described in the release notes.

## What the repository serves

The package a repository serves is the package the release published, byte for
byte, with a signature added to the repository copy. Nothing is rebuilt:
a rebuild would ship a binary that CI never tested, and the Fedora packaging
pulls a CUDA toolkit over the network at build time, which no sandboxed rebuild
service permits.

The repository currently carries the latest stable release only. Prereleases are
never published to it. Rolling back means installing an older release package by
hand from the [releases page](https://github.com/papi-ux/polaris/releases).

Publishing runs on a schedule, so there is a short window after a release where
the repository still serves the previous version and `dnf upgrade` correctly
reports nothing to do. What it currently serves is not a guess:

```bash
curl -fsS https://repo.papi-ux.com/PUBLISHED_TAG
```

---

## Where this is published from

The repositories are built and served by
[papi-ux/packages](https://github.com/papi-ux/packages), not from this
repository.

That is not organisational tidiness. GitHub reserves
`<user-domain>/<repo-name>` for any repository with a Pages site, so publishing
from here took over `papi-ux.com/polaris/` — a real page on the docs site — and
replaced it with a redirect to the repository. A custom domain does not avoid
the collision, only changes what it returns. A repository whose name collides
with nothing does avoid it.

Signing keys, the publishing schedule, and the tooling are documented there. The
key fingerprint is `58017EDFFA9F803E07ED26F835F13F14FAAD15CC`, and the repository serves its own public key at
[repo.papi-ux.com/polaris.gpg](https://repo.papi-ux.com/polaris.gpg).
