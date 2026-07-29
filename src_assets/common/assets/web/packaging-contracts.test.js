import { readFileSync } from 'node:fs'
import { join } from 'node:path'
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
    expect(homeManager).toContain('Requires = [ "polaris-portal-dbus.service" ];')
  })

  it('uses the private portal only after it is ready and preserves host fallback', () => {
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
    expect(polarisStart).toContain('[ -S "$bus_path" ]')
    expect(polarisStart).toContain('status org.freedesktop.portal.Desktop')
    expect(polarisStart).toContain('status org.freedesktop.impl.portal.desktop.gamescope')
    expect(polarisStart).toContain('export POLARIS_PORTAL_DBUS_ADDRESS="$private_address"')
    expect(polarisStart).not.toContain('unix:path=%t/polaris-portal/bus')
    expect(waitPortal).toContain('private_address="unix:path=$bus_path"')
    expect(waitPortal).toContain('--address="$private_address"')
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
})
