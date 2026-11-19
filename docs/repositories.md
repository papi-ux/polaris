# Package repositories

Polaris publishes dnf and pacman repositories, so an upgrade is part of the
host's normal update rather than a download and an exact filename.

```bash
sudo dnf upgrade          # Fedora
sudo rpm-ostree upgrade   # Bazzite and other ostree hosts
sudo pacman -Syu          # Arch, CachyOS
```

Adding the repository is a one-time step. Downloading a release package by hand
still works and is still supported; the repository is an easier path to the same
package, not a different one.

## One-command install

On Fedora 44 and 45, Arch Linux and CachyOS, one command checks the repository's
signing key, adds the repository, installs Polaris and runs host setup:

```bash
curl -fsSL https://repo.papi-ux.com/install.sh | sh
```

It runs four steps, signing key, repository, packages and host setup, and prints
each command under its step before running it. What dnf, rpm-ostree, pacman and
host setup print goes to a log file only you can read, named at the start and at
the end; a command that fails shows the end of its output there and then. An
older Polaris already on the host is upgraded. On Arch and CachyOS it installs with `pacman -Syu`, so the rest of
the system upgrades in the same step, without asking. If the running kernel has
no modules left on disk afterwards, or the loaded NVIDIA driver and its
libraries differ, it asks for a reboot and leaves host setup until after it. To
read every command it would run without changing anything, pass `--dry-run`:

```bash
curl -fsSL https://repo.papi-ux.com/install.sh | sh -s -- --dry-run
```

It trusts only the key with the fingerprint on this page. On Fedora it reads the
downloaded key into an empty keyring first, and unless that holds exactly one
key, `58017EDFFA9F803E07ED26F835F13F14FAAD15CC`, it stops before importing any
key or adding the repository. It then imports that key, keeps it as
`/etc/pki/rpm-gpg/RPM-GPG-KEY-polaris`, and writes
`/etc/yum.repos.d/polaris.repo` itself, naming that local key, so dnf never
fetches a key on its own. On Arch it locally signs only that fingerprint and
writes the `[polaris]` section itself. A Fedora release other than 44 or 45
stops before the key or the repository is touched.

What it leaves to you:

- **Starting Polaris.** It runs host setup but does not start Polaris. Start it
  with `systemctl --user enable --now polaris`, then open the console as
  [After install or upgrade](#after-install-or-upgrade) describes.
- **Leaving a beta.** It installs stable releases only, and the release replaces
  an earlier beta in an ordinary upgrade: 1.5.0 replaces a 1.5.0 beta or a
  1.4.13 beta. Only a host whose 1.4.13 beta carries the very version the
  repository serves is left as it is; the installer then prints the reinstall
  command, and [Leave a 1.4.13 beta](updates.md#leave-a-1413-beta) explains it.
- **DRM/KMS capture.** It does not install `polaris-kms`. It prints the command
  that does, and the [Fedora](fedora.md#optional-drmkms-capture) and
  [Arch](arch.md#optional-drmkms-capture) guides cover the logout that follows.

On Bazzite and other ostree hosts, follow
[Bazzite and other ostree hosts](#bazzite-and-other-ostree-hosts) below, or the
[Bazzite guide](bazzite.md) for the downloaded package. Everywhere else, Ubuntu
and SteamOS included, follow your distribution's guide. The sections below are
the same steps by hand.

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

The repository file names `$releasever`, so dnf reads the Fedora 44 or the
Fedora 45 tree to match the release the host runs, and the same file follows an
upgrade from Fedora 44 to 45. **Before upgrading a host from Fedora 44 to 45,**
replace a repository file fetched or written before 1.5.0: those name a single
Fedora 44 tree, which still serves Fedora 44 packages but does not follow the
upgrade. Run the first command above again, or the one-command install, which
rewrites the file.

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
sudo curl --location --output /etc/yum.repos.d/polaris.repo \
  https://repo.papi-ux.com/fedora/polaris.repo
sudo rpm-ostree install polaris
systemctl reboot
```

There is no key import here either. `rpm --import` cannot write rpm's database
on an ostree host, where it is read-only, and is not needed: `rpm-ostree`
fetches the key named by `gpgkey=` itself, without asking, and checks the
repository metadata and the packages against it.

On a host that already has Polaris layered from a downloaded file,
`sudo rpm-ostree install polaris` succeeds but only records
`Inactive requests: polaris (already provided by polaris-...)`, and the file
stays, so that host does not follow the repository. Swap the file for the
repository package in one transaction instead:

```bash
# Polaris alone:
sudo rpm-ostree install --uninstall=polaris polaris
# Polaris and polaris-kms, both layered from files:
sudo rpm-ostree install --uninstall=polaris --uninstall=polaris-kms polaris polaris-kms
```

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
sudo pacman -Syu polaris
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

Not yet served by a repository. Install the `.deb` as the
[Ubuntu guide](ubuntu.md) describes.

## What the repository serves

The package a repository serves is the package the release published, byte for
byte, with a signature added to the repository copy. Nothing is rebuilt:
a rebuild would ship a binary that CI never tested, and the Fedora and Ubuntu
packaging pull a CUDA toolkit over the network at build time, which no sandboxed
rebuild service permits.

The repository currently carries the latest stable release only: `polaris` and
the optional DRM/KMS helper `polaris-kms`, at the same version. Prereleases are
never published to it. Fedora has one tree for each release,
`fedora/44/x86_64/` and `fedora/45/x86_64/`, each with the package built for that
release, and `fedora/x86_64/` keeps a copy of the Fedora 44 tree for repository
files from before 1.5.0. Rolling back means installing an older release package by
hand from the [releases page](https://github.com/papi-ux/polaris/releases).

Publishing runs on a schedule, so there is a short window after a release where
the repository still serves the previous version and `dnf upgrade` correctly
reports nothing to do. What it currently serves is not a guess:

```bash
curl -fsS https://repo.papi-ux.com/PUBLISHED_TAG
```

---

## Beta releases

For the complete opt-in, installation, subsequent-update and return-to-stable
steps, see [Polaris updates and beta releases](updates.md).

A release is sometimes published early as a beta, tagged `v1.4.13-beta.1` and
marked as a prerelease on GitHub. A beta reuses the notes of the release it
precedes. Its packages carry a version that sorts below that release, so the
release replaces the beta through an ordinary upgrade: a tag like
`v1.5.0-beta.2` builds `1.5.0~beta.2` for RPM and DEB and `1.5.0beta.2` for
pacman, and the host reports `1.5.0-beta.2`. The 1.4.13 betas predate this and
carry `1.4.13` itself, so while the repository served 1.4.13 a repository upgrade
left them in place. It serves 1.5.0 now, which replaces them in an ordinary
upgrade; [Leave a 1.4.13 beta](updates.md#leave-a-1413-beta) has the steps.

A beta never reaches anyone who has not asked for it. GitHub keeps prereleases
out of `releases/latest`, the repositories above never serve one, and Polaris
mentions one only when **Include beta releases** is turned on in **System →
Update Center**. Older versions call this **PreRelease Notifications** in the
General settings tab.

To try one, turn that setting on and let the Update Center offer it, or take the
package straight from the
[releases page](https://github.com/papi-ux/polaris/releases) and install it the
way its release notes describe. Turning the preference off does not downgrade
an installed beta; wait for the stable release it precedes, which an ordinary
upgrade installs over it, or follow the stable package's downgrade instructions.

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
