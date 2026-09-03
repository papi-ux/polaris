import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'

function webSource(relativePath) {
  return readFileSync(join(process.cwd(), 'src_assets/common/assets/web', relativePath), 'utf8')
}

describe('System page product hierarchy', () => {
  it('leads with one compact health strip before host operations', () => {
    const home = webSource('views/HomeView.vue')

    const statusStrip = home.indexOf('data-system-status-strip')
    const telemetry = home.indexOf('data-system-telemetry')
    const issues = home.indexOf('data-system-recent-issues')
    const updates = home.indexOf('data-system-update-summary')

    expect(statusStrip).toBeGreaterThan(-1)
    expect(telemetry).toBeGreaterThan(statusStrip)
    expect(issues).toBeGreaterThan(statusStrip)
    expect(updates).toBeGreaterThan(telemetry)
    expect(updates).toBeGreaterThan(issues)
    expect(home.match(/class="system-status-item"/g)).toHaveLength(3)
    expect(home).not.toContain('class="header-support-card"')
  })

  it('turns recent issue counts into actionable issue and empty states', () => {
    const home = webSource('views/HomeView.vue')

    expect(home).toContain('data-system-recent-issues')
    expect(home).toContain('v-if="recentIssues.length"')
    expect(home).toContain('v-for="issue in recentIssues"')
    expect(home).toContain('{{ issue.message }}')
    expect(home).toContain('{{ issue.timestamp }}')
    expect(home).toContain("$t('index.no_warnings_title')")
    expect(webSource('public/assets/locale/en.json')).toContain('"no_warnings_title": "No warnings or errors"')
    expect(home).toContain('/troubleshooting#logs')
  })

  it('keeps update capabilities behind a compact common-case summary', () => {
    const home = webSource('views/HomeView.vue')

    expect(home).toContain('data-system-update-summary')
    expect(home).toContain(':aria-expanded="String(showUpdateDetails)"')
    expect(home).toContain('v-show="showUpdateDetails"')
    expect(home).toContain('@click="refreshUpdateStatus"')
    expect(home).toContain('copyInstallCommand')
    expect(home).toContain('updateCenterState.installCommand')
  })

  it('refreshes page-level update and issue state from the header action', () => {
    const home = webSource('views/HomeView.vue')

    expect(home).toContain('@click="refreshSystemPage"')
    expect(home).toMatch(/async function refreshSystemPage\(\)[\s\S]*Promise\.all\(\[\s*refreshUpdateStatus\(\),\s*fetchLogs\(\),?\s*\]\)/)
  })

  it('uses real operational routes and demotes product links to one footer', () => {
    const home = webSource('views/HomeView.vue')
    const main = webSource('main.js')

    expect(home).toContain("{ to: '/apps'")
    expect(main).toContain("{ path: '/apps'")
    expect(home).toContain("{ to: '/troubleshooting#logs'")
    expect(home).toContain("{ to: '/config'")
    expect(home).not.toContain("{ to: '/',")
    expect(home).toContain('class="system-resource-footer"')
    expect(home).toContain('v-for="link in resources"')
    expect(home).toContain('v-for="doc in legalDocs"')
    expect(home).toContain(':href="sponsor.href"')
    expect(home).not.toContain('compatibilityClients')
  })

  it('keeps reviewer-identified states honest and accessible', () => {
    const home = webSource('views/HomeView.vue')
    const css = webSource('app.css')

    expect(home).toContain('const updateDetailsForced = computed')
    expect(home).toContain('v-if="!updateDetailsForced"')
    expect(home).toContain('v-show="showUpdateDetails"')
    expect(home).toContain('{{ groupedIssueLogs.length }}')
    expect(home).toContain(':datetime="issue.timestamp || undefined"')
    expect(home).toContain(':class="telemetryLiveClass"')
    expect(home).toContain('system-telemetry-state-muted')
    expect(home).toContain("$t('index.never_auto_installs')")
    expect(webSource('public/assets/locale/en.json')).toContain('Polaris never auto-installs updates from this page.')
    expect(css).toMatch(/\.system-status-value\s*\{[^}]*overflow-wrap:\s*anywhere/)
    expect(css).toMatch(/\.system-footer-link\s*\{[^}]*min-height:\s*1\.75rem/)
    expect(css).toMatch(/\.system-ops-grid\s*\{[^}]*align-items:\s*start/)
    expect(css).toMatch(/\.system-status-dot\s*\{[^}]*background:\s*currentColor/)
  })

  it('keeps telemetry scan-friendly on desktop and stacked on phones', () => {
    const home = webSource('views/HomeView.vue')
    const css = webSource('app.css')

    expect(home).toContain('class="system-telemetry-live')
    expect(home.match(/class="system-telemetry-item"/g)).toHaveLength(4)
    expect(css).toContain('.system-ops-grid')
    expect(css).toContain('.system-telemetry-grid')
    expect(css).toContain('.system-telemetry-live')
    expect(css).toContain('grid-template-columns: repeat(2, minmax(0, 1fr))')
    expect(css).toContain('grid-template-columns: minmax(0, 1fr)')
    expect(css).toContain('align-self: flex-start')
  })
})
