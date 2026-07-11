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
    expect(home).toContain('Restart Polaris from the desktop session')
  })
})
