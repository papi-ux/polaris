import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'

function webSource(relativePath) {
  return readFileSync(join(process.cwd(), 'src_assets/common/assets/web', relativePath), 'utf8')
}

describe('Dashboard Doctor layout', () => {
  it('says what is happening, what to try and one action before anything deeper', () => {
    const dashboard = webSource('views/DashboardView.vue')
    const headline = dashboard.indexOf('{{ doctorHeadline }}')
    const step = dashboard.indexOf('data-doctor-try')
    const action = dashboard.indexOf('data-doctor-action')
    const deeper = dashboard.indexOf('data-doctor-more')
    const details = dashboard.indexOf('<DoctorDetails')

    expect(headline).toBeGreaterThan(-1)
    expect(step).toBeGreaterThan(headline)
    expect(action).toBeGreaterThan(step)
    expect(deeper).toBeGreaterThan(action)
    expect(details).toBeGreaterThan(deeper)
  })

  it('keeps confidence, evidence and Explain one level down, in that order', () => {
    const details = webSource('components/DoctorDetails.vue')
    const template = details.slice(details.indexOf('<template>'))
    const metadata = template.indexOf('data-dashboard-doctor-meta')
    const confidence = template.indexOf('dashboard.doctor_confidence')
    const optimizer = template.indexOf('autoQuality.compactLabel')
    const evidence = template.indexOf('data-doctor-evidence')
    const explain = template.indexOf('data-doctor-explain')

    expect(metadata).toBeGreaterThan(-1)
    expect(confidence).toBeGreaterThan(metadata)
    expect(optimizer).toBeGreaterThan(confidence)
    expect(evidence).toBeGreaterThan(optimizer)
    expect(explain).toBeGreaterThan(evidence)
    expect(template).toContain('role="list" aria-label="Doctor confidence and Live Tuning status"')
    expect(template.match(/role="listitem"/g)).toHaveLength(2)
    expect(webSource('views/DashboardView.vue')).not.toContain('doctor_confidence')
  })
})
