import { readFileSync } from 'node:fs'
import { join } from 'node:path'

const webSource = (relativePath) => readFileSync(
  join(process.cwd(), 'src_assets/common/assets/web', relativePath),
  'utf8',
)

// The Video/Audio page mixes editable controls with host status. These guards
// keep the two visually distinct: settings look like settings, status looks
// like status, and nothing renders as a text field unless it accepts input.
describe('settings affordances', () => {
  it('defines the shared settings vocabulary in app.css', () => {
    const css = webSource('app.css')

    expect(css).toMatch(/\.settings-input\s*\{/)
    expect(css).toMatch(/\.settings-disclosure > summary\s*\{/)
    expect(css).toMatch(/\.settings-disclosure\[open\]/)
    expect(css).toMatch(/\.settings-summary-copy\s*\{/)
    expect(css).toMatch(/\.selectable-card\s*\{/)
    expect(css).toMatch(/\.selectable-card:disabled\s*\{/)
    expect(css).toMatch(/\.stat-tile-compact\s*\{/)
  })

  it('keeps the pointer cursor on interactive cards via selectable-card', () => {
    const css = webSource('app.css')

    expect(css).toMatch(/\.selectable-card\s*\{[^}]*cursor-pointer/s)
    expect(css).toMatch(/\.selectable-card:disabled\s*\{[^}]*cursor-default/s)
  })

  it('keeps read-only tiles off the input-look recipe on the Video/Audio page', () => {
    const source = webSource('configs/tabs/AudioVideo.vue')
    const statTile = webSource('components/StatTile.vue')

    // The old read-only tile literal is nearly indistinguishable from the
    // editable input recipe; every status tile now renders through the shared
    // StatTile component, which is the only holder of the stat grammar here.
    expect(source).not.toContain('border-storm/20 bg-void/25')
    expect(source).toContain('<StatTile')
    expect(source).not.toContain('stat-tile-compact')
    expect(statTile).toContain('stat-tile-compact')
    expect(statTile).toContain('stat-kicker')
    expect(statTile).toContain('data-readonly')
  })

  it('routes every editable field through settings-input on the Video/Audio page', () => {
    const source = webSource('configs/tabs/AudioVideo.vue')

    expect(source).not.toMatch(/border border-storm bg-deep px-3 py-2 text-silver/)
    expect(source).not.toMatch(/bg-deep border border-storm rounded-lg px-3 py-2/)
    expect(source).toContain('settings-input')
  })

  it('marks every choice card on the Video/Audio page as selectable with a pressed state', () => {
    const source = webSource('configs/tabs/AudioVideo.vue')
    const selectableCard = webSource('components/SelectableCard.vue')

    const selectable = source.match(/<SelectableCard/g) || []
    const selected = source.match(/:selected=/g) || []
    // Mode picker, planner presets, and advanced scale factors.
    expect(selectable.length).toBeGreaterThanOrEqual(3)
    expect(selected.length).toBeGreaterThanOrEqual(3)
    // The shared component owns the vocabulary and the pressed-state wiring.
    expect(selectableCard).toContain('selectable-card focus-ring')
    expect(selectableCard).toContain('type="button"')
    expect(selectableCard).toContain(':aria-pressed=')
  })

  it('publishes the shared status vocabulary as importable components', () => {
    const statusTones = webSource('status-tones.js')
    const statusBadge = webSource('components/StatusBadge.vue')
    const troubleshooting = webSource('views/TroubleshootingView.vue')

    // One tone table serves cards, badges, and labels; views import it
    // instead of growing private pass/fail/warning switch copies.
    expect(statusTones).toContain('export function statusTone')
    expect(statusBadge).toContain('meta-pill')
    expect(statusBadge).toContain("from '../status-tones.js'")
    expect(troubleshooting).toContain("from '../status-tones.js'")
    expect(troubleshooting).not.toContain('selfTestCardClass')
    expect(troubleshooting).not.toContain('fixMyStreamBadgeClass')
  })

  it('labels the planner mono value as a target mode instead of dressing it as an input', () => {
    const source = webSource('configs/tabs/AudioVideo.vue')

    expect(source).toContain('Target mode')
    expect(source).not.toMatch(/rounded-md border border-storm\/20 bg-void\/25[^"]*font-mono/)
  })

  it('treats the Auto Quality split state as a real state instead of dead code', () => {
    const source = webSource('configs/tabs/AudioVideo.vue')

    expect(source).toMatch(/autoQualityPartial[\s\S]{0,200}!==/)
  })
})
