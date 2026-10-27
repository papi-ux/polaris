import { describe, expect, it, vi } from 'vitest'
import { drawTuningMarkers, latestTuningChange, readableTuningHistory, tuningChanges, tuningMarkers } from './live-tuning-timeline.js'

const t = (key, params) => (params ? `${key} ${JSON.stringify(params)}` : key)
// The served history as stream_stats writes it, for one live stream on the host's clock.
const history = (entries, extra = {}) => ({ version: 1, clock: 'host_steady_ms', available: true, reason: 'recorded',
  session_generation: 7, dropped: 0, truncated: false, host_elapsed_ms_at_snapshot: 60000, entries, ...extra })
let sequence = 0
const entry = (kind, cause, at, requested, extra = {}) => ({ sequence: ++sequence, kind, cause, host_elapsed_ms: at,
  requested_bitrate_kbps: requested, applied_proof: 'unknown', applied_bitrate_kbps: null, ...extra })
const applied = kbps => ({ applied_proof: 'encoder_confirmed', applied_bitrate_kbps: kbps })
const start = (at = 0, kbps = 20000) => entry('scope_started', 'initial_scope_snapshot', at, kbps, applied(kbps))
const fec = (kind, cause, at, transition) => entry(kind, cause, at, 20000, { fec_transition: { configured_percentage: 10,
  percentage_before: 10, percentage_after: 10, pending_percentage_before: null, pending_percentage_after: null, ...transition } })
// Sixty one-second sparkline points received by the browser, the newest carrying the history above.
const axis = Array.from({ length: 60 }, (_, i) => 1000 + i)

describe('Live Tuning change timeline', () => {
  it('stays quiet for a history that is absent, not live, not this stream’s, or not on the host clock', () => {
    const changed = [start(), entry('decision', 'network_feedback', 30000, 14000)]
    expect(tuningChanges(history(changed))).toHaveLength(1)
    for (const quiet of [undefined, null, history(changed, { available: false, reason: 'no_stream' }),
      history(changed, { reason: 'session_scope_unavailable' }), history(changed, { clock: 'browser_ms' }),
      history(changed, { version: 2 }), history(changed, { session_generation: 0 }),
      history(changed, { host_elapsed_ms_at_snapshot: undefined }), history(changed, { entries: undefined })]) {
      expect(readableTuningHistory(quiet)).toBe(false)
      expect(tuningChanges(quiet)).toEqual([])
      expect(latestTuningChange(tuningChanges(quiet), quiet, t)).toBe('')
    }
    // A live stream whose target never moved has no change to mark or tell.
    const steady = history([start(), entry('decision', 'network_feedback', 9000, 20000), entry('encoder_acknowledged', 'encoder_running_at_rate', 9500, 20000, applied(20000))])
    expect(tuningChanges(steady)).toEqual([])
    expect(latestTuningChange([], steady, t)).toBe('')
    expect(tuningMarkers([], steady, axis)).toEqual({ bitrate: [], loss: [] })
  })

  it('separates a requested rate from one the encoder confirmed, and only counts Live Tuning’s own decisions', () => {
    const changes = tuningChanges(history([
      start(),
      entry('decision', 'network_feedback', 10000, 14000),
      entry('encoder_acknowledged', 'encoder_running_at_rate', 10400, 14000, applied(14000)),
      // A Doctor write moves the target but is not Live Tuning's change; it still replaces the next one.
      entry('decision', 'stream_health', 20000, 16000, applied(14000)),
      entry('decision', 'doctor_action', 21000, 12000, applied(14000)),
      // A state-only decision at the same target is not a rate change.
      entry('decision', 'network_feedback', 30000, 12000, applied(12000)),
      entry('decision', 'network_feedback', 40000, 15000, applied(12000)),
    ]))
    expect(changes.map(({ kbps, direction, cause, outcome, restored }) => ({ kbps, direction, cause, outcome, restored }))).toEqual([
      { kbps: 14000, direction: 'down', cause: 'network_feedback', outcome: 'confirmed', restored: false },
      { kbps: 16000, direction: 'up', cause: 'stream_health', outcome: 'unconfirmed', restored: false },
      { kbps: 15000, direction: 'up', cause: 'network_feedback', outcome: 'pending', restored: false },
    ])
    expect(latestTuningChange(changes, history([]), t)).toBe(
      'mission_control.tuning_rate_up {"mbps":"15.0"} mission_control.tuning_cause_network_feedback · mission_control.tuning_pending · mission_control.age_ago {"age":"20 s"}')
  })

  it('does not take a change, or a baseline, from an entry whose target is unknown', () => {
    const changes = tuningChanges(history([start(), entry('decision', 'network_feedback', 5000, null),
      entry('decision', 'network_feedback', 6000, 12000), entry('decision', 'stream_health', 7000, 10000)]))
    expect(changes.map(change => change.kbps)).toEqual([10000])
  })

  it('says it returned to the starting rate only when the whole history shows that start', () => {
    const entries = [start(), entry('decision', 'network_feedback', 10000, 14000),
      entry('decision', 'network_feedback', 50000, 20000), entry('encoder_acknowledged', 'encoder_running_at_rate', 50300, 20000, applied(20000))]
    const whole = tuningChanges(history(entries))
    expect(whole.at(-1)).toMatchObject({ restored: true, outcome: 'confirmed', kbps: 20000 })
    expect(latestTuningChange(whole, history(entries), t)).toBe(
      'mission_control.tuning_rate_restored {"mbps":"20.0"} mission_control.tuning_cause_network_feedback · mission_control.tuning_confirmed · mission_control.age_ago {"age":"10 s"}')
    // With older entries dropped, the first one kept is not the stream's start.
    for (const cut of [{ dropped: 4, truncated: true }, { truncated: undefined }]) {
      const partial = tuningChanges(history(entries, cut))
      expect(partial.at(-1)).toMatchObject({ restored: false, direction: 'up', outcome: 'confirmed' })
    }
  })

  it('tells an FEC request apart from the parity the encoder confirmed', () => {
    const trial = fec('fec_target', 'parity_trial', 20000, { pending_percentage_after: 15 })
    const restore = fec('fec_target', 'restore_configured_parity', 40000, { percentage_before: 15, percentage_after: 10 })
    const pending = tuningChanges(history([start(), trial]))
    expect(pending).toEqual([expect.objectContaining({ track: 'fec', direction: 'up', percent: 15, outcome: 'pending', restored: false })])
    expect(latestTuningChange(pending, history([]), t)).toBe(
      'mission_control.tuning_fec_parity_trial {"percent":15} · mission_control.tuning_pending · mission_control.age_ago {"age":"40 s"}')
    expect(tuningChanges(history([start(), trial, fec('fec_published', 'in_place_encoder_confirmed', 20500, { percentage_after: 15 })]))[0].outcome).toBe('confirmed')
    for (const closing of ['fec_refused', 'fec_cancelled', 'fec_retired']) {
      expect(tuningChanges(history([start(), trial, fec(closing, 'encoder_boundary_or_feedback_unavailable', 21000, {})]))[0].outcome).toBe('unconfirmed')
    }
    const restored = tuningChanges(history([start(), trial, restore, fec('fec_restored', 'configured_parity_encoder_confirmed', 40400, {})]))
    expect(restored.at(-1)).toMatchObject({ direction: 'down', percent: 10, restored: true, outcome: 'confirmed' })
    expect(latestTuningChange(restored, history([]), t)).toBe(
      'mission_control.tuning_fec_restore_configured_parity {"percent":10} · mission_control.tuning_confirmed · mission_control.age_ago {"age":"20 s"}')
    // A request without its parity, or one that changes nothing, is not a change.
    expect(tuningChanges(history([start(), fec('fec_target', 'parity_trial', 20000, {})]))).toEqual([])
    expect(tuningChanges(history([start(), fec('fec_target', 'parity_trial', 20000, { pending_percentage_after: 10 })]))).toEqual([])
  })

  it('places each change its host-measured age before the newest point, inside the plotted window', () => {
    const recorded = history([start(),
      entry('decision', 'stream_health', 1000, 16000),
      fec('fec_target', 'parity_trial', 5000, { pending_percentage_after: 15 }),
      entry('decision', 'network_feedback', 45000, 14000)])
    const markers = tuningMarkers(tuningChanges(recorded), recorded, axis)
    // Snapshot 60 s, newest point 1059: 1 s is 59 s before it (the first point exactly), 5 s is 55 s
    // before and 45 s is 15 s before. Anything older falls outside the 60-point window.
    expect(markers.bitrate.map(marker => [marker.at, marker.direction])).toEqual([[1000, 'down'], [1044, 'down']])
    expect(markers.loss.map(marker => [marker.at, marker.direction])).toEqual([[1004, 'up']])
    const older = history(recorded.entries, { host_elapsed_ms_at_snapshot: 70000 })
    expect(tuningMarkers(tuningChanges(older), older, axis).bitrate.map(marker => marker.at)).toEqual([1034])
    // A change recorded after the snapshot cannot be placed, and two points are needed for an axis.
    const ahead = history([start(), entry('decision', 'network_feedback', 61000, 14000)])
    expect(tuningMarkers(tuningChanges(ahead), ahead, axis).bitrate).toEqual([])
    expect(tuningMarkers(tuningChanges(recorded), recorded, [1059])).toEqual({ bitrate: [], loss: [] })
    expect(tuningMarkers(tuningChanges(recorded), recorded, [])).toEqual({ bitrate: [], loss: [] })
  })

  it('marks only the current stream after a session reset restarts the host clock', () => {
    const before = history([start(), entry('decision', 'network_feedback', 30000, 14000)])
    expect(tuningMarkers(tuningChanges(before), before, axis).bitrate.map(marker => marker.at)).toEqual([1029])
    // The next session's history starts its own clock; nothing from the previous stream carries over.
    const after = history([start(0, 18000), entry('decision', 'stream_health', 2000, 12000)],
      { session_generation: 8, host_elapsed_ms_at_snapshot: 5000 })
    const changes = tuningChanges(after)
    expect(changes.map(change => change.kbps)).toEqual([12000])
    expect(tuningMarkers(changes, after, axis).bitrate.map(marker => marker.at)).toEqual([1056])
    expect(latestTuningChange(changes, after, t)).toContain('mission_control.tuning_rate_down {"mbps":"12.0"}')
  })

  it('draws a marker on the chart edge at its x-scale position, and skips any outside the plot', () => {
    const calls = []
    const ctx = new Proxy({}, { get: (_, name) => (name === 'fillStyle' ? undefined : (...args) => calls.push([name, ...args])), set: (_, name, value) => { calls.push(['set', name, value]); return true } })
    const u = { ctx, bbox: { left: 0, top: 0, width: 200, height: 64 }, valToPos: vi.fn(at => (at - 1000) * 2) }
    drawTuningMarkers(u, [{ at: 1044, direction: 'down' }, { at: 1004, direction: 'up' }, { at: 1200, direction: 'up' }], '#c8d6e5', 2)
    expect(u.valToPos).toHaveBeenCalledWith(1044, 'x', true)
    expect(calls).toContainEqual(['set', 'fillStyle', '#c8d6e5'])
    // Down points from the top edge; up points from the bottom edge; 1200 is off the plot.
    expect(calls).toContainEqual(['moveTo', 81, 0])
    expect(calls).toContainEqual(['lineTo', 88, expect.closeTo(11.2, 6)])
    expect(calls).toContainEqual(['moveTo', 1, 64])
    expect(calls).toContainEqual(['lineTo', 8, expect.closeTo(52.8, 6)])
    expect(calls.filter(([name]) => name === 'fill')).toHaveLength(2)
    calls.length = 0
    drawTuningMarkers(u, [], '#c8d6e5')
    drawTuningMarkers(u, [{ at: 1044, direction: 'down' }], '')
    expect(calls).toEqual([])
  })
})
