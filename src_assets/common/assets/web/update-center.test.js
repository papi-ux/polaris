import { spawnSync } from 'node:child_process'
import { chmodSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'
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
      name: 'Polaris-steamos3.8-x86_64.pkg.tar.zst',
      browser_download_url: 'https://example.test/Polaris-steamos3.8-x86_64.pkg.tar.zst',
      digest: 'sha256:steamosdigest',
    },
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

const mutablePackageCases = [
  {
    family: 'arch',
    fileName: 'Polaris-arch-x86_64.pkg.tar.zst',
    installLine: 'sudo pacman -U ./Polaris-arch-x86_64.pkg.tar.zst &&',
    installLog: 'sudo pacman -U ./Polaris-arch-x86_64.pkg.tar.zst',
  },
  {
    family: 'fedora',
    fileName: 'Polaris-fedora44-x86_64.rpm',
    installLine: 'sudo dnf install "./Polaris-fedora44-x86_64.rpm" &&',
    installLog: 'sudo dnf install ./Polaris-fedora44-x86_64.rpm',
  },
  {
    family: 'ubuntu',
    fileName: 'Polaris-ubuntu24.04-x86_64.deb',
    installLine: 'sudo apt install ./Polaris-ubuntu24.04-x86_64.deb &&',
    installLog: 'sudo apt install ./Polaris-ubuntu24.04-x86_64.deb',
  },
]

const buildSteamOsState = () => buildUpdateCenterState({
  currentVersion: '1.2.1',
  latestRelease: release,
  host: {
    platform: 'linux',
    distro: { id: 'steamos', id_like: 'arch', version_id: '3.8.16' },
  },
})

const runWithFakeInstallCommands = (command, failSudo) => {
  const fixture = mkdtempSync(join(tmpdir(), 'polaris-steamos-update-'))
  try {
    const binDir = join(fixture, 'bin')
    const commandLog = join(fixture, 'commands.log')
    mkdirSync(binDir)

    const commandLines = command.split('\n').map((line) => line.trim()).filter(Boolean)
    const allowedExternalCommands = [
      /^wget --output-document=\.\/Polaris-steamos3\.8-x86_64\.pkg\.tar\.zst https:\/\/example\.test\/Polaris-steamos3\.8-x86_64\.pkg\.tar\.zst &&$/,
      /^sudo steamos-readonly disable(?: \|\| exit \$\?)?$/,
      /^sudo pacman -U \.\/Polaris-steamos3\.8-x86_64\.pkg\.tar\.zst \|\| exit \$\?$/,
      /^sudo -H polaris --setup-host \|\| exit \$\?$/,
      /^sudo steamos-readonly enable(?: \|\| exit \$\?)?$/,
      /^systemctl --user enable --now polaris$/,
    ]
    const allowedShellControl = new Set([
      '(',
      ')',
      ') &&',
      'set -e',
      "trap 'sudo steamos-readonly enable' EXIT",
      'trap - EXIT',
    ])
    const unsafeLines = commandLines.filter((line) => (
      !allowedShellControl.has(line)
      && !allowedExternalCommands.some((pattern) => pattern.test(line))
    ))
    expect(unsafeLines, 'generated fixture command must contain only mocked external commands and shell control').toEqual([])

    writeFileSync(
      join(binDir, 'wget'),
      `#!/usr/bin/env bash
printf 'wget %s\\n' "$*" >> "$POLARIS_COMMAND_LOG"
if [[ "$POLARIS_FAIL_SUDO" == "download" ]]; then
  exit 44
fi
`,
    )
    writeFileSync(
      join(binDir, 'sudo'),
      `#!/usr/bin/env bash
printf 'sudo %s\\n' "$*" >> "$POLARIS_COMMAND_LOG"
if [[ "$POLARIS_FAIL_SUDO" == "pacman" && "$1" == "pacman" && "$2" == "-U" ]]; then
  exit 41
fi
if [[ "$POLARIS_FAIL_SUDO" == "setup-host" && "$1" == "-H" && "$2" == "polaris" && "$3" == "--setup-host" ]]; then
  exit 42
fi
if [[ "$POLARIS_FAIL_SUDO" == "readonly-disable" && "$1" == "steamos-readonly" && "$2" == "disable" ]]; then
  exit 40
fi
if [[ "$POLARIS_FAIL_SUDO" == "readonly-enable" && "$1" == "steamos-readonly" && "$2" == "enable" ]]; then
  exit 43
fi
exit 0
`,
    )
    writeFileSync(
      join(binDir, 'systemctl'),
      '#!/usr/bin/env bash\nprintf \'systemctl %s\\n\' "$*" >> "$POLARIS_COMMAND_LOG"\n',
    )
    for (const name of ['wget', 'sudo', 'systemctl']) {
      chmodSync(join(binDir, name), 0o755)
    }

    const result = spawnSync('bash', ['-c', command], {
      cwd: fixture,
      encoding: 'utf8',
      env: {
        ...process.env,
        HOME: fixture,
        PATH: `${binDir}:${process.env.PATH || ''}`,
        POLARIS_COMMAND_LOG: commandLog,
        POLARIS_FAIL_SUDO: failSudo,
      },
    })
    const commands = readFileSync(commandLog, 'utf8').trim().split('\n').filter(Boolean)
    return { commands, result }
  } finally {
    rmSync(fixture, { force: true, recursive: true })
  }
}

const runWithFakeMutableInstallCommands = (command, failStep) => {
  const fixture = mkdtempSync(join(tmpdir(), 'polaris-mutable-update-'))
  try {
    const binDir = join(fixture, 'bin')
    const commandLog = join(fixture, 'commands.log')
    mkdirSync(binDir)

    writeFileSync(
      join(binDir, 'wget'),
      `#!/usr/bin/env bash
printf 'wget %s\\n' "$*" >> "$POLARIS_COMMAND_LOG"
if [[ "$POLARIS_FAIL_STEP" == "download" ]]; then
  exit 44
fi
`,
    )
    writeFileSync(
      join(binDir, 'sudo'),
      `#!/usr/bin/env bash
printf 'sudo %s\\n' "$*" >> "$POLARIS_COMMAND_LOG"
if [[ "$POLARIS_FAIL_STEP" == "install" && "$1" != "-H" ]]; then
  exit 41
fi
if [[ "$POLARIS_FAIL_STEP" == "setup-host" && "$1" == "-H" && "$2" == "polaris" && "$3" == "--setup-host" ]]; then
  exit 42
fi
exit 0
`,
    )
    writeFileSync(
      join(binDir, 'systemctl'),
      '#!/usr/bin/env bash\nprintf \'systemctl %s\\n\' "$*" >> "$POLARIS_COMMAND_LOG"\n',
    )
    for (const name of ['wget', 'sudo', 'systemctl']) {
      chmodSync(join(binDir, name), 0o755)
    }

    const result = spawnSync('bash', ['-c', command], {
      cwd: fixture,
      encoding: 'utf8',
      env: {
        ...process.env,
        HOME: fixture,
        PATH: `${binDir}:${process.env.PATH || ''}`,
        POLARIS_COMMAND_LOG: commandLog,
        POLARIS_FAIL_STEP: failStep,
      },
    })
    const commands = readFileSync(commandLog, 'utf8').trim().split('\n').filter(Boolean)
    return { commands, result }
  } finally {
    rmSync(fixture, { force: true, recursive: true })
  }
}

const expectFailureSafeSteamOsCommand = (failSudo, failedCommand) => {
  const state = buildSteamOsState()
  const { commands, result } = runWithFakeInstallCommands(state.installCommand, failSudo)
  const disableReadOnly = commands.indexOf('sudo steamos-readonly disable')
  const failure = commands.indexOf(failedCommand)
  const restoreReadOnly = commands.indexOf('sudo steamos-readonly enable', failure + 1)

  expect(disableReadOnly).toBeGreaterThanOrEqual(0)
  expect(failure).toBeGreaterThan(disableReadOnly)
  expect(restoreReadOnly).toBeGreaterThan(failure)
  expect(result.status, `stdout: ${result.stdout}\nstderr: ${result.stderr}`).not.toBe(0)
  expect(commands.some((command) => /(?:^|\s)systemctl(?:\s|$)/.test(command))).toBe(false)
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
    expect(state.installCommand).toContain('wget --output-document=./Polaris-arch-x86_64.pkg.tar.zst https://example.test/Polaris-arch-x86_64.pkg.tar.zst &&')
    expect(state.installCommand).toContain('sudo pacman -U ./Polaris-arch-x86_64.pkg.tar.zst')
    expect(state.installCommand).toContain('sudo -H polaris --setup-host')
    expect(state.installCommand).toContain('systemctl --user restart polaris')
    expect(state.installCommand).not.toMatch(/curl\s+[^\n|]+\|\s*sudo/i)
  })

  it('prefers the SteamOS 3.8 package before the Arch fallback and restores read-only mode', () => {
    const state = buildSteamOsState()

    expect(state.packageFamily).toBe('steamos')
    expect(state.packageLabel).toBe('SteamOS 3.8 package')
    expect(state.asset.name).toBe('Polaris-steamos3.8-x86_64.pkg.tar.zst')

    const commandLines = state.installCommand.split('\n')
    const expectedCommandLines = [
      'wget --output-document=./Polaris-steamos3.8-x86_64.pkg.tar.zst https://example.test/Polaris-steamos3.8-x86_64.pkg.tar.zst &&',
      '(',
      'set -e',
      "trap 'sudo steamos-readonly enable' EXIT",
      'sudo steamos-readonly disable || exit $?',
      'sudo pacman -U ./Polaris-steamos3.8-x86_64.pkg.tar.zst || exit $?',
      'sudo -H polaris --setup-host || exit $?',
      'sudo steamos-readonly enable || exit $?',
      'trap - EXIT',
      ') &&',
      'systemctl --user enable --now polaris',
    ]
    const downloadPackage = commandLines.indexOf('wget --output-document=./Polaris-steamos3.8-x86_64.pkg.tar.zst https://example.test/Polaris-steamos3.8-x86_64.pkg.tar.zst &&')
    const subshellStart = commandLines.indexOf('(')
    const strictMode = commandLines.indexOf('set -e')
    const disableReadOnly = commandLines.indexOf('sudo steamos-readonly disable || exit $?')
    const restoreTrap = commandLines.indexOf("trap 'sudo steamos-readonly enable' EXIT")
    const installPackage = commandLines.indexOf('sudo pacman -U ./Polaris-steamos3.8-x86_64.pkg.tar.zst || exit $?')
    const setupHost = commandLines.indexOf('sudo -H polaris --setup-host || exit $?')
    const restoreReadOnly = commandLines.indexOf('sudo steamos-readonly enable || exit $?')
    const clearRestoreTrap = commandLines.indexOf('trap - EXIT')
    const subshellEnd = commandLines.indexOf(') &&')
    const startService = commandLines.indexOf('systemctl --user enable --now polaris')

    expect(downloadPackage).toBeGreaterThanOrEqual(0)
    expect(subshellStart).toBeGreaterThan(downloadPackage)
    expect(strictMode).toBeGreaterThan(subshellStart)
    expect(restoreTrap).toBeGreaterThan(strictMode)
    expect(disableReadOnly).toBeGreaterThan(restoreTrap)
    expect(installPackage).toBeGreaterThan(disableReadOnly)
    expect(setupHost).toBeGreaterThan(installPackage)
    expect(restoreReadOnly).toBeGreaterThan(setupHost)
    expect(clearRestoreTrap).toBeGreaterThan(restoreReadOnly)
    expect(subshellEnd).toBeGreaterThan(clearRestoreTrap)
    expect(startService).toBeGreaterThan(restoreReadOnly)
    expect(startService).toBeGreaterThan(subshellEnd)
    expect(commandLines, 'SteamOS install side effects must each occur exactly once').toEqual(expectedCommandLines)
  })

  it('does not fall back to the rolling Arch package for other SteamOS versions', () => {
    const asset = selectReleaseAsset(release, {
      platform: 'linux',
      distro: { id: 'steamos', id_like: 'arch', version_id: '3.7.13' },
    })

    expect(asset).toBeNull()
  })

  it('overwrites the exact SteamOS target and skips privileged work when download fails', () => {
    const state = buildSteamOsState()
    const { commands, result } = runWithFakeInstallCommands(state.installCommand, 'download')

    expect(commands).toEqual([
      'wget --output-document=./Polaris-steamos3.8-x86_64.pkg.tar.zst https://example.test/Polaris-steamos3.8-x86_64.pkg.tar.zst',
    ])
    expect(result.status).not.toBe(0)
  })

  it('restores SteamOS read-only mode and skips service startup when pacman fails', () => {
    expectFailureSafeSteamOsCommand(
      'pacman',
      'sudo pacman -U ./Polaris-steamos3.8-x86_64.pkg.tar.zst',
    )
  })

  it('restores read-only mode and skips install side effects when disabling read-only mode fails', () => {
    const state = buildSteamOsState()
    const { commands, result } = runWithFakeInstallCommands(state.installCommand, 'readonly-disable')

    expect(commands).toContain('sudo steamos-readonly disable')
    expect(commands).toContain('sudo steamos-readonly enable')
    expect(commands).not.toContain('sudo pacman -U ./Polaris-steamos3.8-x86_64.pkg.tar.zst')
    expect(commands).not.toContain('sudo -H polaris --setup-host')
    expect(commands).not.toContain('systemctl --user enable --now polaris')
    expect(result.status).not.toBe(0)
  })

  it('restores SteamOS read-only mode and skips service startup when host setup fails', () => {
    expectFailureSafeSteamOsCommand('setup-host', 'sudo -H polaris --setup-host')
  })

  it('retries SteamOS read-only restoration and skips service startup when restoration fails', () => {
    expectFailureSafeSteamOsCommand('readonly-enable', 'sudo steamos-readonly enable')
  })

  it('ignores Arch recommendations on SteamOS and only supports the exact 3.8 asset', () => {
    const steamOsHost = {
      platform: 'linux',
      package_family: 'arch',
      recommended_asset_name: 'Polaris-arch-x86_64.pkg.tar.zst',
      distro: { id: 'steamos', id_like: 'arch', version_id: '3.8.16' },
    }

    expect(selectReleaseAsset(release, steamOsHost)?.name)
      .toBe('Polaris-steamos3.8-x86_64.pkg.tar.zst')
    expect(selectReleaseAsset(release, {
      ...steamOsHost,
      distro: { ...steamOsHost.distro, version_id: '3.9' },
    })).toBeNull()
  })

  it('rejects shell-unsafe release metadata and mismatched direct SteamOS assets', () => {
    expect(buildManualInstallCommand({
      name: 'Polaris-steamos3.8-x86_64.pkg.tar.zst',
      browser_download_url: 'https://example.test/package;touch /tmp/pwned',
      packageFamily: 'steamos',
    })).toBe('')
    expect(buildManualInstallCommand({
      name: 'Polaris-steamos3.8-x86_64.pkg.tar.zst',
      browser_download_url: 'https://example.test/$(printf${IFS}PWNED)/Polaris-steamos3.8-x86_64.pkg.tar.zst',
      packageFamily: 'steamos',
    })).toBe('')
    expect(buildManualInstallCommand({
      name: 'Polaris-arch-x86_64.pkg.tar.zst',
      browser_download_url: 'https://example.test/Polaris-arch-x86_64.pkg.tar.zst',
      packageFamily: 'steamos',
    })).toBe('')
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

  it.each(mutablePackageCases)(
    'overwrites the exact $family package target and chains install side effects',
    ({ family, fileName, installLine }) => {
      const downloadUrl = `https://example.test/${fileName}`

      expect(buildManualInstallCommand({
        name: fileName,
        browser_download_url: downloadUrl,
        packageFamily: family,
      }).split('\n')).toEqual([
        `wget --output-document=./${fileName} ${downloadUrl} &&`,
        installLine,
        'sudo -H polaris --setup-host &&',
        'systemctl --user restart polaris',
      ])
    },
  )

  it.each(mutablePackageCases)(
    'stops $family update side effects after each failed command',
    ({ family, fileName, installLog }) => {
      const downloadUrl = `https://example.test/${fileName}`
      const command = buildManualInstallCommand({
        name: fileName,
        browser_download_url: downloadUrl,
        packageFamily: family,
      })
      const downloadLog = `wget --output-document=./${fileName} ${downloadUrl}`
      const setupLog = 'sudo -H polaris --setup-host'
      const expectedByFailure = {
        download: [downloadLog],
        install: [downloadLog, installLog],
        'setup-host': [downloadLog, installLog, setupLog],
      }

      for (const [failStep, expectedCommands] of Object.entries(expectedByFailure)) {
        const { commands, result } = runWithFakeMutableInstallCommands(command, failStep)

        expect(result.status, `${family}/${failStep} stdout: ${result.stdout}\nstderr: ${result.stderr}`).not.toBe(0)
        expect(commands).toEqual(expectedCommands)
        expect(commands.some((entry) => entry.startsWith('systemctl '))).toBe(false)
      }

      const success = runWithFakeMutableInstallCommands(command, '')
      expect(success.result.status, `${family}/success stderr: ${success.result.stderr}`).toBe(0)
      expect(success.commands).toEqual([
        downloadLog,
        installLog,
        setupLog,
        'systemctl --user restart polaris',
      ])
    },
  )

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

  it('keeps Update Center out of Mission Control and exposes sidebar/System affordances', () => {
    const dashboard = readFileSync(join(process.cwd(), 'src_assets/common/assets/web/views/DashboardView.vue'), 'utf8')
    const appShell = readFileSync(join(process.cwd(), 'src_assets/common/assets/web/App.vue'), 'utf8')
    const system = readFileSync(join(process.cwd(), 'src_assets/common/assets/web/views/HomeView.vue'), 'utf8')

    expect(dashboard).not.toContain('data-update-center-cta')
    expect(dashboard).not.toContain('data-update-status-light')
    expect(dashboard).not.toContain('buildUpdateCenterState')

    expect(appShell).toContain('data-sidebar-update-status')
    expect(appShell).toContain('data-sidebar-update-status-light')
    expect(appShell).toContain("path: '/info'")
    expect(appShell).toContain("hash: '#update-center'")
    expect(appShell).toContain('buildUpdateCenterState')

    expect(system).toContain('id="update-center"')
    expect(system).toContain('data-update-center-details')
    expect(system).toContain('@click="handlePrimaryUpdateAction"')
    expect(system).toContain('@click="refreshUpdateStatus"')
    expect(system).toContain('scrollIntoView')
    expect(system).not.toMatch(/POST['"]\s*,\s*['"].*update/i)
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
