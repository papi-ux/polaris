import { describe, expect, it } from 'vitest'
import {
  isRollbackUnconfirmedResponse,
  isSuccessfulDoctorActionResponse,
  resolveDoctorActionHttpResponse,
} from './doctor-action-http.js'

function rollback(overrides = {}) {
  return {
    status: false,
    changed: true,
    state: 'rollback_unconfirmed',
    run_id: 'doctor-run-7',
    error: 'The encoder did not confirm restoration.',
    undo: { available: false },
    ...overrides,
  }
}

describe('Doctor action HTTP contract', () => {
  it('returns successful typed results', () => {
    const result = { status: true, changed: true, state: 'watching' }
    expect(isSuccessfulDoctorActionResponse(200, result)).toBe(true)
    expect(resolveDoctorActionHttpResponse({ ok: true, status: 200 }, result)).toBe(result)

    for (const malformed of [
      { status: true, state: 'watching' },
      { status: true, changed: 'true', state: 'watching' },
      { status: true, changed: true, state: '' },
    ]) {
      expect(isSuccessfulDoctorActionResponse(200, malformed)).toBe(false)
      expect(() => resolveDoctorActionHttpResponse({ ok: true, status: 200 }, malformed)).toThrow()
    }
  })

  it('accepts only exact 409 rollback-unconfirmed terminal results', () => {
    const result = rollback()
    expect(isRollbackUnconfirmedResponse(409, result)).toBe(true)
    expect(resolveDoctorActionHttpResponse({ ok: false, status: 409 }, result)).toBe(result)

    for (const status of [200, 400, 403, 500]) {
      expect(isRollbackUnconfirmedResponse(status, result)).toBe(false)
      expect(() => resolveDoctorActionHttpResponse({ ok: status === 200, status }, result)).toThrow()
    }
  })

  it('rejects malformed rollback and never turns an actionable Undo into unavailable', () => {
    for (const result of [
      rollback({ status: 'false' }),
      rollback({ changed: false }),
      rollback({ run_id: '' }),
      rollback({ undo: { available: true } }),
      rollback({ undo: { available: false, action_id: 'undo' } }),
      rollback({ undo: null }),
    ]) {
      expect(isRollbackUnconfirmedResponse(409, result)).toBe(false)
      expect(() => resolveDoctorActionHttpResponse({ ok: false, status: 409 }, result)).toThrow()
    }
  })
})
