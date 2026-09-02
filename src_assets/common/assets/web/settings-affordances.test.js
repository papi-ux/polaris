import { readFileSync } from 'node:fs'
import { join } from 'node:path'

const webSource = (relativePath) => readFileSync(
  join(process.cwd(), 'src_assets/common/assets/web', relativePath),
  'utf8',
)

// Settings surfaces migrated onto the shared vocabulary. Every file here must
// route editable fields through .settings-input and stay off the raw literals.
const migratedSettingsSources = [
  'configs/tabs/AudioVideo.vue',
  'configs/tabs/General.vue',
  'configs/tabs/Network.vue',
  'configs/tabs/Inputs.vue',
  'configs/tabs/Files.vue',
  'configs/tabs/Advanced.vue',
  'configs/tabs/audiovideo/AdapterNameSelector.vue',
  'configs/tabs/audiovideo/DisplayDeviceOptions.vue',
  'configs/tabs/audiovideo/DisplayOutputSelector.vue',
  'configs/tabs/encoders/AmdAmfEncoder.vue',
  'configs/tabs/encoders/IntelQuickSyncEncoder.vue',
  'configs/tabs/encoders/NvidiaNvencEncoder.vue',
  'configs/tabs/encoders/SoftwareEncoder.vue',
  'configs/tabs/encoders/VideotoolboxEncoder.vue',
  'views/PasswordView.vue',
  'views/PinView.vue',
]

// The settings pages mix editable controls with host status. These guards
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

  it('declares the vocabulary in the components layer so utilities can override it', () => {
    const css = webSource('app.css')

    // Unlayered author CSS beats layered utilities after the build flattens
    // layers, which silently killed per-use padding overrides like py-2.5.
    expect(css).toMatch(/@layer components \{[\s\S]*\.settings-input[\s\S]*\.selectable-card[\s\S]*\.stat-tile-compact[\s\S]*\n\}/)
  })

  it('keeps the pointer cursor on interactive cards via selectable-card', () => {
    const css = webSource('app.css')

    expect(css).toMatch(/\.selectable-card\s*\{[^}]*cursor-pointer/s)
    expect(css).toMatch(/\.selectable-card:disabled\s*\{[^}]*cursor-default/s)
  })

  it('keeps read-only tiles off the input-look recipe across the settings surfaces', () => {
    const source = webSource('configs/tabs/AudioVideo.vue')
    const statTile = webSource('components/StatTile.vue')

    // The old read-only tile literal is nearly indistinguishable from the
    // editable input recipe; every status tile now renders through the shared
    // StatTile component, which is the only holder of the stat grammar here.
    expect(source).toContain('<StatTile')
    expect(source).not.toContain('stat-tile-compact')
    expect(statTile).toContain('stat-tile-compact')
    expect(statTile).toContain('stat-kicker')
    expect(statTile).toContain('data-readonly')

    for (const path of migratedSettingsSources) {
      expect(webSource(path), path).not.toContain('border-storm/20 bg-void/25')
    }
  })

  it('routes every editable field through settings-input across the settings surfaces', () => {
    for (const path of migratedSettingsSources) {
      const source = webSource(path)

      // Both historical orderings of the raw editable-input recipe.
      expect(source, path).not.toMatch(/border border-storm bg-deep px-3 py-2 text-silver/)
      expect(source, path).not.toMatch(/bg-deep border border-storm rounded-lg px-3 py-2/)
      expect(source, path).toContain('settings-input')
    }
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
    const locale = webSource('public/assets/locale/en.json')

    // The label text moved into the locale file; the source must still wire
    // the key so the mono value keeps its plain-language caption.
    expect(source).toContain('config.av_planner_target_mode')
    expect(locale).toContain('"av_planner_target_mode": "Target mode"')
    expect(source).not.toMatch(/rounded-md border border-storm\/20 bg-void\/25[^"]*font-mono/)
  })

  it('treats the Auto Quality split state as a real state instead of dead code', () => {
    const source = webSource('configs/tabs/AudioVideo.vue')

    expect(source).toMatch(/autoQualityPartial[\s\S]{0,200}!==/)
  })
})

// F7: the Video/Audio page binds host truth from the settings projection and
// keeps a config-derived fallback for hosts that answer 404.
describe('settings projection binding', () => {
  it('reads badges, availability, provenance, and the live strip from the projection with a fallback', () => {
    const source = webSource('configs/tabs/AudioVideo.vue')

    expect(source).toContain("from '../../composables/useConfigProjection'")
    expect(source).toContain('projection.load()')
    expect(source).toContain('projectionModes.value?.find(')
    expect(source).toContain('resolveStreamDisplayModeAvailability(mode.id, config.value.stream_display_mode_options)')
    expect(source).toContain('data-auto-quality-strip')
    expect(source).toContain("data-provenance=\"max_bitrate\"")
    expect(source).toContain("data-provenance=\"fallback_mode\"")
    expect(source).toContain("data-provenance=\"adaptive_bitrate_enabled\"")
    expect(source).toMatch(/autoQualityLive \? autoQualityLiveRows : autoQualityRows/)
  })

  it('lets the host name the response-only keys the save path strips', () => {
    expect(webSource('views/ConfigView.vue')).toContain('resolveConfigResponseOnlyKeys(source)')
    expect(webSource('client-settings-sync.js')).toContain('export function resolveConfigResponseOnlyKeys')
  })
})

// Vue 3 does not interpolate mustaches inside attributes: `attr="{{ expr }}"`
// ships the literal braces to the DOM. Dynamic attributes must be bound.
describe('attribute interpolation', () => {
  it.each(migratedSettingsSources)('%s binds dynamic attributes instead of interpolating them', (relativePath) => {
    const source = webSource(relativePath)
    const offenders = source.split('\n').filter((line) => /\s[a-zA-Z-]+="[^"]*\{\{/.test(line))
    expect(offenders, `mustache inside an attribute in ${relativePath}`).toEqual([])
  })

  it('binds the Auto Quality strip source marker', () => {
    const source = webSource('configs/tabs/AudioVideo.vue')
    expect(source).toContain(`:data-auto-quality-strip-source="autoQualityLive ? 'host' : 'saved'"`)
  })
})
