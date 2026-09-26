import { describe, expect, it } from 'vitest'
import { spacesDiagnostics } from './spaces-diagnostics.js'
import { translate } from './components/spaces-test-i18n.js'
import { validSetup } from './spaces-setup.js'
import { validSnapshot } from './spaces-access.js'

const setupFixture = (enabled = true) => ({
  version: 2, distribution: 'ubuntu', immutable_host: false, service_uid: 1000,
  host_prerequisites_ready: enabled, configured: enabled, available: enabled,
  checks: [
    ...['docker', 'docker_access', 'identity', 'input', 'gpu', 'security'].map(id => ({
      id, title: id, detail: `Reported ${id}`, action: '', state: enabled ? 'ready' : 'required',
    })),
    { id: 'runtime', title: 'Gaming runtime', detail: 'Verified runtime image', action: '', state: 'ready',
      runtime: { status: 'ready', code: 'runtime_ready', id: 'steam-default', variant: 'default', nvidia_driver: '' } },
    { id: 'spaces', title: 'Spaces configuration', detail: 'Reported setup state', action: '', state: enabled ? 'ready' : 'not_configured' },
  ],
})
const stateFixture = () => ({ enabled: true, available: true, changing: false, failed: false, profiles: [], activity: [] })
const report = (setup, snapshot, extra = {}) => spacesDiagnostics({ setup, snapshot, ...extra }, translate)
const rows = result => result.groups.flatMap(group => group.rows)
const mismatched = (id = 'space-1') => ({
  id, name: `Space ${id}`, clients: [], runtime_mismatch: true, runtime_driver: '580.1', host_driver: '610.2',
  runtime_move: { available: false, code: 'runtime_not_published', reason: 'driver_mismatch' },
})

describe('Spaces Doctor evidence', () => {
  it('uses valid setup and activity contracts and reports checks without claiming a streaming test', () => {
    expect(validSetup(setupFixture())).toBe(true)
    expect(validSnapshot(stateFixture())).toBe(true)
    const result = report(setupFixture(), stateFixture())
    expect(result.status).toBe('pass')
    expect(result.summary).toBe('Checks passed')
    expect(result.groups.map(group => group.id)).toEqual(['host', 'runtime', 'activity'])
    expect(rows(result).find(row => row.id === 'activity').detail).toContain('not a video or input test')
  })
  it('disabled Spaces is informational even when its optional host requirements are absent', () => {
    const result = report(setupFixture(false), { ...stateFixture(), enabled: false, available: false })
    expect(result.status).toBe('info')
    expect(result.summary).toBe('Not enabled')
    expect(rows(result).some(row => ['fail', 'warning'].includes(row.status))).toBe(false)
  })
  it('invalid or failed setup reads cannot reuse healthy evidence', () => {
    const bad = setupFixture(); bad.checks.shift()
    for (const result of [report(bad, stateFixture()), report(setupFixture(), stateFixture(), { setupError: 'offline' })]) {
      expect(result.status).toBe('warning')
      expect(result.summary).toBe('Could not verify')
      expect(result.groups[0].rows.map(row => row.id)).toEqual(['setup-unavailable'])
    }
  })
  it('a host administration failure overrides ready setup checks', () => {
    const result = report(setupFixture(), { ...stateFixture(), failed: true })
    expect(result.status).toBe('fail')
    expect(result.summary).toBe('Needs attention')
  })
  it('a configuration change is still in progress, not accepted as ready', () => {
    const result = report(setupFixture(), { ...stateFixture(), changing: true })
    expect(result.status).toBe('info')
    expect(result.summary).toBe('Updating')
  })
  it('runtime driver mismatches name the Space and both reported versions', () => {
    const state = { ...stateFixture(), profiles: [mismatched()] }
    expect(validSnapshot(state)).toBe(true)
    const result = report(setupFixture(), state)
    expect(result.status).toBe('warning')
    expect(rows(result).find(row => row.id === 'driver-space-1').detail).toContain('580.1; the host reports 610.2')
  })
  it('an explicitly unsupported launcher is a finding, not a healthy Space', () => {
    const result = report(setupFixture(), { ...stateFixture(), profiles: [{
      id: 'unsupported', name: 'Unsupported Space', clients: [], family: '',
      runtime_mismatch: false, runtime_driver: '', host_driver: '',
    }] })
    expect(result.status).toBe('warning')
    expect(rows(result).find(row => row.id === 'unsupported-launchers').detail).toContain('launchers: 1.')
  })
  it('archived Spaces do not produce runtime findings', () => {
    expect(report(setupFixture(), { ...stateFixture(), profiles: [{ ...mismatched(), archived: true }] }).status).toBe('pass')
  })
  it('missing runtime compatibility or activity is unknown rather than passed', () => {
    const withoutActivity = stateFixture(); delete withoutActivity.activity
    const oldRuntime = { ...stateFixture(), profiles: [{ id: 'old', name: 'Older Space', clients: [] }] }
    for (const state of [withoutActivity, oldRuntime]) {
      const result = report(setupFixture(), state)
      expect(result.status).toBe('info')
      expect(result.summary).toBe('Some checks unavailable')
    }
  })
  it('inconsistent snapshots ask for a recheck', () => {
    const result = report(setupFixture(), { ...stateFixture(), available: false })
    expect(rows(result).some(row => row.id === 'snapshot-changed')).toBe(true)
    expect(result.status).toBe('warning')
  })
  it('a failed runtime job offers no returned command or raw error as an action', () => {
    const state = { ...stateFixture(), runtime_move_job: {
      request_id: 'job-1', profile_id: 'space-1', runtime_id: 'steam-default', nvidia_driver: '',
      code: 'download_failed', message: 'private raw error', action: 'sudo some-returned-command', state: 'failed',
    } }
    expect(validSnapshot(state)).toBe(true)
    const result = report(setupFixture(), state)
    expect(result.status).toBe('warning')
    expect(JSON.stringify(result)).not.toContain('some-returned-command')
    expect(JSON.stringify(result)).not.toContain('private raw error')
    expect(rows(result).some(row => row.id === 'runtime-job-failed')).toBe(true)
  })
  it('a pending runtime move is updating rather than a completed check', () => {
    const state = { ...stateFixture(), runtime_move_job: {
      request_id: 'job-1', profile_id: 'space-1', runtime_id: 'steam-default', nvidia_driver: '',
      code: 'moving', message: '', action: '', state: 'moving',
    } }
    expect(report(setupFixture(), state)).toMatchObject({ status: 'info', summary: 'Updating' })
  })
  it('bounds a large mismatch list and reports the remaining count', () => {
    const result = report(setupFixture(), { ...stateFixture(), profiles: Array.from({ length: 55 }, (_, i) => mismatched(`space-${i}`)) })
    expect(rows(result).filter(row => row.id.startsWith('driver-'))).toHaveLength(50)
    expect(rows(result).find(row => row.id === 'more-mismatches').detail).toContain('mismatch: 5.')
  })
  it('activity counts are informational and do not expose paired client identities', () => {
    const result = report(setupFixture(), { ...stateFixture(),
      profiles: [{ id: 'space-1', name: 'Game Space', clients: [], runtime_mismatch: false, runtime_driver: '', host_driver: '' }],
      activity: [{ profile_id: 'space-1', client_id: 'private-client-id', state: 'running' }],
      capacity: { concurrent_active: 1, concurrent_limit: 2 },
    })
    expect(rows(result).find(row => row.id === 'activity')).toMatchObject({ status: 'info' })
    expect(rows(result).find(row => row.id === 'capacity').detail).toBe('1 of 2 concurrent Spaces in use.')
    expect(JSON.stringify(result)).not.toContain('private-client-id')
  })
})
