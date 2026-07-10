import {
  buildManualInstallCommand,
  buildUpdateCenterState,
  selectReleaseAsset,
} from './update-center.js'

const release = {
  tag_name: 'v1.2.2',
  name: 'Polaris v1.2.2',
  html_url: 'https://github.com/papi-ux/polaris/releases/tag/v1.2.2',
  prerelease: false,
  assets: [
    {
      name: 'Polaris-arch-x86_64.pkg.tar.zst',
      browser_download_url: 'https://example.test/Polaris-arch-x86_64.pkg.tar.zst',
      digest: 'sha256:archdigest',
    },
    {
      name: 'Polaris-fedora44-x86_64.rpm',
      browser_download_url: 'https://example.test/Polaris-fedora44-x86_64.rpm',
      digest: 'sha256:fedoradigest',
    },
    {
      name: 'Polaris-ubuntu24.04-x86_64.deb',
      browser_download_url: 'https://example.test/Polaris-ubuntu24.04-x86_64.deb',
      digest: 'sha256:ubuntudigest',
    },
  ],
}

describe('Update Center release awareness', () => {
  it('detects a stable update and selects the Arch/CachyOS package', () => {
    const state = buildUpdateCenterState({
      currentVersion: '1.2.1',
      latestRelease: release,
      host: {
        platform: 'linux',
        distro: { id: 'cachyos', id_like: 'arch', version_id: '2026' },
      },
    })

    expect(state.status).toBe('update_available')
    expect(state.latestVersion).toBe('v1.2.2')
    expect(state.asset.name).toBe('Polaris-arch-x86_64.pkg.tar.zst')
    expect(state.packageLabel).toBe('Arch/CachyOS package')
    expect(state.installCommand).toContain('wget https://example.test/Polaris-arch-x86_64.pkg.tar.zst')
    expect(state.installCommand).toContain('sudo pacman -U ./Polaris-arch-x86_64.pkg.tar.zst')
    expect(state.installCommand).toContain('sudo polaris --setup-host')
    expect(state.installCommand).toContain('systemctl --user restart polaris')
    expect(state.installCommand).not.toMatch(/curl\s+[^\n|]+\|\s*sudo/i)
  })

  it('selects the matching Fedora package by host version', () => {
    const asset = selectReleaseAsset(release, {
      platform: 'linux',
      distro: { id: 'bazzite', id_like: 'fedora', version_id: '44' },
    })

    expect(asset.name).toBe('Polaris-fedora44-x86_64.rpm')
    expect(buildManualInstallCommand(asset, { packageFamily: 'fedora' })).toContain(
      'sudo dnf install "./Polaris-fedora44-x86_64.rpm"',
    )
  })

  it('selects the Ubuntu 24.04 package for Debian-family tester hosts', () => {
    const state = buildUpdateCenterState({
      currentVersion: '1.2.1',
      latestRelease: release,
      host: {
        platform: 'linux',
        distro: { id: 'ubuntu', id_like: 'debian', version_id: '24.04' },
      },
    })

    expect(state.asset.name).toBe('Polaris-ubuntu24.04-x86_64.deb')
    expect(state.installCommand).toContain('sudo apt install ./Polaris-ubuntu24.04-x86_64.deb')
  })

  it('does not offer a prerelease unless the user opted into prerelease notifications', () => {
    const prerelease = { ...release, tag_name: 'v1.3.0-beta.1', prerelease: true }

    expect(buildUpdateCenterState({ currentVersion: '1.2.2', latestRelease: release, prereleaseRelease: prerelease }).status)
      .toBe('current')
    expect(buildUpdateCenterState({ currentVersion: '1.2.2', latestRelease: release, prereleaseRelease: prerelease, includePrereleases: true }).status)
      .toBe('update_available')
  })

  it('stays informational on unsupported platforms instead of attempting auto-install', () => {
    const state = buildUpdateCenterState({
      currentVersion: '1.2.1',
      latestRelease: release,
      host: { platform: 'windows', distro: {} },
    })

    expect(state.status).toBe('update_available')
    expect(state.asset).toBeNull()
    expect(state.installCommand).toBe('')
    expect(state.canCopyInstallCommand).toBe(false)
  })
})
