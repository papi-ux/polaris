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

    const gameModeHost = readFileSync(join(process.cwd(), 'src/platform/linux/game_mode_host.cpp'), 'utf8')

    expect(confighttp).toContain('output["boot_readiness"]')
    expect(confighttp).toContain('game_mode_host::boot_readiness_guidance')
    expect(confighttp).toContain('game_mode_host::display_session_guidance')
    // The statuses and copy themselves are covered by tests/unit/platform/test_game_mode_host.cpp.
    expect(gameModeHost).toContain('expected on a headless-boot host')
  })

  it('names a running Game Mode session instead of asking for a desktop restart', () => {
    // A handheld that switched to Game Mode still has Polaris up when it boots
    // independently, but nothing inside that session can be streamed yet. The
    // console must say so rather than repeat the desktop-restart advice.
    const home = readFileSync(join(process.cwd(), 'src_assets/common/assets/web/views/HomeView.vue'), 'utf8')
    const composable = readFileSync(join(process.cwd(), 'src_assets/common/assets/web/composables/useSystemStats.js'), 'utf8')
    const confighttp = readFileSync(join(process.cwd(), 'src/confighttp.cpp'), 'utf8')
    const gameModeHost = readFileSync(join(process.cwd(), 'src/platform/linux/game_mode_host.cpp'), 'utf8')

    expect(composable).toContain('data.game_mode_host')
    expect(composable).toMatch(/return \{[^}]*\bgameModeHost\b[^}]*\}/)
    expect(home).toContain('gameModeHost?.session_active')
    expect(home).toContain("$t('index.session_game_mode')")
    expect(home.indexOf('gameModeHost?.session_active')).toBeLessThan(home.indexOf("displaySession?.status === 'missing_display_environment'"))
    expect(readFileSync(join(process.cwd(), 'src_assets/common/assets/web/public/assets/locale/en.json'), 'utf8')).toContain('Streaming from inside Game Mode is not supported yet')
    expect(confighttp).toContain('output["game_mode_host"]')
    expect(gameModeHost).toContain('"game_mode_session"')
  })
})
