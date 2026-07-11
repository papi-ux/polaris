import { buildStreamEvidence, sanitizeDiagnosticsValue } from './diagnostics-export.js'

export const AI_DOCTOR_EXPLANATION_SCHEMA = Object.freeze({
  type: 'object',
  additionalProperties: false,
  required: [
    'likely_cause',
    'evidence',
    'try_first',
    'advanced_detail',
    'confidence',
    'destructive_action_allowed',
  ],
  properties: {
    likely_cause: { type: 'string' },
    evidence: { type: 'array', items: { type: 'string' } },
    try_first: { type: 'array', items: { type: 'string' } },
    advanced_detail: { type: 'string' },
    confidence: { type: 'string', enum: ['low', 'medium', 'high', 'deterministic-fallback'] },
    destructive_action_allowed: { type: 'boolean', const: false },
  },
})

export const AI_DOCTOR_EXPLANATION_CATEGORIES = Object.freeze([
  'Doctor diagnosis and evidence',
  'Fix My Stream checklist',
  'Current stream/session metrics',
  'Recent redacted warning/error summaries',
])

function providerConfig(config = {}) {
  return {
    provider: String(config.ai_provider || '').trim(),
    model: String(config.ai_model || '').trim(),
    base_url: String(config.ai_base_url || '').trim(),
    auth_mode: String(config.ai_auth_mode || '').trim() || (config.ai_provider === 'local' ? 'none' : 'api_key'),
  }
}

function hasConfiguredProvider(config = {}) {
  const provider = String(config.ai_provider || '').trim()
  const model = String(config.ai_model || '').trim()
  const baseUrl = String(config.ai_base_url || '').trim()
  return Boolean(provider && model && (provider !== 'local' || baseUrl))
}

export function buildAiDoctorExplanationPayload({ config = {}, supportBundle = {}, deterministicSummary = null } = {}) {
  const rawEvidence = {
    ...buildStreamEvidence(supportBundle),
    session_snapshot: supportBundle.session_snapshot || supportBundle.stream_stats || {},
    doctor: supportBundle.session_snapshot?.doctor || supportBundle.stream_stats?.doctor || supportBundle.doctor || {},
    fix_my_stream_checklist: supportBundle.fix_my_stream_checklist || [],
    recent_issues: supportBundle.recent_issues || [],
  }
  const evidence = sanitizeDiagnosticsValue(JSON.parse(JSON.stringify(rawEvidence)))

  return {
    provider: providerConfig(config),
    categories: [...AI_DOCTOR_EXPLANATION_CATEGORIES],
    schema: AI_DOCTOR_EXPLANATION_SCHEMA,
    deterministic_source_of_truth: sanitizeDiagnosticsValue(deterministicSummary || supportBundle.deterministic_summary || null),
    evidence,
  }
}

function toStringArray(value) {
  if (Array.isArray(value)) return value.map((item) => String(item || '').trim()).filter(Boolean)
  if (value === null || value === undefined || value === '') return []
  return [String(value)]
}

function extractJsonObject(value) {
  if (!value) return null
  if (typeof value === 'object') return value
  const text = String(value)
  const start = text.indexOf('{')
  const end = text.lastIndexOf('}')
  if (start === -1 || end === -1 || end <= start) return null
  return JSON.parse(text.slice(start, end + 1))
}

export function normalizeAiDoctorExplanation(value, { deterministicSummary = {}, providerError = '' } = {}) {
  const parsed = extractJsonObject(value)
  if (!parsed) {
    const title = deterministicSummary?.title || 'Polaris deterministic Doctor result'
    const detail = deterministicSummary?.detail || 'AI explanation was unavailable; Polaris is showing the deterministic Doctor evidence instead.'
    const action = deterministicSummary?.action || 'Review the Fix My Stream checklist and support bundle evidence.'
    return {
      status: false,
      provider_error: providerError || 'AI explanation unavailable.',
      explanation: {
        likely_cause: title,
        evidence: [detail].filter(Boolean),
        try_first: [action].filter(Boolean),
        advanced_detail: providerError
          ? `AI provider unavailable (${providerError}); deterministic Doctor output remains the source of truth.`
          : 'Deterministic Doctor output remains the source of truth.',
        confidence: 'deterministic-fallback',
        destructive_action_allowed: false,
      },
    }
  }

  return {
    status: true,
    explanation: {
      likely_cause: String(parsed.likely_cause || deterministicSummary?.title || 'Polaris Doctor finding').trim(),
      evidence: toStringArray(parsed.evidence),
      try_first: toStringArray(parsed.try_first),
      advanced_detail: String(parsed.advanced_detail || '').trim(),
      confidence: ['low', 'medium', 'high'].includes(parsed.confidence) ? parsed.confidence : 'low',
      destructive_action_allowed: false,
    },
  }
}

export function parseAiDoctorExplanationResponse(response, options = {}) {
  if (!response?.status) {
    return normalizeAiDoctorExplanation(null, {
      ...options,
      providerError: response?.error || response?.provider_error || options.providerError,
    })
  }
  return normalizeAiDoctorExplanation(response.explanation || response.result || response, options)
}

export async function explainDoctorWithAi({ aiEnabled = false, config = {}, supportBundle = {}, deterministicSummary = {}, fetchImpl = fetch } = {}) {
  if (!aiEnabled) {
    return { status: false, disabled: true, error: 'AI explanations are disabled.' }
  }
  if (!hasConfiguredProvider(config)) {
    return { status: false, disabled: true, error: 'Choose an AI provider before asking for an explanation.' }
  }

  const payload = buildAiDoctorExplanationPayload({ config, supportBundle, deterministicSummary })
  const response = await fetchImpl('./api/ai/explain-doctor', {
    credentials: 'include',
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(payload),
  })

  if (!response?.ok) {
    return normalizeAiDoctorExplanation(null, {
      deterministicSummary,
      providerError: response?.status ? `HTTP ${response.status}` : 'Provider request failed',
    })
  }

  return parseAiDoctorExplanationResponse(await response.json(), { deterministicSummary })
}
