import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'

function webSource(relativePath) {
  return readFileSync(join(process.cwd(), 'src_assets/common/assets/web', relativePath), 'utf8')
}

describe('Dashboard Doctor layout', () => {
  it('separates a long diagnosis from confidence and optimizer metadata', () => {
    const dashboard = webSource('views/DashboardView.vue')
    const headline = dashboard.indexOf('{{ doctorHeadline }}')
    const metadata = dashboard.indexOf('data-dashboard-doctor-meta')
    const confidence = dashboard.indexOf('{{ doctorConfidenceLabel }}')
    const optimizer = dashboard.indexOf('{{ autoQuality.compactLabel }}')

    expect(headline).toBeGreaterThan(-1)
    expect(metadata).toBeGreaterThan(headline)
    expect(confidence).toBeGreaterThan(metadata)
    expect(optimizer).toBeGreaterThan(confidence)
    expect(dashboard).toContain('data-dashboard-doctor-meta')
    expect(dashboard).toContain('class="mt-3 flex flex-wrap items-center gap-2"')
  })
})
