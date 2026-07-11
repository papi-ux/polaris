import { describe, expect, it, vi } from 'vitest'
import {
  AI_DOCTOR_EXPLANATION_SCHEMA,
  buildAiDoctorExplanationPayload,
  explainDoctorWithAi,
  normalizeAiDoctorExplanation,
  parseAiDoctorExplanationResponse,
} from './ai-doctor-explanation.js'

describe('AI Doctor explanation payload', () => {
  it('stays disabled unless AI is explicitly enabled and configured', async () => {
    const fetchImpl = vi.fn()

    const disabled = await explainDoctorWithAi({ aiEnabled: false, fetchImpl })
    const missingProvider = await explainDoctorWithAi({ aiEnabled: true, config: { ai_provider: '' }, fetchImpl })

    expect(disabled).toEqual({ status: false, disabled: true, error: 'AI explanations are disabled.' })
    expect(missingProvider).toEqual({ status: false, disabled: true, error: 'Choose an AI provider before asking for an explanation.' })
    expect(fetchImpl).not.toHaveBeenCalled()
  })

  it('redacts support evidence and advertises categories before sending', () => {
    const payload = buildAiDoctorExplanationPayload({
      config: {
        ai_provider: 'local',
        ai_model: 'llama3.1',
        ai_base_url: 'http://127.0.0.1:11434/v1',
        ai_auth_mode: 'none',
        ai_api_key: 'sk-live-secret',
      },
      supportBundle: {
        session_snapshot: {
          client_name: 'Nova Deck',
          client_ip: '10.0.0.8',
          auth_token: 'tok-secret',
          doctor: {
            primary_issue: 'packet_loss',
            simple_state: 'Network is the loudest signal.',
            safe_recovery_action: { id: 'lower_bitrate', destructive: false },
            evidence: [{ id: 'logs', detail: 'Authorization: Bearer abc123 packet_loss=3.4' }],
          },
        },
        fix_my_stream_checklist: [{ label: 'Packet loss', status: 'fail', detail: '3.4% loss', action: 'Lower bitrate' }],
        recent_issues: [{ level: 'Warning', message: 'password=hunter2 packet loss spike' }],
      },
    })

    expect(payload.provider).toEqual({ provider: 'local', model: 'llama3.1', base_url: 'http://127.0.0.1:11434/v1', auth_mode: 'none' })
    expect(payload.categories).toEqual([
      'Doctor diagnosis and evidence',
      'Fix My Stream checklist',
      'Current stream/session metrics',
      'Recent redacted warning/error summaries',
    ])
    expect(payload.schema).toEqual(AI_DOCTOR_EXPLANATION_SCHEMA)
    expect(payload.evidence.session_snapshot.auth_token).toBe('[redacted]')
    expect(payload.evidence.session_snapshot.doctor.evidence[0].detail).toContain('Bearer [redacted]')
    expect(JSON.stringify(payload)).not.toContain('tok-secret')
    expect(JSON.stringify(payload)).not.toContain('hunter2')
    expect(JSON.stringify(payload)).not.toContain('sk-live-secret')
  })
})

describe('AI Doctor explanation parsing', () => {
  it('normalizes structured provider output and never allows destructive actions', () => {
    const parsed = parseAiDoctorExplanationResponse({
      status: true,
      explanation: '{"likely_cause":"Packet loss","evidence":["3.4% loss"],"try_first":["Lower bitrate"],"advanced_detail":"Network evidence beats encoder guesses.","confidence":"high","destructive_action_allowed":true}',
    })

    expect(parsed.status).toBe(true)
    expect(parsed.explanation).toEqual({
      likely_cause: 'Packet loss',
      evidence: ['3.4% loss'],
      try_first: ['Lower bitrate'],
      advanced_detail: 'Network evidence beats encoder guesses.',
      confidence: 'high',
      destructive_action_allowed: false,
    })
  })

  it('returns deterministic fallback when the provider errors or returns invalid JSON', () => {
    const fallback = normalizeAiDoctorExplanation(null, {
      deterministicSummary: {
        title: 'Capture path: warning',
        detail: 'GPU-native capture fell back to system memory.',
        action: 'Export support bundle.',
      },
      providerError: 'HTTP 500',
    })

    expect(fallback.status).toBe(false)
    expect(fallback.provider_error).toBe('HTTP 500')
    expect(fallback.explanation.likely_cause).toBe('Capture path: warning')
    expect(fallback.explanation.evidence).toContain('GPU-native capture fell back to system memory.')
    expect(fallback.explanation.try_first).toContain('Export support bundle.')
    expect(fallback.explanation.confidence).toBe('deterministic-fallback')
    expect(fallback.explanation.destructive_action_allowed).toBe(false)
  })
})
