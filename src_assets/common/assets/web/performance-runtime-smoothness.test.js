import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'

function source(relativePath) {
  return readFileSync(join(process.cwd(), relativePath), 'utf8')
}

describe('web performance runtime smoothness guardrails', () => {
  it('exposes a build budget command for validating emitted assets after Vite builds', () => {
    const pkg = JSON.parse(source('package.json'))
    const budgetScript = source('scripts/check-web-performance-budget.mjs')

    expect(pkg.scripts['budget:web']).toBe('node scripts/check-web-performance-budget.mjs')
    expect(pkg.scripts['build:budget']).toBe('npm run build && npm run budget:web')
    expect(budgetScript).toContain('initial app JS')
    expect(budgetScript).toContain('lazy uPlot JS')
    expect(budgetScript).toContain('run npm run build first')
  })

  it('keeps live telemetry charts lazy and pauses them for reduced motion', () => {
    const dashboard = source('src_assets/common/assets/web/views/DashboardView.vue')

    expect(dashboard).toContain("await import('uplot')")
    expect(dashboard).toContain("await import('uplot/dist/uPlot.min.css')")
    expect(dashboard).toContain('v-if="!prefersReducedMotion" class="dashboard-telemetry-grid')
    expect(dashboard).toContain('Live charts are paused while reduced motion is enabled')
    expect(dashboard).toContain("window.matchMedia('(prefers-reduced-motion: reduce)')")
    expect(dashboard).toContain('!showPreview.value && !prefersReducedMotion.value')
  })

  it('removes expensive live cockpit effects for reduced motion and mobile landscape', () => {
    const css = source('src_assets/common/assets/web/app.css')

    expect(css).toContain('@media (prefers-reduced-motion: reduce)')
    expect(css).toContain('.dashboard-preview-overlay .animate-spin')
    expect(css).toContain('@media (orientation: landscape) and (max-height: 32rem) and (max-width: 64rem)')
    expect(css).toContain('backdrop-filter: none;')
    expect(css).toContain('.dashboard-live-summary-tile')
  })
})
