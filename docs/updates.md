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
`rc.N`, after restarting. A stable release can still be offered when it is newer
than the beta you are running.

An upgrade keeps the existing account, paired devices, settings and library.
Sign in at `https://localhost:47990/#/login`; do not repeat first-run account
creation or delete the configuration directory to update.

For PyroWave testing, use the [PyroWave client and build instructions](pyrowave.md).
Standard Moonlight does not support that codec, and the ordinary Nova Linux
Flatpak attached to beta.3 was not built with it enabled.

## Return to stable

Turn off **Include beta releases** and select **Check again**. This changes which
releases are offered; it **does not downgrade or replace** the installed beta.
You can wait for a newer stable release and install it normally.

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
