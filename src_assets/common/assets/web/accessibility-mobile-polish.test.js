import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'

function webSource(relativePath) {
  return readFileSync(join(process.cwd(), 'src_assets/common/assets/web', relativePath), 'utf8')
}

describe('cross-cutting accessibility and mobile polish', () => {
  it('gives icon-only app chrome controls explicit accessible names', () => {
    const app = webSource('App.vue')

    expect(app).toContain(':aria-label="`Switch theme to ${nextThemeLabel}`"')
    expect(app).toContain(':aria-label="`Open command palette (${paletteShortcut})`"')
    expect(app).toContain(':aria-label="sidebarCollapsed ? \'Expand sidebar\' : \'Collapse sidebar\'"')
    expect(app).toContain('aria-hidden="true" v-html="item.icon"')
  })

  it('marks live status and progress surfaces for assistive technology updates', () => {
    const dashboard = webSource('views/DashboardView.vue')
    const apps = webSource('views/AppsView.vue')
    const config = webSource('views/ConfigView.vue')

    expect(dashboard).toContain('role="status"')
    expect(dashboard).toContain('aria-live="polite"')
    expect(dashboard).toContain('aria-label="Live stream telemetry summary"')
    expect(apps).toContain('role="status"')
    expect(config).toContain('role="status"')
  })

  it('keeps sticky review bars from covering content on handheld widths', () => {
    const css = webSource('app.css')

    expect(css).toContain('@media (max-width: 47.99rem)')
    expect(css).toContain('.operator-console .library-import-staged-summary')
    expect(css).toContain('position: static')
    expect(css).toContain('.settings-command-bar.sticky')
    expect(css).toContain('top: auto')
  })
})
