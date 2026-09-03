// Whether the optional AI explanation on Doctor & Support can actually run,
// from GET /api/ai/status (authoritative) with the saved config as fallback.

export const AI_READINESS_STATES = Object.freeze([
  'off',
  'subscription',
  'subscription_login_needed',
  'api_key',
  'api_key_missing',
  'local',
  'unverified',
])

const truthy = (value) => value === true || value === 'enabled' || value === 'true'

export function describeAiReadiness(status, config = {}) {
  const hasStatus = Boolean(status && typeof status === 'object' && !status._error)
  const enabled = hasStatus ? status.enabled === true : truthy(config.ai_enabled)
  const provider = String((hasStatus && status.provider) || config.ai_provider || '').trim()
  const model = String((hasStatus && status.model) || config.ai_model || '').trim()
  const baseUrl = String((hasStatus && status.base_url) || config.ai_base_url || '').trim()
  const cli = String((hasStatus && status.cli_binary) || 'codex').trim()
  const loginCommand = String((hasStatus && status.cli_login_command) || `${cli} login`).trim()
  const base = { provider, model, cli, loginCommand }

  if (!enabled) return { ...base, state: 'off', configured: false }
  if (!hasStatus) return { ...base, state: 'unverified', configured: false }

  const authMode = String(status.auth_mode || (status.use_subscription ? 'subscription' : '')).toLowerCase()
  if (authMode === 'subscription') {
    const ok = status.cli_authenticated === true
    return { ...base, state: ok ? 'subscription' : 'subscription_login_needed', configured: ok }
  }
  if (provider === 'local' || authMode === 'local' || authMode === 'none') {
    const ok = Boolean(baseUrl)
    return { ...base, state: ok ? 'local' : 'off', configured: ok }
  }
  const ok = status.has_api_key === true
  return { ...base, state: ok ? 'api_key' : 'api_key_missing', configured: ok }
}

/**
 * The sentence the panel shows above the button, keyed on the readiness state.
 */
export function aiReadinessCopy(readiness, t) {
  const params = { provider: readiness.provider, model: readiness.model, cli: readiness.cli, command: readiness.loginCommand }
  switch (readiness.state) {
    case 'subscription':
      return t('troubleshooting.ai_doctor_ready_subscription', params)
    case 'subscription_login_needed':
      return t('troubleshooting.ai_doctor_login_needed', params)
    case 'api_key':
    case 'local':
      return t('troubleshooting.ai_doctor_ready', params)
    case 'api_key_missing':
      return t('troubleshooting.ai_doctor_key_missing', params)
    case 'unverified':
      return t('troubleshooting.ai_doctor_unverified', params)
    default:
      return t('troubleshooting.ai_doctor_explanation_privacy')
  }
}
