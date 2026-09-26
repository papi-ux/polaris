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
sudo curl --location --output /etc/yum.repos.d/polaris.repo \
  https://repo.papi-ux.com/fedora/polaris.repo
sudo dnf install polaris
sudo -H polaris --setup-host
systemctl --user restart polaris
```

After that, `sudo dnf upgrade` carries Polaris with everything else.

There is no separate key import. The repository definition carries `gpgkey=`, so
`dnf install` fetches the key itself and asks you to accept it:

```
Importing OpenPGP key 0xFAAD15CC:
 UserID     : "Polaris Package Repository <papi@papi-ux.com>"
 Fingerprint: 58017EDFFA9F803E07ED26F835F13F14FAAD15CC
```

Compare that fingerprint with the one on this page before answering yes.
Accepting once is enough.

Because that prompt is the only thing that puts the key in place, a script has
to answer it. `repo_gpgcheck=1` means an unanswered prompt leaves the metadata
unverified, and dnf reports `repomd.xml GPG signature verification error:
Signing key not found` rather than installing anything. A script on a fresh host
should run `sudo dnf -y makecache` first, which accepts the key, or import it
explicitly with `sudo rpm --import https://repo.papi-ux.com/polaris.gpg`.

## Bazzite and other ostree hosts

The same repository works, layered rather than installed:

```bash
sudo rpm --import https://repo.papi-ux.com/polaris.gpg
sudo curl --location --output /etc/yum.repos.d/polaris.repo \
  https://repo.papi-ux.com/fedora/polaris.repo
rpm-ostree install polaris
systemctl reboot
```

The key import stays here, unlike the Fedora steps above. `dnf` offers the key
from `gpgkey=` and waits for an answer; `rpm-ostree` does not ask, so the key has
to be in rpm's keyring before it will layer a signed package.

Layered packages are updated by `rpm-ostree upgrade`, so Polaris follows the
image update instead of needing to be re-layered from a downloaded RPM.

Seat isolation still needs the input group, which on ostree hosts lives in
`/usr/lib/group`. See the [Bazzite guide](bazzite.md#controller-and-input-group).

## Arch and CachyOS

```bash
curl -fsSL https://repo.papi-ux.com/polaris.gpg | sudo pacman-key --add -
sudo pacman-key --lsign-key 58017EDFFA9F803E07ED26F835F13F14FAAD15CC
grep -q '^\[polaris\]' /etc/pacman.conf ||
  curl -fsSL https://repo.papi-ux.com/arch/polaris.conf | sudo tee -a /etc/pacman.conf
sudo pacman -Sy polaris
sudo -H polaris --setup-host
systemctl --user restart polaris
```

The repository publishes its own pacman section, so that is fetched rather than
typed. The `grep` guard is not decoration: appending it twice gives pacman a
duplicated `[polaris]` repository, and pasting a block again is exactly what
someone does when a step appears not to have worked.

What it appends is:

```ini
[polaris]
SigLevel = Required DatabaseRequired
Server = https://repo.papi-ux.com/arch/$arch
```

`SigLevel = Required DatabaseRequired` is deliberate. Without it pacman falls
back to the checksum recorded in the database and reports `Validated By: SHA-256
Sum` — it never looks at the signature, so anyone who can rewrite the database
rewrites the checksum with it.

Unlike dnf, pacman has no equivalent of `gpgkey=` in a repository section, so the
two `pacman-key` commands cannot be dropped the way `rpm --import` was for Fedora.
Shipping a `polaris-keyring` package is what would remove them.

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
a rebuild would ship a binary that CI never tested, and the Fedora and Ubuntu
packaging pull a CUDA toolkit over the network at build time, which no sandboxed
rebuild service permits.

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

## Beta releases

A release is sometimes published early as a beta, tagged `v1.4.13-beta.1` and
marked as a prerelease on GitHub. A beta is the same release told early: it
carries the version it will ship as, and it reuses that release's notes.

A beta never reaches anyone who has not asked for it. GitHub keeps prereleases
out of `releases/latest`, the repositories above never serve one, and Polaris
mentions one only when **PreRelease Notifications** is turned on in the console's
General tab.

To try one, turn that setting on and let the Update Center offer it, or take the
package straight from the
[releases page](https://github.com/papi-ux/polaris/releases) and install it the
way its release notes describe. Going back to stable means installing the stable
package over it.

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
