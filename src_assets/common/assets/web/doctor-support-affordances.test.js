import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'

const webSource = (relativePath) => readFileSync(
  join(process.cwd(), 'src_assets/common/assets/web', relativePath),
  'utf8',
)

const view = () => webSource('views/TroubleshootingView.vue')
const template = () => view().slice(0, view().indexOf('</template>\n\n<script setup>'))

// Doctor & Support follows the Video/Audio grammar: status renders through
// the shared read-only tile, actions look like buttons, editable fields share
// the settings input, reference prose lives in the docs, and the copy is
// keyed through the locale file.
describe('doctor and support affordances', () => {
  it('renders every status tile through StatTile instead of the old tile literals', () => {
    const source = view()
    expect(source).toContain("import StatTile from '../components/StatTile.vue'")
    expect(source).toContain('<StatTile v-for="item in sessionSnapshotRows.summary"')
    expect(source).toContain('<StatTile v-for="item in sessionSnapshotRows.details"')
    expect(source).toContain('<StatTile v-for="item in doctorAdvancedItems"')
    expect(source).not.toContain('rounded-xl border border-storm/25 bg-deep/60 px-3 py-3')
    expect(source).not.toContain('rounded-xl border border-storm/20 bg-deep/40 px-3 py-2.5')
    expect(source).not.toContain('rounded-lg border border-storm/15 bg-void/40 px-3 py-2">\n              <div class="text-[11px] uppercase tracking-wide text-storm">{{ item.label }}')
  })

  it('gives every diagnostics action card the button grammar', () => {
    const source = template()
    const cards = source.match(/troubleshooting-action-card"/g) || []
    const marked = source.match(/troubleshooting-action-card" data-action-card/g) || []
    expect(cards.length).toBeGreaterThanOrEqual(6)
    expect(marked.length).toBe(cards.length)
    const css = webSource('app.css')
    const rule = css.match(/\.troubleshooting-action-card \{[^}]*\}/s)?.[0] || ''
    expect(rule).toContain('cursor-pointer')
    expect(rule).toContain('active:translate-y-px')
    expect(rule).toContain('border-ice/25')
  })

  it('routes the log filter and the report notes through the shared settings input', () => {
    const source = template()
    expect(source).toContain('class="settings-input text-sm sm:w-72"')
    expect(source).toContain('class="settings-input text-sm"')
    expect(source).not.toContain('rounded-lg border border-storm/50 bg-deep px-3 py-1.5')
    expect(source).not.toContain('w-full rounded-xl border border-storm/25 bg-deep/35 px-3 py-2 text-sm text-silver')
  })

  it('points reference prose at the docs instead of restating it on the page', () => {
    const source = template()
    for (const url of [
      'https://papi-ux.com/docs/doctor/',
      'https://papi-ux.com/docs/doctor/#optional-ai-explanation',
      'https://papi-ux.com/docs/troubleshooting/#built-in-self-tests',
      'https://papi-ux.com/docs/troubleshooting/#quick-recovery-ladder',
      'https://papi-ux.com/docs/troubleshooting/#what-is-redacted',
      'https://papi-ux.com/docs/troubleshooting/#reporting-a-crash',
    ]) {
      expect(source, url).toContain(`href="${url}"`)
    }
    const docs = readFileSync(join(process.cwd(), 'docs/troubleshooting.md'), 'utf8')
    expect(docs).toContain('## Quick recovery ladder')
    expect(docs).toContain('## Built-in self tests')
    expect(docs).toContain('### What is redacted')
    expect(docs).toContain('### Reporting a crash')
    expect(readFileSync(join(process.cwd(), 'docs/doctor.md'), 'utf8')).toContain('## Optional AI explanation')
  })

  it('keeps the template free of hardcoded English sentences', () => {
    const offenders = template()
      .split('\n')
      .filter((line) => /^\s*[A-Z][a-z]+(?: [a-z,]+){2,}/.test(line) || />[A-Z][a-z]+(?: [a-z,]+){2,}[^<{]*</.test(line))
    expect(offenders).toEqual([])
  })

  it('binds dynamic attributes instead of interpolating them', () => {
    const offenders = template().split('\n').filter((line) => /\s[a-zA-Z-]+="[^"]*\{\{/.test(line))
    expect(offenders).toEqual([])
  })

  it('tells the truth about AI readiness and the previous run', () => {
    const source = view()
    const locale = webSource('public/assets/locale/en.json')
    expect(source).toContain("from '../doctor-ai-readiness.js'")
    expect(source).toContain(':data-ai-readiness="aiReadiness.state"')
    expect(source).toContain('{{ aiReadinessText }}')
    expect(source).toContain("from '../previous-run-banner.js'")
    expect(source).toContain(':data-previous-run-outcome="previousRunBanner.crashed ? \'crashed\' : \'unclean\'"')
    expect(locale).not.toContain('"previous_run_title"')
    expect(locale).toContain('"previous_run_crashed_title": "The previous run crashed"')
    expect(locale).toContain('"previous_run_unclean_title": "The previous run ended without a recorded exit"')
  })

  it('reads the host settings projection for the session snapshot', () => {
    const source = view()
    expect(source).toContain("from '../composables/useConfigProjection'")
    expect(source).toContain('buildSessionSnapshotRows(streamStats.value, i18n.t, {')
    expect(source).toContain('streamDisplay: projection.ok.value ? projection.streamDisplay.value : null')
    expect(source).toContain('provenance: projection.ok.value ? projection.provenance.value : null')
  })
})
