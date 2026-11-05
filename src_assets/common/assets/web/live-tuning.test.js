import { describe, it, expect } from 'vitest'
import fixtures from '../../../../tests/fixtures/live-tuning-v1.json'
import { parseLiveTuning, createLiveTuningReducer, liveTuningLabel } from './live-tuning'

describe('canonical Live Tuning', () => {
  it.each(fixtures)('parses the shared $name fixture', row => {
    expect(parseLiveTuning(row.live_tuning)).toEqual(row.live_tuning)
    expect(liveTuningLabel(row.live_tuning)).not.toContain('Unknown')
  })
  it('keeps explanation readiness and stream health out of enablement', () => {
    const off = { ...fixtures[0].live_tuning, ai_enabled: true, health: { grade: 'degraded' } }
    expect(liveTuningLabel(off)).toBe('Live Tuning Off')
    expect(liveTuningLabel(fixtures[6].live_tuning)).toContain('On')
  })
  // The controller's frame_pacing_observed status arrives as reason frame_pacing under stable or
  // adjusting. It cuts nothing, so it must not read as plain "stable", nor as a bitrate change.
  it('names an observed frame-pacing variation instead of calling the stream stable', () => {
    const stable = { ...fixtures.find(row => row.live_tuning.state === 'stable').live_tuning, reason: 'frame_pacing' }
    expect(parseLiveTuning(stable)).toEqual(stable)
    expect(liveTuningLabel(stable)).toBe('On — frame timing variation observed')
    expect(liveTuningLabel({ ...stable, state: 'adjusting' })).toBe('On — adjusting; frame timing variation observed')
    expect(liveTuningLabel({ ...stable, state: 'applying' })).toBe('On — applying bitrate')
    expect(liveTuningLabel({ ...stable, reason: 'healthy' })).toBe('Live Tuning On — stable')
    expect(liveTuningLabel({ ...stable, enabled: false })).toBe('Live Tuning Off')
    for (const state of ['stable', 'adjusting']) expect(liveTuningLabel({ ...stable, state })).not.toMatch(/VRR|Hz|lower/i)
    // A state the v1 contract does not carry stays rejected rather than guessed at.
    expect(parseLiveTuning({ ...stable, state: 'frame_pacing_observed' })).toBeNull()
  })
  it('rejects invalid state and distinguishes connection loss from Off', () => {
    expect(parseLiveTuning({ ...fixtures[0].live_tuning, enabled: 'false' })).toBeNull()
    expect(liveTuningLabel(fixtures[0].live_tuning, false)).toBe('Live Tuning: Unknown')
  })
  it('rejects old observations, including an old host after restart', () => {
    const accept = createLiveTuningReducer()
    const row = fixtures[0].live_tuning
    expect(accept({ ...row, sequence: 4 })).not.toBeNull()
    expect(accept({ ...row, sequence: 3 })).toBeNull()
    expect(accept({ ...row, host_instance: 'restarted', sequence: 1 })).not.toBeNull()
    expect(accept({ ...row, sequence: 99 })).toBeNull()
  })
})
