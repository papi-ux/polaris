import { readFileSync } from 'node:fs'
import { join } from 'node:path'
import { describe, expect, it } from 'vitest'
import { buildReadyCheckDisplay, isIdleVirtualDisplayCheck } from './dashboard-ready-checks.js'
import { captureStatusTone } from './capture-status-tone.js'
import { launchModeReadout } from './launch-mode-readout.js'
import { frameIntervalBasis, frameIntervalReadout, streamPacingReadouts } from './stream-pacing-readout.js'

describe('Polaris status evidence', () => {
  const idleDisplayCheck = {
    configured: true, source: 'ready', key: 'display', ok: false,
    mode: 'host_virtual_display', stats: { streaming: false }, connected: true, receivedAt: 1000,
  }

  it('does not count unmeasured or inapplicable checks as failures or ready', () => {
    const result = buildReadyCheckDisplay([
      { key: 'clients', status: 'ready' }, { key: 'audio', status: 'not_checked' },
      { key: 'display', status: 'not_applicable' },
    ])
    expect(result).toMatchObject({ total: 2, passing: 1, attention: 0, unknown: 1, notApplicable: 1, allPassing: false, primaryIssue: null })
    expect(result.visibleChecks.map(row => row.key)).toEqual(['audio'])
  })
  it('requires an observed, connected idle sample before making a missing virtual display expected', () => {
    expect(isIdleVirtualDisplayCheck(idleDisplayCheck)).toBe(true)
    expect(isIdleVirtualDisplayCheck({ ...idleDisplayCheck, stats: null })).toBe(false)
    expect(isIdleVirtualDisplayCheck({ ...idleDisplayCheck, stats: {} })).toBe(false)
    expect(isIdleVirtualDisplayCheck({ ...idleDisplayCheck, stats: { streaming: 'false' } })).toBe(false)
    expect(isIdleVirtualDisplayCheck({ ...idleDisplayCheck, connected: undefined })).toBe(false)
    expect(isIdleVirtualDisplayCheck({ ...idleDisplayCheck, receivedAt: undefined })).toBe(false)
    // The transport retains this false sample on disconnect, so the value alone proves nothing.
    expect(isIdleVirtualDisplayCheck({ ...idleDisplayCheck, connected: false })).toBe(false)
    expect(isIdleVirtualDisplayCheck({ ...idleDisplayCheck, receivedAt: 0 })).toBe(false)
    expect(isIdleVirtualDisplayCheck({ ...idleDisplayCheck, stats: { streaming: true } })).toBe(false)
  })
  it('keeps the idle exception limited to loaded config and ready, empty Host Virtual Display outputs', () => {
    expect(isIdleVirtualDisplayCheck({ ...idleDisplayCheck, configured: false })).toBe(false)
    expect(isIdleVirtualDisplayCheck({ ...idleDisplayCheck, source: 'pending' })).toBe(false)
    expect(isIdleVirtualDisplayCheck({ ...idleDisplayCheck, source: 'unavailable' })).toBe(false)
    expect(isIdleVirtualDisplayCheck({ ...idleDisplayCheck, ok: true })).toBe(false)
    expect(isIdleVirtualDisplayCheck({ ...idleDisplayCheck, mode: 'desktop_display' })).toBe(false)
    expect(isIdleVirtualDisplayCheck({ ...idleDisplayCheck, key: 'audio' })).toBe(false)
  })
  it('uses the observed-idle predicate for the dashboard exception and its existing explanation', () => {
    const dashboard = readFileSync(join(process.cwd(), 'src_assets/common/assets/web/views/DashboardView.vue'), 'utf8')
    expect(dashboard).toContain('const virtualDisplayIdle = isIdleVirtualDisplayCheck({')
    expect(dashboard).toContain('mode: configured ? resolveStreamDisplayMode(readinessConfig.value) : null')
    expect(dashboard).toContain('stats: stats.value, connected: streamStatsConnected?.value, receivedAt: streamStatsReceivedAt?.value')
    expect(dashboard).toContain('virtualDisplayIdle ||')
    expect(dashboard).toContain("virtualDisplayIdle ? t('dashboard.check_display_virtual_idle') : item.detail")
    const en = JSON.parse(readFileSync(join(process.cwd(), 'src_assets/common/assets/web/public/assets/locale/en.json'), 'utf8'))
    expect(en.dashboard.check_display_virtual_idle).toContain('Host Virtual Display creates its display when a stream starts')
    expect(en.dashboard.check_display_missing).toBe('No active display path is being reported yet.')
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

// Mission Control's frame-health pill once read "target interval error" for every stream. A source-paced
// stream's figure is encode handoff against the source's own intervals, so the pill and its tooltip name
// that basis with the stream pacing labels, and neither claims anything about the client display or VRR.
describe('frame interval error basis', () => {
  const en = JSON.parse(readFileSync(join(process.cwd(), 'src_assets/common/assets/web/public/assets/locale/en.json'), 'utf8'))
  const t = (key, params = {}) => {
    const message = key.split('.').reduce((node, part) => node?.[part], en)
    if (typeof message !== 'string') throw new Error(`missing ${key}`)
    return message.replace(/\{(\w+)\}/g, (whole, name) => (name in params ? String(params[name]) : whole))
  }
  const source = { streaming: true, frame_interval_error_ms: 1.234, frame_jitter_ms: 9,
    frame_pacing_measurement: { basis: 'source_relative_encode_handoff', available: true, receiver_presentation_verified: false } }
  const fixed = { streaming: true, frame_interval_error_ms: 5.06, frame_jitter_ms: 5.06 }

  it('reads the basis from the host measurement, never from a client request alone', () => {
    expect(frameIntervalBasis(fixed)).toBe('fixed_grid')
    expect(frameIntervalBasis(source)).toBe('source_time')
    expect(frameIntervalBasis({ ...source, frame_pacing_measurement: { ...source.frame_pacing_measurement, available: false } })).toBe('pending')
    expect(frameIntervalBasis({ ...source, frame_pacing_measurement: { basis: 'future_basis', available: true } })).toBe('unknown')
    expect(frameIntervalBasis({ ...fixed, clients: [{ stream_instance_id: 'a', vrr: { schema: 1, requested: 'true', effective: true } }] })).toBe('fixed_grid')
  })
  it('names the fixed-rate target interval in the pill and tooltip', () => {
    const pill = frameIntervalReadout(fixed, t)
    expect(pill).toMatchObject({ basis: 'fixed_grid', value: '5.1 ms', tone: 'text-warning', label: 'Fixed-rate · 5.1 ms target interval error' })
    expect(pill.title).toMatch(/^Fixed-rate: .*requested frame interval/)
    // An older payload with only the compatibility name still reads.
    expect(frameIntervalReadout({ streaming: true, frame_jitter_ms: 2 }, t).label).toBe('Fixed-rate · 2.0 ms target interval error')
  })
  it('names the source-relative encode handoff instead of a target interval when source-paced', () => {
    const pill = frameIntervalReadout(source, t)
    expect(pill).toMatchObject({ basis: 'source_time', value: '1.2 ms', tone: '', label: 'Source-paced · 1.2 ms handoff error' })
    expect(pill.title).toMatch(/^Source-paced: .*encoder handoff intervals.*not verified/)
    expect(`${pill.label} ${pill.title}`).not.toContain('target interval')
  })
  it('shows no figure while source timing evidence is not current, and never claims VRR is active', () => {
    const pending = frameIntervalReadout({ ...source, frame_pacing_measurement: { ...source.frame_pacing_measurement, available: false } }, t)
    expect(pending).toMatchObject({ basis: 'pending', value: '--', tone: '', label: 'Source-paced · Waiting for timing evidence' })
    expect(pending.title).toMatch(/no handoff error is shown/)
    const unknown = frameIntervalReadout({ ...source, frame_pacing_measurement: { basis: 'future_basis' } }, t)
    expect(unknown).toMatchObject({ basis: 'unknown', label: '1.2 ms interval error' })
    expect(unknown.title).toMatch(/^Not verified: /)
    for (const stats of [fixed, source, { ...source, frame_pacing_measurement: { available: false, basis: 'source_relative_encode_handoff' } }]) {
      const { label, title } = frameIntervalReadout(stats, t)
      expect(`${label} ${title}`).not.toMatch(/VRR|active|Hz/)
    }
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
