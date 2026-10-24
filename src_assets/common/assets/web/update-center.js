import PolarisVersion from './polaris_version.js'

/* global __POLARIS_CONSOLE_VERSION__ */
// The release this console was built with, set by the build; empty under test
// and in a console built by hand, which switches the comparison off.
const BUILT_CONSOLE_VERSION = typeof __POLARIS_CONSOLE_VERSION__ === 'string' ? __POLARIS_CONSOLE_VERSION__ : ''

const PACKAGE_LABELS = {
  arch: 'Arch/CachyOS package',
  steamos: 'SteamOS package',
  fedora: 'Fedora RPM package',
  ubuntu: 'Ubuntu 24.04 DEB package',
}

// Each SteamOS release has its own package: it links that release's Boost, so the 3.8 package
// installs on 3.9 and then cannot start. A host gets the package for its own release or none.
const STEAMOS_RELEASES = ['3.8', '3.9']

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

function steamOsRelease(host = {}) {
  const versionId = normalizeToken(host.distro?.version_id)
  return STEAMOS_RELEASES.find((release) => versionId === release || versionId.startsWith(`${release}.`)) || ''
}

function steamOsPackageName(host = {}) {
  const release = steamOsRelease(host)
  return release ? `Polaris-steamos${release}-x86_64.pkg.tar.zst` : ''
}

function packageLabel(packageFamily, host = {}) {
  if (packageFamily === 'steamos' && steamOsRelease(host)) return `SteamOS ${steamOsRelease(host)} package`
  return PACKAGE_LABELS[packageFamily] || ''
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
    base: /^Polaris-steamos3\.[89]-x86_64\.pkg\.tar\.zst$/,
    kms: /^Polaris-kms-steamos3\.[89]-x86_64\.pkg\.tar\.zst$/,
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
  if (family === 'steamos') return steamOsPackageName(host)
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
    const wanted = steamOsPackageName(host)
    if (!wanted) return null
    return assets.find((asset) => asset.name === wanted) || null
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

// The channel counts: the release replaces its own beta through an ordinary upgrade, and until the
// host restarts, 1.4.14 is installed while 1.4.14-beta.3 runs. On the release number alone those are
// the same, and the Update Center would offer the release this host already has.
function isInstalledNewerThanRunning(installedVersion, runningVersion) {
  if (!installedVersion || !runningVersion) return false
  try {
    return new PolarisVersion(null, installedVersion).isGreater(new PolarisVersion(null, runningVersion), true)
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
// A development build's commit suffix does not parse as a version, so it says nothing here. The
// console is named exactly as it was built, channel included: 1.4.14-rc.1 is not 1.4.14.
function describeHostBehindConsole(consoleVersion, runningVersion) {
  if (!isInstalledNewerThanRunning(consoleVersion, runningVersion)) return null
  return {
    status: 'restart_required',
    statusLabel: 'Console is newer than the host',
    summary: `This console came with Polaris ${consoleVersion}, but the host process answering it is ${runningVersion}. The package was updated and the service still runs an older binary. Restart Polaris. If it still shows ${runningVersion}, the service runs a copy outside the package, on Bazzite /usr/local/bin/polaris-kms: run sudo -H polaris --setup-host, then restart again.`,
  }
}

export function isPrereleaseOptIn(value) {
  return value === true || value === 'enabled'
}

function plainVersion(version) {
  return String(version || '').trim().replace(/^v(?=\d)/i, '')
}

// A local build carries the commit after the release it was built from, 1.4.13.36e05224 or
// 1.4.14-beta.1.36e05224.dirty. The release is the part a published beta is compared with.
function developmentBuildBase(version) {
  const match = plainVersion(version).match(/^(\d+\.\d+\.\d+.*?)\.[0-9a-f]{7,40}(?:\.dirty)?$/i)
  return match ? match[1] : ''
}

// With betas included and nothing newer to offer, "on the latest public release" reads as though the
// beta channel was never checked. So the status line says what the check found: no newer beta, and
// which build is the newest one.
function betaChannelNote({ includePrereleases, status, candidateRelease, currentVersion }) {
  if (!isPrereleaseOptIn(includePrereleases) || status !== 'current' || !candidateRelease) return ''
  const base = developmentBuildBase(currentVersion)
  const installed = base || plainVersion(currentVersion)
  const stable = plainVersion(versionFromRelease(candidateRelease))
  const found = candidateRelease.prerelease
    ? `No beta newer than ${installed} is published yet.`
    : `No beta newer than ${installed} is published yet, so stable ${stable} is the newest build.`
  if (base) {
    return `This host runs a development build of ${base}. ${found}`
  }
  return candidateRelease.prerelease ? `${found} This host runs the newest beta.` : found
}

export function chooseCandidateRelease({ latestRelease, prereleaseRelease, includePrereleases = false, currentVersion = '' } = {}) {
  if (isPrereleaseOptIn(includePrereleases) && prereleaseRelease && !prereleaseRelease.draft && isReleaseGreater(prereleaseRelease, latestRelease ? versionFromRelease(latestRelease) : currentVersion, true)) {
    return prereleaseRelease
  }
  return latestRelease || null
}

function buildLegacyUpdateCenterState({ currentVersion = '', latestRelease = null, prereleaseRelease = null, includePrereleases = false, host = {}, disabled = false, consoleVersion = BUILT_CONSOLE_VERSION } = {}) {
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
  if (isReleaseGreater(candidateRelease, currentVersion, true)) {
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
  const channelNote = betaChannelNote({ includePrereleases, status, candidateRelease, currentVersion })

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
    packageLabel: packageLabel(packageFamily, host),
    installCommand,
    canCopyInstallCommand: Boolean(installCommand),
    manualInstallOnly: true,
    runningBinaryPath,
    runningOutsidePackage,
    // The helper is built for this exact Polaris, so a host that has it installs both, and the
    // Package card lists what the install command takes rather than half of it.
    kmsAsset,
    kmsAssetDigest: kmsAsset?.digest || '',
    kmsHelperMissingFromRelease,
    channelNote,
    ...action,
    ...(channelNote ? { primaryActionSummary: channelNote } : {}),
  }
}

// A helper-capable host selects its own official release. Browser GitHub data
// remains useful to guided/older hosts but does not choose a privileged update.
export function buildUpdateCenterState(options = {}) {
  const state = buildLegacyUpdateCenterState(options)
  const plan = options.hostPlan
  const result = { ...state }
  if (plan?.route === 'helper') {
    const assets = plan.assets || []
    const asAsset = (asset) => asset ? { ...asset, browser_download_url: asset.url } : null
    const asset = asAsset(assets.find((entry) => entry.package === 'polaris'))
    const kmsAsset = asAsset(assets.find((entry) => entry.package === 'polaris-kms'))
    const restart = state.status === 'restart_required'
    Object.assign(result, {
      status: restart ? state.status : plan.update_needed ? 'update_available' : 'current',
      statusLabel: restart ? state.statusLabel : plan.update_needed ? 'Update available' : 'Up to date',
      statusTone: restart ? 'restart' : plan.update_needed ? 'update' : 'current',
      statusLightLabel: restart ? state.statusLightLabel : plan.update_needed ? 'Update available' : 'Up to date',
      primaryActionKind: restart ? 'scroll_to_update_center' : 'review_host_update',
      primaryActionLabel: plan.update_needed && !restart ? 'Review update' : 'Update details',
      primaryActionSummary: restart ? state.primaryActionSummary : plan.update_needed
        ? 'Review the selected release before asking for approval on this PC.' : 'The selected release is installed.',
      latestVersion: plan.version, releaseUrl: plan.release_url, asset, kmsAsset,
      assetDigest: asset?.digest || '', kmsAssetDigest: kmsAsset?.digest || '',
      kmsHelperMissingFromRelease: false, installCommand: '', canCopyInstallCommand: false, manualInstallOnly: false,
      packageLabel: 'Fedora 44 RPM release assets',
    })
  }
  const journey = options.hostJourney
  if (journey?.hasJob) Object.assign(result, {
    status: journey.needsFinish ? 'restart_required' : journey.active ? 'updating' : journey.finished ? 'current' : 'unavailable',
    statusLabel: journey.title, primaryActionSummary: journey.message,
    primaryActionLabel: 'Update details', primaryActionKind: 'scroll_to_update_center',
    statusTone: journey.active ? 'update' : journey.needsFinish ? 'restart' : journey.finished ? 'current' : 'warning',
    statusLightLabel: journey.title,
  })
  return result
}

export function selectedReleaseLabel(plan = {}) {
  return plan.release_kind === 'rc' || /-rc\./.test(plan.version || '') ? 'Release candidate'
    : plan.release_kind === 'beta' || /-beta\./.test(plan.version || '') ? 'Beta' : 'Stable'
}

export function hostUpdatePollDelay({ visible = true, active = false, reviewing = false, restarting = false,
  restartTimedOut = false, disconnected = false, capable = true } = {}) {
  if (!visible || !capable) return null
  if (restartTimedOut) return 15000
  return active || restarting || disconnected ? 3000 : reviewing ? 5000 : 60000
}

export function describeHostUpdate(snapshot = {}, plan = null, local = {}) {
  const raw = snapshot.job || {}
  const rawCheck = raw.check || {}
  // A journal describes an earlier transaction, never a fresh package/process readback.
  const installed = snapshot.installed || {}
  const running = snapshot.running_version || ''
  const installedVersion = installed.polaris?.state === 'installed' ? installed.polaris.version : ''
  const consistent = Boolean(installedVersion && (installed['polaris-kms']?.state === 'absent' ||
    (installed['polaris-kms']?.state === 'installed' && installed['polaris-kms'].version === installedVersion)))
  const restartConfirmed = raw.state === 'restarting' && consistent && installedVersion === running &&
    installedVersion === (local.finishTarget || raw.version)
  const rawActive = ['waiting_for_approval', 'running', 'restarting'].includes(raw.state) && !restartConfirmed
  const jobVersion = raw.version || rawCheck.version || (raw.state === 'installed' ? installedVersion : '')
  const recovered = consistent && installedVersion === running && installedVersion === plan?.version
  const currentTerminal = !local.reviewing && !recovered && (!plan?.version || jobVersion === plan.version)
  const job = rawActive || currentTerminal ? raw : {}
  const check = job.check || {}
  // Confirm this restart independently of a newer offer/operation from another tab.
  const finishConfirmed = Boolean(local.finishTarget && !local.disconnected && consistent &&
    installedVersion === running && installedVersion === local.finishTarget)
  const finished = Boolean(!rawActive && !local.disconnected && consistent && installedVersion === running &&
    (!plan?.update_needed || plan.version === installedVersion) &&
    (local.finishTarget ? installedVersion === local.finishTarget : job.state === 'installed'))
  const active = rawActive || Boolean(local.restarting && !finishConfirmed && !finished)
  const needsFinish = Boolean(installedVersion && running && installedVersion !== running && !rawActive)
  const needsAttention = Object.keys(installed).length > 0 && !consistent
  const busyReason = snapshot.busy?.reasons?.join(' ') || ''
  const stage = check.stage || job.state || ''
  let title = plan?.update_needed ? 'Update available' : plan?.route === 'helper' ? 'Up to date' : 'Update this PC'
  let message = plan?.refusal?.message || (plan?.update_needed
    ? 'Choose Review to check the selected release on this host.' : 'The host checked the selected release.')
  if (local.reviewing && plan?.route === 'helper') {
    title = plan.update_needed ? `Review Polaris ${plan.version}` : `Polaris ${plan.version} is up to date`
    message = plan.update_needed ? 'Install only the listed older packages, or stop before changing anything. Polaris keeps running until you choose Finish.'
      : 'No package changes are needed for this release.'
  }
  if (job.state) {
    title = ({ waiting_for_approval: 'Waiting for host approval', running: 'Updating this PC', installed: 'Installed, not running',
      restarting: 'Restarting Polaris', cancelled: 'Approval cancelled', timed_out: 'Approval timed out', no_agent: 'Host approval unavailable',
      not_authorized: 'Host approval refused', refused: 'Update stopped', failed: 'Update stopped', unknown: 'Update result unknown' })[job.state] || 'Update stopped'
    message = job.message || 'Read the installed package state, then review again.'
  }
  if (job.state === 'failed' && check.transaction_attempted === false)
    message = `Polaris stopped before installing, so no packages were changed. ${message}`
  if (job.state === 'running' && stage === 'installing')
    message = 'Installing the listed RPM packages. This cannot be stopped safely now. Streams cannot start until it finishes.'
  if (needsFinish && job.state !== 'failed') {
    title = `Polaris ${installedVersion} is installed`
    message = `You are still running ${running}. Finish restarts Polaris and briefly disconnects this page. Start streams again from your devices afterwards.`
  }
  const recovery = needsAttention ? 'Read the package versions below. Polaris will not force a reinstall or remove package entries. Use Fedora software management or ask your system administrator to repair partial or unreadable entries, then review again.' : ''
  if (needsAttention && !active) {
    title = 'Installed package state needs attention'
    message = plan?.route === 'helper' && plan.update_needed ? 'Review an update of the older packages to match the selected release.' : recovery
  }
  if (finished) {
    title = `Polaris ${installedVersion} is running`
    message = 'The installed version is now running. You can start streams again.'
  } else if (local.restarting && !finishConfirmed) {
    title = 'Restarting Polaris'
    message = local.restartTimedOut ? 'Polaris has not come back yet. Check the PC running it. This page keeps checking in the background.' : 'Restarting Polaris. This page will try to reconnect.'
  } else if (local.disconnected) {
    title = 'Connection lost'
    message = 'The update result is unknown until this page can read the host again. This page keeps checking.'
  }
  return {
    title, message, stage, active, finished, finishConfirmed, needsFinish, needsAttention, recovery,
    hasJob: Boolean(job.state || needsFinish || needsAttention || finished || local.disconnected || (local.restarting && !finishConfirmed)),
    installed: ['polaris', 'polaris-kms'].map((name) => ({ name, state: installed[name]?.state || 'unknown',
      version: installed[name]?.version || '', versions: installed[name]?.versions || [] })), busyReason,
    canInstall: Boolean(local.reviewing && plan?.route === 'helper' && plan.review_id && plan.update_needed && snapshot.available && !active && !local.disconnected),
    canCancel: job.state === 'waiting_for_approval' && Boolean(job.request_id) && !local.disconnected,
    canFinish: Boolean(needsFinish && consistent && snapshot.finish_available && !active && !local.disconnected),
    canReview: !active && !local.disconnected,
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
