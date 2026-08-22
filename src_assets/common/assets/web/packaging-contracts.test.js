import { chmodSync, mkdirSync, mkdtempSync, readFileSync, rmSync, statSync, writeFileSync } from 'node:fs'
import { tmpdir } from 'node:os'
import { join } from 'node:path'
import { spawnSync } from 'node:child_process'
import { describe, expect, it } from 'vitest'

const readSource = (path) => readFileSync(join(process.cwd(), path), 'utf8')

const section = (source, start, end) => {
  const startIndex = source.indexOf(start)
  const endIndex = source.indexOf(end, startIndex + start.length)
  expect(startIndex, `missing section start: ${start}`).toBeGreaterThanOrEqual(0)
  expect(endIndex, `missing section end after ${start}: ${end}`).toBeGreaterThan(startIndex)
  return source.slice(startIndex, endIndex)
}

const yamlStepContaining = (source, marker) => {
  const markerIndex = source.indexOf(marker)
  expect(markerIndex, `missing YAML step marker: ${marker}`).toBeGreaterThanOrEqual(0)
  const stepStart = source.lastIndexOf('\n      - ', markerIndex)
  expect(stepStart, `missing YAML step start before: ${marker}`).toBeGreaterThanOrEqual(0)
  const nextStep = source.indexOf('\n      - ', markerIndex + marker.length)
  return source.slice(stepStart + 1, nextStep >= 0 ? nextStep : source.length)
}

const normalizedShellCommands = (source) => {
  const commands = []
  let pending = ''

  for (const rawLine of source.split('\n')) {
    const trimmed = rawLine.trim()
    if (!pending && (!trimmed || trimmed.startsWith('#'))) continue

    const continued = /\\$/.test(rawLine)
    const piece = continued ? rawLine.trimStart().slice(0, -1) : trimmed
    pending = `${pending}${piece}`

    if (!continued) {
      commands.push(pending.replace(/\s+/g, ' '))
      pending = ''
    }
  }

  if (pending) commands.push(pending.replace(/\s+/g, ' '))
  return commands
}

const shellExecutableOccurrences = (commands, executable) => {
  const pattern = new RegExp(`(?:^|[;&|(){}]\\s*)(?:(?:if|while|until|elif|then|else|do)\\s+)?(?:!\\s+)?(?:[A-Za-z_][A-Za-z0-9_]*=[^\\s;&|(){}]+\\s+)*(?:sudo\\s+)?(?:[^\\s;&|(){}]+/)?${executable}(?=\\s|$)`, 'g')
  return commands.flatMap((command, index) => (
    [...command.matchAll(pattern)].map((match) => ({ command, index, offset: match.index }))
  ))
}

const shellSyntaxView = (command) => {
  let quote = null
  let doubleQuoteExecutes = false
  let view = ''

  for (let index = 0; index < command.length; index += 1) {
    const character = command[index]
    if (quote) {
      if (character === quote) {
        quote = null
        doubleQuoteExecutes = false
      } else if ((quote === "'" && /[;&|()#]/.test(character))
        || (quote === '"' && !doubleQuoteExecutes && /[;&|()#]/.test(character))) {
        view += '·'
      } else {
        view += character
      }
      continue
    }

    if (character === "'" || character === '"') {
      quote = character
      if (character === '"') {
        const closingQuote = command.indexOf('"', index + 1)
        const quotedText = command.slice(index + 1, closingQuote >= 0 ? closingQuote : command.length)
        doubleQuoteExecutes = quotedText.includes('$(') || quotedText.includes('`')
      }
      continue
    }
    if (character === '\\' && index + 1 < command.length) {
      const escaped = command[index + 1]
      view += /[\s;&|(){}#'"\\]/.test(escaped) ? '·' : escaped
      index += 1
      continue
    }
    if (character === '#' && (index === 0 || /[\s;&|()]/.test(command[index - 1]))) break
    view += character
  }

  return view.trimEnd()
}

const canonicalShellCommands = (commands) => commands.map(shellSyntaxView)
const dynamicShellValue = '__POLARIS_DYNAMIC_SHELL_VALUE__'

const expandStaticShellVariables = (commands) => {
  const values = new Map()
  const blockDepths = shellBlockDepths(commands)
  const expand = (value) => value.replace(
    /\$(?:\{([A-Za-z_][A-Za-z0-9_]*)\}|([A-Za-z_][A-Za-z0-9_]*))/g,
    (reference, braced, bare) => values.get(braced ?? bare) ?? reference,
  )

  return commands.map((command, index) => {
    const controlFlowAssignment = blockDepths[index] !== 0
      || shellFunctionDefinition.test(command)
      || shellFunctionAnywhere.test(command)
      || /(?:^|[;&|]\s*)(?:(?:if|then|elif|else|while|until|for|do|done|case|esac|select|function)\b|\{|\()/.test(command)
    const syntaxView = shellSyntaxView(command)
    const assignments = command.matchAll(
      /(?:^|[;&|{]\s*|(?:^|[;&]\s*)[^;&{}]*\)\s*)(?:(?:if|elif|while|until|then|do|else)\s+)?(?:(?:export|readonly|declare|typeset|local)\s+(?:-[A-Za-z]+\s+)*)?([A-Za-z_][A-Za-z0-9_]*)=(?:"([^"]*)"|'([^']*)'|([^\s;&|]+))(?=\s*(?:[;&|}]|$))/dg,
    )
    for (const assignment of assignments) {
      const [, name, doubleQuoted, singleQuoted, unquoted] = assignment
      const nameIndex = assignment.indices[1][0]
      if (!syntaxView.startsWith(`${name}=`, nameIndex)) continue
      const value = singleQuoted ?? expand(doubleQuoted ?? unquoted)
      values.set(name, controlFlowAssignment ? dynamicShellValue : value)
    }
    for (const loopVariable of syntaxView.matchAll(
      /(?:^|[;&|({]\s*)(?:for|select)\s+([A-Za-z_][A-Za-z0-9_]*)\b/g,
    )) {
      values.set(loopVariable[1], dynamicShellValue)
    }
    return expand(command)
  })
}

const forbiddenShellWrapper = /(?:^|[;&|(){}]\s*)(?:(?:if|while|until|elif|then|else|do)\s+)?(?:!\s+)?(?:[A-Za-z_][A-Za-z0-9_]*=[^\s;&|(){}]+\s+)*(?:sudo\s+)?(?:(?:[^\s;&|(){}]+\/)?sudo(?:\s|$)|alias(?:\s|$)|builtin(?:\s|$)|enable(?:\s|$)|hash(?:\s|$)|command\s+(?!-(?:v|V)\b)|(?:(?:[^\s;&|(){}]+\/)?env|eval|exec|source|\.)\s+|(?:[^\s;&|(){}]+\/)?(?:xargs|find|parallel|time|timeout|nice|nohup|stdbuf|setsid|chroot|unshare|coproc|at|batch|crontab|systemd-run)\s+|(?:[^\s;&|(){}]+\/)?(?:ba|da|z)?sh(?:\s|$))/
const shellFunctionDefinition = /^(?:function\s+[A-Za-z_][A-Za-z0-9_]*(?:\s*\(\s*\))?|[A-Za-z_][A-Za-z0-9_]*\s*\(\s*\))(?:\s*\{)?/
const shellFunctionAnywhere = /(?:^|[;&|]\s*)(?:function\s+[A-Za-z_][A-Za-z0-9_]*(?:\s*\(\s*\))?|[A-Za-z_][A-Za-z0-9_]*\s*\(\s*\))\s*\{/
const sensitiveShellName = '(?:pacstrap|cmake|makepkg|cp|mv|install|rsync|cd|test|shopt|exit|\\[)'
const sensitiveShellShadow = new RegExp(`(?:^|[;&|]\\s*)(?:(?:${sensitiveShellName}\\s*\\(\\s*\\)|function\\s+${sensitiveShellName}(?:\\s*\\(\\s*\\))?)(?:\\s*\\{|$)|alias\\s+${sensitiveShellName}=)`)
const hasForbiddenShellWrapper = (source) => {
  const commands = normalizedShellCommands(source)
  const resolvedCommands = [
    ...canonicalShellCommands(commands),
    ...canonicalShellCommands(expandStaticShellVariables(commands)),
  ]
  return /\$(?:'|")/.test(source)
    || resolvedCommands.some((command) => forbiddenShellWrapper.test(command))
}
const withoutSteamOsRootBoundary = (source) => source.replace(/^chroot "\$STEAMOS_ROOT" /gm, '')
const hasSensitiveShellShadow = (source) => canonicalShellCommands(normalizedShellCommands(source))
  .some((command) => sensitiveShellShadow.test(command))
const hasShellDataBlock = (source) => /<<-?(?!<)/.test(source)
const positionalShellExpansion = /\$(?:[0-9@*#?$!-]|\{(?:[0-9]+|[@*#?$!-])(?:[^}]*)?\})/
const hasPositionalShellExpansion = (command) => positionalShellExpansion.test(command)
const dynamicShellExecutable = /(?:^|[;&|]\s*|(?<![=])\(\s*|(?<!\$)\{\s+)(?:(?:if|while|until|elif|then|else|do)\s+)?(?:!\s+)?(?:[A-Za-z_][A-Za-z0-9_]*=(?:"[^"]*"|'[^']*'|[^\s;&|$`(){}]+)\s+)*(?:sudo\s+)?(?![A-Za-z_][A-Za-z0-9_]*(?:\[[^\]]+\])?\+?=)(?=[^\s;&|]+)[^\s;&|]*(?:__POLARIS_DYNAMIC_SHELL_VALUE__|\$\(|`|\$\{[^}]+\}|\$(?:[A-Za-z_][A-Za-z0-9_]*|[0-9@*#?$!-]))/
const hasDynamicShellExecutable = (commands) => commands.some((command) => (
  dynamicShellExecutable.test(command)
))
const hasIndirectShellBinding = (commands) => commands.some((command) => (
  /(?:^|[;&|(){}]\s*)(?:(?:printf\s+-v|read(?:array)?|mapfile|declare\s+-n|typeset\s+-n)\b|[A-Za-z_][A-Za-z0-9_]*\+=(?!\s*\()|[A-Za-z_][A-Za-z0-9_]*\[[^\]]+\]\+?=)/.test(command)
))
const isShellArrayBinding = (command) => /(?:^|[;&|]\s*)[A-Za-z_][A-Za-z0-9_]*\+?=\s*\(/.test(command)
  || /(?:^|[;&|]\s*)[A-Za-z_][A-Za-z0-9_]*\[[^\]]+\]\+?=/.test(command)
const opensShellBlock = (command) => /(?:^|[;&|]\s*)\{(?:\s+[^}]*)?$/.test(command)
  || command === '('
  || /\$\([^)]*$/.test(command)
  || /^(?:case|for|if|select|until|while)\b/.test(command)
const closesShellBlock = (command) => /^(?:\}|fi|done|esac|\)(?:\s*&&)?)$/.test(command)
const shellBlockDepths = (commands) => {
  let depth = 0
  let awaitingFunctionBrace = false
  let insideBacktickSubstitution = false
  return commands.map((command) => {
    const controlToken = command.replace(/;\s*$/, '')
    const backtickCount = (command.match(/(?<!\\)`/g) ?? []).length
    const standaloneBacktick = controlToken === '`'
    if (insideBacktickSubstitution && standaloneBacktick) {
      depth = Math.max(0, depth - 1)
      insideBacktickSubstitution = false
    }
    if (controlToken === '{' && awaitingFunctionBrace) {
      awaitingFunctionBrace = false
      return depth
    }
    if (closesShellBlock(controlToken)) depth = Math.max(0, depth - 1)
    const commandDepth = depth
    if (shellFunctionDefinition.test(command)) {
      const closesOnSameCommand = /\{.*\}/.test(shellSyntaxView(command))
      if (!closesOnSameCommand) {
        depth += 1
        awaitingFunctionBrace = !command.endsWith('{')
      }
    } else if (opensShellBlock(controlToken)) {
      depth += 1
    }
    if (backtickCount % 2 === 1 && !standaloneBacktick) {
      if (insideBacktickSubstitution) {
        depth = Math.max(0, depth - 1)
        insideBacktickSubstitution = false
      } else {
        depth += 1
        insideBacktickSubstitution = true
      }
    }
    return commandDepth
  })
}

describe('Linux packaging contracts', () => {
  it('keeps portal/PipeWire capture independent while gating Wayland helpers', () => {
    const cmake = readSource('cmake/compile_definitions/linux.cmake')
    const portalGrab = readSource('src/platform/linux/portal_grab.cpp')
    const portalBlock = section(
      cmake,
      'if(GIO_FOUND AND GIO_UNIX_FOUND AND PIPEWIRE_FOUND)',
      'elseif(GIO_FOUND AND GIO_UNIX_FOUND)',
    )
    const waylandBlock = section(cmake, 'if(WAYLAND_FOUND)', '# x11')

    expect(portalBlock).toContain('POLARIS_BUILD_PORTAL')
    expect(portalBlock).toContain('portal_grab.cpp')
    expect(portalBlock).toContain('portal_session.cpp')
    expect(portalBlock).toContain('pipewire_capture.cpp')
    expect(portalBlock).not.toContain('cage_screencopy')
    expect(portalBlock).not.toContain('kwingrab')
    expect(waylandBlock).toContain('POLARIS_BUILD_WAYLAND')
    expect(waylandBlock).toContain('cage_screencopy.cpp')
    expect(waylandBlock).toContain('kwingrab.cpp')
    expect(cmake).toContain('AND NOT (GIO_FOUND AND GIO_UNIX_FOUND AND PIPEWIRE_FOUND))')
    expect(portalGrab).toMatch(
      /#ifdef POLARIS_BUILD_WAYLAND\s+#include "src\/platform\/linux\/cage_screencopy\.h"\s+#include "src\/platform\/linux\/kwingrab\.h"\s+#endif/,
    )
    expect(portalGrab).toContain('std::shared_ptr<void> kwin;')
    expect(portalGrab).toMatch(/#ifdef POLARIS_BUILD_WAYLAND[\s\S]*?kwingrab::prefer_for_current_stream_mode\(\)[\s\S]*?#endif/)
    expect(portalGrab).toMatch(/#ifdef POLARIS_BUILD_WAYLAND[\s\S]*?cage_screencopy::capture\([\s\S]*?#endif/)
  })

  it('installs Vulkan development files for every CUDA distro path', () => {
    const installer = readSource('scripts/install/01-install-deps.sh')
    const distroContracts = [
      ['  fedora)', '  arch)', 'vulkan-loader-devel'],
      ['  arch)', '  debian)', 'vulkan-headers vulkan-icd-loader'],
      ['  debian)', '  suse)', 'libvulkan-dev'],
      ['  suse)', '  *)', 'vulkan-devel'],
    ]

    for (const [start, end, vulkanPackage] of distroContracts) {
      const distroBlock = section(installer, start, end)
      const cudaIndex = distroBlock.indexOf('if [ "$WITH_CUDA" = 1 ]; then')
      const vulkanIndex = distroBlock.indexOf(vulkanPackage)
      expect(cudaIndex, `${start} must have a CUDA dependency branch`).toBeGreaterThanOrEqual(0)
      expect(vulkanIndex, `${start} must install ${vulkanPackage}`).toBeGreaterThan(cudaIndex)
    }
  })

  it('does not create system-prefix directories before privilege selection', () => {
    const installer = readSource('scripts/install/03-install-gamescope-stack.sh')
    expect(installer).not.toContain('mkdir -p "$LIBEXEC_DIR" "$BIN_DIR" "$SYSTEMD_USER_DIR" "$CONFIG_DIR"')
    expect(installer).toContain('mkdir -p "$SYSTEMD_USER_DIR" "$CONFIG_DIR"')
    expect(installer).toMatch(
      /if is_user_prefix; then\s+mkdir -p "\$LIBEXEC_DIR" "\$BIN_DIR"\s+else\s+maybe_sudo mkdir -p "\$LIBEXEC_DIR" "\$BIN_DIR"\s+fi/,
    )
  })

  it('emits the complete private-portal user service graph', () => {
    const session = readSource('nix/modules/session-lib.nix')
    const homeManager = readSource('nix/modules/home-manager.nix')

    for (const unit of [
      'polaris-portal-dbus.service',
      'polaris-portal-gamescope.service',
      'polaris-portal.service',
    ]) {
      expect(session).toContain(`"${unit}" = mkUnit`)
      expect(homeManager).toContain(`systemd.user.services.${unit.replace('.service', '')}`)
    }
    expect(session).toContain('RuntimeDirectory=polaris-portal')
    expect(session).toContain('RuntimeDirectoryMode=0700')
    expect(session).toContain('XDG_DESKTOP_PORTAL_DIR')
    expect(session).toContain('org.freedesktop.impl.portal.desktop.gamescope')
    expect(homeManager).toContain('RuntimeDirectory = "polaris-portal";')
    // Hard dep: private bus only. Portal backend/frontend are Wants= so nested
    // handoff can restart polaris-portal-gamescope without cascade-stopping polaris.
    expect(homeManager).toContain('Requires = [ "polaris-portal-dbus.service" ];')
    const polarisUnit = section(
      homeManager,
      'systemd.user.services.polaris = {',
      'home.activation.polarisConfSeed',
    )
    const homeRequires = section(polarisUnit, 'Requires = [', '];')
    const homeWants = section(polarisUnit, 'Wants = [', '];')
    const generatedPolaris = section(session, '"polaris.service" = mkUnit {', '\n    };')
    const generatedRequires = section(generatedPolaris, 'requires = [', '];')
    const generatedWants = section(generatedPolaris, 'wants = [', '];')
    expect(homeRequires).toContain('"polaris-portal-dbus.service"')
    expect(homeRequires).not.toContain('"polaris-portal-gamescope.service"')
    expect(homeRequires).not.toContain('"polaris-portal.service"')
    expect(generatedRequires).toContain('"polaris-portal-dbus.service"')
    expect(generatedRequires).not.toContain('"polaris-portal-gamescope.service"')
    expect(generatedRequires).not.toContain('"polaris-portal.service"')
    for (const unit of [
      'polaris-portal-dbus.service',
      'polaris-portal-gamescope.service',
      'polaris-portal.service',
    ]) {
      expect(polarisUnit).toContain(`"${unit}"`)
      expect(homeWants).toContain(`"${unit}"`)
      expect(generatedWants).toContain(`"${unit}"`)
    }
  })

  it('requires the private portal through readiness and final startup', () => {
    const session = readSource('nix/modules/session-lib.nix')
    const homeManager = readSource('nix/modules/home-manager.nix')
    const installer = readSource('scripts/install/03-install-gamescope-stack.sh')
    const serviceEnvironment = section(session, 'polarisServiceEnvironment = baseEnvironment // {', '  };')
    const polarisStart = section(
      session,
      'polarisStart = pkgs.writeShellScript "polaris-start"',
      '  waitPortal = pkgs.writeShellScript',
    )
    const waitPortal = section(
      session,
      'waitPortal = pkgs.writeShellScript "polaris-wait-private-screencast"',
      '\n  mkUnit =',
    )

    expect(serviceEnvironment).not.toContain('POLARIS_PORTAL_DBUS_ADDRESS')
    expect(polarisStart).toContain('[ ! -S "$bus_path" ]')
    expect(polarisStart).toContain('status org.freedesktop.portal.Desktop')
    expect(polarisStart).toContain('status org.freedesktop.impl.portal.desktop.gamescope')
    expect(polarisStart).toContain('export POLARIS_PORTAL_DBUS_ADDRESS="$private_address"')
    expect(polarisStart).toMatch(/required private ScreenCast portal disappeared before startup[\s\S]*exit 1/)
    expect(polarisStart).not.toContain('host session portal')
    expect(polarisStart).not.toContain('unix:path=%t/polaris-portal/bus')
    expect(waitPortal).toContain('private_address="unix:path=$bus_path"')
    expect(waitPortal).toContain('bus_deadline=$((SECONDS + 10))')
    expect(waitPortal).toContain('while [ ! -S "$bus_path" ]')
    expect(waitPortal).toContain('--address="$private_address"')
    expect(waitPortal).toMatch(/required private portal bus did not appear[\s\S]*exit 1/)
    expect(waitPortal).toMatch(/required private ScreenCast portal did not become ready[\s\S]*exit 1/)
    expect(waitPortal).not.toContain('using host fallback')
    expect(waitPortal).not.toContain('gamescopegrab may still work')
    expect(waitPortal).not.toContain('busctl --user')
    expect(session).toContain('UnsetEnvironment=WAYLAND_DISPLAY')
    expect(homeManager).toContain('UnsetEnvironment = [ "WAYLAND_DISPLAY" ];')
    expect(installer).toContain('UnsetEnvironment=WAYLAND_DISPLAY')
  })

  it('packages pw-cli and diagnoses stream-size query failures without advertising 0x0', () => {
    const portalPackage = readSource('nix/packages/xdg-desktop-portal-gamescope/default.nix')
    const streamSizePatch = readSource('nix/patches/xdg-desktop-portal-gamescope/01-fix-stream-size.patch')

    expect(portalPackage).toMatch(/\n\s+pipewire,\n/)
    expect(portalPackage).toContain('--prefix PATH : ${lib.makeBinPath [ pipewire ]}')
    expect(streamSizePatch).toContain('.map_err(|error| format!("failed to run pw-cli: {error}"))?;')
    expect(streamSizePatch).toContain('pw-cli exited with')
    expect(streamSizePatch).toContain('pw-cli response contained no Rectangle resolution')
    expect(streamSizePatch).toContain('log::warn!(')
    expect(streamSizePatch).toContain('Err(error) =>')
    expect(streamSizePatch).toContain('omitting stream size')
    expect(streamSizePatch).not.toContain('.output()\n+        .await\n+        .ok()?;')
    expect(streamSizePatch).not.toContain('width = 0;')
    expect(streamSizePatch).not.toContain('height = 0;')
  })

  it('keeps optional gamescope and Steam packages out of required build transactions', () => {
    const deps = readSource('scripts/install/01-install-deps.sh')
    const driver = readSource('scripts/install/install.sh')
    expect(deps).toContain('--gamescope-stack')
    expect(driver).toContain('DEPS_ARGS+=(--gamescope-stack)')
    expect(driver).toContain('--skip-stack) SKIP_STACK=1')
    expect(driver).toContain('if [ "$SKIP_STACK" = 0 ] && [ "$LABWC_ONLY" = 0 ]; then')

    for (const [start, end] of [
      ['  fedora)', '  arch)'],
      ['  arch)', '  debian)'],
      ['  debian)', '  suse)'],
      ['  suse)', '  *)'],
    ]) {
      const distroBlock = section(deps, start, end)
      const optionalIndex = distroBlock.indexOf('if [ "$WITH_GAMESCOPE_STACK" = 1 ]; then')
      expect(optionalIndex, `${start} must isolate gamescope runtime packages`).toBeGreaterThanOrEqual(0)
      expect(distroBlock.indexOf('gamescope'), `${start} gamescope must be optional`).toBeGreaterThan(optionalIndex)
      const steamIndex = distroBlock.search(/steam(?:-installer)?/)
      if (steamIndex >= 0) {
        expect(steamIndex, `${start} Steam must be optional`).toBeGreaterThan(optionalIndex)
      }
    }
  })

  it('ships the gamescope stream launcher and runtime library in every Linux package', () => {
    const cmake = readSource('cmake/packaging/linux.cmake')
    const session = readSource('nix/modules/polaris-gamescope-session.sh')
    const runtimeLibrary = readSource('nix/modules/polaris-gamescope-runtime-lib.sh')
    const archPkgbuild = readSource('packaging/linux/Arch/PKGBUILD')
    const steamOsPkgbuild = readSource('packaging/linux/SteamOS/PKGBUILD')
    const steamOsBuild = readSource('scripts/ci/build-steamos-package.sh')
    const workflow = readSource('.github/workflows/build.yml')
    const archJob = section(workflow, '  arch-build:', '  fedora-clang-build:')
    const ubuntuJob = section(workflow, '  ubuntu-build:', '  fedora-rpm-build:')
    const fedoraJob = section(workflow, '  fedora-rpm-build:', '  release-assets:')
    const packageInstall = section(cmake, 'if(NOT ${POLARIS_BUILD_APPIMAGE})', 'endif()')
    const debDependencies = section(cmake, 'set(CPACK_DEBIAN_PACKAGE_DEPENDS', 'set(CPACK_RPM_PACKAGE_REQUIRES')
    const rpmDependencies = section(cmake, 'set(CPACK_RPM_PACKAGE_REQUIRES', 'if(NOT BOOST_USE_STATIC)')

    expect(session.startsWith('#!/bin/bash\n')).toBe(true)
    expect(runtimeLibrary.startsWith('#!/bin/bash\n')).toBe(true)
    expect(packageInstall).toContain('"${CMAKE_SOURCE_DIR}/nix/modules/polaris-gamescope-session.sh"')
    expect(packageInstall).toContain('RENAME "polaris-gamescope-session"')
    expect(packageInstall).toContain('"${CMAKE_SOURCE_DIR}/nix/modules/polaris-gamescope-runtime-lib.sh"')
    expect(packageInstall.match(/DESTINATION "\$\{CMAKE_INSTALL_BINDIR\}"/g)).toHaveLength(2)
    expect(debDependencies).toContain('bash, \\')
    expect(rpmDependencies).toContain('bash, \\')

    for (const pkgbuild of [archPkgbuild, steamOsPkgbuild]) {
      expect(pkgbuild).toMatch(/depends=\([\s\S]*?\n\s+'bash'\n[\s\S]*?\n\)/)
      expect(pkgbuild).toContain('test -x "$pkgdir/usr/bin/polaris-gamescope-session"')
      expect(pkgbuild).toContain('test -x "$pkgdir/usr/bin/polaris-gamescope-runtime-lib.sh"')
      expect(pkgbuild).toContain('bash -n "$pkgdir/usr/bin/polaris-gamescope-session"')
      expect(pkgbuild).toContain('bash -n "$pkgdir/usr/bin/polaris-gamescope-runtime-lib.sh"')
    }

    for (const packagedPath of [
      '$RECEIPT_ROOT/usr/bin/polaris-gamescope-session',
      '$RECEIPT_ROOT/usr/bin/polaris-gamescope-runtime-lib.sh',
    ]) {
      expect(steamOsBuild).toContain(`test -x "${packagedPath}"`)
    }

    for (const [job, packageKind] of [
      [archJob, 'Arch package'],
      [ubuntuJob, 'Ubuntu DEB'],
      [fedoraJob, 'Fedora RPM'],
    ]) {
      expect(job, `${packageKind} must check the installed gamescope launcher`).toContain('/usr/bin/polaris-gamescope-session')
      expect(job, `${packageKind} must check the installed gamescope runtime library`).toContain('/usr/bin/polaris-gamescope-runtime-lib.sh')
      expect(job, `${packageKind} must syntax-check its gamescope payload`).toContain('bash -n "$gamescope_payload"')
    }
  })

  it('defines a distinct SteamOS 3.8 build lane', () => {
    const workflow = readSource('.github/workflows/build.yml')
    const steamOs = section(workflow, '  steamos-build:', '  ubuntu-build:')

    expect(steamOs).toContain('SteamOS 3.8 build')
    expect(steamOs).toContain('scripts/ci/run-steamos-build.sh')
    expect(steamOs).toContain('Polaris-steamos3.8-x86_64.pkg.tar.zst')
    expect(steamOs).not.toContain('packaging/linux/Arch/PKGBUILD')

    const upload = yamlStepContaining(steamOs, 'name: Polaris-steamos3.8-package')
    expect(upload).toContain('uses: actions/upload-artifact@')
    expect(upload).toMatch(/^\s+name: Polaris-steamos3\.8-package\s*$/m)
    expect(upload).toMatch(
      /^\s+path:\s+(?:"[^"\n]*Polaris-steamos3\.8-x86_64\.pkg\.tar\.zst"|'[^'\n]*Polaris-steamos3\.8-x86_64\.pkg\.tar\.zst'|[^\s#\n]*Polaris-steamos3\.8-x86_64\.pkg\.tar\.zst)\s*$/m,
    )
  })

  it('requires signed SteamOS 3.8.1x package sources with a pinned Valve keyring', () => {
    const bootstrap = readSource('scripts/ci/run-steamos-build.sh')
    const repoNames = [...bootstrap.matchAll(/^\[((?:jupiter|holo|core|extra)(?:-[^\]]+)?)\]$/gm)]
      .map((match) => match[1])

    expect(bootstrap).toContain('SigLevel = Required DatabaseOptional')
    expect(bootstrap).toContain('LocalFileSigLevel = Required')
    expect(bootstrap).toContain('a5efa4f9c161ce9607fd9dfcccaf2a587baa9acd35eae04d3c01d967dddc9722')
    expect(repoNames).toEqual([
      'jupiter-3.8.1x',
      'holo-3.8.1x',
      'core-3.8.1x',
      'extra-3.8.1x',
    ])
  })

  it('installs outer-container Git and initializes cleanup state before either is used', () => {
    const bootstrap = readSource('scripts/ci/run-steamos-build.sh')
    const outerInstall = bootstrap.match(/^pacman -Sy --noconfirm ([^\n]+)$/m)?.[1].split(/\s+/) ?? []
    const rootBinding = bootstrap.indexOf('STEAMOS_ROOT=/steamos-root')
    const cleanupTrap = bootstrap.indexOf('trap cleanup EXIT')
    const firstGitUse = bootstrap.search(/^git\s/m)

    expect(outerInstall).toContain('git')
    expect(bootstrap.indexOf('pacman -Sy --noconfirm')).toBeLessThan(firstGitUse)
    expect(rootBinding).toBeGreaterThanOrEqual(0)
    expect(rootBinding).toBeLessThan(cleanupTrap)
  })

  it('keeps the SteamOS root isolated from generic Arch packaging and cross-series pacman caches', () => {
    const workflow = readSource('.github/workflows/build.yml')
    const steamOs = section(workflow, '  steamos-build:', '  ubuntu-build:')
    const bootstrap = readSource('scripts/ci/run-steamos-build.sh')
    const buildScript = readSource('scripts/ci/build-steamos-package.sh')

    for (const source of [steamOs, bootstrap, buildScript]) {
      expect(source).not.toContain('packaging/linux/Arch/PKGBUILD')
      expect(source).not.toContain('/var/cache/pacman/pkg')
    }

    for (const [scriptName, source] of [
      ['run-steamos-build.sh', bootstrap],
      ['build-steamos-package.sh', buildScript],
    ]) {
      const wrapperSource = scriptName === 'run-steamos-build.sh'
        ? withoutSteamOsRootBoundary(source)
        : source
      expect(hasForbiddenShellWrapper(wrapperSource), `${scriptName} must use direct shell commands`).toBe(false)
      expect(hasSensitiveShellShadow(source), `${scriptName} must not shadow contract-sensitive commands`).toBe(false)
      expect(hasShellDataBlock(source), `${scriptName} must not satisfy execution contracts through heredoc data`).toBe(false)
    }

    const bootstrapCommands = normalizedShellCommands(bootstrap)
    const buildCommands = normalizedShellCommands(buildScript)
    const canonicalBootstrapCommands = canonicalShellCommands(bootstrapCommands)
    const expandedBootstrapCommands = canonicalShellCommands(expandStaticShellVariables(bootstrapCommands))
    const canonicalBuildCommands = canonicalShellCommands(buildCommands)
    const rootBindings = canonicalBootstrapCommands
      .map((command, index) => ({ command, index }))
      .filter(({ command }) => /^(?:export\s+|readonly\s+)?STEAMOS_ROOT=/.test(command))
    const rootBindingIndex = rootBindings[0]?.index ?? -1
    const rootBindingValue = rootBindings.length === 1
      ? expandedBootstrapCommands[rootBindingIndex].replace(/^(?:export\s+|readonly\s+)?STEAMOS_ROOT=/, '')
      : null
    const rootTargets = ['STEAMOS_ROOT', rootBindingValue].filter(Boolean)
    const rootReset = 'rm -rf -- "$STEAMOS_ROOT"'
    const rootCreate = 'install -d -m 0755 -- "$STEAMOS_ROOT"'
    const rootResetIndex = bootstrapCommands.indexOf(rootReset)
    const rootCreateIndex = bootstrapCommands.indexOf(rootCreate)
    const rootSelfBind = rootBindingValue ? `mount --bind ${rootBindingValue} ${rootBindingValue}` : null
    const rootSelfBindIndex = rootSelfBind ? expandedBootstrapCommands.indexOf(rootSelfBind) : -1
    const pacstrapIndexes = shellExecutableOccurrences(expandedBootstrapCommands, 'pacstrap')
    const bootstrapBlockDepths = shellBlockDepths(bootstrapCommands)
    const bootstrapTrapCommands = shellExecutableOccurrences(canonicalBootstrapCommands, 'trap')
    const buildTrapCommands = shellExecutableOccurrences(canonicalBuildCommands, 'trap')
    const rootWriterCommands = ['cp', 'install', 'mv', 'rsync', 'tar', 'bsdtar', 'pax', 'star', 'unzip', '7z', 'dd', 'cpio', 'mount', 'zstd', 'lz4', 'gzip', 'ln', 'tee', 'touch', 'truncate']
      .flatMap((executable) => shellExecutableOccurrences(expandedBootstrapCommands, executable))
    const requiredRootBindCommands = rootBindingValue ? [
      `mount --bind /workspace ${rootBindingValue}/mnt`,
      `mount --bind /output ${rootBindingValue}/opt`,
      `mount --bind /etc/resolv.conf ${rootBindingValue}/etc/resolv.conf`,
    ] : []
    const requiredApiMountCommands = rootBindingValue
      ? ['/proc', '/sys', '/dev', '/run'].flatMap((apiPath) => [
          `mount --rbind ${apiPath} ${rootBindingValue}${apiPath}`,
          `mount --make-rslave ${rootBindingValue}${apiPath}`,
        ])
      : []
    const allowedRootMountCommands = [rootSelfBind, ...requiredRootBindCommands, ...requiredApiMountCommands].filter(Boolean)
    const rootBindCommands = shellExecutableOccurrences(expandedBootstrapCommands, 'mount')
      .filter(({ command }) => command.startsWith('mount --bind ') && command !== rootSelfBind)
    const rootReseedCommands = rootWriterCommands
      .filter(({ command, index }) => index !== rootCreateIndex
        && !allowedRootMountCommands.includes(command)
        && rootTargets.some((target) => command.includes(target)))
    const taintedTopLevelRootWriters = rootWriterCommands
      .filter(({ command, index }) => index !== rootCreateIndex
        && bootstrapBlockDepths[index] === 0
        && (command.includes(dynamicShellValue) || hasPositionalShellExpansion(command)))
    const protectedRootArrayBindings = expandedBootstrapCommands
      .filter((command) => isShellArrayBinding(command) && rootTargets.some((target) => command.includes(target)))
    const rootRedirectCommands = expandedBootstrapCommands
      .filter((command) => [...command.matchAll(/\d*(?:>>?|<>)\s*([^\s;&|]+)/g)]
        .some((match) => rootTargets.some((target) => match[1].includes(target))))

    expect(rootBindings, 'STEAMOS_ROOT must have one static binding before bootstrap').toHaveLength(1)
    expect(bootstrapTrapCommands, 'bootstrap must have one static cleanup trap').toHaveLength(1)
    expect(bootstrapTrapCommands[0].command, 'bootstrap cleanup trap must invoke only a static function on EXIT').toMatch(/^trap [A-Za-z_][A-Za-z0-9_]* EXIT$/)
    expect(bootstrapBlockDepths[bootstrapTrapCommands[0].index], 'bootstrap cleanup trap must be armed at top level').toBe(0)
    expect(buildTrapCommands, 'package generation must not defer commands through traps').toEqual([])
    expect(rootBindingIndex, 'STEAMOS_ROOT must be bound before deleting the root').toBeGreaterThanOrEqual(0)
    expect(rootBindingIndex, 'STEAMOS_ROOT must be bound before deleting the root').toBeLessThan(rootResetIndex)
    expect(bootstrapBlockDepths[rootBindingIndex], 'STEAMOS_ROOT must be bound at top level').toBe(0)
    expect(hasDynamicShellExecutable(expandedBootstrapCommands), 'bootstrap commands must not use unresolved dynamic executables').toBe(false)
    expect(hasIndirectShellBinding(expandedBootstrapCommands), 'bootstrap commands must not create indirect shell bindings').toBe(false)
    expect(protectedRootArrayBindings, 'bootstrap commands must not hide the SteamOS root in shell arrays').toEqual([])
    expect(rootResetIndex, 'SteamOS root must be deleted before every bootstrap').toBeGreaterThanOrEqual(0)
    expect(rootCreateIndex, 'SteamOS root must be recreated immediately after deletion').toBe(rootResetIndex + 1)
    expect(rootSelfBindIndex, 'SteamOS root must become a mountpoint immediately after creation').toBe(rootCreateIndex + 1)
    expect(pacstrapIndexes, 'SteamOS root must have exactly one pacstrap population command').toHaveLength(1)
    expect(pacstrapIndexes[0].index, 'nothing except the root self-bind may precede pacstrap').toBe(rootSelfBindIndex + 1)
    expect(
      [rootResetIndex, rootCreateIndex, rootSelfBindIndex, pacstrapIndexes[0].index]
        .map((index) => bootstrapBlockDepths[index]),
      'the clean-root lifecycle must execute at top level',
    ).toEqual([0, 0, 0, 0])
    expect(rootReseedCommands, 'the clean root must not be repopulated through copy, archive, or device commands').toEqual([])
    expect(
      rootBindCommands.map(({ command }) => command),
      'only the checkout and output directories may be bound into the populated root',
    ).toEqual(requiredRootBindCommands)
    expect(
      rootBindCommands.every(({ index }) => index > pacstrapIndexes[0].index && bootstrapBlockDepths[index] === 0),
      'checkout and output binds must execute at top level after pacstrap',
    ).toBe(true)
    expect(taintedTopLevelRootWriters, 'top-level root writers must not depend on branch-sensitive shell state').toEqual([])
    expect(rootRedirectCommands, 'the clean root must not be repopulated through shell redirection').toEqual([])
    const pacstrapCommand = canonicalBootstrapCommands[pacstrapIndexes[0].index]
    const expandedPacstrapCommand = expandedBootstrapCommands[pacstrapIndexes[0].index]
    expect(pacstrapCommand, 'pacstrap must be invoked directly').toMatch(/^pacstrap\s/)
    expect(pacstrapCommand, 'pacstrap must target the freshly recreated SteamOS root').toMatch(
      /^pacstrap -G -M -C \S+ \$STEAMOS_ROOT(?:\s|$)/,
    )
    expect(pacstrapCommand).not.toMatch(/[;&|]/)
    expect(hasPositionalShellExpansion(pacstrapCommand), 'pacstrap must not consume positional shell state').toBe(false)
    expect(expandedPacstrapCommand).not.toMatch(/(?:^|\s)--cachedir(?:=|\s)/)
    for (const shortOption of expandedPacstrapCommand.match(/(?:^|\s)-[A-Za-z]+(?=\s|$)/g) ?? []) {
      expect(shortOption.trim().slice(1), `pacstrap option must not enable host cache reuse: ${shortOption.trim()}`).not.toContain('c')
    }

    expect(buildScript).not.toContain('POLARIS_CONFIGURE_PKGBUILD')
    const expandedBuildCommands = canonicalShellCommands(expandStaticShellVariables(buildCommands))
    expect(hasDynamicShellExecutable(expandedBuildCommands), 'package generation must not use unresolved dynamic executables').toBe(false)
    expect(hasIndirectShellBinding(expandedBuildCommands), 'package generation must not create indirect shell bindings').toBe(false)
    const buildRootMutations = expandedBuildCommands.filter((command) => (
      /(?:^|[\s;&|])(?:(?:export|readonly)\s+)?BUILD_ROOT\+?=/.test(command)
      || /\bunset\s+(?:-[^\s]+\s+)*BUILD_ROOT\b/.test(command)
      || /\bprintf\s+-v\s+BUILD_ROOT\b/.test(command)
    ))
    const importedCmakeState = expandedBuildCommands.filter((command) => (
      /(?:^|[\s;&|])(?:(?:export|readonly)\s+)?(?:CMAKE_TOOLCHAIN_FILE|CMAKE_PROJECT(?:_[A-Za-z0-9_-]+)?_(?:TOP_LEVEL_INCLUDES|INCLUDE|INCLUDE_BEFORE)|CMAKE_USER_MAKE_RULES_OVERRIDE(?:_[A-Za-z0-9_-]+)?)(?:\+?=|:)/.test(command)
    ))
    expect(buildRootMutations, 'BUILD_ROOT must not be reassigned before generating the package').toEqual([])
    expect(importedCmakeState, 'CMake executable import state must not be supplied through shell variables').toEqual([])
    const packageGeneratorSelectors = [
      ...expandedBuildCommands.join('\n').matchAll(/(?:^|\s)-D\s*POLARIS_CONFIGURE_(?!ONLY(?:[:=]|\b))[A-Z0-9_]+(?:[:=][^\s;&|]*)?/g),
    ]
      .map((match) => match[0].replace(/\s+/g, ''))
    const packageGeneratorUnsets = [
      ...expandedBuildCommands.join('\n').matchAll(/(?:^|\s)-U\s*POLARIS_CONFIGURE_(?!ONLY\b)[A-Z0-9_]+\b/g),
    ]
      .map((match) => match[0].replace(/\s+/g, ''))
    expect(packageGeneratorSelectors).toEqual(['-DPOLARIS_CONFIGURE_STEAMOS_PKGBUILD=ON'])
    expect(packageGeneratorUnsets, 'CMake must not unset a package selector after choosing SteamOS').toEqual([])

    const cmakeCommands = shellExecutableOccurrences(expandedBuildCommands, 'cmake')
    const cmakeConfigureCommands = cmakeCommands.filter(
      ({ command }) => command.startsWith('cmake ')
        && command.includes(' -S ')
        && command.includes(' -B ')
        && !/[;&|]/.test(command),
    )
    expect(cmakeCommands, 'SteamOS package build must have exactly one direct CMake invocation').toHaveLength(1)
    expect(cmakeConfigureCommands, 'the sole CMake invocation must be the SteamOS configure command').toEqual(cmakeCommands)
    expect(hasPositionalShellExpansion(cmakeConfigureCommands[0].command), 'CMake configure must not consume positional shell state').toBe(false)
    expect(cmakeConfigureCommands[0].command).toContain('-DPOLARIS_CONFIGURE_STEAMOS_PKGBUILD=ON')
    expect(cmakeConfigureCommands[0].command, 'the CMake configure contract must not be satisfied by inline comments').not.toContain('#')
    expect(cmakeConfigureCommands[0].command, 'CMake must not import unresolved cache or preset state').not.toMatch(
      /(?:^|\s)(?:-C(?:\S|\s|$)|--preset(?:=|\s)|--toolchain(?:=|\s))/,
    )
    expect(cmakeConfigureCommands[0].command, 'CMake must run configure mode rather than an informational or alternate mode').not.toMatch(
      /(?:^|\s)(?:--version|--help(?:-[A-Za-z-]+)?|-E|-P|-N|--build|--install|--open|--find-package|--workflow)(?:=|\s|$)/,
    )
    expect(cmakeConfigureCommands[0].command, 'CMake must not import executable toolchain or project-include state').not.toMatch(
      /-D\s*(?:CMAKE_TOOLCHAIN_FILE|CMAKE_PROJECT(?:_[A-Za-z0-9_-]+)?_(?:TOP_LEVEL_INCLUDES|INCLUDE|INCLUDE_BEFORE)|CMAKE_USER_MAKE_RULES_OVERRIDE(?:_[A-Za-z0-9_-]+)?)(?::[^=\s]+)?=/,
    )

    const pkgbuildDirAssignment = 'STEAMOS_PKGBUILD_DIR="$BUILD_ROOT/packaging/linux/SteamOS"'
    const pkgbuildDirAssignments = buildCommands.filter((command) => command.includes('STEAMOS_PKGBUILD_DIR='))
    const pkgbuildDirMentions = buildCommands.filter((command) => command.includes('STEAMOS_PKGBUILD_DIR'))
    const pkgbuildDirIndex = buildCommands.indexOf(pkgbuildDirAssignment)
    const pkgbuildCheckIndex = buildCommands.indexOf('test -f "$STEAMOS_PKGBUILD_DIR/PKGBUILD"')
    const pkgbuildCdIndex = buildCommands.indexOf('cd "$STEAMOS_PKGBUILD_DIR"')
    const makepkgCommands = shellExecutableOccurrences(expandedBuildCommands, 'makepkg')
    const directMakepkgCommands = makepkgCommands.filter(
      ({ command }) => command.startsWith('makepkg ') && !/[;&|]/.test(command),
    )
    const buildBlockDepths = shellBlockDepths(buildCommands)

    expect(pkgbuildDirAssignments, 'SteamOS PKGBUILD directory must have one immutable binding').toEqual([pkgbuildDirAssignment])
    expect(pkgbuildDirMentions, 'SteamOS PKGBUILD directory must only be bound, checked, and entered').toEqual([
      pkgbuildDirAssignment,
      'test -f "$STEAMOS_PKGBUILD_DIR/PKGBUILD"',
      'cd "$STEAMOS_PKGBUILD_DIR"',
    ])
    expect(pkgbuildDirIndex, 'SteamOS PKGBUILD directory must be assigned from the CMake build root').toBeGreaterThanOrEqual(0)
    expect(cmakeCommands[0].index, 'CMake must immediately consume the immutable build-root binding').toBe(pkgbuildDirIndex + 1)
    expect(pkgbuildCheckIndex, 'generated SteamOS PKGBUILD must be checked immediately after CMake').toBe(cmakeCommands[0].index + 1)
    expect(pkgbuildCdIndex, 'makepkg must enter the generated SteamOS directory').toBe(pkgbuildCheckIndex + 1)
    expect(makepkgCommands, 'SteamOS package build must have exactly one makepkg invocation').toHaveLength(1)
    expect(directMakepkgCommands, 'makepkg must be invoked directly').toHaveLength(1)
    expect(hasPositionalShellExpansion(directMakepkgCommands[0].command), 'makepkg must not consume positional shell state').toBe(false)
    expect(directMakepkgCommands[0].command, 'makepkg must use the generated PKGBUILD in the exact working directory').not.toMatch(
      /(?:^|\s)(?:-p\S*|--buildscript(?:=|\s|$))/,
    )
    expect(directMakepkgCommands[0].index, 'makepkg must consume the checked SteamOS PKGBUILD directory').toBe(pkgbuildCdIndex + 1)
    expect(
      [pkgbuildDirIndex, cmakeCommands[0].index, pkgbuildCheckIndex, pkgbuildCdIndex, directMakepkgCommands[0].index]
        .map((index) => buildBlockDepths[index]),
      'the SteamOS package-generator sequence must execute at top level',
    ).toEqual([0, 0, 0, 0, 0])
  })

  it('keeps the SteamOS package x86_64-only and CUDA-off', () => {
    const pkgbuild = readSource('packaging/linux/SteamOS/PKGBUILD')

    expect(pkgbuild).toContain("arch=('x86_64')")
    expect(pkgbuild).toContain('-DPOLARIS_ENABLE_CUDA=OFF')
    expect(pkgbuild).not.toContain('-D POLARIS_ENABLE_CUDA=OFF')
  })

  it('validates exact SteamOS source identity, package metadata, dependencies, and payload', () => {
    const workflow = readSource('.github/workflows/build.yml')
    const steamOs = section(workflow, '  steamos-build:', '  ubuntu-build:')
    const bootstrap = readSource('scripts/ci/run-steamos-build.sh')
    const buildScript = readSource('scripts/ci/build-steamos-package.sh')
    const commonTargets = readSource('cmake/targets/common.cmake')
    const pkgbuild = readSource('packaging/linux/SteamOS/PKGBUILD')
    const reviewedNamcap = readSource('packaging/linux/SteamOS/namcap-reviewed-warnings.txt')

    expect(steamOs).toContain('git fetch --no-tags --force origin "$POLARIS_CHECKOUT_REF"')
    expect(steamOs).toContain("git rev-parse 'FETCH_HEAD^{commit}'")
    expect(steamOs).toContain('--env "POLARIS_BUILD_COMMIT=$POLARIS_BUILD_COMMIT"')
    expect(bootstrap).toContain('${POLARIS_BUILD_COMMIT:?POLARIS_BUILD_COMMIT is required}')
    expect(bootstrap).not.toMatch(/(?:^|\n)POLARIS_BUILD_COMMIT=/)
    expect(bootstrap).toContain('[ ! -e /workspace/.git ]')

    const exactStatus = 'git -C "$SOURCE_ROOT" status --porcelain=v1 --untracked-files=all --ignore-submodules=none'
    expect(buildScript.split(exactStatus)).toHaveLength(3)
    expect(buildScript).not.toContain('--print-format')
    expect(buildScript).toContain("sed -n 's/^pkgname = //p' \"$RECEIPT_ROOT/.PKGINFO\"")
    expect(buildScript).toContain("sed -n 's/^pkgver = //p' \"$RECEIPT_ROOT/.PKGINFO\"")
    expect(buildScript).toContain("sed -n 's/^arch = //p' \"$RECEIPT_ROOT/.PKGINFO\"")
    expect(buildScript).toContain("'polaris|1.3.12-1|x86_64'")
    expect(buildScript).toContain('PACKAGE_PATHS=(polaris-[0-9]*-x86_64.pkg.tar.zst)')
    expect(buildScript).toContain('CLONE_URL=https://github.com/papi-ux/polaris.git')
    expect(buildScript).toContain("sed -n 's/^depend = //p' \"$RECEIPT_ROOT/.PKGINFO\"")
    expect(bootstrap).toContain('pacman -T "${PACKAGE_DEPENDENCIES[@]}"')
    expect(buildScript).toContain('HOST_BINARIES=("$RECEIPT_ROOT"/usr/bin/polaris-[0-9]*)')
    expect(commonTargets).toContain('-buildmode=pie')
    expect(commonTargets).toContain('-ldflags=-linkmode=external -extldflags=-Wl,-z,relro,-z,now')
    for (const dependency of ['gcc-libs', 'glib2', 'glibc', 'gtk3', 'hicolor-icon-theme', 'libpipewire', 'libxkbcommon']) {
      expect(pkgbuild).toContain(`'${dependency}'`)
    }
    expect(buildScript).toContain('PATH=/usr/bin:/bin namcap "$PACKAGE_PATH" > "$OUTPUT_ROOT/steamos3.8-namcap-all.txt"')
    expect(buildScript).toContain('comm -23 "$NAMCAP_ACTUAL" "$NAMCAP_ALLOWED"')
    expect(buildScript).toContain('comm -13 "$NAMCAP_ACTUAL" "$NAMCAP_ALLOWED"')
    expect(buildScript).toContain('if [ -s "$OUTPUT_ROOT/steamos3.8-namcap.txt" ]; then')
    expect(buildScript).toContain('namcap emitted unreviewed warnings or a reviewed warning disappeared')
    expect(buildScript).not.toContain('namcap "$PACKAGE_PATH" > "$OUTPUT_ROOT/steamos3.8-namcap-all.txt" || true')
    const reviewedWarnings = reviewedNamcap.trim().split('\n')
    // 18 since labwc left depends for optdepends: it pulled wlroots0.19, which pins
    // libdisplay-info.so=2 and downgraded the libdisplay-info 0.3.0 that SteamOS runs
    // KWin, Mesa, and Vulkan on, so no install could succeed. With the dependency gone
    // namcap has nothing unneeded to report and its reviewed line retired with it (#442).
    // It was 19 after the attach guard started linking libxcb for real, which retired
    // that dependency's line the same way (#415).
    expect(reviewedWarnings).toHaveLength(18)
    expect(new Set(reviewedWarnings).size).toBe(reviewedWarnings.length)
    expect(reviewedWarnings.every((warning) => warning.startsWith('polaris W: '))).toBe(true)
    expect(buildScript).toContain('"$RECEIPT_ROOT/usr/bin/polaris-browser-stream-helper"')
    expect(buildScript).toContain('"$RECEIPT_ROOT/usr/share/polaris"')
    expect(buildScript).toContain('"$RECEIPT_ROOT/usr/share/applications/dev.polaris-stream.app.Polaris.desktop"')
    expect(buildScript).toContain('"$RECEIPT_ROOT/usr/lib/systemd/user/polaris.service"')
    expect(pkgbuild).toContain('test -x "$pkgdir/usr/bin/polaris-$pkgver"')
    expect(pkgbuild).toContain('test "$(readlink "$pkgdir/usr/bin/polaris")" = "polaris-$pkgver"')
    expect(pkgbuild).not.toContain('mv "$pkgdir/usr/bin/polaris"')
    expect(pkgbuild).not.toContain('ln -s "polaris-$pkgver"')
    expect(statSync('scripts/check-packaged-binary-paths.sh').mode & 0o111).not.toBe(0)

    const releaseVerifierIndex = workflow.indexOf('      - name: Verify release assets on GitHub release')
    expect(releaseVerifierIndex).toBeGreaterThanOrEqual(0)
    const releaseVerifier = workflow.slice(releaseVerifierIndex)
    for (const requiredBinary of [
      'Polaris-arch-x86_64.pkg.tar.zst',
      'Polaris-fedora44-x86_64.rpm',
      'Polaris-steamos3.8-x86_64.pkg.tar.zst',
      'Polaris-ubuntu24.04-x86_64.deb',
    ]) {
      expect(releaseVerifier).toContain(requiredBinary)
    }
    expect(releaseVerifier).toContain("find release-assets/final -maxdepth 1 -type f -printf '%f\\n' | sort")
    expect(releaseVerifier).toContain("--json assets --jq '.assets[].name' | sort")
    expect(releaseVerifier).toContain('[ "${expected_assets[*]}" != "${published_assets[*]}" ]')
    expect(releaseVerifier).not.toContain('supported_count')
  })

  it('keeps unpublished candidate sourcing explicit, local-only, and absent from CI', () => {
    const workflow = readSource('.github/workflows/build.yml')
    const bootstrap = readSource('scripts/ci/run-steamos-build.sh')
    const buildScript = readSource('scripts/ci/build-steamos-package.sh')

    expect(workflow).not.toContain('POLARIS_LOCAL_CANDIDATE_BUILD')
    for (const script of [bootstrap, buildScript]) {
      const commands = normalizedShellCommands(script)
      expect(commands).toContain('POLARIS_LOCAL_CANDIDATE_BUILD="${POLARIS_LOCAL_CANDIDATE_BUILD-0}"')
      expect(commands).not.toContain('POLARIS_LOCAL_CANDIDATE_BUILD="${POLARIS_LOCAL_CANDIDATE_BUILD:-0}"')

      const lines = script.split('\n')
      const guardStart = lines.indexOf('POLARIS_LOCAL_CANDIDATE_BUILD="${POLARIS_LOCAL_CANDIDATE_BUILD-0}"')
      const guardEnd = lines.indexOf('fi', guardStart + 1)
      expect(guardStart).toBeGreaterThanOrEqual(0)
      expect(guardEnd).toBeGreaterThan(guardStart)
      const guardScript = lines.slice(guardStart, guardEnd + 1).join('\n')
      for (const [value, expectedStatus] of [[undefined, 0], ['', 1], ['0', 0], ['1', 0], ['2', 1]]) {
        const env = { ...process.env }
        if (value === undefined) delete env.POLARIS_LOCAL_CANDIDATE_BUILD
        else env.POLARIS_LOCAL_CANDIDATE_BUILD = value
        const result = spawnSync('bash', ['-c', guardScript], { encoding: 'utf8', env })
        expect(result.status, `unexpected guard status for ${JSON.stringify(value)}`).toBe(expectedStatus)
        if (expectedStatus !== 0) {
          expect(result.stderr).toContain('POLARIS_LOCAL_CANDIDATE_BUILD must be 0 or 1')
        }
      }
    }
    expect(bootstrap).toContain('chroot "$STEAMOS_ROOT" runuser --user builder --')
    expect(bootstrap).not.toContain('arch-chroot "$STEAMOS_ROOT"')
    expect(bootstrap).toContain('mount --bind "$STEAMOS_ROOT" "$STEAMOS_ROOT"')
    expect(bootstrap).toContain('umount "$STEAMOS_ROOT" 2>/dev/null || true')
    expect(bootstrap).toContain('mount --bind /etc/resolv.conf "$STEAMOS_ROOT/etc/resolv.conf"')
    expect(bootstrap).toContain('umount "$STEAMOS_ROOT/etc/resolv.conf" 2>/dev/null || true')
    for (const apiPath of ['/proc', '/sys', '/dev', '/run']) {
      expect(bootstrap).toContain(`mount --rbind ${apiPath} "$STEAMOS_ROOT${apiPath}"`)
      expect(bootstrap).toContain(`mount --make-rslave "$STEAMOS_ROOT${apiPath}"`)
      expect(bootstrap).toContain(`umount -R "$STEAMOS_ROOT${apiPath}" 2>/dev/null || true`)
    }
    expect(bootstrap).not.toContain('runuser --login')
    expect(bootstrap).not.toContain('--whitelist-environment=')
    expect(hasForbiddenShellWrapper(withoutSteamOsRootBoundary(
      'chroot "$STEAMOS_ROOT" sh -c "pacman -T"',
    ))).toBe(true)
    expect(buildScript).toContain('CLONE_URL=https://github.com/papi-ux/polaris.git')
    expect(buildScript).toContain('CLONE_URL=file:///mnt')
    expect(buildScript).not.toMatch(/CLONE_URL=(?:file:\/\/)?\$\{?SOURCE_ROOT/)
    for (const script of [bootstrap, buildScript]) {
      expect(script).toContain("POLARIS_LOCAL_CANDIDATE_BUILD must be 0 or 1")
    }
  })

  it('downloads and stages the SteamOS package in release assembly', () => {
    const workflow = readSource('.github/workflows/build.yml')
    const releaseAssetsIndex = workflow.indexOf('  release-assets:')
    expect(releaseAssetsIndex, 'missing release-assets job').toBeGreaterThanOrEqual(0)
    const releaseAssets = workflow.slice(releaseAssetsIndex)
    const needs = section(releaseAssets, '    needs:', '    if:')

    expect(needs).toContain('- steamos-build')
    const download = yamlStepContaining(releaseAssets, 'name: Polaris-steamos3.8-package')
    expect(download).toContain('uses: actions/download-artifact@v8')
    expect(download).toMatch(/^\s+name: Polaris-steamos3\.8-package\s*$/m)
    expect(download).toMatch(/^\s+path: release-assets\/raw\/steamos3\.8\s*$/m)

    const assembly = yamlStepContaining(
      releaseAssets,
      'steamos_packages=(release-assets/raw/steamos3.8/*.pkg.tar.zst)',
    )
    expect(hasForbiddenShellWrapper(assembly), 'release assembly must not import or wrap writer commands').toBe(false)
    expect(hasSensitiveShellShadow(assembly), 'release assembly must not shadow guard or writer commands').toBe(false)
    expect(hasShellDataBlock(assembly), 'release assembly must not satisfy execution contracts through heredoc data').toBe(false)
    const assemblyCommands = normalizedShellCommands(assembly)
    const nullglobCommand = 'shopt -s nullglob'
    const packageArrayCommand = 'steamos_packages=(release-assets/raw/steamos3.8/*.pkg.tar.zst)'
    const cardinalityGuard = 'if [ "${#steamos_packages[@]}" -ne 1 ]; then'
    const exactCopy = 'cp "release-assets/raw/steamos3.8/Polaris-steamos3.8-x86_64.pkg.tar.zst" "release-assets/staged/Polaris-steamos3.8-x86_64.pkg.tar.zst"'
    const stagedDestination = 'release-assets/staged/Polaris-steamos3.8-x86_64.pkg.tar.zst'
    const stagedDirectory = 'release-assets/staged'
    const stagedBasename = 'Polaris-steamos3.8-x86_64.pkg.tar.zst'
    const nullglobIndex = assemblyCommands.indexOf(nullglobCommand)
    const arrayIndex = assemblyCommands.indexOf(packageArrayCommand)
    const guardIndex = assemblyCommands.indexOf(cardinalityGuard)
    const guardExitIndex = assemblyCommands.indexOf('exit 1', guardIndex + 1)
    const guardCloseIndex = assemblyCommands.indexOf('fi', guardIndex + 1)
    const copyIndex = assemblyCommands.indexOf(exactCopy)
    const expandedAssemblyCommands = canonicalShellCommands(expandStaticShellVariables(assemblyCommands))
    const assemblyBlockDepths = shellBlockDepths(assemblyCommands)
    const assemblyTrapCommands = shellExecutableOccurrences(canonicalShellCommands(assemblyCommands), 'trap')
    expect(hasDynamicShellExecutable(expandedAssemblyCommands), 'release assembly must not use unresolved dynamic executables').toBe(false)
    expect(hasIndirectShellBinding(expandedAssemblyCommands), 'release assembly must not create indirect shell bindings').toBe(false)
    expect(assemblyTrapCommands, 'release assembly must not defer writer commands through traps').toEqual([])
    const canonicalExactCopy = canonicalShellCommands([exactCopy])[0]
    const allSteamOsWriterCommands = ['cp', 'mv', 'install', 'rsync', 'ln', 'tar', 'bsdtar', 'pax', 'star', 'unzip', '7z', 'dd', 'cpio', 'mount', 'zstd', 'lz4', 'gzip', 'tee', 'touch', 'truncate']
      .flatMap((executable) => shellExecutableOccurrences(expandedAssemblyCommands, executable))
    const steamOsPackageWriters = allSteamOsWriterCommands
      .filter(({ command }) => command.includes(stagedDestination)
        || (command.includes(stagedDirectory) && command.includes(stagedBasename)))
      .map(({ command }) => command)
    const taintedTopLevelSteamOsWriters = allSteamOsWriterCommands
      .filter(({ command, index }) => assemblyBlockDepths[index] === 0
        && (command.includes(dynamicShellValue) || hasPositionalShellExpansion(command)))
    const protectedSteamOsArrayBindings = expandedAssemblyCommands
      .filter((command) => isShellArrayBinding(command)
        && (command.includes(stagedDestination)
          || (command.includes(stagedDirectory) && command.includes(stagedBasename))))
    const steamOsRedirectWriters = expandedAssemblyCommands
      .filter((command) => command.includes(stagedDestination) && />/.test(command))
    const stagedDirectoryChanges = ['cd', 'pushd']
      .flatMap((executable) => shellExecutableOccurrences(expandedAssemblyCommands, executable))
      .filter(({ command }) => command.includes(stagedDirectory))

    expect(nullglobIndex, 'release assembly must enable nullglob').toBeGreaterThanOrEqual(0)
    expect(arrayIndex, 'SteamOS package array must immediately follow nullglob').toBe(nullglobIndex + 1)
    expect(guardIndex, 'exact-one cardinality guard must immediately follow package discovery').toBe(arrayIndex + 1)
    expect(guardExitIndex, 'cardinality mismatch must immediately terminate the assembly').toBe(guardIndex + 1)
    expect(guardCloseIndex, 'cardinality guard must close immediately after its exit').toBe(guardExitIndex + 1)
    expect(copyIndex, 'the exact SteamOS copy must run only after cardinality validation').toBe(guardCloseIndex + 1)
    expect(
      [nullglobIndex, arrayIndex, guardIndex, guardExitIndex, guardCloseIndex, copyIndex]
        .map((index) => assemblyBlockDepths[index]),
      'the SteamOS cardinality and staging sequence must run at top level',
    ).toEqual([0, 0, 0, 1, 0, 0])
    expect(steamOsPackageWriters, 'cardinality must dominate every resolved SteamOS artifact writer').toEqual([canonicalExactCopy])
    expect(taintedTopLevelSteamOsWriters, 'top-level artifact writers must not depend on branch-sensitive shell state').toEqual([])
    expect(protectedSteamOsArrayBindings, 'release assembly must not hide the SteamOS destination in shell arrays').toEqual([])
    expect(steamOsRedirectWriters, 'the staged SteamOS artifact must not be written through redirection').toEqual([])
    expect(stagedDirectoryChanges, 'release assembly must not change into the staged-artifact directory').toEqual([])
  })

  it('maps CI source prefixes and materializes package strings before path checks', () => {
    const workflow = readSource('.github/workflows/build.yml')
    const ubuntuConfigure = section(workflow, '  ubuntu-build:', '  fedora-rpm-build:')
    const fedoraConfigure = section(workflow, '  fedora-rpm-build:', '  release-assets:')

    for (const [lane, block] of [
      ['Ubuntu', ubuntuConfigure],
      ['Fedora', fedoraConfigure],
    ]) {
      expect(block, `${lane} must remap C source paths`).toContain(
        '"-DCMAKE_C_FLAGS=-ffile-prefix-map=${GITHUB_WORKSPACE}=."',
      )
      expect(block, `${lane} must remap C++ source paths`).toContain(
        '"-DCMAKE_CXX_FLAGS=-ffile-prefix-map=${GITHUB_WORKSPACE}=."',
      )
      expect(block, `${lane} must use the full-consumption checker`).toContain(
        'bash scripts/check-packaged-binary-paths.sh',
      )
      expect(block).not.toMatch(/strings "\$binary"\s*\|\s*grep/)
    }

    expect(fedoraConfigure).toContain('--forbid-native-pipewire-audio')
    expect(ubuntuConfigure).not.toContain('--forbid-native-pipewire-audio')
    expect(workflow.match(/--forbid-native-pipewire-audio/g)).toHaveLength(1)
  })

  it('rejects contaminated strings without the legacy pipefail false negative', () => {
    const fixture = mkdtempSync(join(tmpdir(), 'polaris-package-paths-'))
    try {
      const binDir = join(fixture, 'bin')
      const stringsPath = join(binDir, 'strings')
      const dummyBinary = join(fixture, 'polaris')
      const report = join(fixture, 'package-strings.txt')
      mkdirSync(binDir)
      writeFileSync(dummyBinary, 'fixture')
      writeFileSync(
        stringsPath,
        `#!/usr/bin/env bash
printf '/__w/polaris/polaris/src/platform/linux/graphics.cpp:593\\n'
for ((i = 0; i < 50000; i++)); do
  printf 'safe-symbol-%s\\n' "$i"
done
`,
      )
      chmodSync(stringsPath, 0o755)

      const env = { ...process.env, PATH: `${binDir}:${process.env.PATH}` }
      const legacy = spawnSync(
        'bash',
        [
          '-c',
          `set -o pipefail
if strings "$1" | grep -Eq '/__w/|src_assets/.*/assets/shaders'; then
  exit 0
fi
exit 42`,
          'legacy-check',
          dummyBinary,
        ],
        { encoding: 'utf8', env },
      )
      expect(legacy.status, `legacy stderr: ${legacy.stderr}`).toBe(42)

      const contaminated = spawnSync(
        'bash',
        ['scripts/check-packaged-binary-paths.sh', dummyBinary, report],
        { encoding: 'utf8', env },
      )
      expect(contaminated.status, `checker stderr: ${contaminated.stderr}`).toBe(1)
      expect(contaminated.stderr).toContain('forbidden build/source path')

      writeFileSync(
        stringsPath,
        `#!/usr/bin/env bash
printf '/usr/share/polaris/shaders/opengl\\n'
printf 'portal: PipeWire format negotiated: \\n'
printf 'safe-symbol\\n'
`,
      )
      const safe = spawnSync(
        'bash',
        ['scripts/check-packaged-binary-paths.sh', dummyBinary, report],
        { encoding: 'utf8', env },
      )
      expect(safe.status, `checker stderr: ${safe.stderr}`).toBe(0)
    } finally {
      rmSync(fixture, { force: true, recursive: true })
    }
  })

  it('rejects packaged binaries without portal capture support', () => {
    const fixture = mkdtempSync(join(tmpdir(), 'polaris-package-portal-'))
    try {
      const binDir = join(fixture, 'bin')
      const stringsPath = join(binDir, 'strings')
      const dummyBinary = join(fixture, 'polaris')
      const report = join(fixture, 'package-strings.txt')
      mkdirSync(binDir)
      writeFileSync(dummyBinary, 'fixture')
      writeFileSync(stringsPath, '#!/usr/bin/env bash\nprintf \'safe-symbol\\n\'\n')
      chmodSync(stringsPath, 0o755)

      const env = { ...process.env, PATH: `${binDir}:${process.env.PATH}` }
      const result = spawnSync(
        'bash',
        ['scripts/check-packaged-binary-paths.sh', dummyBinary, report],
        { encoding: 'utf8', env },
      )

      expect(result.status, `checker stderr: ${result.stderr}`).toBe(1)
      expect(result.stderr).toContain('does not contain XDG Desktop Portal capture support')
    } finally {
      rmSync(fixture, { force: true, recursive: true })
    }
  })

  it('fails closed when grep cannot scan the materialized report', () => {
    const fixture = mkdtempSync(join(tmpdir(), 'polaris-package-paths-'))
    try {
      const binDir = join(fixture, 'bin')
      const stringsPath = join(binDir, 'strings')
      const grepPath = join(binDir, 'grep')
      const dummyBinary = join(fixture, 'polaris')
      const report = join(fixture, 'package-strings.txt')
      mkdirSync(binDir)
      writeFileSync(dummyBinary, 'fixture')
      writeFileSync(stringsPath, '#!/usr/bin/env bash\nprintf \'safe-symbol\\n\'\n')
      writeFileSync(grepPath, '#!/usr/bin/env bash\nexit 2\n')
      chmodSync(stringsPath, 0o755)
      chmodSync(grepPath, 0o755)

      const env = { ...process.env, PATH: `${binDir}:${process.env.PATH}` }
      const result = spawnSync(
        'bash',
        ['scripts/check-packaged-binary-paths.sh', dummyBinary, report],
        { encoding: 'utf8', env },
      )

      expect(result.status, `checker stderr: ${result.stderr}`).toBe(2)
      expect(result.stderr).toContain('Failed to scan packaged Polaris binary strings')
    } finally {
      rmSync(fixture, { force: true, recursive: true })
    }
  })

  it('fails closed when the native PipeWire audio marker scan errors', () => {
    const fixture = mkdtempSync(join(tmpdir(), 'polaris-package-native-audio-scan-'))
    try {
      const binDir = join(fixture, 'bin')
      const stringsPath = join(binDir, 'strings')
      const grepPath = join(binDir, 'grep')
      const dummyBinary = join(fixture, 'polaris')
      const report = join(fixture, 'package-strings.txt')
      mkdirSync(binDir)
      writeFileSync(dummyBinary, 'fixture')
      writeFileSync(
        stringsPath,
        `#!/usr/bin/env bash
printf 'portal: PipeWire format negotiated: \\n'
printf 'safe-symbol\\n'
`,
      )
      writeFileSync(
        grepPath,
        `#!/usr/bin/env bash
if [ "$1" = "-Fq" ] && [ "$2" = "PipeWire detected, will prefer native PipeWire for audio capture" ]; then
  exit 2
fi
exec "$REAL_GREP" "$@"
`,
      )
      chmodSync(stringsPath, 0o755)
      chmodSync(grepPath, 0o755)

      const realGrep = spawnSync('bash', ['-c', 'command -v grep'], { encoding: 'utf8' }).stdout.trim()
      const env = {
        ...process.env,
        PATH: `${binDir}:${process.env.PATH}`,
        REAL_GREP: realGrep,
      }
      const result = spawnSync(
        'bash',
        [
          'scripts/check-packaged-binary-paths.sh',
          dummyBinary,
          report,
          '--forbid-native-pipewire-audio',
        ],
        { encoding: 'utf8', env },
      )

      expect(result.status, `checker stderr: ${result.stderr}`).toBe(2)
      expect(result.stderr).toContain('Failed to scan for native PipeWire audio support')
    } finally {
      rmSync(fixture, { force: true, recursive: true })
    }
  })

  it('rejects native PipeWire audio support when the Fedora policy forbids it', () => {
    const fixture = mkdtempSync(join(tmpdir(), 'polaris-package-native-audio-'))
    try {
      const binDir = join(fixture, 'bin')
      const stringsPath = join(binDir, 'strings')
      const dummyBinary = join(fixture, 'polaris')
      const report = join(fixture, 'package-strings.txt')
      mkdirSync(binDir)
      writeFileSync(dummyBinary, 'fixture')
      writeFileSync(
        stringsPath,
        `#!/usr/bin/env bash
printf 'portal: PipeWire format negotiated: \\n'
printf 'PipeWire detected, will prefer native PipeWire for audio capture\\n'
`,
      )
      chmodSync(stringsPath, 0o755)

      const env = { ...process.env, PATH: `${binDir}:${process.env.PATH}` }
      const result = spawnSync(
        'bash',
        [
          'scripts/check-packaged-binary-paths.sh',
          dummyBinary,
          report,
          '--forbid-native-pipewire-audio',
        ],
        { encoding: 'utf8', env },
      )

      expect(result.status, `checker stderr: ${result.stderr}`).toBe(1)
      expect(result.stderr).toContain('unexpectedly contains native PipeWire audio support')
    } finally {
      rmSync(fixture, { force: true, recursive: true })
    }
  })

  it('rejects aliased binary and report paths before truncating the binary', () => {
    const fixture = mkdtempSync(join(tmpdir(), 'polaris-package-paths-'))
    try {
      const binDir = join(fixture, 'bin')
      const stringsPath = join(binDir, 'strings')
      const dummyBinary = join(fixture, 'polaris')
      mkdirSync(binDir)
      writeFileSync(dummyBinary, 'fixture')
      writeFileSync(stringsPath, '#!/usr/bin/env bash\nprintf \'safe-symbol\\n\'\n')
      chmodSync(stringsPath, 0o755)

      const env = { ...process.env, PATH: `${binDir}:${process.env.PATH}` }
      const result = spawnSync(
        'bash',
        ['scripts/check-packaged-binary-paths.sh', dummyBinary, dummyBinary],
        { encoding: 'utf8', env },
      )

      expect(result.status, `checker stderr: ${result.stderr}`).toBe(2)
      expect(result.stderr).toContain('Binary and report must be different files')
      expect(readFileSync(dummyBinary, 'utf8')).toBe('fixture')
    } finally {
      rmSync(fixture, { force: true, recursive: true })
    }
  })
})
