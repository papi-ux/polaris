import { readFileSync, readdirSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'
import { liveSignalCells } from './mission-control.js'

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
  it('leads live mode with the stage and its opt-in preview, then one readings strip, then the Doctor note', () => {
    const dashboard = webSource('views/DashboardView.vue')

    // Issue #408 deliberately reorders this (it was strip, Doctor, preview): the preview now
    // replaces the artwork inside the stage on request, the strip runs along the stage's lower
    // edge, and the Doctor note follows the stage.
    expectBefore(dashboard, '<MissionStage', 'data-start-preview')
    expectBefore(dashboard, 'data-start-preview', 'dashboard-preview-frame')
    expectBefore(dashboard, 'dashboard-preview-frame', '<SignalStrip')
    expectBefore(dashboard, '<SignalStrip', 'data-dashboard-doctor')

    // The strip says each stream number exactly once; path and runtime are on the route.
    const metrics = liveSignalCells({ stats: { streaming: true } }).map(cell => cell.attrs['data-live-summary-metric'])
    expect(metrics).toEqual(['Quality', 'Latency', 'FPS', 'Loss', 'Bitrate', 'Encode'])
    expect(webSource('components/SignalStrip.vue')).toContain('v-bind="cell.attrs"')
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

  it('executes Doctor v2 actions on the web origin and verifies reversible fixes', () => {
    const dashboard = webSource('views/DashboardView.vue')

    expect(dashboard).toContain("const payload = { ...(action.payload_preview || action.payload || {}) }")
    expect(dashboard).toContain("payload.action_id === 'lower_bitrate' || payload.action_id === 'restore_quality'")
    expect(dashboard).toContain('payload.request_id = randomId')
    expect(dashboard).toContain('postDoctorAction(payload)')
    expect(dashboard).toContain("fetch('./api/doctor/action'")
    expect(dashboard).toContain('verifyDoctorAction(result.verification)')
    expect(dashboard).toContain('undoDoctorAction')
    expect(dashboard).toContain('requestDoctorSafeAction')
    expect(dashboard).not.toContain("fetch('./polaris/v1/client-settings'")
  })
  it('leads idle mode with the status hero and keeps the play rail launchable', () => {
    const dashboard = webSource('views/DashboardView.vue')

    expectBefore(dashboard, 'data-dashboard-idle-hero', 'data-dashboard-play-rail')
    // The shelf sits right under the stage, ahead of the deck and the host vitals.
    expectBefore(dashboard, 'data-dashboard-play-rail', 'class="mc-deck"')
    expectBefore(dashboard, 'data-dashboard-play-rail', 'data-dashboard-idle-vitals')
    expect(dashboard).toContain('featureApp(app)')
    // Hidden Play takes no hits and shows for any touch input; only a press that began on it launches.
    const css = webSource('app.css')
    expect(css).toContain('pointer-events: var(--play-hit, none);')
    expect(css).toMatch(/@media \(hover: none\), \(any-pointer: coarse\) \{\s*\.mc-film \{\s*--play: 1;\s*--play-hit: auto;/)
    expect(dashboard).toContain('@pointerdown.capture="notePlayPress"')
    // Inside Nova's Host Console every page's guide control is hidden; contextual help stays.
    const pages = ['views', 'components', 'configs/tabs'].flatMap(dir => readdirSync(`src_assets/common/assets/web/${dir}`)
      .filter(name => name.endsWith('.vue')).map(name => `${dir}/${name}`))
    const guided = pages.filter(page => webSource(page).includes('data-tab-docs-link'))
    expect(guided).toEqual(expect.arrayContaining([
      'views/AppsView.vue', 'views/DashboardView.vue',
      ...['Advanced', 'Files', 'General', 'Inputs', 'Network'].map(name => `configs/tabs/${name}.vue`),
    ]))
    for (const page of guided) {
      const source = webSource(page)
      const guides = source.split('data-tab-docs-link').length - 1
      expect(source.match(/<[a-z]+ v-if="!inNova"[^>]*data-tab-docs-link/g)?.length, page).toBe(guides)
    }
    expect(dashboard).toContain('@click="playClicked(app, $event)"')
    expect(dashboard).toContain(':data-dashboard-feature-app="app.uuid"')
    expect(dashboard).toContain('launchRecentApp(app)')
    expect(dashboard).toContain("$t('dashboard.open_priority_fix')")
    expect(dashboard).toContain('<PosterImage')
    expect(dashboard).toContain(':name="app.name"')
    expect(webSource('components/PosterImage.vue')).toContain(':alt="`${name || \'Untitled app\'} artwork`"')
  })
})
