import { readFileSync } from 'node:fs'
import { join } from 'node:path'
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



  it('surfaces a front-page update CTA and status light metadata', () => {
    const state = buildUpdateCenterState({
      currentVersion: '1.2.1',
      latestRelease: release,
      host: {
        platform: 'linux',
        distro: { id: 'cachyos', id_like: 'arch', version_id: '2026' },
      },
    })

    expect(state.primaryActionLabel).toBe('Update')
    expect(state.primaryActionKind).toBe('copy_install_command')
    expect(state.statusTone).toBe('update')
    expect(state.statusLightLabel).toBe('Update available')
  })

  it('keeps the front-page Update button copy-only and wires refresh affordances', () => {
    const source = readFileSync(join(process.cwd(), 'src_assets/common/assets/web/views/DashboardView.vue'), 'utf8')

    expect(source).toContain('data-update-center-cta')
    expect(source).toContain('data-update-status-light')
    expect(source).toContain('@click="handlePrimaryUpdateAction"')
    expect(source).toContain('@click="refreshUpdateStatus"')
    expect(source).toContain('scrollIntoView')
    expect(source).toContain('buildUpdateCenterState')
    expect(source).not.toMatch(/POST['"]\s*,\s*['"].*update/i)
  })



  it('allows the browser release check through the web UI content security policy', () => {
    const source = readFileSync(join(process.cwd(), 'src/confighttp.cpp'), 'utf8')

    expect(source).toContain('https://api.github.com')
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
