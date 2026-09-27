# Polaris updates and beta releases

Use **System → Update Center** in the Polaris console to check your installed
version, read release notes, and get the package or install command for your host.
Polaris does not install packages or restart the host automatically.

## Opt in to beta releases

1. Open **System → Update Center** and turn on **Include beta releases**. The
   preference saves immediately. On older versions without this control, open
   **Settings → General**, enable **PreRelease Notifications**, and save.
2. Select **Check again**. Update Center offers a beta when it is newer than both
   the available stable release and your installed version. A beta or release
   candidate is marked as a prerelease; opting in does not mean one is always
   available.
3. Open the release notes and check the host requirements. Download the package
   offered for your distribution, or copy the install command from Update Center.
4. End active streams before installing. Follow that release's instructions, then
   restart Polaris when required and check the running version in Update Center.

Beta packages come from the selected
[GitHub prerelease](https://github.com/papi-ux/polaris/releases). The normal dnf
and pacman repositories carry **stable releases only**. Enabling beta releases
does not reconfigure those repositories, and their usual upgrade command will
not install a beta.

If this host uses `polaris-kms`, update the base package and matching KMS helper
from the **same release** together. Update Center includes the helper when it is
installed; if that release lacks a matching helper, use the release instructions
instead of installing a mismatched pair. Bazzite and SteamOS have separate
installation steps in their [Bazzite](bazzite.md) and [SteamOS](steamos.md) guides.

## Update an existing beta

Leave **Include beta releases** on, select **Check again**, and install the newer
package offered by Update Center. Check the full version, including `beta.N` or
`rc.N`, after restarting. A stable release is offered when it is newer than the
beta you are running, and the release a beta precedes counts as newer.

The 1.4.13 betas are the exception. They report `1.4.13`, the same version as the
1.4.13 release, so on those hosts Update Center shows **Current release** and never
offers 1.4.13. Replace one by hand as described in
[Leave a 1.4.13 beta](#leave-a-1413-beta). A later stable release, such as 1.4.14,
is offered as usual.

An upgrade keeps the existing account, paired devices, settings and library.
Sign in at `https://localhost:47990/#/login`; do not repeat first-run account
creation or delete the configuration directory to update.

For PyroWave testing, use the [PyroWave client and build instructions](pyrowave.md).
Standard Moonlight does not support that codec, and the ordinary Nova Linux
Flatpak attached to beta.3 was not built with it enabled.

## Leave a 1.4.13 beta

The 1.4.13 betas and the 1.4.13 release carry the same package version, `1.4.13`,
so a package manager can take the beta for the release. On Fedora, `dnf install`
of the release package answers that it is already installed, "Nothing to do", and
`dnf upgrade` from the Polaris repository leaves the beta in place. `pacman -Syu`
from the Polaris repository does the same. Reinstall the release over the beta
instead, which replaces the installed files with the release's.

End active streams first. The commands that name files take them from the
[1.4.13 release](https://github.com/papi-ux/polaris/releases/tag/v1.4.13), run
in the folder they were downloaded to. If `polaris-kms` is installed, reinstall
it in the same command from the same release: a command that names only
`polaris` leaves the beta's helper in place. Without the helper, leave out the
`polaris-kms` package or file. Restart Polaris afterwards. Once a later stable
release such as 1.4.14 is out, none of this is needed: an ordinary upgrade replaces
a 1.4.13 beta, and the repository commands below stop working because the
repository then carries only that newer release.

**Fedora**:

```bash
# From the downloaded files
sudo dnf reinstall ./Polaris-fedora44-x86_64.rpm ./Polaris-kms-fedora44-x86_64.rpm
# Or from the Polaris repository
sudo dnf reinstall polaris polaris-kms
```

**Ubuntu 24.04**, from the downloaded files. Keep `--reinstall`: without it, apt
can decide the package is already installed and change nothing.

```bash
sudo apt install --reinstall ./Polaris-ubuntu24.04-x86_64.deb ./Polaris-kms-ubuntu24.04-x86_64.deb
```

**Arch**. pacman warns that the package is up to date and reinstalls it.

```bash
# From the downloaded files
sudo pacman -U ./Polaris-arch-x86_64.pkg.tar.zst ./Polaris-kms-arch-x86_64.pkg.tar.zst
# Or from the Polaris repository
sudo pacman -S polaris polaris-kms
```

**SteamOS** takes the same `pacman -U` its [SteamOS guide](steamos.md) installs
with: reinstall `Polaris-steamos3.8-x86_64.pkg.tar.zst`, and
`Polaris-kms-steamos3.8-x86_64.pkg.tar.zst` with the helper. This reinstall was
checked with pacman on Arch, not on a Steam Deck.

**Bazzite** layers the package with `rpm-ostree`, and no step that replaces a
1.4.13 beta with the release there has been verified yet.

## Return to stable

Turn off **Include beta releases** and select **Check again**. This changes which
releases are offered; it **does not downgrade or replace** the installed beta.
You can wait for the stable release the beta precedes, or a newer one, and
install it normally. A beta's packages sort below that release, so an ordinary
upgrade replaces the beta. The 1.4.13 betas are the exception: they carry
`1.4.13` itself, so a package manager can treat the 1.4.13 release as already
installed. [Leave a 1.4.13 beta](#leave-a-1413-beta) has the commands that
replace one.

To move to an older stable release immediately, open the
[stable release](https://github.com/papi-ux/polaris/releases/latest), back up your
configuration, and follow its package installation or downgrade instructions for
your distribution. Keep the base package and any KMS helper on the same version.
A package manager may require an explicit downgrade; do not remove the app's
data or force a mismatched dependency to get past that check.

For stable repository setup and package trust, see
[Repositories and upgrades](repositories.md). Nova has its own
[beta and update guide](https://papi-ux.com/docs/nova/updates/); selecting Polaris
betas does not change the Nova client's update channel.
