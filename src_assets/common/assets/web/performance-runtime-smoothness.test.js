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
    // The sparklines moved into the stage's signal strip with the same lazy load and reduced-motion hiding.
    const strip = source('src_assets/common/assets/web/components/SignalStrip.vue')

    expect(strip).toContain("await import('uplot')")
    expect(strip).toContain("await import('uplot/dist/uPlot.min.css')")
    expect(strip).toContain('v-if="!prefersReducedMotion" class="dashboard-strip-spark')
    expect(dashboard).toContain(':prefers-reduced-motion="prefersReducedMotion"')
    expect(dashboard).toContain('Live charts are paused while reduced motion is enabled')
    expect(dashboard).toContain("window.matchMedia('(prefers-reduced-motion: reduce)')")
    // Reduced motion still chooses the polling preview over the self-animating stream. Issue #408
    // makes the preview opt-in: the stats watcher used to start it on a new stream unless motion
    // was reduced; now no stream update starts it (DashboardView.spaces.test.js covers the behaviour).
    expect(dashboard).toContain("previewMode.value = prefersReducedMotion.value ? 'poll' : 'mjpeg'")
    const watcher = dashboard.slice(dashboard.indexOf('watch(stats, (newStats, oldStats) => {'))
    expect(watcher.slice(0, watcher.indexOf('\n})\n'))).not.toContain('startPreview(')
  })

  it('removes expensive live cockpit effects for reduced motion and mobile landscape', () => {
    const css = source('src_assets/common/assets/web/app.css')

    expect(css).toContain('@media (prefers-reduced-motion: reduce)')
    expect(css).toContain('.dashboard-preview-overlay .animate-spin')
    expect(css).toContain('@media (orientation: landscape) and (max-height: 32rem) and (max-width: 64rem)')
    expect(css).toContain('backdrop-filter: none;')
    // Issue #408 replaced the live summary tiles and preview panel with the Mission Control stage,
    // which drops its shadow in the same short-landscape rule.
    const landscape = css.slice(css.indexOf('@media (orientation: landscape) and (max-height: 32rem) and (max-width: 64rem)'))
    expect(landscape.slice(0, landscape.indexOf('\n}\n'))).toMatch(/\.mc-stage \{\s*box-shadow: none;/)
  })
})
