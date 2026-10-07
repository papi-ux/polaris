import { describe, expect, it } from 'vitest'
import { buildReadyCheckDisplay } from './dashboard-ready-checks.js'
import { captureStatusTone } from './capture-status-tone.js'
import { launchModeReadout } from './launch-mode-readout.js'
import { streamPacingReadouts } from './stream-pacing-readout.js'

describe('Polaris status evidence', () => {
  it('does not count unmeasured or inapplicable checks as failures or ready', () => {
    const result = buildReadyCheckDisplay([
      { key: 'clients', status: 'ready' }, { key: 'audio', status: 'not_checked' },
      { key: 'display', status: 'not_applicable' },
    ])
    expect(result).toMatchObject({ total: 2, passing: 1, attention: 0, unknown: 1, notApplicable: 1, allPassing: false, primaryIssue: null })
    expect(result.visibleChecks.map(row => row.key)).toEqual(['audio'])
  })
  it('selects an actual issue before an earlier unmeasured check', () => {
    const result = buildReadyCheckDisplay([{ key: 'audio', status: 'not_checked' }, { key: 'clients', status: 'attention' }])
    expect(result.primaryIssue.key).toBe('clients')
    expect(result.attention).toBe(1)
    expect(buildReadyCheckDisplay([{ status: 'ready' }, { status: 'not_applicable' }]).allPassing).toBe(true)
  })
  it('keeps expected CPU copies neutral while marking an explicit requested-path fallback', () => {
    const stream = { streaming: true, capture_gpu_native: false, capture_cpu_copy: true, capture_transport: 'shm' }
    expect(captureStatusTone({ ...stream, capture_path_reason: 'headless_shm_default' })).toBe('neutral')
    expect(captureStatusTone({ ...stream, capture_path_reason: 'gpu_native_requested_shm_fallback' })).toBe('warning')
    expect(captureStatusTone({ streaming: true })).toBe('neutral')
    expect(captureStatusTone({ ...stream, streaming: false, capture_path_reason: 'gpu_native_requested_shm_fallback' })).toBe('neutral')
  })
})

describe('per-client source pacing receipts', () => {
  const receipt = { schema: 1, stream_instance_id: 'stream-a', requested: 'true', effective: true,
    pacing_mode: 'source_time', fallback_reason: null, host_pacing: { state: 'source_time', fresh: true } }
  const client = { name: 'Pixel', stream_instance_id: 'stream-a', vrr: receipt }
  const read = (row) => streamPacingReadouts({ streaming: true, clients: [row] })[0]
  it('only reports current, bound source-time pacing', () => {
    expect(read(client)).toMatchObject({ requested: 'requested', effective: 'source_time', fallbackKnown: true })
    for (const vrr of [undefined, { ...receipt, schema: 2 }, { ...receipt, stream_instance_id: 'old-stream' },
      { ...receipt, host_pacing: { fresh: false, state: 'source_time' } }, { ...receipt, host_pacing: undefined }]) {
      expect(read({ ...client, vrr }).effective).toBe('unknown')
    }
    expect(streamPacingReadouts({ streaming: false, clients: [client], last_session: { vrr: receipt } })).toEqual([])
    expect(streamPacingReadouts({ streaming: true, clients: [client] }, false)[0].effective).toBe('unknown')
    expect(read({ ...client, vrr: { ...receipt, requested: 'false' } }).effective).toBe('unknown')
  })
  it('keeps independent requests and named fallback evidence separate', () => {
    const fixed = { ...client, name: 'Deck', stream_instance_id: 'stream-b', vrr: { ...receipt,
      stream_instance_id: 'stream-b', requested: 'false', effective: false, pacing_mode: 'fixed_grid', fallback_reason: 'not_requested' } }
    const rows = streamPacingReadouts({ streaming: true, clients: [client, fixed] })
    expect(rows[0]).toMatchObject({ requested: 'requested', effective: 'source_time' })
    expect(rows[1]).toMatchObject({ requested: 'not_requested', effective: 'fixed_grid', reasonLabel: 'reason_not_requested' })
    expect(read({ ...client, vrr: { ...receipt, requested: true } }).requested).toBe('unknown')
  })
  it('explains frozen host permission without guessing from current config or a different stream', () => {
    const off = { ...receipt, host_permission: false, effective: false, pacing_mode: 'fixed_grid', fallback_reason: 'route_unreported' }
    expect(read({ ...client, vrr: off })).toMatchObject({ effective: 'fixed_grid', reasonLabel: 'reason_host_permission_off' })
    expect(read({ ...client, vrr: { ...off, stream_instance_id: 'old' } }).reason).toBe('')
    expect(read({ ...client, vrr: { ...off, host_permission: undefined } }).reasonLabel).toBe('reason_route_unreported')
    expect(read({ ...client, vrr: { ...off, fallback_reason: 'watch_or_input_only' } }).reasonLabel).toBe('reason_watch_or_input_only')
    expect(read({ ...client, vrr: { ...receipt, host_permission: false } }).effective).toBe('unknown')
    for (const requested of ['constructor', '__proto__', 'toString']) expect(read({ ...client, vrr: { ...receipt, requested } }).requested).toBe('unknown')
    for (const reason of ['unsupported_codec', 'unsupported_route', 'unsupported_loop']) {
      expect(read({ ...client, vrr: { ...off, host_permission: true, fallback_reason: reason } }).reasonLabel).toBe(`reason_${reason}`)
    }
  })
  it('distinguishes pending evidence and preserves unfamiliar fallback codes without guessing', () => {
    expect(read({ ...client, vrr: { ...receipt, effective: false, pacing_mode: 'source_time_pending', fallback_reason: 'source_time_unproven' } })).toMatchObject({ effective: 'pending', reasonLabel: 'reason_source_time_unproven' })
    expect(read({ ...client, vrr: { ...receipt, effective: false, pacing_mode: 'unverified', fallback_reason: 'future_reason' } })).toMatchObject({ effective: 'unknown', reason: 'future_reason', reasonLabel: 'fallback_used' })
  })
})

describe('launch mode outcomes', () => {
  it('distinguishes a borrowed connector from a created virtual output', () => {
    expect(launchModeReadout('host_virtual_display', { vdisplayBackend: 'kscreen-doctor' })).toMatchObject({ borrowed: true, physical: 'borrowed' })
    expect(launchModeReadout('host_virtual_display', { vdisplayBackend: 'KWin virtual output' })).toMatchObject({ borrowed: false, physical: 'virtual' })
    expect(launchModeReadout('host_virtual_display', { vdisplayBackend: 'Future provider' }).physical).toBe('unknown')
  })
  it('only suggests alternatives confirmed available on this host', () => {
    const options = [{ id: 'headless_stream', available: false }, { id: 'desktop_display', available: true }, { id: 'desktop_takeover', available: false }]
    expect(launchModeReadout('host_virtual_display', {}, options)).toMatchObject({ alternative: 'desktop_display', monitorOffAlternative: false })
    expect(launchModeReadout('desktop_display', {}, options).alternative).toBe('')
    expect(launchModeReadout('host_virtual_display', {}, [...options, { id: 'desktop_takeover', available: true }]).monitorOffAlternative).toBe(true)
  })
})
