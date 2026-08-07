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

## Fedora

```bash
sudo rpm --import https://papi-ux.github.io/polaris/repo/polaris.gpg
sudo curl --location --output /etc/yum.repos.d/polaris.repo \
  https://papi-ux.github.io/polaris/repo/fedora/polaris.repo
sudo dnf install polaris
sudo -H polaris --setup-host
systemctl --user restart polaris
```

After that, `sudo dnf upgrade` carries Polaris with everything else.

## Bazzite and other ostree hosts

The same repository works, layered rather than installed:

```bash
sudo rpm --import https://papi-ux.github.io/polaris/repo/polaris.gpg
sudo curl --location --output /etc/yum.repos.d/polaris.repo \
  https://papi-ux.github.io/polaris/repo/fedora/polaris.repo
rpm-ostree install polaris
systemctl reboot
```

Layered packages are updated by `rpm-ostree upgrade`, so Polaris follows the
image update instead of needing to be re-layered from a downloaded RPM.

Seat isolation still needs the input group, which on ostree hosts lives in
`/usr/lib/group`. See the [Bazzite guide](bazzite.md#controller-and-input-group).

## Arch and CachyOS

```bash
sudo pacman-key --recv-keys <KEY-ID> --keyserver keyserver.ubuntu.com
sudo pacman-key --lsign-key <KEY-ID>
```

Then append to `/etc/pacman.conf`:

```ini
[polaris]
SigLevel = Required DatabaseRequired
Server = https://papi-ux.github.io/polaris/repo/arch/$arch
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

The repository currently carries the latest release only. Rolling back means
installing an older release package by hand from the
[releases page](https://github.com/papi-ux/polaris/releases).

---

## Maintaining the repository

`.github/workflows/package-repos.yml` runs when a release is published, and can
be run by hand against any existing tag.

### One-time setup

The signing key must be RSA. `rpmsign` exits 0 on an Ed25519 key and silently
signs nothing — `scripts/build-package-repos.sh` verifies the signature after
signing for exactly that reason, so a wrong key type fails the build rather than
publishing an unsigned package.

```bash
gpg --batch --gen-key <<'EOF'
%no-protection
Key-Type: RSA
Key-Length: 4096
Name-Real: Polaris
Name-Email: papi@papi-ux.com
Expire-Date: 0
%commit
EOF

gpg --list-keys --with-colons | awk -F: '/^fpr/{print $10; exit}'   # the key id
gpg --armor --export-secret-keys <KEY-ID>                           # the secret
```

Set two repository secrets:

| Secret | Value |
|---|---|
| `POLARIS_REPO_GPG_KEY_ID` | the key fingerprint |
| `POLARIS_REPO_GPG_PRIVATE_KEY` | the armored secret key |

Then set Pages to deploy from GitHub Actions in the repository settings, and
publish the public key somewhere clients can reach it — the workflow serves it
at `repo/polaris.gpg`, and uploading it to a keyserver makes the `pacman-key
--recv-keys` flow above work.

Back the secret key up somewhere that is not this repository. Losing it means
every host that trusts it has to be re-pointed at a new one.

### Checking the layout locally

`scripts/build-package-repos.sh` runs without a key, which assembles the
repositories unsigned so the layout can be inspected:

```bash
scripts/build-package-repos.sh --only fedora --assets ./assets --version 1.3.7 --output ./repo
```

Each ecosystem needs its own toolchain — `createrepo_c` and `rpmsign` on Fedora,
`repo-add` on Arch — so run each `--only` in the matching container.
