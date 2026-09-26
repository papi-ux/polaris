import { describe, expect, it } from 'vitest'
import { summarizeDashboardSpaces } from './dashboard-spaces.js'

const state = extra => ({ enabled: true, available: true, failed: false, changing: false,
  profiles: [{ id: 'one', name: 'Living room', family: 'steam', clients: ['c1'] }], activity: [], ...extra })
const model = (extra, options = {}) => summarizeDashboardSpaces(state(extra), { loaded: true, ...options })

describe('Spaces on Mission Control', () => {
  it('keeps disabled Spaces out of the normal dashboard and permits desktop preview', () => {
    expect(model({ enabled: false })).toMatchObject({ visible: false, previewBlocked: false, attention: false })
  })
  it('blocks preview until activity is known, including old hosts and failed refreshes', () => {
    expect(model({}, { loaded: false }).previewBlocked).toBe(true)
    expect(model({ activity: null }).previewBlocked).toBe(true)
    expect(model({}, { error: true })).toMatchObject({ stale: true, attention: true, previewBlocked: true })
  })
  it.each(['starting', 'running', 'stopping'])('blocks desktop images during %s Space activity', status => {
    expect(model({ activity: [{ profile_id: 'one', client_id: 'c1', state: status }] }))
      .toMatchObject({ hasActivity: true, previewBlocked: true, total: 1, sessions: [{ name: 'Living room', family: 'steam', state: status }] })
  })
  it.each([{ failed: true }, { changing: true }, { available: false }])('does not trust empty activity while the controller is unavailable: %j', extra => {
    expect(model(extra).previewBlocked).toBe(true)
  })
  it('joins device names only by exact identity and does not expose identifiers as names', () => {
    const result = model({ activity: [{ profile_id: 'one', client_id: 'secret-id', state: 'running' }] },
      { clients: [{ uuid: 'other-id', name: 'Different device' }] })
    expect(result.sessions[0].client).toBe('')
    expect(model({ activity: [{ profile_id: 'one', client_id: 'c1', state: 'running' }] },
      { clients: [{ uuid: 'c1', name: 'Handheld' }] }).sessions[0].client).toBe('Handheld')
  })
  it('does not confuse assignments or archived driver warnings with active sessions', () => {
    expect(model({ profiles: [{ id: 'one', clients: ['c1'], archived: true, runtime_mismatch: true }] }))
      .toMatchObject({ hasActivity: false, mismatch: 0, previewBlocked: false })
  })
  it.each([
    { failed: true }, { available: false }, { runtime_move_job: { state: 'failed' } },
    { profiles: [{ id: 'one', runtime_mismatch: true }] }, { profiles: [{ id: 'one', family: '' }] },
  ])('flags actionable reported state %j', extra => { expect(model(extra).attention).toBe(true) })
  it('bounds rendering without undercounting sessions and ignores duplicate observations', () => {
    const activity = Array.from({ length: 102 }, (_, i) => ({ profile_id: 'one', client_id: `c${i}`, state: 'running' }))
    const result = model({ activity: [...activity, activity[0]] })
    expect(result).toMatchObject({ total: 102, remaining: 2 })
    expect(result.sessions).toHaveLength(100)
  })
})
