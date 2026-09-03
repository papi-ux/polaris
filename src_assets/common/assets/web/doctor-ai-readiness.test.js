import { describe, expect, it } from 'vitest'
import { AI_READINESS_STATES, aiReadinessCopy, describeAiReadiness } from './doctor-ai-readiness.js'

const t = (key, params = {}) => {
  const base = key.replace(/^troubleshooting\./, '')
  const values = Object.entries(params).map(([k, v]) => `${k}=${v}`).join(',')
  return values ? `${base}(${values})` : base
}

const hostStatus = {
  enabled: true,
  auth_mode: 'subscription',
  provider: 'openai',
  model: 'gpt-5.6-luna',
  base_url: 'https://api.openai.com/v1',
  cli_binary: 'codex',
  cli_login_command: 'codex login',
  cli_authenticated: true,
  has_api_key: false,
}

describe('doctor AI readiness', () => {
  it('is off when the host says AI is disabled, whatever the config claims', () => {
    expect(describeAiReadiness({ ...hostStatus, enabled: false }, { ai_enabled: 'enabled' })).toMatchObject({ state: 'off', configured: false })
    expect(describeAiReadiness(null, { ai_enabled: 'disabled' })).toMatchObject({ state: 'off', configured: false })
  })

  it('trusts a signed-in subscription and names the CLI', () => {
    const readiness = describeAiReadiness(hostStatus, {})
    expect(readiness).toMatchObject({ state: 'subscription', configured: true, provider: 'openai', model: 'gpt-5.6-luna', cli: 'codex' })
    expect(aiReadinessCopy(readiness, t)).toBe('ai_doctor_ready_subscription(provider=openai,model=gpt-5.6-luna,cli=codex,command=codex login)')
  })

  it('asks for the CLI login when the subscription is enabled but not signed in', () => {
    const readiness = describeAiReadiness({ ...hostStatus, cli_authenticated: false }, {})
    expect(readiness).toMatchObject({ state: 'subscription_login_needed', configured: false })
    expect(aiReadinessCopy(readiness, t)).toContain('command=codex login')
  })

  it('requires a saved key for a hosted API provider', () => {
    expect(describeAiReadiness({ ...hostStatus, auth_mode: 'api_key', has_api_key: true }, {})).toMatchObject({ state: 'api_key', configured: true })
    expect(describeAiReadiness({ ...hostStatus, auth_mode: 'api_key', has_api_key: false }, {})).toMatchObject({ state: 'api_key_missing', configured: false })
  })

  it('treats a local provider with a base URL as configured without a key', () => {
    expect(describeAiReadiness({ ...hostStatus, auth_mode: 'api_key', provider: 'local', base_url: 'http://127.0.0.1:11434/v1' }, {})).toMatchObject({ state: 'local', configured: true })
    expect(describeAiReadiness({ ...hostStatus, auth_mode: 'api_key', provider: 'local', base_url: '' }, {})).toMatchObject({ state: 'off', configured: false })
  })

  it('reports unverified instead of guessing when the status call failed but config says enabled', () => {
    const readiness = describeAiReadiness({ _error: 'HTTP 500' }, { ai_enabled: 'enabled', ai_provider: 'openai', ai_model: 'gpt-5.6-luna' })
    expect(readiness).toMatchObject({ state: 'unverified', configured: false, provider: 'openai' })
    expect(aiReadinessCopy(readiness, t)).toMatch(/^ai_doctor_unverified/)
  })

  it('falls back to the privacy copy for the off state', () => {
    expect(aiReadinessCopy({ state: 'off' }, t)).toBe('ai_doctor_explanation_privacy')
    expect(AI_READINESS_STATES).toContain('unverified')
  })
})
