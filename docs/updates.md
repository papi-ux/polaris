# Polaris updates and beta releases

Use **System → Update Center** in the Polaris console to check your installed
version, read release notes, and get the package or install command for your host.
Polaris does not install packages or restart the host automatically.

## Host-approved Fedora updates

The first host-update source path supports an existing native Fedora 44 or 45
x86_64 installation. **Review update** asks the host to select the newest official
Stable release, or the newest official stable, beta or release candidate when
**Include beta releases** is enabled. The selected release is labeled Stable,
Beta or Release candidate. Host selection is independent of release data fetched
by the browser. If this channel would downgrade an installed package, Polaris
refuses; wait for a suitable newer release. The checkbox has the same meaning on
guided hosts.

Review lists the exact release and package assets, their asset IDs and SHA-256
digests under **Details**. It updates `polaris` and the matching `polaris-kms` only
when that helper is already installed. If one member is already at the selected
version, only the older member is submitted to RPM; the current member is kept.
No additional KMS capability is enabled. The helper
independently fetches the official metadata over system-CA HTTPS after approval
and compares its derived release, assets, digests and installed package state
against the host's review fingerprint. A changed or missing identity requires a
new review. Neither a browser URL nor a caller digest, filename, path or command
authorizes root to download or install anything.

Both Stable and Beta use the actual official GitHub release assets in this
slice. The published digest over HTTPS checks **integrity, not an independent
publisher signature**. This route does not claim a signed repository or a
separate Beta signature. Existing repository instructions are a separate guided
route and are not used by this helper.

**Install with host approval** opens the operating system's administrator prompt
on this PC's active local desktop. There is no password field in the console.
The new install, cancel and Finish APIs require a console cookie and CSRF token;
Bearer keys retain read access only. A pending approval can be cancelled or
time out. Once approved, the transaction is allowed to finish, including if the
page disconnects. A privileged update lock and a small root-owned status journal
let a new Polaris process report an unfinished operation conservatively.

Updates and Finish wait for active streams, pending RTSP launches, owned apps
kept running for resume, non-idle session phases, Spaces setup/activity and other
host-admin work. Ordinary launch and resume reserve the existing Spaces
host-admin service during preflight; the console app launch and isolated Browser
Stream paths use it too, until the app/session busy state is published. Update
admission uses that same service.
Polaris never ends a session to make an update eligible.

The bounded operation uses RPM directly, without a dependency resolver. It
accepts only the exact verified package files, refuses incoming obsoletes,
unexpected names/epoch/version/release/architecture, downgrades and reinstalls,
and keeps dependency and file-conflict checks enabled in the actual RPM
transaction. It cannot fetch unrelated dependencies or synchronize/upgrade the
OS. Its `--test` check helps explain refusals; the closed package set is enforced
by the non-resolving actual operation, rather than trusting a preview followed by
a resolver. A refusal preserves RPM's details: it may concern package state,
dependencies or conflicts. Polaris will not force a reinstall, remove entries or
update the OS to resolve it. Repair the reported issue separately, then review
again. Partial/duplicate or unreadable package entries require manual recovery;
the console shows known versions without promising an automatic repair.
Package scripts and triggers are disabled for this existing-installation route,
so the advisory device-rule reload and desktop-cache updates are skipped and no
automatic service restart runs. New rules or icons may need a subsequent sign-in
or reboot; use guided setup after Finish if streaming setup needs attention.
RPM's ordinary file ownership, capability, sysusers and security labeling
handling remain enabled. Actual package behavior still needs Fedora qualification.
This update path is not a substitute for first-install setup.

After any attempted transaction, the helper queries each package again. The
console shows actual versions, absent, partial or unknown state; a failed run
does not imply that the old versions survived. RPM database readback is not a
full payload-health or physical streaming check. A retry always requires
**Review again** and fresh OS approval. Reopening the page reads the existing job
or the installed package state; it never blindly repeats an install.
Terminal results from older releases do not override a newer update offer. An
explicit Review or Check again fetches official metadata anew; initial tab mounts
can reuse a host review for 60 seconds. Up to eight independent unconsumed reviews
are retained for ten minutes, so another tab's explicit review does not replace
yours. Busy/installed facts and root's identity check still run at admission.

**Finish** is explicit. It confirms a consistent installed package pair and the
packaged running binary, uses the same admitted busy guard, and asks Polaris to
restart without terminating an app or using `/api/restart`. The page checks for
the exact installed target running with a consistent package pair, independently
of any old, failed or missing journal. Each read is bounded; if Polaris has not
returned within 90 seconds, the page says to check the PC and slows read-only
reconnect attempts to 15 seconds. Visible idle tabs read status once per minute;
active operations read every three seconds, reviews every five, and hidden tabs
pause automatic polling until visible again. No reconnect repeats a mutation.

This source slice does not close #490 or the approved 1.5.0 installer/update
objective. Arch/CachyOS and Ubuntu adapters, immutable/ostree and SteamOS routes,
headless authorization, first-install site/native-installer handoff and
post-install setup remain explicitly open. Unqualified systems fail closed for
host installation and keep their guided instructions. Live polkit, package,
restart, per-family installer and physical streaming qualification remain
separate acceptance gates.

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
[Leave a 1.4.13 beta](#leave-a-1413-beta). A later stable release, such as 1.5.0,
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
from the Polaris repository does the same, and so does the
[one-command install](repositories.md#one-command-install) on both. Reinstall the
release over the beta instead, which replaces the installed files with the release's.

End active streams first. The commands that name files take them from the
[1.4.13 release](https://github.com/papi-ux/polaris/releases/tag/v1.4.13), run
in the folder they were downloaded to. If `polaris-kms` is installed, reinstall
it in the same command from the same release: a command that names only
`polaris` leaves the beta's helper in place. Without the helper, leave out the
`polaris-kms` package or file. Restart Polaris afterwards. Once a later stable
release such as 1.5.0 is out, none of this is needed: an ordinary upgrade replaces
a 1.4.13 beta, and the repository commands below stop working because the
repository then carries only that newer release.

**Fedora**:

```bash
# From the downloaded files
sudo dnf reinstall ./Polaris-fedora44-x86_64.rpm ./Polaris-kms-fedora44-x86_64.rpm
# Or from the Polaris repository
sudo dnf reinstall polaris polaris-kms
```

**Ubuntu 24.04**, from the downloaded files. apt replaces both with the release's
files, the same command the [Ubuntu guide](ubuntu.md#update) updates with.

```bash
sudo apt install ./Polaris-ubuntu24.04-x86_64.deb ./Polaris-kms-ubuntu24.04-x86_64.deb
```

**Arch**. pacman warns that the package is up to date and reinstalls it.

```bash
# From the downloaded files
sudo pacman -U ./Polaris-arch-x86_64.pkg.tar.zst ./Polaris-kms-arch-x86_64.pkg.tar.zst
# Or from the Polaris repository
sudo pacman -Syu polaris polaris-kms
```

**SteamOS** takes the same `pacman -U` its [SteamOS guide](steamos.md) installs
with: reinstall the package for the host's SteamOS release, `Polaris-steamos3.8-x86_64.pkg.tar.zst`
or `Polaris-steamos3.9-x86_64.pkg.tar.zst`, and its `Polaris-kms-` helper beside it when the host has one. This reinstall was
checked with pacman on Arch, not on a Steam Deck.
The 3.9 packages begin with `v1.5.0-beta.2`: use that tagged prerelease after
publication, as the SteamOS guide shows, rather than the latest stable download.

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
