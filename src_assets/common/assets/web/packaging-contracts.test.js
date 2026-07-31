import { chmodSync, mkdirSync, mkdtempSync, readFileSync, rmSync, writeFileSync } from 'node:fs'
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
