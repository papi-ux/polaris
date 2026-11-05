import { describe, expect, it } from 'vitest'
import { emptyEpicSetup, reduceEpicSetup, epicSetupPresentation } from './epic-setup-state'
import { epicSample, epicTitle } from './epic-setup-fixtures'

const present = facts => epicSetupPresentation(reduceEpicSetup(emptyEpicSetup(), { type: 'sample', facts: epicSample(facts) }))
describe('Epic setup facts and historical rows', () => {
  it('keeps three ordered stable identities and four textual states', () => {
    expect(epicSetupPresentation(emptyEpicSetup()).rows.map(r => r.id)).toEqual(['observed', 'receipt', 'closed'])
    expect(present().rows.map(r => r.status)).toEqual(['Complete', 'Complete', 'Needs action'])
    expect(present().rows[2].message).toBe('Epic is closed; choose a game to check its setup.')
  })
  it.each(['required', 'unknown'])('never infers a %s receipt from open/proof/provider/imported games', receipt => {
    const p = present({ desktop: 'open', state: 'needs_action', reason: 'epic_open_on_desktop', receipt, receipt_revision: '0' })
    expect(p.rows[0].status).toBe('Complete')
    expect(p.rows[1].status).not.toBe('Complete')
    expect(p.rows[2].message).toContain('Quit Epic')
  })
  it('reports open then closed as history and never as simultaneous current claims', () => {
    expect(present({ desktop: 'open' }).rows[0].message).toContain('Epic is open')
    expect(present().rows[0].message).toContain('historical')
    expect(present().rows[0].message).not.toContain('Epic is open')
  })
  it('accepts successful empty metadata only through its receipt, and not unavailable scans', () => {
    expect(present().rows[1].status).toBe('Complete')
    expect(present({ metadata_available: false, receipt: 'required' }).rows[1].status).toBe('Needs action')
  })
  it('marks failed rescans and previous receipts historical without blessing the failed transaction', () => {
    let s = reduceEpicSetup(emptyEpicSetup(), { type: 'sample', facts: epicSample() })
    s = reduceEpicSetup(s, { type: 'scan', scan: { scan: 'metadata_unavailable' } })
    s = reduceEpicSetup(s, { type: 'sample', facts: epicSample({ receipt: 'previous', last_scan: 'metadata_unavailable' }) })
    expect(epicSetupPresentation(s).rows[1].message).toContain('Previous successful scan (historical)')
    expect(epicSetupPresentation(s).scanMessage).toContain('No new setup receipt')
    expect(epicSetupPresentation(reduceEpicSetup(s, { type: 'sample', facts: epicSample() })).scanMessage).toBe('')
  })
  it.each(['unknown', 'unavailable'])('does not call a selected title ready with %s prerequisites', prerequisites => {
    const p = present({ selected: epicTitle, runtime: 'absent', prerequisites })
    expect(p.rows[2].status).toBe("Can't check")
    expect(p.summary).not.toContain('ready for Private Stream')
  })
  it('requires selected, positively known support for a sampled ready label', () => {
    const p = present({ selected: epicTitle, runtime: 'absent', prerequisites: 'available', state: 'ready', reason: 'ready' })
    expect(p.rows[2].status).toBe('Complete')
    expect(p.rows[2].message).toContain('at this sample')
  })
  it('reserves Windows first-play and update alternatives for the typed Windows cause', () => {
    const message = present({ reason: 'windows_setup_unknown', runtime: 'unknown', selected: epicTitle }).rows[2].message
    expect(message).toContain('Windows setup folder')
    expect(message).toContain('If this is your first play')
    expect(message).toContain("If you've played before, finish any update")
    expect(present({ reason: 'check_unresolved', desktop: 'unknown' }).summary).not.toContain('Windows')
  })
  it('keeps unresolved census generic without branding or terminating another app', () => {
    const p = present({ desktop: 'unknown', reason: 'check_unresolved' })
    expect(p.rows[2].message).toContain("couldn't complete the launcher check")
    expect(p.summary).not.toMatch(/AppImage|NamedBlocker|kill/)
  })
  it.each(['busy', 'unknown'])('suspends green facts during a %s active-session sample', state => {
    const p = present({ state, reason: 'session_busy' })
    expect(p.message).toContain('stream is active or changing')
    expect(p.rows.every(r => r.status !== 'Complete')).toBe(true)
  })
  it('distinguishes a coalesced status read from an active stream', () => {
    const p = present({ state: 'busy', reason: 'check_in_progress' })
    expect(p.message).toContain('Another setup check')
    expect(p.message).not.toContain('stream')
    expect(p.rows.every(r => r.status === 'Checking')).toBe(true)
  })
  it.each(['epic_installation_changed', 'epic_installation_unavailable', 'epic_game_already_running', 'launcher_changed'])('explains %s without readiness', reason => {
    expect(present({ reason }).rows[2].status).not.toBe('Complete')
  })
  it.each(['offline', 'timeout', 'stale', 'auth', 'auth_store', 'malformed', 'unsupported', 'superseded', 'failed'])('clears current facts for %s and distinguishes its explanation', phase => {
    const sampled = reduceEpicSetup(emptyEpicSetup(), { type: 'sample', facts: epicSample() })
    const failed = reduceEpicSetup(sampled, { type: 'error', phase })
    expect(failed.facts).toBeNull()
    expect(epicSetupPresentation(failed).message).not.toBe('')
    expect(epicSetupPresentation(failed).rows.every(r => r.status !== 'Complete')).toBe(true)
    if (phase === 'auth') expect(failed.history).toBeNull()
  })
  it('distinguishes a restart from a file update and clears receipt history on generation invalidation', () => {
    const old = reduceEpicSetup(emptyEpicSetup(), { type: 'sample', facts: epicSample() })
    const next = reduceEpicSetup(old, { type: 'sample', restarted: true, facts: epicSample({ epoch: 'b', receipt: 'required', receipt_revision: '0' }) })
    expect(next.history).toBeNull()
    expect(epicSetupPresentation(next).notice).toContain('restarted')
    expect(epicSetupPresentation(reduceEpicSetup(old, { type: 'sample', facts: epicSample({ proof: 'changed', receipt: 'required' }) })).notice).toContain('updated')
  })
})
