import PolarisVersion from './polaris_version'

const PACKAGE_LABELS = {
  arch: 'Arch/CachyOS package',
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

export function inferPackageFamily(host = {}) {
  if (normalizeToken(host.platform) !== 'linux') return ''
  if (host.package_family) return normalizeToken(host.package_family)

  const tokens = distroTokens(host)
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
  if (host.recommended_asset_name) return host.recommended_asset_name

  const family = inferPackageFamily(host)
  const versionId = String(host.distro?.version_id || '').trim()
  if (family === 'arch') return 'Polaris-arch-x86_64.pkg.tar.zst'
  if (family === 'fedora' && versionId) return `Polaris-fedora${versionId}-x86_64.rpm`
  if (family === 'ubuntu' && versionId.startsWith('24.04')) return 'Polaris-ubuntu24.04-x86_64.deb'
  return ''
}

export function selectReleaseAsset(release, host = {}) {
  const assets = Array.isArray(release?.assets) ? release.assets : []
  const preferredName = preferredAssetName(host)
  if (preferredName) {
    const exact = assets.find((asset) => asset.name === preferredName)
    if (exact) return { ...exact, packageFamily: inferPackageFamily(host) }
  }

  const family = inferPackageFamily(host)
  const fallback = assets.find((asset) => {
    if (family === 'arch') return /Polaris-arch-x86_64\.pkg\.tar\.zst$/.test(asset.name)
    if (family === 'fedora') return /Polaris-fedora\d+-x86_64\.rpm$/.test(asset.name)
    if (family === 'ubuntu') return /Polaris-ubuntu24\.04-x86_64\.deb$/.test(asset.name)
    return false
  })
  return fallback ? { ...fallback, packageFamily: family } : null
}

export function buildManualInstallCommand(asset, host = {}) {
  if (!asset?.name || !asset?.browser_download_url) return ''

  const family = normalizeToken(asset.packageFamily || host.packageFamily || inferPackageFamily(host))
  const fileName = asset.name
  const lines = [`wget ${asset.browser_download_url}`]

  if (family === 'arch') {
    lines.push(`sudo pacman -U ./${fileName}`)
  } else if (family === 'fedora') {
    lines.push(`sudo dnf install "./${fileName}"`)
  } else if (family === 'ubuntu') {
    lines.push(`sudo apt install ./${fileName}`)
  } else {
    return ''
  }

  lines.push('sudo polaris --setup-host')
  lines.push('systemctl --user restart polaris')
  return lines.join('\n')
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
    }
  }

  const candidateRelease = chooseCandidateRelease({ latestRelease, prereleaseRelease, includePrereleases, currentVersion })
  if (!currentVersion || !candidateRelease) {
    return {
      status: 'unavailable',
      statusLabel: 'Update status unavailable',
      summary: 'Polaris could not check GitHub releases from this browser session.',
      latestVersion: versionFromRelease(candidateRelease),
      releaseUrl: candidateRelease?.html_url || '',
      asset: null,
      packageLabel: '',
      installCommand: '',
      canCopyInstallCommand: false,
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

  const asset = selectReleaseAsset(candidateRelease, host)
  const packageFamily = normalizeToken(asset?.packageFamily || inferPackageFamily(host))
  const installCommand = asset ? buildManualInstallCommand(asset, { ...host, packageFamily }) : ''

  return {
    status,
    statusLabel,
    summary,
    currentVersion,
    latestVersion: versionFromRelease(candidateRelease),
    releaseUrl: candidateRelease.html_url || '',
    asset,
    assetDigest: asset?.digest || '',
    packageFamily,
    packageLabel: PACKAGE_LABELS[packageFamily] || '',
    installCommand,
    canCopyInstallCommand: Boolean(installCommand),
    manualInstallOnly: true,
  }
}
