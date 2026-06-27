import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'

function webSource(relativePath) {
  return readFileSync(join(process.cwd(), 'src_assets/common/assets/web', relativePath), 'utf8')
}

function expectBefore(source, first, second) {
  const firstIndex = source.indexOf(first)
  const secondIndex = source.indexOf(second)
  expect(firstIndex, `${first} should exist`).toBeGreaterThanOrEqual(0)
  expect(secondIndex, `${second} should exist`).toBeGreaterThanOrEqual(0)
  expect(firstIndex, `${first} should appear before ${second}`).toBeLessThan(secondIndex)
}

describe('DashboardView live-session hierarchy', () => {
  it('starts live mode with a summary strip covering quality, signal, path, and runtime', () => {
    const dashboard = webSource('views/DashboardView.vue')

    expectBefore(dashboard, 'dashboard-live-summary-grid', 'Auto Quality')
    expectBefore(dashboard, 'dashboard-live-summary-grid', 'dashboard-preview-panel')

    for (const label of ['Quality', 'Latency', 'FPS', 'Loss', 'Bitrate', 'Capture path', 'Runtime mode']) {
      expect(dashboard).toContain(`data-live-summary-metric="${label}"`)
    }
  })

  it('keeps secondary live panels in collapsible groups below the primary summary', () => {
    const dashboard = webSource('views/DashboardView.vue')
    const groupedPanelCount = (dashboard.match(/<details class="dashboard-secondary-group/g) || []).length

    expect(groupedPanelCount).toBeGreaterThanOrEqual(3)
    expectBefore(dashboard, 'dashboard-live-summary-grid', '<details class="dashboard-secondary-group')
    expect(dashboard).toContain('dashboard-secondary-group-summary')
  })

  it('places Now Next Fix before standby and live dashboard detail', () => {
    const dashboard = webSource('views/DashboardView.vue')

    expectBefore(dashboard, 'mission-control-strip', 'dashboard-live-shell')
    expectBefore(dashboard, 'mission-control-strip', 'dashboard-live-summary-grid')
    expect(dashboard).toContain('Mission Control Now Next Fix')
    expect(dashboard).toContain('Open fix')
  })

  it('keeps second-screen overlay framed as a later local lane', () => {
    const dashboard = webSource('views/DashboardView.vue')

    expectBefore(dashboard, 'mission-control-overlay-note', 'dashboard-live-shell')
    expect(dashboard).toContain('Recommended later local lane')
    expect(dashboard).toContain('secondScreenOverlayRecommendation')
  })
})
