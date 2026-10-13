import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'

function webSource(relativePath) {
  return readFileSync(join(process.cwd(), 'src_assets/common/assets/web', relativePath), 'utf8')
}

describe('cross-cutting accessibility and mobile polish', () => {
  it('gives icon-only app chrome controls explicit accessible names', () => {
    const app = webSource('App.vue')
    const themeToggle = webSource('ThemeToggle.vue')

    // The theme control is the shared ThemeToggle picker with an explicit
    // accessible name and listbox semantics.
    expect(app).toContain('<ThemeToggle :collapsed="sidebarCollapsed" />')
    expect(themeToggle).toContain('aria-label="Choose theme"')
    expect(themeToggle).toContain('aria-haspopup="listbox"')
    expect(themeToggle).toContain('role="listbox"')
    expect(themeToggle).toContain('role="option"')
    expect(app).toContain(':aria-label="`Open command palette (${paletteShortcut})`"')
    expect(app).toContain(':aria-label="sidebarCollapsed ? \'Expand sidebar\' : \'Collapse sidebar\'"')
    expect(app).toContain('aria-hidden="true" v-html="item.icon"')
    expect(app).toContain('item.badge ? `${item.label}, ${item.badge}` : item.label')
    expect(app).not.toContain('{{ item.shortcut }}')
    expect(app).not.toContain('parseInt(e.key)')
    expect(themeToggle).toContain('>Appearance</span>')
  })

  it('keeps the mobile shell keyboard navigable with a landmark skip target and menu state', () => {
    const app = webSource('App.vue')
    const css = webSource('app.css')

    expect(app).toContain('href="#polaris-main"')
    expect(app).toContain('id="polaris-main"')
    expect(app).toContain('tabindex="-1"')
    expect(app).toContain('id="polaris-sidebar"')
    expect(app).toContain('aria-controls="polaris-sidebar"')
    expect(app).toContain(':aria-expanded="sidebarOpen"')
    expect(css).toContain('.skip-link')
    expect(css).toContain('focus-visible:translate-y-0')
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

  it('gives the Spaces console named regions, live status and reachable targets', () => {
    const view = webSource('views/SpacesView.vue')
    const assignments = webSource('components/MultiseatAssignments.vue')
    const list = webSource('components/SpacesList.vue')
    const setup = webSource('components/SpacesSetup.vue')
    const firstSetup = webSource('components/SpacesFirstSetup.vue')
    const access = webSource('components/SpacesDevices.vue')

    expect(view).toContain('aria-labelledby="spaces-intro-title"')
    expect(assignments).toContain('aria-labelledby="profile-assignment-title"')
    expect(assignments).toContain('role="status" data-stream-lock')
    expect(access).toContain('aria-describedby')
    expect(list).toContain('<ConfirmActionDialog')
    expect(list).not.toContain('<form v-if="selected"')
    expect(setup).toContain('role="status" aria-live="polite"')
    expect(setup).toContain('md:grid-cols-2')
    expect(firstSetup).toContain('role="group" aria-labelledby="spaces-first-title"')
    expect(firstSetup).not.toContain('.job-button')
    expect(access).toContain('class="h-4 w-4 shrink-0 rounded border-storm bg-void text-ice accent-ice"')
    // The device table keeps its roles when a narrow container lays its rows out as cards.
    for (const role of ['role="table"', 'role="row"', 'role="columnheader"', 'role="rowheader"', 'role="cell"']) expect(access).toContain(role)
    expect(webSource('app.css')).toContain('.devices-wrap {\n    container-type: inline-size;')
    for (const source of [view, assignments, list, setup, firstSetup, access]) {
      expect(source).not.toMatch(/rgb\(136 192 208/)
    }
  })

  it('keeps sticky review bars from covering content on handheld widths', () => {
    const css = webSource('app.css')

    expect(css).toContain('@media (max-width: 47.99rem)')
    expect(css).toContain('.operator-console .library-import-staged-summary')
    expect(css).toContain('position: static')
    expect(css).toContain('.settings-command-bar.sticky')
    expect(css).toContain('top: auto')
  })

  it('keeps compact state labels readable while Doctor actions can wrap within their cards', () => {
    const css = webSource('app.css')
    const audioVideo = webSource('configs/tabs/AudioVideo.vue')

    expect(css).toMatch(/\.system-footer-link\s*\{[^}]*gap:\s*0\.375rem[^}]*white-space:\s*nowrap/s)
    // Long translated or enlarged Doctor actions must stay inside their card.
    // The browser regressions check both font preferences and CSS-root scaling.
    expect(css).toMatch(/\.troubleshooting-action-button\s*\{[^}]*min-h-9 max-w-full[^}]*whitespace-normal/s)
    expect(audioVideo).toContain('class="shrink-0 whitespace-nowrap rounded-full')
  })

  it('wraps Doctor controls and shared subtle cards at whole words, dropping controls below headings', () => {
    const css = webSource('app.css')
    const doctor = webSource('views/TroubleshootingView.vue')
    const rule = selector => css.match(new RegExp(`(?:^|\\n)${selector.replace(/[.*+?^${}()|[\]\\]/g, '\\$&')}\\s*\\{([^}]*)\\}`))?.[1] ?? ''

    // break-word only splits a token wider than its box; anywhere also shrinks the
    // min-content width, which let flex rows squeeze words into single syllables.
    for (const selector of ['.surface-subtle', '.troubleshooting-action-button', '[data-session-snapshot] .meta-pill', '.troubleshooting-page-header .meta-pill']) {
      expect(rule(selector), selector).toMatch(/overflow-wrap:\s*break-word/)
      expect(rule(selector), selector).not.toContain('anywhere')
    }
    expect(rule('[data-session-snapshot] .meta-pill')).toMatch(/line-height:\s*1\.25[\s\S]*border-radius:\s*1rem/)
    // Rows only wrap where they run horizontally; the copy keeps a readable floor
    // and the controls move to their own line instead of narrowing it.
    expect(css).toMatch(/@media \(min-width: 64rem\)\s*\{\s*\.troubleshooting-action-row\s*\{\s*flex-wrap:\s*wrap;\s*\}\s*\.troubleshooting-action-row > :first-child\s*\{\s*flex:\s*1 1 24rem;\s*min-width:\s*0;/)
    expect(css).toMatch(/@media \(min-width: 80rem\)\s*\{\s*\.troubleshooting-page-header\s*\{\s*flex-wrap:\s*wrap;\s*\}\s*\.troubleshooting-page-header > \.page-heading\s*\{\s*flex:\s*1 1 24rem;/)
    expect(doctor.match(/class="troubleshooting-action-row flex flex-col gap-3 lg:flex-row lg:items-start lg:justify-between"/g)).toHaveLength(5)
    expect(doctor).toContain('<section class="page-header troubleshooting-page-header">')
    // Card titles and their status badges share a row until enlarged text needs
    // both lines; then the badge drops below instead of pushing out of the card.
    expect(doctor.match(/<div class="troubleshooting-card-header flex flex-wrap items-center justify-between gap-x-3 gap-y-1">/g)).toHaveLength(10)
    expect(doctor).not.toContain('<div class="flex items-center justify-between gap-3">')
    expect(rule('.troubleshooting-card-header > *')).toMatch(/max-width:\s*100%/)
    // Self-test columns are sized in text units: three at desktop sizes, one when
    // enlarged text would leave three slivers.
    expect(doctor).toContain('<div class="troubleshooting-card-grid grid gap-3">')
    expect(css).toMatch(/@media \(min-width: 80rem\)\s*\{\s*\.troubleshooting-card-grid\s*\{\s*grid-template-columns:\s*repeat\(auto-fit, minmax\(min\(100%, max\(16rem, 30%\)\), 1fr\)\);/)
  })

  it('lays out pairing context as separate readable groups and stacks them on phones', () => {
    const css = webSource('app.css')
    const pairing = webSource('views/PinView.vue')

    expect(pairing).toContain('class="pairing-flow-strip mt-5"')
    expect(css).toMatch(/\.pairing-flow-strip\s*\{[^}]*display:\s*flex[^}]*justify-content:\s*space-between/s)
    expect(css).toMatch(/\.pairing-flow-copy\s*\{[^}]*min-width:\s*0/s)
    expect(css).toMatch(/\.pairing-inline-note\s*\{[^}]*border-left:/s)
    expect(css).toMatch(/@media \(max-width: 47\.99rem\)[\s\S]*?\.pairing-flow-strip\s*\{[^}]*flex-direction:\s*column/)
  })
})
