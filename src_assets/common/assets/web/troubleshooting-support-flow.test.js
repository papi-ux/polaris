import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'

function webSource(relativePath) {
  return readFileSync(join(process.cwd(), 'src_assets/common/assets/web', relativePath), 'utf8')
}

describe('Troubleshooting self-service support flow', () => {
  it('shows the plain Doctor diagnosis before advanced/raw evidence', () => {
    const source = webSource('views/TroubleshootingView.vue')

    const plainDiagnosis = source.indexOf('troubleshooting.doctor_plain_diagnosis')
    const advancedDetails = source.indexOf('troubleshooting.advanced_diagnostics')

    expect(plainDiagnosis).toBeGreaterThan(-1)
    expect(advancedDetails).toBeGreaterThan(-1)
    expect(plainDiagnosis).toBeLessThan(advancedDetails)
    expect(source).toContain('<details')
  })

  it('offers copy and download issue draft actions without public GitHub mutation', () => {
    const source = webSource('views/TroubleshootingView.vue')

    expect(source).toContain('@click="copyIssueDraft"')
    expect(source).toContain('@click="downloadIssueDraft"')
    expect(source).toContain('buildGithubIssueDraft')
    expect(source).not.toContain('api.github.com/repos')
    expect(source).not.toContain('github.com/papi-ux/polaris/issues/new')
  })

  it('keeps support bundle and issue draft copy explicitly redaction-aware', () => {
    const source = webSource('views/TroubleshootingView.vue')
    const locale = webSource('public/assets/locale/en.json')

    expect(source).toContain('troubleshooting.support_redaction_notice')
    expect(locale).toContain('No passwords, tokens, cookies, auth headers, or credentials are intentionally included')
  })

  it('places optional AI Doctor explanation behind privacy copy and no action execution', () => {
    const source = webSource('views/TroubleshootingView.vue')
    const locale = webSource('public/assets/locale/en.json')

    expect(source).toContain('data-ai-doctor-explanation')
    expect(source).toContain('@click="requestAiDoctorExplanation"')
    expect(source).toContain('AI_DOCTOR_EXPLANATION_CATEGORIES')
    expect(locale).toContain('Disabled unless you configure AI')
    expect(locale).toContain('Ollama or LM Studio')
    expect(locale).toContain('AI cannot execute recovery actions')
  })
})
