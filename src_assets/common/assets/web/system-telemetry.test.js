import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'

describe('System telemetry display-session guidance', () => {
  it('surfaces automatic display environment repair and recovery guidance in the app shell', () => {
    const home = readFileSync(join(process.cwd(), 'src_assets/common/assets/web/views/HomeView.vue'), 'utf8')
    const composable = readFileSync(join(process.cwd(), 'src_assets/common/assets/web/composables/useSystemStats.js'), 'utf8')

    expect(composable).toContain('displaySession')
    expect(composable).toContain('data.display_session')
    expect(home).toContain('data-display-session-health')
    expect(home).toContain('environment_repaired')
    expect(home).toContain('missing_display_environment')
    expect(home).toContain("$t('index.session_missing_env')")
    expect(readFileSync(join(process.cwd(), 'src_assets/common/assets/web/public/assets/locale/en.json'), 'utf8')).toContain('Restart Polaris from the desktop session')
  })

  it('reports boot readiness and keeps headless-boot hosts off the restart-from-desktop advice', () => {
    // /api/stats/system must say whether this host survives a reboot with no
    // desktop login, and a host that deliberately boots headless must not be
    // told its missing desktop environment is a problem to fix by restarting
    // from the desktop.
    const confighttp = readFileSync(join(process.cwd(), 'src/confighttp.cpp'), 'utf8')

    expect(confighttp).toContain('boot_readiness')
    expect(confighttp).toContain('boot_independent')
    expect(confighttp).toContain('session_bound')
    expect(confighttp).toContain('/var/lib/systemd/linger')
    expect(confighttp).toContain('default.target.wants/polaris.service')
    expect(confighttp).toContain('sudo -H polaris --setup-host --enable-headless-boot')
    expect(confighttp).toContain('expected on a headless-boot host')
  })
})
