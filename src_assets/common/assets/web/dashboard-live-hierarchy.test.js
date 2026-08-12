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

describe('DashboardView hierarchy', () => {
  it('starts live mode with one metrics strip, then the Doctor verdict, then the preview', () => {
    const dashboard = webSource('views/DashboardView.vue')

    expectBefore(dashboard, 'dashboard-live-summary-grid', 'data-dashboard-doctor')
    expectBefore(dashboard, 'data-dashboard-doctor', 'dashboard-preview-panel')

    // The strip says each stream number exactly once; path and runtime moved
    // to chips and the session context rail.
    for (const label of ['Quality', 'Latency', 'FPS', 'Loss', 'Bitrate']) {
      expect(dashboard).toContain(`data-live-summary-metric="${label}"`)
    }
    expect(dashboard).not.toContain('data-live-summary-metric="Capture path"')
    expect(dashboard).not.toContain('data-live-summary-metric="Runtime mode"')
  })

  it('renders the host Doctor verdict instead of client-side guidance panels', () => {
    const dashboard = webSource('views/DashboardView.vue')

    expect(dashboard).toContain('data-dashboard-doctor')
    expect(dashboard).toContain('doctorSafeAction')
    // The replaced trio must stay gone.
    expect(dashboard).not.toContain('Priority guidance')
    expect(dashboard).not.toContain('streamPathNotices')
    expect(dashboard).not.toContain('mission-control-strip')
  })

  it('keeps secondary live panels in collapsible groups below the primary summary', () => {
    const dashboard = webSource('views/DashboardView.vue')
    const groupedPanelCount = (dashboard.match(/<details class="dashboard-secondary-group/g) || []).length

    expect(groupedPanelCount).toBeGreaterThanOrEqual(2)
    expectBefore(dashboard, 'dashboard-live-summary-grid', '<details class="dashboard-secondary-group')
    expect(dashboard).toContain('dashboard-secondary-group-summary')
  })

  it('leads idle mode with the status hero and keeps the play rail launchable', () => {
    const dashboard = webSource('views/DashboardView.vue')

    expectBefore(dashboard, 'data-dashboard-idle-hero', 'data-dashboard-play-rail')
    expect(dashboard).toContain('launchRecentApp(app)')
    expect(dashboard).toContain("$t('dashboard.open_priority_fix')")
  })
})
