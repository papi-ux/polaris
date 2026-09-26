import PolarisVersion from './polaris_version'

/* global __POLARIS_CONSOLE_VERSION__ */
// The release this console was built with, set by the build; empty under test
// and in a console built by hand, which switches the comparison off.
const BUILT_CONSOLE_VERSION = typeof __POLARIS_CONSOLE_VERSION__ === 'string' ? __POLARIS_CONSOLE_VERSION__ : ''

const PACKAGE_LABELS = {
  arch: 'Arch/CachyOS package',
  steamos: 'SteamOS 3.8 package',
  fedora: 'Fedora RPM package',
  ubuntu: 'Ubuntu 24.04 DEB package',
}

function normalizeToken(value) {
  return String(value || '').trim().toLowerCase()
}

function distroTokens(host = {}) {
  const distro = host.distro || {}
  const tokens = []
  const id = normalizeToken(distro.id)
  if (id) tokens.push(id)
  String(distro.id_like || '')
    .split(/\s+/)
    .map(normalizeToken)
    .filter(Boolean)
    .forEach((token) => tokens.push(token))
  return tokens
}

function hasToken(tokens, value) {
  return tokens.includes(value)
}

function isSteamOs38(host = {}) {
  const versionId = normalizeToken(host.distro?.version_id)
  return versionId === '3.8' || versionId.startsWith('3.8.')
}

export function inferPackageFamily(host = {}) {
  if (normalizeToken(host.platform) !== 'linux') return ''

  const tokens = distroTokens(host)
  if (hasToken(tokens, 'steamos')) return 'steamos'
  if (host.package_family) return normalizeToken(host.package_family)
  if (hasToken(tokens, 'arch') || hasToken(tokens, 'cachyos') || hasToken(tokens, 'manjaro') || hasToken(tokens, 'endeavouros')) {
    return 'arch'
  }
  if (hasToken(tokens, 'fedora') || hasToken(tokens, 'bazzite') || hasToken(tokens, 'rhel') || hasToken(tokens, 'centos')) {
    return 'fedora'
  }
  if (hasToken(tokens, 'ubuntu')) {
    return 'ubuntu'
  }
  return ''
}

// The release names the DRM/KMS helper after its base package, so one rule covers both and there
// is no second place for a Fedora version to be got wrong.
const packageNamePatterns = {
  steamos: {
    base: /^Polaris-steamos3\.8-x86_64\.pkg\.tar\.zst$/,
    kms: /^Polaris-kms-steamos3\.8-x86_64\.pkg\.tar\.zst$/,
  },
  arch: {
    base: /^Polaris-arch-x86_64\.pkg\.tar\.zst$/,
    kms: /^Polaris-kms-arch-x86_64\.pkg\.tar\.zst$/,
  },
  fedora: {
    base: /^Polaris-fedora\d+-x86_64\.rpm$/,
    kms: /^Polaris-kms-fedora\d+-x86_64\.rpm$/,
  },
  ubuntu: {
    base: /^Polaris-ubuntu24\.04-x86_64\.deb$/,
    kms: /^Polaris-kms-ubuntu24\.04-x86_64\.deb$/,
  },
}

function kmsNameForBase(baseName) {
  const name = String(baseName || '')
  return name.startsWith('Polaris-') ? name.replace('Polaris-', 'Polaris-kms-') : ''
}

function preferredAssetName(host = {}) {
  const family = inferPackageFamily(host)
  const versionId = String(host.distro?.version_id || '').trim()
  if (family === 'steamos' && isSteamOs38(host)) return 'Polaris-steamos3.8-x86_64.pkg.tar.zst'
  if (family === 'steamos') return ''
  if (host.recommended_asset_name) return host.recommended_asset_name
  if (family === 'arch') return 'Polaris-arch-x86_64.pkg.tar.zst'
  if (family === 'fedora' && versionId) return `Polaris-fedora${versionId}-x86_64.rpm`
  if (family === 'ubuntu' && versionId.startsWith('24.04')) return 'Polaris-ubuntu24.04-x86_64.deb'
  return ''
}

export function selectReleaseAsset(release, host = {}) {
  if (!release || !Array.isArray(release.assets)) return null

  const assets = release.assets
  const family = inferPackageFamily(host)
  if (family === 'steamos') {
    if (!isSteamOs38(host)) return null
    return assets.find((asset) => asset.name === 'Polaris-steamos3.8-x86_64.pkg.tar.zst') || null
  }
  const preferredName = preferredAssetName(host)
  if (preferredName) {
    const exact = assets.find((asset) => asset.name === preferredName)
    if (exact) return exact
  }

  const fallback = assets.find((asset) => packageNamePatterns[family]?.base.test(asset.name))
  return fallback || null
}

/**
 * The DRM/KMS helper package that belongs beside a chosen base asset, or null.
 *
 * Only for a host that has the helper installed. polaris-kms requires the exact version of polaris
 * beside it, so installing the base package on its own is not an upgrade of such a host: it is a
 * broken dependency. Named after the base asset, because that is how the release names it.
 */
export function selectKmsReleaseAsset(release, host = {}, baseAsset = null) {
  if (host.kms_helper_installed !== true) return null
  if (!release || !Array.isArray(release.assets) || !baseAsset?.name) return null
  const wanted = kmsNameForBase(baseAsset.name)
  if (!wanted) return null
  return release.assets.find((asset) => asset.name === wanted) || null
}

function isSafeAssetForFamily(asset, family, kind = 'base') {
  const fileName = String(asset?.name || '')
  const rawUrl = String(asset?.browser_download_url || '')
  const expectedName = packageNamePatterns[family]?.[kind] || null
  if (!expectedName?.test(fileName)) return false
  if (!/^https:\/\/[A-Za-z0-9.-]+(?::[0-9]+)?\/[A-Za-z0-9._~%+/-]+$/.test(rawUrl)) return false

  try {
    const url = new URL(rawUrl)
    return url.protocol === 'https:'
      && !url.username
      && !url.password
      && !url.search
      && !url.hash
      && url.pathname.endsWith(`/${fileName}`)
  } catch {
    return false
  }
}

export function buildManualInstallCommand(asset, host = {}, kmsAsset = null) {
  if (!asset?.name || !asset?.browser_download_url) return ''

  const family = normalizeToken(asset.packageFamily || host.packageFamily || inferPackageFamily(host))
  if (!isSafeAssetForFamily(asset, family)) return ''

  // A host with the helper installed cannot take the base package alone: polaris-kms requires the
  // exact version beside it. If this release does not carry a matching helper, and a prerelease
  // before 1.4.13 does not, then there is no command that leaves this host working, so offer none
  // rather than one that breaks it.
  const needsKms = host.kms_helper_installed === true
  const kmsUsable = Boolean(kmsAsset?.name) && Boolean(kmsAsset?.browser_download_url)
    && isSafeAssetForFamily(kmsAsset, family, 'kms')
  if (needsKms && !kmsUsable) return ''

  const fileName = asset.name
  const downloadUrl = asset.browser_download_url
  const lines = [`wget --output-document=./${fileName} ${downloadUrl} &&`]
  // Both files are fetched before either is installed, so a failed second download cannot leave the
  // host with a base package its helper no longer matches.
  const packageFiles = needsKms ? [fileName, kmsAsset.name] : [fileName]
  if (needsKms) lines.push(`wget --output-document=./${kmsAsset.name} ${kmsAsset.browser_download_url} &&`)

  if (family === 'steamos') {
    lines.push('(')
    lines.push('set -e')
    lines.push("trap 'sudo steamos-readonly enable' EXIT")
    lines.push('sudo steamos-readonly disable || exit $?')
    lines.push(`sudo pacman -U ${packageFiles.map((name) => `./${name}`).join(' ')} || exit $?`)
    // Boot start stays as the host has it. Naming the flag again when it is on is a no-op, and
    // leaving it out when it is off means an update never turns it back on for someone who
    // ran --disable-headless-boot; Troubleshooting offers it to a Game Mode host without it.
    lines.push(host.boot_start_enabled === true
      ? 'sudo -H polaris --setup-host --enable-headless-boot || exit $?'
      : 'sudo -H polaris --setup-host || exit $?')
    lines.push('sudo steamos-readonly enable || exit $?')
    lines.push('trap - EXIT')
    lines.push(') &&')
    lines.push('systemctl --user enable --now polaris')
    return lines.join('\n')
  }

  if (family === 'arch') {
    lines.push(`sudo pacman -U ${packageFiles.map((name) => `./${name}`).join(' ')} &&`)
  } else if (family === 'fedora') {
    lines.push(`sudo dnf install ${packageFiles.map((name) => `"./${name}"`).join(' ')} &&`)
  } else if (family === 'ubuntu') {
    lines.push(`sudo apt install ${packageFiles.map((name) => `./${name}`).join(' ')} &&`)
  } else {
    return ''
  }

  lines.push('sudo -H polaris --setup-host &&')
  lines.push('systemctl --user restart polaris')
  return lines.join('\n')
}


export function buildRepositoryUpgradeCommand(host = {}) {
  if (!host.repository_configured) return ''

  // The command comes from the host rather than being rebuilt here. Only the
  // host knows whether this is an ostree image, where dnf is not the thing that
  // changes the system and `dnf upgrade` is the same shape of wrong answer as
  // `usermod -aG input` was on Bazzite.
  const command = String(host.repository_upgrade_command || '').trim()
  if (!command) return ''

  return `${command} &&\nsystemctl --user restart polaris`
}


function buildActionMetadata(status, asset, installCommand, releaseUrl, kmsHelperMissingFromRelease = false) {
  if (status === 'update_available') {
    if (installCommand) {
      return {
        statusTone: 'update',
        statusLightLabel: 'Update available',
        primaryActionLabel: 'Update',
        primaryActionKind: 'copy_install_command',
        primaryActionSummary: 'Copy the safe local install command and jump to the package details.',
      }
    }

    return {
      statusTone: 'update',
      statusLightLabel: 'Update available',
      primaryActionLabel: releaseUrl ? 'Open release' : 'Update details',
      primaryActionKind: releaseUrl ? 'open_release' : 'scroll_to_update_center',
      // There is a difference between finding no package for this host and finding the package but
      // not the DRM/KMS helper it is pinned to, and saying the first when the second is true sends
      // someone looking for a package that is right there.
      primaryActionSummary: kmsHelperMissingFromRelease
        ? 'A newer release is available, but it carries no polaris-kms package. This host captures through DRM/KMS, and installing Polaris without the matching helper would break it.'
        : 'A newer release is available, but Polaris could not match a local package for this host.',
    }
  }

  if (status === 'restart_required') {
    return {
      statusTone: 'restart',
      statusLightLabel: 'Installed version is not running',
      primaryActionLabel: 'Details',
      primaryActionKind: 'scroll_to_update_center',
      primaryActionSummary: 'A newer Polaris package is installed than the one running.',
    }
  }

  if (status === 'ahead') {
    return {
      statusTone: 'ahead',
      statusLightLabel: 'Build is newer than selected release channel',
      primaryActionLabel: 'Details',
      primaryActionKind: 'scroll_to_update_center',
      primaryActionSummary: 'This host is ahead of the selected release channel.',
    }
  }

  if (status === 'unavailable') {
    return {
      statusTone: 'warning',
      statusLightLabel: 'Update check unavailable',
      primaryActionLabel: 'Check again',
      primaryActionKind: 'refresh_update_status',
      primaryActionSummary: 'Release metadata could not be loaded from this browser session.',
    }
  }

  if (status === 'disabled') {
    return {
      statusTone: 'disabled',
      statusLightLabel: 'Update checks disabled',
      primaryActionLabel: 'Disabled',
      primaryActionKind: 'none',
      primaryActionSummary: 'Update checks are disabled for this host.',
    }
  }

  return {
    statusTone: 'current',
    statusLightLabel: 'Current release installed',
    primaryActionLabel: asset ? 'Package' : 'Current',
    primaryActionKind: asset ? 'scroll_to_update_center' : 'none',
    primaryActionSummary: 'This host is on the latest public release.',
  }
}

function versionFromRelease(release) {
  return release?.tag_name || release?.name || ''
}

function isReleaseGreater(release, version, includeIncremental = false) {
  if (!release || !version) return false
  try {
    return new PolarisVersion(release, null).isGreater(new PolarisVersion(null, version), includeIncremental)
  } catch {
    return false
  }
}

function isInstalledNewerThanRunning(installedVersion, runningVersion) {
  if (!installedVersion || !runningVersion) return false
  try {
    return new PolarisVersion(null, installedVersion).isGreater(new PolarisVersion(null, runningVersion))
  } catch {
    return false
  }
}

function isVersionGreater(version, release) {
  if (!release || !version) return false
  try {
    return new PolarisVersion(null, version).isGreater(new PolarisVersion(release, null))
  } catch {
    return false
  }
}

// Every build serves the web files the package installed, so the console is
// the one part of a host that always comes from the new package. A process
// older than its console is running a binary the package no longer installs:
// an update nobody restarted after, or the Bazzite KMS runtime copy, which no
// update touches. Hosts before 1.4.3 report no installed package version and
// hosts before 1.4.8 no running binary, so for them this is the only signal.
// Release numbers only: a development build's commit suffix says nothing here.
function describeHostBehindConsole(consoleVersion, runningVersion) {
  if (!isInstalledNewerThanRunning(consoleVersion, runningVersion)) return null
  const consoleRelease = new PolarisVersion(null, consoleVersion).versionParts.slice(0, 3).join('.')
  return {
    status: 'restart_required',
    statusLabel: 'Console is newer than the host',
    summary: `This console came with Polaris ${consoleRelease}, but the host process answering it is ${runningVersion}. The package was updated and the service still runs an older binary. Restart Polaris. If it still shows ${runningVersion}, the service runs a copy outside the package, on Bazzite /usr/local/bin/polaris-kms: run sudo -H polaris --setup-host, then restart again.`,
  }
}

export function chooseCandidateRelease({ latestRelease, prereleaseRelease, includePrereleases = false, currentVersion = '' } = {}) {
  if (includePrereleases && prereleaseRelease && isReleaseGreater(prereleaseRelease, latestRelease ? versionFromRelease(latestRelease) : currentVersion, true)) {
    return prereleaseRelease
  }
  return latestRelease || null
}

export function buildUpdateCenterState({ currentVersion = '', latestRelease = null, prereleaseRelease = null, includePrereleases = false, host = {}, disabled = false, consoleVersion = BUILT_CONSOLE_VERSION } = {}) {
  if (disabled) {
    const action = buildActionMetadata('disabled', null, '', '')
    return {
      status: 'disabled',
      statusLabel: 'Update checks disabled',
      summary: 'Update awareness is disabled for this host.',
      latestVersion: '',
      releaseUrl: '',
      asset: null,
      packageLabel: '',
      installCommand: '',
      canCopyInstallCommand: false,
      ...action,
    }
  }

  const candidateRelease = chooseCandidateRelease({ latestRelease, prereleaseRelease, includePrereleases, currentVersion })
  const hostBehindConsole = describeHostBehindConsole(consoleVersion, currentVersion)
  if (hostBehindConsole && !candidateRelease) {
    // Nothing here needs GitHub, and a host this old is the one that must not
    // be left reading "could not check".
    return {
      ...hostBehindConsole,
      currentVersion,
      latestVersion: '',
      releaseUrl: '',
      asset: null,
      packageLabel: '',
      installCommand: '',
      canCopyInstallCommand: false,
      ...buildActionMetadata(hostBehindConsole.status, null, '', ''),
    }
  }
  if (!currentVersion || !candidateRelease) {
    const releaseUrl = candidateRelease?.html_url || ''
    const action = buildActionMetadata('unavailable', null, '', releaseUrl)
    return {
      status: 'unavailable',
      statusLabel: 'Update status unavailable',
      summary: 'Polaris could not check GitHub releases from this browser session.',
      latestVersion: versionFromRelease(candidateRelease),
      releaseUrl,
      asset: null,
      packageLabel: '',
      installCommand: '',
      canCopyInstallCommand: false,
      ...action,
    }
  }

  let status = 'current'
  let statusLabel = 'Current release'
  let summary = 'This host is on the latest public release.'
  if (isReleaseGreater(candidateRelease, currentVersion, includePrereleases)) {
    status = 'update_available'
    statusLabel = candidateRelease.prerelease ? 'Prerelease available' : 'Update available'
    summary = 'A newer Polaris package is available. Copy the manual install command when you are ready.'
  } else if (isVersionGreater(currentVersion, candidateRelease)) {
    status = 'ahead'
    statusLabel = 'Ahead of latest release'
    summary = 'This host is running a build newer than the selected release channel.'
  }

  // The package database can be ahead of the running process: dnf, apt, and
  // pacman replace the binary but leave the old process running, and rpm-ostree
  // hosts keep running the booted deployment. That confusion reads like a
  // failed update, so it gets its own state ahead of everything else.
  const installedVersion = String(host.installed_package_version || '').trim()
  // A service pointed at a copy of the binary outside the package (the Bazzite
  // DRM/KMS recipe) keeps running the old version after every update, and a
  // restart changes nothing; the advice has to be the copy.
  const runningBinary = host.running_binary && typeof host.running_binary === 'object' ? host.running_binary : null
  const runningBinaryPath = String(runningBinary?.path || '').trim()
  const runningOutsidePackage = runningBinary?.matches_package === false && Boolean(runningBinaryPath)
  if (installedVersion && isInstalledNewerThanRunning(installedVersion, currentVersion)) {
    status = 'restart_required'
    statusLabel = runningOutsidePackage ? 'Installed, running a copy' : 'Installed, not running'
    summary = runningOutsidePackage
      ? `Polaris ${installedVersion} is installed but this host is running ${currentVersion} from ${runningBinaryPath}, a copy outside the package. Refresh that copy from the package or remove the service drop-in, then restart Polaris.`
      : `Polaris ${installedVersion} is installed but this host is still running ${currentVersion}. Restart Polaris to use it.`
  }
  if (status !== 'restart_required' && hostBehindConsole) {
    ({ status, statusLabel, summary } = hostBehindConsole)
  }

  const asset = selectReleaseAsset(candidateRelease, host)
  const packageFamily = normalizeToken(asset?.packageFamily || inferPackageFamily(host))
  // A configured repository wins over the download command: it upgrades the
  // same package with one line and no exact filename to get wrong. The download
  // path stays for hosts without the repository, which is still every host that
  // has not opted in.
  // A repository never carries a prerelease: the publisher refuses to assemble one, so
  // `dnf upgrade polaris` on a beta offer answers "nothing to do" and reads like the beta failed
  // to install. A prerelease is always the download path.
  const repositoryCommand = candidateRelease.prerelease ? '' : buildRepositoryUpgradeCommand(host)
  const kmsAsset = selectKmsReleaseAsset(candidateRelease, host, asset)
  const installCommand = repositoryCommand ||
    (asset ? buildManualInstallCommand(asset, { ...host, packageFamily }, kmsAsset) : '')
  const releaseUrl = candidateRelease.html_url || ''
  const kmsHelperMissingFromRelease = host.kms_helper_installed === true && Boolean(asset) && !kmsAsset
  const action = buildActionMetadata(status, asset, installCommand, releaseUrl, kmsHelperMissingFromRelease)

  return {
    status,
    statusLabel,
    summary,
    currentVersion,
    latestVersion: versionFromRelease(candidateRelease),
    releaseUrl,
    asset,
    assetDigest: asset?.digest || '',
    packageFamily,
    packageLabel: PACKAGE_LABELS[packageFamily] || '',
    installCommand,
    canCopyInstallCommand: Boolean(installCommand),
    manualInstallOnly: true,
    runningBinaryPath,
    runningOutsidePackage,
    ...action,
  }
}

export function updateStatusLightClass(statusTone) {
  switch (statusTone) {
    case 'update':
      return 'bg-ice shadow-[0_0_18px_color-mix(in_srgb,var(--color-ice)_75%,transparent)] animate-pulse'
    case 'ahead':
      return 'bg-accent shadow-[0_0_14px_color-mix(in_srgb,var(--color-accent)_55%,transparent)]'
    case 'restart':
      return 'bg-ice shadow-[0_0_14px_color-mix(in_srgb,var(--color-ice)_55%,transparent)]'
    case 'warning':
      return 'bg-warning shadow-[0_0_14px_color-mix(in_srgb,var(--color-warning)_55%,transparent)]'
    case 'disabled':
      return 'bg-storm/60'
    default:
      return 'bg-success shadow-[0_0_14px_color-mix(in_srgb,var(--color-success)_55%,transparent)]'
  }
}
