import PolarisVersion from './polaris_version'

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

  const fallback = assets.find((asset) => {
    if (family === 'arch') return /Polaris-arch-x86_64\.pkg\.tar\.zst$/.test(asset.name)
    if (family === 'fedora') return /Polaris-fedora\d+-x86_64\.rpm$/.test(asset.name)
    if (family === 'ubuntu') return /Polaris-ubuntu24\.04-x86_64\.deb$/.test(asset.name)
    return false
  })
  return fallback || null
}

function isSafeAssetForFamily(asset, family) {
  const fileName = String(asset?.name || '')
  const rawUrl = String(asset?.browser_download_url || '')
  const expectedName = family === 'steamos'
    ? /^Polaris-steamos3\.8-x86_64\.pkg\.tar\.zst$/
    : family === 'arch'
      ? /^Polaris-arch-x86_64\.pkg\.tar\.zst$/
      : family === 'fedora'
        ? /^Polaris-fedora\d+-x86_64\.rpm$/
        : family === 'ubuntu'
          ? /^Polaris-ubuntu24\.04-x86_64\.deb$/
          : null
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

export function buildManualInstallCommand(asset, host = {}) {
  if (!asset?.name || !asset?.browser_download_url) return ''

  const family = normalizeToken(asset.packageFamily || host.packageFamily || inferPackageFamily(host))
  if (!isSafeAssetForFamily(asset, family)) return ''
  const fileName = asset.name
  const downloadUrl = asset.browser_download_url
  const lines = [`wget --output-document=./${fileName} ${downloadUrl} &&`]

  if (family === 'steamos') {
    lines.push('(')
    lines.push('set -e')
    lines.push("trap 'sudo steamos-readonly enable' EXIT")
    lines.push('sudo steamos-readonly disable || exit $?')
    lines.push(`sudo pacman -U ./${fileName} || exit $?`)
    lines.push('sudo -H polaris --setup-host || exit $?')
    lines.push('sudo steamos-readonly enable || exit $?')
    lines.push('trap - EXIT')
    lines.push(') &&')
    lines.push('systemctl --user enable --now polaris')
    return lines.join('\n')
  }

  if (family === 'arch') {
    lines.push(`sudo pacman -U ./${fileName} &&`)
  } else if (family === 'fedora') {
    lines.push(`sudo dnf install "./${fileName}" &&`)
  } else if (family === 'ubuntu') {
    lines.push(`sudo apt install ./${fileName} &&`)
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


function buildActionMetadata(status, asset, installCommand, releaseUrl) {
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
      primaryActionSummary: 'A newer release is available, but Polaris could not match a local package for this host.',
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

export function chooseCandidateRelease({ latestRelease, prereleaseRelease, includePrereleases = false, currentVersion = '' } = {}) {
  if (includePrereleases && prereleaseRelease && isReleaseGreater(prereleaseRelease, latestRelease ? versionFromRelease(latestRelease) : currentVersion, true)) {
    return prereleaseRelease
  }
  return latestRelease || null
}

export function buildUpdateCenterState({ currentVersion = '', latestRelease = null, prereleaseRelease = null, includePrereleases = false, host = {}, disabled = false } = {}) {
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
  if (installedVersion && isInstalledNewerThanRunning(installedVersion, currentVersion)) {
    status = 'restart_required'
    statusLabel = 'Installed, not running'
    summary = `Polaris ${installedVersion} is installed but this host is still running ${currentVersion}. Restart Polaris to use it.`
  }

  const asset = selectReleaseAsset(candidateRelease, host)
  const packageFamily = normalizeToken(asset?.packageFamily || inferPackageFamily(host))
  // A configured repository wins over the download command: it upgrades the
  // same package with one line and no exact filename to get wrong. The download
  // path stays for hosts without the repository, which is still every host that
  // has not opted in.
  const repositoryCommand = buildRepositoryUpgradeCommand(host)
  const installCommand = repositoryCommand ||
    (asset ? buildManualInstallCommand(asset, { ...host, packageFamily }) : '')
  const releaseUrl = candidateRelease.html_url || ''
  const action = buildActionMetadata(status, asset, installCommand, releaseUrl)

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
