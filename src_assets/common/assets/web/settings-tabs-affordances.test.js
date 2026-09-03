import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'

const webSource = (relativePath) => readFileSync(
  join(process.cwd(), 'src_assets/common/assets/web', relativePath),
  'utf8',
)
const templateOf = (source) => {
  const start = source.indexOf('<template>')
  const end = source.lastIndexOf('</template>')
  return source.slice(start, end)
}

const tabs = {
  'configs/tabs/General.vue': 'https://papi-ux.com/docs/configuration/#general-tab',
  'configs/tabs/Inputs.vue': 'https://papi-ux.com/docs/configuration/#input-tab',
  'configs/tabs/Network.vue': 'https://papi-ux.com/docs/configuration/#network-tab',
  'configs/tabs/Advanced.vue': 'https://papi-ux.com/docs/configuration/#advanced-tab',
  'configs/tabs/Files.vue': 'https://papi-ux.com/docs/configuration/#files',
  'configs/tabs/AiOptimizer.vue': 'https://papi-ux.com/docs/configuration/#ai-provider-settings',
}

// The Settings tabs follow the grammar Video/Audio set in #584: keyed copy,
// shared inputs, read-only tiles for status, selectable cards for choices,
// and one docs pointer per tab.
describe('settings tabs affordances', () => {
  it.each(Object.keys(tabs))('%s keeps its template free of hardcoded English sentences', (path) => {
    const offenders = templateOf(webSource(path))
      .split('\n')
      .filter((line) => /^\s*[A-Z][a-z]+(?: [a-z,]+){2,}/.test(line) || />[A-Z][a-z]+(?: [a-z,]+){2,}[^<{]*</.test(line) || /\slabel="[A-Z][^"]*"/.test(line))
    expect(offenders).toEqual([])
  })

  it.each(Object.keys(tabs))('%s binds dynamic attributes instead of interpolating them', (path) => {
    const offenders = templateOf(webSource(path)).split('\n').filter((line) => /\s[a-zA-Z-]+="[^"]*\{\{/.test(line))
    expect(offenders).toEqual([])
  })

  it.each(Object.entries(tabs))('%s points at its guide', (path, url) => {
    expect(webSource(path)).toContain(`href="${url}"`)
  })

  it.each(['configs/tabs/General.vue', 'configs/tabs/Inputs.vue', 'configs/tabs/Network.vue', 'configs/tabs/Files.vue'])('%s carries no (i) hints; that depth lives in the reference tables', (path) => {
    expect(webSource(path)).not.toContain('<InfoHint')
  })

  it('backs every tab pointer with a field table in the reference', () => {
    const configuration = readFileSync(join(process.cwd(), 'docs/configuration.md'), 'utf8')
    for (const tab of ['General', 'Input', 'Network', 'Advanced', 'Files']) {
      const start = configuration.indexOf(`### ${tab} tab`)
      expect(start, tab).toBeGreaterThan(-1)
      expect(configuration.slice(start, start + 4000)).toContain('| Field | What it does |')
    }
  })

  it('anchors every docs pointer to a real heading', () => {
    const configuration = readFileSync(join(process.cwd(), 'docs/configuration.md'), 'utf8')
    const troubleshooting = readFileSync(join(process.cwd(), 'docs/troubleshooting.md'), 'utf8')
    expect(configuration).toContain('## Common options')
    for (const tab of ['General', 'Input', 'Network', 'Advanced']) {
      expect(configuration).toContain(`### ${tab} tab`)
    }
    expect(configuration).toContain('## Files')
    expect(configuration).toContain('## AI provider settings')
    expect(troubleshooting).toContain('## Input does not work')
  })

  it('renders the AI tab through the shared input, tile, and selectable card', () => {
    const source = webSource('configs/tabs/AiOptimizer.vue')
    expect(source).toContain("import SelectableCard from '../../components/SelectableCard.vue'")
    expect(source).toContain("import StatTile from '../../components/StatTile.vue'")
    expect((source.match(/class="settings-input[^"]*"/g) || []).length).toBe(8)
    expect(source).not.toContain('bg-void/50 border border-storm/50 rounded-lg')
    expect((source.match(/<SelectableCard\n/g) || []).length).toBe(3)
    expect((source.match(/<StatTile/g) || []).length).toBeGreaterThanOrEqual(10)
    expect(source).not.toContain('rounded-xl border border-storm/30 bg-void/30 p-3')
    expect(source).toContain('role="switch"')
  })

  it('reads the saved runtime readiness the same way Doctor & Support does', () => {
    const source = webSource('configs/tabs/AiOptimizer.vue')
    expect(source).toContain("from '../../doctor-ai-readiness.js'")
    expect(source).toContain('data-ai-readiness-panel :data-ai-readiness="aiReadiness.state"')
    expect(source).toContain('{{ aiReadinessText }}')
  })
})
